// app_net.cpp -- Wi-Fi per mode, events, scan, setup join, Wi-Fi change with revert, mDNS, DNS/NTP (see app.h).
#include <ESPmDNS.h>
#include <WiFi.h>
#include <esp_sntp.h>
#include <esp_wifi.h>
#include <lwip/etharp.h>
#include <lwip/sockets.h>
#include <lwip/tcpip.h>
#include <mdns.h>
#include <sys/time.h>

#include "app.h"
#include "src/dns_wire.h"
#include "src/lifecycle.h"
#include "src/ntp_wire.h"
#include "src/tls_sni.h"

namespace app {

static const IPAddress AP_IP(192, 168, 77, 1);  // NOT 192.168.1.x (the console's own setup AP); not 192.168.4.1
static const IPAddress AP_MASK(255, 255, 255, 0);
static const int AP_MAX_CLIENTS = 4;            // up to 4 consoles

static volatile bool gStaUp = false;
static std::atomic<uint32_t> gGotIpCount{0};   // GOT_IP events since boot: "a NEW connection happened"
static char gStaReason[48] = "";
static volatile int gStaReasonCode = 0;        // last disconnect reason during an attempt (0 = none)
static volatile bool gStaAssociated = false;   // the STA reached the AP during an attempt
static portMUX_TYPE gReasonMux = portMUX_INITIALIZER_UNLOCKED;
static volatile bool gMdnsWanted = false, gApRestart = false, gMdnsUp = false;

// Wi-Fi attempts (loop() only, read by the httpd task as strings)
static WifiAttempt gJoin;          // setup: credentials in gSettings
static uint32_t gJoinIpBase = 0;
static volatile bool gJoinStartReq = false;
static char gSetupState[64] = "idle";
static WifiAttempt gChange;        // admin: new credentials, revert on failure
static uint32_t gChangeIpBase = 0;
static char gChangeSsid[33], gChangePass[64];
const char* netChangeSsid() { return gChangeSsid; }  // the boot log's redaction (read up to 32 B; any task)
static volatile bool gChangeStartReq = false;
static volatile uint32_t gChangeStartAt = 0;  // the PUT's response must leave the ESP before the STA drops
static char gChangeStatus[96] = "";

bool staUp() { return gStaUp; }

void staIpStr(char out[16]) {
  if (!gStaUp) {
    out[0] = 0;
    return;
  }
  snprintf(out, 16, "%s", WiFi.localIP().toString().c_str());
}

void staMaskStr(char out[16]) {
  if (!gStaUp) {
    out[0] = 0;
    return;
  }
  snprintf(out, 16, "%s", WiFi.subnetMask().toString().c_str());
}

static void reasonCopy(char* out, size_t cap) {
  portENTER_CRITICAL(&gReasonMux);
  snprintf(out, cap, "%s", gStaReason);
  portEXIT_CRITICAL(&gReasonMux);
}

// ---------------------------------------------------------------- the console's MAC
// The AP's leases as the DHCP server hands them out (event task writes, httpd task reads): ip -> MAC.
static struct {
  uint32_t ip;
  uint8_t mac[6];
} gLease[8];
static portMUX_TYPE gLeaseMux = portMUX_INITIALIZER_UNLOCKED;
static void leaseRemember(uint32_t ip, const uint8_t mac[6]) {
  portENTER_CRITICAL(&gLeaseMux);
  int at = 0;
  for (int i = 0; i < 8; i++) {
    if (gLease[i].ip == ip || !memcmp(gLease[i].mac, mac, 6)) { at = i; break; }  // the same lease or client again
    if (!gLease[i].ip) at = i;
  }
  gLease[at].ip = ip;
  memcpy(gLease[at].mac, mac, 6);
  portEXIT_CRITICAL(&gLeaseMux);
}

bool apLeaseOf(const uint8_t mac[6], uint32_t* ip) {
  bool found = false;
  portENTER_CRITICAL(&gLeaseMux);
  for (auto& l : gLease)
    if (l.ip && !memcmp(l.mac, mac, 6)) {
      *ip = l.ip;
      found = true;
    }
  portEXIT_CRITICAL(&gLeaseMux);
  return found;
}

bool peerMac(uint32_t ip, SrcIf via, uint8_t mac[6]) {
  if (!ip) return false;
  if (via == SrcIf::Ap) {
    bool found = false;
    portENTER_CRITICAL(&gLeaseMux);
    for (auto& l : gLease)
      if (l.ip == ip) {
        memcpy(mac, l.mac, 6);
        found = true;
      }
    portEXIT_CRITICAL(&gLeaseMux);
    return found;
  }
  if (via != SrcIf::Sta || !staUp()) return false;
  // only a console in the home subnet: behind a router the ARP entry is the router's, not the console's
  const uint32_t mine = (uint32_t)WiFi.localIP(), mask = (uint32_t)WiFi.subnetMask();
  if ((ip & mask) != (mine & mask)) return false;
  ip4_addr_t a;
  a.addr = ip;
  struct eth_addr* e = nullptr;
  const ip4_addr_t* got = nullptr;
  LOCK_TCPIP_CORE();
  const bool ok = etharp_find_addr(nullptr, &a, &e, &got) >= 0 && e;
  if (ok) memcpy(mac, e->addr, 6);
  UNLOCK_TCPIP_CORE();
  return ok;
}

// ---------------------------------------------------------------- events
// A console that drops off the AP and back is a bug-report fact, so the three lines stay Events, rate
// limited (a change prints -- the other direction, or another client; a repeat prints the first of a spell, then one per
// 10 min with the count; a flap holds its changes after four in 10 min and the heartbeat prints the settled state; the
// lease line rides with a printed "joined"). gApLineMux: the Wi-Fi event task and the heartbeat.
static ApLine gApLine;
static portMUX_TYPE gApLineMux = portMUX_INITIALIZER_UNLOCKED;
static ApLine::Out apEvent(bool join, const uint8_t* m, uint32_t* rn, uint32_t* rm) {
  portENTER_CRITICAL(&gApLineMux);
  const ApLine::Out o = gApLine.event(join, m, millis(), rn, rm);
  portEXIT_CRITICAL(&gApLineMux);
  return o;
}
void apLineSettle() {
  uint32_t n = 0;
  bool join = false;
  uint8_t m[6];
  portENTER_CRITICAL(&gApLineMux);
  const bool due = gApLine.settle(millis(), &n, &join, m);
  portEXIT_CRITICAL(&gApLineMux);
  if (due)
    logf("AP", "client now %s MAC %02X:%02X:%02X:%02X:%02X:%02X (x%lu changes)", join ? "joined" : "left", m[0], m[1], m[2],
         m[3], m[4], m[5], (unsigned long)n);
}
static void onWiFiEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
  uint32_t rn = 1, rm = 0;
  char sx[32];
  switch (event) {
    case ARDUINO_EVENT_WIFI_AP_STACONNECTED: {
      const uint8_t* m = info.wifi_ap_staconnected.mac;
      captureApJoin(m);
      if (apEvent(true, m, &rn, &rm) != ApLine::None)
        logf("AP", "client joined MAC %02X:%02X:%02X:%02X:%02X:%02X%s", m[0], m[1], m[2], m[3], m[4], m[5],
             rateSuffix(sx, sizeof sx, rn, rm));
      break;
    }
    case ARDUINO_EVENT_WIFI_AP_STADISCONNECTED: {
      const uint8_t* m = info.wifi_ap_stadisconnected.mac;
      captureApLeave(m);
      if (apEvent(false, m, &rn, &rm) != ApLine::None)
        logf("AP", "client left MAC %02X:%02X:%02X:%02X:%02X:%02X%s", m[0], m[1], m[2], m[3], m[4], m[5],
             rateSuffix(sx, sizeof sx, rn, rm));
      break;
    }
    case ARDUINO_EVENT_WIFI_AP_STAIPASSIGNED: {
      leaseRemember(info.wifi_ap_staipassigned.ip.addr, info.wifi_ap_staipassigned.mac);
      captureApLease(info.wifi_ap_staipassigned.ip.addr, info.wifi_ap_staipassigned.mac);
      portENTER_CRITICAL(&gApLineMux);
      const bool lease = gApLine.lease();
      portEXIT_CRITICAL(&gApLineMux);
      if (lease) logf("AP", "DHCP lease given: %s", IPAddress(info.wifi_ap_staipassigned.ip.addr).toString().c_str());
      break;
    }
    case ARDUINO_EVENT_WIFI_STA_CONNECTED:
      gStaAssociated = true;
      break;
    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
      gStaUp = true;
      gGotIpCount++;
      // only the first join is a boot fact; a flap in the first 120 s must not crowd the head
      logl(gGotIpCount == 1 ? Log::Boot : Log::Event, "STA", "connected to \"%s\", IP %s, channel %d, RSSI %d dBm", WiFi.SSID().c_str(),
           WiFi.localIP().toString().c_str(), (int)WiFi.channel(), (int)WiFi.RSSI());
      if (gMode == Mode::Run) {
        gMdnsWanted = true;
        requestMqttStart();  // starts the client when there is none; an existing one reconnects itself
      }
      break;
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED: {
      const bool wasUp = gStaUp;
      gStaUp = false;
      gStaReasonCode = info.wifi_sta_disconnected.reason;
      const char* why = WiFi.disconnectReasonName((wifi_err_reason_t)info.wifi_sta_disconnected.reason);
      char prev[48];
      reasonCopy(prev, sizeof prev);
      portENTER_CRITICAL(&gReasonMux);
      snprintf(gStaReason, sizeof gStaReason, "%s", why ? why : "unknown");
      portEXIT_CRITICAL(&gReasonMux);
      // the STA retries every few seconds: log transitions and new reasons only
      if (wasUp || strcmp(prev, why ? why : "unknown") != 0)
        logf("STA", "disconnected (%s)%s", why ? why : "unknown", gMode == Mode::Run ? ", AP stays up" : "");
      break;
    }
    default:
      break;
  }
}

