// discovery.ino -- a "listen first" logger firmware.
//
// What it does: runs a Wi-Fi access point for the weather console and logs EVERYTHING the console
// tries to do, while giving the minimal answers that keep it talking:
//   * DHCP: the ESP hands itself out as the DNS server (ESP-IDF dhcps default).
//   * DNS  (UDP/53):  logs every queried name, answers every A query with the AP address.
//   * NTP  (UDP/123): logs every request; answers (SNTP, stratum 2) only when the ESP itself has real
//                     time, which it gets from the optional STA uplink (see secrets.h).
//   * HTTP (TCP/80):  raw server -- logs the exact request line + headers + decoded query args, then
//                     replies 200 "success" on the Weather Underground path, 200 "200" when the Host
//                     header mentions weathercloud, 200 "success" otherwise.
//   * TLS  (TCP/443): accepts and drops connections, logging each attempt (detects TLS-only firmware).
//   * Wi-Fi events:   logs the console's MAC when it joins and the IP it was given.
//
// Nothing is published anywhere. Serial 115200. Everything in this file is English.
//
// Optional STA uplink (only so the NTP responder has a real clock): create firmware/discovery/secrets.h
// (gitignored) with:
//   #define STA_SSID "your-home-wifi"
//   #define STA_PASS "your-password"
// Without it the sketch is AP-only and logs NTP requests without answering them.

#include <WiFi.h>
#include <WiFiUdp.h>
#include <time.h>
#include <sys/time.h>

#if __has_include("secrets.h")
#include "secrets.h"
#define HAVE_STA 1
#else
#define HAVE_STA 0
#endif

// ---------------------------------------------------------------- config
static const char* AP_SSID = "fauxcloud-setup";
static const char* AP_PASS = "weather123";  // >= 8 chars; the console's setup page asks for it
static const IPAddress AP_IP(192, 168, 4, 1);  // NOT 192.168.1.x -- the console's own AP lives there
static const IPAddress AP_MASK(255, 255, 255, 0);
static const char* WU_PATH = "/weatherstation/updateweatherstation.php";
static const uint32_t NTP_UNIX_OFFSET = 2208988800UL;  // 1900 -> 1970

WiFiUDP dnsUdp, ntpUdp;
WiFiServer http80(80), tls443(443);

// ---------------------------------------------------------------- logging
static void logf(const char* tag, const char* fmt, ...) {
  char msg[512];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(msg, sizeof msg, fmt, ap);
  va_end(ap);
  Serial.printf("%10lu [%s] %s\n", (unsigned long)millis(), tag, msg);
}

// Test build: -DDISCOVERY_FAKE_NTP=<unix> answers every NTP request with <unix> + uptime, the same
// packet as a real answer; the STA uplink does not set the clock. Answers whether a console uploads on a
// time that is not the real one.
#ifdef DISCOVERY_FAKE_NTP
static bool haveTime() { return true; }
#else
static bool haveTime() { return time(nullptr) > 1700000000; }  // "after 2023" == SNTP synced
#endif

// ---------------------------------------------------------------- Wi-Fi events
static void onWiFiEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
  switch (event) {
    case ARDUINO_EVENT_WIFI_AP_STACONNECTED: {
      const uint8_t* m = info.wifi_ap_staconnected.mac;
      logf("AP", "client joined MAC %02X:%02X:%02X:%02X:%02X:%02X (OUI identifies the Wi-Fi module vendor)",
           m[0], m[1], m[2], m[3], m[4], m[5]);
      break;
    }
    case ARDUINO_EVENT_WIFI_AP_STADISCONNECTED: {
      const uint8_t* m = info.wifi_ap_stadisconnected.mac;
      logf("AP", "client left MAC %02X:%02X:%02X:%02X:%02X:%02X", m[0], m[1], m[2], m[3], m[4], m[5]);
      break;
    }
    case ARDUINO_EVENT_WIFI_AP_STAIPASSIGNED:
      logf("AP", "DHCP lease given: %s", IPAddress(info.wifi_ap_staipassigned.ip.addr).toString().c_str());
      break;
    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
#ifdef DISCOVERY_FAKE_NTP
      logf("STA", "uplink connected, IP %s -- SNTP not started (fake NTP build)", WiFi.localIP().toString().c_str());
#else
      logf("STA", "uplink connected, IP %s -- starting SNTP", WiFi.localIP().toString().c_str());
      configTime(0, 0, "pool.ntp.org", "time.google.com");
#endif
      break;
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
      logf("STA", "uplink lost");
      break;
    default:
      break;
  }
}