// WPA2/WPA3 mixed with PMF capable, not required -- the setup AP's shared password then
// gives a radio neighbour nothing to decrypt for a phone that uses SAE (per-session keys); a WPA2-only phone still
// joins. The run-mode AP stays WPA2: the console's WPA3 support is not verified.
// Returns the auth mode the driver reports back (the log says what the AP really offers); a WPA2/WPA3
// start that did not take falls back to WPA2.
static const char* apUp(const char* ssid, const char* pass, bool wpa3) {
  bool ok = WiFi.softAP(ssid, pass, 1, 0, AP_MAX_CLIENTS, false, wpa3 ? WIFI_AUTH_WPA2_WPA3_PSK : WIFI_AUTH_WPA2_PSK);
  wifi_config_t c = {};
  if (wpa3 && ok && esp_wifi_get_config(WIFI_IF_AP, &c) == ESP_OK) {
    c.ap.pmf_cfg.capable = true;
    c.ap.pmf_cfg.required = false;
    ok = esp_wifi_set_config(WIFI_IF_AP, &c) == ESP_OK;
  }
  if (wpa3 && (!ok || esp_wifi_get_config(WIFI_IF_AP, &c) != ESP_OK || c.ap.authmode != WIFI_AUTH_WPA2_WPA3_PSK)) {
    logf("AP", "WPA2/WPA3 did not start: falling back to WPA2");
    WiFi.softAP(ssid, pass, 1, 0, AP_MAX_CLIENTS, false, WIFI_AUTH_WPA2_PSK);
  }
  WiFi.softAPConfig(AP_IP, AP_IP, AP_MASK);  // core 3.x: configure after start (restarts the DHCP server)
  if (esp_wifi_get_config(WIFI_IF_AP, &c) != ESP_OK) return "unknown";
  switch (c.ap.authmode) {
    case WIFI_AUTH_WPA2_PSK: return "WPA2";
    case WIFI_AUTH_WPA2_WPA3_PSK: return c.ap.pmf_cfg.capable ? "WPA2/WPA3, PMF capable" : "WPA2/WPA3";
    case WIFI_AUTH_WPA3_PSK: return "WPA3";
    default: return "other";
  }
}

void netStartRun() {
  snprintf(gApSsid, sizeof gApSsid, "fauxcloud-%s", gMac4);
  WiFi.onEvent(onWiFiEvent);
  // before mode(): the core copies the name into the STA netif only when mode() enables the STA, and DHCP sends that
  WiFi.setHostname(gSettings.hostname);
  WiFi.mode(WIFI_AP_STA);
  WiFi.setSleep(false);  // modem sleep adds latency to inbound TCP and UDP
  // AP first, and independent of the STA: the console must always find its Wi-Fi.
  apUp(gApSsid, gSettings.ap_pass, false);
  logf("AP", "run mode: SSID \"%s\" IP %s, max %d clients (password from settings)", gApSsid,
       WiFi.softAPIP().toString().c_str(), AP_MAX_CLIENTS);
  udpStart(true);
  // The router's DHCP option 42 lands in slot 0 and lwIP then clears slots 1-2;
  // clockTick moves it to slot 2 and puts the public hosts back in 0-1 (sntpPlan: SNTP before DHCP).
  esp_sntp_servermode_dhcp(true);
  configTime(0, 0, SNTP_HOST_A, SNTP_HOST_B);
  WiFi.begin(gSettings.sta_ssid, gSettings.sta_pass);
  applyTz();  // configTime() resets TZ to UTC0; ours goes back on top
}