// ---------------------------------------------------------------- DNS (wildcard)
// Minimal: parses the first question, logs it, answers A with the AP address, anything else with an
// empty NOERROR answer. Good enough for a console that only needs "some IPv4 for that name".
static void dnsLoop() {
  int len = dnsUdp.parsePacket();
  if (len <= 0) return;
  uint8_t buf[512];
  if (len > (int)sizeof buf) { dnsUdp.clear(); return; }
  dnsUdp.read(buf, len);
  if (len < 12) return;

  char name[160];
  int p = 12, n = 0;
  while (p < len && buf[p] != 0 && n < (int)sizeof name - 2) {
    int l = buf[p++];
    if (l & 0xC0) break;  // compression pointer in a question -- not expected
    if (n) name[n++] = '.';
    for (int i = 0; i < l && p < len && n < (int)sizeof name - 2; i++) name[n++] = (char)buf[p++];
  }
  name[n] = 0;
  p++;  // the terminating zero label
  if (p + 4 > len) return;
  uint16_t qtype = (buf[p] << 8) | buf[p + 1];
  int qend = p + 4;
  logf("DNS", "%s type=%u from %s", name, qtype, dnsUdp.remoteIP().toString().c_str());

  uint8_t out[600];
  memcpy(out, buf, qend);
  out[2] = 0x81; out[3] = 0x80;         // response, recursion available, NOERROR
  out[4] = 0; out[5] = 1;               // 1 question
  out[6] = 0; out[7] = (qtype == 1) ? 1 : 0;  // 1 answer for A, none otherwise
  out[8] = out[9] = out[10] = out[11] = 0;
  int o = qend;
  if (qtype == 1) {
    const uint8_t rr[] = {0xC0, 0x0C, 0, 1, 0, 1, 0, 0, 0, 60, 0, 4};  // ptr to qname, A, IN, TTL 60, len 4
    memcpy(out + o, rr, sizeof rr); o += sizeof rr;
    for (int i = 0; i < 4; i++) out[o++] = AP_IP[i];
  }
  dnsUdp.beginPacket(dnsUdp.remoteIP(), dnsUdp.remotePort());
  dnsUdp.write(out, o);
  dnsUdp.endPacket();
}

// ---------------------------------------------------------------- NTP (SNTP responder)
static void ntpLoop() {
  int len = ntpUdp.parsePacket();
  if (len <= 0) return;
  uint8_t req[48];
  int got = ntpUdp.read(req, sizeof req);
  if (len > 48) ntpUdp.clear();
  int vn = (got >= 1) ? (req[0] >> 3) & 7 : 0;
  int mode = (got >= 1) ? req[0] & 7 : 0;
  if (!haveTime()) {
    logf("NTP", "request from %s (%d B, VN=%d mode=%d) -- NOT answered, no time source",
         ntpUdp.remoteIP().toString().c_str(), len, vn, mode);
    return;
  }
  struct timeval tv;
#ifdef DISCOVERY_FAKE_NTP
  uint32_t up = millis();
  tv.tv_sec = (time_t)((uint32_t)DISCOVERY_FAKE_NTP + up / 1000);
  tv.tv_usec = (suseconds_t)((up % 1000) * 1000);
#else
  gettimeofday(&tv, nullptr);
#endif
  uint32_t secs = (uint32_t)tv.tv_sec + NTP_UNIX_OFFSET;
  uint32_t frac = (uint32_t)((uint64_t)tv.tv_usec * 4294967296ULL / 1000000ULL);

  uint8_t rsp[48] = {0};
  rsp[0] = (uint8_t)(0 << 6 | (vn ? vn : 4) << 3 | 4);  // LI=0, same version, mode 4 (server)
  rsp[1] = 2;                                            // stratum 2
  rsp[2] = got >= 3 ? req[2] : 6;                        // poll
  rsp[3] = (uint8_t)-20;                                 // precision ~1 us
  memcpy(rsp + 12, "LOCL", 4);                           // reference id
  auto put = [&](int off, uint32_t s, uint32_t f) {
    rsp[off] = s >> 24; rsp[off + 1] = s >> 16; rsp[off + 2] = s >> 8; rsp[off + 3] = s;
    rsp[off + 4] = f >> 24; rsp[off + 5] = f >> 16; rsp[off + 6] = f >> 8; rsp[off + 7] = f;
  };
  put(16, secs, frac);                                   // reference timestamp
  if (got >= 48) memcpy(rsp + 24, req + 40, 8);          // originate = client's transmit
  put(32, secs, frac);                                   // receive
  put(40, secs, frac);                                   // transmit
  ntpUdp.beginPacket(ntpUdp.remoteIP(), ntpUdp.remotePort());
  ntpUdp.write(rsp, sizeof rsp);
  ntpUdp.endPacket();
#ifdef DISCOVERY_FAKE_NTP
  logf("NTP", "request from %s (VN=%d mode=%d) -- answered FAKE unix=%lu", ntpUdp.remoteIP().toString().c_str(),
       vn, mode, (unsigned long)tv.tv_sec);
#else
  logf("NTP", "request from %s (VN=%d mode=%d) -- answered, unix=%lu", ntpUdp.remoteIP().toString().c_str(),
       vn, mode, (unsigned long)tv.tv_sec);
#endif
}