static void tls443Start();  // setup mode's port-443 logger, below

void netStartSetup(const char* setup_pass) {
  snprintf(gApSsid, sizeof gApSsid, "fauxcloud-setup-%s", gMac4);
  WiFi.onEvent(onWiFiEvent);
  WiFi.setHostname(gSettings.hostname);  // before mode(), as in netStartRun
  WiFi.mode(WIFI_AP_STA);  // the STA is needed for the scan and the join attempt
  WiFi.setSleep(false);
  const char* auth = apUp(gApSsid, setup_pass, true);
  logf("SETUP", "Wi-Fi \"%s\" open for setup (%s, shared password, see README), up to 30 min", gApSsid, auth);
  udpStart(false);  // DNS for the captive page; no NTP responder in setup mode
  tls443Start();    // which HTTPS host a phone's captive check asks for
  scanStart();      // scan once on entry; the page reads the cache
}

void requestApRestart() { gApRestart = true; }

void netReinit() {
  WiFi.disconnect(false, false);  // the STA only; the AP keeps serving the console
  delay(100);
  WiFi.begin(gSettings.sta_ssid, gSettings.sta_pass);
}
void requestMdnsRestart() { gMdnsWanted = true; }

// mDNS on the STA side only: the core registers the AP netif too, so it is switched off there.
static void mdnsStart() {
  if (gMdnsUp) MDNS.end();
  gMdnsUp = MDNS.begin(gSettings.hostname);
  if (!gMdnsUp) {
    logf("MDNS", "start failed");
    return;
  }
  MDNS.addService("http", "tcp", 80);
  esp_err_t rc = mdns_netif_action(WiFi.AP.netif(), (mdns_event_actions_t)(MDNS_EVENT_DISABLE_IP4 | MDNS_EVENT_DISABLE_IP6));
  logl(Log::Boot, "MDNS", "%s.local on the STA side (AP side %s)", gSettings.hostname, rc == ESP_OK ? "disabled" : "NOT disabled");
}

// ---------------------------------------------------------------- scan (cached <= 10 s)
struct Net {
  char ssid[33];
  int8_t rssi;
  bool open;
};
static Net gNets[20];
static int gNetCount = 0;
static uint32_t gNetAtMs = 0;
static bool gScanning = false;
static portMUX_TYPE gScanMux = portMUX_INITIALIZER_UNLOCKED;

static uint32_t gScanStartMs = 0;
static const uint32_t RESCAN_MIN_MS = 15000;

// Active scan, 120 ms per channel: the radio leaves the AP channel only briefly, so a phone on the setup AP stays
// connected through it.
void scanStart() {
  if (gScanning) return;
  gScanning = WiFi.scanNetworks(true, false, false, 120) == WIFI_SCAN_RUNNING;
  gScanStartMs = millis();
  if (!gScanning) logf("WIFI", "scan could not start");
}

static bool scanBusy() { return gScanning; }

void scanJson(Out& o, bool again) {
  const uint32_t ms = millis();
  bool throttled = false;
  if (again && !gScanning) {
    if (gScanStartMs && ms - gScanStartMs < RESCAN_MIN_MS) throttled = true;
    else scanStart();
  }
  if (!gScanning && !gNetAtMs) scanStart();  // nothing cached yet
  if (gScanning) {
    const int16_t r = WiFi.scanComplete();
    if (r == WIFI_SCAN_RUNNING) {
      o.raw("{\"state\":\"running\",\"networks\":[]}");
      return;
    }
    gScanning = false;
    int n = 0;
    for (int i = 0; i < r && n < 20; i++) {
      String ssid = WiFi.SSID(i);
      if (!ssid.length()) continue;  // hidden networks
      int dup = -1;
      for (int k = 0; k < n; k++)
        if (strcmp(gNets[k].ssid, ssid.c_str()) == 0) dup = k;
      const int8_t rssi = (int8_t)WiFi.RSSI(i);
      if (dup >= 0) {
        if (rssi > gNets[dup].rssi) gNets[dup].rssi = rssi;
        continue;
      }
      snprintf(gNets[n].ssid, sizeof gNets[n].ssid, "%s", ssid.c_str());
      gNets[n].rssi = rssi;
      gNets[n].open = WiFi.encryptionType(i) == WIFI_AUTH_OPEN;
      n++;
    }
    WiFi.scanDelete();
    for (int a = 1; a < n; a++)  // strongest first
      for (int b = a; b > 0 && gNets[b].rssi > gNets[b - 1].rssi; b--) {
        Net t = gNets[b];
        gNets[b] = gNets[b - 1];
        gNets[b - 1] = t;
      }
    portENTER_CRITICAL(&gScanMux);
    gNetCount = r < 0 ? 0 : n;
    gNetAtMs = ms ? ms : 1;
    portEXIT_CRITICAL(&gScanMux);
  }
  o.printf("{\"state\":\"done\",\"age_s\":%lu,%s\"networks\":[", (unsigned long)((ms - gNetAtMs) / 1000),
           throttled ? "\"throttled\":true," : "");
  for (int i = 0; i < gNetCount; i++) {
    o.raw(i ? ",{\"ssid\":" : "{\"ssid\":");
    o.jsonStr(gNets[i].ssid);
    o.printf(",\"rssi\":%d,\"open\":%s}", gNets[i].rssi, gNets[i].open ? "true" : "false");
  }
  o.raw("]}");
}

// ---------------------------------------------------------------- setup join (setup mode)
const char* setupState(char* buf, size_t cap) {
  portENTER_CRITICAL(&gReasonMux);
  snprintf(buf, cap, "%s", gSetupState);
  portEXIT_CRITICAL(&gReasonMux);
  if (strcmp(buf, "idle") == 0 && scanBusy()) snprintf(buf, cap, "busy");
  return buf;
}

static void setSetupState(const char* s) {
  portENTER_CRITICAL(&gReasonMux);
  snprintf(gSetupState, sizeof gSetupState, "%s", s);
  portEXIT_CRITICAL(&gReasonMux);
}

// The state is "joining" before the apply answers: the page polls at once, and the loop's setupTick may start the
// join a few hundred ms later -- until then a re-apply after a failed join would still read the old failed:<why>.
void setupJoinStart() {
  setSetupState("joining");
  gJoinStartReq = true;
}

// ---------------------------------------------------------------- setup mode: port 443 logger
// Android's captive check may go over HTTPS only. Nothing serves TLS here: a connection on 443 (AP side only) is
// accepted, its ClientHello read for up to 300 ms, logged with the server name it asks for, and closed. One
// listening socket, one connection at a time, a 512 B static buffer. The log names each server once per 10 min (the
// phone's app traffic retries every second); a count summary follows when the join starts.
static int gTls443 = -1;
static NameLogLimiter gTlsLim;
static uint32_t gTlsConns = 0, gTlsNoName = 0;

static void tls443Summary() {
  if (gTls443 >= 0 && gTlsConns)
    logf("TCP443", "%lu HTTPS connection attempt(s) on the setup Wi-Fi so far, %lu without a server name",
         (unsigned long)gTlsConns, (unsigned long)gTlsNoName);
}