// ---------------------------------------------------------------- HTTP (raw, logs everything)
static void urlDecode(char* s) {
  char* w = s;
  for (char* r = s; *r; r++) {
    if (*r == '+') *w++ = ' ';
    else if (*r == '%' && isxdigit((unsigned char)r[1]) && isxdigit((unsigned char)r[2])) {
      char hex[3] = {r[1], r[2], 0};
      *w++ = (char)strtol(hex, nullptr, 16);
      r += 2;
    } else *w++ = *r;
  }
  *w = 0;
}

static void httpLoop() {
  WiFiClient c = http80.accept();
  if (!c) return;
  c.setTimeout(2);
  // Some clients (seen: Windows) open a connection and send a blank line first, or nothing at all.
  // Skip up to 3 blank lines before treating a line as the request line.
  String reqLine;
  for (int i = 0; i < 3 && c.connected(); i++) {
    reqLine = c.readStringUntil('\n');
    reqLine.trim();
    if (reqLine.length()) break;
  }
  if (reqLine.length() == 0) {
    logf("HTTP", "from %s: <no request line, closed>", c.remoteIP().toString().c_str());
    c.stop();
    return;
  }
  // the UTC time of the request when the clock is synced ("-" before): the log pairs uploads with the
  // console's own dateutc and its upload interval
  char utc[24] = "-";
  if (haveTime()) {
    const time_t now = time(nullptr);
    struct tm t;
    gmtime_r(&now, &t);
    strftime(utc, sizeof utc, "%Y-%m-%dT%H:%M:%SZ", &t);
  }
  logf("HTTP", "%s from %s: %s", utc, c.remoteIP().toString().c_str(), reqLine.c_str());

  String host;
  while (c.connected()) {
    String h = c.readStringUntil('\n');
    h.trim();
    if (h.length() == 0) break;
    logf("HTTP", "   hdr %s", h.c_str());
    if (h.startsWith("Host:") || h.startsWith("host:")) { host = h.substring(5); host.trim(); }
  }

  // method SP target SP version
  int s1 = reqLine.indexOf(' '), s2 = reqLine.indexOf(' ', s1 + 1);
  String target = (s1 > 0 && s2 > s1) ? reqLine.substring(s1 + 1, s2) : reqLine;
  int q = target.indexOf('?');
  String path = q >= 0 ? target.substring(0, q) : target;
  String query = q >= 0 ? target.substring(q + 1) : "";

  if (query.length()) {
    char buf[1024];
    strlcpy(buf, query.c_str(), sizeof buf);
    char* save = nullptr;
    for (char* kv = strtok_r(buf, "&", &save); kv; kv = strtok_r(nullptr, "&", &save)) {
      char* eq = strchr(kv, '=');
      if (eq) { *eq = 0; urlDecode(eq + 1); urlDecode(kv); logf("HTTP", "   arg %s = %s", kv, eq + 1); }
      else { urlDecode(kv); logf("HTTP", "   arg %s", kv); }
    }
  }

  bool isWU = path == WU_PATH;
  bool isWC = host.indexOf("weathercloud") >= 0;
  const char* body = isWC ? "200" : "success";
  logf("HTTP", "   -> %s%s reply \"%s\"", isWU ? "WU-UPLOAD " : "", isWC ? "WEATHERCLOUD" : (isWU ? "" : "OTHER"), body);

  c.printf("HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: %u\r\nConnection: close\r\n\r\n%s",
           (unsigned)strlen(body), body);
  c.flush();
  delay(1);
  c.stop();
}

static void tlsLoop() {
  WiFiClient c = tls443.accept();
  if (!c) return;
  logf("TCP443", "connection attempt from %s -- TLS-capable firmware? (dropped)", c.remoteIP().toString().c_str());
  delay(5);
  c.stop();
}

// ---------------------------------------------------------------- setup / loop
void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println();
  logf("BOOT", "discovery sketch, core %s, STA uplink %s", ESP_ARDUINO_VERSION_STR, HAVE_STA ? "configured" : "absent");

  WiFi.onEvent(onWiFiEvent);
  WiFi.mode(HAVE_STA ? WIFI_AP_STA : WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASS);
  WiFi.softAPConfig(AP_IP, AP_IP, AP_MASK);  // core 3.x: configure after start (restarts the DHCP server)
  logf("AP", "SSID \"%s\"  IP %s  (DHCP should advertise this IP as DNS)", AP_SSID, WiFi.softAPIP().toString().c_str());
#if HAVE_STA
  WiFi.begin(STA_SSID, STA_PASS);
#endif

  dnsUdp.begin(53);
  ntpUdp.begin(123);
  http80.begin();
  tls443.begin();
  logf("BOOT", "listening: UDP/53 DNS, UDP/123 NTP, TCP/80 HTTP, TCP/443 probe");
}

void loop() {
  dnsLoop();
  ntpLoop();
  httpLoop();
  tlsLoop();
  static uint32_t lastBeat = 0;
  if (millis() - lastBeat > 60000) {
    lastBeat = millis();
    logf("BEAT", "AP clients=%d heap=%u time=%s", WiFi.softAPgetStationNum(), (unsigned)ESP.getFreeHeap(),
         haveTime() ? "synced" : "none");
  }
}