static void tls443Start() {
  const int s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (s < 0) return;
  const int one = 1;
  setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  struct sockaddr_in a = {};
  a.sin_family = AF_INET;
  a.sin_port = htons(443);
  a.sin_addr.s_addr = apIpNet();
  if (bind(s, (struct sockaddr*)&a, sizeof a) != 0 || listen(s, 2) != 0) {
    logf("TCP443", "listen failed, errno %d", errno);
    close(s);
    return;
  }
  fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK);
  gTls443 = s;
  logf("TCP443", "logging HTTPS connection attempts on the setup Wi-Fi (nothing is served)");
}

static void tls443Tick() {
  if (gTls443 < 0) return;
  struct sockaddr_in from;
  socklen_t fl = sizeof from;
  const int c = accept(gTls443, (struct sockaddr*)&from, &fl);
  if (c < 0) return;
  static uint8_t buf[512];
  struct timeval tv = {0, 300000};
  setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
  const int n = recv(c, buf, sizeof buf, 0);
  close(c);
  char ip[16], sni[64];
  inet_ntoa_r(from.sin_addr, ip, sizeof ip);
  gTlsConns++;
  if (n > 0 && tlsSni(buf, (size_t)n, sni, sizeof sni)) {
    if (gTlsLim.shouldLog(sni, millis() / 1000))
      logf("TCP443", "%s sni=%s (%d B, closed; each name logged once per 10 min)", ip, sni, n);
  } else {
    gTlsNoName++;
  }
}

static void setupTick() {
  const uint32_t ms = millis();
  tls443Tick();
  if (gJoinStartReq) {
    gJoinStartReq = false;
    WiFi.disconnect(false, false);
    gStaUp = false;
    portENTER_CRITICAL(&gReasonMux);
    gStaReason[0] = 0;
    portEXIT_CRITICAL(&gReasonMux);
    gJoinIpBase = gGotIpCount.load();
    gStaReasonCode = 0;
    gStaAssociated = false;
    WiFi.setHostname(gSettings.hostname);
    WiFi.begin(gSettings.sta_ssid, gSettings.sta_pass);
    gJoin.start(ms);
    setSetupState("joining");
    logf("SETUP", "joining \"%s\" (up to 60 s)", gSettings.sta_ssid);
    tls443Summary();
  }
  if (gJoin.state() != WifiAttempt::State::Trying) return;
  const bool connected = gStaUp && gGotIpCount.load() > gJoinIpBase;
  switch (gJoin.tick(ms, connected, gStaReasonCode, gStaAssociated)) {
    case WifiAttempt::State::Succeeded: {
      xSemaphoreTake(gLock, portMAX_DELAY);
      gSettings.setup_done = true;
      xSemaphoreGive(gLock);
      const bool saved = saveSettings() && saveBridge();
      logf("SETUP", "joined; settings %s -> reboot into run mode in 6 s", saved ? "saved" : "NOT saved");
      setSetupState("joined");
      scheduleReboot(6000);  // time for the page (polling every 2 s) to show the checklist
      break;
    }
    case WifiAttempt::State::Failed: {
      char s[64], why[48];
      snprintf(s, sizeof s, "failed:%s", gJoin.reason());
      setSetupState(s);
      reasonCopy(why, sizeof why);
      logf("SETUP", "join failed: %s (last reason %s); staying in setup mode", gJoin.reason(), why[0] ? why : "none");
      WiFi.disconnect(false, false);
      gJoin.reset();
      break;
    }
    default:
      break;
  }
}

// ---------------------------------------------------------------- Wi-Fi change with revert (run mode)
void wifiChangeStart(const char* ssid, const char* pass) {
  snprintf(gChangeSsid, sizeof gChangeSsid, "%s", ssid);
  snprintf(gChangePass, sizeof gChangePass, "%s", pass);
  gChangeStartAt = millis() + 1500;
  gChangeStartReq = true;
}

const char* wifiChangeStatus() { return gChangeStatus; }

static void setChangeStatus(const char* s) {
  portENTER_CRITICAL(&gReasonMux);
  snprintf(gChangeStatus, sizeof gChangeStatus, "%s", s);
  portEXIT_CRITICAL(&gReasonMux);
}

static void changeTick() {
  const uint32_t ms = millis();
  if (gChangeStartReq && (int32_t)(ms - gChangeStartAt) >= 0) {
    gChangeStartReq = false;
    setChangeStatus("busy");
    logf("WIFI", "trying new network \"%s\" for up to 60 s (old one restored on failure)", gChangeSsid);
    WiFi.disconnect(false, false);
    gStaUp = false;
    portENTER_CRITICAL(&gReasonMux);
    gStaReason[0] = 0;
    portEXIT_CRITICAL(&gReasonMux);
    gChangeIpBase = gGotIpCount.load();
    gStaReasonCode = 0;
    gStaAssociated = false;
    WiFi.begin(gChangeSsid, gChangePass);
    gChange.start(ms);
  }
  if (gChange.state() != WifiAttempt::State::Trying) return;
  const bool connected = gStaUp && gGotIpCount.load() > gChangeIpBase;
  switch (gChange.tick(ms, connected, gStaReasonCode, gStaAssociated)) {
    case WifiAttempt::State::Succeeded: {
      xSemaphoreTake(gLock, portMAX_DELAY);
      snprintf(gSettings.sta_ssid, sizeof gSettings.sta_ssid, "%s", gChangeSsid);
      snprintf(gSettings.sta_pass, sizeof gSettings.sta_pass, "%s", gChangePass);
      xSemaphoreGive(gLock);
      const bool saved = saveSettings();
      setChangeStatus("ok");
      logf("WIFI", "joined \"%s\"%s", gChangeSsid, saved ? "" : " (credentials NOT saved)");
      gChange.reset();
      memset(gChangePass, 0, sizeof gChangePass);
      break;
    }
    case WifiAttempt::State::Failed: {
      char s[96];
      snprintf(s, sizeof s, "failed:%s", gChange.reason());  // the old network is restored
      setChangeStatus(s);
      logf("WIFI", "new network failed (%s) -- reverting to \"%s\"", gChange.reason(), gSettings.sta_ssid);
      WiFi.disconnect(false, false);
      WiFi.begin(gSettings.sta_ssid, gSettings.sta_pass);
      gChange.reset();
      memset(gChangePass, 0, sizeof gChangePass);
      break;
    }
    default:
      break;
  }
}

void netLoop() {
  if (gMode == Mode::Setup) {
    setupTick();
    return;
  }
  changeTick();
  if (gApRestart) {
    gApRestart = false;
    apUp(gApSsid, gSettings.ap_pass, false);
    logf("AP", "restarted with the new password -- consoles must be re-paired");
  }
  if (gMdnsWanted && gStaUp) {
    gMdnsWanted = false;
    mdnsStart();
  }
}

// ---------------------------------------------------------------- DNS + NTP (AP side only)
static int udpBind(uint16_t port) {
  int s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (s < 0) return -1;
  struct sockaddr_in a = {};
  a.sin_family = AF_INET;
  a.sin_port = htons(port);
  a.sin_addr.s_addr = apIpNet();  // never INADDR_ANY: the STA side must not answer DNS/NTP
  if (bind(s, (struct sockaddr*)&a, sizeof a) != 0) {
    logf("UDP", "bind %u failed, errno %d", port, errno);
    close(s);
    return -1;
  }
  return s;
}

static void handleDns(int s, NameLogLimiter& lim) {
  static uint8_t in[512], out[600];
  struct sockaddr_in from;
  socklen_t fl = sizeof from;
  int n = recvfrom(s, in, sizeof in, 0, (struct sockaddr*)&from, &fl);
  if (n <= 0) return;
  DnsQuestion q;
  if (!dnsParseQuery(in, (size_t)n, &q)) {
    gDnsRejected++;
    return;
  }
  const uint32_t ip = apIpNet();
  size_t m = dnsBuildAnswer(in, q, (const uint8_t*)&ip, out, sizeof out);
  if (!m) {
    gDnsRejected++;
    return;
  }
  sendto(s, out, m, 0, (struct sockaddr*)&from, fl);
  gDnsAnswered++;
  captureDns(q.name, q.qtype, from.sin_addr.s_addr);
  bool first;
  if (lim.shouldLog(q.name, millis() / 1000, &first)) {  // a name's repeats are chatter
    char ipStr[16];
    ipToStr(from.sin_addr.s_addr, ipStr);
    char name[sizeof q.name];
    snprintf(name, sizeof name, "%s", q.name);
    logSafe(name);
    logl(first ? Log::Event : Log::Chatter, "DNS", "%s type=%u from %s -> %s (each name logged once per 10 min)", name, q.qtype, ipStr,
         q.qtype == 1 ? "192.168.77.1" : "no records");
  }
}

static void handleNtp(int s, NameLogLimiter& lim) {
  static uint8_t in[128], out[NTP_PACKET];
  struct sockaddr_in from;
  socklen_t fl = sizeof from;
  int n = recvfrom(s, in, sizeof in, 0, (struct sockaddr*)&from, &fl);
  if (n <= 0) return;
  NtpRequest r;
  if (!ntpParseRequest(in, (size_t)n, &r)) return;
  char ipStr[16];
  ipToStr(from.sin_addr.s_addr, ipStr);
  char key[24];
  snprintf(key, sizeof key, "ntp:%s", ipStr);
  // Only a real clock (SNTP, DHCP-NTP, browser seed) answers. A cold-started console
  // uploads nothing until then; it keeps whatever it gets first and ignores later re-syncs, so an approximate
  // answer would stick (clock.h).
  struct timeval tv;
  gettimeofday(&tv, nullptr);
  // a clock held behind the stored history is not given to the console either
  const bool held = clockHeld((uint32_t)tv.tv_sec, storeClockRef());
  bool first;  // a client's repeats are chatter
  if (ntpAnswer((uint32_t)tv.tv_sec) == NtpAnswer::None || held) {
    gNtpUnanswered++;
    captureNtp(false, from.sin_addr.s_addr);
    if (lim.shouldLog(key, millis() / 1000, &first))
      logl(first ? Log::Event : Log::Chatter, "NTP", "request from %s (VN=%d mode=%d) -- NOT answered, %s%s", ipStr, r.vn, r.mode,
           held ? "the clock is held (behind the stored history)" : "no real time yet (SNTP, DHCP-NTP or a browser seed)",
           timeSrc() == TimeSrc::Persisted ? "; approximate time is not given to consoles" : "");
    return;
  }
  ntpBuildReply(in, r, (uint32_t)tv.tv_sec, (uint32_t)tv.tv_usec, out);
  sendto(s, out, sizeof out, 0, (struct sockaddr*)&from, fl);
  gNtpAnswered++;
  captureNtp(true, from.sin_addr.s_addr);
  if (lim.shouldLog(key, millis() / 1000, &first))
    logl(first ? Log::Event : Log::Chatter, "NTP", "request from %s (VN=%d mode=%d) -- answered (%s), unix=%lu (logged once per 10 min per client)", ipStr,
         r.vn, r.mode, timeSrcName(timeSrc()), (unsigned long)tv.tv_sec);
}

// Blocks in select() on both sockets; runs above loop() so answers never wait for MQTT.
static void udpTask(void* arg) {
  static NameLogLimiter lim;  // this task only
  const bool ntpOn = arg != nullptr;
  const int dns = udpBind(53), ntp = ntpOn ? udpBind(123) : -1;
  logl(Log::Boot, "UDP", "DNS %s, NTP %s on 192.168.77.1 (AP side only)", dns >= 0 ? "up" : "FAILED",
       ntpOn ? (ntp >= 0 ? "up" : "FAILED") : "off (setup mode)");
  if (dns < 0 && ntp < 0) vTaskDelete(nullptr);
  const int maxfd = dns > ntp ? dns : ntp;
  for (;;) {
    fd_set rf;
    FD_ZERO(&rf);
    if (dns >= 0) FD_SET(dns, &rf);
    if (ntp >= 0) FD_SET(ntp, &rf);
    if (select(maxfd + 1, &rf, nullptr, nullptr, nullptr) <= 0) {
      vTaskDelay(pdMS_TO_TICKS(10));
      continue;
    }
    if (dns >= 0 && FD_ISSET(dns, &rf)) handleDns(dns, lim);
    if (ntp >= 0 && FD_ISSET(ntp, &rf)) handleNtp(ntp, lim);
  }
}

void udpStart(bool ntp) { xTaskCreate(udpTask, "udp53_123", 4096, ntp ? (void*)1 : nullptr, 5, nullptr); }

}  // namespace app
