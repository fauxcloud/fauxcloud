// test_fauxcloud.cpp -- host tests for firmware/fauxcloud/src. No framework, no network.
// Build + run: test/host/run.ps1 (Windows) or test/host/run.sh.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "heapinfo.h"
#include "dns_wire.h"
#include "auth.h"
#include "clock.h"
#include "derive.h"
#include "notices.h"
#include "config.h"
#include "fields.h"
#include "filter.h"
#include "json.h"
#include "tz.h"
#include "ha_discovery.h"
#include "bootlog.h"
#include "history.h"
#include "led.h"
#include "lifecycle.h"
#include "ntp_wire.h"
#include "settings.h"
#include "station.h"
#include "web_render.h"
#include "metrics.h"
#include "console.h"
#include "tls_sni.h"
#include "store.h"
#include "robust.h"
#include "wu_parse.h"
#include "sun.h"
#include "forecast.h"
#include "learn.h"
#include "log.h"
#include "fwd.h"  // fwdEnablePlan in testStationsPerProfile (test_fwd.inc includes it too)

using namespace fc;

static int g_fail = 0, g_pass = 0;

#define CHECK(cond)                                                     \
  do {                                                                  \
    if (cond) g_pass++;                                                 \
    else { g_fail++; printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } \
  } while (0)

#define CHECK_NEAR(a, b, eps)                                                                   \
  do {                                                                                          \
    double a_ = (a), b_ = (b);                                                                  \
    if (std::fabs(a_ - b_) <= (eps)) g_pass++;                                                  \
    else { g_fail++; printf("FAIL %s:%d  %s = %.4f, expected %.4f\n", __FILE__, __LINE__, #a, a_, b_); } \
  } while (0)

static bool contains(const std::string& s, const char* needle) { return s.find(needle) != std::string::npos; }
static HourRec hourRec(uint32_t hour, float temp, float gust, float rain, uint8_t n = 60);  // history record tests

static bool toString(void* ctx, const char* d, size_t n) {
  static_cast<std::string*>(ctx)->append(d, n);
  return true;
}

// A replayed upload: an unpadded date and a garbled solarRadiation value.
static const char* REPLAY =
    "ID=ST1&PASSWORD=x&dateutc=2026-11-1+12%3A5%3A3&baromin=29.92&tempf=50.0&humidity=60&dewptf=37.0"
    "&rainin=0.00&dailyrainin=0.12&winddir=180&windspeedmph=3.0&windgustmph=5.0&UV=1&solarRadiation=-117.-5";

static void testCapturedRequestLine() {
  // A real request line from a public capture (key redacted there; parameters after
  // baromin were elided in the source, so the test stops where the capture does).
  Sample s;
  parseQuery("ID=IEXAMPLE01&PASSWORD=REDACTED&dateutc=2024-12-11+18%3A50%3A3&baromin=29.92", &s);
  CHECK(strcmp(s.station_id, "IEXAMPLE01") == 0);
  CHECK(s.has_dateutc);
  CHECK(s.dateutc == unixFromUtc(2024, 12, 11, 18, 50, 3));
  CHECK(s.has(F_PRESSURE_HPA));
  CHECK_NEAR(s.v[F_PRESSURE_HPA], 1013.21, 0.01);
  CHECK(s.rejected() == 0);
  CHECK(s.n_extras == 0);  // PASSWORD is never kept
}

static void testReplayMetric() {
  Sample s;
  parseQuery(REPLAY, &s);
  CHECK(strcmp(s.station_id, "ST1") == 0);
  CHECK_NEAR(s.v[F_TEMP_C], 10.0, 0.001);
  CHECK_NEAR(s.v[F_DEWPT_C], 2.778, 0.001);
  CHECK_NEAR(s.v[F_HUMIDITY], 60, 0.001);
  CHECK_NEAR(s.v[F_PRESSURE_HPA], 1013.208, 0.001);
  CHECK_NEAR(s.v[F_WIND_KMH], 4.828, 0.001);
  CHECK_NEAR(s.v[F_GUST_KMH], 8.047, 0.001);
  CHECK_NEAR(s.v[F_WIND_DIR], 180, 0.001);
  CHECK_NEAR(s.v[F_RAIN_MM], 0.0, 0.001);
  CHECK_NEAR(s.v[F_RAIN_DAILY_MM], 3.048, 0.001);
  CHECK_NEAR(s.v[F_UV], 1, 0.001);
  CHECK(s.has_dateutc && s.dateutc == unixFromUtc(2026, 11, 1, 12, 5, 3));
  char b[16];
  formatValue(b, sizeof b, F_PRESSURE_HPA, s.v[F_PRESSURE_HPA]);
  CHECK(strcmp(b, "1013.2") == 0);
  formatValue(b, sizeof b, F_WIND_KMH, s.v[F_WIND_KMH]);
  CHECK(strcmp(b, "4.8") == 0);
  formatValue(b, sizeof b, F_GUST_KMH, s.v[F_GUST_KMH]);
  CHECK(strcmp(b, "8.0") == 0);
  formatValue(b, sizeof b, F_RAIN_DAILY_MM, s.v[F_RAIN_DAILY_MM]);
  CHECK(strcmp(b, "3.0") == 0);
}

static void testSolarGarbageDropsOnlySolar() {
  Sample s;
  parseQuery(REPLAY, &s);
  CHECK(!s.has(F_SOLAR_WM2));
  CHECK(s.rejected_parse == (1u << F_SOLAR_WM2));
  CHECK(s.rejected_bounds == 0);
  for (int f = 0; f < CORE_FIELDS; f++)
    if (f != F_SOLAR_WM2) CHECK(s.has(f));
}

static void testUnpaddedDate() {
  int64_t t;
  CHECK(parseDateUtc("2024-12-5 21:0:9", &t));
  CHECK(t == unixFromUtc(2024, 12, 5, 21, 0, 9));
  Sample s;
  parseQuery("dateutc=2024-12-5+21%3A0%3A9", &s);
  CHECK(s.has_dateutc && s.dateutc == unixFromUtc(2024, 12, 5, 21, 0, 9));
  CHECK(strcmp(s.station_id, DEFAULT_STATION_ID) == 0);
  CHECK(!parseDateUtc("now", &t));
  CHECK(!parseDateUtc("2024-13-5 21:0:9", &t));
  CHECK(!parseDateUtc("2024-12-5 21:0", &t));
  char iso[24];
  formatIsoUtc(iso, sizeof iso, unixFromUtc(2024, 12, 5, 21, 0, 9));
  CHECK(strcmp(iso, "2024-12-05T21:00:09Z") == 0);
  formatIsoUtc(iso, sizeof iso, 0);
  CHECK(strcmp(iso, "1970-01-01T00:00:00Z") == 0);
}

static void testParserTolerance() {
  double x;
  CHECK(parseNumber("12.5", &x) && x == 12.5);
  CHECK(parseNumber("-3", &x) && x == -3);
  CHECK(!parseNumber("-117.-5", &x));
  CHECK(!parseNumber("", &x));
  CHECK(!parseNumber("nan", &x));
  CHECK(!parseNumber("0x10", &x));
  CHECK(!parseNumber("1e", &x));

  Sample s;
  // bounds: humidity 120, temp 200 F (93 C), negative wind; unknown params; hostile id; empty value
  parseQuery("ID=a/b%23c&humidity=120&tempf=200&windspeedmph=-1&UV=&foo=bar&softwaretype=X%20Y", &s);
  CHECK(strcmp(s.station_id, "a_b_c") == 0 && s.id_sanitized);
  CHECK(s.rejected_bounds == ((1u << F_HUMIDITY) | (1u << F_TEMP_C) | (1u << F_WIND_KMH)));
  CHECK(!s.has(F_UV) && !((s.rejected() >> F_UV) & 1u));
  CHECK(s.n_extras == 1);  // softwaretype is a WU protocol flag: not an extra
  CHECK(strcmp(s.extras[0].key, "foo") == 0 && strcmp(s.extras[0].val, "bar") == 0);

  // measurement keys in any case; protocol flags ignored; the extended fields, converted
  parseQuery("ID=X&solarradiation=512&uv=3&TEMPF=50&action=updateraw&realtime=1&rtfreq=5&Action=x&SoilTempF=60", &s);
  CHECK(s.has(F_SOLAR_WM2) && std::fabs(s.v[F_SOLAR_WM2] - 512) < .01f && s.has(F_UV) && std::fabs(s.v[F_UV] - 3) < .01f);
  CHECK(s.has(F_TEMP_C) && std::fabs(s.v[F_TEMP_C] - 10) < .01f);
  CHECK(s.n_extras == 1 && strcmp(s.extras[0].key, "SoilTempF") == 0);  // extras keep the sender's spelling
  CHECK(fieldByParam("solarRadiation") == F_SOLAR_WM2 && fieldByParam("SOLARRADIATION") == F_SOLAR_WM2 && fieldByParam("solar") < 0);
  CHECK(isProtocolFlag("action") && isProtocolFlag("SoftwareType") && isProtocolFlag("rtfreq") && !isProtocolFlag("actions"));
  parseQuery("windgustdir=270&windspdmph_avg2m=10&winddir_avg2m=90&windgustmph_10m=20&windgustdir_10m=180&visibility=5", &s);
  CHECK(s.has(F_GUST_DIR) && s.v[F_GUST_DIR] == 270 && s.has(F_WIND_DIR_AVG2M) && s.v[F_WIND_DIR_AVG2M] == 90);
  CHECK(std::fabs(s.v[F_WIND_AVG2M_KMH] - 16.09f) < .01f && std::fabs(s.v[F_GUST_10M_KMH] - 32.19f) < .01f);
  CHECK(s.has(F_GUST_DIR_10M) && std::fabs(s.v[F_VISIBILITY_KM] - 9.26f) < .01f && s.n_extras == 0);
  CHECK(F_COUNT == 27 && CORE_FIELDS == 11);  // live only: no history, no file-format change for the day records

  // extras are capped at MAX_EXTRAS
  parseQuery("a=1&b=2&c=3&d=4&e=5&f=6&g=7&h=8&i=9&j=10", &s);
  CHECK(s.n_extras == MAX_EXTRAS && s.extras_dropped == 6);
  // long unknown names/values are truncated to 15 chars
  parseQuery("averyveryverylongname=0123456789abcdefXYZ", &s);
  CHECK(s.n_extras == 1 && strcmp(s.extras[0].key, "averyveryverylo") == 0 &&
        strcmp(s.extras[0].val, "0123456789abcde") == 0);
  CHECK(sizeof(Sample) <= 288);
  // extended field from a sibling console
  parseQuery("indoortempf=68", &s);
  CHECK(s.has(F_INDOOR_TEMP_C));
  CHECK_NEAR(s.v[F_INDOOR_TEMP_C], 20.0, 0.001);
}

static Sample sampleWith(float temp, float wind, float gust, float dir, float daily) {
  Sample s;
  memset(&s, 0, sizeof s);
  strcpy(s.station_id, "ST1");
  auto set = [&](int f, float v) { s.v[f] = v; s.present |= 1u << f; };
  set(F_TEMP_C, temp);
  set(F_WIND_KMH, wind);
  set(F_GUST_KMH, gust);
  set(F_WIND_DIR, dir);
  set(F_RAIN_DAILY_MM, daily);
  return s;
}

static void testRecentRing() {
  static Recent r;  // static: keep big objects off the stack
  r = Recent();  // a fresh ring
  for (int i = 0; i < 61; i++) r.push(sampleWith((float)i, 0, 0, 0, 0), 1000 + i);
  CHECK(r.size() == RECENT_LEN);
  CHECK(r.at(0).t == 1001);  // sample 0 was overwritten
  CHECK(r.at(RECENT_LEN - 1).t == 1060);
  CHECK_NEAR(unpack(F_TEMP_C, r.at(0).v[F_TEMP_C]), 1.0, 1e-6);
  CHECK_NEAR(unpack(F_TEMP_C, r.at(RECENT_LEN - 1).v[F_TEMP_C]), 60.0, 1e-6);
  CHECK(!hasValue(r.at(0).v[F_SOLAR_WM2]));
  CHECK(sizeof(PackedSample) == 28);
}

static void testDiscoveryPayload() {
  const uint8_t mac[6] = {0x24, 0x6f, 0x28, 0xaa, 0xbb, 0xcc};
  char topic[128], payload[768];
  CHECK(discoveryTopic(topic, sizeof topic, "ST1", mac, F_TEMP_C) > 0);
  CHECK(strcmp(topic, "homeassistant/sensor/fauxcloud_246f28aabbcc_ST1/temp_c/config") == 0);
  CHECK(discoveryPayload(payload, sizeof payload, "ST1", "fauxcloud ST1", mac, F_TEMP_C, "spike-0.3") > 0);
  std::string p = payload;
  CHECK(contains(p, "\"device_class\":\"temperature\""));
  CHECK(contains(p, "\"state_class\":\"measurement\""));
  CHECK(contains(p, "\"unit_of_measurement\":\"°C\""));
  CHECK(contains(p, "\"availability\":[{\"topic\":\"fauxcloud/bridge/246f28aabbcc/status\"},"
                    "{\"topic\":\"fauxcloud/BBCC/ST1/availability\"}]"));
  CHECK(contains(p, "\"availability_mode\":\"all\""));
  CHECK(!contains(p, "availability_topic"));
  CHECK(contains(p, "\"unique_id\":\"fauxcloud_246f28aabbcc_ST1_temp_c\""));
  CHECK(contains(p, "\"state_topic\":\"fauxcloud/BBCC/ST1/temp_c\""));
  CHECK(contains(p, "\"name\":\"fauxcloud ST1\""));
  // the station topics carry the bridge's mac4; the legacy availability topic (no mac4) is still built for the clean-up
  CHECK(stateTopic(topic, sizeof topic, mac, "ST1", F_TEMP_C) > 0 && strcmp(topic, "fauxcloud/BBCC/ST1/temp_c") == 0);
  CHECK(availabilityTopic(topic, sizeof topic, mac, "ST1") > 0 && strcmp(topic, "fauxcloud/BBCC/ST1/availability") == 0);
  CHECK(legacyAvailabilityTopic(topic, sizeof topic, "ST1") > 0 && strcmp(topic, "fauxcloud/ST1/availability") == 0);
  CHECK(stateTopicKey(topic, 20, mac, "ST1", "temp_c") < 0);  // too small: nothing usable
  CHECK(p.front() == '{' && p.back() == '}');

  CHECK(discoveryPayload(payload, sizeof payload, "ST1", "fauxcloud ST1", mac, FIELD_LAST_SEEN, "spike-0.3") > 0);
  p = payload;
  CHECK(contains(p, "\"device_class\":\"timestamp\""));
  CHECK(contains(p, "\"state_topic\":\"fauxcloud/BBCC/ST1/last_seen\""));
  CHECK(!contains(p, "unit_of_measurement"));

  CHECK(discoveryPayload(payload, sizeof payload, "ST1", "fauxcloud ST1", mac, F_UV, "spike-0.3") > 0);
  CHECK(!contains(payload, "unit_of_measurement"));  // UV index has no unit
  CHECK(discoveryPayload(payload, 64, "ST1", "fauxcloud ST1", mac, F_TEMP_C, "spike-0.3") == -1);  // truncation detected
  CHECK(bridgeStatusTopic(topic, sizeof topic, mac) > 0);
  CHECK(strcmp(topic, "fauxcloud/bridge/246f28aabbcc/status") == 0);
  CHECK(discoveryPayload(payload, sizeof payload, "ST1", "fauxcloud ST1", mac, F_WIND_DIR, "b1") > 0);
  CHECK(contains(payload, "\"device_class\":\"wind_direction\""));
  CHECK(contains(payload, "\"state_class\":\"measurement_angle\""));
  CHECK(strlen(payload) < 768);
}

static void testRender() {
  static Stations all;
  all.reserve(MAX_STATIONS);
  Sample s;
  parseQuery(REPLAY, &s);
  const uint32_t now = (uint32_t)unixFromUtc(2026, 11, 1, 12, 5, 10);
  Station* st = all.findOrAdd(s.station_id, defaultStationConfig());
  CHECK(st != nullptr);
  PosixTz tz;
  parsePosixTz(DEFAULT_TZ, &tz);
  Stations::ingest(*st, s, now, 1000, 0x0a01a8c0, SrcIf::Sta, true, tz);
  Clock c{now + 5, true, 100, 6000};

  char buf[256];  // small on purpose: exercises chunked flushing
  std::string live;
  Out o1(toString, &live, buf, sizeof buf);
  Diag d = {};
  d.ap_clients = 1;
  d.dns_answered = 7;
  renderLive(o1, all, c, &d);
  o1.flush();
  CHECK(contains(live, "\"temp_c\":10.0"));
  CHECK(contains(live, "\"pressure_hpa\":1013.2"));
  CHECK(contains(live, "\"last_rejected\":[\"solar_wm2\"]"));
  CHECK(!contains(live, "\"solar_wm2\":-"));
  CHECK(contains(live, "\"age_s\":5"));
  CHECK(contains(live, "\"src_ip\":\"192.168.1.10\""));
  CHECK(contains(live, "\"src_if\":\"sta\""));
  CHECK(contains(live, "\"diag\":{\"ap_clients\":1,"));
  CHECK(contains(live, "\"dns_answered\":7,"));

  std::string hourly;
  Out o2(toString, &hourly, buf, sizeof buf);
  renderHourly(o2, *st);
  o2.flush();
  CHECK(contains(hourly, "\"open\":{\"h\":\"2026-11-01T12:00Z\",\"n\":1,\"q\":[") && contains(hourly, "\"rejected\"") &&
        contains(hourly, "\"rej_fields\":1"));
  CHECK(contains(hourly, "\"temp_c\":[10.0,10.0,10.0]"));
  CHECK(contains(hourly, "\"hours\":[]"));

  std::string recent;
  Out o3(toString, &recent, buf, sizeof buf);
  renderRecent(o3, *st);
  o3.flush();
  CHECK(contains(recent, "\"rows\":[["));
  CHECK(contains(recent, ",null]]}"));  // solar is the last core field and was rejected

  std::string page;
  Out o4(toString, &page, buf, sizeof buf);
  renderPage(o4, all, c, "spike-0.3");
  o4.flush();
  CHECK(contains(page, "<svg"));
  CHECK(contains(page, "rejected</span>"));
  CHECK(!contains(page, "http://") && !contains(page, "https://") && !contains(page, "<script"));
  CHECK(contains(page, "</html>"));
}

// A query as a resolver sends it: id 0xBEEF, RD set, one question, optional EDNS OPT record.
static size_t dnsQuery(uint8_t* out, const char* name, uint16_t qtype, bool edns) {
  size_t n = 0;
  const uint8_t hdr[12] = {0xBE, 0xEF, 0x01, 0x00, 0, 1, 0, 0, 0, 0, 0, (uint8_t)(edns ? 1 : 0)};
  memcpy(out, hdr, 12);
  n = 12;
  const char* p = name;
  while (*p) {
    const char* dot = strchr(p, '.');
    size_t l = dot ? (size_t)(dot - p) : strlen(p);
    out[n++] = (uint8_t)l;
    memcpy(out + n, p, l);
    n += l;
    p += l;
    if (*p == '.') p++;
  }
  out[n++] = 0;
  out[n++] = (uint8_t)(qtype >> 8);
  out[n++] = (uint8_t)qtype;
  out[n++] = 0;
  out[n++] = 1;  // class IN
  if (edns) {
    const uint8_t opt[11] = {0, 0, 41, 0x10, 0, 0, 0, 0, 0, 0, 0};
    memcpy(out + n, opt, sizeof opt);
    n += sizeof opt;
  }
  return n;
}

static void testDnsWire() {
  uint8_t q[256], a[600];
  const uint8_t ip[4] = {192, 168, 4, 1};
  DnsQuestion dq;

  // A query (with EDNS, as Windows sends it) -> one A record with the AP address
  size_t n = dnsQuery(q, "rtupdate.wunderground.com", 1, true);
  CHECK(dnsParseQuery(q, n, &dq));
  CHECK(strcmp(dq.name, "rtupdate.wunderground.com") == 0);
  CHECK(dq.qtype == 1 && dq.qclass == 1 && dq.id == 0xBEEF && dq.rd);
  CHECK(dq.qend == n - 11);
  size_t m = dnsBuildAnswer(q, dq, ip, a, sizeof a);
  CHECK(m == dq.qend + 16);
  CHECK(a[0] == 0xBE && a[1] == 0xEF);                      // id echoed
  CHECK(a[2] == 0x85 && a[3] == 0x80);                      // QR AA RD / RA NOERROR
  CHECK(a[5] == 1 && a[7] == 1 && a[9] == 0 && a[11] == 0); // 1 question, 1 answer, EDNS dropped
  CHECK(a[dq.qend] == 0xC0 && a[dq.qend + 1] == 0x0C);      // name pointer to the question
  CHECK(a[dq.qend + 3] == 1 && a[dq.qend + 9] == 60);       // type A, TTL 60
  CHECK(memcmp(a + m - 4, ip, 4) == 0);

  // AAAA -> empty NOERROR answer
  n = dnsQuery(q, "time.windows.com", 28, false);
  CHECK(dnsParseQuery(q, n, &dq) && dq.qtype == 28);
  m = dnsBuildAnswer(q, dq, ip, a, sizeof a);
  CHECK(m == n && a[7] == 0 && a[3] == 0x80);

  // compression pointer inside a question is rejected
  n = dnsQuery(q, "example.com", 1, false);
  q[12] = 0xC0;
  CHECK(!dnsParseQuery(q, n, &dq));

  // truncated packets are rejected: inside a label, inside type/class, inside the header
  n = dnsQuery(q, "example.com", 1, false);
  CHECK(!dnsParseQuery(q, 16, &dq));
  CHECK(!dnsParseQuery(q, n - 2, &dq));
  CHECK(!dnsParseQuery(q, 11, &dq));

  // a response, a non-QUERY opcode and two questions are not answered
  n = dnsQuery(q, "example.com", 1, false);
  q[2] |= 0x80;
  CHECK(!dnsParseQuery(q, n, &dq));
  n = dnsQuery(q, "example.com", 1, false);
  q[2] = 0x28;  // opcode 5 (UPDATE)
  CHECK(!dnsParseQuery(q, n, &dq));
  n = dnsQuery(q, "example.com", 1, false);
  q[5] = 2;
  CHECK(!dnsParseQuery(q, n, &dq));

  // a name longer than DNS_NAME_MAX is rejected, the answer never overruns a small buffer
  char longName[200];
  memset(longName, 0, sizeof longName);
  for (int i = 0; i < 4; i++) strcat(longName, "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa.");
  strcat(longName, "com");
  n = dnsQuery(q, longName, 1, false);
  CHECK(!dnsParseQuery(q, n, &dq));
  n = dnsQuery(q, "example.com", 1, false);
  CHECK(dnsParseQuery(q, n, &dq));
  CHECK(dnsBuildAnswer(q, dq, ip, a, dq.qend + 15) == 0);
}

static void testDnsLogLimiter() {
  static NameLogLimiter lim;
  CHECK(lim.shouldLog("rtupdate.wunderground.com", 100));
  CHECK(!lim.shouldLog("rtupdate.wunderground.com", 101));
  CHECK(!lim.shouldLog("RTUPDATE.wunderground.COM", 300));  // case-insensitive
  CHECK(lim.shouldLog("time.windows.com", 300));
  CHECK(!lim.shouldLog("rtupdate.wunderground.com", 100 + 599));
  CHECK(lim.shouldLog("rtupdate.wunderground.com", 100 + 600));
  // a flood of distinct names evicts the oldest entries but never grows
  char name[32];
  for (int i = 0; i < 100; i++) {
    snprintf(name, sizeof name, "host%d.example", i);
    CHECK(lim.shouldLog(name, 1000 + i));
  }
  CHECK(!lim.shouldLog("host99.example", 1100));
  // *first = the name was not in the table (the boot log keeps that line with the detailed log off)
  static NameLogLimiter l2;
  bool first = false;
  CHECK(l2.shouldLog("a.example", 10, &first) && first);
  CHECK(!l2.shouldLog("a.example", 20, &first) && !first);
  CHECK(l2.shouldLog("a.example", 10 + 600, &first) && !first);  // a repeat after the window: logged, not first
  CHECK(l2.shouldLog("b.example", 700, &first) && first);
}

static void testNtpWire() {
  uint8_t req[48] = {0}, rsp[48];
  req[0] = (0 << 6) | (3 << 3) | 3;  // LI 0, VN 3, client
  req[2] = 6;                        // poll
  for (int i = 0; i < 8; i++) req[40 + i] = (uint8_t)(0xA0 + i);  // client's transmit timestamp
  NtpRequest r;
  CHECK(ntpParseRequest(req, sizeof req, &r) && r.vn == 3 && r.mode == 3);
  const uint32_t unix_s = (uint32_t)unixFromUtc(2026, 11, 1, 12, 0, 0);
  ntpBuildReply(req, r, unix_s, 500000, rsp);
  CHECK((rsp[0] >> 6) == 0 && ((rsp[0] >> 3) & 7) == 3 && (rsp[0] & 7) == 4);  // LI 0, VN echoed, server
  CHECK(rsp[1] == 2 && rsp[2] == 6);
  CHECK(memcmp(rsp + 12, "LOCL", 4) == 0);
  CHECK(memcmp(rsp + 24, req + 40, 8) == 0);  // originate = client's transmit
  const uint32_t secs = (uint32_t)rsp[40] << 24 | (uint32_t)rsp[41] << 16 | (uint32_t)rsp[42] << 8 | rsp[43];
  CHECK(secs == unix_s + NTP_UNIX_OFFSET);
  CHECK(rsp[44] == 0x80 && rsp[45] == 0x00);  // 0.5 s fraction
  CHECK(memcmp(rsp + 32, rsp + 40, 8) == 0);  // receive == transmit

  req[0] = (4 << 3) | 1;  // symmetric active (seen from Windows w32tm) is answered too
  CHECK(ntpParseRequest(req, sizeof req, &r) && r.mode == 1);
  req[0] = 0;  // VN 0 -> reply with VN 4
  ntpParseRequest(req, sizeof req, &r);
  r.mode = 3;
  ntpBuildReply(req, r, unix_s, 0, rsp);
  CHECK(((rsp[0] >> 3) & 7) == 4);

  req[0] = (4 << 3) | 4;  // a server reply is never answered (no ping-pong between responders)
  CHECK(!ntpParseRequest(req, sizeof req, &r));
  req[0] = (4 << 3) | 3;
  CHECK(!ntpParseRequest(req, 47, &r));  // short packet
}

// ---------------------------------------------------------------- JSON, config, filters, TZ, daily

static std::string render(void (*fn)(Out&, const void*), const void* arg) {
  std::string s;
  char buf[96];
  Out o(toString, &s, buf, sizeof buf);
  fn(o, arg);
  o.flush();
  return s;
}
static void toJsonFn(Out& o, const void* c) { stationToJson(o, *(const StationConfig*)c); }
static void effJsonFn(Out& o, const void* c) { stationEffectiveJson(o, *(const StationConfig*)c); }
static void bridgeJsonFn(Out& o, const void* c) { bridgeToJson(o, *(const BridgeConfig*)c); }
static void bridgeEffFn(Out& o, const void* c) { bridgeEffectiveJson(o, *(const BridgeConfig*)c); }

static void testJsonReader() {
  // escapes, \uXXXX incl. a surrogate pair, nested skip
  const char* doc = "{\"a\":\"x\\\"y\\\\z\\n\\u0142\\ud83d\\ude00\",\"skip\":{\"q\":[1,{\"r\":null},true]},\"b\":-2.5e1,\"c\":false}";
  JsonReader r(doc, strlen(doc));
  char k[16], v[32];
  CHECK(r.beginObject());
  CHECK(r.nextKey(k, sizeof k) && strcmp(k, "a") == 0);
  CHECK(r.readString(v, sizeof v));
  CHECK(strcmp(v, "x\"y\\z\n\xc5\x82\xf0\x9f\x98\x80") == 0);
  CHECK(r.nextKey(k, sizeof k) && strcmp(k, "skip") == 0);
  CHECK(r.skipValue());
  double d;
  bool b;
  CHECK(r.nextKey(k, sizeof k) && strcmp(k, "b") == 0 && r.readNumber(&d) && d == -25.0);
  CHECK(r.nextKey(k, sizeof k) && strcmp(k, "c") == 0 && r.readBool(&b) && !b);
  CHECK(!r.nextKey(k, sizeof k) && !r.failed() && r.atEnd());

  // truncation never splits a UTF-8 sequence: three 2-byte letters are 6 bytes, a 4-byte buffer holds 1 char + cut
  JsonReader r2("\"\xc3\xb3\xc5\x82\xc5\xbc\"", 8);
  char small[4];
  bool trunc = false;
  CHECK(r2.readString(small, sizeof small, &trunc) && trunc);
  CHECK(strcmp(small, "\xc3\xb3") == 0);

  // malformed: unterminated string, lone low surrogate, deep nesting
  JsonReader r3("{\"a\":\"abc", 9);
  CHECK(r3.beginObject() && r3.nextKey(k, sizeof k) && !r3.readString(v, sizeof v) && r3.failed());
  JsonReader r4("\"\\udc00\"", 8);
  CHECK(!r4.readString(v, sizeof v));
  std::string deep(40, '[');
  JsonReader r5(deep.c_str(), deep.size());
  CHECK(!r5.skipValue());

  JsonReader r6("\xEF\xBB\xBF{\"a\":1}", 10);  // UTF-8 BOM
  CHECK(r6.beginObject() && r6.nextKey(k, sizeof k) && strcmp(k, "a") == 0);

  char q[64];
  CHECK(jsonQuote(q, sizeof q, "Jard\xc3\xad" "n \"Pe\xc3\xb1\xc3\xb3n\"\\\x01") == strlen(q));
  CHECK(strcmp(q, "\"Jard\xc3\xad" "n \\\"Pe\xc3\xb1\xc3\xb3n\\\"\\\\\\u0001\"") == 0);
}

static void testConfigJson() {
  // Merge: defaults <- profile <- overrides; Polish name with quotes survives a round trip.
  const char* ov =
      "{\"name\":\"Jard\xc3\xad" "n \\\"Pe\xc3\xb1\xc3\xb3n\\\"\",\"profile\":\"generic\",\"publish\":false,"
      "\"limits\":{\"wind_kmh_max\":150,\"temp_c_min\":-40},\"jumps\":{\"temp_c_per_min\":5},"
      "\"calib\":{\"temp_c_offset\":-0.5,\"wind_dir_offset\":-5,\"rain_factor\":1.1},\"disabled\":[\"uv\"],"
      "\"promote\":{\"soilmoisture\":{\"as\":\"soil_moisture\",\"unit\":\"%\",\"conv\":\"none\","
      "\"device_class\":\"moisture\",\"state_class\":\"measurement\"}}}";
  Errors e;
  StationConfig c = mergeStation(ov, &e);
  CHECK(e.n == 0);
  CHECK(strcmp(c.name, "Jard\xc3\xad" "n \"Pe\xc3\xb1\xc3\xb3n\"") == 0);
  CHECK(c.profile == Profile::Generic && !c.publish);
  CHECK(c.lim_max[F_WIND_KMH] == 150 && c.lim_min[F_TEMP_C] == -40 && std::isnan(c.lim_max[F_TEMP_C]));
  CHECK(c.jump_per_min[F_TEMP_C] == 5);
  CHECK(c.temp_c_offset == -0.5f && c.wind_dir_offset == -5 && std::fabs(c.rain_factor - 1.1f) < 1e-6);
  CHECK(c.disabled == (1u << F_UV));
  CHECK(c.promote[0].used && strcmp(c.promote[0].param, "soilmoisture") == 0 &&
        strcmp(c.promote[0].key, "soil_moisture") == 0 && strcmp(c.promote[0].device_class, "moisture") == 0);
  CHECK(stationConfigured(c));

  const std::string out = render(toJsonFn, &c);
  StationConfig c2 = mergeStation(out.c_str(), &e);
  CHECK(e.n == 0);
  CHECK(render(toJsonFn, &c2) == out);  // stable round trip
  CHECK(strcmp(c2.name, c.name) == 0 && c2.promote[0].used && c2.disabled == c.disabled);
  CHECK(out.size() < 4000);            // NVS value limit, with headroom
  CHECK(contains(out, "\"name\":\"Jard\xc3\xad" "n \\\"Pe\xc3\xb1\xc3\xb3n\\\"\""));

  // defaults write "{}", and the effective view has every key
  StationConfig d = defaultStationConfig();
  CHECK(render(toJsonFn, &d) == "{}");
  CHECK(!stationConfigured(d));
  const std::string eff = render(effJsonFn, &d);
  CHECK(contains(eff, "\"profile\":\"yt60234\"") && contains(eff, "\"publish\":true") && contains(eff, "\"calib\":{"));

  // a partial PUT on top keeps the rest; null resets a key to the default
  StationConfig p = c;
  const char* patch = "{\"calib\":{\"temp_c_offset\":null},\"stale_s\":600,\"name\":null}";
  CHECK(stationFromJson(patch, strlen(patch), &p, &e) && e.n == 0);
  CHECK(p.temp_c_offset == 0 && p.wind_dir_offset == -5 && p.stale_s == 600 && p.name[0] == 0);
}

// Worst case for NVS (value <= 4000 B, sketch buffer 3800 B): every key overridden, longest strings.
static void testConfigWorstCaseSize() {
  StationConfig c = defaultStationConfig();
  memset(c.name, 0, sizeof c.name);
  for (size_t i = 0; i < NAME_MAX_BYTES; i++) c.name[i] = (i % 2) ? '"' : '\\';  // every byte escaped
  c.profile = Profile::Generic;
  c.publish = false;
  c.stale_s = 86400;
  for (int f = 0; f < CORE_FIELDS; f++) {
    c.lim_min[f] = -12345.678f;
    c.lim_max[f] = 12345.678f;
    c.jump_per_min[f] = 1234.567f;
  }
  c.temp_c_offset = -19.99f;
  c.humidity_offset = -49.99f;
  c.pressure_hpa_offset = -99.99f;
  c.wind_dir_offset = -359.9f;
  c.rain_factor = 9.999f;
  c.wind_factor = 9.999f;
  c.disabled = (1u << F_COUNT) - 1;
  for (int i = 0; i < MAX_PROMOTE; i++) {
    Promote& p = c.promote[i];
    p.used = true;
    snprintf(p.param, sizeof p.param, "param%010d", i);
    snprintf(p.key, sizeof p.key, "promoted_key_%010d", i);
    memset(p.name, '"', PROMO_NAME_MAX_BYTES);  // every byte escaped: the longest JSON a name can produce
    p.name[PROMO_NAME_MAX_BYTES] = 0;
    p.type = PromoType::SoilTemperature;
    strcpy(p.unit, "abcdefghijk");
    p.conv = PromoConv::Scale;
    p.scale = -1234.5678f;
    strcpy(p.device_class, "atmospheric_pressure_x");
    strcpy(p.state_class, "total_increasing_xx");
  }
  const std::string out = render(toJsonFn, &c);
  printf("  info: worst-case station overrides JSON = %u bytes\n", (unsigned)out.size());
  CHECK(out.size() + 16 + 1 < 3800);  // "<ID>\n" prefix + terminator fit the sketch's NVS buffer
  Errors e;
  StationConfig back = mergeStation(out.c_str(), &e);
  const std::string again = render(toJsonFn, &back);
  if (e.n || again != out) printf("  out=%s\n  back=%s (errors %d: %s)\n", out.c_str(), again.c_str(), e.n, e.n ? e.at(0) : "");
  CHECK(e.n == 0 && again == out);
}

static void testNameUtf8() {
  StationConfig c = defaultStationConfig();
  Errors e;
  // cp1250 bytes as a Windows shell may send them: 0xF3 (o-acute), 0xB3 (l-stroke) -- not valid UTF-8
  const char* bad = "{\"name\":\"Jard\xed" "n Pe\xf1\xf3n\"}";
  CHECK(stationFromJson(bad, strlen(bad), &c, &e));
  CHECK(strcmp(c.name, "Jard?n Pe??n") == 0 && e.n == 1);
  Errors e2;
  const char* good = "{\"name\":\"Jard\xc3\xad" "n Pe\xc3\xb1\xc3\xb3n \xe2\x98\x80\xf0\x9f\x8c\xa7\"}";
  CHECK(stationFromJson(good, strlen(good), &c, &e2) && e2.n == 0);
  CHECK(strcmp(c.name, "Jard\xc3\xad" "n Pe\xc3\xb1\xc3\xb3n \xe2\x98\x80\xf0\x9f\x8c\xa7") == 0);
  Errors e3;
  const char* trunc3 = "{\"name\":\"\xe2\x98\"}";  // a cut 3-byte sequence
  CHECK(stationFromJson(trunc3, strlen(trunc3), &c, &e3) && strcmp(c.name, "??") == 0);
}

static void testConfigTolerance() {
  StationConfig c = defaultStationConfig();
  Errors e;
  const char* bad =
      "{\"bogus\":1,\"publish\":\"yes\",\"stale_s\":5,\"limits\":{\"temp_c_min\":\"x\",\"wind_kmh_max\":100,"
      "\"nope_max\":1},\"calib\":{\"temp_c_offset\":99},\"disabled\":[\"uv\",\"nope\"],\"profile\":\"vevor\","
      "\"promote\":{\"tempf\":{\"as\":\"t\"},\"x1\":{\"as\":\"temp_c\"},\"x2\":{\"as\":\"Bad Key\"}},"
      "\"name\":\"" "0123456789012345678901234567890123456789012345678901234567890" "\"}";
  CHECK(stationFromJson(bad, strlen(bad), &c, &e));
  CHECK(e.n == Errors::MAX && e.dropped > 0);  // >= 10 problems reported, the rest counted
  CHECK(c.publish && c.stale_s == 240);        // bad values keep the defaults
  CHECK(c.lim_max[F_WIND_KMH] == 100);         // good values in the same object still apply
  CHECK(std::isnan(c.lim_min[F_TEMP_C]));
  CHECK(c.temp_c_offset == 0);
  CHECK(c.disabled == (1u << F_UV));
  CHECK(c.profile == Profile::Yt60234);
  CHECK(!c.promote[0].used);                   // known param, reserved key and bad key all rejected
  CHECK(strlen(c.name) == NAME_MAX_BYTES);     // truncated, still applied

  // min > max clears both; not an object at all is refused as a whole
  StationConfig m = defaultStationConfig();
  Errors e2;
  const char* mm = "{\"limits\":{\"temp_c_min\":10,\"temp_c_max\":5}}";
  stationFromJson(mm, strlen(mm), &m, &e2);
  CHECK(e2.n == 1 && std::isnan(m.lim_min[F_TEMP_C]) && std::isnan(m.lim_max[F_TEMP_C]));
  Errors e3;
  CHECK(!stationFromJson("[1,2]", 5, &m, &e3) && e3.n == 1);
  // a syntax error keeps what was read before it
  StationConfig s = defaultStationConfig();
  Errors e4;
  const char* cut = "{\"stale_s\":300,\"name\":\"abc";
  CHECK(stationFromJson(cut, strlen(cut), &s, &e4));
  CHECK(s.stale_s == 300 && s.name[0] == 0 && e4.n >= 1);

  // bridge config
  BridgeConfig b = defaultBridgeConfig();
  CHECK(render(bridgeJsonFn, &b) == "{}");
  Errors e5;
  const char* bj = "{\"tz\":\"EST5EDT,M3.2.0,M11.1.0\",\"auto_publish_new_stations\":false,\"mqtt_interval_last_seen_s\":5}";
  CHECK(bridgeFromJson(bj, strlen(bj), &b, &e5));
  CHECK(strcmp(b.tz, "EST5EDT,M3.2.0,M11.1.0") == 0 && !b.auto_publish_new_stations);
  CHECK(b.mqtt_interval_last_seen_s == 300 && e5.n == 1);
  Errors e6;
  const char* badtz = "{\"tz\":\"Europe/London\"}";
  CHECK(bridgeFromJson(badtz, strlen(badtz), &b, &e6) && e6.n == 1 && strcmp(b.tz, "EST5EDT,M3.2.0,M11.1.0") == 0);
}

static Sample sampleFrom(const char* q) {
  Sample s;
  parseQuery(q, &s);
  return s;
}

static void testFilterRules() {
  static FilterState st;
  static Filtered fo;
  StationConfig yt = defaultStationConfig();  // profile yt60234

  // profile_wind: 130 mph (209 km/h) -> wind, gust, direction dropped; rest kept
  memset(&st, 0, sizeof st);
  runFilters(sampleFrom("windspeedmph=130&windgustmph=5&winddir=180&humidity=60&tempf=50"), yt, 1000, &st, &fo);
  CHECK(fo.fired(R_PROFILE_WIND) && !fo.sample_dropped);
  CHECK(!fo.s.has(F_WIND_KMH) && !fo.s.has(F_GUST_KMH) && !fo.s.has(F_WIND_DIR));
  CHECK(fo.s.has(F_HUMIDITY) && fo.s.has(F_TEMP_C));
  runFilters(sampleFrom("windspeedmph=100&windgustmph=120&winddir=180"), yt, 1060, &st, &fo);  // 193 km/h passes
  CHECK(!fo.fired(R_PROFILE_WIND) && fo.s.has(F_WIND_KMH) && fo.s.has(F_GUST_KMH));
  // the same sample under the generic profile keeps the wind
  StationConfig gen = profileDefaults(Profile::Generic);
  memset(&st, 0, sizeof st);
  runFilters(sampleFrom("windspeedmph=130&winddir=180"), gen, 1000, &st, &fo);
  CHECK(!fo.fired(R_PROFILE_WIND) && fo.s.has(F_WIND_KMH));

  // profile_humidity: < 3 % drops the whole sample, >= 3 % passes
  memset(&st, 0, sizeof st);
  runFilters(sampleFrom("humidity=2&tempf=50&windspeedmph=3"), yt, 1000, &st, &fo);
  CHECK(fo.sample_dropped && fo.fired(R_PROFILE_HUMIDITY) && fo.s.present == 0);
  CHECK(st.have == 0);  // a dropped sample never becomes the "last accepted" reference
  runFilters(sampleFrom("humidity=3&tempf=50"), yt, 1000, &st, &fo);
  CHECK(!fo.sample_dropped && fo.s.has(F_HUMIDITY));

  // parse / bounds come from the parser and count as rules too
  runFilters(sampleFrom("solarRadiation=-117.-5&humidity=120"), yt, 1000, &st, &fo);
  CHECK(fo.fired(R_PARSE) && fo.fired(R_BOUNDS) && fo.rejectedFields() == ((1u << F_SOLAR_WM2) | (1u << F_HUMIDITY)));

  // limits: metric, before calibration
  StationConfig lim = defaultStationConfig();
  lim.lim_max[F_TEMP_C] = 30;
  lim.lim_min[F_PRESSURE_HPA] = 900;
  lim.temp_c_offset = 5;  // 28 + 5 = 33 > 30 would fail only if the limit ran after calibration
  memset(&st, 0, sizeof st);
  runFilters(sampleFrom("tempf=82.4&baromin=29.92"), lim, 1000, &st, &fo);  // 28 C, 1013 hPa
  CHECK(!fo.fired(R_LIMITS) && fo.s.has(F_TEMP_C));
  CHECK_NEAR(fo.s.v[F_TEMP_C], 33.0, 0.01);
  runFilters(sampleFrom("tempf=95&baromin=26"), lim, 1060, &st, &fo);  // 35 C, 880 hPa
  CHECK(fo.fired(R_LIMITS) && !fo.s.has(F_TEMP_C) && !fo.s.has(F_PRESSURE_HPA));

  // hidden (disabled): kept in the sample for the history, not counted as rejected
  StationConfig dis = defaultStationConfig();
  dis.disabled = 1u << F_UV;
  runFilters(sampleFrom("UV=3&tempf=50"), dis, 1000, &st, &fo);
  CHECK(fo.s.has(F_UV) && fo.rejectedFields() == 0);

  // promotions: an extra becomes a converted value
  StationConfig pro = defaultStationConfig();
  pro.promote[0].used = true;
  strcpy(pro.promote[0].param, "soiltempf");
  strcpy(pro.promote[0].key, "soil_temp_c");
  pro.promote[0].conv = PromoConv::FtoC;
  runFilters(sampleFrom("soiltempf=50&other=1"), pro, 1000, &st, &fo);
  CHECK(fo.promoted_mask == 1);
  CHECK_NEAR(fo.promoted[0], 10.0, 0.01);
  runFilters(sampleFrom("soiltempf=abc"), pro, 1000, &st, &fo);
  CHECK(fo.promoted_mask == 0);
  // a value beyond a float (or past +-1e9) is not promoted
  runFilters(sampleFrom("soiltempf=1e300"), pro, 1000, &st, &fo);
  CHECK(fo.promoted_mask == 0);
  runFilters(sampleFrom("soiltempf=-1e12"), pro, 1000, &st, &fo);
  CHECK(fo.promoted_mask == 0 && promotedOk(1e9) && !promotedOk(1.0000001e9) && !promotedOk(NAN));
}

static void testFilterJump() {
  static FilterState st;
  static Filtered fo;
  StationConfig c = defaultStationConfig();
  c.jump_per_min[F_TEMP_C] = 2.0f;  // 2 C per minute
  memset(&st, 0, sizeof st);
  runFilters(sampleFrom("tempf=50"), c, 1000, &st, &fo);  // 10 C, first sample always passes
  CHECK(fo.s.has(F_TEMP_C));
  runFilters(sampleFrom("tempf=51.7"), c, 1030, &st, &fo);  // +0.94 C in 30 s: allowed 1.0 -> passes
  CHECK(fo.s.has(F_TEMP_C) && !fo.fired(R_JUMP));
  runFilters(sampleFrom("tempf=53.96"), c, 1060, &st, &fo);  // +1.26 C in 30 s: allowed 1.0 -> jump
  CHECK(!fo.s.has(F_TEMP_C) && fo.fired(R_JUMP));
  // the reference stays at the last ACCEPTED value (10.94 C at t=1030); 2 min later the same value passes
  runFilters(sampleFrom("tempf=53.96"), c, 1150, &st, &fo);  // +1.26 C in 120 s: allowed 4.0
  CHECK(fo.s.has(F_TEMP_C) && !fo.fired(R_JUMP));
  // very short intervals use the 10 s floor: +0.3 C in 1 s is allowed (2 * 1/6 = 0.33)
  runFilters(sampleFrom("tempf=54.5"), c, 1151, &st, &fo);
  CHECK(fo.s.has(F_TEMP_C));
}

static void testFilterCalibration() {
  static FilterState st;
  static Filtered fo;
  StationConfig c = defaultStationConfig();
  c.wind_dir_offset = -5;
  c.temp_c_offset = -0.5f;
  c.humidity_offset = 50;
  c.wind_factor = 1.5f;
  memset(&st, 0, sizeof st);
  runFilters(sampleFrom("winddir=2&tempf=50&humidity=60&windspeedmph=10"), c, 1000, &st, &fo);
  CHECK_NEAR(fo.s.v[F_WIND_DIR], 357.0, 0.001);  // -5 deg on 2 deg -> 357
  CHECK_NEAR(fo.s.v[F_TEMP_C], 9.5, 0.001);
  CHECK_NEAR(fo.s.v[F_HUMIDITY], 100.0, 0.001);  // clamped
  CHECK_NEAR(fo.s.v[F_WIND_KMH], 24.14, 0.01);
  // the raw console direction is wrapped for every profile: 360 -> 0, -10 -> 350
  StationConfig g = profileDefaults(Profile::Generic);
  runFilters(sampleFrom("winddir=360"), g, 1000, &st, &fo);
  CHECK_NEAR(fo.s.v[F_WIND_DIR], 0.0, 0.001);
  runFilters(sampleFrom("winddir=-10"), g, 1000, &st, &fo);
  CHECK_NEAR(fo.s.v[F_WIND_DIR], 350.0, 0.001);
}

static void testRainRule() {
  static FilterState st;
  static Filtered fo;
  StationConfig c = defaultStationConfig();  // yt60234: monotonic rule
  memset(&st, 0, sizeof st);
  runFilters(sampleFrom("dailyrainin=0.10"), c, 1000, &st, &fo);  // 2.54 mm, first value: no delta
  CHECK(!fo.rain_delta_valid && !fo.rain_glitch);
  runFilters(sampleFrom("dailyrainin=0.12"), c, 1060, &st, &fo);  // +0.508 mm
  CHECK(fo.rain_delta_valid);
  CHECK_NEAR(fo.rain_delta_mm, 0.508, 0.001);
  runFilters(sampleFrom("dailyrainin=0.05"), c, 1120, &st, &fo);  // drop to 1.27 mm > 0.5: glitch
  CHECK(fo.rain_glitch && fo.fired(R_RAIN_GLITCH) && !fo.rain_delta_valid);
  CHECK(fo.s.has(F_RAIN_DAILY_MM));  // the counter value itself is still published
  runFilters(sampleFrom("dailyrainin=0.13"), c, 1180, &st, &fo);  // back above: delta vs the last good 3.048
  CHECK(fo.rain_delta_valid);
  CHECK_NEAR(fo.rain_delta_mm, 0.254, 0.001);
  runFilters(sampleFrom("dailyrainin=0.01"), c, 1240, &st, &fo);  // 0.254 mm <= 0.5: midnight reset
  CHECK(fo.rain_delta_valid && !fo.rain_glitch);
  CHECK_NEAR(fo.rain_delta_mm, 0.254, 0.001);
  // generic profile: any drop is a reset
  StationConfig g = profileDefaults(Profile::Generic);
  memset(&st, 0, sizeof st);
  runFilters(sampleFrom("dailyrainin=0.10"), g, 1000, &st, &fo);
  runFilters(sampleFrom("dailyrainin=0.05"), g, 1060, &st, &fo);
  CHECK(!fo.rain_glitch && fo.rain_delta_valid);
  CHECK_NEAR(fo.rain_delta_mm, 1.27, 0.001);

  // a console reset that already shows 2 mm: three glitches, then the counter is the new baseline
  memset(&st, 0, sizeof st);
  runFilters(sampleFrom("dailyrainin=0.60"), c, 1000, &st, &fo);  // 15.24 mm before the console's midnight
  runFilters(sampleFrom("dailyrainin=0.08"), c, 1060, &st, &fo);  // 2.03 mm after it: > 0.5, a glitch
  CHECK(fo.rain_glitch && !fo.rain_delta_valid);
  runFilters(sampleFrom("dailyrainin=0.08"), c, 1120, &st, &fo);
  CHECK(fo.rain_glitch);
  runFilters(sampleFrom("dailyrainin=0.09"), c, 1180, &st, &fo);  // third in a row: re-based on 2.29 mm
  CHECK(fo.rain_glitch && !fo.rain_delta_valid);
  CHECK_NEAR(st.last_daily, 2.286, 0.001);
  runFilters(sampleFrom("dailyrainin=0.10"), c, 1240, &st, &fo);  // rain counts again from there
  CHECK(fo.rain_delta_valid && !fo.rain_glitch);
  CHECK_NEAR(fo.rain_delta_mm, 0.254, 0.001);
  // an upward spike, then normal values: the spike is a glitch, the baseline stays
  memset(&st, 0, sizeof st);
  runFilters(sampleFrom("dailyrainin=0.10"), c, 1000, &st, &fo);
  runFilters(sampleFrom("dailyrainin=70"), c, 1060, &st, &fo);    // +1 775 mm in a minute (inside the 2 000 mm bound)
  CHECK(fo.rain_glitch && !fo.rain_delta_valid && fo.s.has(F_RAIN_DAILY_MM));
  runFilters(sampleFrom("dailyrainin=0.11"), c, 1120, &st, &fo);
  CHECK(fo.rain_delta_valid && !fo.rain_glitch);
  CHECK_NEAR(fo.rain_delta_mm, 0.254, 0.001);
  CHECK(st.rain_glitches == 0);
  // a single glitch between good values never re-bases
  runFilters(sampleFrom("dailyrainin=0.02"), c, 1180, &st, &fo);
  runFilters(sampleFrom("dailyrainin=0.12"), c, 1240, &st, &fo);
  CHECK(fo.rain_delta_valid);
  CHECK_NEAR(fo.rain_delta_mm, 0.254, 0.001);
  static_assert(sizeof(FilterState) == CORE_FIELDS * 8 + 4 + 4 + 4, "snapshot layout: rain_glitches in the padding");
}

static void testPosixTz() {
  PosixTz tz;
  CHECK(parsePosixTz(DEFAULT_TZ, &tz));
  CHECK(tz.std_off == 3600 && tz.dst_off == 7200 && tz.has_dst);
  // 2026: DST from Sun 29 March 01:00 UTC to Sun 25 October 01:00 UTC
  CHECK(localOffset(tz, unixFromUtc(2026, 3, 29, 0, 59, 59)) == 3600);
  CHECK(localOffset(tz, unixFromUtc(2026, 3, 29, 1, 0, 0)) == 7200);
  CHECK(localOffset(tz, unixFromUtc(2026, 10, 25, 0, 59, 59)) == 7200);
  CHECK(localOffset(tz, unixFromUtc(2026, 10, 25, 1, 0, 0)) == 3600);
  CHECK(localOffset(tz, unixFromUtc(2026, 7, 1, 12, 0, 0)) == 7200);
  CHECK(localOffset(tz, unixFromUtc(2026, 12, 31, 23, 30, 0)) == 3600);
  // local day: 23:30 UTC on 31 Dec is already 1 Jan in CET
  CHECK(dayNumberToYmd(localDayNumber(tz, unixFromUtc(2026, 12, 31, 23, 30, 0))) == 20270101);
  CHECK(dayNumberToYmd(localDayNumber(tz, unixFromUtc(2026, 11, 1, 12, 0, 0))) == 20261101);
  // other zones: US east coast, a southern-hemisphere rule, UTC, bracketed names
  PosixTz us, au, utc, br;
  CHECK(parsePosixTz("EST5EDT,M3.2.0,M11.1.0", &us));
  CHECK(localOffset(us, unixFromUtc(2026, 7, 1, 12, 0, 0)) == -4 * 3600);
  CHECK(localOffset(us, unixFromUtc(2026, 1, 1, 12, 0, 0)) == -5 * 3600);
  CHECK(parsePosixTz("AEST-10AEDT,M10.1.0,M4.1.0/3", &au));
  CHECK(localOffset(au, unixFromUtc(2026, 1, 1, 0, 0, 0)) == 11 * 3600);
  CHECK(localOffset(au, unixFromUtc(2026, 7, 1, 0, 0, 0)) == 10 * 3600);
  CHECK(parsePosixTz("UTC0", &utc) && localOffset(utc, 1800000000) == 0);
  CHECK(parsePosixTz("<+0530>-5:30", &br) && localOffset(br, 1800000000) == 5 * 3600 + 1800);
  // unsupported or malformed
  CHECK(!parsePosixTz("Europe/London", &tz) && !parsePosixTz("CET-1CEST", &tz) && !parsePosixTz("CET-1CEST,J60,J300", &tz));
}

static void testStationIngestProfile() {
  static Stations all;
  all.reserve(MAX_STATIONS);
  PosixTz tz;
  parsePosixTz(DEFAULT_TZ, &tz);
  StationConfig cfg = defaultStationConfig();
  cfg.publish = false;  // e.g. auto_publish_new_stations = false at registration
  Station* st = all.findOrAdd("ST9", cfg);
  CHECK(st && !st->cfg.publish && all.findOrAdd("ST9", defaultStationConfig()) == st);
  char name[64];
  CHECK(strcmp(st->displayName(name, sizeof name), "fauxcloud ST9") == 0);
  const uint32_t t = (uint32_t)unixFromUtc(2026, 11, 1, 12, 0, 0);
  // wind 130 mph, then humidity 2 %
  IngestResult r1 = Stations::ingest(*st, sampleFrom("ID=ST9&windspeedmph=130&humidity=60&tempf=50"), t, 1000, 0, SrcIf::Sta, true, tz);
  CHECK(!r1.dropped && st->rej_rule[R_PROFILE_WIND] == 1 && !st->live.has(F_WIND_KMH) && st->live.has(F_TEMP_C));
  IngestResult r2 = Stations::ingest(*st, sampleFrom("ID=ST9&humidity=2&tempf=99"), t + 30, 31000, 0, SrcIf::Sta, true, tz);
  CHECK(r2.dropped && st->rej_rule[R_PROFILE_HUMIDITY] == 1 && st->uploads == 2 && st->dropped == 1);
  CHECK_NEAR(st->live.v[F_TEMP_C], 10.0, 0.01);  // live stays at the last accepted sample
  CHECK(st->recent.size() == 1);
  HourRec open;
  CHECK(st->hourly.openRecord(&open) && open.n == 1 && (open.quality >> Q_REJECTED & 1u));  // wind, then a dropped sample
  // closing hour and day by the test hooks gives one daily record
  CHECK(Stations::closeDayNow(*st, tz));
  CHECK(st->daily.size() == 1 && st->daily.at(0).ymd == 20261101 && st->daily.at(0).n == 1);
  // name shows up escaped in discovery
  strcpy(st->cfg.name, "Jard\xc3\xad" "n \"Pe\xc3\xb1\xc3\xb3n\"");
  const uint8_t mac[6] = {1, 2, 3, 4, 5, 6};
  char payload[1024];
  CHECK(discoveryPayload(payload, sizeof payload, st->id, st->displayName(name, sizeof name), mac, F_TEMP_C, "b2") > 0);
  CHECK(contains(payload, "\"name\":\"Jard\xc3\xad" "n \\\"Pe\xc3\xb1\xc3\xb3n\\\"\",\"manufacturer\""));
  // rule counters as diagnostics
  char k[24], l[40];
  EntitySpec rs = ruleSpec(R_PROFILE_WIND, k, sizeof k, l, sizeof l);
  CHECK(discoveryPayloadSpec(payload, sizeof payload, st->id, name, mac, rs, "b2") > 0);
  CHECK(contains(payload, "\"name\":\"Rejected profile wind\"") && contains(payload, "\"entity_category\":\"diagnostic\"") &&
        contains(payload, "\"state_class\":\"total_increasing\"") && contains(payload, "\"state_topic\":\"fauxcloud/0506/ST9/rej_profile_wind\""));
  CHECK(!contains(payload, "unit_of_measurement"));
}

// ---------------------------------------------------------------- auth, sessions, settings, lifecycle

static void testPbkdf2() {
  // Vectors computed with Python (hashlib / hmac); the same values are checked by the JS in the pages.
  uint8_t out[32];
  char hex[65];
  CHECK(pbkdf2Sha256((const uint8_t*)"passwd", 6, (const uint8_t*)"salt", 4, 1, out));
  toHex(out, 32, hex);
  CHECK(strcmp(hex, "55ac046e56e3089fec1691c22544b605f94185216dde0465e68b9d57c20dacbc") == 0);  // RFC 7914 §11
  uint8_t salt[16];
  for (int i = 0; i < 16; i++) salt[i] = (uint8_t)i;
  const char* pl = "correct horse battery staple \xc5\xbc";  // the passphrase ending in U+017C as UTF-8 (a non-ASCII byte), bytes(range(16)), 10000
  CHECK(pbkdf2Sha256((const uint8_t*)pl, strlen(pl), salt, 16, 10000, out));
  toHex(out, 32, hex);
  CHECK(strcmp(hex, "460b333275bd03de2073208552f336d871c8cf4ea173a9b5e61c0ef17e98998f") == 0);
  std::string longKey(100, 'x');  // key longer than the HMAC block: hashed first
  uint8_t out2[32];
  CHECK(pbkdf2Sha256((const uint8_t*)longKey.data(), 100, (const uint8_t*)"salt", 4, 2, out2));
  toHex(out2, 32, hex);
  CHECK(strcmp(hex, "d43a18cd77bafc1a4b0c6025dbbf29c7e6d67acce6ad02a736d4a3003b6a3c26") == 0);
  uint8_t mac[32];
  const char* fox = "The quick brown fox jumps over the lazy dog";
  CHECK(hmacSha256((const uint8_t*)"key", 3, (const uint8_t*)fox, strlen(fox), mac));
  toHex(mac, 32, hex);
  CHECK(strcmp(hex, "f7bc83f430538424b13298e6aa6fb143ef4d59a14946175997479dbc2d1a3cd8") == 0);

  // challenge-response: proof = HMAC(H, nonce hex as ASCII); the server never sees the password
  char proof[65];
  CHECK(computeProof(out, "00112233445566778899aabbccddeeff", proof));
  CHECK(strcmp(proof, "6358e538940f03d3d087a6ab8dceb417a0e3a26746d6bd589bd457ba703926e5") == 0);
  char stored[HASH_STR_MAX];
  CHECK(formatCredential(salt, PBKDF2_ITER, out, stored, sizeof stored));
  CHECK(strncmp(stored, "pbkdf2-sha256$10000$000102030405060708090a0b0c0d0e0f$460b3332", 61) == 0);
  CHECK(verifyProof(stored, "00112233445566778899aabbccddeeff", proof));
  CHECK(!verifyProof(stored, "00112233445566778899aabbccddeef0", proof));          // other nonce
  proof[5] = proof[5] == '0' ? '1' : '0';
  CHECK(!verifyProof(stored, "00112233445566778899aabbccddeeff", proof));          // tampered proof
  CHECK(!verifyProof("", "00112233445566778899aabbccddeeff", proof));              // no credential = no login
  CHECK(!verifyProof(stored, "short", proof) && !verifyProof(stored, "00112233445566778899aabbccddeeff", "zz"));
  uint8_t s2[16], h2[32];
  uint32_t it = 0;
  CHECK(parseCredential(stored, s2, &it, h2) && it == 10000 && memcmp(h2, out, 32) == 0 && memcmp(s2, salt, 16) == 0);
  CHECK(!parseCredential("pbkdf2-sha256$5$00$00", s2, &it, h2) && !parseCredential("md5$x", s2, &it, h2));
  char viaPw[HASH_STR_MAX];
  CHECK(hashPassword(pl, salt, PBKDF2_ITER, viaPw, sizeof viaPw) && strcmp(viaPw, stored) == 0);
  CHECK(!formatCredential(salt, PBKDF2_ITER, out, stored, 40));                     // too small buffer
}

static void testSessions() {
  static SessionStore s;
  s.clear();
  uint8_t r[16], id[4] = {1, 2, 3, 4};
  char tok[5][33];
  const uint32_t ip = 0x0a00a8c0, other = 0x0b00a8c0;
  for (int i = 0; i < 5; i++) {
    memset(r, 0x10 + i, 16);
    id[3] = (uint8_t)i;
    s.create(r, id, ip, "Mozilla/5.0 test", 100 + i, tok[i]);  // 5th evicts the least recently used (the 1st)
  }
  CHECK(strlen(tok[0]) == 32 && s.count(105) == SessionStore::MAX);
  CHECK(!s.check(tok[0], ip, 106));
  char cid[9] = "";
  CHECK(s.check(tok[1], ip, 106, cid) && strcmp(cid, "01020301") == 0 && s.check(tok[4], ip, 106));
  CHECK(!s.check(tok[1], other, 107));  // bound to the client IP
  // a touched session survives; the next eviction takes the oldest untouched one
  CHECK(s.check(tok[2], ip, 200));
  memset(r, 0x77, 16);
  char t6[33];
  id[3] = 0x66;
  s.create(r, id, ip, "", 201, t6);
  CHECK(!s.check(tok[3], ip, 202) && s.check(tok[2], ip, 202) && s.check(t6, ip, 202));
  // list: id, ip, ua (<= 96), times; revoke by id; idle expiry; logout
  SessionStore::Info info[4];
  const int n = s.list(202, info, 4);
  CHECK(n == s.count(202) && info[0].ip == ip && strlen(info[0].id) == 8);
  std::string longUa(150, 'u');
  char t7[33];
  memset(r, 0x88, 16);
  id[3] = 0x77;
  s.create(r, id, ip, longUa.c_str(), 203, t7);
  CHECK(s.list(203, info, 4) == 4);
  bool seenLong = false;
  for (int i = 0; i < 4; i++) seenLong |= strlen(info[i].ua) == SessionStore::UA_MAX;
  CHECK(seenLong);
  CHECK(s.revoke(info[0].id) && !s.revoke(info[0].id) && s.count(203) == 3);
  CHECK(!s.check(tok[1], ip, 106 + SessionStore::IDLE_S));
  CHECK(s.remove(t6) && !s.check(t6, ip, 300) && !s.remove(t6));
  CHECK(!s.check("zz", ip, 300) && !s.check("00112233445566778899aabbccddeeff", ip, 300));
}

static void testLockout() {
  static Lockout l;
  l.clear();
  const uint32_t ipA = 0x0a00a8c0, ipB = 0x0b00a8c0;
  uint32_t retry = 0;
  for (int i = 1; i <= 4; i++) CHECK(!l.fail(ipA, 1000 + i));
  CHECK(!l.locked(ipA, 1005, &retry));
  CHECK(l.fail(ipA, 1005));  // 5th failure within 60 s locks
  CHECK(l.locked(ipA, 1006, &retry) && retry == 59);
  CHECK(!l.locked(ipB, 1006, &retry));  // per source IP
  CHECK(!l.locked(ipA, 1065, &retry));  // 60 s later
  // failures spread over more than the window never lock
  for (int i = 0; i < 10; i++) CHECK(!l.fail(ipB, 2000 + i * 20));
  l.success(ipA);
  for (int i = 0; i < 4; i++) l.fail(ipA, 1070);
  CHECK(!l.locked(ipA, 1070, &retry));
  for (uint32_t ip = 1; ip < 100; ip++) l.fail(ip, 3000 + ip);  // many IPs never grow the table
}

static void testAccountLock() {
  AccountLock a;
  uint32_t len = 0, until = 0;
  // 10 failures from any IPs within 10 min -> 5 min
  for (int i = 0; i < 9; i++) CHECK(!a.fail(1000 + i * 30, &len));
  CHECK(a.failures(1300) == 9 && !a.locked(1300));
  CHECK(a.fail(1300, &len) && len == 300);
  CHECK(a.locked(1301, &until) && until == 1600);
  CHECK(!a.fail(1302, &len) && a.locked(1302));  // failures during a lock do not extend it
  CHECK(!a.expiredSince(1599));
  CHECK(!a.locked(1600) && a.expiredSince(1600) && !a.expiredSince(1601));  // end reported once
  // next burst -> 10 min, then 20, 40, cap 60
  uint32_t t = 2000;
  const uint32_t want[] = {600, 1200, 2400, 3600, 3600};
  for (int burst = 0; burst < 5; burst++) {
    for (int i = 0; i < 9; i++) a.fail(t + i, &len);
    CHECK(a.fail(t + 9, &len) && len == want[burst]);
    t += 9 + len;
  }
  // failures spread over more than 10 min never lock
  AccountLock b;
  for (int i = 0; i < 30; i++) CHECK(!b.fail(10000 + i * 70, &len));
  // a success clears the counters and the doubling
  a.success();
  CHECK(!a.locked(t) && a.failures(t) == 0);
  for (int i = 0; i < 9; i++) a.fail(t + i, &len);
  CHECK(a.fail(t + 9, &len) && len == 300);
}

static void testNonces() {
  static NonceStore n;
  n = NonceStore();
  uint8_t r[16];
  char a[33], b[33];
  const uint32_t ip1 = 0x0A01A8C0u, ip2 = 0x0B01A8C0u;
  memset(r, 0xA1, 16);
  CHECK(n.issue(r, ip1, 1000, a));
  memset(r, 0xB2, 16);
  CHECK(!n.issue(r, ip1, 1500, b));  // rate limit: one per second per address
  CHECK(n.issue(r, ip1, 2000, b));   // replaces this address's previous nonce
  CHECK(!n.consume(a, ip1, 2100));   // a is gone
  CHECK(!n.consume(b, ip2, 2100));   // another address cannot use it...
  CHECK(n.consume(b, ip1, 2200) && !n.consume(b, ip1, 2300));  // ...its own can, once
  CHECK(n.issue(r, ip1, 3000, b) && !n.consume(b, ip1, 3000 + NonceStore::TTL_MS));  // expired after 60 s (and gone)
  CHECK(!n.consume("zz", ip1, 3000) && !n.consume("00112233445566778899aabbccddeeff", ip1, 3000));
  // one per address; four addresses fill the store, a fifth cannot evict anyone
  char t[6][33];
  for (int i = 0; i < 4; i++) {
    memset(r, 0x10 + i, 16);
    CHECK(n.issue(r, ip1 + (uint32_t)i, 100000, t[i]));
  }
  CHECK(n.outstanding(100500) == NonceStore::MAX);
  memset(r, 0x20, 16);
  CHECK(!n.issue(r, ip1 + 9, 100500, t[4]));  // refused, nothing evicted
  CHECK(n.consume(t[0], ip1, 100600) && n.consume(t[3], ip1 + 3, 100600));
  CHECK(n.issue(r, ip1 + 9, 100700, t[4]) && n.consume(t[4], ip1 + 9, 100800));  // a slot was free again
  // expired slots of other addresses are reused
  memset(r, 0x30, 16);
  CHECK(n.issue(r, ip1 + 20, 100000 + NonceStore::TTL_MS + 1, t[5]));
}

static void testAuthBody() {
  AuthBody b;
  const char* j = "{\"nonce\":\"00112233445566778899aabbccddeeff\",\"proof\":\"ab\",\"salt\":\"000102030405060708090a0b0c0d0e0f\","
                  "\"h\":\"460b333275bd03de2073208552f336d871c8cf4ea173a9b5e61c0ef17e98998f\",\"iters\":10000,\"x\":[1]}";
  CHECK(parseAuthBody(j, strlen(j), &b) && strcmp(b.proof, "ab") == 0 && b.iters == 10000);
  char cred[HASH_STR_MAX];
  CHECK(credentialFromBody(b, cred, sizeof cred) && strncmp(cred, "pbkdf2-sha256$10000$0001", 24) == 0);
  b.iters = 999;
  CHECK(!credentialFromBody(b, cred, sizeof cred));
  AuthBody c;
  const char* longNonce = "{\"nonce\":\"00112233445566778899aabbccddeeff00\"}";  // too long -> empty
  CHECK(parseAuthBody(longNonce, strlen(longNonce), &c) && c.nonce[0] == 0);
  CHECK(!parseAuthBody("[1]", 3, &c));
}

static void testCookies() {
  char v[40];
  CHECK(cookieValue("fcs=abc", "fcs", v, sizeof v) && strcmp(v, "abc") == 0);
  CHECK(cookieValue("a=1; fcs=00112233445566778899aabbccddeeff; b=2", "fcs", v, sizeof v) &&
        strcmp(v, "00112233445566778899aabbccddeeff") == 0);
  CHECK(!cookieValue("xfcs=1; fcsx=2", "fcs", v, sizeof v));
  CHECK(!cookieValue("", "fcs", v, sizeof v));
  CHECK(!cookieValue("fcs=0123456789", "fcs", v, 5));  // too long for the buffer
}

static void settingsJsonFn(Out& o, const void* p) { settingsToJson(o, *(const Settings*)p); }

static void testSettings() {
  Settings s = defaultSettings();
  CHECK(strcmp(s.hostname, "fauxcloud") == 0 && s.mqtt_port == 1883 && !s.setup_done && !settingsComplete(s));
  Errors e;
  SettingsChange ch;
  const char* put = "{\"sta_ssid\":\"Home\",\"sta_pass\":\"wifi-pass-123\",\"mqtt_host\":\"192.168.1.5\",\"mqtt_port\":1884,"
                    "\"mqtt_user\":\"ha\",\"mqtt_pass\":\"mq\",\"ap_pass\":\"ap-pass-123\",\"hostname\":\"weather-1\","
                    "\"admin_password\":\"x\",\"setup_done\":true}";
  CHECK(settingsFromJson(put, strlen(put), &s, &e, &ch));
  CHECK(e.n == 2);  // admin_password and setup_done are read-only here
  CHECK(ch.wifi && ch.mqtt && ch.ap && ch.hostname && !s.setup_done);
  CHECK(strcmp(s.sta_pass, "wifi-pass-123") == 0 && s.mqtt_port == 1884 && strcmp(s.hostname, "weather-1") == 0);
  strcpy(s.admin_hash, "pbkdf2-sha256$...");
  CHECK(settingsComplete(s));

  // GET masks every secret, never shows the hash
  const std::string out = render(settingsJsonFn, &s);
  CHECK(contains(out, "\"sta_pass\":\"<set>\"") && contains(out, "\"mqtt_pass\":\"<set>\"") &&
        contains(out, "\"ap_pass\":\"<set>\"") && contains(out, "\"admin_password\":\"<set>\""));
  CHECK(!contains(out, "wifi-pass-123") && !contains(out, "ap-pass-123") && !contains(out, "pbkdf2"));
  CHECK(contains(out, "\"sta_ssid\":\"Home\"") && contains(out, "\"mqtt_port\":1884"));

  // the GET document PUT back unchanged changes nothing
  Settings s2 = s;
  Errors e2;
  SettingsChange ch2;
  CHECK(settingsFromJson(out.c_str(), out.size(), &s2, &e2, &ch2));
  CHECK(!ch2.any() && strcmp(s2.sta_pass, "wifi-pass-123") == 0 && strcmp(s2.ap_pass, "ap-pass-123") == 0);

  // validation per key
  Errors e3;
  SettingsChange ch3;
  const char* bad = "{\"sta_pass\":\"short\",\"ap_pass\":\"\",\"hostname\":\"Bad_Host\",\"mqtt_port\":70000,"
                    "\"sta_ssid\":\"\",\"mqtt_host\":\"broker.lan\"}";
  CHECK(settingsFromJson(bad, strlen(bad), &s2, &e3, &ch3));
  CHECK(e3.n == 4 && ch3.mqtt && !ch3.wifi && !ch3.ap && !ch3.hostname);  // ap_pass "" = unchanged, no error
  CHECK(strcmp(s2.ap_pass, "ap-pass-123") == 0);

  // an empty secret = unchanged (never carried to a changed host, port or SSID), null clears the MQTT login / makes the
  // Wi-Fi open, a null AP pass is refused
  Settings s4 = s2;
  Errors e4;
  SettingsChange ch4;
  const char* part = "{\"mqtt_user\":null,\"mqtt_pass\":null,\"sta_pass\":\"\",\"ap_pass\":null}";
  CHECK(settingsFromJson(part, strlen(part), &s4, &e4, &ch4));
  CHECK(s4.mqtt_user[0] == 0 && s4.mqtt_pass[0] == 0 && ch4.mqtt && !ch4.wifi && !ch4.ap);
  CHECK(strcmp(s4.sta_pass, "wifi-pass-123") == 0 && strcmp(s4.ap_pass, "ap-pass-123") == 0);
  CHECK(e4.n == 1 && strncmp(e4.at(0), "ap_pass", 7) == 0);
  Errors e5;
  SettingsChange ch5;
  const char* open = "{\"sta_pass\":null,\"hostname\":null}";
  CHECK(settingsFromJson(open, strlen(open), &s4, &e5, &ch5) && s4.sta_pass[0] == 0 && ch5.wifi && e5.n == 1);
  CHECK(strcmp(s2.mqtt_host, "broker.lan") == 0 && strcmp(s2.hostname, "weather-1") == 0);
  CHECK(validWifiPass("") && !validApPass("") && validHostname("a-b1") && !validHostname("-ab") &&
        !validHostname("ab-") && !validAdminPass("1234567") && validAdminPass("12345678"));

  // shared setup password is a valid WPA2 passphrase; SSID suffix from the MAC
  CHECK(validApPass(SETUP_AP_PASS));
  const uint8_t mac[6] = {0x24, 0x6f, 0x28, 0x00, 0xa1, 0xb2};
  char m4[5];
  mac4(mac, m4);
  CHECK(strcmp(m4, "A1B2") == 0);
}

// a stored secret is bound to its endpoint: a settings PUT, or the MQTT test (same parser on a copy),
// that moves the broker or the network without its own secret in the body gets no secret, never the stored one.
static Settings stored2() {
  Settings s = defaultSettings();
  strcpy(s.sta_ssid, "Home");
  strcpy(s.sta_pass, "wifi-pass-123");
  strcpy(s.mqtt_host, "broker.lan");
  s.mqtt_port = 1883;
  strcpy(s.mqtt_user, "ha");
  strcpy(s.mqtt_pass, "mqtt-pass-9");
  return s;
}
static bool put2(Settings* s, const char* body, SettingsChange* ch, int* nerr = nullptr) {
  Errors e;
  const bool ok = settingsFromJson(body, strlen(body), s, &e, ch);
  if (nerr) *nerr = e.n;
  return ok;
}
static void testSecretBoundToEndpoint() {
  SettingsChange ch;
  int ne = 0;
  // a new host, with or without the GET's markers: the MQTT password is cleared (user kept)
  for (const char* b : {"{\"mqtt_host\":\"x.example\"}", "{\"mqtt_host\":\"x.example\",\"mqtt_pass\":\"\"}",
                        "{\"mqtt_host\":\"x.example\",\"mqtt_pass\":\"<set>\"}", "{\"mqtt_pass\":\"<set>\",\"mqtt_host\":\"x.example\"}"}) {
    Settings s = stored2();
    CHECK(put2(&s, b, &ch, &ne) && ne == 0);
    CHECK(strcmp(s.mqtt_host, "x.example") == 0 && s.mqtt_pass[0] == 0 && strcmp(s.mqtt_user, "ha") == 0);
    CHECK(ch.mqtt && ch.mqtt_pass_cleared && !ch.wifi && !ch.sta_pass_cleared);
  }
  // a new port alone moves the endpoint too
  Settings s = stored2();
  CHECK(put2(&s, "{\"mqtt_port\":8883}", &ch) && s.mqtt_port == 8883 && s.mqtt_pass[0] == 0 && ch.mqtt_pass_cleared);
  // MQTT off (host "") forgets it as well
  s = stored2();
  CHECK(put2(&s, "{\"mqtt_host\":\"\"}", &ch) && s.mqtt_host[0] == 0 && s.mqtt_pass[0] == 0 && ch.mqtt_pass_cleared);
  // a new host with its own password keeps that one, in either key order; the same password re-typed counts as sent
  s = stored2();
  CHECK(put2(&s, "{\"mqtt_pass\":\"other-pass\",\"mqtt_host\":\"x.example\"}", &ch) &&
        strcmp(s.mqtt_pass, "other-pass") == 0 && !ch.mqtt_pass_cleared);
  s = stored2();
  CHECK(put2(&s, "{\"mqtt_host\":\"x.example\",\"mqtt_pass\":\"mqtt-pass-9\"}", &ch) &&
        strcmp(s.mqtt_pass, "mqtt-pass-9") == 0 && !ch.mqtt_pass_cleared && ch.mqtt);
  // an invalid (too long) password for the new host is not "sent": cleared, and the error is reported
  s = stored2();
  const std::string longPass = "{\"mqtt_host\":\"x.example\",\"mqtt_pass\":\"" + std::string(70, 'x') + "\"}";
  CHECK(put2(&s, longPass.c_str(), &ch, &ne) && ne == 1 && s.mqtt_pass[0] == 0 && ch.mqtt_pass_cleared);
  // the same host and port, or only the user changed: the password stays
  s = stored2();
  CHECK(put2(&s, "{\"mqtt_host\":\"broker.lan\",\"mqtt_port\":1883,\"mqtt_user\":\"ha2\"}", &ch) &&
        strcmp(s.mqtt_pass, "mqtt-pass-9") == 0 && !ch.mqtt_pass_cleared && ch.mqtt);
  // a refused port value moves nothing, so nothing is cleared
  s = stored2();
  CHECK(put2(&s, "{\"mqtt_port\":70000}", &ch, &ne) && ne == 1 && strcmp(s.mqtt_pass, "mqtt-pass-9") == 0 && !ch.mqtt);
  // Wi-Fi: a new SSID without sta_pass is an open network, never the stored password
  for (const char* b : {"{\"sta_ssid\":\"Evil\"}", "{\"sta_ssid\":\"Evil\",\"sta_pass\":\"\"}", "{\"sta_ssid\":\"Evil\",\"sta_pass\":\"<set>\"}"}) {
    s = stored2();
    CHECK(put2(&s, b, &ch) && strcmp(s.sta_ssid, "Evil") == 0 && s.sta_pass[0] == 0 && ch.wifi && ch.sta_pass_cleared);
    CHECK(strcmp(s.mqtt_pass, "mqtt-pass-9") == 0 && !ch.mqtt_pass_cleared);  // the broker did not move
  }
  s = stored2();
  CHECK(put2(&s, "{\"sta_ssid\":\"Cafe\",\"sta_pass\":null}", &ch) && s.sta_pass[0] == 0 && !ch.sta_pass_cleared);
  s = stored2();
  CHECK(put2(&s, "{\"sta_pass\":\"new-wifi-pass\",\"sta_ssid\":\"Other\"}", &ch) &&
        strcmp(s.sta_pass, "new-wifi-pass") == 0 && !ch.sta_pass_cleared);
  // the same SSID without a password keeps the stored one (the GET document PUT back)
  s = stored2();
  CHECK(put2(&s, "{\"sta_ssid\":\"Home\",\"sta_pass\":\"<set>\"}", &ch) && strcmp(s.sta_pass, "wifi-pass-123") == 0 && !ch.wifi);
  // nothing stored: nothing to clear, nothing reported
  s = stored2();
  s.mqtt_pass[0] = 0;
  CHECK(put2(&s, "{\"mqtt_host\":\"x.example\"}", &ch) && !ch.mqtt_pass_cleared && ch.mqtt);
}

// the DNS rebinding guard: which Host headers reach the pages and the API from the home network in run mode
static void testHostAllowed() {
  for (const char* h : {"", "192.0.2.40", "192.0.2.40:80", "198.51.100.1.", "[::1]", "[fe80::1]:8080", "weather-1",
                        "WEATHER-1", "weather-1.local", "Weather-1.Local.", "weather-1.local:80", "weather-1:80",
                        "weather-1.:80", "WEATHER-1.LOCAL:80", "weather-1.lan", "weather-1.home", "weather-1.internal",
                        "weather-1.home.arpa", "Weather-1.Home.Arpa.", "weather-1.lan:8080"})
    CHECK(hostAllowed(h, "weather-1"));
  CHECK(hostAllowed(nullptr, "weather-1"));
  for (const char* h : {"evil.example", "weather-1.evil.example", "weather-1.local.evil", "evil-weather-1", "weather-1x",
                        "1.2.3.4.evil", "1.2.3", "1.2.3.256", "1.2.3.4:", "weather-1:abc", "weather-1:123456",
                        "[::1", "[]", "[::g]", "[::1]x", "fauxcloud", ".", ":80", "weather-1.example.com", "weather-1.box",
                        "weather-1.arpa", "weather-1.lan.evil.example", "weather-1.evil.lan", "evil.lan", "weather-1.lanx", "weather-1.fritz.box",
                        "weather-1..lan"})
    CHECK(!hostAllowed(h, "weather-1"));
  CHECK(hostAllowed("fauxcloud.local", DEFAULT_HOSTNAME) && !hostAllowed("fauxcloud.example", DEFAULT_HOSTNAME));
  CHECK(hostAllowed("fauxcloud.lan", DEFAULT_HOSTNAME) && !hostAllowed("fauxcloud.example.com", DEFAULT_HOSTNAME));
  CHECK(!hostAllowed("fauxcloud.fritz.box", DEFAULT_HOSTNAME));  // a public domain: someone outside could point it at the board
  CHECK(!hostAllowed("anything", "") && hostAllowed("127.0.0.1", ""));
}

// setup-mode log lines show a URI the way run mode does: no query, no Weathercloud key
static void testLogUri() {
  char out[80];
  logUri("/weatherstation/updateweatherstation.php?ID=ST1&PASSWORD=secret123", out, sizeof out);
  CHECK(strcmp(out, "/weatherstation/updateweatherstation.php?<query>") == 0);
  logUri("/v01/set/wid/ABC/key/SECRETKEY/temp/205", out, sizeof out);
  CHECK(strcmp(out, "/v01/set/<more>") == 0);
  logUri("/generate_204", out, sizeof out);
  CHECK(strcmp(out, "/generate_204") == 0);
  logUri("/a b\t", out, sizeof out);
  CHECK(strcmp(out, "/a?b?") == 0);
  char small[24];
  logUri("/abcdefghijklmnopqrstuvwxyz?PASSWORD=x", small, sizeof small);
  CHECK(strcmp(small, "/abcdefg/<more>?<query>") == 0 && !contains(small, "PASSWORD"));
}

static void testSetupApply() {
  Settings s = defaultSettings();
  BridgeConfig b = defaultBridgeConfig();
  Errors e;
  const char* ok = "{\"sta_ssid\":\"Home\",\"sta_pass\":\"wifi-pass-123\",\"ap_pass\":\"ap-pass-123\","
                   "\"admin_salt\":\"000102030405060708090a0b0c0d0e0f\","
                   "\"admin_h\":\"460b333275bd03de2073208552f336d871c8cf4ea173a9b5e61c0ef17e98998f\",\"admin_iters\":10000,"
                   "\"tz\":\"GMT0BST,M3.5.0/1,M10.5.0\",\"tz_name\":\"Europe/London\",\"lat\":51.5074,\"lon\":-0.1278}";
  CHECK(setupFromJson(ok, strlen(ok), &s, &b, &e) && e.n == 0);
  CHECK(strcmp(s.sta_ssid, "Home") == 0 && strcmp(s.ap_pass, "ap-pass-123") == 0 && !s.setup_done);
  CHECK(strcmp(s.admin_hash, "pbkdf2-sha256$10000$000102030405060708090a0b0c0d0e0f$"
                             "460b333275bd03de2073208552f336d871c8cf4ea173a9b5e61c0ef17e98998f") == 0);
  CHECK(verifyProof(s.admin_hash, "00112233445566778899aabbccddeeff",
                    "6358e538940f03d3d087a6ab8dceb417a0e3a26746d6bd589bd457ba703926e5"));  // the setup credential logs in
  CHECK(strcmp(b.tz_name, "Europe/London") == 0 && std::fabs(b.lat - 51.5074f) < 1e-4f);
  CHECK(strcmp(s.hostname, "fauxcloud") == 0);  // optional, default kept
  // setup again starts from the stored bridge block (PUT -> NVS -> boot into setup) and the apply
  // changes only its own fields, so the Publishing settings survive
  {
    BridgeConfig put = defaultBridgeConfig();
    Errors ep;
    const char* pj = "{\"lan_uploads\":true,\"max_stations\":1,\"auto_publish_new_stations\":false,\"mqtt_interval_last_seen_s\":900}";
    CHECK(bridgeFromJson(pj, strlen(pj), &put, &ep, 1) && ep.n == 0 && put.lan_uploads);
    std::string nvs;
    char ob[64];
    Out o(toString, &nvs, ob, sizeof ob);
    bridgeToJson(o, put);
    o.flush();
    BridgeConfig boot = defaultBridgeConfig();  // what loadBridge() does at boot
    Errors eb;
    CHECK(bridgeFromJson(nvs.c_str(), nvs.size(), &boot, &eb) && eb.n == 0);
    CHECK(boot.lan_uploads && boot.max_stations == 1 && !boot.auto_publish_new_stations && boot.mqtt_interval_last_seen_s == 900);
    Settings s2 = defaultSettings();
    Errors ea;
    CHECK(setupFromJson(ok, strlen(ok), &s2, &boot, &ea) && ea.n == 0);
    CHECK(boot.lan_uploads && boot.max_stations == 1 && !boot.auto_publish_new_stations &&
          boot.mqtt_interval_last_seen_s == 900 && strcmp(boot.tz_name, "Europe/London") == 0);
    // the rest of the settings block survives too: MQTT and the hostname are not on the setup page; Wi-Fi, the AP
    // password, the admin credential and the zone are (the page sends all four)
    Settings old = defaultSettings();
    snprintf(old.mqtt_host, sizeof old.mqtt_host, "%s", "ha.lan");
    old.mqtt_port = 1884;
    snprintf(old.mqtt_user, sizeof old.mqtt_user, "%s", "mq");
    snprintf(old.mqtt_pass, sizeof old.mqtt_pass, "%s", "mq-pass-1");
    snprintf(old.hostname, sizeof old.hostname, "%s", "garden");
    snprintf(old.sta_ssid, sizeof old.sta_ssid, "%s", "OldNet");
    BridgeConfig ob2 = boot;
    ob2.lat = 53.0f;
    ob2.lon = -2.0f;
    const char* nolatlon = "{\"sta_ssid\":\"Home\",\"sta_pass\":\"wifi-pass-123\",\"ap_pass\":\"ap-pass-123\","
                           "\"admin_salt\":\"000102030405060708090a0b0c0d0e0f\","
                           "\"admin_h\":\"460b333275bd03de2073208552f336d871c8cf4ea173a9b5e61c0ef17e98998f\","
                           "\"admin_iters\":10000,\"tz\":\"UTC0\"}";
    Errors eo;
    // the same SSID keeps the MQTT password; a new SSID (OldNet -> Home) clears it, user and host kept
    {
      Settings same = old;
      BridgeConfig sb = ob2;
      snprintf(same.sta_ssid, sizeof same.sta_ssid, "%s", "Home");
      Errors es;
      bool cl = true;
      CHECK(setupFromJson(nolatlon, strlen(nolatlon), &same, &sb, &es, &cl) && !cl && strcmp(same.mqtt_pass, "mq-pass-1") == 0);
    }
    bool cleared = false;
    CHECK(setupFromJson(nolatlon, strlen(nolatlon), &old, &ob2, &eo, &cleared) && eo.n == 0 && cleared);
    CHECK(strcmp(old.mqtt_host, "ha.lan") == 0 && old.mqtt_port == 1884 && strcmp(old.mqtt_user, "mq") == 0 &&
          old.mqtt_pass[0] == 0 && strcmp(old.hostname, "garden") == 0);
    CHECK(strcmp(old.sta_ssid, "Home") == 0 && strcmp(ob2.tz, "UTC0") == 0);  // what the page asks for changes
    CHECK(std::fabs(ob2.lat - 53.0f) < 1e-4f && std::fabs(ob2.lon - -2.0f) < 1e-4f);  // no location sent: kept
    CHECK(ob2.lan_uploads && ob2.max_stations == 1);
    // setup again: no ap_pass and no admin credential keep the stored ones; sent ones replace them
    char apBefore[sizeof old.ap_pass], adBefore[sizeof old.admin_hash];
    snprintf(apBefore, sizeof apBefore, "%s", old.ap_pass);
    snprintf(adBefore, sizeof adBefore, "%s", old.admin_hash);
    CHECK(apBefore[0] && adBefore[0]);
    const char* keep = "{\"sta_ssid\":\"Home2\",\"sta_pass\":\"wifi-pass-456\",\"tz\":\"UTC0\"}";
    Errors ek;
    CHECK(setupFromJson(keep, strlen(keep), &old, &ob2, &ek) && ek.n == 0);
    CHECK(strcmp(old.sta_ssid, "Home2") == 0 && strcmp(old.ap_pass, apBefore) == 0 && strcmp(old.admin_hash, adBefore) == 0);
    const char* repl = "{\"sta_ssid\":\"Home2\",\"sta_pass\":\"wifi-pass-456\",\"tz\":\"UTC0\",\"ap_pass\":\"new-ap-pass-9\","
                       "\"admin_salt\":\"0f0e0d0c0b0a09080706050403020100\","
                       "\"admin_h\":\"460b333275bd03de2073208552f336d871c8cf4ea173a9b5e61c0ef17e98998f\",\"admin_iters\":10000}";
    Errors er;
    CHECK(setupFromJson(repl, strlen(repl), &old, &ob2, &er) && er.n == 0);
    CHECK(strcmp(old.ap_pass, "new-ap-pass-9") == 0 && strcmp(old.admin_hash, adBefore) != 0);
    // a sent but bad AP password is an error, not "keep"
    const char* badap = "{\"sta_ssid\":\"Home2\",\"sta_pass\":\"wifi-pass-456\",\"tz\":\"UTC0\",\"ap_pass\":\"short\"}";
    Errors eb2;
    CHECK(!setupFromJson(badap, strlen(badap), &old, &ob2, &eb2) && eb2.n == 1 && strncmp(eb2.at(0), "ap_pass", 7) == 0);
    // first run (nothing stored): both still required
    Settings fresh = defaultSettings();
    BridgeConfig fb = defaultBridgeConfig();
    Errors ef;
    CHECK(!setupFromJson(keep, strlen(keep), &fresh, &fb, &ef) && ef.n == 1 && strncmp(ef.at(0), "admin_h", 7) == 0);
    // the first run (no stored SSID) clears nothing
    Settings first = defaultSettings();
    snprintf(first.mqtt_pass, sizeof first.mqtt_pass, "%s", "mq-pass-2");
    BridgeConfig fb2 = defaultBridgeConfig();
    Errors e1;
    bool cl1 = true;
    CHECK(setupFromJson(ok, strlen(ok), &first, &fb2, &e1, &cl1) && !cl1 && strcmp(first.mqtt_pass, "mq-pass-2") == 0);
  }
  // the plain password is refused, a missing credential is reported, nothing is applied
  Settings s3 = defaultSettings();
  BridgeConfig b3 = defaultBridgeConfig();
  Errors e3;
  const char* plain = "{\"sta_ssid\":\"Home\",\"sta_pass\":\"wifi-pass-123\",\"ap_pass\":\"ap-pass-123\","
                      "\"admin_password\":\"admin-pass-1\",\"tz\":\"UTC0\"}";
  CHECK(!setupFromJson(plain, strlen(plain), &s3, &b3, &e3) && e3.n == 1 && strncmp(e3.at(0), "admin_password", 14) == 0);
  CHECK(s3.sta_ssid[0] == 0 && s3.admin_hash[0] == 0);
  Errors e4;
  const char* bad = "{\"sta_ssid\":\"Home\",\"sta_pass\":\"short\",\"ap_pass\":\"ap\",\"tz\":\"Europe/London\","
                    "\"admin_salt\":\"00\",\"admin_h\":\"00\"}";
  CHECK(!setupFromJson(bad, strlen(bad), &s3, &b3, &e4) && e4.n == 3);
  Errors e5;
  const char* nocred = "{\"sta_ssid\":\"Cafe\",\"sta_pass\":\"\",\"ap_pass\":\"ap-pass-123\",\"tz\":\"UTC0\"}";
  CHECK(!setupFromJson(nocred, strlen(nocred), &s3, &b3, &e5) && e5.n == 1 && strncmp(e5.at(0), "admin_h", 7) == 0);
  CHECK(strcmp(e5.at(0), "admin_h: required: admin_salt (32 hex) + admin_h (64 hex), iters 10000..200000") == 0);  // whole
}

static void testWifiAttempt() {
  // fixed failure vocabulary
  CHECK(strcmp(wifiFailCode(202, false), "auth") == 0 && strcmp(wifiFailCode(15, true), "auth") == 0);
  CHECK(strcmp(wifiFailCode(201, false), "no_ap") == 0 && strcmp(wifiFailCode(211, false), "no_ap") == 0);
  CHECK(strcmp(wifiFailCode(0, true), "dhcp") == 0 && strcmp(wifiFailCode(8, true), "dhcp") == 0);
  CHECK(strcmp(wifiFailCode(0, false), "timeout") == 0 && strcmp(wifiFailCode(200, false), "timeout") == 0);
  WifiAttempt a;
  CHECK(a.state() == WifiAttempt::State::Idle);
  a.start(1000);
  CHECK(a.tick(5000, false, 0, false) == WifiAttempt::State::Trying);
  CHECK(a.tick(9000, false, 15, true) == WifiAttempt::State::Trying);
  CHECK(a.tick(10000, false, 0, false) == WifiAttempt::State::Trying);  // 0 = no new reason, the last one stays
  CHECK(a.tick(1000 + WifiAttempt::TIMEOUT_MS, false, 0, false) == WifiAttempt::State::Failed);
  CHECK(strcmp(a.reason(), "auth") == 0);
  CHECK(a.tick(1000 + 2 * WifiAttempt::TIMEOUT_MS, true, 0, true) == WifiAttempt::State::Failed);  // sticky
  a.start(200000);
  CHECK(a.tick(212000, true, 0, true) == WifiAttempt::State::Succeeded);
  WifiAttempt b;
  b.start(0);
  b.tick(1000, false, 0, true);  // associated, never an address
  CHECK(b.tick(60000, false, 0, false) == WifiAttempt::State::Failed && strcmp(b.reason(), "dhcp") == 0);
  WifiAttempt c;
  c.start(0);
  CHECK(c.tick(60000, false, 201, false) == WifiAttempt::State::Failed && strcmp(c.reason(), "no_ap") == 0);
  WifiAttempt w;  // millis() wrap-around
  w.start(0xFFFFF000u);
  CHECK(w.tick(0x00001000u, false, 0, false) == WifiAttempt::State::Trying);
}

static void testHoldButton() {
  HoldButton b;
  int fired = 0, last = HoldButton::NONE, countdown = 0;
  // bounce on press: 20 ms pressed, 10 ms released, then held
  CHECK(b.feed(0, true) == HoldButton::NONE);
  CHECK(b.feed(20, false) == HoldButton::NONE);
  for (uint32_t t = 30; t <= 6000; t += 10) {
    int r = b.feed(t, true);
    if (r == HoldButton::FIRED) fired++;
    else if (r > 0) { countdown++; last = r; }
  }
  CHECK(fired == 1 && countdown == 4 && last == 4);  // 1..4 s, then fired once at 5 s
  // a short press never fires; releasing re-arms
  HoldButton c;
  int f2 = 0;
  for (uint32_t t = 0; t < 3000; t += 10) f2 += c.feed(t, true) == HoldButton::FIRED;
  for (uint32_t t = 3000; t < 3200; t += 10) c.feed(t, false);
  for (uint32_t t = 3200; t < 6000; t += 10) f2 += c.feed(t, true) == HoldButton::FIRED;
  CHECK(f2 == 0);
  for (uint32_t t = 6000; t < 8300; t += 10) f2 += c.feed(t, true) == HoldButton::FIRED;
  CHECK(f2 == 1);
  // the release decides (>= 1 s and < 3 s = setup, keeping the data)
  auto press = [](uint32_t hold_ms) {
    HoldButton h;
    int shorts = 0, fires = 0;
    uint32_t t = 0;
    for (; t < hold_ms; t += 10) fires += h.feed(t, true) == HoldButton::FIRED;
    for (uint32_t e = t + 200; t < e; t += 10) {
      const int r = h.feed(t, false);
      shorts += r == HoldButton::SHORT;
      fires += r == HoldButton::FIRED;
    }
    return shorts * 10 + fires;
  };
  CHECK(press(500) == 0 && press(990) == 0);     // a tap: nothing
  CHECK(press(1000) == 10 && press(2500) == 10 && press(2990) == 10);  // short press: setup once
  CHECK(press(3000) == 0 && press(4500) == 0);   // 3..5 s, released: nothing (changed my mind)
  CHECK(press(5100) == 1);                       // 5 s: the reset fired, no setup on release
  HoldButton hm;
  for (uint32_t t = 0; t <= 1500; t += 10) hm.feed(t, true);
  CHECK(hm.heldMs(1500) == 1500 && hm.heldMs(1500) > 0);
  for (uint32_t t = 1510; t < 1700; t += 10) hm.feed(t, false);
  CHECK(hm.heldMs(1700) == 0);
  // a glitch shorter than the debounce never starts a press
  HoldButton d;
  int f3 = 0;
  for (uint32_t t = 0; t < 10000; t += 10) f3 += d.feed(t, (t % 100) < 30) != HoldButton::NONE;
  CHECK(f3 == 0);
}

static void testLazyStations() {
  static Stations all;
  CHECK(all.allocated() == 0 && all.first() == nullptr && all.at(0) == nullptr);
  StationConfig cfg = defaultStationConfig();
  CHECK(all.findOrAdd("A", cfg) == nullptr);  // no reserved slot, no station (never a lazy allocation)
  CHECK(all.reserve(1) == 1 && all.allocated() == 1 && all.active() == 0);
  Station* a = all.findOrAdd("A", cfg);
  CHECK(a && all.allocated() == 1 && all.find("A") == a);
  CHECK(!all.findOrAdd("B", cfg));                                  // the one reserved slot in use
  CHECK(all.reserve(MAX_STATIONS) == MAX_STATIONS && all.at(0) == a);  // raising max_stations keeps the stations
#if FC_MAX_STATIONS > 1
  Station* b = all.findOrAdd("B", cfg);
  CHECK(b && b != a && all.indexOf(b) == 1 && !all.findOrAdd("C", cfg));  // full at 2 (the two-station ceiling)
#else
  CHECK(!all.findOrAdd("B", cfg) && all.reserve(2) == 1);  // one slot on this profile, a second reserve adds none
#endif
  CHECK(all.allocated() == MAX_STATIONS && all.find("C") == nullptr && all.indexOf(nullptr) == -1);
  CHECK(a->uploads == 0 && a->recent.size() == 0 && a->hourly.size() == 0 && a->daily.size() == 0);  // zeroed
  // liveness on millis, immune to the wall clock jumping at SNTP sync
  PosixTz tz;
  parsePosixTz("UTC0", &tz);
  Stations::ingest(*a, sampleFrom("ID=A&tempf=50"), 120 /*uptime clock*/, 120000, 0, SrcIf::Ap, false, tz);
  CHECK(a->online(120000 + 239000) && !a->online(120000 + 240000));
}

static void testBridgeAddendum() {
  BridgeConfig b = defaultBridgeConfig();
  CHECK(std::isnan(b.lat) && std::isnan(b.lon) && b.tz_name[0] == 0);
  Errors e;
  const char* j = "{\"tz\":\"GMT0BST,M3.5.0/1,M10.5.0\",\"tz_name\":\"Europe/London\",\"lat\":51.5074,\"lon\":-0.1278}";
  CHECK(bridgeFromJson(j, strlen(j), &b, &e) && e.n == 0);
  CHECK(strcmp(b.tz_name, "Europe/London") == 0 && std::fabs(b.lat - 51.5074f) < 1e-4f);
  const std::string out = render(bridgeJsonFn, &b);
  CHECK(contains(out, "\"tz_name\":\"Europe/London\"") && contains(out, "\"lat\":51.5074") && contains(out, "\"lon\":-0.1278"));
  Errors e2;
  const char* bad = "{\"tz_name\":\"Europe/Lon don\",\"lat\":91,\"lon\":\"x\"}";
  CHECK(bridgeFromJson(bad, strlen(bad), &b, &e2) && e2.n == 3 && strcmp(b.tz_name, "Europe/London") == 0);
  Errors e3;
  const char* clr = "{\"lat\":null,\"lon\":null,\"tz_name\":null}";
  CHECK(bridgeFromJson(clr, strlen(clr), &b, &e3) && std::isnan(b.lat) && b.tz_name[0] == 0);
}

static bool jsonParses(const std::string& j);  // defined with testFieldMaxima
static void testDiagAddendum() {
  static Stations all;
  all.reserve(MAX_STATIONS);
  Diag d = {};
  d.heap_free = 50000;
  strcpy(d.sta_ip, "192.168.1.20");
  d.fw_version = "fauxcloud 0.1.0-b3";
  d.hostname = "fauxcloud";
  d.stations_allocated = 1;
  d.stations_used = 1;
  d.admin_locked = true;
  d.admin_lock_s = 290;
  d.admin_failures = 10;
  d.heap_low = true;
  d.heap_min_10m = 40000;
  d.http_requests = 7;
  d.http_304 = 3;
  d.http_busy_ms_max = 12;
  d.http_aborted = 2;
  d.http_purged = 4;
  d.http_shed = 9;
  d.page = "ad31c0de";
  std::string s;
  char buf[128];
  Out o(toString, &s, buf, sizeof buf);
  Clock c{1800000000, true, 10, 10000};
  renderLive(o, all, c, &d, "e204b305");
  o.flush();
  // the dashboard shell's version in the head, the admin shell's in the diag, next to the build
  CHECK(contains(s, "\"time_src\":\"none\",\"page\":\"e204b305\",\"uptime_s\":10,"));
#if FC_DIAG_BENCH
  const char* const mid = "\"ota_heap\":null,\"heap_min8_t\":null,\"heap_min8_ctx\":[],\"heap_min8_socks\":null,\"tasks\":{},"
                          "\"http_busy_ms_max\":12,\"http_aborted\":2,";
#else  // the bench keys are absent from the release JSON
  const char* const mid = "\"heap_min8_t\":null,\"heap_min8_ctx\":[],\"heap_min8_socks\":null,\"http_aborted\":2,";
#endif
  CHECK(contains(s, (std::string("\"heap_low\":true,\"heap_min_10m\":40000,\"http_requests\":7,\"http_304\":3,") + mid +
                    "\"http_purged\":4,\"http_shed\":9,\"http_paced\":0,\"sta_ip\":\"192.168.1.20\",\"fw_version\":\"fauxcloud 0.1.0-b3\","
                    "\"hostname\":\"fauxcloud\",\"stations_allocated\":1,\"stations_used\":1,\"mode\":\"run\","
                    "\"admin_lock\":{\"active\":true,\"until_s\":290,\"failures\":10},\"store_ms_max\":0,\"build\":\"\",\"page\":\"ad31c0de\","
                    "\"ota_state\":\"stable\",\"ota_slot\":0,\"last_reset\":\"unknown\",\"reset_raw\":\"\",\"restarts\":0,\"sta_reinits\":0,"
                    "\"mqtt_reconnects\":0,\"last_crash\":null}").c_str()));
  // the last OTA write's heap record, after the http_304 group (no INTERNAL heap figures)
  CHECK(!contains(s, "heap_internal") && !contains(s, "store_archived"));
#if !FC_DIAG_BENCH
  // every bench key of /api/live's diag is absent (not null, not {}) in the release, whatever the struct holds
  {
    Diag e = d;
    e.ota_heap_have = true, e.ota_start8 = 1, e.task_free[0] = 5120, e.fwd_heap_min = 7, e.fwd_largest_min = 7, e.fwd_send_ms_max = 7;
    std::string se;
    Out oe(toString, &se, buf, sizeof buf);
    renderLive(oe, all, c, &e);
    oe.flush();
    for (const char* k : {"\"ota_heap\"", "\"tasks\"", "\"fwd_heap_min\"", "\"fwd_largest_min\"", "\"fwd_send_ms_max\"",
                          "\"http_busy_ms_max\""})
      CHECK(!contains(se, k));
    CHECK(jsonParses(se) && contains(se, "\"fwd_drop\":0,\"heap_low\""));
  }
#else
  {
    Diag e = d;
    e.ota_heap_have = true, e.ota_start8 = 50240, e.ota_min8 = 24812, e.ota_largest_min8 = 12288;
    std::string se;
    Out oe(toString, &se, buf, sizeof buf);
    renderLive(oe, all, c, &e);
    oe.flush();
    CHECK(contains(se, "\"http_304\":3,"
                       "\"ota_heap\":{\"start8\":50240,\"min8\":24812,\"largest_min8\":12288},\"heap_min8_t\":null,\"heap_min8_ctx\":[],\"heap_min8_socks\":null,\"tasks\":{},\"http_busy_ms_max\":12") &&
          jsonParses(se));
  }
  // diag.tasks: bytes of stack never used per task, only the tasks that exist (0 = none); names map to keys
  {
    CHECK(diagTaskIndex("loopTask") == 0 && diagTaskIndex("mqtt_task") == 2 && diagTaskIndex("netout") == 5 &&
          diagTaskIndex("mqtt") == -1 && diagTaskIndex(nullptr) == -1 && diagTaskIndex("IDLE0") == -1);
    Diag e = d;
    e.task_free[0] = 5120, e.task_free[1] = 3300, e.task_free[2] = 2100, e.task_free[3] = 1400, e.task_free[5] = 2644;
    std::string se;
    Out oe(toString, &se, buf, sizeof buf);
    renderLive(oe, all, c, &e);
    oe.flush();
    CHECK(contains(se, "\"ota_heap\":null,\"heap_min8_t\":null,\"heap_min8_ctx\":[],\"heap_min8_socks\":null,\"tasks\":{\"loop\":5120,\"httpd\":3300,\"mqtt\":2100,\"udp53_123\":1400,"
                       "\"netout\":2644},\"http_busy_ms_max\"") && jsonParses(se));
  }
#endif  // FC_DIAG_BENCH
  // the second of the since-boot 8-bit minimum, its context names in bit order and the open sockets
  {
    Diag e = d;
    CHECK(e.heap_dip.note(2812, 60927, HC_HTTP | HC_ACCEPT | HC_MQTT, 3));
    std::string se;
    Out oe(toString, &se, buf, sizeof buf);
    renderLive(oe, all, c, &e);
    oe.flush();
    CHECK(contains(se, "\"heap_min8_t\":60927,\"heap_min8_ctx\":[\"http\",\"accept\",\"mqtt\"],"
                       "\"heap_min8_socks\":3") && jsonParses(se));
    Diag f = d;
    CHECK(f.heap_dip.note(9000, 5, 0xFF, -1));
    std::string sf;
    Out of(toString, &sf, buf, sizeof buf);
    renderLive(of, all, c, &f);
    of.flush();
    CHECK(contains(sf, "\"heap_min8_ctx\":[\"http\",\"accept\",\"fwd\",\"mqtt\",\"store\",\"learn\",\"ota\",\"upload\"],"
                       "\"heap_min8_socks\":-1,") && jsonParses(sf));
  }
  // rollback state, reset reason, crash data
  d.ota_state = "rolled_back";
  d.last_reset = "task_wdt";
  d.restarts = 7;
  d.crash = true;
  d.crash_reason = "Task watchdog got triggered";
  d.crash_task = "loopTask";
  d.crash_pc = 0x400d1234;
  std::string s2;
  Out o3(toString, &s2, buf, sizeof buf);
  renderLive(o3, all, c, &d);
  o3.flush();
  CHECK(contains(s2, "\"ota_state\":\"rolled_back\"") && contains(s2, "\"last_reset\":\"task_wdt\",\"reset_raw\":\"\",\"restarts\":7") &&
        contains(s2, "\"last_crash\":{\"reason\":\"Task watchdog got triggered\",\"task\":\"loopTask\",\"pc\":\"0x400d1234\"}}"));
  // the store counters must not overrun Out::printf's 192 B (that cut the diag JSON on the board)
  CHECK(contains(s2, ("\"store_fail\":0,\"store_drop_old\":0,\"store_cut\":0,"
                     "\"store_fail_append\":0,\"store_future_dropped\":0,\"clock_held\":false,\"clock_held_n\":0,"
                     "\"clock_ref\":0,\"clock_cut_n\":0,\"store_future_kept\":0,\"store_retry\":0,\"http_stream_abort\":0,"
                     "\"mqtt_init_fail\":0,\"snap_from\":0,\"hist_back\":[0" + std::string(FC_MAX_STATIONS > 1 ? ",0" : "") +
                     "],\"dns_answered\":").c_str()));
  std::string page;
  Out o2(toString, &page, buf, sizeof buf);
  renderPage(o2, all, c, "b3", &d);
  o2.flush();
  CHECK(contains(page, "Admin login locked for 290 s after 10 failed attempts."));
  // /tools: the httpd renders each card from a memcpy of the station, without gLock -- the same bytes
  {
    Station* t = all.findOrAdd("TL1", defaultStationConfig());
    const uint32_t t0 = (uint32_t)unixFromUtc(2026, 11, 9, 20, 0, 0);
    PosixTz utc;
    parsePosixTz("UTC0", &utc);
    Stations::ingest(*t, sampleFrom("ID=TL1&tempf=50&windspeedmph=3&winddir=200"), t0, 1000, 0, SrcIf::Ap, true, utc);
    Stations::ingest(*t, sampleFrom("ID=TL1&tempf=51&windspeedmph=4&winddir=210"), t0 + 60, 61000, 0, SrcIf::Ap, true, utc);
  }
  page.clear();
  Out o4b(toString, &page, buf, sizeof buf);
  renderPage(o4b, all, c, "b3", &d);
  o4b.flush();
  CHECK(contains(page, "TL1"));
  std::string parts;
  Out o5(toString, &parts, buf, sizeof buf);
  renderPageHead(o5, c, "b3", &d);
  bool any = false;
  static Station copy;
  for (int i = 0; i < MAX_STATIONS; i++)
    if (const Station* sp = all.at(i)) {
      memcpy((void*)&copy, (const void*)sp, sizeof copy);
      renderStationCard(o5, copy, c);
      any = true;
    }
  renderPageTail(o5, any);
  o5.flush();
  CHECK(any && parts == page);
}

static void testLed() {
  LedInputs in = {};
  in.uptime_ms = 20000;
  in.sta_up = true;
  CHECK(ledState(in) == LedState::Ok);
  in.attention = true;
  CHECK(ledState(in) == LedState::Attention);
  in.sta_up = false;
  CHECK(ledState(in) == LedState::Connecting);  // no home Wi-Fi outranks attention
  in.uptime_ms = 5000;
  CHECK(ledState(in) == LedState::Booting);     // boot transients are not alarms
  in.setup_mode = true;
  CHECK(ledState(in) == LedState::Setup);
  in.button_held = true;
  CHECK(ledState(in) == LedState::ButtonHeld);
  in.reset_commit = true;
  CHECK(ledState(in) == LedState::ResetCommit);
  // patterns: duty within one period
  auto lit = [](LedState s, uint32_t period, bool sta) {
    int n = 0;
    for (uint32_t t = 0; t < period; t++) n += ledOn(s, 100000 + t, sta);
    return n;
  };
  CHECK(lit(LedState::ButtonHeld, 1000, false) == 0);  // held < 1 s: dark (a tap does nothing)
  int blinks = 0;  // held 1..4.4 s: one 200 ms blink per whole second
  for (uint32_t h = 0; h < 4400; h++) blinks += ledOn(LedState::ButtonHeld, 100000 + h, false, h) && !ledOn(LedState::ButtonHeld, 100000 + h - 1, false, h - 1);
  CHECK(blinks == 4);
  LedInputs hold = {};
  hold.button_held = true;
  hold.held_ms = 4400;
  CHECK(ledState(hold) == LedState::ButtonHeld);
  hold.held_ms = 4500;
  CHECK(ledState(hold) == LedState::ResetCommit);  // from 4.5 s the strobe says: the next second erases
  CHECK(lit(LedState::Setup, 250, false) == 125);
  CHECK(lit(LedState::ResetCommit, 100, false) == 50);
  CHECK(lit(LedState::Ok, 6000, true) == 500);
  CHECK(lit(LedState::Connecting, 1000, false) == 100);
  CHECK(lit(LedState::Attention, 1600, true) == 360);
  CHECK(lit(LedState::Booting, 6000, true) == 500 && lit(LedState::Booting, 1000, false) == 100);
}

static void testForgetSlot() {
  static Stations all;
  all.reserve(MAX_STATIONS);
  StationConfig cfg = defaultStationConfig();
  Station* a = all.findOrAdd("A", cfg);
  a->uploads = 5;
#if FC_MAX_STATIONS > 1
  Station* b = all.findOrAdd("B", cfg);
  CHECK(all.allocated() == 2 && all.active() == 2);
  CHECK(all.forget(0) && !all.forget(0));
  CHECK(all.allocated() == 2 && all.active() == 1 && all.find("A") == nullptr && all.at(0) == nullptr);
  CHECK(all.first() == b);
  Station* c = all.findOrAdd("C", cfg);  // reuses the forgotten allocation
  CHECK(c == a && c->uploads == 0 && strcmp(c->id, "C") == 0 && all.allocated() == 2 && all.active() == 2);
#else
  CHECK(all.allocated() == 1 && all.active() == 1 && !all.findOrAdd("B", cfg));  // the one slot is taken
  CHECK(all.forget(0) && !all.forget(0) && all.active() == 0 && all.first() == nullptr);
  Station* c = all.findOrAdd("C", cfg);  // reuses the forgotten allocation
  CHECK(c == a && c->uploads == 0 && strcmp(c->id, "C") == 0 && all.allocated() == 1 && all.active() == 1);
#endif
}

// ---- promotions, station slots ----------------------------------------------------------------------------------

static void testPromoteByType() {
  char slug[24];
  promoSlug("Jardín Peñón – suelo", slug);
  CHECK(strcmp(slug, "jardin_penon_suelo") == 0);
  promoSlug("CO₂ µg", slug);
  CHECK(strcmp(slug, "co2_ug") == 0);
  promoSlug("  !!  ", slug);
  CHECK(slug[0] == 0);
  PromoType t;
  CHECK(promoTypeFromName("soil_moisture", &t) && t == PromoType::SoilMoisture && !promoTypeFromName("bogus", &t));
  CHECK(strcmp(promoType(PromoType::Pressure).device_class, "atmospheric_pressure") == 0);
  // the type fills every technical field; the key is derived from the name and made unique
  Errors e;
  StationConfig c = mergeStation(
      "{\"promote\":{\"soiltempf\":{\"name\":\"Soil temp\",\"type\":\"soil_temperature\"},"
      "\"soiltemp2f\":{\"name\":\"Soil temp\",\"type\":\"soil_temperature\"},"
      "\"soilmoisture\":{\"name\":\"Bed moisture\",\"type\":\"soil_moisture\",\"unit\":\"vwc\"}}}",
      &e);
  CHECK(e.n == 0);
  CHECK(c.promote[0].used && strcmp(c.promote[0].key, "soil_temp") == 0 && c.promote[0].conv == PromoConv::FtoC &&
        strcmp(c.promote[0].unit, "°C") == 0 && strcmp(c.promote[0].device_class, "temperature") == 0);
  CHECK(strcmp(c.promote[1].key, "soil_temp_2") == 0);
  CHECK(strcmp(c.promote[2].unit, "vwc") == 0 && strcmp(c.promote[2].device_class, "moisture") == 0);
  const std::string out = render(toJsonFn, &c);
  CHECK(contains(out, "\"soilmoisture\":{\"name\":\"Bed moisture\",\"type\":\"soil_moisture\",\"as\":\"bed_moisture\","
                      "\"unit\":\"vwc\"}"));  // only the override that differs from the type
  Errors e2;
  StationConfig back = mergeStation(out.c_str(), &e2);
  CHECK(e2.n == 0 && render(toJsonFn, &back) == out);
  // unknown type and an explicit duplicate key are errors; a 32-byte name is refused
  Errors e3;
  mergeStation("{\"promote\":{\"a1\":{\"type\":\"plasma\"},\"a2\":{\"as\":\"k\"},\"a3\":{\"as\":\"k\"},"
               "\"a4\":{\"name\":\"0123456789012345678901234567890X\"}}}",
               &e3);
  CHECK(e3.n == 3);
}

static void testMaxStations() {
  BridgeConfig b = defaultBridgeConfig();
  CHECK(b.max_stations == MAX_STATIONS);  // the profile's
  CHECK(contains(render(bridgeEffFn, &b), ("\"max_stations_cap\":" + std::to_string(MAX_STATIONS)).c_str()) &&
        render(bridgeJsonFn, &b).find("max_stations_cap") == std::string::npos);  // in the view, never stored
  {
    Errors ec;
    const char* cap = "{\"max_stations_cap\":9}";  // read-only: a PUT of the effective view passes it silently
    BridgeConfig c = b;
    CHECK(bridgeFromJson(cap, strlen(cap), &c, &ec) && ec.n == 0 && c.max_stations == b.max_stations);
  }
#if FC_MAX_STATIONS == 1
  {  // fixed at 1 on the WROOM-32 profile; a stored 2 (the old default) or 4 loads as 1
    Errors e1;
    const char* two = "{\"max_stations\":2}";
    CHECK(bridgeFromJson(two, strlen(two), &b, &e1, 0) && e1.n == 1 && b.max_stations == 1 &&
          strcmp(e1.at(0), "max_stations: fixed at 1 on this board") == 0);
    Errors e2;
    const char* one = "{\"max_stations\":1}";
    CHECK(bridgeFromJson(one, strlen(one), &b, &e2, 1) && e2.n == 0 && b.max_stations == 1);
    BridgeConfig boot = defaultBridgeConfig();
    Errors e4;
    const char* stored = "{\"max_stations\":4,\"lan_uploads\":true}";
    bridgeFromJson(stored, strlen(stored), &boot, &e4, 0);
    CHECK(e4.n == 1 && boot.max_stations == 1 && boot.lan_uploads && strncmp(e4.at(0), "max_stations", 12) == 0);
    CHECK(render(bridgeJsonFn, &boot).find("max_stations") == std::string::npos);  // the default: not stored
    static Stations one1;
    one1.reserve(MAX_STATIONS);
    StationConfig cfg = defaultStationConfig();
    CHECK(one1.findOrAdd("A", cfg, 1) && !one1.findOrAdd("B", cfg, 1) && !one1.findOrAdd("B", cfg, 2));
    const uint32_t g = one1.find("A")->gen;
    CHECK(one1.forget(0) && one1.findOrAdd("E", cfg, 1) && one1.find("E")->gen != g);  // slot reuse: new generation
  }
#else
  Errors e;
  const char* j1 = "{\"max_stations\":1}";
  CHECK(bridgeFromJson(j1, strlen(j1), &b, &e, 1) && e.n == 0 && b.max_stations == 1);
  Errors e2;
  const char* j2 = "{\"max_stations\":1}";
  b.max_stations = 2;
  bridgeFromJson(j2, strlen(j2), &b, &e2, 2);  // below the stations in use: refused, value kept
  CHECK(e2.n == 1 && b.max_stations == 2 &&
        strcmp(e2.at(0), "max_stations: below the number of stations in use: forget one first") == 0);  // whole
  // the ceiling clamp: 3 and 4 are refused; a stored 3/4 at load keeps the default 2
  Errors e3;
  const char* j5 = "{\"max_stations\":3}";
  b.max_stations = 1;
  bridgeFromJson(j5, strlen(j5), &b, &e3, 0);
  CHECK(e3.n == 1 && b.max_stations == 1 && contains(std::string(e3.at(0)), "must be 1..2"));
  BridgeConfig boot = defaultBridgeConfig();
  Errors e4;
  const char* stored = "{\"max_stations\":4,\"lan_uploads\":true}";
  bridgeFromJson(stored, strlen(stored), &boot, &e4, 0);
  CHECK(e4.n == 1 && boot.max_stations == 2 && boot.lan_uploads && strncmp(e4.at(0), "max_stations", 12) == 0);
  b.max_stations = 2;
  CHECK(contains(render(bridgeJsonFn, &b), "\"max_stations\":2") || render(bridgeJsonFn, &b).find("max_stations") == std::string::npos);
  static Stations all;
  all.reserve(MAX_STATIONS);
  StationConfig cfg = defaultStationConfig();
  CHECK(all.findOrAdd("A", cfg, 1) && !all.findOrAdd("B", cfg, 1));
  CHECK(all.findOrAdd("A", cfg, 1) != nullptr);  // known stations still found at the limit
  CHECK(all.findOrAdd("B", cfg, 2) && !all.findOrAdd("C", cfg, 2) && all.active() == 2);
  const uint32_t g = all.find("B")->gen;
  CHECK(all.forget(1) && all.findOrAdd("E", cfg, 2) && all.find("E")->gen != g);  // slot reuse: new generation
#endif
}

// the fingerprints, pinned per profile (RING_SLOTS is the profile's). The PSRAM profile keeps the earlier two-ring
// layout's (0x35379df4 / 0xb40a844f); the WROOM-32's one-ring layout (the 1.x table) has its own.
#if FC_PROFILE_PSRAM
constexpr uint32_t RING_FP_PIN = 0x35379df4u, SNAP_FP_PIN = 0xb40a844fu;
#else
constexpr uint32_t RING_FP_PIN = 0xf3ddaea3u, SNAP_FP_PIN = 0x17c6fd04u;
#endif
// the station count is a build-profile constant (profile.h). Runs in every host variant (1 and 2 stations).
static void testStationsPerProfile() {
  CHECK(MAX_STATIONS == FC_MAX_STATIONS && DEFAULT_MAX_STATIONS == FC_MAX_STATIONS && RING_SLOTS == (FC_PROFILE_PSRAM ? 2 : 1));
  // the stored formats never carry the station count, only the profile's ring layout (pinned per profile)
  printf("  info: build profile %d station(s): ringFingerprint 0x%08lx, snapFingerprint 0x%08lx\n", MAX_STATIONS,
         (unsigned long)ringFingerprint(), (unsigned long)snapFingerprint());
  CHECK(ringFingerprint() == RING_FP_PIN && snapFingerprint() == SNAP_FP_PIN);
  // the partition table the image expects (tableOk on the board), per profile: size, littlefs offset and app slot
#if FC_PROFILE_PSRAM
  CHECK(tableMatches(0x398000, 0x68000, 0x1C0000) && !tableMatches(0x3BA000, 0x46000, 0x1D0000));
#else
  CHECK(tableMatches(0x3BA000, 0x46000, 0x1D0000) && LFS_BLOCKS == 70 && TABLE_LFS == 0x46000);
  CHECK(!tableMatches(0x398000, 0x68000, 0x1C0000));  // the earlier table (this image OTA'd onto an old board): RAM only
  CHECK(!tableMatches(0x3BA000, 0x46000, 0x1C0000));  // the right littlefs, the old app slot
  CHECK(!tableMatches(0x3B8000, 0x46000, 0x1D0000));  // the right sizes at another offset
  CHECK(!tableMatches(0x3BA000, 0x44000, 0x1D0000));  // a smaller littlefs (68 blocks)
#endif
  // boot load order: the ring owner with the newest hour key, the other ring owner, then
  // the ids without a ring; a tie keeps ring 0; no owner -> "" (nobody in passes 0 / 1)
  {
    const LoadOrder a = loadOrder("OLD1", 5000, "NEW1", 9000);  // the console changed its id: the live one first
    CHECK(strcmp(a.first, "NEW1") == 0 && strcmp(a.second, "OLD1") == 0);
    CHECK(loadPass("NEW1", a) == 0 && loadPass("OLD1", a) == 1 && loadPass("C1", a) == 2 && loadPass(nullptr, a) == 2);
    const LoadOrder t = loadOrder("R0", 7000, "R1", 7000);  // a tie: ring 0
    CHECK(strcmp(t.first, "R0") == 0 && strcmp(t.second, "R1") == 0);
    const LoadOrder o1 = loadOrder("", 0, "R1", 10);  // ring 0 free: ring 1's owner first, ringless after it
    CHECK(strcmp(o1.first, "R1") == 0 && o1.second[0] == 0 && loadPass("R1", o1) == 0 && loadPass("C1", o1) == 2);
    const LoadOrder none = loadOrder(nullptr, 0, "", 0);
    CHECK(none.first[0] == 0 && none.second[0] == 0 && loadPass("A1", none) == 2 && loadPass("", none) == 2);
  }
  {  // a board that stored three stations (NVS order C1, OLD1, NEW1; NEW1 newest on ring 1, OLD1 on ring 0, C1 no ring)
    static Stations st;
    st.reserve(MAX_STATIONS);
    const char* nvs[3] = {"C1", "OLD1", "NEW1"};
    const LoadOrder o = loadOrder("OLD1", 5000, "NEW1", 9000);
    ParkedList parked = {};
    for (int pass = 0; pass < 3; pass++)
      for (const char* id : nvs)
        if (loadPass(id, o) == pass && !st.findOrAdd(id, defaultStationConfig())) parked.add(id);
    CHECK(st.at(0) && strcmp(st.at(0)->id, "NEW1") == 0);  // the live console keeps the slot
#if FC_MAX_STATIONS == 1
    CHECK(parked.n == 2 && parked.has("OLD1") && parked.has("C1") && st.active() == 1);
#else
    CHECK(parked.n == 1 && parked.has("C1") && strcmp(st.at(1)->id, "OLD1") == 0);  // the ring owner before the ringless
#endif
    // a parked station is not a ghost (its HA device stays); an id nobody knows is
    CHECK(!isGhost("NEW1", st, parked) && !isGhost("C1", st, parked) && isGhost("GONE1", st, parked));
    // a parked id never gets a slot at runtime (it would start from defaults); a Forget that frees the slot
    // restarts, so the boot loader admits it with its stored config and key
    CHECK(!admitStation(st, parked, "C1", defaultStationConfig(), MAX_STATIONS) && admitStation(st, parked, "NEW1", defaultStationConfig(), MAX_STATIONS) == st.at(0));
    CHECK(st.forget(0) && !admitStation(st, parked, "C1", defaultStationConfig(), MAX_STATIONS) && forgetNeedsRestart(parked));
    Station* fresh = admitStation(st, parked, "FRESH1", defaultStationConfig(), MAX_STATIONS);  // a new id takes the slot
    CHECK(fresh && strcmp(fresh->id, "FRESH1") == 0);
    ParkedList empty = {};
    CHECK(!forgetNeedsRestart(empty) && isGhost("C1", st, empty));
  }
  {  // the turned-away console, noted, shown for an hour after its last try, cleared by a Forget
    NoSlotNote n = {};
    CHECK(n.ms == 0 && n.id[0] == 0);
    n.note("KNOCK1", 0);  // at millis 0 the note still counts (ms | 1)
    CHECK(strcmp(n.id, "KNOCK1") == 0 && n.ms == 1);
    n.note("KNOCK1", 1000);
    n.expire(1000 + NoSlotNote::SHOW_MS - 2);
    CHECK(strcmp(n.id, "KNOCK1") == 0);
    n.expire(1001 + NoSlotNote::SHOW_MS);  // exactly 3 600 000 ms after the last try (ms = 1001)
    CHECK(n.id[0] == 0 && n.ms == 0);
    n.note("KNOCK1", 5000);
    n.note("KNOCK2", 6000);  // a second console replaces the first
    CHECK(strcmp(n.id, "KNOCK2") == 0 && n.ms == (6000u | 1));
    n.clear();
    CHECK(n.id[0] == 0 && n.ms == 0);
    n.note("KNOCK3", 0xFFFFF000u);  // millis() wraps
    n.expire(0xFFFFF000u + 60000u);
    CHECK(strcmp(n.id, "KNOCK3") == 0);
  }
  {  // one station: nothing to forget, no choice, no restart
    const char* one[1] = {"ST1"};
    const FwdEnablePlan p = fwdEnablePlan(one, 1, nullptr);
    CHECK(p.ok && p.forget == -1);
    BridgeConfig b = defaultBridgeConfig();
    Errors e;
    const char* on = "{\"forwarding\":true}";
    CHECK(bridgeFromJson(on, strlen(on), &b, &e, 1) && e.n == 0 && b.forwarding && b.max_stations == 1 && !b.lan_uploads);
  }
}

// a Chatter line with the detailed log off reaches no sink (neither the UART nor the ring),
// an Event line always does; a repeating Event line: a spell's first, one per 10 min with the count, the spell
// ends after 10 min without the event
static void testLogRules() {
  CHECK(logKeeps(false, false) && logKeeps(false, true) && !logKeeps(true, false) && logKeeps(true, true));
  RateLimitedLine l = {};
  uint32_t n = 99, m = 99;
  char sx[32];
  const uint32_t t0 = 5000;
  CHECK(l.hit(t0, &n, &m) && n == 1 && m == 0 && rateSuffix(sx, sizeof sx, n, m)[0] == 0);  // the first line of a spell
  int printed = 0;
  for (uint32_t t = t0 + 10000; t < t0 + RateLimitedLine::WINDOW_MS; t += 10000) printed += l.hit(t, &n, &m);  // MQTT retries
  CHECK(printed == 0 && l.held == 59);
  CHECK(l.hit(t0 + RateLimitedLine::WINDOW_MS, &n, &m) && n == 60 && m == 10);  // the 10-min line with the count
  CHECK(std::string(rateSuffix(sx, sizeof sx, n, m)) == " (x60 in 10 min)");
  CHECK(!l.hit(t0 + RateLimitedLine::WINDOW_MS + 10000, &n, &m));
  // the spell ends after 10 min without the event: the next one is a first line again
  const uint32_t t1 = t0 + RateLimitedLine::WINDOW_MS + 10000 + RateLimitedLine::WINDOW_MS;
  CHECK(l.hit(t1, &n, &m) && n == 1 && m == 0);
  CHECK(!l.hit(t1 + RateLimitedLine::WINDOW_MS - 1, &n, &m));  // inside: still the same spell
  // the MQTT state lines. A 15-min outage, one DISCONNECTED per 10-s retry (the ERROR only keeps its
  // detail): "disconnected", one 10-min line counting the retries, "connected" = 3 lines, the last one the true state
  {
    MqttStateLine q = {};
    const uint32_t b = 1000000;
    CHECK(q.event(true, b - 300000, &n, &m) == MqttStateLine::Change);  // the boot's connect
    int lines = 0, repeats = 0;
    for (uint32_t t = 0; t <= 15 * 60000; t += 10000) {
      const MqttStateLine::Out o = q.event(false, b + t, &n, &m);
      lines += o != MqttStateLine::None;
      if (o == MqttStateLine::Repeat) {
        repeats++;
        CHECK(n == 60 && m == 10);  // retries, not events
      }
    }
    CHECK(q.event(true, b + 15 * 60000 + 20000, &n, &m) == MqttStateLine::Change);
    CHECK(lines + 1 == 3 && repeats == 1 && q.printed_up);
    bool su0;
    uint32_t sn0;
    CHECK(!q.settle(b + 20 * 60000, &sn0, &su0));  // nothing held: no settle line
  }
  {  // up at 0:05, Mosquitto restarted at 3:00 (down), back at 3:15 -- the reconnect prints
    MqttStateLine q = {};
    CHECK(q.event(true, 5 * 60000, &n, &m) == MqttStateLine::Change);
    CHECK(q.event(false, 180 * 60000, &n, &m) == MqttStateLine::Change);
    CHECK(q.event(true, 180 * 60000 + 15 * 1000, &n, &m) == MqttStateLine::Change && q.printed_up);
    CHECK(q.event(false, 181 * 60000, &n, &m) == MqttStateLine::Change && !q.printed_up);  // 3 changes in 10 min: printed
  }
  {  // a flap within 10 min: 3 changes print, the rest are held; one minute of a stable state settles on the truth
    MqttStateLine q = {};
    int printedQ = 0;
    bool up = true;
    for (int i = 0; i < 9; i++, up = !up) printedQ += q.event(up, 2000000 + i * 10000u, &n, &m) == MqttStateLine::Change;
    // the last event (i = 8) was "up": the true state is up, the last printed line (the 3rd change) was "up" too, but
    // six more changes were held
    CHECK(printedQ == 3 && q.held == 6 && q.cur_up);
    bool su = false;
    uint32_t sn = 0;
    CHECK(!q.settle(2000000 + 8 * 10000u + MqttStateLine::STABLE_MS - 1, &sn, &su));  // not stable for a minute yet
    CHECK(q.settle(2000000 + 8 * 10000u + MqttStateLine::STABLE_MS, &sn, &su) && sn == 6 && su && q.printed_up == q.cur_up);
    CHECK(!q.settle(2000000 + 8 * 10000u + 3 * MqttStateLine::STABLE_MS, &sn, &su));  // once
    // ending down instead: the settle line says down
    MqttStateLine d = {};
    up = true;
    for (int i = 0; i < 6; i++, up = !up) d.event(up, 3000000 + i * 10000u, &n, &m);
    CHECK(!d.cur_up && d.settle(3000000 + 5 * 10000u + MqttStateLine::STABLE_MS, &sn, &su) && !su && sn == 3);
    // after the window a change prints again
    CHECK(d.event(true, 3000000 + RateLimitedLine::WINDOW_MS + 60000, &n, &m) == MqttStateLine::Change);
  }
  {  // nothing held: settle is quiet
    MqttStateLine q = {};
    bool su;
    uint32_t sn;
    q.event(false, 10, &n, &m);
    CHECK(!q.settle(10 + 10 * MqttStateLine::STABLE_MS, &sn, &su));
  }
  // millis() wraps inside a spell
  RateLimitedLine w = {};
  CHECK(w.hit(0xFFFFF000u, &n, &m) && !w.hit(0xFFFFF000u + 60000u, &n, &m) && w.hit(0xFFFFF000u + RateLimitedLine::WINDOW_MS, &n, &m) && n == 2);
  // the held count saturates (a fault every millisecond for 10 min)
  RateLimitedLine f = {};
  f.hit(0, &n, &m);
  for (uint32_t t = 1; t < RateLimitedLine::WINDOW_MS; t += 5) f.hit(t, &n, &m);
  CHECK(f.held == 65535 && f.hit(RateLimitedLine::WINDOW_MS + 1, &n, &m) && n == 65536);
  // a failing source's scheduled jobs (a target sending every 5 min to a service down for 3 h): the first failure, one
  // count line per hour, the first success after it -- 4 Event lines instead of 36; a success outside a spell is chatter
  {
    FailSpell s = {};
    const uint32_t b = 7000, step = 300000, H = FailSpell::REMIND_MS;
    CHECK(!s.ok(b - step));  // a working target: chatter
    int events = 0, counts = 0;
    for (uint32_t t = 0; t < 3 * H; t += step)
      if (s.fail(b + t, &n, &m)) events++, counts += n > 1 && m == 60 && n == H / step;
    CHECK(events == 3 && counts == 2);  // the spell's first + the 60-min lines with x12
    CHECK(s.ok(b + 3 * H) && !s.ok(b + 3 * H + step));  // back: one Event, then chatter again
    CHECK(s.fail(b + 3 * H + 2 * step, &n, &m) && n == 1);  // a new spell starts with a first line
    // a target switched off during a spell: an hour without a failure ends it; nor is a success then an Event
    FailSpell q = {};
    CHECK(q.fail(b, &n, &m) && !q.fail(b + step, &n, &m));
    CHECK(q.fail(b + step + H, &n, &m) && n == 1 && m == 0);
    FailSpell z = {};
    CHECK(z.fail(b, &n, &m) && !z.ok(b + H));
  }
  // the AP lines -- joined <-> left always prints (the log ends on the true state), a repeat of one
  // direction is limited, the lease prints once after a printed "joined" and never on its own
  {
    const uint8_t A[6] = {0x24, 0x6f, 0x28, 1, 2, 3}, B[6] = {0x24, 0x6f, 0x28, 1, 2, 4};
    const uint32_t W = RateLimitedLine::WINDOW_MS;
    ApLine a = {};
    CHECK(a.event(true, A, 1000, &n, &m) == ApLine::Change && a.lease() && !a.lease());
    CHECK(a.event(false, A, 2000, &n, &m) == ApLine::Change && !a.lease());
    CHECK(a.event(true, A, 3000, &n, &m) == ApLine::Change && a.lease());  // a quick rejoin is not swallowed
    // the same client, the same direction again: the first of the spell printed above, then held, then the count line
    CHECK(a.event(true, A, 60000, &n, &m) == ApLine::None && !a.lease());
    CHECK(a.event(true, A, 3000 + W - 1, &n, &m) == ApLine::None);
    CHECK(a.event(true, A, 3000 + W, &n, &m) == ApLine::Repeat && n == 3 && a.lease());
    // a repeated join (no "left" seen) is held, so is its lease; the first after the quiet window prints plain
    ApLine j = {};
    CHECK(j.event(true, A, 0, &n, &m) == ApLine::Change && j.lease());
    CHECK(j.event(true, A, 60000, &n, &m) == ApLine::None && !j.lease());
    CHECK(j.event(true, A, 60000 + W, &n, &m) == ApLine::Change && n == 1 && j.lease());
    // a second client (another MAC) joining 2 min after the first is a change, printed with its lease (it was held as
    // a repeat when ApLine kept one direction for the whole AP); its leave and the first client's leave print too
    ApLine p = {};
    CHECK(p.event(true, A, 0, &n, &m) == ApLine::Change && p.lease());
    CHECK(p.event(true, B, 120000, &n, &m) == ApLine::Change && p.lease());
    CHECK(p.event(false, B, 180000, &n, &m) == ApLine::Change && !p.lease());
    CHECK(p.event(false, A, 240000, &n, &m) == ApLine::Change && !p.joined && p.held == 0);
    // a console flapping every 15 s for 5 min: FLAP_LINES changes print, the rest are held, and the heartbeat's settle
    // prints one line with the true state once it has been stable for a minute (the log still ends on that state)
    ApLine fl = {};
    int lines = 0;
    bool up = true;
    for (int i = 0; i < 21; i++, up = !up) lines += fl.event(up, A, 1000 + (uint32_t)i * 15000u, &n, &m) != ApLine::None;
    const uint32_t last = 1000 + 20 * 15000u;
    CHECK(lines == ApLine::FLAP_LINES && fl.held == 21 - ApLine::FLAP_LINES && fl.joined);
    bool sj = false;
    uint8_t sm[6] = {};
    CHECK(!fl.settle(last + ApLine::STABLE_MS - 1, &n, &sj, sm));
    CHECK(fl.settle(last + ApLine::STABLE_MS, &n, &sj, sm) && n == 17 && sj && memcmp(sm, A, 6) == 0);
    CHECK(!fl.settle(last + 2 * ApLine::STABLE_MS, &n, &sj, sm));  // once
    CHECK(fl.event(false, A, 1000 + W, &n, &m) == ApLine::Change);   // a new window: changes print again
    // no flap held: settle never prints (the old behaviour of a quiet AP)
    CHECK(!p.settle(10 * W, &n, &sj, sm) && !j.settle(10 * W, &n, &sj, sm));
  }
}

static void testRose() {
  CHECK(roseSector(0) == 0 && roseSector(22.4f) == 0 && roseSector(22.5f) == 1 && roseSector(337.4f) == 7 &&
        roseSector(337.5f) == 0 && roseSector(359.9f) == 0 && roseSector(180) == 4 && roseSector(-45) == 7);
  static Hourly hrs;
  static Daily d;
  hrs = Hourly();
  d = Daily();
  PosixTz tz;
  parsePosixTz("UTC0", &tz);
  const uint32_t t0 = (uint32_t)unixFromUtc(2026, 11, 1, 0, 0, 0);
  for (int h = 0; h < 25; h++) {
    HourRec r = hourRec(t0 + h * 3600, 5, 10, 0);
    r.wdir_steady = h < 20 ? packDir(270, 127) : h < 23 ? packDir(10, 127) : packDir(DIR_NONE, 0);
    sealHour(&r);
    hrs.pushRaw(r);
    d.addHour(r, hrs, tz);  // the 25th hour closes 1 Nov
  }
  CHECK(d.size() == 1);
  const DayRec& rec = d.at(0);
  CHECK(rec.rose[6] == 20 && rec.rose[0] == 3 && rec.rose[2] == 0 && rec.calm_hours == 1);
  std::string s;
  char buf[128];
  Out o(toString, &s, buf, sizeof buf);
  renderDaily(o, "ST", d, nullptr, 0, RowView{0, nullptr, 0});
  o.flush();
  CHECK(contains(s, "\"rose\":[3,0,0,0,0,0,20,0]"));
}

static void testConsoleRing() {
  char line[CONSOLE_LINE_MAX];
  redactRequestLine("GET", "/weatherstation/updateweatherstation.php?ID=ST1&PASSWORD=sekret&tempf=50&id2=x", line,
                    sizeof line);
  CHECK(strcmp(line, "GET /weatherstation/updateweatherstation.php?ID=***&PASSWORD=***&tempf=50&id2=x") == 0);
  redactRequestLine("GET", "/x?%49D=a&password=b&Pass=c", line, sizeof line);
  CHECK(strcmp(line, "GET /x?%49D=***&password=***&Pass=c") == 0);
  redactRequestLine("GET", "/plain", line, sizeof line);
  CHECK(strcmp(line, "GET /plain") == 0);
  char tiny[24];
  redactRequestLine("GET", "/x?a=1&PASSWORD=verylongsecretvalue", tiny, sizeof tiny);
  CHECK(strlen(tiny) == sizeof tiny - 1 && !contains(tiny, "very"));  // truncation never leaks a secret
  static ConsoleRing r;
  CHECK(r.first("A") == 0 && r.last("A") == 0 && r.size() == 0);
  for (int i = 1; i <= 7; i++) {  // one ring for all stations: A and B alternate
    char l[16];
    snprintf(l, sizeof l, "L%d", i);
    r.push(i % 2 ? "A" : "B", l, 1000 + i, (uint32_t)(i + 1) / 2);
  }
  // kept: L2..L7 = B:1 A:2 B:2 A:3 B:3 A:4
  CHECK(r.size() == CONSOLE_LINES && r.first("A") == 2 && r.last("A") == 4 && r.first("B") == 1 && r.last("B") == 3);
  std::string longLine(600, 'x');
  r.push("A", longLine.c_str(), 2000, 5);  // stored lines are cut to CONSOLE_STORE_MAX - 1 bytes
  CHECK(strlen(r.at(r.size() - 1).text) == CONSOLE_STORE_MAX - 1);
  std::string s;
  char buf[64];
  Out o(toString, &s, buf, sizeof buf);
  renderConsole(o, r, "A", 3);
  o.flush();
  CHECK(contains(s, "{\"first\":2,\"last\":5,\"items\":[{\"seq\":4,\"t\":1007,\"line\":\"L7\"},{\"seq\":5,"));
  CHECK(!contains(s, "L4") && !contains(s, "L6"));  // B's lines stay out of A's console
  r.forget("B");
  std::string sb;
  Out ob(toString, &sb, buf, sizeof buf);
  renderConsole(ob, r, "B", 0);
  ob.flush();
  CHECK(sb == "{\"first\":0,\"last\":0,\"items\":[]}");
}

static void testMetricsEtag() {
  MinuteWindow mn(MinuteWindow::Kind::Min), mx(MinuteWindow::Kind::Max);
  uint32_t v = 0;
  CHECK(!mn.value(0, &v));
  mn.feed(0, 100000);
  mn.feed(30000, 55000);
  mn.feed(60000, 90000);
  CHECK(mn.value(60000, &v) && v == 55000);
  CHECK(mn.value(9 * 60000 + 59000, &v) && v == 55000);  // minute 0 still inside the 10-minute window
  CHECK(mn.value(10 * 60000, &v) && v == 90000);         // minute 0 aged out
  CHECK(!mn.value(11 * 60000, &v));
  mx.feed(5000, 12);
  mx.feed(6000, 40);
  mx.feed(7000, 3);
  CHECK(mx.value(8000, &v) && v == 40);
  // a shorter span reads only the last `minutes` slots; older slots still count for the full window
  MinuteWindow m3(MinuteWindow::Kind::Min);
  m3.feed(0, 5000);                  // minute 0: the deep dip
  m3.feed(60000, 30000);             // minute 1
  m3.feed(4 * 60000, 40000);         // minute 4
  m3.feed(5 * 60000 + 1000, 35000);  // minute 5
  m3.feed(6 * 60000, 45000);         // minute 6
  CHECK(m3.value(6 * 60000 + 30000, &v, 3) && v == 35000);  // minutes 4..6
  CHECK(m3.value(6 * 60000 + 30000, &v) && v == 5000);      // the default stays 10 minutes
  CHECK(m3.value(6 * 60000 + 30000, &v, 10) && v == 5000);
  CHECK(m3.value(6 * 60000 + 30000, &v, 1) && v == 45000);
  CHECK(m3.value(6 * 60000 + 30000, &v, 0) && v == 45000);   // clamped up to 1
  CHECK(m3.value(6 * 60000 + 30000, &v, -5) && v == 45000);
  CHECK(m3.value(6 * 60000 + 30000, &v, 99) && v == 5000);   // clamped down to MINUTES
  CHECK(m3.value(3 * 60000, &v, 3) && v == 30000);           // minutes 1..3: minute 0 just left the short span
  CHECK(m3.value(2 * 60000 + 59999, &v, 3) && v == 5000);    // minute 0 still inside minutes 0..2
  CHECK(!m3.value(9 * 60000, &v, 3));                        // minutes 7..9 empty
  CHECK(m3.value(9 * 60000, &v) && v == 5000);               // the full window still holds the dip
  CHECK(m3.value(10 * 60000, &v) && v == 30000);             // minute 0 aged out of the full window
  CHECK(HEAP_LOW_MINUTES == 3 && HEAP_LOW_MINUTES <= MinuteWindow::MINUTES);
  char et[48];
  makeEtag(et, 0xdeadbeef, 1, 0x2a, 3);
  CHECK(strcmp(et, "\"deadbeef-1-2a-3\"") == 0);
  CHECK(etagMatches(et, et) && etagMatches("\"x\", W/\"deadbeef-1-2a-3\"", et) && etagMatches("*", et));
  CHECK(!etagMatches(nullptr, et) && !etagMatches("", et) && !etagMatches("\"deadbeef-1-2a-4\"", et));
}

static void testBridgeStatusAndLiveOne() {
  static Stations all;
  all.reserve(MAX_STATIONS);
  StationConfig cfg = defaultStationConfig();
  StationConfig named = cfg;
  strcpy(named.name, "Garden");
  StationConfig off = named;
  off.publish = false;
  PosixTz tz;
  parsePosixTz("UTC0", &tz);
  Station* a = all.findOrAdd("ALPHA00001", named);
  // one Stations each (the WROOM-32 profile has one slot); b, c and d only for their state letters
  static Stations sb, sc, sd;
  sb.reserve(MAX_STATIONS), sc.reserve(MAX_STATIONS), sd.reserve(MAX_STATIONS);
  Station* b = sb.findOrAdd("BRAVO00002", named);
  Station* c = sc.findOrAdd("CHARLIE003", off);
  Station* d = sd.findOrAdd("DELTA00004", cfg);
  CHECK(a && b && c && d);
  Stations::ingest(*a, sampleFrom("ID=ALPHA00001&tempf=50"), 1000, 1000000, 0, SrcIf::Ap, false, tz);
  Stations::ingest(*b, sampleFrom("ID=BRAVO00002&tempf=50"), 1000, 1000, 0, SrcIf::Ap, false, tz);
  CHECK(stationStateChar(*a, 1000000) == 'o' && stationStateChar(*b, 1000000) == 's' &&
        stationStateChar(*c, 1000000) == 'x' && stationStateChar(*d, 1000000) == 'u');
  BridgeStatus bs = {};
  bs.mode = "run";
  bs.sta = true;
  bs.mqtt = 1;
  bs.time = true;
  const Station* two[2] = {a, b};
  for (int i = 0; i < MAX_STATIONS; i++) {
    strcpy(bs.ids[bs.n], two[i]->id);
    bs.state[bs.n++] = stationStateChar(*two[i], 1000000);
  }
  std::string s;
  char buf[64];
  Out o(toString, &s, buf, sizeof buf);
  renderBridgeStatus(o, bs);
  o.flush();
  printf("  info: bridge/status with %d station(s) = %u bytes\n", MAX_STATIONS, (unsigned)s.size());
#if FC_MAX_STATIONS > 1
  CHECK(s == "{\"mode\":\"run\",\"sta\":1,\"mqtt\":1,\"time\":1,\"lock\":0,\"heap_low\":0,\"time_src\":\"none\",\"ids\":[\"ALPHA00001\","
             "\"BRAVO00002\"],\"st\":\"os\"}");
#else
  CHECK(s == "{\"mode\":\"run\",\"sta\":1,\"mqtt\":1,\"time\":1,\"lock\":0,\"heap_low\":0,\"time_src\":\"none\",\"ids\":[\"ALPHA00001\"],"
             "\"st\":\"o\"}");
#endif
  {  // the console turned away for lack of a slot, named next to the count
    BridgeStatus ns = bs;
    ns.no_slot = 3;
    strcpy(ns.no_slot_id, "KNOCK0001");
    std::string sn;
    Out on2(toString, &sn, buf, sizeof buf);
    renderBridgeStatus(on2, ns);
    on2.flush();
    CHECK(contains(sn, "\"no_slot\":3,\"no_slot_id\":\"KNOCK0001\",") && jsonParses(sn) && !contains(s, "no_slot_id"));
  }
  {  // the airport tab's flag, only while set
    BridgeStatus ms = bs;
    ms.metar = true;
    std::string sm;
    Out om(toString, &sm, buf, sizeof buf);
    renderBridgeStatus(om, ms);
    om.flush();
    CHECK(contains(sm, "\"metar\":1,") && jsonParses(sm) && !contains(s, "metar"));
  }
  // worst case: two 32-char ids, every problem count at its widest, an OTA running, a turned-away 32-char id, the
  // airport tab's flag
  bs.store_fail = bs.too_long = bs.no_slot = 4294967295u;
  bs.metar = true;
  memset(bs.no_slot_id, 'K', STATION_ID_LEN - 1);
  bs.no_slot_id[STATION_ID_LEN - 1] = 0;
  bs.ota = true;
  for (int i = 0; i < MAX_STATIONS; i++) {
    memset(bs.ids[i], 'W', STATION_ID_LEN - 1);
    bs.ids[i][STATION_ID_LEN - 1] = 0;
  }
  std::string w;
  Out ow(toString, &w, buf, sizeof buf);
  renderBridgeStatus(ow, bs);
  ow.flush();
  printf("  info: bridge/status worst case = %u bytes\n", (unsigned)w.size());
  CHECK(w.size() <= 300 || STATION_ID_LEN > 48);
  // no station yet: the AP name for the pairing card, never otherwise
  BridgeStatus none = {};
  none.mode = "run";
  none.ap = "fauxcloud-A1B2";
  std::string es;
  Out oe(toString, &es, buf, sizeof buf);
  renderBridgeStatus(oe, none);
  oe.flush();
  CHECK(contains(es, "\"heap_low\":0,\"time_src\":\"none\",\"ap\":\"fauxcloud-A1B2\",\"ids\":[],\"st\":\"\"}") && !contains(s, "\"ap\""));
  // one-station live with a promotion by type; the AP face has no diag
  a->cfg.promote[0] = Promote{};
  Errors e;
  a->cfg = mergeStation("{\"name\":\"Garden\",\"promote\":{\"soilmoisture\":{\"name\":\"Bed\",\"type\":\"soil_moisture\"}}}", &e);
  Stations::ingest(*a, sampleFrom("ID=ALPHA00001&tempf=50&soilmoisture=41"), 1001, 1001000, 0, SrcIf::Ap, false, tz);
  static LiveSnap snap;
  snapLive(*a, 1001000, &snap);
  std::string live;
  Out ol(toString, &live, buf, sizeof buf);
  Clock clk{1001, false, 1001, 1001000};
  renderLiveOne(ol, snap, clk, nullptr);
  ol.flush();
  CHECK(contains(live, "\"station\":{\"id\":\"ALPHA00001\",\"name\":\"Garden\""));
  CHECK(contains(live, "\"promoted\":{\"bed\":{\"v\":41,\"name\":\"Bed\",\"type\":\"soil_moisture\",\"unit\":\"%\","
                       "\"group\":\"garden\",\"icon\":\"sprout\",\"span\":10,\"max\":100,\"dec\":0,\"age\":0}}"));
  CHECK(!contains(live, "\"diag\"") && !contains(live, "uptime_s") && !contains(live, "\"heap"));

  // a configured promotion is always there, "v": null until a value arrives
  Stations::ingest(*a, sampleFrom("ID=ALPHA00001&tempf=51"), 1002, 1002000, 0, SrcIf::Ap, false, tz);
  snapLive(*a, 1002000, &snap);
  std::string nullv;
  Out on(toString, &nullv, buf, sizeof buf);
  renderLiveOne(on, snap, clk, nullptr);
  on.flush();
  // the value is carried, with its age (seconds before the last upload)
  CHECK(contains(nullv, "\"promoted\":{\"bed\":{\"v\":41,\"name\":\"Bed\"") && contains(nullv, "\"dec\":0,\"age\":1}"));
  CHECK(contains(live, "\"hidden\":[]"));
  // the hidden fields travel in the station head, so the page drops them from history and the CSV as well
  a->cfg.disabled = (1u << F_DEWPT_C) | (1u << F_UV);
  snapLive(*a, 1002000, &snap);
  std::string hid;
  Out oh(toString, &hid, buf, sizeof buf);
  renderLiveOne(oh, snap, clk, nullptr);
  oh.flush();
  CHECK(contains(hid, "\"hidden\":[\"dewpt_c\",\"uv\"]"));
  // hidden = recorded but not published: omitted from /api/live and the history views
  // too, still recorded -- un-hidden it is back everywhere and the history has no gap
  Stations::ingest(*a, sampleFrom("ID=ALPHA00001&tempf=52&dewptf=41&UV=4"), (uint32_t)unixFromUtc(2026, 11, 2, 12, 0, 0), 1003000, 0,
                   SrcIf::Ap, true, tz);  // synced: the hourly record opens
  snapLive(*a, 1003000, &snap);
  std::string hl, hr, hh;
  Out ohl(toString, &hl, buf, sizeof buf);
  renderLiveOne(ohl, snap, clk, nullptr);
  ohl.flush();
  CHECK(!contains(hl, "\"dewpt_c\":") && !contains(hl, "\"uv\":") && contains(hl, "\"temp_c\":"));
  Out ohr(toString, &hr, buf, sizeof buf);
  renderRecent(ohr, *a);
  ohr.flush();
  CHECK(!contains(hr, "dewpt_c") && !contains(hr, "\"uv\"") && contains(hr, "temp_c"));
  Out ohh(toString, &hh, buf, sizeof buf);
  renderHourly(ohh, *a);
  ohh.flush();
  CHECK(!contains(hh, "\"dewpt_c\":") && !contains(hh, "\"uv\":") && contains(hh, "\"temp_c\":"));
  {
    HourRec orec;
    CHECK(a->hourly.openRecord(&orec) && orec.td_avg != I16_NONE && orec.uv_max != U8_NONE);  // still recorded
  }
  a->cfg.disabled = 0;
  snapLive(*a, 1003000, &snap);
  std::string un;
  Out oun(toString, &un, buf, sizeof buf);
  renderLiveOne(oun, snap, clk, nullptr);
  oun.flush();
  CHECK(contains(un, "\"dewpt_c\":") && contains(un, "\"uv\":4") && contains(un, "\"hidden\":[]"));

  // migration: a stored promotion whose key is now a built-in field or a protocol flag is dropped with its key
  Errors st;
  const StationConfig mc = mergeStation("{\"promote\":{\"solarradiation\":{\"name\":\"Light level\",\"type\":\"illuminance\"},"
                                        "\"action\":{\"name\":\"Something else\"},\"soiltempf\":{\"name\":\"Soil\",\"type\":\"soil_temperature\"}}}", &st);
  CHECK(st.n_stale == 2 && strcmp(st.stale_keys[0], "light_level") == 0 && strcmp(st.stale_keys[1], "something_else") == 0);
  CHECK(st.n == 2 && contains(st.at(0), "promotion light_level dropped: built in now") &&
        contains(st.at(1), "promotion something_else dropped: protocol flag"));
  CHECK(mc.promote[0].used && strcmp(mc.promote[0].param, "soiltempf") == 0 && !mc.promote[1].used);  // the rest stays
}

// a reading taken before SNTP (seconds since boot) gets its unix time at the first sync
static void testRestampAtSync() {
  static Stations all;
  all.reserve(MAX_STATIONS);
  PosixTz tz;
  parsePosixTz("UTC0", &tz);
  Station* s = all.findOrAdd("RS1", defaultStationConfig());
  CHECK(s != nullptr);
  Stations::ingest(*s, sampleFrom("ID=RS1&tempf=50&windspeedmph=5"), 4, 4000, 0, SrcIf::Ap, false, tz);  // 4 s after boot
  const uint32_t boot = (uint32_t)unixFromUtc(2026, 11, 2, 20, 0, 0);
  Stations::ingest(*s, sampleFrom("ID=RS1&tempf=51&windspeedmph=6"), boot + 70, 70000, 0, SrcIf::Ap, true, tz);
  CHECK(s->recent.size() == 2 && s->recent.at(0).t == 4 && s->recent.at(1).t == boot + 70);
  CHECK(Stations::clockSynced(*s, boot) == 1);
  CHECK(s->recent.at(0).t == boot + 4 && s->recent.at(1).t == boot + 70);  // in order, both real
  CHECK(Stations::clockSynced(*s, boot) == 0);                               // once
  std::string j;
  char b[256];
  Out o(toString, &j, b, sizeof b);
  renderRecent(o, *s);
  o.flush();
  char want[32];
  snprintf(want, sizeof want, "[%lu,", (unsigned long)(boot + 4));
  CHECK(contains(j, want) && !contains(j, "[4,"));
}

// a browser seed 10 min fast opens hour 10 at real 09:55; SNTP corrects to 09:56: the late readings
// join hour 10, the RAM ring stays strictly increasing, no duplicate keys
static void testHistBack() {
  static Stations all;
  all.reserve(MAX_STATIONS);
  PosixTz tz;
  parsePosixTz("UTC0", &tz);
  Station* s = all.findOrAdd("HB1", defaultStationConfig());
  const uint32_t h9 = (uint32_t)unixFromUtc(2026, 11, 12, 9, 0, 0);
  uint32_t t = h9 - 2 * 3600;
  for (; t < h9 + 55 * 60; t += 65) {  // real time; the seed: +10 min
    Stations::ingest(*s, sampleFrom("ID=HB1&tempf=50"), t + 600, 1000, 0, SrcIf::Ap, true, tz);
    Stations::tick(*s, t + 600, tz);
  }
  CHECK(s->hourly.acc().hour == h9 + 3600);  // hour 10 open at real 09:55
  int late = 0;
  for (; t < h9 + 3 * 3600; t += 65) {  // SNTP: the real time from here
    late += t < h9 + 3600;
    Stations::ingest(*s, sampleFrom("ID=HB1&tempf=51"), t, 1000, 0, SrcIf::Ap, true, tz);
    Stations::tick(*s, t, tz);
  }
  bool inc = true;
  for (int i = 1; i < s->hourly.size(); i++) inc = inc && s->hourly.at(i).hour > s->hourly.at(i - 1).hour;
  CHECK(inc && late >= 4 && s->hourly.back() == (uint32_t)late);
  for (int i = 1; i < s->daily.size(); i++) inc = inc && s->daily.at(i).ymd > s->daily.at(i - 1).ymd;
  CHECK(inc);
  // with no hour open, a reading earlier than the newest closed hour stays out of history
  Hourly h;
  HourIn in = {};
  in.t = h9 + 3600 + 30;
  h.add(in);
  h.tick(h9 + 2 * 3600 + 5);  // hour 10 closed, none open
  in.t = h9 + 30;             // an hour-9 reading after it
  CHECK(!h.add(in) && h.size() == 1 && h.back() == 1 && !h.acc().hour);
}

static void testLiveEtagOnline() {
  static Stations all;
  all.reserve(MAX_STATIONS);
  PosixTz tz;
  parsePosixTz("UTC0", &tz);
  Station* s = all.findOrAdd("ET1", defaultStationConfig());
  CHECK(s != nullptr);
  Stations::ingest(*s, sampleFrom("ID=ET1&tempf=50"), 1000, 1000000, 0, SrcIf::Ap, false, tz);
  char fresh[48], stale[48], ap[48], again[48];
  liveEtag(fresh, 7, *s, 1000000 + 1000, false, true);
  liveEtag(stale, 7, *s, 1000000 + s->cfg.stale_s * 1000u + 1000, false, true);  // no upload for stale_s: offline
  liveEtag(ap, 7, *s, 1000000 + 1000, true, true);
  CHECK(strcmp(fresh, stale) != 0 && strcmp(fresh, ap) != 0 && strcmp(stale, ap) != 0);
  Stations::ingest(*s, sampleFrom("ID=ET1&tempf=51"), 2000, 1000000 + s->cfg.stale_s * 1000u + 2000, 0, SrcIf::Ap, false, tz);
  liveEtag(again, 7, *s, 1000000 + s->cfg.stale_s * 1000u + 3000, false, true);
  CHECK(strcmp(again, stale) != 0 && strcmp(again, fresh) != 0);  // back online with a new upload

  // a station restored from flash after a reboot, no upload yet: the first SNTP sync and the restore each change
  // the ETag (else a dashboard stays on "No readings yet" through 304s)
  static Stations allET2;  // a slot of its own (one station per Stations on the WROOM-32 profile)
  allET2.reserve(MAX_STATIONS);
  Station* r = allET2.findOrAdd("ET2", defaultStationConfig());
  CHECK(r != nullptr);
  char unsynced[48], synced[48], restored[48];
  liveEtag(unsynced, 7, *r, 5000, false, false);
  liveEtag(synced, 7, *r, 5000, false, true);
  r->flash_rev++;
  liveEtag(restored, 7, *r, 5000, false, true);
  CHECK(strcmp(unsynced, synced) != 0 && strcmp(synced, restored) != 0 && strcmp(unsynced, restored) != 0);

  // the outlook block changes with an hour close, a calibration entry, the location or a real
  // clock without an upload; each changes the live ETag
  const uint32_t in0 = outlookEtagIn(*r, 52.f, 20.f, true);
  char e0[48], e1[48];
  liveEtag(e0, 7, *r, 5000, false, true, in0);
  CHECK(outlookEtagIn(*r, 52.f, 20.f, false) != in0);
  CHECK(outlookEtagIn(*r, 52.f, 20.5f, true) != in0 && outlookEtagIn(*r, 51.f, 20.f, true) != in0);
  r->hour_closes++;
  const uint32_t in1 = outlookEtagIn(*r, 52.f, 20.f, true);
  CHECK(in1 != in0);
  liveEtag(e1, 7, *r, 5000, false, true, in1);
  CHECK(strcmp(e0, e1) != 0);
  r->cal.n = 1;  // a calibration entry
  r->cal.e[0].t_from = 1;
  r->cal.appends++;
  CHECK(outlookEtagIn(*r, 52.f, 20.f, true) != in1);
}

// ---- flash store ------------------------------------------------------------------------------------------------

// The host has no LittleFS: the store runs on files in a map.
class RamFs : public FileIo {
 public:
  std::map<std::string, std::string> files;
  int writes = 0;
  long size(const char* path) override { auto it = files.find(path); return it == files.end() ? -1 : (long)it->second.size(); }
  bool read(const char* path, size_t off, void* buf, size_t len) override {
    auto it = files.find(path);
    if (it == files.end() || off + len > it->second.size()) return false;
    memcpy(buf, it->second.data() + off, len);
    return true;
  }
  bool write(const char* path, const void* buf, size_t len) override {
    writes++;
    files[path].assign(len ? (const char*)buf : "", len);
    return true;
  }
  bool append(const char* path, const void* buf, size_t len) override { writes++; files[path].append((const char*)buf, len); return true; }
  bool rename(const char* from, const char* to) override {
    auto it = files.find(from);
    if (it == files.end()) return false;
    files[to] = it->second;
    files.erase(from);
    return true;
  }
  bool remove(const char* path) override { files.erase(path); return true; }
  bool format() override { files.clear(); return true; }
  bool writeAt(const char* path, size_t off, const void* buf, size_t len) override {
    auto it = files.find(path);
    if (it == files.end() || off + len > it->second.size()) return false;
    writes++;
    memcpy(&it->second[off], buf, len);
    return true;
  }
  bool writeParts(const char* path, const Part* parts, int n) override {
    writes++;
    std::string& f = files[path];
    f.clear();
    for (int i = 0; i < n; i++) f.append((const char*)parts[i].buf, parts[i].len);
    return true;
  }
  // root entries: "/name" files, and "/dir/..." as one directory entry
  int list(void (*fn)(void* ctx, const char* name, bool dir), void* ctx) override {
    std::vector<std::string> seen;
    int n = 0;
    for (auto& f : files) {
      const std::string rest = f.first.substr(1);
      const size_t slash = rest.find('/');
      const std::string name = slash == std::string::npos ? rest : rest.substr(0, slash);
      bool dup = false;
      for (auto& x : seen) dup = dup || x == name;
      if (dup) continue;
      seen.push_back(name);
      fn(ctx, name.c_str(), slash != std::string::npos);
      n++;
    }
    return n;
  }
  bool removeTree(const char* path) override {
    const std::string pre = std::string(path) + "/";
    for (auto it = files.begin(); it != files.end();) it = it->first.compare(0, pre.size(), pre) == 0 ? files.erase(it) : std::next(it);
    files.erase(path);
    return true;
  }
};

// ---- records, rings, aggregation, rows, format plan, snapshot ----------------------------------------------------

static HourIn hin(uint32_t t, float temp, float wind, float gust, float dir, float rain_delta, bool counter = true) {
  HourIn in;
  memset(&in, 0, sizeof in);
  in.t = t;
  in.wind_cal = NAN;
  in.wind2m = NAN;
  in.rain_delta = rain_delta;
  in.rain_counter = counter;
  in.rain_rate = NAN;
  for (float& c : in.ch) c = NAN;
  auto set = [&](int f, float v) {
    if (!std::isnan(v)) {
      in.v[f] = v;
      in.present |= 1u << f;
    }
  };
  set(F_TEMP_C, temp);
  set(F_WIND_KMH, wind);
  set(F_GUST_KMH, gust);
  set(F_WIND_DIR, dir);
  return in;
}

// A closed hour as a record (values in their units; NAN = none)
static HourRec hourRec(uint32_t hour, float temp, float gust, float rain, uint8_t n) {
  HourRec r;
  clearHour(&r, hour);
  r.n = n;
  if (!std::isnan(temp)) {
    r.t_min = toI16(temp - 1, 10);
    r.t_avg = toI16(temp, 10);
    r.t_max = toI16(temp + 1, 10);
  }
  r.gust_max = toU16(gust, 10);
  r.rain_sum = toU16(rain, 100);
  r.wind_avg = toU16(3, 10);
  r.wdir_steady = packDir(90, 127);
  sealHour(&r);
  return r;
}

// records, CRC, sentinels
static void testRecords() {
  CHECK(sizeof(HourRec) == 48 && sizeof(DayRec) == 88);
  HourRec z;
  memset(&z, 0, sizeof z);
  CHECK(!hourValid(z));
  memset(&z, 0xFF, sizeof z);
  CHECK(!hourValid(z));
  DayRec dz;
  memset(&dz, 0, sizeof dz);
  CHECK(!dayValid(dz));
  memset(&dz, 0xFF, sizeof dz);
  CHECK(!dayValid(dz));
  HourRec h;
  clearHour(&h, 1800000000u);
  h.t_avg = toI16(-3.25f, 10);
  h.rh_avg = toU8(99.6f, 1);
  h.rain_sum = toU16(1.234f, 100);
  sealHour(&h);
  uint8_t bytes[48];
  memcpy(bytes, &h, sizeof bytes);
  HourRec back;
  memcpy(&back, bytes, sizeof back);
  CHECK(hourValid(back) && back.hour == 1800000000u && back.t_avg == -33 && back.rh_avg == 100 && back.rain_sum == 123);
  CHECK(back.t_min == I16_NONE && back.wind_avg == U16_NONE && back.uv_max == U8_NONE && dirOf(back.wdir_steady) == DIR_NONE);
  back.t_avg++;
  CHECK(!hourValid(back));  // one changed byte fails the CRC
  CHECK(toI16(NAN, 10) == I16_NONE && toU16(NAN, 10) == U16_NONE && toU8(NAN, 1) == U8_NONE);
  CHECK(toI16(5000.0f, 10) == 32767 && toU16(-1, 10) == 0 && toU8(300, 1) == 254);  // saturating, never a sentinel
  CHECK(strcmp(qualityName(Q_STALE), "stale") == 0 && strcmp(qualityName(Q_RAIN_PARTIAL), "rain_partial") == 0 &&
        strcmp(qualityName(Q_EPOCH_SPLIT), "epoch_split") == 0);
  CHECK(crc8("123456789", 9) == 0xFB);  // poly 0x07, init 0xFF, not reflected, no final xor (check value computed apart)
}

static void testHourlyAggregate() {
  static Hourly h;
  h = Hourly();
  const uint32_t H0 = (uint32_t)unixFromUtc(2026, 11, 1, 14, 0, 0);
  // 60 readings, one per minute: temp 10..12 (avg 11); wind 10 km/h from 10° and 5 km/h from 350° alternating; gust
  // max 30 at minute 7; rain 0.01 mm per reading after the first (its delta unknown: rain_partial); a rejected field
  for (int i = 0; i < 60; i++) {
    HourIn in = hin(H0 + i * 60, 10.0f + 2.0f * i / 59.0f, (i % 2) ? 10.0f : 5.0f, i == 7 ? 30.0f : 12.0f,
                    (i % 2) ? 10.0f : 350.0f, i == 0 ? NAN : 0.01f);
    if (i == 3) in.flags = 1u << Q_REJECTED;
    CHECK(!h.add(in));
  }
  CHECK(h.size() == 0);
  HourRec open;
  CHECK(h.openRecord(&open) && open.n == 60 && hourValid(open));
  CHECK(!h.tick(H0 + 3599));
  CHECK(h.tick(H0 + 3600));
  CHECK(h.size() == 1);
  const HourRec& r = h.at(0);
  CHECK(r.hour == H0 && r.n == 60 && hourValid(r));
  CHECK(r.quality == ((1u << Q_REJECTED) | (1u << Q_RAIN_PARTIAL)));  // no gap: every minute, both edges inside
  CHECK(r.t_min == 100 && r.t_avg == 110 && r.t_max == 120);
  CHECK(r.wind_avg == 75 && r.gust_max == 300 && r.calm_n == 0);
  // speed-weighted vector mean: 10 x 10° + 5 x 350° -> atan(tan 10° / 3) = 3.4°, R = 0.986
  CHECK(dirOf(r.wdir_steady) == 3 && steadyOf(r.wdir_steady) == 125);
  CHECK(r.rain_sum == 59);  // 59 deltas of 0.01 mm
  CHECK(r.solar_avg == U16_NONE && r.uv_max == U8_NONE && r.p_avg == I16_NONE);

  // a gap of 11 min inside the next hour, and its end edge (the last reading 20 min before the hour ends)
  h.add(hin(H0 + 3600 + 30, 5, 0, 0, 90, 0.11f));
  h.add(hin(H0 + 3600 + 30 + 660, 5, 0, 0, 90, 0.2f));
  CHECK(h.closeNow());
  CHECK(h.size() == 2 && (h.at(1).quality >> Q_GAP & 1u) && h.at(1).rain_sum == 31);
  h.add(hin(H0 + 2 * 3600 + 5, 5, 0, 0, 90, 0));
  h.add(hin(H0 + 2 * 3600 + 3000, 5, 0, 0, 90, 0));  // 600 s before the end: still a gap at the edge
  CHECK(h.closeNow() && (h.at(2).quality >> Q_GAP & 1u));
  // an hour change during add() closes the previous hour; 48 records are kept
  for (int k = 0; k < 60; k++) h.add(hin(H0 + (3 + k) * 3600, 1, 1, 1, 1, 0));
  h.closeNow();
  CHECK(h.size() == HOURLY_LEN && h.at(HOURLY_LEN - 1).hour == H0 + 62 * 3600);
  // the same clock hour closed again (debug roll) replaces the newest record
  h.add(hin(H0 + 62 * 3600 + 10, 30, 1, 1, 1, 0));
  CHECK(h.closeNow() && h.lastCloseReplaced() && h.size() == HOURLY_LEN && h.at(HOURLY_LEN - 1).t_avg == 300);
  // a dropped sample alone leaves no record, only an hour with readings does
  h.addRejected(H0 + 70 * 3600);
  CHECK(!h.closeNow() && h.at(HOURLY_LEN - 1).hour == H0 + 62 * 3600);
}

// a calm night (vane parked at 335°, 0 km/h) and a windy hour: the night has no direction, the rose no NW
static void testCalmNight() {
  static Hourly h;
  h = Hourly();
  PosixTz tz;
  parsePosixTz("UTC0", &tz);
  const uint32_t H0 = (uint32_t)unixFromUtc(2026, 11, 7, 2, 0, 0);
  for (int i = 0; i < 60; i++) h.add(hin(H0 + i * 60, -5, 0, 0, 335, 0));
  for (int i = 0; i < 60; i++) {
    HourIn in = hin(H0 + 3600 + i * 60, -4, 10, 15, 250, 0);
    in.wind2m = 9;
    h.add(in);
  }
  // a reading under 1 km/h whose 2-min mean is still windy is not calm
  HourIn gusty = hin(H0 + 7200, -4, 0.5f, 3, 200, 0);
  gusty.wind2m = 4;
  h.add(gusty);
  h.closeNow();
  CHECK(h.size() == 3);
  CHECK(h.at(0).calm_n == 60 && dirOf(h.at(0).wdir_steady) == DIR_NONE && calmHour(h.at(0)));
  CHECK(h.at(1).calm_n == 0 && dirOf(h.at(1).wdir_steady) == 250 && steadyOf(h.at(1).wdir_steady) == 127 && !calmHour(h.at(1)));
  CHECK(h.at(2).calm_n == 0 && dirOf(h.at(2).wdir_steady) == 200);
  DayRec d;
  CHECK(foldDay(h, localDayNumber(tz, H0), tz, &d) && dayValid(d));
  CHECK(d.hours == 3 && d.calm_hours == 1 && d.rose[6] == 1 && d.rose[4] == 1 && d.rose[7] == 0 && d.n == 121);
  CHECK(dirOf(d.wdir_steady) != DIR_NONE);
}

// the daily fold against a hand-computed day
static void testDailyFold() {
  static Hourly h;
  h = Hourly();
  PosixTz tz;
  parsePosixTz("UTC0", &tz);
  const uint32_t H0 = (uint32_t)unixFromUtc(2026, 11, 4, 6, 0, 0);
  HourRec a, b, c;
  clearHour(&a, H0);
  a.n = 60;
  a.t_min = 10, a.t_avg = 20, a.t_max = 30, a.td_avg = 5, a.p_avg = 10000, a.rh_min = 80, a.rh_avg = 85, a.rh_max = 90;
  a.wind_avg = 20, a.gust_max = 50, a.wdir_steady = packDir(90, 127), a.rain_sum = 10, a.solar_avg = 20, a.solar_max = 40;
  a.uv_max = 10;
  clearHour(&b, H0 + 3600);
  b.n = 30;
  b.quality = 1u << Q_GAP;
  b.t_min = -10, b.t_avg = 0, b.t_max = 10, b.td_avg = -5, b.p_avg = 10020, b.rh_min = 70, b.rh_avg = 75, b.rh_max = 80;
  b.wind_avg = 40, b.gust_max = 90, b.wdir_steady = packDir(0, 127), b.rain_sum = 20, b.rain_rate_max = 7;
  clearHour(&c, H0 + 7200);
  c.n = 60;
  c.calm_n = 50;
  c.t_min = 0, c.t_avg = 10, c.t_max = 20, c.td_avg = 0, c.p_avg = 10010, c.rh_min = 60, c.rh_avg = 65, c.rh_max = 70;
  c.wind_avg = 5, c.gust_max = 20, c.rain_sum = 0;
  for (HourRec* r : {&a, &b, &c}) {
    sealHour(r);
    h.pushRaw(*r);
  }
  DayRec d;
  CHECK(foldDay(h, localDayNumber(tz, H0), tz, &d) && dayValid(d));
  CHECK(d.ymd == 20261104 && d.n == 150 && d.hours == 3 && d.clean_hours == 2 && d.quality == (1u << Q_GAP));
  CHECK(d.t_min == -10 && d.t_avg == 12 && d.t_max == 30);   // (2.0 x 60 + 0 x 30 + 1.0 x 60) / 150 = 1.2
  CHECK(d.td_min == -5 && d.td_avg == 1 && d.td_max == 5);   // of the hourly means
  CHECK(d.p_min == 10000 && d.p_avg == 10008 && d.p_max == 10020);
  CHECK(d.rh_min == 60 && d.rh_avg == 75 && d.rh_max == 90);
  CHECK(d.wind_avg == 18 && d.gust_max == 90 && d.uv_max == 10 && d.rain_rate_max == 7);
  CHECK(d.calm_hours == 1 && d.rose[2] == 1 && d.rose[0] == 1);  // c is calm: no sector
  // R composition: (2.0 at 90°) + (4.0 at 0°) over 6.0 -> 26.6°, R = 0.745
  CHECK(dirOf(d.wdir_steady) == 27 && steadyOf(d.wdir_steady) == 95);
  CHECK(d.rain_sum == 30 && d.rain_hours == 2 && d.solar_avg == 20 && d.solar_max == 40);
}

static void testDailyDst() {
  static Hourly hrs;
  static Daily d;
  hrs = Hourly();
  d = Daily();
  PosixTz tz;
  parsePosixTz(DEFAULT_TZ, &tz);
  // Sunday 25 Oct 2026 (local) is a 25-hour day: 24 Oct 22:00 UTC .. 25 Oct 23:00 UTC.
  const uint32_t start = (uint32_t)unixFromUtc(2026, 10, 24, 22, 0, 0);
  for (int h = 0; h < 25; h++) {
    hrs.pushRaw(hourRec(start + h * 3600, 5.0f + h, h == 13 ? 50.0f : 10.0f, 0.2f));
    CHECK(!d.addHour(hrs.at(hrs.size() - 1), hrs, tz));
  }
  CHECK(d.size() == 0);
  DayRec open;
  CHECK(d.openRecord(hrs, tz, &open) && open.ymd == 20261025 && open.hours == 25);
  // the first hour of Monday 26 Oct closes the Sunday
  hrs.pushRaw(hourRec(start + 25 * 3600, 1, 1, 0));
  CHECK(d.addHour(hrs.at(hrs.size() - 1), hrs, tz));
  CHECK(d.size() == 1);
  const DayRec& r = d.at(0);
  CHECK(r.ymd == 20261025 && r.n == 25 * 60 && r.hours == 25);
  CHECK(r.t_min == 40 && r.t_avg == 170 && r.t_max == 300 && r.gust_max == 500 && r.rain_sum == 500);
  CHECK(r.uv_max == U8_NONE);
  // tick closes the open day once local midnight has passed (26 Oct 23:00 UTC = 27 Oct 00:00 CET)
  CHECK(!d.tick((uint32_t)unixFromUtc(2026, 10, 26, 22, 59, 0), hrs, tz));
  CHECK(d.tick((uint32_t)unixFromUtc(2026, 10, 26, 23, 0, 0), hrs, tz));
  CHECK(d.size() == 2 && d.at(1).ymd == 20261026);
  // spring forward: Sunday 29 Mar 2026 has 23 hours (28 Mar 23:00 UTC .. 29 Mar 22:00 UTC)
  static Hourly sh;
  static Daily s;
  sh = Hourly();
  s = Daily();
  const uint32_t m0 = (uint32_t)unixFromUtc(2026, 3, 28, 23, 0, 0);
  for (int h = 0; h < 24; h++) {
    sh.pushRaw(hourRec(m0 + h * 3600, 1, 1, 0));
    s.addHour(sh.at(sh.size() - 1), sh, tz);
  }
  CHECK(s.size() == 1 && s.at(0).ymd == 20260329 && s.at(0).n == 23 * 60 && s.at(0).hours == 23);
  // after a restore the open day is the newest hour's, unless that date is closed already
  static Daily re;
  re = Daily();
  re.reopen(sh, tz);
  CHECK(re.isOpen() && dayNumberToYmd(re.openDay()) == 20260330);
  re.pushClosed(s.at(0));
  DayRec later;
  clearDay(&later, 20260330);
  re.pushClosed(later);
  re.reopen(sh, tz);
  CHECK(!re.isOpen());
}

static void testDailyRing() {
  static Hourly hrs;
  static Daily d;
  hrs = Hourly();
  d = Daily();
  PosixTz tz;
  parsePosixTz("UTC0", &tz);
  const uint32_t t0 = (uint32_t)unixFromUtc(2026, 1, 1, 12, 0, 0);
  for (int day = 0; day < 92; day++) {
    hrs.pushRaw(hourRec(t0 + day * 86400, (float)day, 1, 0));
    d.addHour(hrs.at(hrs.size() - 1), hrs, tz);
  }
  CHECK(DAILY_LEN == 30 && d.size() == DAILY_LEN);  // 91 closed days -> 30 kept in RAM
  CHECK(d.at(0).ymd == 20260303 && d.at(DAILY_LEN - 1).ymd == 20260401);
}

// the flash ring over the RAM file system
static void testFlashRing() {
  static RamFs fs;
  fs.files.clear();
  static FlashRing r;
  r.init(&fs, hourSpec(0));
  CHECK(r.scan() == 0 && r.count() == 0);
  const int R = HOUR_PER_SEG, S = HOUR_SEGS, CAP = R * S;
  const uint32_t H0 = 1800000000u - 1800000000u % 3600;
  int rotations = 0;
  for (int i = 0; i < 2 * CAP + 5; i++) {
    HourRec h = hourRec(H0 + (uint32_t)i * 3600, (float)(i % 50), 1, 0);
    const int before = r.count();
    const FlashRing::Add a = r.append(&h);
    if (a == FlashRing::Add::Rotated && i >= CAP) {
      rotations++;
      CHECK(r.count() == before - R + 1);  // the rotation drops exactly the R oldest
    }
  }
  CHECK(rotations > 0);
  CHECK(r.count() == (S - 1) * R + 5 && r.newestKey() == H0 + (uint32_t)(2 * CAP + 4) * 3600);
  CHECK(r.oldestKey() == r.newestKey() - (uint32_t)(r.count() - 1) * 3600);
  // after a "reboot" the newest is found again
  static FlashRing again;
  again.init(&fs, hourSpec(0));
  CHECK(again.scan() == r.count() && again.newestKey() == r.newestKey() && again.oldestKey() == r.oldestKey());
  HourRec x;
  CHECK(again.read(0, &x) && hourValid(x) && x.hour == r.oldestKey());
  CHECK(again.read(again.count() - 1, &x) && x.hour == r.newestKey());
  // lowerBound: a key inside, before the oldest, after the newest
  const uint32_t mid = r.oldestKey() + 1000u * 3600;
  const int i = again.lowerBound(mid);
  CHECK(i == 1000 && again.read(i, &x) && x.hour == mid);
  CHECK(again.lowerBound(mid + 1) == 1001 && again.lowerBound(0) == 0 && again.lowerBound(r.newestKey() + 1) == again.count());
  // an equal key replaces the newest, an older one is dropped and counted
  HourRec rep = hourRec(again.newestKey(), 42, 1, 0);
  CHECK(again.append(&rep) == FlashRing::Add::Replaced && again.count() == r.count());
  CHECK(again.read(again.count() - 1, &x) && x.t_avg == 420);
  HourRec old = hourRec(again.newestKey() - 3600, 1, 1, 0);
  CHECK(again.append(&old) == FlashRing::Add::Older && again.dropped() == 1 && again.count() == r.count());
  // a torn last record (a power cut mid-append): whole records count, the next append cuts it in one commit
  char p[RING_PATH_MAX];
  int cur = 0;
  for (int s = 0; s < S; s++) {
    ringSegPath(p, hourSpec(0), s);
    if (fs.files[p].size() / 48 == 5) cur = s;
  }
  ringSegPath(p, hourSpec(0), cur);
  fs.files[p].append("torn-bytes", 10);
  static FlashRing torn;
  torn.init(&fs, hourSpec(0));
  CHECK(torn.scan() == r.count());
  HourRec nx = hourRec(torn.newestKey() + 3600, 1, 1, 0);
  CHECK(torn.append(&nx) == FlashRing::Add::Appended && torn.cut() == 1 && fs.files[p].size() == 6 * 48);
  CHECK(torn.read(torn.count() - 1, &x) && hourValid(x) && x.hour == nx.hour);
  // records with a future key (a clock that went back) are cut from the newest end, across segments
  {
    const int before = torn.count();
    const uint32_t lim = torn.newestKey();
    for (int k = 1; k <= 120; k++) {  // 120 future hours: more than one segment (stamped on a browser seed)
      HourRec f = hourRec(lim + (uint32_t)k * 3600, 1, 1, 0);
      f.quality |= 1u << Q_CLOCK_APPROX;
      sealHour(&f);
      torn.append(&f);
    }
    CHECK(torn.count() > before && torn.newestKey() > lim);
    // the rotation they caused overwrote the oldest segment (85 real hours): those stay gone, the future ones are cut
    CHECK(torn.dropFuture(lim) == 120 && torn.count() == before - HOUR_PER_SEG && torn.newestKey() == lim &&
          torn.futureDropped() == 120);
    HourRec ok1 = hourRec(lim + 3600, 1, 1, 0);
    CHECK(torn.append(&ok1) == FlashRing::Add::Appended || torn.append(&ok1) == FlashRing::Add::Replaced);
  }
  // Forget: every segment empty (truncated, no erase), the ring empty after a rescan too
  CHECK(torn.forget() && torn.count() == 0);
  int bytes = 0;
  for (int s = 0; s < S; s++) {
    ringSegPath(p, hourSpec(0), s);
    bytes += (int)fs.files[p].size();
  }
  CHECK(bytes == 0 && again.scan() == 0);
  // the daily ring: keys are dates
  static FlashRing d;
  constexpr int KL = RING_SLOTS - 1;  // the profile's last ring (WROOM-32: ring 0; PSRAM: ring 1, 8 daily segments)
  d.init(&fs, daySpec(KL));
  const int DS1 = daySpec(KL).segs;
  for (int k = 0; k < 2 * DS1 * DAY_PER_SEG + 5; k++) {
    DayRec dr;
    clearDay(&dr, dayNumberToYmd(20000 + k));
    sealDay(&dr);
    d.append(&dr);
  }
  CHECK(d.count() == (DS1 - 1) * DAY_PER_SEG + 5 && d.newestKey() == dayNumberToYmd(20000 + 2 * DS1 * DAY_PER_SEG + 4));
  DayRec dr100;
  CHECK(d.read(100, &dr100) && d.lowerBound(dr100.ymd) == 100 && d.lowerBound(dr100.ymd + 0) == 100);
  char hp[RING_PATH_MAX];
  ringSegPath(hp, daySpec(KL), 7);
  ringHeaderPath(p, hourSpec(KL));
  CHECK(strcmp(hp, KL ? "/d1_07" : "/d0_07") == 0 && strcmp(p, KL ? "/h1.hdr" : "/h0.hdr") == 0);
  // the per-ring capacities and names: ring 0 131 d / 736 d on every profile; the PSRAM profile's ring 1 88 d / 368 d
  CHECK(hourSpec(0).segs * HOUR_PER_SEG == 3145 && daySpec(0).segs * DAY_PER_SEG == 736);
  CHECK(b7Name("h0_36") && !b7Name("h0_37") && b7Name("d0_15") && !b7Name("d0_16"));
  if (RING_SLOTS > 1) {
    CHECK(hourSpec(1).segs * HOUR_PER_SEG == 2125 && daySpec(1).segs * DAY_PER_SEG == 368);
    CHECK(b7Name("h1_24") && !b7Name("h1_25") && b7Name("d1_07") && !b7Name("d1_08"));
  } else {
    CHECK(!b7Name("h1_00") && !b7Name("d1_00") && !b7Name("h1.hdr") && hourSpec(1).k == 0);  // no ring 1 (the guard)
  }
  // the last ring fills, rotates and forgets like the first
  static FlashRing h1;
  fs.files.clear();
  h1.init(&fs, hourSpec(KL));
  const int CAP1 = hourSpec(KL).segs * HOUR_PER_SEG;
  for (int k = 0; k < CAP1 + 10; k++) {
    HourRec hr = hourRec(H0 + (uint32_t)k * 3600, 1, 1, 0);
    h1.append(&hr);
  }
  CHECK(h1.count() == CAP1 - HOUR_PER_SEG + 10 && h1.newestKey() == H0 + (uint32_t)(CAP1 + 9) * 3600);
  CHECK(h1.forget() && h1.count() == 0);
}

// a clock behind the stored history is held; only approximate records are ever cut
static HourRec approxHour(uint32_t hour) {
  HourRec r = hourRec(hour, 5, 5, 0);
  r.quality |= 1u << Q_CLOCK_APPROX;
  sealHour(&r);
  return r;
}
static void testClockHold() {
  const uint32_t ref = TIME_FLOOR + 30u * 86400u;
  CHECK(!clockHeld(ref - 3600, ref) && clockHeld(ref - 3601, ref) && !clockHeld(ref - 3601, 0) &&
        !clockHeld(TIME_FLOOR - 1, ref) && !clockHeld(ref + 7200, ref));
  CHECK(trustedStart(ref, 0) == ref && trustedStart(ref, 1u << Q_CLOCK_APPROX) == 0 && trustedStart(0, 0) == 0);
  // an hourly ring: 200 trusted hours, then 3 approximate ones two hours past the trusted tail
  static RamFs io;
  static FlashRing h;
  h.init(&io, hourSpec(0));
  const uint32_t h0 = (uint32_t)unixFromUtc(2026, 9, 1, 0, 0, 0);
  for (int k = 0; k < 200; k++) {
    HourRec r = hourRec(h0 + (uint32_t)k * 3600, 5, 5, 0);
    h.append(&r);
  }
  const uint32_t tail = h0 + 199u * 3600u, tailEnd = tail + 3600;
  for (int k = 3; k <= 5; k++) {
    HourRec a = approxHour(tail + (uint32_t)k * 3600);
    h.append(&a);
  }
  CHECK(h.count() == 203 && trustedHourStart(h) == tail);
  // a clock 2 h behind the trusted tail's start: held, and the store never calls the cut on a held clock
  uint32_t now = tail - 7200;
  int cut = clockHeld(now, trustedHourStart(h)) ? 0 : h.dropFuture(now + 3600);
  CHECK(clockHeld(now, trustedHourStart(h)) && cut == 0 && h.count() == 203);
  // the clock right (behind the approximate tail only): not held, exactly the 3 approximate hours go
  now = tailEnd + 600;
  CHECK(!clockHeld(now, trustedHourStart(h)) && h.dropFuture(now + 3600) == 3 && h.count() == 200 && h.newestKey() == tail);
  // a trusted record past the limit is kept (counted once per newest key), approximate ones after it would go
  CHECK(h.dropFuture(tail - 7200) == 0 && h.count() == 200 && h.futureKept() == 1);
  CHECK(h.dropFuture(tail - 7200) == 0 && h.futureKept() == 1);
  HourRec a2 = approxHour(tail + 3600);
  h.append(&a2);
  CHECK(h.dropFuture(tail - 7200) == 1 && h.newestKey() == tail && h.futureKept() == 1);
  // the 24-h acceptance cuts whatever is newer than the accepted clock, trusted or not
  CHECK(h.dropFuture(tail - 7200, false) == 2 && h.newestKey() == tail - 7200 && trustedHourStart(h) == tail - 7200);
  // the daily ring alike (bit 2 is OR-ed from the hours)
  static FlashRing d;
  d.init(&io, daySpec(0));
  for (int k = 0; k < 60; k++) {
    DayRec dr;
    clearDay(&dr, dayNumberToYmd(20600 + k));
    if (k >= 57) dr.quality |= 1u << Q_CLOCK_APPROX;
    sealDay(&dr);
    d.append(&dr);
  }
  CHECK(d.dropFuture(dayNumberToYmd(20600 + 56)) == 3 && d.count() == 57 && d.dropFuture(dayNumberToYmd(20600 + 50)) == 0 &&
        d.count() == 57 && d.futureKept() == 1);
  // the hold's bookkeeping: 24 h of steady public SNTP accept once; a step or another source starts over
  {
    ClockHold k;
    const uint32_t r = ref, behind = ref - 3 * 86400;  // a clock three days behind (still held after 2 days of ticking)
    bool started = false, accepted = false;
    CHECK(!k.step(behind, 100, r, true, &started) && started && k.held && k.held_n == 1);
    uint32_t up = 100;
    bool early = false;
    for (; up < 100 + CLOCK_ACCEPT_S - 1; up += 60) early = early || k.step(behind + (up - 100), up, r, true);
    CHECK(!early);
    CHECK(!k.step(behind + 4000, up, r, false));  // the router's rung: the run starts over
    const uint32_t up2 = up;
    for (up = up2 + 1; up < up2 + CLOCK_ACCEPT_S + 120 && !accepted; up += 60) accepted = k.step(behind + (up - 100), up, r, true);
    CHECK(accepted && up - up2 >= CLOCK_ACCEPT_S);
    // a forward step past the reference releases without an acceptance
    ClockHold f;
    CHECK(!f.step(behind, 10, r, true) && f.held && !f.step(r, 20, r, true) && !f.held && f.held_n == 1);
    // a clock that steps by more than CLOCK_STEADY_S restarts the 24 h
    ClockHold g;
    g.step(behind, 10, r, true);
    g.step(behind + 50000, 50010, r, true);
    CHECK(!g.step(behind + 50000 + 600, 50020, r, true) && g.sntp_since == 50020);
  }
  // an ingest sequence real -> held (uptime stamps) -> released by a forward step: the held readings land at their real
  // time, the RAM hourly ring stays strictly increasing, no 1970 hour
  {
    static Stations all;
    all.reserve(MAX_STATIONS);
    PosixTz tz;
    parsePosixTz("UTC0", &tz);
    Station* s = all.findOrAdd("CH1", defaultStationConfig());
    const uint32_t boot = (uint32_t)unixFromUtc(2026, 11, 2, 9, 0, 0);
    for (uint32_t t = boot + 60; t < boot + 3 * 3600; t += 65) {
      Stations::ingest(*s, sampleFrom("ID=CH1&tempf=50&windspeedmph=5"), t, (t - boot) * 1000, 0, SrcIf::Ap, true, tz);
      Stations::tick(*s, t, tz);
    }
    const int closed = s->hourly.size();
    // held for 40 min: the clock reads valid but behind, so readings carry uptime (clockNow's ms / 1000)
    const uint32_t up0 = 3 * 3600;
    for (uint32_t u = up0; u < up0 + 2400; u += 65) {
      Stations::ingest(*s, sampleFrom("ID=CH1&tempf=51&windspeedmph=6"), u, u * 1000, 0, SrcIf::Ap, false, tz);
      Stations::tick(*s, u, tz);  // an uptime now never closes or opens an hour
    }
    CHECK(s->hourly.size() == closed);
    // released: the real clock is boot + uptime (a forward step back to the truth)
    const int n = Stations::clockSynced(*s, boot);
    CHECK(n > 30);
    bool inc = true, no1970 = true;
    for (int i = 0; i < s->recent.size(); i++) {
      no1970 = no1970 && s->recent.at(i).t >= EPOCH_MIN;
      if (i) inc = inc && s->recent.at(i).t > s->recent.at(i - 1).t;
    }
    for (uint32_t t = boot + up0 + 2400; t < boot + up0 + 2 * 3600; t += 65) {
      Stations::ingest(*s, sampleFrom("ID=CH1&tempf=52&windspeedmph=5"), t, (t - boot) * 1000, 0, SrcIf::Ap, true, tz);
      Stations::tick(*s, t, tz);
    }
    for (int i = 0; i < s->hourly.size(); i++) {
      no1970 = no1970 && s->hourly.at(i).hour >= EPOCH_MIN;
      if (i) inc = inc && s->hourly.at(i).hour > s->hourly.at(i - 1).hour;
    }
    CHECK(inc && no1970 && s->hourly.size() > closed);
  }
}

// a ring stream stops reading flash once the client is gone (Out::ok false)
class CountingFs : public RamFs {
 public:
  int reads = 0;
  bool read(const char* path, size_t off, void* buf, size_t len) override {
    reads++;
    return RamFs::read(path, off, buf, len);
  }
};
struct FailAfter {
  size_t left;
};
static bool failAfter(void* ctx, const char*, size_t n) {
  FailAfter* f = (FailAfter*)ctx;
  if (n > f->left) return false;
  f->left -= n;
  return true;
}
// the hold's edges, the 24-h acceptance with the app's own limits, a restart during a
// hold, the release by a seed, the re-stamp re-arm
static void testClockHold2() {
  PosixTz utc;
  parsePosixTz("UTC0", &utc);
  const uint32_t day0 = (uint32_t)unixFromUtc(2026, 10, 20, 0, 0, 0);
  // SNTP correcting a fast clock by minutes inside an hour never holds (the reference is the hour's start)
  const uint32_t h10 = day0 + 10 * 3600;
  CHECK(!clockHeld(h10 - 4 * 60, h10) && !clockHeld(h10 - 3600, h10) && clockHeld(h10 - 3601, h10));
  // a ring written for days by a router clock 2 days fast (trusted rung: no clock_approx), then the 24-h acceptance
  const uint32_t now = day0 + 5 * 86400 + 14 * 3600 + 1234;  // the accepted (real) clock
  static RamFs io;
  static FlashRing h, d;
  h.init(&io, hourSpec(0));
  d.init(&io, daySpec(0));
  const uint32_t fastEnd = now + 2 * 86400;
  for (uint32_t t = day0; t <= fastEnd; t += 3600) {
    HourRec r = hourRec(t - t % 3600, 5, 5, 0);
    h.append(&r);
  }
  for (int32_t dn = localDayNumber(utc, day0); dn <= localDayNumber(utc, fastEnd); dn++) {
    DayRec dr;
    clearDay(&dr, dayNumberToYmd(dn));
    sealDay(&dr);
    d.append(&dr);
  }
  CHECK(clockHeld(now, trustedHourStart(h)));
  ClockHold boot;  // a restart during the hold: the reference comes back from flash and holds again at once
  bool started = false;
  CHECK(!boot.step(now, 30, trustedHourStart(h), true, &started) && started && boot.held && boot.held_n == 1);
  const AcceptLimits lim = clockAcceptLimits(now, utc);
  CHECK(lim.hour == now - now % 3600 - 1 && lim.ymd == dayNumberToYmd(localDayNumber(utc, now) - 1));
  h.dropFuture(lim.hour, false);
  d.dropFuture(lim.ymd, false);
  CHECK(!clockHeld(now, trustedHourStart(h)) && h.newestKey() == now - now % 3600 - 3600 && d.newestKey() == lim.ymd);
  HourRec cur = hourRec(now - now % 3600, 6, 6, 0);  // the next real close (the hour that was open at the acceptance)
  DayRec today;
  clearDay(&today, dayNumberToYmd(localDayNumber(utc, now)));
  sealDay(&today);
  CHECK(h.append(&cur) == FlashRing::Add::Appended && d.append(&today) == FlashRing::Add::Appended);
  // a ring entirely in the future: the acceptance empties it, and nothing holds afterwards
  static RamFs io2;
  static FlashRing f;
  f.init(&io2, hourSpec(1));
  for (int k = 1; k <= 30; k++) {
    HourRec r = hourRec(now - now % 3600 + (uint32_t)k * 3600, 5, 5, 0);
    f.append(&r);
  }
  CHECK(f.dropFuture(clockAcceptLimits(now, utc).hour, false) == 30 && f.count() == 0 && trustedHourStart(f) == 0 &&
        !clockHeld(now, trustedHourStart(f)));
  // while held, a seed replaces the SNTP clock only when it releases the hold; a seed that would stay held is refused
  // (an SNTP clock is never overwritten), so it neither overwrites SNTP nor restarts the 24-h run
  const uint32_t ref = h10 + 86400, held = ref - 86400;
  ClockLadder lad;
  lad.sys_src = TimeSrc::Sntp;
  const uint32_t ceil = TIME_FLOOR + SEED_SPAN_S;
  const uint64_t stillBehind = (uint64_t)(ref - 7200) * 1000, releases = (uint64_t)(ref + 60) * 1000;
  CHECK(seedJudgeTime(held, ref, ref - 7200) == held && !lad.seedAccepted(stillBehind, ceil, seedJudgeTime(held, ref, ref - 7200)));
  CHECK(seedJudgeTime(held, ref, ref + 60) == 0 && lad.seedAccepted(releases, ceil, seedJudgeTime(held, ref, ref + 60)));
  CHECK(seedJudgeTime(ref, ref, ref - 7200) == ref);  // not held: the clock as it is
  // the re-stamp is re-armed when a hold starts, and only then
  CHECK(restampRearm(true, true) && !restampRearm(true, false) && !restampRearm(false, true) && !restampRearm(false, false));
}

static void testStreamAbort() {
  static CountingFs io;
  static FlashRing h;
  h.init(&io, hourSpec(1));
  const uint32_t h0 = (uint32_t)unixFromUtc(2026, 9, 1, 0, 0, 0);
  for (int k = 0; k < 300; k++) {
    HourRec r = hourRec(h0 + (uint32_t)k * 3600, 5, 5, 0);
    h.append(&r);
  }
  ChMap m = {};
  const RowView v{0, &m, 1};
  FailAfter f{2048};
  char buf[256];
  Out o(failAfter, &f, buf, sizeof buf);
  const uint32_t aborts = streamAborts();
  io.reads = 0;
  renderHourlyRing(o, "ST1", h, h0, h0 + 300u * 3600u, nullptr, v);
  o.flush();
  // one hour row is > 48 B of JSON; the stream may read the records of 2 KB + one buffer, then it stops
  CHECK(!o.ok() && io.reads <= 2048 / 48 + 8 && streamAborts() == aborts + 1);
  printf("  info: stream to a client gone after 2 KB: %d ring reads of 300\n", io.reads);
}

// readRun reads a run of records of one segment in one file read; the ring views stream the same
// rows as record-by-record reads, with fewer reads
template <typename Rec, typename Row>
static std::string refView(const FlashRing& ring, uint32_t from, uint32_t to, int newest, Row row, const RowView& v,
                           bool (*valid)(const Rec&)) {  // the reference: one read per record
  std::string j;
  char b[128];
  Out o(toString, &j, b, sizeof b);
  int i = newest > 0 ? (ring.count() > newest ? ring.count() - newest : 0) : ring.lowerBound(from);
  int written = 0;
  uint32_t last = 0;
  Rec r;
  for (; i < ring.count(); i++) {
    if (!ring.read(i, &r) || !valid(r)) continue;
    uint32_t key;
    memcpy(&key, &r, sizeof key);
    if (newest <= 0 && key < from) continue;
    if (newest <= 0 && key >= to) break;
    if (written && key <= last) continue;
    last = key;
    if (written++) o.raw(",");
    row(o, r, v);
  }
  o.flush();
  return j;
}
static std::string between(const std::string& j, const char* open) {
  const size_t a = j.find(open);
  return a == std::string::npos ? "?" : j.substr(a + strlen(open), j.rfind(']') - a - strlen(open));
}
static void testReadRun() {
  static CountingFs io;
  static FlashRing h;
  h.init(&io, hourSpec(0));
  const uint32_t h0 = (uint32_t)unixFromUtc(2026, 6, 1, 0, 0, 0);
  for (int k = 0; k < 400; k++) {
    HourRec r = hourRec(h0 + (uint32_t)k * 3600, 5.0f + (float)(k % 7), 5, 0);
    h.append(&r);
  }
  // a run never crosses a segment edge and equals the record reads
  bool same = true;
  int runs = 0;
  for (int i = 0; i < h.count();) {
    static HourRec run[8];
    const int n = h.readRun(i, run, 8);
    runs++;
    same = same && n >= 1 && n <= 8;
    for (int k = 0; k < n && same; k++) {
      HourRec one;
      same = h.read(i + k, &one) && memcmp(&one, &run[k], sizeof one) == 0;
    }
    i += n ? n : 1;
  }
  CHECK(same && runs >= h.count() / 8 && h.readRun(h.count(), nullptr, 8) == 0 && h.readRun(0, nullptr, 0) == 0);
  // a torn tail (half a record appended to the newest segment): readRun stops at the whole records
  {
    static RamFs tio;
    static FlashRing t;
    t.init(&tio, hourSpec(1));
    for (int k = 0; k < 10; k++) {
      HourRec r = hourRec(h0 + (uint32_t)k * 3600, 5, 5, 0);
      t.append(&r);
    }
    char path[RING_PATH_MAX];
    ringSegPath(path, hourSpec(1), 0);
    tio.files[path].append(20, '\x55');
    t.scan();
    static HourRec run[16];
    CHECK(t.count() == 10 && t.readRun(6, run, 16) == 4 && run[3].hour == h0 + 9 * 3600);
  }
  // every ring view: the same rows as before, with fewer reads
  ChMap m = {};
  const RowView v{0, &m, 1};
  const uint32_t win[][2] = {{h0, h0 + 400u * 3600u}, {h0 + 37u * 3600u, h0 + 200u * 3600u}, {h0 + 84u * 3600u, h0 + 86u * 3600u},
                             {h0 + 399u * 3600u, h0 + 500u * 3600u}, {h0 - 3600, h0 + 3600}};
  bool eq = true;
  int readsRun = 0, readsRec = 0;
  for (const auto& w : win) {
    std::string j;
    char b[128];
    Out o(toString, &j, b, sizeof b);
    io.reads = 0;
    renderHourlyRing(o, "ST1", h, w[0], w[1], nullptr, v);
    o.flush();
    readsRun += io.reads;
    io.reads = 0;
    const std::string ref = refView<HourRec>(h, w[0], w[1], 0, hourRow, v, hourValid);
    readsRec += io.reads;
    eq = eq && between(j, "\"hours\":[") == ref;
  }
  CHECK(eq && readsRun * 4 < readsRec);
  printf("  info: ring views read %d times in runs, %d times record by record\n", readsRun, readsRec);
  static FlashRing d;
  d.init(&io, daySpec(0));
  for (int k = 0; k < 120; k++) {
    DayRec dr;
    clearDay(&dr, dayNumberToYmd(20500 + k));
    sealDay(&dr);
    d.append(&dr);
  }
  bool deq = true;
  for (int nw : {0, 1, 30, 90, 500}) {
    std::string j;
    char b[128];
    Out o(toString, &j, b, sizeof b);
    renderDailyRing(o, "ST1", d, dayNumberToYmd(20510), dayNumberToYmd(20600), nw, nullptr, v);
    o.flush();
    deq = deq && between(j, "\"days\":[") == refView<DayRec>(d, dayNumberToYmd(20510), dayNumberToYmd(20600), nw, dayRow, v, dayValid);
  }
  CHECK(deq);
}

// full rings of every slot + the snapshot (+ its .tmp) leave >= 8 of LFS_BLOCKS free (files > 512 B take whole blocks;
// smaller ones are inline in the metadata; the metadata is budgeted at 4 blocks with full rings)
static void testRingBudget() {
  static RamFs fs;
  fs.files.clear();
  CHECK(buildLayout(fs) && markerRead(fs) == MarkerState::Valid);
  for (int k = 0; k < RING_SLOTS; k++) {
    static FlashRing h, d;
    h.init(&fs, hourSpec(k));
    d.init(&fs, daySpec(k));
    for (int i = 0; i < hourSpec(k).segs * HOUR_PER_SEG; i++) {
      HourRec r = hourRec(1700000000u + (uint32_t)i * 3600, 1, 1, 0);
      h.append(&r);
    }
    for (int i = 0; i < daySpec(k).segs * DAY_PER_SEG; i++) {
      DayRec r;
      clearDay(&r, dayNumberToYmd(19000 + i));
      d.append(&r);
    }
  }
  static SnapEntry e[RING_SLOTS];
  memset((void*)e, 0, sizeof e);
  CHECK(snapWrite(fs, e, RING_SLOTS));
  fs.files[SNAP_TMP_PATH] = fs.files[SNAP_PATH];  // the transient copy during a write
  int blocks = 0, files = 0;
  for (auto& f : fs.files) {
    files++;
    if (f.second.size() > 512) blocks += (int)((f.second.size() + 4095) / 4096);
  }
  const int meta = 4, segs = ringBlocks();  // every ring of the profile
  CHECK(files == segs + 2 * RING_SLOTS + 3);  // segments, 2 headers per slot, marker, snapshot + tmp
  CHECK(blocks == segs + 2);
  CHECK(LFS_BLOCKS - blocks - meta >= 8);
  printf("  info: full rings + snapshot = %d blocks + %d metadata of %d (%d free during a snapshot write)\n", blocks, meta,
         LFS_BLOCKS, LFS_BLOCKS - blocks - meta);
}

// the row JSON (one shape: a RAM row = the same record read back from flash, byte for byte)
static void testRowGolden() {
  HourRec r;
  clearHour(&r, (uint32_t)unixFromUtc(2026, 11, 5, 10, 0, 0));
  r.n = 56;
  r.quality = (1u << Q_RESTART) | (1u << Q_GAP);
  r.t_min = 81, r.t_avg = 83, r.t_max = 85, r.td_avg = 50, r.p_avg = 10098, r.rh_min = 79, r.rh_avg = 80, r.rh_max = 81;
  r.wind_avg = 61, r.gust_max = 148, r.wdir_steady = packDir(238, 104), r.calm_n = 3, r.rain_sum = 0, r.uv_max = 4;
  r.solar_avg = 81, r.solar_max = 96;
  r.ch_ver = 1;
  r.ch_avg[0] = 214;
  sealHour(&r);
  ChMap m = {};
  m.ver = 1;
  strcpy(m.key[0], "indoor_temp_c");
  m.scale[0] = 10;
  const RowView v{0, &m, 1};
  std::string ram, flash, hid;
  char buf[64];
  Out o(toString, &ram, buf, sizeof buf);
  hourRow(o, r, v);
  o.flush();
  CHECK(ram ==
        "{\"h\":\"2026-11-05T10:00Z\",\"n\":56,\"q\":[\"restart\",\"gap\"],\"epoch\":0,\"rej_fields\":0,\"temp_c\":[8.1,8.3,8.5],"
        "\"dewpt_c\":[null,5.0,null],\"humidity\":[79,80,81],\"pressure_hpa\":[null,1009.8,null],\"wind_kmh\":[null,6.1,null],"
        "\"gust_kmh\":14.8,\"wind_dir\":238,\"wind_steady\":0.82,\"calm_n\":3,\"rain_mm\":0.00,\"rain_daily_mm\":null,"
        "\"rain_rate_max\":null,\"uv\":[null,null,0.4],\"solar_wm2\":[null,81,96],\"indoor_temp_c\":[null,21.4,null]}");
  static RamFs fs;
  fs.files.clear();
  static FlashRing ring;
  ring.init(&fs, hourSpec(0));
  ring.append(&r);
  HourRec back;
  CHECK(ring.read(0, &back) && memcmp(&back, &r, sizeof r) == 0);
  Out of(toString, &flash, buf, sizeof buf);
  renderHourlyRing(of, "ST", ring, 0, UINT32_MAX, nullptr, v);
  of.flush();
  CHECK(contains(flash, ram.c_str()) && contains(flash, "{\"station\":\"ST\",\"open\":null,\"hours\":[{"));
  // hidden fields are omitted, keys and all; a channel of a hidden field too
  const RowView vh{(1u << F_TEMP_C) | (1u << F_WIND_DIR) | (1u << F_INDOOR_TEMP_C), &m, 1};
  Out oh(toString, &hid, buf, sizeof buf);
  hourRow(oh, r, vh);
  oh.flush();
  CHECK(!contains(hid, "temp_c") && !contains(hid, "wind_dir") && !contains(hid, "wind_steady") && contains(hid, "\"humidity\""));
  std::string hr;
  Out ohr(toString, &hr, buf, sizeof buf);
  hourRow(ohr, r, RowView{1u << F_RAIN_MM, &m, 1});
  ohr.flush();
  CHECK(!contains(hr, "\"rain_mm\"") && contains(hr, "\"rain_daily_mm\":null"));
  // an unknown map version: no channel keys; a calm hour: wind_dir null; the all-null triple is null
  HourRec c = r;
  c.ch_ver = 9;
  c.wdir_steady = packDir(DIR_NONE, 0);
  c.t_min = c.t_avg = c.t_max = I16_NONE;
  sealHour(&c);
  std::string cs;
  Out oc(toString, &cs, buf, sizeof buf);
  hourRow(oc, c, v);
  oc.flush();
  CHECK(!contains(cs, "indoor") && contains(cs, "\"wind_dir\":null,\"wind_steady\":null") && contains(cs, "\"temp_c\":null"));
  // the daily row
  DayRec d;
  clearDay(&d, 20261104);
  d.n = 1310, d.hours = 24, d.clean_hours = 22, d.quality = 1u << Q_GAP, d.t_min = 38, d.t_avg = 65, d.t_max = 102;
  d.wdir_steady = packDir(250, 52), d.calm_hours = 9, d.rain_sum = 140, d.rain_rate_max = 7, d.rain_hours = 2;
  d.rose[0] = 1, d.rose[3] = 2, d.rose[4] = 6, d.rose[5] = 4, d.rose[6] = 2, d.uv_max = 21;
  sealDay(&d);
  std::string ds;
  Out od(toString, &ds, buf, sizeof buf);
  dayRow(od, d, RowView{0, nullptr, 0});
  od.flush();
  CHECK(contains(ds, "{\"d\":\"2026-11-04\",\"n\":1310,\"hours\":24,\"clean_hours\":22,\"q\":[\"gap\"],\"epoch\":0,\"rej_fields\":0,"
                     "\"temp_c\":[3.8,6.5,10.2]"));
  CHECK(contains(ds, "\"wind_dir\":250,\"wind_steady\":0.41,\"calm_hours\":9,\"rose\":[1,0,0,2,6,4,2,0],\"rain_mm\":1.40,"
                     "\"rain_daily_mm\":1.40,\"rain_hours\":2,\"rain_rate_max\":3.5,\"uv\":[null,null,2.1]"));
}

// the format plan and the leftover filter
static void testFormatPlan() {
  CHECK(formatPlan(MarkerState::Missing, true) == FormatPlan::WaitValid);
  CHECK(formatPlan(MarkerState::Missing, false) == FormatPlan::Format);
  CHECK(formatPlan(MarkerState::Foreign, true) == FormatPlan::RamOnlyForeign);
  CHECK(formatPlan(MarkerState::Foreign, false) == FormatPlan::RamOnlyForeign);
  CHECK(formatPlan(MarkerState::Valid, true) == FormatPlan::Use && formatPlan(MarkerState::Valid, false) == FormatPlan::Use);
  static RamFs fs;
  fs.files.clear();
  CHECK(markerRead(fs) == MarkerState::Missing);
  fs.files["/fc7"] = "FC7M\x01\x02\x03\x04";
  CHECK(markerRead(fs) == MarkerState::Foreign);
  fs.files.clear();
  fs.files["/d/ST1.dat"] = "old";
  CHECK(buildLayout(fs) && markerRead(fs) == MarkerState::Valid);
  RingHeader rh;
  CHECK(ringHeaderRead(fs, daySpec(RING_SLOTS - 1), ringFingerprint(), &rh) && rh.id[0] == 0);
  CHECK(ringHeaderWrite(fs, daySpec(RING_SLOTS - 1), ringFingerprint(), "ST1") &&
        ringHeaderRead(fs, daySpec(RING_SLOTS - 1), ringFingerprint(), &rh) && strcmp(rh.id, "ST1") == 0);
  CHECK(!ringHeaderRead(fs, daySpec(RING_SLOTS - 1), ringFingerprint() + 1, &rh));  // another layout: not ours
  // the marker's fingerprint is the records + geometry only, so a bigger Sample / HourAcc / SnapEntry or a
  // new WU field never makes the rings foreign; the snapshot has its own
#if FC_PROFILE_PSRAM
  const uint16_t ringLayout[] = {48, 36, 47, 88, 56, 87, 37, 25, 85, 16, 8, 46, 2};  // the per-ring geometry
#else
  const uint16_t ringLayout[] = {48, 36, 47, 88, 56, 87, 37, 85, 16, 46, 1};  // the WROOM-32's one ring
#endif
  CHECK(ringFingerprint() == crc32(ringLayout, sizeof ringLayout) && snapFingerprint() != ringFingerprint());
  for (const char* ok : {"h0_00", "d0_00", "h0.hdr", "s", "s.tmp", "fc7"}) CHECK(b7Name(ok));
  for (const char* no : {"d", "h0_37", "d0_16", "h1_25", "d1_16", "h2_00", "h0.hdr.old", "s.old", "ST1.dat", "h0_1", "h0_xx", ""}) CHECK(!b7Name(no));
  for (const char* k1 : {"h1_24", "d1_07", "d1.hdr"}) CHECK(b7Name(k1) == (RING_SLOTS > 1));
  // the leftovers of the older per-station tree: the "d" directory, nothing else
  struct Ctx {
    std::vector<std::string> out;
  } c;
  fs.list([](void* p, const char* name, bool) { if (!b7Name(name)) ((Ctx*)p)->out.push_back(name); }, &c);
  CHECK(c.out.size() == 1 && c.out[0] == "d");
  CHECK(fs.removeTree("/d") && fs.files.count("/d/ST1.dat") == 0);
}

// the shared snapshot (one block for both stations) and its application
static void testSnapshotV7() {
  CHECK(SNAP_HEADER_SIZE + RING_SLOTS * sizeof(SnapEntry) <= 4096);
  printf("  info: snapshot entry %u B, shared snapshot %u B (one 4 KB block)\n", (unsigned)sizeof(SnapEntry),
         (unsigned)(SNAP_HEADER_SIZE + RING_SLOTS * sizeof(SnapEntry)));
  static Stations all, boot;
  all.reserve(MAX_STATIONS);
  boot.reserve(MAX_STATIONS);
  PosixTz tz;
  parsePosixTz("UTC0", &tz);
  Station* a = all.findOrAdd("SA", defaultStationConfig());
  static Stations allSB;  // a slot of its own (one station per Stations on the WROOM-32 profile)
  allSB.reserve(MAX_STATIONS);
  Station* b = allSB.findOrAdd("SB", defaultStationConfig());
  const uint32_t t0 = (uint32_t)unixFromUtc(2026, 11, 9, 10, 5, 0);
  Stations::ingest(*a, sampleFrom("ID=SA&tempf=50&lowbatt=1&dailyrainin=0.1"), t0, 1000, 0, SrcIf::Ap, true, tz);
  Stations::ingest(*a, sampleFrom("ID=SA&tempf=52&dailyrainin=0.2"), t0 + 60, 61000, 0, SrcIf::Ap, true, tz);
  Stations::ingest(*b, sampleFrom("ID=SB&tempf=40"), t0, 1000, 0, SrcIf::Ap, true, tz);
  static SnapEntry e[2], back[2];  // two stations built; the profile writes RING_SLOTS of them
  CHECK(snapshotWorthWriting(*a) && snapshotWorthWriting(*b));
  takeSnapEntry(*a, &e[0]);
  takeSnapEntry(*b, &e[1]);
  static RamFs fs;
  fs.files.clear();
  // one entry per ring of the profile (WROOM-32: SA alone; PSRAM: SA and SB)
  CHECK(snapWrite(fs, e, RING_SLOTS) && fs.files.count(SNAP_TMP_PATH) == 0 && fs.files[SNAP_PATH].size() <= 4096);
  CHECK(snapRead(fs, back, RING_SLOTS) == RING_SLOTS && memcmp(back, e, RING_SLOTS * sizeof e[0]) == 0);
  CHECK(RING_SLOTS > 1 || !snapWrite(fs, e, RING_SLOTS + 1));  // never more entries than rings
  fs.files[SNAP_PATH][20] ^= 1;
  CHECK(snapRead(fs, back, RING_SLOTS) == -1);  // the CRC
  CHECK(snapWrite(fs, e, RING_SLOTS) && snapRead(fs, back, RING_SLOTS) == RING_SLOTS);
  // a restart: the open hour, the rain baseline, the last reading and the known keys come back; `restart` marked
  Station* ra = boot.findOrAdd("SA", defaultStationConfig());
  CHECK(applySnapEntry(*ra, back[0], 0));
  CHECK(ra->hourly.acc().hour == t0 - 300 && ra->hourly.acc().n == 2 && (ra->hourly.acc().quality >> Q_RESTART & 1u));
  CHECK(ra->fstate.have_daily && ra->live.has(F_TEMP_C) && (ra->flash_state & FLASH_LIVE_RESTORED) && ra->fresh == 0);
  CHECK(((ra->seen_fields >> F_BATTERY_LOW) & 1u) && ra->recent.size() == 1);
  HourRec open;
  CHECK(ra->hourly.openRecord(&open) && open.n == 2 && (open.quality >> Q_RESTART & 1u));
  // flash wins: an open hour already closed on flash is dropped
  static Stations bootSB;  // a slot of its own (one station per Stations on the WROOM-32 profile)
  bootSB.reserve(MAX_STATIONS);
  Station* rb = bootSB.findOrAdd("SB", defaultStationConfig());
  if (RING_SLOTS > 1) CHECK(!applySnapEntry(*rb, back[1], t0 - 300) && rb->hourly.acc().hour == 0 && rb->live.has(F_TEMP_C));
  // a station that uploaded meanwhile keeps what it has
  static Stations boot2;  // two slots per Stations
  boot2.reserve(MAX_STATIONS);
  Station* c = boot2.findOrAdd("SC", defaultStationConfig());
  Stations::ingest(*c, sampleFrom("ID=SC&tempf=30"), t0, 1000, 0, SrcIf::Ap, true, tz);
  CHECK(!applySnapEntry(*c, back[0], 0) && c->live.v[F_TEMP_C] < 0);
}

// the thresholds on the byte-addressable heap, with measured vectors
static void testHeapThresholds() {
  CHECK(HEAP_LOW_FREE8 == 18432 && heapLowBy(18431) && !heapLowBy(18432));
  // idle 50 240, worst normal ~24 KB, an estimate of ~35 KB: never low; a measured dip to ~3.6 KB: low
  CHECK(!heapLowBy(50240) && !heapLowBy(24 * 1024) && !heapLowBy(35 * 1024) && heapLowBy(3600));
  CHECK(OTA_MIN_FREE8 == 28672 && OTA_MIN_BLOCK == 12288);
  CHECK(!otaHeapOk(28671, 12288) && !otaHeapOk(28672, 12287) && otaHeapOk(28672, 12288) && otaHeapOk(50240, 45044) &&
        !otaHeapOk(24 * 1024, 45044));
  CHECK(HEAP_TIGHT_FREE8 == 36864 && heapTight(36863, 47092) && !heapTight(36864, 16384) && heapTight(50240, 16383) &&
        !heapTight(50240, 47092));
  // the OTA record keeps the minimum of each figure over the write
  OtaHeapRec r{OTA_HEAP_MAGIC, 50240, 50240, 47092};
  otaHeapStep(&r, 30000, 40000);
  otaHeapStep(&r, 41000, 12288);
  CHECK(r.start8 == 50240 && r.min8 == 30000 && r.largest8_min == 12288);
  // the since-boot minimum's second -- time, context and sockets move only with a lower minimum, all together
  HeapDip dip;
  CHECK(!dip.have() && dip.socks == -1 && dip.lower(UINT32_MAX - 1));
  CHECK(dip.note(52860, 10, HC_FWD, 1) && dip.have() && dip.min8 == 52860 && dip.up_s == 10 && dip.ctx == HC_FWD &&
        dip.socks == 1);
  CHECK(!dip.note(52860, 20, HC_HTTP, 4) && !dip.note(60000, 30, HC_MQTT, 5));  // equal or higher: nothing moves
  CHECK(dip.min8 == 52860 && dip.up_s == 10 && dip.ctx == HC_FWD && dip.socks == 1);
  CHECK(dip.note(2812, 60927, HC_HTTP | HC_ACCEPT, 300) && dip.min8 == 2812 && dip.up_s == 60927 &&
        dip.ctx == (HC_HTTP | HC_ACCEPT) && dip.socks == 127);  // the socket count is clamped to int8
  CHECK(dip.note(2000, 61000, 0, -5) && dip.ctx == 0 && dip.socks == -1);
  CHECK(std::string(heapCtxName(0)) == "http" && std::string(heapCtxName(7)) == "upload" &&
        std::string(heapCtxName(8)).empty() && std::string(heapCtxName(-1)).empty());
  // the low-water log line carries the context; its widest case fits the boot log's 200-B line uncut
  char cj[48];
  heapCtxJoin(0, cj, sizeof cj);
  CHECK(std::string(cj) == "-");
  heapCtxJoin(HC_HTTP | HC_ACCEPT, cj, sizeof cj);
  CHECK(std::string(cj) == "http,accept");
  heapCtxJoin(0xFF, cj, sizeof cj);
  CHECK(std::string(cj) == "http,accept,fwd,mqtt,store,learn,ota,upload");
  heapCtxJoin(0xFF, cj, 12);  // a short buffer keeps whole names only
  CHECK(std::string(cj) == "http,accept");
  HeapLowLine hl{2812, 53120, 49140, -66, 1, true, 4, HeapDip{}};
  hl.dip.note(2812, 60927, HC_HTTP | HC_ACCEPT, 6);
  char msg[BOOTLOG_LINE_MAX];
  heapLowLine(msg, sizeof msg, hl);
#if FC_DIAG_BENCH
  CHECK(std::string(msg) == "low-water min8 2812 B (free8 53120, largest8 49140) rssi -66 ap 1 mqtt up busy10m 4 ms "
                            "ctx http,accept socks 6");
#else  // busy10m leaves the release line with http_busy_ms_max
  CHECK(std::string(msg) == "low-water min8 2812 B (free8 53120, largest8 49140) rssi -66 ap 1 mqtt up ctx http,accept socks 6");
#endif
  HeapLowLine wide{327680, 327680, 327680, -100, 4, false, 4294967295u, HeapDip{}};
  wide.dip.note(327680, 4294967295u, 0xFF, 300);
  heapLowLine(msg, sizeof msg, wide);
  char wl[BOOTLOG_LINE_MAX + 1];
  const size_t wn = bootlogLine(wl, 4294967295u, "HEAP", msg, false);
  printf("  info: widest [HEAP] low-water line = %u bytes of %u\n", (unsigned)wn, (unsigned)BOOTLOG_LINE_MAX);
  CHECK(wn <= BOOTLOG_LINE_MAX && !contains(wl, "[cut]") && contains(wl, "ctx http,accept,fwd,mqtt,store,learn,ota,upload socks 127\n"));
}

// page pacing on a fake heap -- go above the line, wait in steps up to the limit, then shed; the largest-block
// shedding rule first; an answer whose earlier ones are acknowledged meanwhile goes on
static void testPagePace() {
  CHECK(PACE_FREE8 == 24576 && PACE_STEP_MS == 20 && PACE_MAX_MS == 200);
  CHECK(pagePace(52000, 49140, 0) == Pace::Go && pagePace(PACE_FREE8, 20000, 0) == Pace::Go);
  CHECK(pagePace(PACE_FREE8 - 1, 20000, 0) == Pace::Wait);
  CHECK(pagePace(30000, SHED_LARGEST_MIN - 1, 0) == Pace::Shed);  // no block for an answer: shed at once
  CHECK(pagePace(10000, 9000, PACE_MAX_MS - PACE_STEP_MS) == Pace::Wait && pagePace(10000, 9000, PACE_MAX_MS) == Pace::Shed);
  // the dispatch loop on a fake heap: free8 climbs back by 3 KB per step as acknowledgements free the queues
  auto loop = [](uint32_t free8, uint32_t climb, int* steps) {
    uint32_t waited = 0;
    Pace p;
    *steps = 0;
    while ((p = pagePace(free8, 30000, waited)) == Pace::Wait) {
      waited += PACE_STEP_MS;
      free8 += climb;
      (*steps)++;
    }
    return p;
  };
  int steps = 0;
  CHECK(loop(15000, 3072, &steps) == Pace::Go && steps == 4);   // 15 000 + 4 x 3 072 = 27 288 >= 24 576
  CHECK(loop(15000, 0, &steps) == Pace::Shed && steps == 10);   // nothing acknowledged: 10 steps = 200 ms, then 503
  CHECK(loop(30000, 0, &steps) == Pace::Go && steps == 0);
}

// the back-off before retrying a failed ring append
static void testStoreRetry() {
  CHECK(storeRetryS(0) == 60 && storeRetryS(1) == 60 && storeRetryS(2) == 120 && storeRetryS(3) == 240 &&
        storeRetryS(4) == 480 && storeRetryS(5) == 600 && storeRetryS(6) == 600 && storeRetryS(255) == 600);
}

// snapshot version 8 = 7 + the stale-sensor state; a version 7 file is read and migrated
static void putLe(std::string& f, size_t off, uint32_t v, int n) {
  for (int i = 0; i < n; i++) f[off + i] = (char)(v >> (8 * i));
}
static void testSnapshotV8() {
  static Stations all;
  all.reserve(MAX_STATIONS);
  PosixTz tz;
  parsePosixTz("UTC0", &tz);
  Station* a = all.findOrAdd("SV1", defaultStationConfig());
  const uint32_t t0 = (uint32_t)unixFromUtc(2026, 11, 12, 6, 0, 0);
  uint32_t t = t0;
  // identical uploads (a dead sensor battery, windy set) until one short of the stale count
  Stations::ingest(*a, sampleFrom("ID=SV1&tempf=28.2&humidity=91&windspeedmph=4&windgustmph=6&baromin=30.01"), t, 1000, 0, SrcIf::Ap, true, tz);
  while (a->same_n < STALE_SENSOR_N - 1) {
    t += 65;
    Stations::ingest(*a, sampleFrom("ID=SV1&tempf=28.2&humidity=91&windspeedmph=4&windgustmph=6&baromin=30.02"), t, 1000, 0, SrcIf::Ap, true, tz);
  }
  CHECK(!a->sensor_stale && a->sensor_init);
  static SnapEntry e[RING_SLOTS], back[RING_SLOTS];
  takeSnapEntry(*a, &e[0]);
  CHECK(e[0].stale.same_n == STALE_SENSOR_N - 1 && e[0].stale.sensor_init == 1);
  static RamFs fs;
  fs.files.clear();
  // v8 round trip
  uint8_t ver = 0;
  CHECK(snapWrite(fs, e, 1) && snapRead(fs, back, RING_SLOTS, &ver) == 1 && ver == 8 && memcmp(&back[0], &e[0], sizeof e[0]) == 0);
  // the restart: one more identical upload -> stale at once, the set kept out of the hour
  static Stations boot;
  boot.reserve(MAX_STATIONS);
  Station* r = boot.findOrAdd("SV1", defaultStationConfig());
  applySnapEntry(*r, back[0], 0);
  const uint16_t tn = r->hourly.acc().t_n;
  t += 65;
  Stations::ingest(*r, sampleFrom("ID=SV1&tempf=28.2&humidity=91&windspeedmph=4&windgustmph=6&baromin=30.03"), t, 1000, 0, SrcIf::Ap, true, tz);
  CHECK(r->sensor_stale && r->hourly.acc().t_n == tn && r->hourly.acc().p_n > 0);
  // a changed reading after the restore clears it
  t += 65;
  Stations::ingest(*r, sampleFrom("ID=SV1&tempf=28.4&humidity=91&windspeedmph=4&windgustmph=6&baromin=30.03"), t, 1000, 0, SrcIf::Ap, true, tz);
  CHECK(!r->sensor_stale && r->hourly.acc().t_n == tn + 1);
  // a hand-built version 7 file: read, fields equal, the stale tail zero
  {
    std::string f(SNAP_HEADER_SIZE + 2 * sizeof(SnapEntryV7), '\0');
    memcpy(&f[0], "FCS7", 4);
    f[4] = 7;
    f[5] = 2;
    putLe(f, 6, (uint32_t)sizeof(SnapEntryV7), 2);
    putLe(f, 8, snapFingerprintV7(), 4);
    static SnapEntry two[2];
    two[0] = e[0];
    two[1] = e[0];
    memcpy(two[1].id, "SV2", 4);
    memcpy(&f[SNAP_HEADER_SIZE], &two[0], sizeof(SnapEntryV7));
    memcpy(&f[SNAP_HEADER_SIZE + sizeof(SnapEntryV7)], &two[1], sizeof(SnapEntryV7));
    putLe(f, 12, crc32(&f[SNAP_HEADER_SIZE], 2 * sizeof(SnapEntryV7)), 4);
    fs.files[SNAP_PATH] = f;
    static SnapEntry rd[RING_SLOTS];
    memset(rd, 0xAB, sizeof rd);
    ver = 0;
    static const SnapStale zero = {};
    if (RING_SLOTS > 1) {
      CHECK(snapRead(fs, rd, RING_SLOTS, &ver) == 2 && ver == 7);
      bool same = true;
      for (int i = 0; i < 2; i++)
        same = same && memcmp(&rd[i], &two[i], sizeof(SnapEntryV7)) == 0 && memcmp(&rd[i].stale, &zero, sizeof zero) == 0;
      CHECK(same);
    } else {
      CHECK(snapRead(fs, rd, RING_SLOTS, &ver) == -1);  // a two-entry file on a one-ring profile is not ours
      putLe(f, 5, 1, 1);  // the same file with one entry (the WROOM-32's): read as v7
      f.resize(SNAP_HEADER_SIZE + sizeof(SnapEntryV7));
      memcpy(&f[SNAP_HEADER_SIZE], &two[1], sizeof(SnapEntryV7));
      putLe(f, 12, crc32(&f[SNAP_HEADER_SIZE], sizeof(SnapEntryV7)), 4);
      fs.files[SNAP_PATH] = f;
      CHECK(snapRead(fs, rd, RING_SLOTS, &ver) == 1 && ver == 7 && memcmp(&rd[0], &two[1], sizeof(SnapEntryV7)) == 0);
    }
    static SnapEntry one;
    memset(&one, 0xAB, sizeof one);
    ver = 0;
    CHECK(snapFind(fs, "SV2", &one, &ver) && ver == 7 && memcmp(&one, &two[1], sizeof(SnapEntryV7)) == 0 &&
          memcmp(&one.stale, &zero, sizeof zero) == 0);
    // a v7 entry restores with no stale state
    static Stations b2;
    b2.reserve(MAX_STATIONS);
    Station* q = b2.findOrAdd("SV2", defaultStationConfig());
    applySnapEntry(*q, one, 0);
    CHECK(!q->sensor_init && !q->sensor_stale && q->live.has(F_TEMP_C));
    // version 9 (a later image): refused
    fs.files[SNAP_PATH][4] = 9;
    CHECK(snapRead(fs, rd, RING_SLOTS) == -1 && !snapFind(fs, "SV2", &one));
  }
}

// the station feeds raw values (before the offsets), the quality flags and the channels
static void testStationHistoryInputs() {
  static Stations all;
  all.reserve(MAX_STATIONS);
  static Stations all2;  // two slots per Stations, the 3rd+ station here
  all2.reserve(MAX_STATIONS);
  PosixTz tz;
  parsePosixTz("UTC0", &tz);
  StationConfig cfg = defaultStationConfig();
  cfg.temp_c_offset = 2.0f;
  cfg.pressure_hpa_offset = 10.0f;
  cfg.wind_dir_offset = 20.0f;
  Station* st = all.findOrAdd("RAW1", cfg);
  const uint32_t t0 = (uint32_t)unixFromUtc(2026, 11, 9, 13, 0, 0);
  Stations::setBootTime(t0 - 100);  // this boot is inside the previous hour, not this one
  Stations::ingest(*st, sampleFrom("ID=RAW1&tempf=50&baromin=29.92&winddir=90&windspeedmph=10&indoortempf=68&indoorhumidity=40"),
                   t0, 1000, 0, SrcIf::Ap, true, tz);
  CHECK_NEAR(st->live.v[F_TEMP_C], 12.0, 0.01);  // live / MQTT stay calibrated
  HourRec open;
  CHECK(st->hourly.openRecord(&open) && open.t_avg == 100 && dirOf(open.wdir_steady) == 90);  // raw: 10.0 °C, 90°
  CHECK_NEAR(open.p_avg / 10.0, 1013.2, 0.11);  // raw: before the +10 hPa offset
  CHECK(!(open.quality >> Q_RESTART & 1u));
  // the channel map: indoor temperature then humidity, version 1, the record carries it
  CHECK(st->chmap.ver == 1 && st->ch_dirty && strcmp(st->chmap.key[0], "indoor_temp_c") == 0 &&
        strcmp(st->chmap.key[1], "indoor_humidity") == 0 && open.ch_ver == 1 && open.ch_avg[0] == 200 && open.ch_avg[1] == 400);  // FIELDS scales (10, 10)
  CHECK(!Stations::updateChannels(*st));  // nothing new: the map stays
  // the restart bit when the boot is inside the reading's hour; clock_approx from a browser seed
  Stations::setBootTime(t0 + 3600 + 5);
  Stations::setClockApprox(true);
  Stations::ingest(*st, sampleFrom("ID=RAW1&tempf=51"), t0 + 3600 + 60, 61000, 0, SrcIf::Ap, true, tz);
  CHECK(st->hourly.size() == 1 && st->hourly.openRecord(&open) && (open.quality >> Q_RESTART & 1u) &&
        (open.quality >> Q_CLOCK_APPROX & 1u) && (open.quality >> Q_GAP & 1u) == 0);
  Stations::setBootTime(0);
  Stations::setClockApprox(false);
  // a daily-counter reset away from local midnight is the console's (console_reset)
  Stations::ingest(*st, sampleFrom("ID=RAW1&tempf=51&dailyrainin=0.5"), t0 + 3600 + 120, 121000, 0, SrcIf::Ap, true, tz);
  Stations::ingest(*st, sampleFrom("ID=RAW1&tempf=51&dailyrainin=0.01"), t0 + 3600 + 180, 181000, 0, SrcIf::Ap, true, tz);
  CHECK(st->hourly.openRecord(&open) && (open.quality >> Q_CONSOLE_RESET & 1u));
  // the channels chosen by hand (`hist`): exactly those keys, a new map version; bad keys are reported
  Errors e;
  const char* hj = "{\"hist\":[\"indoor_humidity\",\"Bad Key\"]}";
  CHECK(stationFromJson(hj, strlen(hj), &st->cfg, &e) && e.n == 1);
  CHECK(strcmp(st->cfg.hist[0], "indoor_humidity") == 0 && !st->cfg.hist[1][0]);
  const uint8_t ver = st->chmap.ver;
  CHECK(Stations::updateChannels(*st) && st->chmap.ver == ver + 1 && strcmp(st->chmap.key[0], "indoor_humidity") == 0 &&
        !st->chmap.key[1][0] && st->chmap.scale[0] == 10);
  CHECK(!Stations::updateChannels(*st));
  std::string js;
  char jb[64];
  Out oj(toString, &js, jb, sizeof jb);
  stationToJson(oj, st->cfg);
  oj.flush();
  CHECK(contains(js, "\"hist\":[\"indoor_humidity\"]"));
  // a hist change inside the hour waits for the next hour boundary (one map per hour)
  StationConfig wc = defaultStationConfig();
  static Stations allCHB1;  // a slot of its own (one station per Stations on the WROOM-32 profile)
  allCHB1.reserve(MAX_STATIONS);
  Station* ch = allCHB1.findOrAdd("CHB1", wc);
  const uint32_t c0 = (uint32_t)unixFromUtc(2026, 11, 9, 16, 0, 0);
  Stations::ingest(*ch, sampleFrom("ID=CHB1&tempf=50&indoortempf=68"), c0, 1000, 0, SrcIf::Ap, true, tz);
  CHECK(ch->chmap.ver == 1 && strcmp(ch->chmap.key[0], "indoor_temp_c") == 0);
  Stations::ingest(*ch, sampleFrom("ID=CHB1&tempf=50&indoortempf=68&indoorhumidity=40"), c0 + 60, 61000, 0, SrcIf::Ap, true, tz);
  CHECK(ch->chmap.ver == 1 && !ch->chmap.key[1][0]);  // humidity arrived mid-hour: not yet
  Stations::ingest(*ch, sampleFrom("ID=CHB1&tempf=50&indoortempf=68&indoorhumidity=40"), c0 + 3600 + 5, 3605000, 0, SrcIf::Ap, true, tz);
  CHECK(ch->chmap.ver == 2 && strcmp(ch->chmap.key[1], "indoor_humidity") == 0 && ch->hourly.size() == 1 && ch->hourly.at(0).ch_ver == 1);
  // the wind factor: history keeps the unfactored speed, the calm rule judges the factored one
  StationConfig fc1 = defaultStationConfig();
  fc1.wind_factor = 1.5f;
  Station* wf = all2.findOrAdd("WF1", fc1);
  const uint32_t w0 = (uint32_t)unixFromUtc(2026, 11, 9, 18, 0, 0);
  Stations::ingest(*wf, sampleFrom("ID=WF1&windspeedmph=0.5&winddir=200"), w0, 1000, 0, SrcIf::Ap, true, tz);
  Stations::ingest(*wf, sampleFrom("ID=WF1&windspeedmph=0.3&winddir=200"), w0 + 60, 61000, 0, SrcIf::Ap, true, tz);
  CHECK_NEAR(wf->live.v[F_WIND_KMH], 0.3 * 1.609344 * 1.5, 0.01);  // live factored
  HourRec wr;
  CHECK(wf->hourly.openRecord(&wr) && wr.wind_avg == 6);  // raw: (0.80 + 0.48) / 2 = 0.64 km/h
  // 0.80 raw is 1.21 factored: not calm; 0.48 raw is 0.72 factored with a 2-min mean of 0.97: calm
  CHECK(wr.calm_n == 1 && dirOf(wr.wdir_steady) == 200);
  // the rain factor: history raw, live (and MQTT) factored
  StationConfig rc = defaultStationConfig();
  rc.profile = Profile::Generic;  // rainin = the past hour (no rate profile)
  rc.rain_factor = 1.5f;
  static Stations all2RF1;  // a slot of its own (one station per Stations on the WROOM-32 profile)
  all2RF1.reserve(MAX_STATIONS);
  Station* rf = all2RF1.findOrAdd("RF1", rc);
  const uint32_t r0 = (uint32_t)unixFromUtc(2026, 11, 9, 20, 0, 0);
  Stations::ingest(*rf, sampleFrom("ID=RF1&dailyrainin=0.10"), r0, 1000, 0, SrcIf::Ap, true, tz);
  Stations::ingest(*rf, sampleFrom("ID=RF1&dailyrainin=0.20"), r0 + 60, 61000, 0, SrcIf::Ap, true, tz);
  CHECK_NEAR(rf->live.v[F_RAIN_DAILY_MM], 0.20 * 25.4 * 1.5, 0.01);  // live factored
  HourRec rr;
  CHECK(rf->hourly.openRecord(&rr) && rr.rain_sum == 254);  // raw: 0.10 in = 2.54 mm fell, not 3.81
}


static void testSnapTimer() {
  SnapTimer t;
  const uint32_t P = SnapTimer::PERIOD_MS;
  t.written(1000, 5);
  CHECK(!t.due(1000 + P, 5));          // unchanged: no write, however long
  CHECK(!t.due(1000 + P - 1, 6));      // changed, but less than 10 min
  CHECK(t.due(1000 + P, 6));           // changed and 10 min
  t.written(1000 + P, 6);
  CHECK(!t.due(1000 + 2 * P, 6) && t.due(1000 + 2 * P, 7));
  t.written(0xFFFFFF00u, 7);           // millis() wraps: still measured as a difference
  CHECK(!t.due(0xFFFFFF00u + P - 1, 8) && t.due(0xFFFFFF00u + P, 8));
}

static void testBatteryBinarySensor() {
  const uint8_t mac[6] = {0x02, 0x00, 0x00, 0x00, 0x00, 0x01};  // locally administered, nobody's device
  const EntitySpec e = fieldSpec(F_BATTERY_LOW);
  char topic[128], payload[896];
  CHECK(discoveryTopicKey(topic, sizeof topic, "ST1", mac, e.key, e.component) > 0);
  CHECK(contains(topic, "homeassistant/binary_sensor/fauxcloud_020000000001_ST1/battery_low/config"));
  CHECK(discoveryPayloadSpec(payload, sizeof payload, "ST1", "Garden", mac, e, "fauxcloud test") > 0);
  CHECK(contains(payload, "\"device_class\":\"battery\"") && contains(payload, "\"payload_on\":\"1\",\"payload_off\":\"0\""));
  CHECK(!contains(payload, "unit_of_measurement") && !contains(payload, "state_class") && !contains(payload, "precision"));
  CHECK(contains(payload, "\"state_topic\":\"fauxcloud/0001/ST1/battery_low\""));
  char v[16];
  CHECK(formatValue(v, sizeof v, F_BATTERY_LOW, 1.f) > 0 && strcmp(v, "1") == 0);  // matches payload_on
  const EntitySpec t = fieldSpec(F_TEMP_C);
  CHECK(t.component == nullptr && discoveryTopicKey(topic, sizeof topic, "ST1", mac, t.key, t.component) > 0 &&
        contains(topic, "homeassistant/sensor/"));
}

// the promoted keys kept per announced id (ghost retraction covers promotions)
static void testAnnPromos() {
  char k1[16], k2[16], keys[ANN_PROMO_MAX][ANN_PROMO_LEN];
  annPromoKey("ST1", k1);
  annPromoKey("ST2", k2);
  CHECK(strlen(k1) == 9 && k1[0] == 'p' && strcmp(k1, k2) != 0);
  CHECK(annPromoParse("soil_moisture,bed,leaf_wetness_number_23x", keys, ANN_PROMO_MAX) == 3 &&
        strcmp(keys[0], "soil_moisture") == 0 && strcmp(keys[2], "leaf_wetness_number_23x") == 0);
  CHECK(annPromoParse("", keys, ANN_PROMO_MAX) == 0 && annPromoParse("a,b,c,d,e", keys, ANN_PROMO_MAX) == 4);
  CHECK(annPromoParse("a,,b", keys, ANN_PROMO_MAX) == 2 && strcmp(keys[1], "b") == 0);
  // a promotion removed in admin: the old set minus the new one is retracted
  char gone[ANN_PROMO_MAX][ANN_PROMO_LEN];
  CHECK(annPromoGone("soil,bed,leaf", "bed", gone, ANN_PROMO_MAX) == 2 && strcmp(gone[0], "soil") == 0 &&
        strcmp(gone[1], "leaf") == 0);
  CHECK(annPromoGone("soil", "", gone, ANN_PROMO_MAX) == 1 && annPromoGone("", "soil", gone, ANN_PROMO_MAX) == 0);
  CHECK(annPromoGone("soil,bed", "bed,soil", gone, ANN_PROMO_MAX) == 0);  // order does not matter
}

static void testKnownParameters() {
  static Stations all;
  all.reserve(MAX_STATIONS);
  PosixTz tz;
  parsePosixTz("UTC0", &tz);
  Station* s = all.findOrAdd("KP1", defaultStationConfig());
  CHECK(s != nullptr);
  const uint32_t t0 = (uint32_t)unixFromUtc(2026, 11, 3, 9, 0, 0);
  Stations::ingest(*s, sampleFrom("ID=KP1&tempf=50&windspeedmph=5&humidity=60&soilmoisture=41"), t0, 1000, 0, SrcIf::Ap,
                   true, tz);
  // a partial upload (two unknown keys only): the weather fields stay, with their time; only the new ones are fresh
  Stations::ingest(*s, sampleFrom("ID=KP1&tempf=51&leafwetness=3"), t0 + 60, 61000, 0, SrcIf::Ap, true, tz);
  CHECK(s->live.has(F_WIND_KMH) && s->live.has(F_HUMIDITY) && s->field_t[F_WIND_KMH] == t0 && s->field_t[F_TEMP_C] == t0 + 60);
  CHECK((s->fresh & ~s->derived) == (1u << F_TEMP_C));  // what MQTT publishes for this upload (plus the derived wind)
  CHECK(std::fabs(s->live.v[F_TEMP_C] - 10.56f) < .01f);
  // the rare key stays known while the station uploads, with its own time
  int nk = 0;
  for (const KnownKey& k : s->known) nk += k.t != 0;
  CHECK(nk == 2);
  // the live body: values carried, ages before the last upload, the known keys
  static LiveSnap ls;
  snapLive(*s, 61000, &ls);
  std::string j;
  char buf[256];
  Out o(toString, &j, buf, sizeof buf);
  renderLiveOne(o, ls, Clock{t0 + 70, true, 70, 70000}, nullptr);
  o.flush();
  CHECK(contains(j, "\"age\":{\"temp_c\":0,") && contains(j, "\"wind_kmh\":60") && contains(j, "\"humidity\":60"));
  CHECK(contains(j, "\"known\":[{\"k\":\"soilmoisture\",\"v\":\"41\",\"age\":60},{\"k\":\"leafwetness\",\"v\":\"3\",\"age\":0}]"));
  // offline: the values stay (the page dims them), the station is not online
  CHECK(!s->online(61000 + s->cfg.stale_s * 1000UL + 1) && s->live.has(F_WIND_KMH));
  // promoting a known key shows its last value at once, with its time; later uploads without it keep it
  StationConfig c = s->cfg;
  Errors e;
  stationFromJson("{\"promote\":{\"soilmoisture\":{\"name\":\"Bed\",\"type\":\"soil_moisture\"}}}",
                  strlen("{\"promote\":{\"soilmoisture\":{\"name\":\"Bed\",\"type\":\"soil_moisture\"}}}"), &c, &e);
  s->cfg = c;
  Stations::repromote(*s);
  CHECK((s->promoted_mask & 1) && std::fabs(s->promoted[0] - 41.f) < .01f && s->promo_t[0] == t0 && !(s->promo_fresh & 1));
  Stations::ingest(*s, sampleFrom("ID=KP1&tempf=52"), t0 + 120, 121000, 0, SrcIf::Ap, true, tz);
  CHECK((s->promoted_mask & 1) && !(s->promo_fresh & 1) && s->promo_t[0] == t0);
  Stations::ingest(*s, sampleFrom("ID=KP1&tempf=52&soilmoisture=44"), t0 + 180, 181000, 0, SrcIf::Ap, true, tz);
  CHECK((s->promo_fresh & 1) && std::fabs(s->promoted[0] - 44.f) < .01f && s->promo_t[0] == t0 + 180);
  // Forget: gone until sent again
  CHECK(Stations::forgetKey(*s, "leafwetness") && !Stations::forgetKey(*s, "leafwetness"));
  Stations::ingest(*s, sampleFrom("ID=KP1&tempf=52&leafwetness=4"), t0 + 240, 241000, 0, SrcIf::Ap, true, tz);
  bool back = false;
  for (const KnownKey& k : s->known) back |= k.t && strcmp(k.key, "leafwetness") == 0 && strcmp(k.val, "4") == 0;
  CHECK(back);
  // a 9th key evicts the oldest-seen key that is not promoted (soilmoisture is promoted and stays)
  char q[96];
  for (int i = 0; i < 7; i++) {
    snprintf(q, sizeof q, "ID=KP1&k%d=%d", i, i);
    Stations::ingest(*s, sampleFrom(q), t0 + 300 + (uint32_t)i, 300000, 0, SrcIf::Ap, true, tz);
  }
  bool soil = false, leaf = false, k0 = false;
  for (const KnownKey& k : s->known) {
    soil |= k.t && strcmp(k.key, "soilmoisture") == 0;
    leaf |= k.t && strcmp(k.key, "leafwetness") == 0;
    k0 |= k.t && strcmp(k.key, "k0") == 0;
  }
  CHECK(soil && !leaf && k0);  // 8 slots: soilmoisture + k0..k6; leafwetness (oldest, not promoted) went
  // the known keys and ages survive a snapshot (v4)
  RamFs fs;
  static SnapEntry snap, rd;
  takeSnapEntry(*s, &snap);
  CHECK(snapWrite(fs, &snap, 1) && snapRead(fs, &rd, 1) == 1);
  static Stations boot;
  boot.reserve(MAX_STATIONS);
  Station* b = boot.findOrAdd("KP1", defaultStationConfig());
  b->cfg = s->cfg;
  applySnapEntry(*b, rd, 0);
  int nb = 0;
  for (const KnownKey& k : b->known) nb += k.t != 0;
  CHECK(nb == 8 && b->field_t[F_WIND_KMH] == t0 && b->fresh == 0 && b->promo_fresh == 0 && b->promo_t[0] == t0 + 180);
  // re-stamped at the first SNTP sync like the recent ring
  static Stations allKP2;  // a slot of its own (one station per Stations on the WROOM-32 profile)
  allKP2.reserve(MAX_STATIONS);
  Station* p = allKP2.findOrAdd("KP2", defaultStationConfig());
  Stations::ingest(*p, sampleFrom("ID=KP2&tempf=50&soilmoisture=1"), 5, 5000, 0, SrcIf::Ap, false, tz);
  Stations::clockSynced(*p, t0);
  bool restamped = false;
  for (const KnownKey& k : p->known) restamped |= k.t == t0 + 5;
  CHECK(p->field_t[F_TEMP_C] == t0 + 5 && restamped);
}

// ---- robustness -------------------------------------------------------------------------------------------------

// the server name of a TLS ClientHello (setup mode's port-443 logger).
static std::vector<uint8_t> clientHello(const char* host, bool withSni) {
  std::vector<uint8_t> ext = {0x00, 0x0b, 0x00, 0x02, 0x01, 0x00};  // ec_point_formats first
  if (withSni) {
    const uint16_t n = (uint16_t)strlen(host);
    const uint8_t sni[] = {0x00, 0x00, (uint8_t)((n + 5) >> 8), (uint8_t)(n + 5), (uint8_t)((n + 3) >> 8),
                           (uint8_t)(n + 3), 0x00, (uint8_t)(n >> 8), (uint8_t)n};
    ext.insert(ext.end(), sni, sni + sizeof sni);
    ext.insert(ext.end(), host, host + n);
  }
  std::vector<uint8_t> hs = {0x03, 0x03};
  hs.insert(hs.end(), 32, 0xab);                                  // random
  const uint8_t mid[] = {0x20};                                   // 32-byte session id
  hs.insert(hs.end(), mid, mid + 1);
  hs.insert(hs.end(), 32, 0xcd);
  const uint8_t cs[] = {0x00, 0x04, 0x13, 0x01, 0x13, 0x02, 0x01, 0x00};  // 2 suites, 1 compression method
  hs.insert(hs.end(), cs, cs + sizeof cs);
  hs.push_back((uint8_t)(ext.size() >> 8));
  hs.push_back((uint8_t)ext.size());
  hs.insert(hs.end(), ext.begin(), ext.end());
  std::vector<uint8_t> rec = {0x16, 0x03, 0x01, (uint8_t)((hs.size() + 4) >> 8), (uint8_t)(hs.size() + 4), 0x01, 0x00,
                              (uint8_t)(hs.size() >> 8), (uint8_t)hs.size()};
  rec.insert(rec.end(), hs.begin(), hs.end());
  return rec;
}

static void testTlsSni() {
  char out[64];
  std::vector<uint8_t> a = clientHello("connectivitycheck.gstatic.com", true);
  CHECK(tlsSni(a.data(), a.size(), out, sizeof out) && strcmp(out, "connectivitycheck.gstatic.com") == 0);
  char tiny[8];
  CHECK(tlsSni(a.data(), a.size(), tiny, sizeof tiny) && strcmp(tiny, "connect") == 0);  // cut to the buffer
  std::vector<uint8_t> b = clientHello("x", false);
  CHECK(!tlsSni(b.data(), b.size(), out, sizeof out) && out[0] == 0);  // no server_name extension
  for (size_t cut = 0; cut < a.size(); cut++) tlsSni(a.data(), cut, out, sizeof out);  // every truncation is safe
  CHECK(!tlsSni(a.data(), 50, out, sizeof out));
  const uint8_t http[] = "GET / HTTP/1.1\r\nHost: x\r\n\r\n0123456789012345678901234567890123456789";
  CHECK(!tlsSni(http, sizeof http, out, sizeof out));
}

// who may upload (LAN switch, block list) and the console key bound on first use.
static void testTrust() {
  CHECK(uploadDrop(SrcIf::Ap, false, false) == nullptr);                     // the fauxcloud Wi-Fi: always
  CHECK(strcmp(uploadDrop(SrcIf::Sta, false, false), "lan") == 0);           // home network, switch off
  CHECK(uploadDrop(SrcIf::Sta, true, false) == nullptr);                     // switch on
  CHECK(strcmp(uploadDrop(SrcIf::Ap, false, true), "blocked") == 0 && strcmp(uploadDrop(SrcIf::Sta, true, true), "blocked") == 0);
  BridgeConfig b = defaultBridgeConfig();
  CHECK(!b.lan_uploads);
  const char* on = "{\"lan_uploads\":true}";
  Errors e;
  CHECK(bridgeFromJson(on, strlen(on), &b, &e) && e.n == 0 && b.lan_uploads);
  std::string j;
  char buf[64];
  Out o(toString, &j, buf, sizeof buf);
  bridgeToJson(o, b);
  o.flush();
  CHECK(j == "{\"lan_uploads\":true}");
  const char* bad = "{\"lan_uploads\":1}";
  Errors e2;
  CHECK(bridgeFromJson(bad, strlen(bad), &b, &e2) && e2.n == 1 && b.lan_uploads);
  {  // the detailed-log switch, default off, stored only when on, null = default, not a bool = refused
    BridgeConfig v = defaultBridgeConfig();
    CHECK(!v.log_verbose && render(bridgeJsonFn, &v) == "{}");
    const char* von = "{\"log_verbose\":true}";
    Errors ev;
    CHECK(bridgeFromJson(von, strlen(von), &v, &ev) && ev.n == 0 && v.log_verbose && render(bridgeJsonFn, &v) == von);
    const char* vbad = "{\"log_verbose\":\"yes\"}";
    Errors eb;
    CHECK(bridgeFromJson(vbad, strlen(vbad), &v, &eb) && eb.n == 1 && v.log_verbose);
    const char* vnull = "{\"log_verbose\":null}";
    Errors en;
    CHECK(bridgeFromJson(vnull, strlen(vnull), &v, &en) && en.n == 0 && !v.log_verbose);
  }

  static Stations all;
  all.reserve(MAX_STATIONS);
  Station* st = all.findOrAdd("K1", defaultStationConfig());
  uint8_t k1[32], k2[32], none[32];
  memset(k1, 1, sizeof k1);
  memset(k2, 2, sizeof k2);
  memset(none, 0, sizeof none);  // an upload without PASSWORD hashes "" like any other key
  CHECK(!st->key_bound && checkStationKey(*st, k1) == KeyCheck::Bound && st->key_bound);
  CHECK(checkStationKey(*st, k1) == KeyCheck::Match && checkStationKey(*st, k2) == KeyCheck::Mismatch);
  CHECK(memcmp(st->key_hash, k1, sizeof k1) == 0);  // a mismatch never rebinds
  st->key_bound = false;                             // "Accept the new key"
  CHECK(checkStationKey(*st, k2) == KeyCheck::Bound && checkStationKey(*st, k2) == KeyCheck::Match);
  CHECK(all.forget(0) && !all.at(0));
  Station* again = all.findOrAdd("K1", defaultStationConfig());
  CHECK(again && !again->key_bound && checkStationKey(*again, none) == KeyCheck::Bound);  // forget clears it

  // the admin view says bound / mismatches, never the hash
  std::string v;
  Out ov(toString, &v, buf, sizeof buf);
  KeyState ks{true, 3};
  memset(again->key_hash, 0xAB, sizeof again->key_hash);
  renderStationConfig(ov, "K1", true, again->cfg, nullptr, &ks);
  ov.flush();
  CHECK(contains(v, "\"key\":\"bound\",\"key_mismatch\":3") && !contains(v, "abab") && !contains(v, "ABAB"));
}

// ghost stations: the list of announced ids (NVS fc_ann).
static void testAnnList() {
  AnnList a;
  a.parse("ST1,ST2,,ST1,TOOLONGSTATIONID_X,ST3");
  CHECK(a.size() == 3 && a.has("ST1") && a.has("ST3") && !a.has("TOOLONGSTATIONID_X"));
  char s[160];
  CHECK(a.format(s, sizeof s) == 11 && strcmp(s, "ST1,ST2,ST3") == 0);
  CHECK(!a.add("ST2") && a.add("ST4") && a.remove("ST2") && !a.remove("ST2"));
  CHECK(a.format(s, sizeof s) > 0 && strcmp(s, "ST1,ST3,ST4") == 0);
  for (int i = 0; i < 6; i++) {
    char id[8];
    snprintf(id, sizeof id, "X%d", i);
    a.add(id);
  }
  CHECK(a.size() == AnnList::MAX && !a.has("ST1") && a.has("ST3") && a.has("X5"));  // full: the oldest drops
  // the block list: a 9th id is refused and the first stays; a stored id is no change; a freed place is reused
  AnnList b;
  for (int i = 0; i < AnnList::MAX; i++) {
    char id[8];
    snprintf(id, sizeof id, "B%d", i);
    CHECK(b.addIfRoom(id));
  }
  CHECK(!b.addIfRoom("B8") && b.size() == AnnList::MAX && b.has("B0") && !b.has("B8") && !b.addIfRoom("B3"));
  CHECK(b.remove("B0") && b.addIfRoom("B8") && b.has("B8") && !b.addIfRoom("B9"));
  CHECK(a.format(s, 8) == -1);
  AnnList e;
  e.parse("");
  CHECK(e.size() == 0 && e.format(s, sizeof s) == 0 && s[0] == 0);
  e.parse(nullptr);
  CHECK(e.size() == 0 && !e.add("") && e.add("A") && e.size() == 1);
}

static void testMarkerScan() {
  const char* img = "....junk FAUXCLOUD-IMAG FAUXCLOUD-IMAGE:0.1.0-b5 Nov  2 2026 15:00:00\0tail";
  const size_t len = 72;
  for (size_t chunk = 1; chunk <= len; chunk += 7) {  // the marker may span chunk borders
    MarkerScan m;
    for (size_t off = 0; off < len; off += chunk) m.feed((const uint8_t*)img + off, off + chunk > len ? len - off : chunk);
    CHECK(m.found() && strcmp(m.version(), "0.1.0-b5 Nov  2 2026 15:00:00") == 0);
  }
  MarkerScan none;
  const char* other = "arduino-lib-builder FAUXCLOUD IMAGE: FAUXCLOUD-IMAGE";
  none.feed((const uint8_t*)other, strlen(other));
  CHECK(!none.found());
}

static void testBridgeStatusOta() {
  BridgeStatus bs = {};
  bs.mode = "run";
  bs.ota = true;
  std::string s;
  char buf[64];
  Out o(toString, &s, buf, sizeof buf);
  renderBridgeStatus(o, bs);
  o.flush();
  CHECK(contains(s, "\"heap_low\":0,\"time_src\":\"none\",\"ota\":1,\"ids\":[]"));
}

static void testRollbackAndWatchdogs() {
  RollbackInputs in = {true, true, true, 60, false};
  CHECK(shouldMarkValid(in));
  in.uptime_s = 59;
  CHECK(!shouldMarkValid(in));
  in = {true, false, true, 600, false};
  CHECK(!shouldMarkValid(in));  // no home Wi-Fi
  in = {true, true, false, 600, false};
  CHECK(!shouldMarkValid(in));  // nothing served yet
  in = {true, true, true, 600, true};
  CHECK(!shouldMarkValid(in));  // came back from a watchdog
  in = {false, true, true, 600, false};
  CHECK(!shouldMarkValid(in));  // a stable image needs nothing
  CHECK(strcmp(otaStateName(OtaState::RolledBack), "rolled_back") == 0 && strcmp(otaStateName(OtaState::Stable), "stable") == 0);

  {  // page-session cap: the 7th open page session closes the idlest other one; a closing one is not counted
    PageSessions p;
    for (int fd = 50; fd < 56; fd++) CHECK(p.touch(fd, 1000u * (uint32_t)fd) == -1);
    CHECK(p.touch(52, 60000) == -1);  // a known fd: just refreshed
    CHECK(p.touch(56, 61000) == 50);  // 7 open -> the idlest (50, at 50 s)
    CHECK(p.touch(57, 62000) == 51);  // 50 is closing, not counted: 7 open again -> 51
    p.drop(50);
    p.drop(51);
    CHECK(p.count() == 6 && p.touch(56, 63000) == -1);
    CHECK(p.touch(58, 64000) == 53);  // 52 was refreshed at 60 s, so 53 (53 s) is the idlest
    PageSessions w;  // millis() wrap: the idlest is the one touched longest ago
    for (int fd = 1; fd <= 6; fd++) CHECK(w.touch(fd, 0xFFFFF000u + (uint32_t)fd) == -1);
    CHECK(w.touch(1, 5) == -1 && w.touch(7, 10) == 2);
  }
  {  // heap-fragmentation watchdog
    FragWatch f;
    const uint32_t L = FragWatch::FRAG_LARGEST_MIN, H = FragWatch::FRAG_HOLD_MS;
    CHECK(!f.feed(0, L - 1, 5, false) && !f.feed(H - 1, L - 1, 5, false) && f.feed(H, L - 1, 5, false));
    CHECK(!f.feed(H + 1000, L - 1, 5, false));  // fired once, a new window starts
    FragWatch g;  // an upload inside the window restarts it
    CHECK(!g.feed(0, 100, 5, false) && !g.feed(30000, 100, 6, false) && !g.feed(H, 100, 6, false) &&
          !g.feed(30000 + H - 1, 100, 6, false) && g.feed(30000 + H, 100, 6, false));
    FragWatch h;  // a recovery (largest back >= L) clears it
    CHECK(!h.feed(0, 100, 5, false) && !h.feed(50000, L, 5, false) && !h.feed(60000, 100, 5, false) &&
          !h.feed(60000 + H - 1, 100, 5, false) && h.feed(60000 + H, 100, 5, false));
    FragWatch o;  // never during an OTA
    CHECK(!o.feed(0, 100, 5, false) && !o.feed(H, 100, 5, true) && !o.feed(H + 10000, 100, 5, false) &&
          !o.feed(2 * H + 9999, 100, 5, false) && o.feed(2 * H + 10000, 100, 5, false));
    CHECK(!FragWatch().feed(0, L, 0, false));
    CHECK(H == 180000);
    FragWatch q;  // after a heap_frag boot, no restart within 30 min; the first one after it
    q.quiet(FragWatch::FRAG_QUIET_MS);
    CHECK(!q.feed(0, 100, 5, false) && !q.feed(H, 100, 5, false) && !q.feed(FragWatch::FRAG_QUIET_MS - 1, 100, 5, false) &&
          q.feed(FragWatch::FRAG_QUIET_MS, 100, 5, false));
    // shedding: pages only, below 8 KB
    CHECK(shedRequest(false, SHED_LARGEST_MIN - 1) && !shedRequest(false, SHED_LARGEST_MIN) &&
          !shedRequest(true, 0) && !shedRequest(true, SHED_LARGEST_MIN - 1));
  }
  NetWatchdog w;
  CHECK(w.feed(0, true, false) == NetAction::None);
  CHECK(w.feed(100, false, false) == NetAction::None);  // went down at 100
  CHECK(w.feed(100 + NetWatchdog::REINIT_S - 1, false, false) == NetAction::None);
  CHECK(w.feed(100 + NetWatchdog::REINIT_S, false, false) == NetAction::Reinit);
  CHECK(w.feed(100 + NetWatchdog::REINIT_S + 5, false, false) == NetAction::None);  // then every 15 min
  CHECK(w.feed(100 + 2 * NetWatchdog::REINIT_S, false, false) == NetAction::Reinit && w.reinits() == 2);
  CHECK(w.feed(100 + NetWatchdog::REBOOT_S, false, false) == NetAction::Reboot && w.rebooted());
  // one restart per outage -- after it (flag from NVS at boot) only re-inits, every 15 min
  NetWatchdog after;
  after.setRebooted(true);
  after.feed(0, false, false);
  CHECK(after.feed(NetWatchdog::REBOOT_S, false, false) != NetAction::Reboot);
  CHECK(after.feed(5 * NetWatchdog::REBOOT_S, false, false) == NetAction::Reinit);
  CHECK(after.feed(5 * NetWatchdog::REBOOT_S + 60, false, false) == NetAction::None);
  CHECK(after.feed(5 * NetWatchdog::REBOOT_S + NetWatchdog::REINIT_S, false, false) == NetAction::Reinit);
  CHECK(after.feed(6 * NetWatchdog::REBOOT_S, true, false) == NetAction::None && !after.rebooted());  // joined
  after.feed(7 * NetWatchdog::REBOOT_S, false, false);  // the next outage restarts once again
  CHECK(after.feed(8 * NetWatchdog::REBOOT_S, false, false) == NetAction::Reboot);
  NetWatchdog p;
  p.feed(0, false, false);
  CHECK(p.feed(NetWatchdog::REBOOT_S, false, true) == NetAction::None);  // paused (OTA, Wi-Fi change): timer restarts
  CHECK(p.feed(NetWatchdog::REBOOT_S + 10, false, false) == NetAction::None);
  CHECK(p.feed(NetWatchdog::REBOOT_S + 10 + NetWatchdog::REINIT_S, false, false) == NetAction::Reinit);
  CHECK(p.feed(NetWatchdog::REBOOT_S + 20 + NetWatchdog::REINIT_S, true, false) == NetAction::None);  // back up

  CHECK(strcmp(resetReasonName(6, nullptr), "task_wdt") == 0 && strcmp(resetReasonName(1, ""), "power_on") == 0);
  CHECK(strcmp(resetReasonName(3, "sta_lost"), "sta_lost") == 0 && strcmp(resetReasonName(4, "ota"), "panic") == 0);
  CHECK(strcmp(resetReasonName(99, nullptr), "unknown") == 0 && resetIsWatchdog(6) && !resetIsWatchdog(3) && !resetIsWatchdog(7));

  // power-on detection: no RTC magic = power was cut (the DevKit's cold boot reports the ROM's RTC WDT, 7)
  CHECK(effectiveResetReason(7, false) == 1 && effectiveResetReason(1, false) == 1 && effectiveResetReason(0, false) == 1);
  CHECK(effectiveResetReason(7, true) == 7 && effectiveResetReason(1, true) == 1 && effectiveResetReason(2, true) == 2);
  const int keep[] = {3, 4, 5, 6, 9};
  for (int r : keep) CHECK(effectiveResetReason(r, false) == r && effectiveResetReason(r, true) == r);
  CHECK(strcmp(resetReasonName(effectiveResetReason(7, false), nullptr), "power_on") == 0);
  CHECK(!resetIsWatchdog(effectiveResetReason(7, false)));
}

static void testBridgeDevice() {
  const uint8_t mac[6] = {0x24, 0x6f, 0x28, 0xaa, 0xbb, 0xcc};
  char topic[128], payload[896];
  CHECK(bridgeDiscoveryTopic(topic, sizeof topic, mac, "rssi") > 0);
  CHECK(strcmp(topic, "homeassistant/sensor/fauxcloud_bridge_246f28aabbcc/rssi/config") == 0);
  CHECK(bridgeStateTopic(topic, sizeof topic, mac, "rssi") > 0 && strcmp(topic, "fauxcloud/bridge/246f28aabbcc/rssi") == 0);
  for (int i = 0; i < B_COUNT; i++)
    CHECK(bridgeDiscoveryPayload(payload, sizeof payload, mac, bridgeSpec(i), "fauxcloud 0.1.0-b4",
                                 "http://192.168.1.20/") > 0);
  CHECK(bridgeDiscoveryPayload(payload, sizeof payload, mac, bridgeSpec(B_RSSI), "v", "http://192.168.1.20/") > 0);
  std::string p = payload;
  CHECK(contains(p, "\"name\":\"Wi-Fi signal\",\"unit_of_measurement\":\"dBm\",\"device_class\":\"signal_strength\""));
  CHECK(contains(p, "\"entity_category\":\"diagnostic\""));
  CHECK(contains(p, "\"availability_topic\":\"fauxcloud/bridge/246f28aabbcc/status\""));
  CHECK(contains(p, "\"device\":{\"identifiers\":[\"fauxcloud_bridge_246f28aabbcc\"],\"name\":\"fauxcloud bridge BBCC\""));
  CHECK(contains(p, "\"configuration_url\":\"http://192.168.1.20/\"}}"));
  CHECK(bridgeDiscoveryPayload(payload, sizeof payload, mac, bridgeSpec(B_FIRMWARE), "v", nullptr) > 0);
  CHECK(!contains(payload, "configuration_url") && !contains(payload, "unit_of_measurement"));
  CHECK(bridgeDiscoveryPayload(payload, sizeof payload, mac, bridgeSpec(B_LAST_RESTART), "v", nullptr) > 0);
  CHECK(contains(payload, "{\"name\":\"Last restart\",\"device_class\":\"timestamp\",\"entity_category\":\"diagnostic\""));
  // station devices hang off the bridge and carry the same URL; promotions are named by the user
  CHECK(discoveryPayload(payload, sizeof payload, "ST1", "Garden", mac, F_TEMP_C, "v", "http://192.168.1.20/") > 0);
  p = payload;
  CHECK(contains(p, "\"via_device\":\"fauxcloud_bridge_246f28aabbcc\",\"configuration_url\":\"http://192.168.1.20/\"}}"));
  Errors e;
  StationConfig c = mergeStation("{\"promote\":{\"soilmoisture\":{\"name\":\"Bed \\\"A\\\"\",\"type\":\"soil_moisture\"}}}", &e);
  CHECK(e.n == 0);
  CHECK(discoveryPayloadSpec(payload, sizeof payload, "ST1", "Garden", mac, promoteSpec(c.promote[0]), "v") > 0);
  p = payload;
  CHECK(contains(p, "{\"name\":\"Bed \\\"A\\\"\",\"unit_of_measurement\":\"%\",\"device_class\":\"moisture\""));
  CHECK(contains(p, "\"suggested_display_precision\":0") && contains(p, "\"state_topic\":\"fauxcloud/BBCC/ST1/bed_a\""));
}

// ---------------------------------------------------------------- the body reader on the only worker
struct FakeSock {
  std::vector<int> script;  // per recv: n > 0 bytes, BODY_RECV_TIMEOUT, or <= 0 closed
  size_t at = 0;
  uint32_t ms = 0, stepMs = 0;  // the clock advances stepMs per recv (and 5000 per timeout)
};
static int fakeRecv(void* ctx, char* buf, size_t len) {
  FakeSock* s = static_cast<FakeSock*>(ctx);
  const int r = s->at < s->script.size() ? s->script[s->at++] : 0;
  s->ms += r == BODY_RECV_TIMEOUT ? 5000 : s->stepMs;
  if (r > 0) {
    const size_t n = (size_t)r < len ? (size_t)r : len;
    memset(buf, 'x', n);
    return (int)n;
  }
  return r;
}
static uint32_t fakeMs(void* ctx) { return static_cast<FakeSock*>(ctx)->ms; }

static void testReadBody() {
  char buf[33];
  size_t got = 0;
  FakeSock exact{{10, 6}, 0, 0, 0};
  CHECK(readAll(fakeRecv, fakeMs, &exact, buf, 32, 16, &got) == BodyRead::Ok && got == 16 && buf[16] == 0);
  FakeSock none{{}, 0, 0, 0};
  CHECK(readAll(fakeRecv, fakeMs, &none, buf, 32, 0, &got) == BodyRead::Ok && got == 0 && buf[0] == 0);
  // announced, never sent: two timeouts (~10 s) and out
  FakeSock stall{{BODY_RECV_TIMEOUT, BODY_RECV_TIMEOUT, 16}, 0, 0, 0};
  CHECK(readAll(fakeRecv, fakeMs, &stall, buf, 32, 16, &got) == BodyRead::Stalled && got == 0 && stall.at == 2);
  // one timeout between data is fine
  FakeSock blip{{8, BODY_RECV_TIMEOUT, 8}, 0, 0, 0};
  CHECK(readAll(fakeRecv, fakeMs, &blip, buf, 32, 16, &got) == BodyRead::Ok && got == 16);
  // a drip (a byte every 4 s, never a timeout) ends at the 6 s deadline
  FakeSock drip{{1, 1, 1, 1, 1, 1, 1, 1, 1, 1}, 0, 0, 4000};
  CHECK(readAll(fakeRecv, fakeMs, &drip, buf, 32, 16, &got) == BodyRead::Stalled && got == 2);
  // two timeouts in the whole body, data between them does not reset the count
  FakeSock two{{4, BODY_RECV_TIMEOUT, 4, BODY_RECV_TIMEOUT, 8}, 0, 0, 0};
  CHECK(readAll(fakeRecv, fakeMs, &two, buf, 32, 16, &got) == BodyRead::Stalled && got == 8);
  // short body: the peer closes early
  FakeSock shrt{{5, 0}, 0, 0, 0};
  CHECK(readAll(fakeRecv, fakeMs, &shrt, buf, 32, 16, &got) == BodyRead::Lost && got == 5);
  // over the cap: nothing read at all
  FakeSock big{{16}, 0, 0, 0};
  CHECK(readAll(fakeRecv, fakeMs, &big, buf, 32, 3000, &got) == BodyRead::TooBig && got == 0 && big.at == 0);
  // the clock wraps during the read
  FakeSock wrap{{8, 8}, 0, 0xFFFFF000u, 1000};
  CHECK(readAll(fakeRecv, fakeMs, &wrap, buf, 32, 16, &got) == BodyRead::Ok);
  CHECK(strcmp(bodyReadName(BodyRead::Stalled), "stalled") == 0);
  // a too-big body within drain_max is read to its end and discarded under the same rules
  FakeSock over{{32, 32, 32, 4}, 0, 0, 0};
  CHECK(readAll(fakeRecv, fakeMs, &over, buf, 32, 100, &got, 1000) == BodyRead::Drained && got == 100);
  FakeSock overBig{{32}, 0, 0, 0};
  CHECK(readAll(fakeRecv, fakeMs, &overBig, buf, 32, 2000, &got, 1000) == BodyRead::TooBig && overBig.at == 0);
  // the drip that held the worker: 3000 B, then a byte every 2 s -> stalled at the 6 s deadline, never "drained"
  FakeSock dripBig{{32, 1, 1, 1, 1, 1, 1, 1, 1}, 0, 0, 2000};
  CHECK(readAll(fakeRecv, fakeMs, &dripBig, buf, 32, 10000, &got, 16384) == BodyRead::Stalled && got < 10000);
  FakeSock drainStall{{32, BODY_RECV_TIMEOUT, BODY_RECV_TIMEOUT}, 0, 0, 0};
  CHECK(readAll(fakeRecv, fakeMs, &drainStall, buf, 32, 100, &got, 1000) == BodyRead::Stalled && got == 32);
}

// the side of a request needs the local and the peer address to agree
static uint32_t ipN(int a, int b, int c, int d) {
  const uint8_t x[4] = {(uint8_t)a, (uint8_t)b, (uint8_t)c, (uint8_t)d};
  uint32_t v;
  memcpy(&v, x, 4);
  return v;
}
static void testClassifySrc() {
  const uint32_t ap = ipN(192, 168, 77, 1), sta = ipN(10, 1, 2, 3);
  CHECK(classifySrc(ap, ipN(192, 168, 77, 2), ap) == SrcIf::Ap);       // console on the AP
  CHECK(classifySrc(sta, ipN(10, 1, 2, 40), ap) == SrcIf::Sta);        // a LAN browser
  CHECK(classifySrc(sta, ipN(192, 168, 77, 2), ap) == SrcIf::Mixed);   // AP client -> http://<STA IP>/
  CHECK(classifySrc(ap, ipN(10, 1, 2, 40), ap) == SrcIf::Mixed);       // LAN host routed to 192.168.77.1
  CHECK(classifySrc(0, ipN(10, 1, 2, 40), ap) == SrcIf::Unknown);
  CHECK(classifySrc(sta, ipN(192, 168, 78, 2), ap) == SrcIf::Sta);     // a neighbouring /24 is the LAN
  CHECK(strcmp(srcIfName(SrcIf::Mixed), "mixed") == 0);
  CHECK(strcmp(uploadDrop(SrcIf::Mixed, true, false), "lan") == 0);   // dropped even with LAN uploads on
  CHECK(uploadDrop(SrcIf::Ap, false, false) == nullptr);
  // the admin gate. AP / Mixed / Unknown: always the catch-all; STA without the CSRF header: the
  // catch-all (invisible to probes); STA + header without a valid session: 401; with one, or a sessionless route: pass
  for (SrcIf v : {SrcIf::Ap, SrcIf::Mixed, SrcIf::Unknown})
    for (int f = 0; f < 8; f++) CHECK(adminGate(v, f & 1, f & 2, f & 4) == AdminGate::CatchAll);
  CHECK(adminGate(SrcIf::Sta, false, true, false) == AdminGate::CatchAll && adminGate(SrcIf::Sta, false, false, false) == AdminGate::CatchAll);
  CHECK(adminGate(SrcIf::Sta, true, true, false) == AdminGate::Unauth);
  // the AP catch-all is a run-mode rule only; setup mode serves its page and assets on the AP
  CHECK(!apCatchAllApplies(SrcIf::Ap, true) && apCatchAllApplies(SrcIf::Ap, false));
  for (SrcIf v : {SrcIf::Sta, SrcIf::Mixed, SrcIf::Unknown}) CHECK(!apCatchAllApplies(v, true) && !apCatchAllApplies(v, false));
  CHECK(adminGate(SrcIf::Sta, true, true, true) == AdminGate::Pass && adminGate(SrcIf::Sta, true, false, false) == AdminGate::Pass);
  // sessionOk as the gate computes it: a token from the same IP passes; another IP, idle expiry, revoke-all -> 401
  static SessionStore ss;
  ss.clear();
  const uint8_t tr[16] = {1, 2, 3}, ir[4] = {9};
  char tok[33];
  const uint32_t lan = ipN(10, 1, 2, 40), t0 = 1000;
  ss.create(tr, ir, lan, "test", t0, tok);
  CHECK(adminGate(SrcIf::Sta, true, true, ss.check(tok, lan, t0 + 5)) == AdminGate::Pass);
  CHECK(adminGate(SrcIf::Sta, true, true, ss.check(tok, ipN(10, 1, 2, 41), t0 + 6)) == AdminGate::Unauth);
  CHECK(adminGate(SrcIf::Sta, true, true, ss.check(tok, lan, t0 + 6 + SessionStore::IDLE_S)) == AdminGate::Unauth);
  ss.create(tr, ir, lan, "test", t0, tok);
  ss.clear();  // revoke-all
  CHECK(adminGate(SrcIf::Sta, true, true, ss.check(tok, lan, t0 + 7)) == AdminGate::Unauth);
  // the parked list -- dedupe, cap 4 + dropped count, remove keeps the order
  ParkedList pk = {};
  pk.add("ST3");
  pk.add("ST3");
  pk.add("");
  CHECK(pk.n == 1 && pk.has("ST3") && !pk.has("ST4"));
  pk.add("ST4"), pk.add("ST5"), pk.add("ST6"), pk.add("ST7");
  CHECK(pk.n == 4 && pk.dropped == 1 && !pk.has("ST7"));
  CHECK(pk.remove("ST4") && pk.n == 3 && strcmp(pk.ids[0], "ST3") == 0 && strcmp(pk.ids[1], "ST5") == 0 &&
        strcmp(pk.ids[2], "ST6") == 0 && !pk.remove("ST4"));
  // a parked id that takes a slot (a freed slot, a station PUT) is no longer listed
  static Stations ps;
  ps = Stations();
  ps.reserve(MAX_STATIONS);
  CHECK(parkedWithoutSlot(pk, ps).n == 3);
  ps.findOrAdd("ST5", defaultStationConfig());
  const ParkedList free = parkedWithoutSlot(pk, ps);
  CHECK(free.n == 2 && free.has("ST3") && !free.has("ST5") && free.has("ST6") && free.dropped == 1);
}

static void testParserB5e() {
  Sample s;
  parseQuery("ID=X1&baromin=1e308&winddir=1e40&tempf=50&windspeedmph=1e39&visibility=500&humidity=60", &s);
  CHECK(!s.has(F_PRESSURE_HPA) && !s.has(F_WIND_DIR) && !s.has(F_WIND_KMH) && s.has(F_TEMP_C) && s.has(F_HUMIDITY));
  CHECK((s.rejected_bounds >> F_PRESSURE_HPA) & 1u);
  CHECK((s.rejected_bounds >> F_WIND_DIR) & 1u);
  CHECK((s.rejected_bounds >> F_WIND_KMH) & 1u);
  CHECK(fieldByKey("visibility_km") >= 0 && ((s.rejected_bounds >> fieldByKey("visibility_km")) & 1u));
  parseQuery("baromin=29.92&winddir=360&visibility=10", &s);
  CHECK(s.has(F_PRESSURE_HPA) && s.has(F_WIND_DIR) && s.has(fieldByKey("visibility_km")));
  parseQuery("baromin=8.8", &s);  // 298 hPa: below the range
  CHECK(!s.has(F_PRESSURE_HPA));
  char v[16];
  CHECK(formatValue(v, sizeof v, F_TEMP_C, INFINITY) < 0 && formatValue(v, sizeof v, F_TEMP_C, NAN) < 0);
  // the live JSON stays valid with the hostile upload
  static Stations all;
  all.reserve(MAX_STATIONS);
  PosixTz tz;
  parsePosixTz("UTC0", &tz);
  Station* st = all.findOrAdd("X1", defaultStationConfig());
  parseQuery("ID=X1&baromin=1e308&winddir=1e40&tempf=50", &s);
  Stations::ingest(*st, s, 1000, 1000, 0, SrcIf::Ap, false, tz);
  LiveSnap ls;
  snapLive(*st, 1000, &ls);
  std::string body;
  char buf[128];
  Out o(toString, &body, buf, sizeof buf);
  renderLiveOne(o, ls, Clock{1000, false, 1000, 1000}, nullptr);
  o.flush();
  CHECK(!contains(body, "inf") && !contains(body, "nan") && contains(body, "\"temp_c\":"));
  // ID / PASSWORD in any case: never extras
  parseQuery("Id=K1&password=secret&Password=x&tempf=50", &s);
  CHECK(strcmp(s.station_id, "K1") == 0 && s.n_extras == 0);
  char pw[96];
  queryValueCi("ID=K1&password=se%26cret&tempf=50", "PASSWORD", pw, sizeof pw);
  CHECK(strcmp(pw, "se%26cret") == 0);  // raw, as sent
  queryValueCi("ID=K1&tempf=50", "PASSWORD", pw, sizeof pw);
  CHECK(pw[0] == 0);
  queryValueCi("PASSWORDX=1&PASSWORD=", "PASSWORD", pw, sizeof pw);
  CHECK(pw[0] == 0);
  std::string longKey = "PASSWORD=" + std::string(150, 'k');
  queryValueCi(longKey.c_str(), "PASSWORD", pw, sizeof pw);
  CHECK(strlen(pw) == sizeof pw - 1);  // cut, never empty
}

// an upload stamped with seconds since boot never opens an hour, even when the caller says the clock
// is synced (the sync landed between two reads of the clock); nor does a tick
static void testNo1970Hour() {
  PosixTz tz;
  parsePosixTz("UTC0", &tz);
  static Stations all;
  all.reserve(MAX_STATIONS);
  Station* st = all.findOrAdd("C1", defaultStationConfig());
  IngestResult r = Stations::ingest(*st, sampleFrom("ID=C1&tempf=50"), 120, 120000, 0, SrcIf::Ap, true, tz);
  HourRec h;
  CHECK(!r.hour_closed && !st->hourly.openRecord(&h) && st->recent.size() == 1);
  const TickResult t = Stations::tick(*st, 4000, tz);
  CHECK(!t.hour_closed && !t.day_closed && st->hourly.size() == 0);
  const uint32_t real = (uint32_t)unixFromUtc(2026, 11, 3, 10, 5, 0);
  r = Stations::ingest(*st, sampleFrom("ID=C1&tempf=51"), real, 121000, 0, SrcIf::Ap, true, tz);
  CHECK(st->hourly.openRecord(&h) && h.hour == real - 300);
}

// the setup time box
static void testSetupIdle() {
  const uint32_t L = 30UL * 60UL * 1000UL;
  CHECK(setupIdleAction(L, L, true) == SetupIdle::Wait && setupIdleAction(L - 1, L, false) == SetupIdle::Wait);
  CHECK(setupIdleAction(L + 1, L, true) == SetupIdle::BackToRun);
  CHECK(setupIdleAction(L + 1, L, false) == SetupIdle::Restart);
}

// stored on its first accepted upload when auto-publish is on; the MAC is admin-only
static void testStoreOnUpload() {
  CHECK(storeOnUpload(false, true) && !storeOnUpload(true, true) && !storeOnUpload(false, false));
  KeyState k = {true, 0, true, {0x02, 0, 0, 0, 0, 0x2a}};
  std::string s;
  char buf[64];
  Out o(toString, &s, buf, sizeof buf);
  renderStationConfig(o, "M1", true, defaultStationConfig(), nullptr, &k);
  o.flush();
  CHECK(contains(s, "\"src_mac\":\"02:00:00:00:00:2a\"") && contains(s, "\"cal_n\":0"));
  CHECK(contains(s, "\"cal_n\":0,\"learn_use\":0"));
  k.has_mac = false, k.cal_n = 3, k.learn_use = 4;  // the calibration log's entry count, admin only
  std::string n;
  Out o2(toString, &n, buf, sizeof buf);
  renderStationConfig(o2, "M1", true, defaultStationConfig(), nullptr, &k);
  o2.flush();
  CHECK(contains(n, "\"src_mac\":null") && contains(n, "\"cal_n\":3,\"learn_use\":4"));
  // never on the public live JSON
  static Stations all;
  all.reserve(MAX_STATIONS);
  PosixTz tz;
  parsePosixTz("UTC0", &tz);
  Station* st = all.findOrAdd("M1", defaultStationConfig());
  Stations::ingest(*st, sampleFrom("ID=M1&tempf=50"), 1000, 1000, 0, SrcIf::Ap, false, tz);
  st->has_src_mac = true;
  st->src_mac[0] = 0x02;
  LiveSnap ls;
  snapLive(*st, 1000, &ls);
  std::string body;
  Out o3(toString, &body, buf, sizeof buf);
  renderLiveOne(o3, ls, Clock{1000, false, 1000, 1000}, nullptr);
  o3.flush();
  CHECK(!contains(body, "mac"));
}

// a parked station's snapshot entry rides through a rewrite untouched, so a later two-station image
// reads it back; an entry of an id that is neither written nor parked goes (a forgotten one)
static void testSnapCarryParked() {
  if (RING_SLOTS < 2) return;  // the WROOM-32 profile has one ring and one snapshot entry: nothing to carry beside it
  static Stations a1, b1;
  a1.reserve(MAX_STATIONS), b1.reserve(MAX_STATIONS);
  PosixTz tz;
  parsePosixTz("UTC0", &tz);
  Station* a = a1.findOrAdd("PA", defaultStationConfig());
  Station* b = b1.findOrAdd("PB", defaultStationConfig());
  const uint32_t t0 = (uint32_t)unixFromUtc(2026, 11, 14, 9, 5, 0);
  Stations::ingest(*a, sampleFrom("ID=PA&tempf=50"), t0, 1000, 0, SrcIf::Ap, true, tz);
  Stations::ingest(*b, sampleFrom("ID=PB&tempf=41"), t0, 1000, 0, SrcIf::Ap, true, tz);
  static SnapEntry e[RING_SLOTS], back[RING_SLOTS];
  takeSnapEntry(*a, &e[0]);
  takeSnapEntry(*b, &e[1]);
  static RamFs fs;
  fs.files.clear();
  CHECK(snapWrite(fs, e, 2));  // the file before the OTA: both stations
  static SnapEntry out[RING_SLOTS];
  takeSnapEntry(*a, &out[0]);  // after the boot at cap 1: only PA is loaded
  ParkedList parked = {};
  parked.add("PB");
  int n = snapCarryParked(fs, out, 1, RING_SLOTS, parked);
  CHECK(n == 2 && strcmp(out[1].id, "PB") == 0 && memcmp(&out[1], &e[1], sizeof out[1]) == 0);
  CHECK(snapWrite(fs, out, n) && snapRead(fs, back, RING_SLOTS) == 2 && strcmp(back[1].id, "PB") == 0);  // a cap-2 read
  CHECK(snapCarryParked(fs, out, 2, RING_SLOTS, parked) == 2);  // already there: not carried twice
  ParkedList none = {};
  CHECK(snapCarryParked(fs, out, 1, RING_SLOTS, none) == 1);  // not parked (forgotten): not carried
  CHECK(snapCarryParked(fs, out, 1, 1, parked) == 1);        // no room: nothing written past cap
}
static void testRateLimit() {
  static Stations all;
  all.reserve(MAX_STATIONS);
  PosixTz tz;
  parsePosixTz("UTC0", &tz);
  Station* a = all.findOrAdd("RA", defaultStationConfig());
  static Stations allRB;  // a slot of its own (one station per Stations on the WROOM-32 profile)
  allRB.reserve(MAX_STATIONS);
  Station* b = allRB.findOrAdd("RB", defaultStationConfig());
  CHECK(!rateLimited(*a, 1000));  // the first upload since boot
  Stations::ingest(*a, sampleFrom("ID=RA&tempf=50"), 1000, 1000, 0, SrcIf::Ap, false, tz);
  a->last_rx_ms = 1000;
  CHECK(rateLimited(*a, 1000 + 3000) && rateLimited(*a, 1000 + RATE_MIN_MS - 1));  // 3 s: dropped
  CHECK(!rateLimited(*a, 1000 + RATE_MIN_MS) && !rateLimited(*a, 1000 + 5000));    // 4 s and 5 s: accepted
  CHECK(!rateLimited(*b, 1000 + 2000));                                          // another id: its own clock
  a->last_rx_ms = 0xFFFFF000u;                                                   // millis() wraps
  CHECK(rateLimited(*a, 0xFFFFF000u + 2000) && !rateLimited(*a, 0xFFFFF000u + RATE_MIN_MS + 100));
}

static void testOpsecB5e() {
  char s[] = "evil\x1b[2Jname\xc3\xa9 x";
  logSafe(s);
  CHECK(strcmp(s, "evil?[2Jname??\?x") == 0);
  char ua[] = "Mozilla/5.0 (X\x01)\xe2";
  logSafe(ua, true);
  CHECK(strcmp(ua, "Mozilla/5.0 (X?)?") == 0);
  CHECK(pathPrefixLen("/v01/set/wid/ABC/key/SECRET/temp/205", 2) == 8);
  CHECK(pathPrefixLen("/weatherstation/updateweatherstation.php?ID=x", 2) == 40);
  CHECK(pathPrefixLen("/", 2) == 1 && pathPrefixLen("/a?b/c/d", 2) == 2);
  // src_ip / src_if on the LAN face only
  static Stations all;
  all.reserve(MAX_STATIONS);
  PosixTz tz;
  parsePosixTz("UTC0", &tz);
  Station* st = all.findOrAdd("N4", defaultStationConfig());
  Stations::ingest(*st, sampleFrom("ID=N4&tempf=50"), 1000, 1000, 0x0A01A8C0u, SrcIf::Ap, false, tz);
  LiveSnap ls;
  snapLive(*st, 1000, &ls);
  char buf[128];
  std::string ap, lan;
  Out o1(toString, &ap, buf, sizeof buf);
  renderLiveOne(o1, ls, Clock{1000, false, 1000, 1000}, nullptr, "e204b305");
  o1.flush();
  CHECK(!contains(ap, "src_ip") && !contains(ap, "src_if") && !contains(ap, "192.168"));
  // the page version is on the AP face too (no diag there); a caller without one renders no key
  CHECK(contains(ap, "\"time_src\":\"none\",\"page\":\"e204b305\",\"station\":") && !contains(ap, "\"diag\""));
  Diag d = {};
  Out o2(toString, &lan, buf, sizeof buf);
  renderLiveOne(o2, ls, Clock{1000, false, 1000, 1000}, &d);
  o2.flush();
  CHECK(contains(lan, "\"src_ip\":\"192.168.1.10\"") && contains(lan, "\"src_if\":\"ap\""));
  CHECK(!contains(lan, "\"time_src\":\"none\",\"page\"") && contains(lan, "\"build\":\"\",\"page\":\"\","));
  // a key too long to check is redacted
  char line[256];
  redactRequestLine("GET", "/x?%50%41%53%53%57%4F%52%44%00%00%00=secret&tempf=50", line, sizeof line);
  CHECK(!contains(line, "secret") && contains(line, "tempf=50"));
}

// a station with nothing to save writes no snapshot
static void testSnapshotWorth() {
  static Stations all;
  all.reserve(MAX_STATIONS);
  PosixTz tz;
  parsePosixTz("UTC0", &tz);
  Station* st = all.findOrAdd("SW1", defaultStationConfig());
  CHECK(!snapshotWorthWriting(*st));
  st->flash_state |= FLASH_LIVE_RESTORED;
  CHECK(snapshotWorthWriting(*st));
  st->flash_state = 0;
  Stations::ingest(*st, sampleFrom("ID=SW1&tempf=50"), 1000, 1000, 0, SrcIf::Ap, false, tz);
  CHECK(snapshotWorthWriting(*st));
}

// every field has a maximum; a value that cannot format is null; a snapshot value beyond it heals
static bool jsonParses(const std::string& j) {
  JsonReader r(j.data(), j.size());
  return r.skipValue() && !r.failed() && r.atEnd();
}
static void testFieldMaxima() {
  struct V { const char* q; int f; };
  const V bad[] = {{"windspeedmph=1e22", F_WIND_KMH}, {"lowbatt=1e30", F_BATTERY_LOW}, {"rainin=1e21", F_RAIN_MM},
                   {"UV=31", F_UV}, {"solarRadiation=2001", F_SOLAR_WM2}, {"windgustmph=311", F_GUST_KMH},
                   {"dailyrainin=79", F_RAIN_DAILY_MM}, {"yearlyrainin=1182", F_RAIN_YEARLY_MM}, {"rainratein=40", F_RAIN_RATE_MMH},
                   {"lowbatt=101", F_BATTERY_LOW}, {"windspdmph_avg2m=-1", F_WIND_AVG2M_KMH}};
  int rejected = 0;
  for (const V& v : bad) {
    Sample s;
    char q[64];
    snprintf(q, sizeof q, "ID=ST1&%s", v.q);
    parseQuery(q, &s);
    rejected += ((s.rejected_bounds >> v.f) & 1u) && !s.has(v.f);
  }
  CHECK(rejected == (int)(sizeof bad / sizeof bad[0]));
  // each range edge accepted (after conversion: 310 mph = 498.9 km/h, 78 in = 1981 mm, 1180 in = 29972 mm)
  Sample e;
  parseQuery("ID=ST1&windspeedmph=310&windgustmph=0&rainin=78&dailyrainin=0&weeklyrainin=1180&UV=30&solarRadiation=2000"
             "&lowbatt=100&rainratein=39", &e);
  CHECK(e.rejected_bounds == 0 && e.has(F_WIND_KMH) && e.has(F_GUST_KMH) && e.has(F_RAIN_MM) && e.has(F_RAIN_WEEKLY_MM) &&
        e.has(F_UV) && e.has(F_SOLAR_WM2) && e.has(F_BATTERY_LOW) && e.has(F_RAIN_RATE_MMH));
  CHECK(fieldInBounds(F_BATTERY_LOW, 0) && fieldInBounds(F_BATTERY_LOW, 100) && !fieldInBounds(F_BATTERY_LOW, 100.5) &&
        !fieldInBounds(F_WIND_KMH, 1e22) && !fieldInBounds(F_TEMP_C, NAN) && !fieldInBounds(-1, 0));
  // /api/live with a forced 1e30 (what an older snapshot may hold): "wind_kmh":null, and the document parses
  static Stations all;
  all.reserve(MAX_STATIONS);
  PosixTz tz;
  parsePosixTz("UTC0", &tz);
  Station* st = all.findOrAdd("ST1", defaultStationConfig());
  const uint32_t t0 = (uint32_t)unixFromUtc(2026, 11, 12, 9, 0, 0);
  Stations::ingest(*st, sampleFrom("ID=ST1&tempf=50&windspeedmph=5"), t0, 1000, 0, SrcIf::Ap, true, tz);
  st->live.v[F_WIND_KMH] = 1e30f;
  Clock c{t0 + 5, true, 100, 6000};
  char buf[256];
  std::string live;
  Out o(toString, &live, buf, sizeof buf);
  renderLive(o, all, c, nullptr);
  o.flush();
  CHECK(contains(live, "\"wind_kmh\":null") && jsonParses(live));
  // the same value in a snapshot entry is dropped at the restore (the boot heals an older snapshot)
  static SnapEntry snap;
  takeSnapEntry(*st, &snap);
  static Stations boot;
  boot.reserve(MAX_STATIONS);
  Station* re = boot.findOrAdd("ST1", defaultStationConfig());
  applySnapEntry(*re, snap, 0);
  CHECK(!re->live.has(F_WIND_KMH) && re->live.has(F_TEMP_C));
  // a promoted extra with soil=1e300 is not taken; /api/live still parses
  StationConfig pro = defaultStationConfig();
  pro.promote[0].used = true;
  strcpy(pro.promote[0].param, "soil");
  strcpy(pro.promote[0].key, "soil");
  static Stations ps;
  ps.reserve(MAX_STATIONS);
  Station* sp = ps.findOrAdd("ST2", pro);
  Stations::ingest(*sp, sampleFrom("ID=ST2&tempf=50&soil=1e300"), t0, 1000, 0, SrcIf::Ap, true, tz);
  std::string l2;
  Out o2(toString, &l2, buf, sizeof buf);
  renderLive(o2, ps, c, nullptr);
  o2.flush();
  CHECK(!(sp->promoted_mask & 1u) && contains(l2, "\"soil\":{\"v\":null") && !contains(l2, "inf") && jsonParses(l2));
}

// the order of the upload checks after onUpload; Out::htmlStr escapes names on /tools
static void testUploadVerdictAndHtml() {
  static Stations all;
  all.reserve(MAX_STATIONS);
  PosixTz tz;
  parsePosixTz("UTC0", &tz);
  Station* st = all.findOrAdd("UV1", defaultStationConfig());
  CHECK(uploadVerdict(nullptr, KeyCheck::Mismatch, 0, 0) == UploadVerdict::NoSlot);  // no slot wins over the key
  uint8_t k1[32] = {1}, k2[32] = {2};
  CHECK(checkStationKey(*st, k1) == KeyCheck::Bound);
  CHECK(uploadVerdict(st, KeyCheck::Bound, 1000, 0) == UploadVerdict::Accept);  // the first upload: never rate-limited
  Stations::ingest(*st, sampleFrom("ID=UV1&tempf=50"), 1000, 1000, 0, SrcIf::Ap, false, tz);
  st->last_rx_ms = 1000;
  st->last_qhash = 7;
  const KeyCheck other = checkStationKey(*st, k2);
  CHECK(other == KeyCheck::Mismatch && uploadVerdict(st, other, 1500, 0) == UploadVerdict::RateLimited);  // rate first
  CHECK(uploadVerdict(st, other, 1000 + RATE_MIN_MS, 8) == UploadVerdict::KeyMismatch);
  CHECK(uploadVerdict(st, KeyCheck::Match, 1500, 0) == UploadVerdict::RateLimited);
  CHECK(uploadVerdict(st, KeyCheck::Match, 1000 + RATE_MIN_MS, 8) == UploadVerdict::Accept);
  std::string h;
  char buf[32];
  Out o(toString, &h, buf, sizeof buf);
  o.htmlStr("<b>\"Bed\" & 'A'</b>");
  o.flush();
  CHECK(!contains(h, "<b>") && contains(h, "&lt;b&gt;") && contains(h, "&amp;") && !contains(h, "\""));
}

// fields received before a restart are seen again after it, so discovery re-announces them
static void testSeenFromSnapshot() {
  static Stations all, boot;
  all.reserve(MAX_STATIONS);
  boot.reserve(MAX_STATIONS);
  PosixTz tz;
  parsePosixTz("UTC0", &tz);
  Station* st = all.findOrAdd("SF1", defaultStationConfig());
  const uint32_t t0 = (uint32_t)unixFromUtc(2026, 11, 3, 9, 0, 0);
  Stations::ingest(*st, sampleFrom("ID=SF1&tempf=50&lowbatt=1"), t0, 1000, 0, SrcIf::Ap, true, tz);
  Stations::ingest(*st, sampleFrom("ID=SF1&tempf=51"), t0 + 60, 2000, 0, SrcIf::Ap, true, tz);
  CHECK((st->seen_fields >> F_BATTERY_LOW) & 1u);
  RamFs fs;
  static SnapEntry snap, back;
  takeSnapEntry(*st, &snap);
  CHECK(snapWrite(fs, &snap, 1) && snapRead(fs, &back, 1) == 1);
  Station* re = boot.findOrAdd("SF1", defaultStationConfig());
  CHECK(re->seen_fields == 0);
  applySnapEntry(*re, back, 0);
  CHECK(((re->seen_fields >> F_BATTERY_LOW) & 1u) && ((re->seen_fields >> F_TEMP_C) & 1u));
  CHECK(!((re->seen_fields >> F_UV) & 1u));  // never received: not announced
  CHECK(re->fresh == 0);                     // states stay fresh-only: nothing to publish from a restore
}

// the clock ladder
static void testClockLadder() {
  const uint32_t T = (uint32_t)unixFromUtc(2026, 11, 4, 12, 0, 0);
  CHECK(TIME_FLOOR == (uint32_t)unixFromUtc(2026, 1, 1, 0, 0, 0));
  CHECK(!timeValid((uint32_t)unixFromUtc(2025, 12, 31, 23, 59, 59)) && timeValid(TIME_FLOOR));
  ClockLadder L;
  // nothing at all
  CHECK(L.best(100) == TimeSrc::None && L.approx(100) == 0 && L.bestTime(100, 100) == 0);
  CHECK(ntpAnswer(100) == NtpAnswer::None);
  // persisted only: approximate = persisted + uptime since the restore; never the system clock
  L.persisted = T;
  L.persisted_up = 2;
  CHECK(L.best(100) == TimeSrc::Persisted && L.approx(62) == T + 60 && L.bestTime(100, 62) == T + 60);
  CHECK(ntpAnswer(100) == NtpAnswer::None);  // the approximate time never answers a console
  // a persisted value below the floor is "none"
  ClockLadder old;
  old.persisted = TIME_FLOOR - 1;
  CHECK(old.best(5) == TimeSrc::None && old.approx(5) == 0);
  // every source that sets the system clock wins over persisted, but only with a valid clock
  for (TimeSrc s : {TimeSrc::Browser, TimeSrc::Dhcp, TimeSrc::Sntp}) {
    ClockLadder c = L;
    c.sys_src = s;
    CHECK(c.best(T + 5) == s && c.bestTime(T + 5, 62) == T + 5);
    CHECK(c.best(1000) == TimeSrc::Persisted);  // source recorded, clock invalid: not trusted
  }
  CHECK(ntpAnswer(T) == NtpAnswer::System && ntpAnswer(TIME_FLOOR - 1) == NtpAnswer::None);
  // seed acceptance: within [floor, ceil] (checked on the 64-bit ms), only while neither SNTP nor DHCP set it
  const uint32_t build = unixFromBuildDate("Nov  4 2026"), ceil = build + SEED_SPAN_S;
  CHECK(build == (uint32_t)unixFromUtc(2026, 11, 4, 0, 0, 0) && unixFromBuildDate("Jan 15 2027") == (uint32_t)unixFromUtc(2027, 1, 15, 0, 0, 0));
  CHECK(unixFromBuildDate("Xyz  1 2026") == 0 && unixFromBuildDate("") == 0 && unixFromBuildDate(nullptr) == 0);
  ClockLadder sd;
  const uint64_t Tms = (uint64_t)T * 1000;
  CHECK(sd.seedAccepted(Tms, ceil, 0) && !sd.seedAccepted((uint64_t)unixFromUtc(2025, 12, 31, 0, 0, 0) * 1000, ceil, 0));
  CHECK(sd.seedAccepted((uint64_t)ceil * 1000, ceil, 0) && !sd.seedAccepted((uint64_t)(ceil + 1) * 1000, ceil, 0));
  CHECK(!sd.seedAccepted(((uint64_t)1 << 32) * 1000 + Tms, ceil, 0));  // would wrap into range as uint32: refused
  CHECK(!sd.seedAccepted((uint64_t)unixFromUtc(2100, 1, 1, 0, 0, 0) * 1000, ceil, 0));
  sd.sys_src = TimeSrc::Browser;
  CHECK(sd.seedAccepted(Tms + 10000, ceil, T));  // a later seed (admin login) may refresh a browser time
  sd.sys_src = TimeSrc::Dhcp;
  CHECK(!sd.seedAccepted(Tms, ceil, T));
  sd.sys_src = TimeSrc::Sntp;  // also the provisional mark of the SNTP callback
  CHECK(!sd.seedAccepted(Tms, ceil, T));
  // a real source but an invalid clock (an unsynced router answered below the floor): the seed is open again
  CHECK(sd.seedAccepted(Tms, ceil, 1000) && sd.seedAccepted(Tms, ceil, TIME_FLOOR - 1) && !sd.seedAccepted(Tms, ceil, TIME_FLOOR));
  sd.sys_src = TimeSrc::Dhcp;
  CHECK(sd.seedAccepted(Tms, ceil, 0));
  // a seed ahead of real time is repaired by the first SNTP time, even backwards, once
  ClockLadder ah;
  CHECK(ah.persistDue(T + 86400, 5, false));  // a browser seed a day ahead, written
  ah.markWritten(T + 86400, 5);
  CHECK(!ah.persistDue(T, 7, false) && !ah.persistDue(T, 7, true));  // not by a non-authoritative time
  CHECK(ah.persistDue(T, 7, false, true));                           // SNTP: at once, backwards
  ah.markWritten(T, 7, true);
  CHECK(ah.written_auth && !ah.persistDue(T + 1, 8, false, true));  // then the normal cadence
  CHECK(!ah.persistDue(T - 100, 2000, true, true));                  // and never backwards again
  ClockLadder rs;  // a restored t_last ahead of SNTP
  rs.persisted = T + 3600;
  CHECK(!rs.persistDue(T, 1, false) && rs.persistDue(T, 1, false, true));
  // persist cadence: 15 min, never backwards, not the restored value again, force for a planned restart
  ClockLadder p;
  p.persisted = T;
  p.persisted_up = 0;
  CHECK(!p.persistDue(T, 0, false) && !p.persistDue(T, 0, true));  // the restored value itself
  CHECK(p.persistDue(T + 1, 1, false));                             // first write of this run
  p.markWritten(T + 1, 1);
  CHECK(!p.persistDue(T + 600, 600, false));                        // < 15 min
  CHECK(p.persistDue(T + 600, 600, true));                          // planned restart
  CHECK(p.persistDue(T + 901, 901, false));
  p.markWritten(T + 901, 901);
  CHECK(!p.persistDue(T + 100, 2000, false) && !p.persistDue(T + 100, 2000, true));  // never backwards
  CHECK(!p.persistDue(TIME_FLOOR - 5, 3000, true));                  // below the floor
  ClockLadder fresh;  // no t_last yet: an SNTP time is written at once
  CHECK(fresh.persistDue(T, 10, false) && !fresh.persistDue(0, 10, true));
  CHECK(strcmp(timeSrcName(TimeSrc::Persisted), "persisted") == 0 && strcmp(timeSrcName(TimeSrc::None), "none") == 0 &&
        strcmp(timeSrcName(TimeSrc::Dhcp), "dhcp") == 0 && strcmp(timeSrcName(TimeSrc::Browser), "browser") == 0 &&
        strcmp(timeSrcName(TimeSrc::Sntp), "sntp") == 0);
  // the SNTP table after DHCP. lwIP clears slots 1-2 after writing option 42 into slot 0;
  // the layout follows the ladder, public hosts in 0-1 and DHCP's server moved to slot 2
  const char* want[SNTP_SLOTS];
  bool mv = true;
  SntpSlot boot[SNTP_SLOTS] = {{SNTP_HOST_A, false}, {SNTP_HOST_B, false}, {nullptr, false}};  // configTime
  CHECK(sntpPlan(boot, want, &mv) == 0 && !mv);
  SntpSlot leased[SNTP_SLOTS] = {{nullptr, true}, {nullptr, false}, {nullptr, false}};  // option 42 taken
  CHECK(sntpPlan(leased, want, &mv) == 3 && mv && strcmp(want[0], "pool.ntp.org") == 0 &&
        strcmp(want[1], "time.google.com") == 0 && !want[2]);
  SntpSlot fixed[SNTP_SLOTS] = {{"pool.ntp.org", true}, {"time.google.com", false}, {nullptr, true}};  // after the move
  CHECK(sntpPlan(fixed, want, &mv) == 0 && !mv);
  SntpSlot empty[SNTP_SLOTS] = {{nullptr, false}, {nullptr, false}, {nullptr, false}};  // a lease without option 42 cleared all
  CHECK(sntpPlan(empty, want, &mv) == 2 && !mv && strcmp(want[0], "pool.ntp.org") == 0 &&
        strcmp(want[1], "time.google.com") == 0 && !want[2]);
  SntpSlot half[SNTP_SLOTS] = {{"pool.ntp.org", true}, {"pool.ntp.org", false}, {nullptr, false}};  // B lost
  CHECK(sntpPlan(half, want, &mv) == 1 && !mv && !want[0] && strcmp(want[1], "time.google.com") == 0 && !want[2]);
  CHECK(sntpDhcpSlot(leased[0]) && !sntpDhcpSlot(boot[0]) && !sntpDhcpSlot(empty[0]) && sntpDhcpSlot(fixed[2]));
}

// the same query again within 120 s is a duplicate; order slot -> rate -> duplicate -> key
static void testDedupe() {
  static Stations all;
  all.reserve(MAX_STATIONS);
  static Stations all2;  // two slots per Stations, the 3rd+ station here
  all2.reserve(MAX_STATIONS);
  PosixTz tz;
  parsePosixTz("UTC0", &tz);
  const char* q = "ID=DD1&PASSWORD=x&tempf=50&humidity=40";
  const uint32_t h = fnv1a32(q);
  CHECK(h == fnv1a32("ID=DD1&PASSWORD=x&tempf=50&humidity=40") && h != fnv1a32("ID=DD1&PASSWORD=x&tempf=50&humidity=41"));
  CHECK(fnv1a32("") == 2166136261u && fnv1a32("a") == 0xe40c292cu);  // the published FNV-1a 32 vectors
  Station* a = all.findOrAdd("DD1", defaultStationConfig());
  static Stations allDD2;  // a slot of its own (one station per Stations on the WROOM-32 profile)
  allDD2.reserve(MAX_STATIONS);
  Station* b = allDD2.findOrAdd("DD2", defaultStationConfig());
  CHECK(uploadVerdict(a, KeyCheck::Match, 1000, h) == UploadVerdict::Accept);  // first upload: never a duplicate
  Stations::ingest(*a, sampleFrom(q), 1000, 1000, 0, SrcIf::Ap, false, tz);
  a->last_rx_ms = 1000;
  a->last_qhash = h;
  CHECK(uploadVerdict(a, KeyCheck::Match, 71000, h) == UploadVerdict::Duplicate);    // 70 s later
  CHECK(uploadVerdict(a, KeyCheck::Match, 1000 + DUP_WINDOW_MS - 1, h) == UploadVerdict::Duplicate);
  CHECK(uploadVerdict(a, KeyCheck::Match, 122000, h) == UploadVerdict::Accept);      // 121 s later
  CHECK(uploadVerdict(a, KeyCheck::Match, 71000, fnv1a32("ID=DD1&PASSWORD=x&tempf=50&humidity=4O")) == UploadVerdict::Accept);
  CHECK(uploadVerdict(a, KeyCheck::Match, 3000, h) == UploadVerdict::RateLimited);   // the rate rule first
  CHECK(uploadVerdict(a, KeyCheck::Mismatch, 71000, h) == UploadVerdict::Duplicate); // before the key
  CHECK(uploadVerdict(b, KeyCheck::Match, 71000, h) == UploadVerdict::Accept);       // ids are independent
  a->last_rx_ms = 0xFFFFF000u;  // wrap-safe millis
  CHECK(uploadVerdict(a, KeyCheck::Match, 0x00001000u, h) == UploadVerdict::Duplicate);
  // dateutc=now (or none) with steady values at a 60 s cadence: never a duplicate; a repeat within 30 s still is
  const char* qn = "ID=DD3&PASSWORD=x&dateutc=now&tempf=50";
  Sample sn = sampleFrom(qn);
  CHECK(!sn.has_dateutc);
  Station* c = all2.findOrAdd("DD3", defaultStationConfig());
  Stations::ingest(*c, sn, 1000, 1000, 0, SrcIf::Ap, false, tz);
  c->last_rx_ms = 1000;
  c->last_qhash = fnv1a32(qn);
  for (int i = 1; i <= 4; i++) {  // every 60 s, the same query: all accepted
    CHECK(uploadVerdict(c, KeyCheck::Match, 1000 + 60000u * (uint32_t)i, fnv1a32(qn), sn.has_dateutc) == UploadVerdict::Accept);
    c->last_rx_ms = 1000 + 60000u * (uint32_t)i;
  }
  CHECK(uploadVerdict(c, KeyCheck::Match, c->last_rx_ms + 20000, fnv1a32(qn), false) == UploadVerdict::Duplicate);
  CHECK(uploadVerdict(c, KeyCheck::Match, c->last_rx_ms + 31000, fnv1a32(qn), false) == UploadVerdict::Accept);
}

// profiles as data, the 7-in-1 fingerprint
static void testProfilesFingerprint() {
  const StationProfile& y = profileRules(Profile::Yt60234);
  const StationProfile& g = profileRules(Profile::Generic);
  CHECK(strcmp(y.name, "yt60234") == 0 && strcmp(y.label, "Vevor/Youtong 7-in-1") == 0 && y.rainin_is_rate &&
        std::fabs(y.baro_factory_hpa - 1013.2f) < 0.01f && y.fingerprinted);
  CHECK(strcmp(g.name, "generic") == 0 && strcmp(g.label, "Generic WU") == 0 && !g.rainin_is_rate &&
        std::isnan(g.baro_factory_hpa) && !g.fingerprinted);
  CHECK(y.sensor_mask == g.sensor_mask && ((y.sensor_mask >> F_TEMP_C) & 1u) && ((y.sensor_mask >> F_SOLAR_WM2) & 1u) &&
        !((y.sensor_mask >> F_PRESSURE_HPA) & 1u) && !((y.sensor_mask >> F_RAIN_MM) & 1u) &&
        !((y.sensor_mask >> F_RAIN_DAILY_MM) & 1u) && !((y.sensor_mask >> F_INDOOR_TEMP_C) & 1u));
  // a real console upload (redacted) and the cold-start upload with no sensor heard
  const char* full = "ID=IXXXXXX1&PASSWORD=xxxxxxxx&dateutc=2026-11-4+11%3A16%3A18&baromin=29.92&tempf=73.4&humidity=39"
                     "&dewptf=46.7&rainin=0&dailyrainin=0&winddir=334&windspeedmph=0&windgustmph=0&UV=0&solarRadiation=0.4";
  const char* cold = "ID=IXXXXXX1&PASSWORD=xxxxxxxx&dateutc=2026-6-1+13%3A3%3A20&baromin=29.92";
  const uint8_t bouffalo[6] = {0x1C, 0x78, 0x4B, 1, 2, 3}, weewx[6] = {0xB8, 0x3D, 0xFB, 1, 2, 3},
                other[6] = {0x24, 0x0A, 0xC4, 1, 2, 3};
  CHECK(stationFingerprint("lwIP/2.1.2", full, bouffalo) == Fingerprint::Match);
  CHECK(stationFingerprint("lwIP/2.1.2", full, weewx) == Fingerprint::Match);
  CHECK(stationFingerprint("lwIP/2.1.2", cold, bouffalo) == Fingerprint::Match);
  CHECK(stationFingerprint("lwIP/2.1.2", full, nullptr) == Fingerprint::Unknown);  // no MAC: no veto
  CHECK(stationFingerprint("lwIP/2.1.2", full, other) == Fingerprint::Unknown);    // another OUI: no veto
  CHECK(stationFingerprint("lwIP/2.1.2", "id=X&password=y&TEMPF=50", nullptr) == Fingerprint::Unknown);  // any case
  // Generic consoles
  CHECK(stationFingerprint("lwIP/2.1.2", "ID=X&PASSWORD=y&tempf=50&softwaretype=EasyWeather", bouffalo) == Fingerprint::Mismatch);
  CHECK(stationFingerprint("lwIP/2.1.2", "ID=X&PASSWORD=y&humidity=40&tempf=50", bouffalo) == Fingerprint::Mismatch);  // order
  CHECK(stationFingerprint("lwIP/2.1.2", "ID=X&ID=X&tempf=50", bouffalo) == Fingerprint::Mismatch);  // repeated
  CHECK(stationFingerprint("WindowsPowerShell/5.1", full, bouffalo) == Fingerprint::Mismatch);
  CHECK(stationFingerprint("", full, bouffalo) == Fingerprint::Mismatch);
  CHECK(stationFingerprint("lwIP/2.1.2", "ID=X&PASSWORD=y&action=updateraw&tempf=50", nullptr) == Fingerprint::Mismatch);
  CHECK(strcmp(fingerprintName(Fingerprint::NotChecked), "") == 0 && strcmp(fingerprintName(Fingerprint::Mismatch), "mismatch") == 0);
  // the first upload of a new station decides; a stored station keeps its profile
  static Stations all;
  all.reserve(MAX_STATIONS);
  static Stations all2;  // two slots per Stations, the 3rd+ station here
  all2.reserve(MAX_STATIONS);
  Station* n = all.findOrAdd("NEW1", defaultStationConfig());
  CHECK(n->cfg.profile == Profile::Yt60234);
  CHECK(applyFingerprint(*n, Fingerprint::Mismatch) && n->cfg.profile == Profile::Generic && n->fp == Fingerprint::Mismatch);
  static Stations allNEW2;  // a slot of its own (one station per Stations on the WROOM-32 profile)
  allNEW2.reserve(MAX_STATIONS);
  Station* m = allNEW2.findOrAdd("NEW2", defaultStationConfig());
  CHECK(!applyFingerprint(*m, Fingerprint::Unknown) && m->cfg.profile == Profile::Yt60234 && m->fp == Fingerprint::Unknown);
  Station* s = all2.findOrAdd("OLD1", defaultStationConfig());
  s->stored = true;
  CHECK(!applyFingerprint(*s, Fingerprint::Mismatch) && s->cfg.profile == Profile::Yt60234);
  static Stations all2OLD2;  // a slot of its own (one station per Stations on the WROOM-32 profile)
  all2OLD2.reserve(MAX_STATIONS);
  Station* u = all2OLD2.findOrAdd("OLD2", defaultStationConfig());
  u->uploads = 3;  // not the first upload
  CHECK(!applyFingerprint(*u, Fingerprint::Mismatch) && u->cfg.profile == Profile::Yt60234 && u->fp == Fingerprint::Mismatch);
}

// the boot log ring (src/bootlog.h): head pinning, head close rules, tail wrap and gap, the line cap,
// freeze / thaw, no duplicates, and the redaction helper
static std::string bootRender(const BootLog& b) {
  std::string js;
  char buf[64];
  Out o(toString, &js, buf, sizeof buf);
  bootlogRender(o, b);
  o.flush();
  return js;
}
static std::vector<std::string> bootLines(const std::string& s) {
  std::vector<std::string> v;
  size_t at = 0;
  while (at < s.size()) {
    const size_t e = s.find('\n', at);
    v.push_back(s.substr(at, e == std::string::npos ? std::string::npos : e - at));
    if (e == std::string::npos) break;
    at = e + 1;
  }
  return v;
}
static void testBootLog() {
  static BootLog b;
  char m[400];
  // head pinning: 15 boot lines, then 10 KB of later lines -> the head byte-identical, the tail the newest lines
  bootlogReset(b);
  for (int i = 0; i < 15; i++) {
    snprintf(m, sizeof m, "boot fact %d", i);
    bootlogAdd(b, 100 + (uint64_t)i, "BOOT", m);
  }
  const std::string head(b.head, b.head_len);
  CHECK(b.head_len > 0 && !b.head_closed && head.rfind("100 [BOOT] boot fact 0\n", 0) == 0);
  int later = 0;
  for (size_t bytes = 0; bytes < 10240; later++) {
    snprintf(m, sizeof m, "later line %d %s", later, std::string((size_t)(later % 50), 'x').c_str());
    bootlogAdd(b, 130000 + (uint64_t)later, "MQTT", m);
    bytes += strlen(m) + 20;
  }
  CHECK(std::string(b.head, b.head_len) == head && b.head_closed);
  std::string out = bootRender(b);
  CHECK(out.rfind(head, 0) == 0);
  std::vector<std::string> ls = bootLines(out.substr(head.size()));
  CHECK(!ls.empty() && ls[0].rfind("--- ", 0) == 0 && contains(ls[0], "line(s) not kept here ---") && b.gap > 0);
  bool shape = true;
  for (size_t i = 1; i < ls.size(); i++) shape = shape && !ls[i].empty() && isdigit((unsigned char)ls[i][0]) && contains(ls[i], " [MQTT] ");
  snprintf(m, sizeof m, "later line %d ", later - 1);
  CHECK(shape && contains(ls.back(), m) && !contains(out, "boot fact 0\n130000"));
  // gap = the tail-only lines evicted: every later line is in the tail or counted
  CHECK((int)(ls.size() - 1) + (int)b.gap == later);
  // the head closes when full: the first line that does not fit goes to the tail only, nothing later enters
  bootlogReset(b);
  std::string big(150, 'h');
  int took = 0;
  while (!b.head_closed) {
    bootlogAdd(b, 10, "BOOT", big.c_str());
    took++;
  }
  const uint16_t hl = b.head_len;
  bootlogAdd(b, 11, "BOOT", "short");
  CHECK(b.head_len == hl && hl <= BOOTLOG_HEAD && took > 1 && b.tail[b.tail_start] == 'H');
  out = bootRender(b);
  CHECK(contains(out, "--- latest lines ---\n") && contains(out.substr(out.find("--- latest")), "11 [BOOT] short\n"));
  // the head closes at 120 000 ms: 119 999 enters, 120 000 does not (nor anything after)
  bootlogReset(b);
  bootlogAdd(b, 119999, "BOOT", "a");
  bootlogAdd(b, 120000, "BOOT", "b");
  bootlogAdd(b, 5, "BOOT", "c");
  CHECK(std::string(b.head, b.head_len) == "119999 [BOOT] a\n" && b.head_closed);
  out = bootRender(b);
  CHECK(out == "119999 [BOOT] a\n--- latest lines ---\n120000 [BOOT] b\n5 [BOOT] c\n");  // no line twice
  // REQ / HTTP chatter (not Boot) skips the head but reaches the tail; an HTTP Boot line -- the
  // first upload -- enters the head, the tag does not keep it out
  bootlogReset(b);
  bootlogAdd(b, 1, "REQ", "GET /api/live", false, false);
  bootlogAdd(b, 2, "HTTP", "asset x", false, false);
  bootlogAdd(b, 3, "BOOT", "y");
  bootlogAdd(b, 4, "HTTP", "first upload", false, true);
  CHECK(std::string(b.head, b.head_len) == "3 [BOOT] y\n4 [HTTP] first upload\n" && !b.head_closed);
  CHECK(bootRender(b) == "3 [BOOT] y\n4 [HTTP] first upload\n--- latest lines ---\n1 [REQ] GET /api/live\n2 [HTTP] asset x\n");
  // tail wrap: varied lengths over >= 3 wraps, oldest -> newest, never a partial first line; gap = evicted tail-only
  bootlogReset(b);
  b.head_closed = 1;  // the tail alone
  size_t stored = 0;
  int total = 0;
  for (int i = 0; stored < 3 * BOOTLOG_TAIL + 500; i++, total++) {
    snprintf(m, sizeof m, "w%d %s", i, std::string((size_t)(i * 37 % 170), 'a' + (char)(i % 26)).c_str());
    bootlogAdd(b, 200000 + (uint64_t)i, "WRAP", m);
    stored += strlen(m) + 16;
  }
  out = bootRender(b);
  ls = bootLines(out);
  CHECK(ls[0] == "--- " + std::to_string(b.gap) + " line(s) not kept here ---");
  bool order = true;
  int first = -1, prev = -1;
  for (size_t i = 1; i < ls.size(); i++) {
    int ms = 0, w = -1;
    order = order && sscanf(ls[i].c_str(), "%d [WRAP] w%d", &ms, &w) == 2 && ms == 200000 + w && (prev < 0 || w == prev + 1);
    if (first < 0) first = w;
    prev = w;
  }
  CHECK(order && prev == total - 1 && first == (int)b.gap && (int)(ls.size() - 1) + (int)b.gap == total);
  // a line exactly filling the free bytes; 199 / 200 / 201 B lines; a vsnprintf-truncated msg -> " [cut]"
  bootlogReset(b);
  b.head_closed = 1;
  bootlogAdd(b, 7, "T", std::string(100, 'q').c_str());  // "7 [T] " + 100 + '\n' = 107 + the flag = 108


  int adds = 0;
  while (BOOTLOG_TAIL - b.tail_len > 200) {
    bootlogAdd(b, 8, "T", std::string(186, 'f').c_str());  // 195 B + the flag
    adds++;
  }
  const size_t rest = BOOTLOG_TAIL - b.tail_len;  // the last free bytes, filled by one exact line
  const uint32_t gap0 = b.gap;
  bootlogAdd(b, 9, "T", std::string(rest - 8, 'e').c_str());  // flag + "9 [T] " + text + newline == rest
  CHECK(b.tail_len == BOOTLOG_TAIL && b.gap == gap0 && adds > 0);
  char line[BOOTLOG_LINE_MAX + 1];
  CHECK(bootlogLine(line, 1, "T", std::string(198 - 6, 'a').c_str(), false) == 199 && !contains(line, "[cut]"));
  CHECK(bootlogLine(line, 1, "T", std::string(199 - 6, 'a').c_str(), false) == 200 && !contains(line, "[cut]"));
  CHECK(bootlogLine(line, 1, "T", std::string(200 - 6, 'a').c_str(), false) == 200 &&
        std::string(line).size() == 200 && std::string(line).substr(193) == " [cut]\n");
  CHECK(bootlogLine(line, 1, "T", std::string(383, 'a').c_str(), true) == 200 && std::string(line).substr(193) == " [cut]\n");
  CHECK(bootlogLine(line, 1, "T", "short", true) == 18 && std::string(line) == "1 [T] short [cut]\n");
  CHECK(bootlogLine(line, 1, "T", "two\nlines", false) == 16 && std::string(line) == "1 [T] two lines\n");
  // freeze: adds are counted, not stored; thaw hands back n (the glue's one [LOG] line); 0 when none
  bootlogReset(b);
  bootlogAdd(b, 1, "BOOT", "x");
  bootlogFreeze(b);
  bootlogAdd(b, 2, "BOOT", "lost 1");
  bootlogAdd(b, 3, "BOOT", "lost 2");
  CHECK(b.lines == 1 && bootlogThaw(b) == 2 && bootlogThaw(b) == 0 && !b.frozen);
  bootlogAdd(b, 4, "LOG", "2 line(s) not kept while the log was sent");
  CHECK(bootRender(b) == "1 [BOOT] x\n4 [LOG] 2 line(s) not kept while the log was sent\n--- latest lines ---\n");
  // redaction: twice in a line, an SSID with % and ", an empty SSID = no-op, the MAC in any case, the cut end
  char r[64];
  strcpy(r, "joining \"My%Wifi\"\" ... My%Wifi\" again");
  CHECK(bootlogRedact(r, sizeof r, "My%Wifi\"", "<home Wi-Fi>", false) == 2 &&
        std::string(r) == "joining \"<home Wi-Fi>\" ... <home Wi-Fi> again");
  strcpy(r, "nothing to do");
  CHECK(bootlogRedact(r, sizeof r, "", "<home Wi-Fi>", false) == 0 && std::string(r) == "nothing to do");
  strcpy(r, "MAC 02:6F:28:AA:BB:CC up, topic fauxcloud_026f28aabbcc");
  CHECK(bootlogRedact(r, sizeof r, "02:6f:28:aa:bb:cc", "<mac>", true) == 1 &&
        bootlogRedact(r, sizeof r, "026f28aabbcc", "<mac>", true) == 1 && std::string(r) == "MAC <mac> up, topic fauxcloud_<mac>");
  char t[24];
  strcpy(t, "ab HomeNet cd HomeNet");  // 21 B in a 24-B buffer: the first grows the text, the cut halves the second, its
                                       // kept start goes too
  CHECK(bootlogRedact(t, sizeof t, "HomeNet", "<home Wi-Fi>", false) == 1 && std::string(t) == "ab <home Wi-Fi> cd ");
  strcpy(t, "xxxxxxxxxxxxxxxxxx Home");  // full on entry (vsnprintf cut "HomeNet"): the trailing start goes
  t[23] = 0;
  CHECK(bootlogRedact(t, sizeof t, "HomeNet", "<home Wi-Fi>", false) == 0 && std::string(t) == "xxxxxxxxxxxxxxxxxx ");

  // the head takes only Log::Boot lines; an Event in the first 120 s goes to the tail only and never
  // closes the head; a Boot line at 119 s goes in, one at 121 s is an Event (tail only)
  {
    static BootLog h;
    bootlogReset(h);
    bootlogAdd(h, 1000, "BOOT", "banner", false, true);
    bootlogAdd(h, 30000, "AP", "client joined", false, false);
    for (int i = 0; i < 10; i++) bootlogAdd(h, 31000 + (uint64_t)i, "HTTP", "an event that does not fill the head", false, false);
    bootlogAdd(h, 119000, "MQTT", "connected", false, true);
    bootlogAdd(h, 121000, "STA", "connected late", false, true);
    const std::string hd(h.head, h.head_len);
    CHECK(contains(hd, "[BOOT] banner\n") && contains(hd, "[MQTT] connected\n") && !contains(hd, "client joined") &&
          !contains(hd, "an event") && !contains(hd, "connected late") && h.head_closed);
    std::string all;
    char bb[64];
    Out ro(toString, &all, bb, sizeof bb);
    bootlogRender(ro, h);
    ro.flush();
    CHECK(contains(all, "[AP] client joined\n") && contains(all, "[STA] connected late\n"));
    // the head's budget: 20 boot facts of ~100 B fit the 2 048-B head (the board's widest list is checked at the watch)
    bootlogReset(h);
    char bl[120];
    for (int i = 0; i < 20; i++) {
      snprintf(bl, sizeof bl, "boot fact %02d %s", i, std::string(70, 'x').c_str());
      bootlogAdd(h, 1000 + (uint64_t)i, "BOOT", bl, false, true);
    }
    CHECK(!h.head_closed && std::count(h.head, h.head + h.head_len, '\n') == 20 && h.head_len <= BOOTLOG_HEAD);
  }

  // the previous boot's tail across a restart
  {
    static BootLog rp;
    static BootPrev pv;
    bootlogReset(rp);
    bootlogAdd(rp, 1000, "BOOT", "banner", false, true);  // in the head and in the tail (flag H): kept too
    bootlogAdd(rp, 5000, "HIST", "registered", false, false);
    bootlogAdd(rp, 9000, "SYS", "about to crash", false, false);
    CHECK(bootlogSane(rp));
    // a software reset: the tail's lines, oldest first, the head copy included
    CHECK(bootlogKeepPrev(rp, pv, true) > 0 && pv.magic == BOOTLOG_PREV_MAGIC);
    CHECK(std::string(pv.text, pv.len) == "1000 [BOOT] banner\n5000 [HIST] registered\n9000 [SYS] about to crash\n");
    std::string po;
    char ob[64];
    Out o(toString, &po, ob, sizeof ob);
    bootlogRenderPrev(o, pv, "task_wdt", 41);
    o.flush();
    CHECK(po == "--- previous boot (reset: task_wdt, restart #41) ---\n1000 [BOOT] banner\n5000 [HIST] registered\n"
                 "9000 [SYS] about to crash\n");
    // the previous boot's own number (its RTC word) is stored and named, whatever the NVS counter says now; without
    // one (an image before the word: the old behaviour) the header keeps the caller's fallback, as above
    CHECK(pv.restart == BOOTPREV_RESTART_UNKNOWN);
    CHECK(bootlogKeepPrev(rp, pv, true, 17) > 0 && pv.magic == BOOTLOG_PREV_MAGIC && pv.restart == 17);
    {
      std::string ps;
      Out os(toString, &ps, ob, sizeof ob);
      bootlogRenderPrev(os, pv, "sw_reset", 41);  // NVS was wiped since: the counter says 42, the boot was #17
      os.flush();
      CHECK(ps.rfind("--- previous boot (reset: sw_reset, restart #17) ---\n1000 [BOOT] banner\n", 0) == 0);
      CHECK(BOOTLOG_PREV_MAGIC == 0x32565250u && sizeof(BootPrev) == 2060 && offsetof(BootPrev, restart) == 2056);
    }
    // a power-on: emptied, and nothing rendered
    CHECK(bootlogKeepPrev(rp, pv, false, 17) == 0 && pv.magic == 0 && pv.len == 0 && pv.restart == BOOTPREV_RESTART_UNKNOWN);
    std::string none;
    Out on(toString, &none, ob, sizeof ob);
    bootlogRenderPrev(on, pv, "power_on", 1);
    on.flush();
    CHECK(none.empty());
    // RTC garbage: a header out of range, a bad flag, a control byte, a line without its '\n' -> emptied
    static BootLog g;
    memset(&g, 0x5A, sizeof g);
    CHECK(!bootlogSane(g) && bootlogKeepPrev(g, pv, true) == 0 && pv.magic == 0);
    g = rp;
    g.tail[g.tail_start] = 'X';
    CHECK(!bootlogSane(g));
    g = rp;
    g.tail[(g.tail_start + 3) % BOOTLOG_TAIL] = 0x01;
    CHECK(!bootlogSane(g));
    g = rp;
    g.tail_len = (uint16_t)(g.tail_len - 1);  // the last line loses its '\n'
    CHECK(!bootlogSane(g));
    // a damaged line keeps the whole lines before it for the previous-boot copy (the ring itself is still not sane)
    CHECK(bootlogKeepPrev(g, pv, true) > 0 && std::string(pv.text, pv.len) == "1000 [BOOT] banner\n5000 [HIST] registered\n");
    g = rp;
    {
      const size_t third = std::string(rp.tail + rp.tail_start, rp.tail_len).find("9000");  // inside the third line
      g.tail[(g.tail_start + third + 2) % BOOTLOG_TAIL] = 0x01;
    }
    CHECK(!bootlogSane(g) && bootlogKeepPrev(g, pv, true) > 0 &&
          std::string(pv.text, pv.len) == "1000 [BOOT] banner\n5000 [HIST] registered\n");
    g = rp;
    g.tail[g.tail_start] = 'X';  // the first line damaged: nothing whole before it
    CHECK(bootlogKeepPrev(g, pv, true) == 0 && pv.magic == 0);
    // a full tail that wrapped and evicted (gap > 0): whole lines, oldest kept first, within 2 048 B
    bootlogReset(rp);
    for (int i = 0; i < 200; i++) {
      snprintf(m, sizeof m, "line %03d %s", i, std::string(40, 'y').c_str());
      bootlogAdd(rp, 1000 + (uint64_t)i, "FWD", m, false, false);
    }
    CHECK(rp.gap > 0 && bootlogSane(rp) && bootlogKeepPrev(rp, pv, true) > 0 && pv.len <= BOOTLOG_PREV);
    const std::string kept(pv.text, pv.len);
    CHECK(kept.back() == '\n' && contains(kept, "line 199 ") && !contains(kept, "line 000 ") &&
          kept.find("line ") == kept.find('[') + 6);  // starts at a whole line
    // an empty ring: nothing kept
    bootlogReset(rp);
    CHECK(bootlogSane(rp) && bootlogKeepPrev(rp, pv, true) == 0 && pv.magic == 0);
  }
}

// wind_avg10m_kmh = recentFieldMean over [now - 600, now] (inclusive), a pseudo-field in /api/live and HA
static void testWindAvg10m() {
  PosixTz tz;
  parsePosixTz("UTC0", &tz);
  static Stations all;
  all.reserve(MAX_STATIONS);
  const uint32_t t0 = (uint32_t)unixFromUtc(2026, 11, 4, 12, 0, 0);
  auto live = [&](Station* st, uint32_t now, uint32_t disabled) {
    static LiveSnap ls;
    st->cfg.disabled = disabled;
    snapLive(*st, 1000, &ls);
    std::string js;
    char buf[64];
    Out o(toString, &js, buf, sizeof buf);
    renderLiveStation(o, ls, Clock{now, true, 1, 1000}, false);
    o.flush();
    return js;
  };
  // empty: a station that never sent wind has no value and no key
  Station* e = all.findOrAdd("W0", defaultStationConfig());
  Stations::ingest(*e, sampleFrom("ID=W0&tempf=50"), t0, 1000, 0, SrcIf::Ap, true, tz);
  CHECK(!e->wind10_have && !e->wind10_fresh && !contains(live(e, t0, 0), "wind_avg10m_kmh"));
  // every 60 s, windspeedmph = i: at i = 15 the window [t0 + 300, t0 + 900] holds i = 5..15 (both edges in): n 11, mean 10 mph
  static Stations allW1;  // a slot of its own (one station per Stations on the WROOM-32 profile)
  allW1.reserve(MAX_STATIONS);
  Station* st = allW1.findOrAdd("W1", defaultStationConfig());
  char q[96];
  for (int i = 1; i <= 15; i++) {
    snprintf(q, sizeof q, "ID=W1&windspeedmph=%d&winddir=180", i);
    Stations::ingest(*st, sampleFrom(q), t0 + 60u * (uint32_t)i, 1000, 0, SrcIf::Ap, true, tz);
  }
  printf("  info: wind_avg10m %.3f km/h, n %u\n", st->wind10, (unsigned)st->wind10_n);
  CHECK(st->wind10_have && st->wind10_fresh && st->wind10_n == 11 && std::fabs(st->wind10 - 16.09f) < 0.06f);
  const std::string js = live(st, t0 + 900, 0);
  CHECK(contains(js, "\"wind_avg10m_kmh\":16.1") && contains(js, "\"wind_avg10m_kmh\":11}"));
  // hidden wind_kmh: computed, never shown
  const std::string jh = live(st, t0 + 900, 1u << F_WIND_KMH);
  CHECK(!contains(jh, "wind_avg10m_kmh"));
  // an upload without wind 11 min later: the window is empty, the last value stays (not fresh: no MQTT state)
  Stations::ingest(*st, sampleFrom("ID=W1&tempf=50"), t0 + 900 + 660, 1000, 0, SrcIf::Ap, true, tz);
  CHECK(st->wind10_have && !st->wind10_fresh && st->wind10_n == 11 && st->wind10_t == t0 + 900);
  // its age like any field; null once the window has been empty for more than 600 s
  const std::string js1 = live(st, t0 + 1560, 0);
  CHECK(contains(js1, "\"wind_avg10m_kmh\":null") && contains(js1, "\"age\":{") && contains(js1, "\"wind_avg10m_kmh\":660") &&
        jsonParses(js1));
  CHECK(contains(js, "\"wind_avg10m_kmh\":0") && jsonParses(js));  // fresh: age 0
  {  // the widest case: derived holding only this key, 498.9 km/h, n 255, an old age
    static LiveSnap ls;
    st->cfg.disabled = 0;
    snapLive(*st, 1000, &ls);
    ls.derived = 0, ls.w10v = 498.9f, ls.w10n = 255, ls.w10t = ls.last_seen;
    std::string jw;
    char buf[64];
    Out o(toString, &jw, buf, sizeof buf);
    renderLiveStation(o, ls, Clock{t0 + 1560, true, 1, 1000}, false);
    o.flush();
    CHECK(contains(jw, "\"derived\":{\"wind_avg10m_kmh\":255}") && contains(jw, "\"wind_avg10m_kmh\":498.9") && jsonParses(jw));
  }
  // the formatter and the discovery entity (never FIELDS[] past F_COUNT)
  char v[16];
  CHECK(formatValue(v, sizeof v, FIELD_WIND_AVG10M, 16.0934f) > 0 && strcmp(v, "16.1") == 0);
  const uint8_t mac[6] = {0x24, 0x6f, 0x28, 0xaa, 0xbb, 0xcc};
  static char p[1152];
  CHECK(discoveryPayload(p, sizeof p, "ST1", "x", mac, FIELD_WIND_AVG10M, "b14") > 0 &&
        contains(p, "\"default_entity_id\":\"sensor.fauxcloud_st1_wind_avg10m_kmh\"") && contains(p, "\"unit_of_measurement\":\"km/h\"") &&
        contains(p, "json_attributes_topic"));
}

// a rate profile's rainin -> rain_rate_mmh; rain_mm = the last hour of daily-counter deltas
static void testRainRateDerived() {
  PosixTz tz;
  parsePosixTz("UTC0", &tz);
  static Stations all;
  all.reserve(MAX_STATIONS);
  static Stations all2;  // two slots per Stations, the 3rd+ station here
  all2.reserve(MAX_STATIONS);
  Station* st = all.findOrAdd("RR1", defaultStationConfig());  // yt60234: rate
  const uint32_t t0 = (uint32_t)unixFromUtc(2026, 11, 4, 22, 0, 0);
  char q[160];
  // the rate moves; no rain_mm before the warm-up
  Stations::ingest(*st, sampleFrom("ID=RR1&rainin=0.10&dailyrainin=0.40"), t0, 1000, 0, SrcIf::Ap, true, tz);
  CHECK(st->live.has(F_RAIN_RATE_MMH) && std::fabs(st->live.v[F_RAIN_RATE_MMH] - 2.54f) < .01f);
  CHECK(!st->live.has(F_RAIN_MM) && !((st->fresh >> F_RAIN_MM) & 1u) && ((st->fresh >> F_RAIN_RATE_MMH) & 1u));
  CHECK((st->seen_fields >> F_RAIN_MM) & 1u);  // the entity exists from the start, unavailable until warm
  CHECK(!st->rain_started);                    // the first counter value has no delta
  {
    static LiveSnap ls;
    snapLive(*st, 1000, &ls);
    std::string js;
    char buf[64];
    Out o(toString, &js, buf, sizeof buf);
    renderLiveStation(o, ls, Clock{t0, true, 1, 1000}, false);
    o.flush();
    CHECK(contains(js, "\"warming\":[\"rain_mm\"]") && !contains(js, "\"derived\"") && contains(js, "rain_rate_mmh"));
  }
  // every 65 s; +0.01 in (0.254 mm) every 4th upload; crosses midnight (counter reset to 0.01) at upload 20
  float daily = 0.40f;
  float exp[200] = {};
  uint32_t ts[200] = {};
  int n = 0;
  for (int i = 1; i <= 80; i++) {
    const uint32_t t = t0 + 65u * (uint32_t)i;
    float d = 0;
    if (i == 20) { daily = 0.01f; d = 0.254f; }           // midnight: all of it is new
    else if (i % 4 == 0) { daily += 0.01f; d = 0.254f; }
    snprintf(q, sizeof q, "ID=RR1&rainin=0.02&dailyrainin=%.2f", daily);
    Stations::ingest(*st, sampleFrom(q), t, 1000 + 65000u * (uint32_t)i, 0, SrcIf::Ap, true, tz);
    ts[n] = t;
    exp[n++] = d;
    if (i == 10) CHECK(st->rain_started && !st->rain_warm && !st->live.has(F_RAIN_MM));
  }
  // first valid delta at upload 1 (+65 s); warm 60 min later
  CHECK(st->rain_warm && st->live.has(F_RAIN_MM) && ((st->fresh >> F_RAIN_MM) & 1u));
  const uint32_t now = ts[n - 1];
  float want = 0;
  int wn = 0;
  for (int i = 0; i < n; i++)
    if (now - ts[i] < 3600) { want += exp[i]; wn++; }
  // the ring packs 0.01 mm: a 0.254 mm tip is kept as 0.25 (-1.6 %, under the counter's own 0.01 in steps)
  printf("  info: derived last hour %.3f mm, exact %.3f mm, n %d / %d\n", st->live.v[F_RAIN_MM], want, st->rain_n, wn);
  CHECK(std::fabs(st->live.v[F_RAIN_MM] - want) <= want * 0.02f && st->rain_n == wn);
  {  // /api/live: the derived value with its readings; before the warm-up the key is listed as warming
    static LiveSnap ls;
    snapLive(*st, 1000 + 65000u * 80, &ls);
    std::string js;
    char buf[64];
    Out o(toString, &js, buf, sizeof buf);
    renderLiveStation(o, ls, Clock{now, true, 100, 1000 + 65000u * 80}, false);
    o.flush();
    char want2[48];
    snprintf(want2, sizeof want2, "\"derived\":{\"rain_mm\":%d}", wn);
    CHECK(contains(js, want2) && !contains(js, "\"warming\""));
  }
  // a glitch (spike > 100 mm) adds nothing to the hour
  const float before = st->live.v[F_RAIN_MM];
  snprintf(q, sizeof q, "ID=RR1&rainin=0&dailyrainin=%.2f", daily + 5.0f);
  Stations::ingest(*st, sampleFrom(q), now + 65, 1000 + 65000u * 81, 0, SrcIf::Ap, true, tz);
  CHECK(st->live.has(F_RAIN_MM) && st->live.v[F_RAIN_MM] <= before + .001f);
  // rain factor applies to the rate too
  StationConfig c = defaultStationConfig();
  c.rain_factor = 2.0f;
  static Stations allRR2;  // a slot of its own (one station per Stations on the WROOM-32 profile)
  allRR2.reserve(MAX_STATIONS);
  Station* f2 = allRR2.findOrAdd("RR2", c);
  Stations::ingest(*f2, sampleFrom("ID=RR2&rainin=0.10"), t0, 1000, 0, SrcIf::Ap, true, tz);
  CHECK(std::fabs(f2->live.v[F_RAIN_RATE_MMH] - 5.08f) < .01f);
  // Generic WU: rainin is the past-hour sum, unchanged
  StationConfig g = defaultStationConfig();
  g.profile = Profile::Generic;
  Station* gs = all2.findOrAdd("RG1", g);
  Stations::ingest(*gs, sampleFrom("ID=RG1&rainin=0.10&dailyrainin=0.40"), t0, 1000, 0, SrcIf::Ap, true, tz);
  CHECK(gs->live.has(F_RAIN_MM) && std::fabs(gs->live.v[F_RAIN_MM] - 2.54f) < .01f && !gs->live.has(F_RAIN_RATE_MMH));
  // rainratein reaches the field directly on any profile
  Stations::ingest(*gs, sampleFrom("ID=RG1&rainratein=0.20"), t0 + 65, 66000, 0, SrcIf::Ap, true, tz);
  CHECK(gs->live.has(F_RAIN_RATE_MMH) && std::fabs(gs->live.v[F_RAIN_RATE_MMH] - 5.08f) < .01f);
  // a rate the console sent as junk is rejected under the rate's own bit
  static Stations all2RR3;  // a slot of its own (one station per Stations on the WROOM-32 profile)
  all2RR3.reserve(MAX_STATIONS);
  Station* bad = all2RR3.findOrAdd("RR3", defaultStationConfig());
  Stations::ingest(*bad, sampleFrom("ID=RR3&rainin=abc"), t0, 1000, 0, SrcIf::Ap, true, tz);
  CHECK(bad->rejects[F_RAIN_RATE_MMH] == 1 && bad->rejects[F_RAIN_MM] == 0);
  // the window helpers
  Recent r;
  CHECK(recentSpans(r, 100, 3600) && recentSum(r, F_RAIN_MM, 100, 3600).n == 0);
}

// default_entity_id on every entity; the slug rule; unique_id unchanged by it
static void testDefaultEntityId() {
  char o[48];
  const char* in[] = {"ST-1", "a--b", "_x_", "ABC", "___", "rain_mm", "Garden (north)", ""};
  const char* want[] = {"st_1", "a_b", "x", "abc", "", "rain_mm", "garden_north", ""};
  bool ok = true;
  for (int i = 0; i < 8; i++) ok = ok && haSlug(o, sizeof o, in[i]) == (int)strlen(want[i]) && strcmp(o, want[i]) == 0;
  CHECK(ok);
  const uint8_t mac[6] = {0x24, 0x6f, 0x28, 0xaa, 0xbb, 0xcc};
  static char p[1152];
  CHECK(discoveryPayload(p, sizeof p, "ST1", "fauxcloud ST1", mac, F_TEMP_C, "b14") > 0 &&
        contains(p, "\"unique_id\":\"fauxcloud_246f28aabbcc_ST1_temp_c\",\"default_entity_id\":\"sensor.fauxcloud_st1_temp_c\",") &&
        !contains(p, "object_id"));
  CHECK(discoveryPayload(p, sizeof p, "ST-1", "x", mac, FIELD_SENSOR_LINK, "b14") > 0 &&
        contains(p, "\"default_entity_id\":\"binary_sensor.fauxcloud_st_1_sensor_stale\""));
  CHECK(discoveryPayload(p, sizeof p, "___", "x", mac, F_TEMP_C, "b14") > 0 && !contains(p, "default_entity_id") &&
        contains(p, "\"unique_id\":\"fauxcloud_246f28aabbcc_____temp_c\""));  // an empty slug: the key omitted
  char kb[24], lb[32];
  CHECK(discoveryPayloadSpec(p, sizeof p, "ST1", "x", mac, ruleSpec(0, kb, sizeof kb, lb, sizeof lb), "b14") > 0 &&
        contains(p, "\"default_entity_id\":\"sensor.fauxcloud_st1_rej_"));
  static char bp[768];  // app_mqtt.cpp bridge payload buffer
  CHECK(bridgeDiscoveryPayload(bp, sizeof bp, mac, bridgeSpec(B_HEAP), "fauxcloud 0.1.0-b14", "http://255.255.255.255/") > 0 &&
        contains(bp, "\"unique_id\":\"fauxcloud_bridge_246f28aabbcc_heap_free\",\"default_entity_id\":\"sensor.fauxcloud_bridge_bbcc_heap_free\""));
  // the longest payloads still fit their buffers: a 32-char id with a derived entity (the existing worst case), a
  // promotion with a 23-char key under a 15-char id (STATION_ID_LEN 16), the bridge notices
  const char* id = "ABCDEFGHIJKLMNOPQRSTUVWXYZ012345";
  EntitySpec e = fieldSpec(F_RAIN_MM);
  e.own_availability = e.attributes = true;
  const int n1 = discoveryPayloadSpec(p, sizeof p, id, std::string(47, '"').c_str(), mac, e, "fauxcloud 0.1.0-b14", "http://255.255.255.255/");
  Promote pr = {};
  pr.used = true;
  strcpy(pr.key, "abcdefghijklmnopqrstuvw");
  strcpy(pr.name, "Soil moisture in the garden");
  const int n2 = discoveryPayloadSpec(p, sizeof p, "ABCDEFGHIJKLMNO", std::string(47, '"').c_str(), mac, promoteSpec(pr), "fauxcloud 0.1.0-b14",
                                      "http://255.255.255.255/");
  const int n3 = bridgeDiscoveryPayload(bp, sizeof bp, mac, bridgeSpec(B_NOTICES), "fauxcloud 0.1.0-b14", "http://255.255.255.255/");
  printf("  info: discovery worst case with default_entity_id: derived %d, promotion %d B (buffer 1152); bridge %d B (768)\n", n1, n2, n3);
  CHECK(n1 > 0 && n2 > 0 && n3 > 0);
}

// the derived rain_mm has its own availability and attributes; rain_rate_mmh is a new sensor
static void testDerivedEntity() {
  const uint8_t mac[6] = {0x24, 0x6f, 0x28, 0xaa, 0xbb, 0xcc};
  static char payload[1152];  // app_mqtt.cpp publishEntity
  const char* id = "ABCDEFGHIJKLMNOPQRSTUVWXYZ012345";  // the longest id
  EntitySpec e = fieldSpec(F_RAIN_MM);
  e.own_availability = e.attributes = true;
  const int n = discoveryPayloadSpec(payload, sizeof payload, id, std::string(47, '"').c_str(), mac, e,
                                     "fauxcloud 0.1.0-b6", "http://255.255.255.255/");
  printf("  info: derived rain_mm discovery payload, worst case = %d bytes (buffer %u)\n", n, (unsigned)sizeof payload);
  CHECK(n > 0);
  std::string p = payload;
  CHECK(contains(p, "{\"topic\":\"fauxcloud/BBCC/ABCDEFGHIJKLMNOPQRSTUVWXYZ012345/availability\"},"
                    "{\"topic\":\"fauxcloud/BBCC/ABCDEFGHIJKLMNOPQRSTUVWXYZ012345/rain_mm/availability\"}]"));
  CHECK(contains(p, "\"json_attributes_topic\":\"fauxcloud/BBCC/ABCDEFGHIJKLMNOPQRSTUVWXYZ012345/rain_mm/attr\""));
  CHECK(contains(p, "\"unique_id\":\"fauxcloud_246f28aabbcc_ABCDEFGHIJKLMNOPQRSTUVWXYZ012345_rain_mm\""));  // unchanged
  CHECK(discoveryPayloadSpec(payload, sizeof payload, "ST1", "x", mac, fieldSpec(F_RAIN_MM), "v") > 0 &&
        !contains(payload, "rain_mm/availability") && !contains(payload, "json_attributes_topic"));
  const EntitySpec r = fieldSpec(F_RAIN_RATE_MMH);
  CHECK(strcmp(r.key, "rain_rate_mmh") == 0 && strcmp(r.device_class, "precipitation_intensity") == 0 &&
        strcmp(r.unit, "mm/h") == 0 && strcmp(r.state_class, "measurement") == 0);
  char t[80];
  CHECK(entitySubTopic(t, sizeof t, mac, "ST1", "rain_mm", "attr") > 0 && strcmp(t, "fauxcloud/BBCC/ST1/rain_mm/attr") == 0);
}

// 10 uploads with the outdoor set unchanged (or absent after it was seen) -> stale
static void testStaleSensor() {
  PosixTz tz;
  parsePosixTz("UTC0", &tz);
  static Stations all;
  all.reserve(MAX_STATIONS);
  static Stations all2;  // two slots per Stations, the 3rd+ station here
  all2.reserve(MAX_STATIONS);
  const uint32_t t0 = (uint32_t)unixFromUtc(2026, 11, 4, 10, 0, 0);
  const char* q = "ID=SS1&baromin=29.80&tempf=50&humidity=60&dewptf=40&winddir=90&windspeedmph=3&windgustmph=5&UV=0&solarRadiation=10";
  Station* st = all.findOrAdd("SS1", defaultStationConfig());
  for (int i = 0; i < 10; i++) {  // the first upload + 9 identical: same_n 9
    Stations::ingest(*st, sampleFrom(q), t0 + 65u * (uint32_t)i, 1000 + 65000u * (uint32_t)i, 0, SrcIf::Ap, true, tz);
    CHECK(!st->sensor_stale);
  }
  const int hourN = st->hourly.size();
  (void)hourN;
  // the 10th identical (11th upload): stale; the set is stripped, pressure goes on
  char q2[200];
  snprintf(q2, sizeof q2, "%s", q);
  memcpy(strstr(q2, "29.80"), "29.81", 5);
  Stations::ingest(*st, sampleFrom(q2), t0 + 650, 1000 + 650000, 0, SrcIf::Ap, true, tz);
  CHECK(st->sensor_stale && !st->sensor_absent && st->stale_since == t0);
  CHECK(!((st->fresh >> F_TEMP_C) & 1u) && ((st->fresh >> F_PRESSURE_HPA) & 1u));
  CHECK(st->live.has(F_TEMP_C) && st->field_t[F_TEMP_C] == t0 + 585);  // kept, with its old time
  CHECK(!hasValue(st->recent.at(st->recent.size() - 1).v[F_TEMP_C]) && hasValue(st->recent.at(st->recent.size() - 1).v[F_PRESSURE_HPA]));
  Notices ns = stationNotices(*st);
  char txt[300];
  CHECK(ns.n == 1 && noticesState(txt, sizeof txt, ns, tz, 0) > 0 && strcmp(txt, "Outdoor sensor not updating since 10:00. A calm night can do this; a long gap means its battery or radio.") == 0);
  // one changed value clears it
  snprintf(q2, sizeof q2, "%s", q);
  memcpy(strstr(q2, "tempf=50"), "tempf=51", 8);
  Stations::ingest(*st, sampleFrom(q2), t0 + 715, 1000 + 715000, 0, SrcIf::Ap, true, tz);
  CHECK(!st->sensor_stale && st->same_n == 0 && st->stale_since == t0 + 715 && ((st->fresh >> F_TEMP_C) & 1u));
  // absent (a cold start: ID, PASSWORD, dateutc, baromin only) counts as unchanged once seen
  for (int i = 0; i < 10; i++)
    Stations::ingest(*st, sampleFrom("ID=SS1&baromin=29.80"), t0 + 780 + 65u * (uint32_t)i, 800000 + 65000u * (uint32_t)i, 0,
                     SrcIf::Ap, true, tz);
  CHECK(st->sensor_stale && st->sensor_absent);
  ns = stationNotices(*st);
  CHECK(ns.n == 1 && noticeText(txt, sizeof txt, ns.item[0], tz, 0) > 0 && strcmp(txt, "Outdoor sensor missing since 10:11: check its batteries. A short battery-out of the console helps it find the sensor.") == 0);
  // absent before it was ever seen: never stale
  static Stations allSS2;  // a slot of its own (one station per Stations on the WROOM-32 profile)
  allSS2.reserve(MAX_STATIONS);
  Station* n = allSS2.findOrAdd("SS2", defaultStationConfig());
  for (int i = 0; i < 15; i++)
    Stations::ingest(*n, sampleFrom("ID=SS2&baromin=29.80"), t0 + 65u * (uint32_t)i, 1000 + 65000u * (uint32_t)i, 0, SrcIf::Ap, true, tz);
  CHECK(!n->sensor_stale);
  // Generic: the same rule
  StationConfig g = defaultStationConfig();
  g.profile = Profile::Generic;
  Station* gs = all2.findOrAdd("SS3", g);
  for (int i = 0; i < 11; i++)
    Stations::ingest(*gs, sampleFrom("ID=SS3&tempf=50&windspeedmph=3"), t0 + 65u * (uint32_t)i, 1000 + 65000u * (uint32_t)i, 0, SrcIf::Ap, true, tz);
  CHECK(gs->sensor_stale);
}

// the raw pressure at the factory 29.92 inHg for BARO_UNCAL_N uploads -> not calibrated; Generic never
static void testBaroUncalibrated() {
  PosixTz tz;
  parsePosixTz("UTC0", &tz);
  static Stations all;
  all.reserve(MAX_STATIONS);
  const uint32_t t0 = (uint32_t)unixFromUtc(2026, 11, 4, 10, 0, 0);
  StationConfig c = defaultStationConfig();
  c.pressure_hpa_offset = 5;  // a user's offset does not hide it: the rule reads the raw value
  Station* st = all.findOrAdd("BU1", c);
  for (int i = 0; i < BARO_UNCAL_N; i++) {
    CHECK(!st->baro_uncal);
    Stations::ingest(*st, sampleFrom("ID=BU1&baromin=29.92"), t0 + 65u * (uint32_t)i, 1000 + 65000u * (uint32_t)i, 0, SrcIf::Ap, true, tz);
  }
  CHECK(st->baro_uncal && BARO_UNCAL_N == 60);  // about an hour unchanged, not 3 uploads
  Notices ns = stationNotices(*st);
  char txt[300];
  CHECK(ns.n == 1 && noticesState(txt, sizeof txt, ns, tz, 0) > 0 && strcmp(txt, "The console's pressure is at its factory value; set REL once in its SET menu.") == 0);
  Stations::ingest(*st, sampleFrom("ID=BU1&baromin=29.95"), t0 + 65 * 61, 1000 + 65000 * 61, 0, SrcIf::Ap, true, tz);
  CHECK(!st->baro_uncal && st->baro_n == 0);
  // a flat 1013.2 hPa day that moves now and then never raises it (59 at the factory value, one 0.1 off, ...)
  for (int i = 0; i < 180; i++) {
    const char* q = i % 59 == 58 ? "ID=BU1&baromin=29.93" : "ID=BU1&baromin=29.92";
    Stations::ingest(*st, sampleFrom(q), t0 + 65u * (uint32_t)(62 + i), 1000 + 65000u * (uint32_t)(62 + i), 0, SrcIf::Ap, true, tz);
    CHECK(!st->baro_uncal);
  }
  StationConfig g = defaultStationConfig();
  g.profile = Profile::Generic;
  static Stations allBU2;  // a slot of its own (one station per Stations on the WROOM-32 profile)
  allBU2.reserve(MAX_STATIONS);
  Station* gs = allBU2.findOrAdd("BU2", g);
  for (int i = 0; i < 5; i++)
    Stations::ingest(*gs, sampleFrom("ID=BU2&baromin=29.92"), t0 + 65u * (uint32_t)i, 1000 + 65000u * (uint32_t)i, 0, SrcIf::Ap, true, tz);
  CHECK(!gs->baro_uncal);
}

// the WU wind fields by time window; the console's own value wins
static void testDerivedWind() {
  PosixTz tz;
  parsePosixTz("UTC0", &tz);
  static Stations all;
  all.reserve(MAX_STATIONS);
  const uint32_t t0 = (uint32_t)unixFromUtc(2026, 11, 4, 10, 0, 0);
  Station* st = all.findOrAdd("DW1", defaultStationConfig());
  char q[160];
  // 65 s cadence: 2 readings in 2 min (this one and the one before), 10 in 10 min
  for (int i = 0; i < 12; i++) {
    snprintf(q, sizeof q, "ID=DW1&winddir=%d&windspeedmph=%d&windgustmph=%d&tempf=%d", i % 2 ? 350 : 10, 2 + i % 2, 5 + i, 40 + i);
    Stations::ingest(*st, sampleFrom(q), t0 + 65u * (uint32_t)i, 1000 + 65000u * (uint32_t)i, 0, SrcIf::Ap, true, tz);
  }
  CHECK(((st->derived >> F_WIND_AVG2M_KMH) & 1u) && st->derived_n[F_WIND_AVG2M_KMH] == 2);
  CHECK(std::fabs(st->live.v[F_WIND_AVG2M_KMH] - 2.5f * 1.609344f) < .1f);
  // speed-weighted -- 350° at 3 mph and 10° at 2 mph -> 358°
  CHECK(st->derived_n[F_WIND_DIR_AVG2M] == 2 && std::fabs(st->live.v[F_WIND_DIR_AVG2M] - 358) < .5f);
  CHECK(st->derived_n[F_GUST_10M_KMH] == 10 && std::fabs(st->live.v[F_GUST_10M_KMH] - 16 * 1.609344f) < .1f);
  CHECK(((st->fresh >> F_GUST_10M_KMH) & 1u) && ((st->seen_fields >> F_GUST_10M_KMH) & 1u));
  // a 30-min gap: only this reading in the window
  Stations::ingest(*st, sampleFrom("ID=DW1&winddir=90&windspeedmph=4&windgustmph=6&tempf=60"), t0 + 65 * 11 + 1800,
                   1000 + 65000 * 11 + 1800000, 0, SrcIf::Ap, true, tz);
  CHECK(st->derived_n[F_WIND_AVG2M_KMH] == 1 && st->derived_n[F_GUST_10M_KMH] == 1 &&
        std::fabs(st->live.v[F_WIND_DIR_AVG2M] - 90) < .5f);
  // reported wins: windgustmph_10m sent -> not derived, the console's value stays
  Stations::ingest(*st, sampleFrom("ID=DW1&windgustmph=7&windgustmph_10m=30&tempf=61"), t0 + 65 * 12 + 1800,
                   1000 + 65000 * 12 + 1800000, 0, SrcIf::Ap, true, tz);
  CHECK(!((st->derived >> F_GUST_10M_KMH) & 1u) && std::fabs(st->live.v[F_GUST_10M_KMH] - 30 * 1.609344f) < .1f);
  CHECK((st->derived >> F_WIND_AVG2M_KMH) & 1u);
  // the notices formatting: "OK" when empty, attributes, the 255-byte cut with "…"
  Notices none = {};
  char txt[300];
  CHECK(noticesState(txt, sizeof txt, none, tz, 0) == 2 && strcmp(txt, "OK") == 0);
  CHECK(noticesAttributes(txt, sizeof txt, none) > 0 && strcmp(txt, "{\"items\":[]}") == 0);
  Notices many = {};
  many.add(NK_SENSOR_STALE, t0);
  many.add(NK_BARO_UNCAL, 0);
  many.add(NK_KEY_MISMATCH, 0, 12);
  many.add(NK_TIME_APPROX, 0);
  CHECK(noticesAttributes(txt, sizeof txt, many) > 0 &&
        strcmp(txt, "{\"items\":[{\"id\":\"sensor_stale\",\"since\":\"2026-11-04T10:00:00Z\"},{\"id\":\"baro_uncalibrated\"},"
                    "{\"id\":\"key_mismatch\",\"n\":12},{\"id\":\"time_approximate\"}]}") == 0);
  // sensor_absent is its own id; console_clock carries skew_s, store_fail n
  Notices more = {};
  more.add(NK_SENSOR_ABSENT, t0);
  more.add(NK_CONSOLE_CLOCK, 0, (uint32_t)clockSkewShown(-4000));
  more.add(NK_STORE_FAIL, 0, 3);
  more.add(NK_PROFILE_CHANGED, t0 + 60);
  {  // room for six (five station notices are possible), a seventh is dropped
    Notices six = more;
    six.add(NK_BARO_UNCAL, 0, t0, -32000, -32000);  // with the widest last-good parameters
    six.add(NK_KEY_MISMATCH, 0, 4294967295u);
    CHECK(six.n == 6 && six.item[5].kind == NK_KEY_MISMATCH);
    six.add(NK_HEAP_LOW, 0);
    CHECK(six.n == 6);
    char big[NOTICES_MAX * 64 + 16];
    const int w6 = noticesAttributes(big, sizeof big, six);
    Notices ref6 = six;  // baro_ref in the uncal slot (never both), at its widest
    ref6.item[4] = Notice{NK_BARO_REF, t0, t0, -32000, -32000};
    const int w6r = noticesAttributes(big, sizeof big, ref6);
    printf("  info: notice attributes worst case %d / %d B (buffer %u)\n", w6, w6r, (unsigned)sizeof big);
    CHECK(w6 > 0 && w6r > 0);
  }
  CHECK(noticesAttributes(txt, sizeof txt, more) > 0 &&
        strcmp(txt, "{\"items\":[{\"id\":\"sensor_absent\",\"since\":\"2026-11-04T10:00:00Z\"},{\"id\":\"console_clock\",\"skew_s\":-4020},"
                    "{\"id\":\"store_fail\",\"n\":3},{\"id\":\"profile_changed\",\"since\":\"2026-11-04T10:01:00Z\"}]}") == 0);
  {
    std::string mj;
    char mb[64];
    Out mo(toString, &mj, mb, sizeof mb);
    noticesJson(mo, more, tz, 0);
    mo.flush();
    CHECK(contains(mj, "{\"id\":\"console_clock\",\"skew_s\":-4020,\"chip\":\"Console clock off\",\"text\":\"The console's clock is off by 67 min") &&
          contains(mj, "{\"id\":\"profile_changed\",\"since\":\"2026-11-04T10:01:00Z\",\"chip\":\"Profile changed\",\"text\":\"Station profile changed: 10:01.\"}"));
  }
  const int len = noticesState(txt, sizeof txt, many, tz, 0);
  // four sentences pass 255 B: the chips instead
  CHECK(len > 0 && len <= 255 && strcmp(txt, "Sensor not updating. Pressure at factory value. Key changed. Time approximate.") == 0);
  {
    Notices two = {};  // two short sentences still fit: sentences, joined by " "
    two.add(NK_BARO_UNCAL, 0);
    two.add(NK_KEY_MISMATCH, 0, 12);
    CHECK(noticesState(txt, sizeof txt, two, tz, 0) > 0 && contains(txt, "SET menu. Another key arrived for this station (12 uploads ignored)."));
    // the count sentences: one whole template for one, one for more (no plural built from parts)
    Notices cnt = {};
    cnt.add(NK_KEY_MISMATCH, 0, 1), cnt.add(NK_KEY_MISMATCH, 0, 2), cnt.add(NK_STORE_FAIL, 0, 1), cnt.add(NK_STORE_FAIL, 0, 2);
    const char* const want[4] = {
        "Another key arrived for this station (1 upload ignored). Re-paired the console? Accept it in Settings.",
        "Another key arrived for this station (2 uploads ignored). Re-paired the console? Accept it in Settings.",
        "History not saved (1 write failed); readings still arrive. If this stays, restart fauxcloud in Settings.",
        "History not saved (2 writes failed); readings still arrive. If this stays, restart fauxcloud in Settings."};
    CHECK(cnt.n == 4);
    for (int i = 0; i < 4 && i < cnt.n; i++) CHECK(noticeText(txt, sizeof txt, cnt.item[i], tz, 0) > 0 && strcmp(txt, want[i]) == 0);
    // time_none + heap_low + store_fail = three sentences over 255 B -> three chips
    BridgeNoticeIn b3 = {};
    b3.time_valid = false;
    b3.heap_low = true;
    b3.store_fail = 3;
    const Notices n3 = bridgeNotices(b3);
    CHECK(n3.n == 3 && noticesState(txt, sizeof txt, n3, tz, 0) > 0 &&
          strcmp(txt, "Waiting for time. Low memory. History not saved.") == 0);
    Notices dup = {};  // two kinds with one chip: named once
    for (int k : {NK_SENSOR_STALE, NK_SENSOR_ABSENT, NK_KEY_MISMATCH, NK_STORE_FAIL, NK_STORE_LAYOUT}) dup.add((uint8_t)k, t0, 3);
    CHECK(noticesState(txt, sizeof txt, dup, tz, 0) > 0 &&
          strcmp(txt, "Sensor not updating. Sensor missing. Key changed. History not saved.") == 0);
  }
  char small[64];
  const int ls = noticesState(small, sizeof small, many, tz, 0);
  CHECK(ls == 63 && strcmp(small + 60, "\xE2\x80\xA6") == 0);
  BridgeNoticeIn bi = {};
  bi.time_valid = false;
  bi.time_approx = true;
  bi.store_fail = 2;
  const Notices bn = bridgeNotices(bi);
  CHECK(bn.n == 2 && noticesState(txt, sizeof txt, bn, tz, 0) > 0 &&
        strcmp(txt, "Time is estimated since the restart. Sign in to the admin page once on your phone, or connect fauxcloud to the internet. History not saved (2 writes failed); readings still arrive. If this stays, restart fauxcloud in Settings.") == 0);
  bi.time_approx = false;
  bi.time_valid = false;
  CHECK(bridgeNotices(bi).item[0].kind == NK_TIME_NONE);
  bi.time_valid = true;
  bi.store_fail = 0;
  CHECK(bridgeNotices(bi).n == 0);
  bi.frag_restart = true;
  bi.boot_t = 1794089238;
  CHECK(bridgeNotices(bi).n == 1 && bridgeNotices(bi).item[0].kind == NK_HEAP_FRAG &&
        bridgeNotices(bi).item[0].since == 1794089238 && noticesState(txt, sizeof txt, bridgeNotices(bi), tz, 0) > 0 &&
        strcmp(txt, "fauxcloud restarted itself to free memory; nothing to do.") == 0);
  bi.frag_restart = false, bi.store_layout = true;  // another partition table / ring layout
  CHECK(bridgeNotices(bi).n == 1 && bridgeNotices(bi).item[0].kind == NK_STORE_LAYOUT &&
        noticesState(txt, sizeof txt, bridgeNotices(bi), tz, 0) > 0 && strstr(txt, "needs one USB flash to prepare its storage") &&
        strcmp(NOTICES[NK_STORE_LAYOUT].id, "store_layout") == 0);
}

// every notice text but the two pressure ones fits at its worst case (the time as "yesterday 23:59") in 127 B (a 128-B
// buffer); those two are measured against NOTICE_TEXT_MAX below
static void testNoticeTextLength() {
  PosixTz tz;
  parsePosixTz("UTC0", &tz);
  size_t longest = 0;
  bool all = true;
  for (int k = 0; k < NK_COUNT; k++) {
    if (k == NK_BARO_REF || k == NK_BARO_CHECK) continue;  // their own sentence builders, measured below
    std::string t = NOTICES[k].text;
    const size_t s = t.find("%s"), l = t.find("%lu");
    if (s != std::string::npos) t.replace(s, 2, k == NK_CONSOLE_CLOCK ? "4294967295 days" : "yesterday 23:59");
    if (l != std::string::npos) t.replace(l, 3, "4294967295");
    if (t.size() > longest) longest = t.size();
    all = all && t.size() <= 127 && !strchr(NOTICES[k].text, '\xe2');  // ASCII only (no typographic apostrophe)
    // the real path renders at the worst counts and the longest day form
    char buf[128];
    Notice x{(uint8_t)k, 1794300000u, k == NK_CONSOLE_CLOCK ? 0x80000000u : 0xFFFFFFFFu};
    all = all && noticeText(buf, sizeof buf, x, tz, 1794300000u + 86400u) > 0;
  }
  printf("  info: longest notice text at worst case %u B (cap 127)\n", (unsigned)longest);
  CHECK(all && longest <= 127);
  // baro_ref at its worst (999.9 hPa, both endings) fits NOTICE_TEXT_MAX with room; ASCII only
  {
    char big[NOTICE_TEXT_MAX];
    int w = 0;
    const int16_t tgs[2] = {REL_NONE, (int16_t)32767};
    for (int16_t tg : tgs) {
      const Notice x{NK_BARO_REF, 1794300000u, 1794300000u, (int16_t)-9999, tg};
      const int n = noticeText(big, sizeof big, x, tz, 1794300000u + 86400u);  // "since yesterday 23:59"-long
      CHECK(n > 0);
      if (n > w) w = n;
    }
    printf("  info: baro_ref text at worst case %d B (buffer %u)\n", w, (unsigned)NOTICE_TEXT_MAX);
    CHECK(w > 120 && w <= (int)NOTICE_TEXT_MAX - 8 && !strchr(NOTICES[NK_BARO_REF].text, '\xe2'));
  }
  // baro_check at its worst (the largest offset the count holds, 60 days shown) fits with room; ASCII only
  {
    char big[NOTICE_TEXT_MAX];
    const Notice x{NK_BARO_CHECK, 0, baroCheckCount(-0x7FFF, 0xFFFF)};
    const int w = noticeText(big, sizeof big, x, tz, 0);
    printf("  info: baro_check text at worst case %d B (buffer %u)\n", w, (unsigned)NOTICE_TEXT_MAX);
    CHECK(w > 120 && w <= (int)NOTICE_TEXT_MAX - 8 && !strchr(NOTICES[NK_BARO_CHECK].text, '\xe2'));
  }
  // the notice's time with its day when it is not today's (as the page's dayTime): today, yesterday, a date; HH:MM
  // without a real clock or for a time after now; the day boundary in the local zone
  {
    char d[NOTICE_TEXT_MAX];
    const uint32_t s = (uint32_t)unixFromUtc(2026, 11, 17, 14, 5, 0);
    const Notice x{NK_SENSOR_STALE, s, 0};
    auto since = [&](const PosixTz& z, uint32_t now) {
      return noticeText(d, sizeof d, x, z, now) > 0 ? std::string(d).substr(0, std::string(d).find('.')) : std::string("?");
    };
    CHECK(since(tz, s + 3600) == "Outdoor sensor not updating since 14:05");
    CHECK(since(tz, (uint32_t)unixFromUtc(2026, 11, 18, 0, 10, 0)) == "Outdoor sensor not updating since yesterday 14:05");
    CHECK(since(tz, (uint32_t)unixFromUtc(2026, 11, 20, 9, 0, 0)) == "Outdoor sensor not updating since 17 Nov 14:05");
    CHECK(since(tz, 0) == "Outdoor sensor not updating since 14:05");       // no real time: HH:MM alone
    CHECK(since(tz, s - 86400) == "Outdoor sensor not updating since 14:05");  // a time after now: no day word
    PosixTz cet;
    parsePosixTz("CET-1CEST,M3.5.0,M10.5.0/3", &cet);
    const Notice late{NK_SENSOR_ABSENT, (uint32_t)unixFromUtc(2026, 11, 17, 23, 30, 0), 0};  // 00:30 on 18 Nov, local
    CHECK(noticeText(d, sizeof d, late, cet, (uint32_t)unixFromUtc(2026, 11, 18, 10, 0, 0)) > 0 && contains(d, "since 00:30:"));
    CHECK(noticeText(d, sizeof d, late, cet, (uint32_t)unixFromUtc(2026, 11, 18, 23, 30, 0)) > 0 && contains(d, "since yesterday 00:30:"));
    // the DST changes in CET: the day boundary uses each instant's own offset (the 25-h and the 23-h day)
    const Notice aut{NK_SENSOR_STALE, (uint32_t)unixFromUtc(2026, 10, 25, 22, 30, 0), 0};  // 23:30 CET on 25 Oct (back to winter time)
    CHECK(noticeText(d, sizeof d, aut, cet, (uint32_t)unixFromUtc(2026, 10, 26, 7, 0, 0)) > 0 && contains(d, "since yesterday 23:30."));
    const Notice aut0{NK_SENSOR_STALE, (uint32_t)unixFromUtc(2026, 10, 24, 22, 30, 0), 0};  // 00:30 CEST on 25 Oct
    CHECK(noticeText(d, sizeof d, aut0, cet, (uint32_t)unixFromUtc(2026, 10, 25, 22, 30, 0)) > 0 && contains(d, "since 00:30."));  // 23:30 CET: the same day
    const Notice spr{NK_SENSOR_STALE, (uint32_t)unixFromUtc(2026, 3, 29, 21, 30, 0), 0};  // 23:30 CEST on 29 Mar (summer time began)
    CHECK(noticeText(d, sizeof d, spr, cet, (uint32_t)unixFromUtc(2026, 3, 30, 6, 0, 0)) > 0 && contains(d, "since yesterday 23:30."));
    const Notice spr0{NK_SENSOR_STALE, (uint32_t)unixFromUtc(2026, 3, 28, 23, 30, 0), 0};  // 00:30 CET on 29 Mar
    CHECK(noticeText(d, sizeof d, spr0, cet, (uint32_t)unixFromUtc(2026, 3, 29, 21, 59, 0)) > 0 && contains(d, "since 00:30."));  // 23:59 CEST: the same day
    const Notice ny{NK_PROFILE_CHANGED, (uint32_t)unixFromUtc(2026, 12, 30, 8, 0, 0), 0};  // across the new year
    CHECK(noticeText(d, sizeof d, ny, tz, (uint32_t)unixFromUtc(2027, 1, 2, 8, 0, 0)) > 0 &&
          strcmp(d, "Station profile changed: 30 Dec 08:00.") == 0);
    const Notice early{NK_SENSOR_STALE, 600, 0};  // stamped before the clock was set
    CHECK(noticeText(d, sizeof d, early, tz, s) > 0 && contains(d, "since --:--."));
    // the HA state carries the same day form
    Notices one = {};
    one.add(NK_SENSOR_STALE, s);
    CHECK(noticesState(d, sizeof d, one, tz, s + 2 * 86400) > 0 && contains(d, "since 17 Nov 14:05."));
  }
  // the chips of the two time items in plain words (not "No NTP")
  CHECK(strcmp(NOTICES[NK_TIME_APPROX].chip, "Time approximate") == 0 && strcmp(NOTICES[NK_TIME_NONE].chip, "Waiting for time") == 0);
}

static void testNoticesApi() {
  PosixTz tz;
  CHECK(parsePosixTz("CET-1CEST,M3.5.0,M10.5.0/3", &tz));
  static Stations all;
  all.reserve(MAX_STATIONS);
  const uint32_t t0 = (uint32_t)unixFromUtc(2026, 11, 4, 10, 0, 0);
  Station* st = all.findOrAdd("NA1", defaultStationConfig());
  for (int i = 0; i < BARO_UNCAL_N; i++)  // the pressure flag needs BARO_UNCAL_N unchanged uploads
    Stations::ingest(*st, sampleFrom("ID=NA1&baromin=29.92&tempf=50&windspeedmph=3"), t0 + 65u * (uint32_t)i, 1000 + 65000u * (uint32_t)i, 0,
                     SrcIf::Ap, true, tz);
  static LiveSnap ls;
  snapLive(*st, 1000 + 65000u * (BARO_UNCAL_N - 1), &ls, &tz);
  std::string js;
  char buf[64];
  Out o(toString, &js, buf, sizeof buf);
  renderLiveStation(o, ls, Clock{t0 + 65u * BARO_UNCAL_N, true, 700, 1000 + 65000u * (BARO_UNCAL_N - 1)}, false);
  o.flush();
  CHECK(contains(js, "\"sensor_stale\":true,\"baro_uncal\":true,\"stale_since\":\"2026-11-04T10:00:00Z\""));
  CHECK(contains(js, "\"notices\":[{\"id\":\"sensor_stale\",\"since\":\"2026-11-04T10:00:00Z\",\"chip\":\"Sensor not updating\",\"text\":\"Outdoor sensor not updating since 11:00. A calm night can do this; a long gap means its battery or radio.\"},"
                     "{\"id\":\"baro_uncalibrated\",\"chip\":\"Pressure at factory value\",\"text\":\"The console's pressure is at its factory value; set REL once in its SET menu.\"}]"));
  // the outlook block (a barometer seen; no real clock -> the time gate, everything else null)
  snapOutlook(*st, &ls, 52, 20, false);
  std::string jo;
  Out oo(toString, &jo, buf, sizeof buf);
  renderLiveStation(oo, ls, Clock{t0 + 65u * BARO_UNCAL_N, true, 700, 1000 + 65000u * (BARO_UNCAL_N - 1)}, false);
  oo.flush();
  CHECK(contains(jo, "\"outlook\":{\"v\":1,") && contains(jo, "\"gate\":\"time\",\"state\":null,\"next\":null,\"warn\":null"));
  snapOutlook(*st, &ls, 52, 20, true);  // a real clock: fewer than 4 closed hours -> collecting; stale set -> gate stale
  CHECK(ls.outlook.present && ls.outlook.next == ON_COLLECTING && ls.outlook.left_h == 4 - st->hourly.size() && ls.outlook.gate == OG_STALE);
  BridgeStatus b;
  memset(&b, 0, sizeof b);
  b.mode = "run";
  b.time_src = "persisted";
  BridgeNoticeIn bi = {};
  bi.time_approx = true;
  b.notices = bridgeNotices(bi);
  std::string bs;
  Out ob(toString, &bs, buf, sizeof buf);
  renderBridgeStatus(ob, b);
  ob.flush();
  CHECK(contains(bs, "\"time_src\":\"persisted\",\"notices\":[{\"id\":\"time_approximate\",\"chip\":\"Time approximate\",\"text\":\"Time is estimated since the restart. Sign in to the admin page once on your phone, or connect fauxcloud to the internet.\"}],"));
  const EntitySpec n = fieldSpec(FIELD_NOTICES), l = fieldSpec(FIELD_SENSOR_LINK);
  CHECK(strcmp(n.key, "notices") == 0 && n.attributes && strcmp(n.entity_category, "diagnostic") == 0 && !n.device_class);
  CHECK(strcmp(l.key, "sensor_stale") == 0 && strcmp(l.device_class, "problem") == 0 && strcmp(l.component, "binary_sensor") == 0);
  const uint8_t mac[6] = {0x24, 0x6f, 0x28, 0xaa, 0xbb, 0xcc};
  char topic[96], payload[1152];
  CHECK(stateTopic(topic, sizeof topic, mac, "NA1", FIELD_NOTICES) > 0 && strcmp(topic, "fauxcloud/BBCC/NA1/notices") == 0);
  CHECK(discoveryPayloadSpec(payload, sizeof payload, "NA1", "x", mac, n, "v") > 0 &&
        contains(payload, "\"json_attributes_topic\":\"fauxcloud/BBCC/NA1/notices/attr\"") && contains(payload, "mdi:message-alert"));
  CHECK(discoveryPayloadSpec(payload, sizeof payload, "NA1", "x", mac, l, "v") > 0 && contains(payload, "\"payload_on\":\"1\""));
  CHECK(bridgeDiscoveryPayload(payload, sizeof payload, mac, bridgeSpec(B_NOTICES), "v", nullptr) > 0 &&
        contains(payload, "\"json_attributes_topic\":\"fauxcloud/bridge/246f28aabbcc/notices/attr\""));
  CHECK(bridgeDiscoveryPayload(payload, sizeof payload, mac, bridgeSpec(B_TIME_SRC), "v", nullptr) > 0 &&
        !contains(payload, "json_attributes_topic"));
}

// the console's clock vs ours, from dateutc; the summer +1 h of the 7-in-1 is not an error
static void testConsoleClock() {
  CHECK(!consoleClockOff(0, false) && !consoleClockOff(600, false) && consoleClockOff(601, false) && consoleClockOff(-601, false));
  CHECK(consoleClockOff(3600, false) && !consoleClockOff(3600, true) && !consoleClockOff(3900, true) && consoleClockOff(3901, true));
  CHECK(consoleClockOff(-3600, true) && consoleClockOff(-82800, true));
  PosixTz tz;
  CHECK(parsePosixTz("CET-1CEST,M3.5.0,M10.5.0/3", &tz));
  static Stations all;
  all.reserve(MAX_STATIONS);
  Station* st = all.findOrAdd("CC1", defaultStationConfig());
  // November (no DST): a console 23 h behind
  const uint32_t nov = (uint32_t)unixFromUtc(2026, 11, 4, 13, 12, 0);
  Stations::ingest(*st, sampleFrom("ID=CC1&dateutc=2026-11-3+14%3A12%3A00&tempf=50"), nov, 1000, 0, SrcIf::Ap, true, tz);
  CHECK(st->clock_off && st->clock_skew_s == -82800);
  Notices ns = stationNotices(*st);
  char txt[120];
  CHECK(ns.n == 1 && noticeText(txt, sizeof txt, ns.item[0], tz, 0) > 0 && strcmp(txt, "The console's clock is off by 23 h; switch NTP off and on in its SET menu.") == 0);
  // November, +1 h: a real error
  Stations::ingest(*st, sampleFrom("ID=CC1&dateutc=2026-11-4+14%3A13%3A05&tempf=51"), nov + 65, 66000, 0, SrcIf::Ap, true, tz);
  CHECK(st->clock_off && st->clock_skew_s == 3600);
  // June (DST): +1 h is the console's summer quirk
  const uint32_t jun = (uint32_t)unixFromUtc(2026, 6, 1, 12, 3, 18);
  Stations::ingest(*st, sampleFrom("ID=CC1&dateutc=2026-6-1+13%3A3%3A20&tempf=52"), jun, 200000, 0, SrcIf::Ap, true, tz);
  CHECK(!st->clock_off && st->clock_skew_s == 3602);
  // 12 min off: "12 min"; our clock not valid: not judged
  Stations::ingest(*st, sampleFrom("ID=CC1&dateutc=2026-6-1+12%3A16%3A23&tempf=53"), jun + 65, 265000, 0, SrcIf::Ap, true, tz);
  ns = stationNotices(*st);
  CHECK(st->clock_off && noticeText(txt, sizeof txt, ns.item[0], tz, 0) > 0 && strcmp(txt, "The console's clock is off by 12 min; switch NTP off and on in its SET menu.") == 0);
  Stations::ingest(*st, sampleFrom("ID=CC1&dateutc=2026-6-1+12%3A27%3A23&tempf=54"), 500, 330000, 0, SrcIf::Ap, false, tz);
  CHECK(!st->clock_off);
}

// the rain derivation across a profile switch; warm at a 60 s cadence
static void testRainProfileSwitch() {
  PosixTz tz;
  parsePosixTz("UTC0", &tz);
  static Stations all;
  all.reserve(MAX_STATIONS);
  const uint32_t t0 = (uint32_t)unixFromUtc(2026, 11, 4, 10, 0, 0);
  char q[120];
  // a 60 s cadence (60 readings = 59 min) reaches warm and stays warm
  Station* st = all.findOrAdd("PS1", defaultStationConfig());
  float daily = 0.10f;
  int flips = 0;
  bool was = false;
  for (int i = 0; i <= 120; i++) {
    if (i % 5 == 0) daily += 0.01f;
    snprintf(q, sizeof q, "ID=PS1&rainin=0.02&dailyrainin=%.2f", daily);
    Stations::ingest(*st, sampleFrom(q), t0 + 60u * (uint32_t)i, 1000 + 60000u * (uint32_t)i, 0, SrcIf::Ap, true, tz);
    if (st->rain_warm != was) flips++;
    was = st->rain_warm;
  }
  CHECK(st->rain_warm && flips == 1);  // warm once, never back
  // 7-in-1 -> Generic: the derived / warming flags go, the reported last hour is plain
  st->cfg.profile = Profile::Generic;
  Stations::profileChanged(*st, t0 + 7250);
  CHECK(!st->rain_warm && !st->rain_started && !((st->derived >> F_RAIN_MM) & 1u) && !((st->warming >> F_RAIN_MM) & 1u));
  CHECK(!hasValue(st->recent.at(st->recent.size() - 1).v[F_RAIN_MM]));  // the old deltas are out of the ring
  Stations::ingest(*st, sampleFrom("ID=PS1&rainin=0.50&dailyrainin=0.40"), t0 + 7300, 7400000, 0, SrcIf::Ap, true, tz);
  CHECK(std::fabs(st->live.v[F_RAIN_MM] - 12.7f) < .01f && !((st->derived >> F_RAIN_MM) & 1u) && !((st->warming >> F_RAIN_MM) & 1u));
  // Generic -> 7-in-1: not warm at once, and the Generic past-hour sums are not summed as per-upload rain
  st->cfg.profile = Profile::Yt60234;
  Stations::profileChanged(*st, t0 + 7330);
  Stations::ingest(*st, sampleFrom("ID=PS1&rainin=0.10&dailyrainin=0.41"), t0 + 7360, 7460000, 0, SrcIf::Ap, true, tz);
  // profile_changed for 24 h from the change, measured at the uploads
  {
    const Notices pn = stationNotices(*st);
    bool has = false;
    for (int i = 0; i < pn.n; i++) has = has || (pn.item[i].kind == NK_PROFILE_CHANGED && pn.item[i].since == t0 + 7330);
    CHECK(has);
    st->last_seen = t0 + 7330 + PROFILE_NOTICE_S;
    const Notices gone = stationNotices(*st);
    for (int i = 0; i < gone.n; i++) CHECK(gone.item[i].kind != NK_PROFILE_CHANGED);
  }
  // the ring is not warm, but the station has a closed 11:00 hour and the open 12:00 one, so the
  // last hour is estimated from those records
  {
    HourRec open, prev = st->hourly.at(st->hourly.size() - 1);
    CHECK(st->hourly.openRecord(&open) && prev.hour == open.hour - 3600);
    const float frac = (3600.0f - (float)(t0 + 7360 - open.hour)) / 3600.0f;
    // the branch asserted, not copied: the 11:00 hour is whole, so both hours count
    CHECK(prev.n >= PROFILE_HOUR_N_MIN && !(prev.quality & ((1u << Q_GAP) | (1u << Q_RAIN_PARTIAL) | (1u << Q_CONSOLE_RESET))));
    const float want = open.rain_sum / 100.0f + prev.rain_sum / 100.0f * frac;
    CHECK(!st->rain_warm && st->rain_hours && st->live.has(F_RAIN_MM) && std::fabs(st->live.v[F_RAIN_MM] - want) < .01f &&
          !((st->warming >> F_RAIN_MM) & 1u) && ((st->derived >> F_RAIN_MM) & 1u));
  }
  CHECK(std::fabs(recentSum(st->recent, F_RAIN_MM, t0 + 7360, RAIN_HOUR_S).sum - 0.254f) < .01f);  // this upload's tip only
}

// "Rain, last hour" from the hour records after a restart, until the ring is warm
static float hoursWant(const Station& st, uint32_t now, bool usePrev) {
  HourRec open, prev = st.hourly.at(st.hourly.size() - 1);
  st.hourly.openRecord(&open);
  calHour(&open, calFor(st.cal, open.hour));
  calHour(&prev, calFor(st.cal, prev.hour));
  return open.rain_sum / 100.0f + (usePrev ? prev.rain_sum / 100.0f * (3600.0f - (float)(now - open.hour)) / 3600.0f : 0.0f);
}
static bool liveHas(const Station& st, uint32_t now, const char* needle) {
  static LiveSnap ls;
  snapLive(st, 1000, &ls);
  std::string js;
  char buf[64];
  Out o(toString, &js, buf, sizeof buf);
  renderLiveStation(o, ls, Clock{now, true, 1, 1000}, false);
  o.flush();
  return contains(js, needle);
}
static void testRainFromHours() {
  PosixTz tz;
  parsePosixTz("UTC0", &tz);
  static Stations before, after;
  before.reserve(MAX_STATIONS), after.reserve(MAX_STATIONS);
  const uint32_t t10 = (uint32_t)unixFromUtc(2026, 11, 4, 10, 0, 0);
  char q[120];
  // before the restart: 10:00 .. 11:19 every 65 s, a 0.01 in tip every 4th upload
  StationConfig cfg = defaultStationConfig();
  Station* a = before.findOrAdd("RH1", cfg);
  float daily = 0.20f;
  uint32_t t = t10 - 3600, ms = 1000;  // from 09:00, so the 10:00 hour is whole (its first reading has a delta)
  for (int i = 0; t < t10 + 3600 + 1140; i++, t += 65, ms += 65000) {
    if (i % 4 == 3) daily += 0.01f;
    snprintf(q, sizeof q, "ID=RH1&rainin=0.03&dailyrainin=%.2f", daily);
    Stations::ingest(*a, sampleFrom(q), t, ms, 0, SrcIf::Ap, true, tz);
  }
  // the restart: what a boot brings back (the hour ring from flash, the open hour + the filter state from the
  // snapshot); the recent ring, rain_started and rain_warm start empty
  const auto restart = [&](Station* b) {
    b->hourly.clear();
    for (int i = 0; i < a->hourly.size(); i++) b->hourly.pushRaw(a->hourly.at(i));
    b->hourly.restoreAcc(a->hourly.acc());
    b->fstate = a->fstate;
    b->cal = a->cal;
  };
  Station* b = after.findOrAdd("RH1", cfg);
  restart(b);
  CHECK(b->hourly.size() >= 1 && b->hourly.at(b->hourly.size() - 1).hour == t10 && b->hourly.at(b->hourly.size() - 1).n >= PROFILE_HOUR_N_MIN);
  // the first upload after the boot, 11:20:00, +0.01 in: a value at once, from the records, tagged
  const uint32_t t1120 = t10 + 3600 + 1200;
  daily += 0.01f;
  snprintf(q, sizeof q, "ID=RH1&rainin=0.03&dailyrainin=%.2f", daily);
  Stations::ingest(*b, sampleFrom(q), t1120, 9000000, 0, SrcIf::Ap, true, tz);
  float want = hoursWant(*b, t1120, true);
  printf("  info: rain last hour after a restart at :20 from the records: %.2f mm\n", b->live.v[F_RAIN_MM]);
  CHECK(b->rain_hours && !b->rain_warm && b->live.has(F_RAIN_MM) && std::fabs(b->live.v[F_RAIN_MM] - want) < .005f && want > 0);
  CHECK(((b->derived >> F_RAIN_MM) & 1u) && !((b->warming >> F_RAIN_MM) & 1u));
  CHECK(liveHas(*b, t1120, "\"sources\":{\"rain_mm\":\"hours\"}") && !liveHas(*b, t1120, "\"warming\""));
  // uploads every 65 s with a tip every 4th until warm: crossing 12:00 uses 12:00 + 11:00; the source switches to the
  // ring exactly once, never back
  int switches = 0;
  bool hours = true, crossed = false;
  float lastHours = 0, atSwitch = 0;
  t = t1120, ms = 9000000;
  for (int i = 0; i < 90; i++) {
    t += 65, ms += 65000;
    if (i % 4 == 3) daily += 0.01f;
    snprintf(q, sizeof q, "ID=RH1&rainin=0.03&dailyrainin=%.2f", daily);
    Stations::ingest(*b, sampleFrom(q), t, ms, 0, SrcIf::Ap, true, tz);
    if (!crossed && t >= t10 + 7200 && b->rain_hours) {
      crossed = true;
      HourRec open;
      CHECK(b->hourly.openRecord(&open) && open.hour == t10 + 7200 && b->hourly.at(b->hourly.size() - 1).hour == t10 + 3600);
      CHECK(std::fabs(b->live.v[F_RAIN_MM] - hoursWant(*b, t, true)) < .005f);
    }
    if (hours && !b->rain_hours) switches++, atSwitch = b->live.v[F_RAIN_MM];
    if (!hours && b->rain_hours) switches++;
    if (b->rain_hours) lastHours = b->live.v[F_RAIN_MM];
    hours = b->rain_hours;
  }
  printf("  info: switch to the ring: hours %.2f mm -> ring %.2f mm\n", lastHours, atSwitch);
  CHECK(crossed && switches == 1 && b->rain_warm && !b->rain_hours && !liveHas(*b, t, "\"sources\""));
  CHECK(std::fabs(atSwitch - lastHours) <= .05f * atSwitch);  // steady rain: the estimate meets the ring
  // a weak previous hour (fewer readings, or a gap / partial rain / console reset): the open hour alone
  const uint16_t weak[] = {0, 1u << Q_GAP, 1u << Q_RAIN_PARTIAL, 1u << Q_CONSOLE_RESET};
  for (uint16_t flag : weak) {
    static Stations w;
    w = Stations();
    w.reserve(MAX_STATIONS);
    Station* c = w.findOrAdd("RH1", cfg);
    restart(c);
    HourRec p = c->hourly.at(c->hourly.size() - 1);
    if (flag) p.quality |= flag;
    else p.n = PROFILE_HOUR_N_MIN - 1;
    c->hourly.pushRaw(p);  // the same hour replaces the newest
    Stations::ingest(*c, sampleFrom(q), t1120, 9000000, 0, SrcIf::Ap, true, tz);
    CHECK(c->rain_hours && std::fabs(c->live.v[F_RAIN_MM] - hoursWant(*c, t1120, false)) < .005f);
  }
  // no previous closed hour (the newest is 09:00: a gap), or no clock: today's behaviour, warming and no value
  {
    static Stations g;
    g = Stations();
    g.reserve(MAX_STATIONS);
    Station* c = g.findOrAdd("RH1", cfg);
    restart(c);
    HourRec old = c->hourly.at(c->hourly.size() - 1);
    c->hourly.clear();
    old.hour = t10 - 3600;
    c->hourly.pushRaw(old);
    Stations::ingest(*c, sampleFrom(q), t1120, 9000000, 0, SrcIf::Ap, true, tz);
    CHECK(!c->rain_hours && !c->live.has(F_RAIN_MM) && ((c->warming >> F_RAIN_MM) & 1u));
    static Stations gRH2;  // a slot of its own (one station per Stations on the WROOM-32 profile)
    gRH2.reserve(MAX_STATIONS);
    Station* d = gRH2.findOrAdd("RH2", cfg);
    restart(d);
    Stations::ingest(*d, sampleFrom(q), t1120, 9000000, 0, SrcIf::Ap, false, tz);
    CHECK(!d->rain_hours && !d->live.has(F_RAIN_MM));
  }
  // the other guards of the estimate, each warming with no value -- an open hour stamped by an
  // approximate clock, a previous hour without a rain sum, an open hour with no rain seen
  for (int k = 0; k < 3; k++) {
    static Stations g3;
    g3 = Stations();
    g3.reserve(MAX_STATIONS);
    Station* c = g3.findOrAdd("RH1", cfg);
    restart(c);
    HourAcc acc = a->hourly.acc();
    const char* up = q;
    if (k == 0) acc.quality |= 1u << Q_CLOCK_APPROX;
    if (k == 1) {
      HourRec p = c->hourly.at(c->hourly.size() - 1);
      p.rain_sum = U16_NONE;
      c->hourly.pushRaw(p);
    }
    if (k == 2) acc.rain_seen = false, acc.rain_sum = 0, up = "ID=RH1&rainin=0.03&tempf=50";  // no counter: no delta
    c->hourly.restoreAcc(acc);
    Stations::ingest(*c, sampleFrom(up), t1120, 9000000, 0, SrcIf::Ap, true, tz);
    CHECK(!c->rain_hours && !c->live.has(F_RAIN_MM) && ((c->warming >> F_RAIN_MM) & 1u));
  }
  // a rain factor (1.1, in the calibration log as on the board): the estimate is calibrated like the ring
  {
    static Stations f1, f2;
    f1 = Stations(), f2 = Stations();
    f1.reserve(MAX_STATIONS), f2.reserve(MAX_STATIONS);
    StationConfig cf = cfg;
    cf.rain_factor = 1.1f;
    Station* x = f1.findOrAdd("RF1", cf);
    calAppend(&x->cal, calOf(cf, 0));
    float dd = 0.20f;
    uint32_t tt = t10 - 3600, mm = 1000;
    for (int i = 0; tt < t10 + 3600 + 1140; i++, tt += 65, mm += 65000) {
      if (i % 4 == 3) dd += 0.01f;
      snprintf(q, sizeof q, "ID=RF1&rainin=0.03&dailyrainin=%.2f", dd);
      Stations::ingest(*x, sampleFrom(q), tt, mm, 0, SrcIf::Ap, true, tz);
    }
    const float ringBefore = x->live.v[F_RAIN_MM];  // warm by now: the ring's calibrated last hour
    Station* y = f2.findOrAdd("RF1", cf);
    y->hourly.clear();
    for (int i = 0; i < x->hourly.size(); i++) y->hourly.pushRaw(x->hourly.at(i));
    y->hourly.restoreAcc(x->hourly.acc());
    y->fstate = x->fstate, y->cal = x->cal;
    snprintf(q, sizeof q, "ID=RF1&rainin=0.03&dailyrainin=%.2f", dd);  // no new tip
    Stations::ingest(*y, sampleFrom(q), tt, mm, 0, SrcIf::Ap, true, tz);
    printf("  info: rain factor 1.1: ring %.3f mm, hours estimate %.3f mm\n", ringBefore, y->live.v[F_RAIN_MM]);
    CHECK(x->rain_warm && y->rain_hours && std::fabs(y->live.v[F_RAIN_MM] - ringBefore) <= .05f * ringBefore);
  }
}

// parseAuthBody's t, the stale_since re-stamp, the console-clock signature
static void testSmallFixes() {
  AuthBody b;
  CHECK(parseAuthBody("{\"nonce\":\"ab\"}", 14, &b) && b.t_ms == 0);  // absent
  CHECK(parseAuthBody("{\"t\":\"1793810945000\"}", 21, &b) && b.t_ms == 0);  // not a number
  const char* ok = "{\"t\":1793810945123}";
  CHECK(parseAuthBody(ok, strlen(ok), &b) && b.t_ms == 1793810945123ull);
  const char* big = "{\"t\":100000000000000}";  // 1e14: over the cap
  CHECK(parseAuthBody(big, strlen(big), &b) && b.t_ms == 0);
  const char* neg = "{\"t\":-5}";
  CHECK(parseAuthBody(neg, strlen(neg), &b) && b.t_ms == 0);
  // a station that went stale before the clock was set gets its real "since" at the sync
  PosixTz tz;
  parsePosixTz("UTC0", &tz);
  static Stations all;
  all.reserve(MAX_STATIONS);
  Station* st = all.findOrAdd("RS1", defaultStationConfig());
  for (int i = 0; i < 12; i++)
    Stations::ingest(*st, sampleFrom("ID=RS1&tempf=50&windspeedmph=3"), 100 + 65u * (uint32_t)i, 100000 + 65000u * (uint32_t)i, 0, SrcIf::Ap, false, tz);
  CHECK(st->sensor_stale && st->stale_since == 100);
  static LiveSnap ls;
  snapLive(*st, 900000, &ls);
  std::string js;
  char buf[64];
  Out o(toString, &js, buf, sizeof buf);
  renderLiveStation(o, ls, Clock{900, false, 900, 900000}, false);
  o.flush();
  CHECK(contains(js, "\"sensor_stale\":true") && !contains(js, "stale_since"));  // no 1970 time
  const uint32_t off = (uint32_t)unixFromUtc(2026, 11, 4, 10, 0, 0);
  Stations::clockSynced(*st, off);
  CHECK(st->stale_since == off + 100);
  // the console_clock signature follows the text, not the seconds
  CHECK(clockSkewShown(-82800) == -82800 && clockSkewShown(-82803) == -82800 && clockSkewShown(745) == 720 &&
        clockSkewShown(755) == 780 && clockSkewShown(3 * 86400 + 500) == 3 * 86400);
  static Stations allRS2;  // a slot of its own (one station per Stations on the WROOM-32 profile)
  allRS2.reserve(MAX_STATIONS);
  Station* c = allRS2.findOrAdd("RS2", defaultStationConfig());
  c->clock_off = true;
  c->clock_skew_s = 721;
  const uint32_t s1 = noticesSig(stationNotices(*c));
  c->clock_skew_s = 723;
  CHECK(noticesSig(stationNotices(*c)) == s1);
  c->clock_skew_s = 781;
  CHECK(noticesSig(stationNotices(*c)) != s1);
}

// the stale-sensor tuning counters
static void testStaleCounters() {
  PosixTz tz;
  parsePosixTz("UTC0", &tz);
  static Stations all;
  all.reserve(MAX_STATIONS);
  const uint32_t t0 = (uint32_t)unixFromUtc(2026, 11, 4, 22, 0, 0);
  Station* st = all.findOrAdd("SC1", defaultStationConfig());
  uint32_t t = t0, ms = 1000;
  auto up = [&](const char* q) { Stations::ingest(*st, sampleFrom(q), t, ms, 0, SrcIf::Ap, true, tz); t += 78; ms += 78000; };
  for (int i = 0; i < 7; i++) up("ID=SC1&tempf=50&windspeedmph=3");  // a run of 6
  up("ID=SC1&tempf=51&windspeedmph=3");
  CHECK(st->stale_longest_run == 6 && st->stale_on_count == 0 && st->stale_run == 0);
  for (int i = 0; i < 14; i++) up("ID=SC1&tempf=51&windspeedmph=3");  // 14 more: stale at the 10th, once
  CHECK(st->sensor_stale && st->stale_on_count == 1 && st->stale_longest_run == 14);
  CHECK(std::fabs(st->last_sensor[F_WIND_KMH] - 3 * 1.609344f) < .01f && ((st->last_sensor_present >> F_WIND_KMH) & 1u));
  up("ID=SC1&tempf=52&windspeedmph=3");  // clears
  for (int i = 0; i < 10; i++) up("ID=SC1&tempf=52&windspeedmph=3");
  CHECK(st->sensor_stale && st->stale_on_count == 2 && st->stale_longest_run == 14);
  Diag d = {};
  d.stale_longest_run = 14;
  d.stale_on_count = 2;
  std::string js;
  char buf[64];
  Out o(toString, &js, buf, sizeof buf);
  renderLiveHead(o, Clock{t0, true, 1, 1}, &d);
  o.flush();
  CHECK(contains(js, "\"stale_longest_run\":14,\"stale_on_count\":2"));
}

// The stale count by kind: windy or absent STALE_SENSOR_N (10), calm STALE_CALM_N (110, about 2 h)
static void testStaleCalm() {
  CHECK(staleThreshold(true, 0, 0) == STALE_SENSOR_N && staleThreshold(false, 3.2f, 0) == STALE_SENSOR_N &&
        staleThreshold(false, 0, 1.6f) == STALE_SENSOR_N && staleThreshold(false, 0, 0) == STALE_CALM_N);
  PosixTz tz;
  parsePosixTz("UTC0", &tz);
  static Stations all;
  all.reserve(MAX_STATIONS);
  Station* st = all.findOrAdd("CA1", defaultStationConfig());
  const uint32_t t0 = (uint32_t)unixFromUtc(2026, 11, 5, 2, 0, 0);
  for (int i = 0; i <= 109; i++) {  // a still night: wind 0, the rest steady -- 109 unchanged: not yet
    Stations::ingest(*st, sampleFrom("ID=CA1&tempf=41.2&humidity=88&windspeedmph=0&windgustmph=0&UV=0&solarRadiation=0"),
                     t0 + 65u * (uint32_t)i, 1000 + 65000u * (uint32_t)i, 0, SrcIf::Ap, true, tz);
    CHECK(!st->sensor_stale);
  }
  Stations::ingest(*st, sampleFrom("ID=CA1&tempf=41.2&humidity=88&windspeedmph=0&windgustmph=0&UV=0&solarRadiation=0"),
                   t0 + 65 * 110, 1000 + 65000 * 110, 0, SrcIf::Ap, true, tz);
  CHECK(st->sensor_stale && STALE_CALM_N == 110);  // the 110th unchanged upload
  // the 2-min direction is calm-gated -- a calm reading never steers it; all calm -> no value, the last stays
  static Stations allCG1;  // a slot of its own (one station per Stations on the WROOM-32 profile)
  allCG1.reserve(MAX_STATIONS);
  Station* cg = allCG1.findOrAdd("CG1", defaultStationConfig());
  const uint32_t c0 = t0 + 20000;
  Stations::ingest(*cg, sampleFrom("ID=CG1&winddir=90&windspeedmph=5"), c0, 2000000, 0, SrcIf::Ap, true, tz);
  Stations::ingest(*cg, sampleFrom("ID=CG1&winddir=270&windspeedmph=0"), c0 + 65, 2065000, 0, SrcIf::Ap, true, tz);
  CHECK(cg->derived_n[F_WIND_DIR_AVG2M] == 1 && std::fabs(cg->live.v[F_WIND_DIR_AVG2M] - 90) < .5f);
  Stations::ingest(*cg, sampleFrom("ID=CG1&winddir=270&windspeedmph=0"), c0 + 300, 2300000, 0, SrcIf::Ap, true, tz);
  CHECK(!((cg->derived >> F_WIND_DIR_AVG2M) & 1u) && std::fabs(cg->live.v[F_WIND_DIR_AVG2M] - 90) < .5f);
}

// the calibration log, applied on read -- entry boundaries, a full log, RH clamp, direction
// wrap, 511 kept, the rose by whole sectors, the factors on wind and rain; the stored rows stay raw
static void testCalibLog() {
  StationConfig c = defaultStationConfig();
  CalLog log = {};
  CHECK(calFor(log, 100) == nullptr && calSig(log) == 0);
  CHECK(calAppend(&log, calOf(c, 0)) && log.n == 1 && !calAppend(&log, calOf(c, 5000)));  // same values: no entry
  const uint32_t t1 = (uint32_t)unixFromUtc(2026, 11, 9, 12, 0, 0);
  c.temp_c_offset = -0.5f;
  c.humidity_offset = 8;
  c.pressure_hpa_offset = 12.3f;
  c.wind_dir_offset = 30;
  c.wind_factor = 1.5f;
  c.rain_factor = 2.0f;
  const uint32_t sig1 = calSig(log);
  CHECK(calAppend(&log, calOf(c, t1)) && log.n == 2 && calSig(log) != sig1);
  CHECK(calFor(log, t1 - 1) == &log.e[0] && calFor(log, t1) == &log.e[1] && calFor(log, t1 + 99999) == &log.e[1]);
  HourRec h = hourRec(t1, 21.0f, 20.0f, 1.5f);
  h.rh_min = 90, h.rh_avg = 95, h.rh_max = 99;
  h.p_avg = 10000;
  h.wdir_steady = packDir(350, 100);
  h.wind_avg = 100, h.gust_max = 200;
  h.rain_sum = 150, h.rain_rate_max = 10;
  h.td_avg = 50;
  const HourRec raw = h;
  calHour(&h, calFor(log, h.hour));
  CHECK(h.t_avg == 205 && h.t_min == raw.t_min - 5 && h.td_avg == 50);       // T -0.5; dew point untouched
  CHECK(h.rh_min == 98 && h.rh_avg == 100 && h.rh_max == 100);               // +8 %, clamped at 100
  CHECK(h.p_avg == 10123 && dirOf(h.wdir_steady) == 20 && steadyOf(h.wdir_steady) == 100);  // 350 + 30 -> 20
  CHECK(h.wind_avg == 150 && h.gust_max == 300 && h.rain_sum == 300 && h.rain_rate_max == 20);
  HourRec before = raw;
  before.hour = t1 - 3600;
  calHour(&before, calFor(log, before.hour));
  CHECK(memcmp(&before.t_avg, &raw.t_avg, sizeof(HourRec) - 8) == 0);  // the first entry (no offsets): the raw values
  HourRec calm = raw;
  calm.wdir_steady = packDir(DIR_NONE, 0);
  calm.wind_avg = 0xFFFF;
  calm.t_avg = INT16_MIN;
  calHour(&calm, calFor(log, t1));
  CHECK(dirOf(calm.wdir_steady) == DIR_NONE && calm.wind_avg == 0xFFFF && calm.t_avg == INT16_MIN);  // sentinels stay
  // the row JSON applies it (the record itself stays raw)
  {
    std::string js;
    char b[64];
    Out o(toString, &js, b, sizeof b);
    hourRow(o, raw, RowView{0, nullptr, 0, &log});
    o.flush();
    CHECK(contains(js, "\"temp_c\":[") && contains(js, ",20.5,") && contains(js, "\"wind_dir\":20") && raw.t_avg == 210);
  }
  // daily: the entry in force at the day's start; the rose rotated by round(30 / 45) = 1 sector
  DayRec d;
  clearDay(&d, 20261110);
  d.rose[0] = 5, d.rose[7] = 2, d.rh_avg = 50, d.p_min = 9990;
  calDay(&d, calFor(log, calDayTime(d.ymd)));
  CHECK(d.rose[1] == 5 && d.rose[0] == 2 && d.rose[7] == 0 && d.rh_avg == 58 && d.p_min == 10113);
  DayRec d0;
  clearDay(&d0, 20261108);  // before the change: the first entry
  d0.rose[0] = 5;
  calDay(&d0, calFor(log, calDayTime(d0.ymd)));
  CHECK(d0.rose[0] == 5);
  // RH below 0 clamps; a negative direction offset wraps
  CalEntry neg = calOf(c, 0);
  neg.rh = -500, neg.dir = -200;
  HourRec lo = raw;
  lo.rh_min = 10;
  lo.wdir_steady = packDir(5, 3);
  calHour(&lo, &neg);
  CHECK(lo.rh_min == 0 && dirOf(lo.wdir_steady) == 345);
  // a full log drops the oldest; rows older than the new oldest read with it
  for (int i = 0; i < 10; i++) {
    c.temp_c_offset = (float)i;
    calAppend(&log, calOf(c, t1 + 3600u * (uint32_t)(i + 1)));
  }
  CHECK(log.n == CAL_MAX && log.e[0].t_from == t1 + 3600u * 3 && calFor(log, 0) == &log.e[0]);
  // the same second replaces the newest
  c.temp_c_offset = 42;
  CHECK(calAppend(&log, calOf(c, log.e[CAL_MAX - 1].t_from)) && log.n == CAL_MAX && log.e[CAL_MAX - 1].temp == 4200);
}

// a ring owned on flash with no RAM slot is forgotten by its header id; another id touches nothing
static void testForgetByHeader() {
  static RamFs fs;
  static FlashRing h, d;
  h.init(&fs, hourSpec(1));
  d.init(&fs, daySpec(1));
  const uint32_t fp = ringFingerprint();
  CHECK(ringHeaderWrite(fs, hourSpec(1), fp, "ORPH") && ringHeaderWrite(fs, daySpec(1), fp, "ORPH"));
  const uint32_t t0 = (uint32_t)unixFromUtc(2026, 11, 9, 0, 0, 0);
  for (int i = 0; i < 100; i++) {
    const HourRec r = hourRec(t0 + 3600u * (uint32_t)i, 10, 5, 0);
    CHECK(h.append(&r) != FlashRing::Add::Failed);
  }
  DayRec dr;
  clearDay(&dr, 20261109);
  sealDay(&dr);
  CHECK(d.append(&dr) != FlashRing::Add::Failed && h.count() == 100 && d.count() == 1);
  CHECK(!ringForgetOwner(fs, fp, "OTHER", &h, &d) && !ringForgetOwner(fs, fp, "", &h, &d) && h.count() == 100);
  CHECK(ringForgetOwner(fs, fp, "ORPH", &h, &d) && h.count() == 0 && d.count() == 0);
  RingHeader hh, dh;
  CHECK(ringHeaderRead(fs, hourSpec(1), fp, &hh) && !hh.id[0] && ringHeaderRead(fs, daySpec(1), fp, &dh) && !dh.id[0]);
  static FlashRing again;  // after a "reboot": the scan finds nothing
  again.init(&fs, hourSpec(1));
  CHECK(again.scan() == 0);
}

// sun.cpp = dash.js sunTimes (vectors computed by the JS original; NOAA within ~1 min for the two
// real cities checked). A generic point 52 N 20 E, a southern and a western one, polar day and night.
static void testSun() {
  struct V { int y, m, d; float lat, lon; uint32_t rise, set; };
  const V v[] = {
      {2026, 11, 10, 52, 20, 1794289827u, 1794322647u},  // 05:50 / 14:57 UTC
      {2026, 6, 21, 52, 20, 1782008382u, 1782068621u},   // 02:19 / 19:03
      {2026, 3, 20, 52, 20, 1773981804u, 1774025504u},
      {2026, 11, 6, 52, 20, 1793943793u, 1793977442u},
      {2026, 11, 10, -33.9f, 18.4f, 1794281903u, 1794331339u},  // NOAA: 05:38 / 19:22 SAST
      {2026, 11, 10, 40.7f, -74, 1794310608u, 1794346989u},     // NOAA: 06:37 / 16:42 EST
  };
  for (const V& x : v) {
    uint32_t r = 0, st = 0;
    const uint32_t noon = (uint32_t)unixFromUtc(x.y, x.m, x.d, 12, 0, 0);
    CHECK(sunTimes(noon, x.lat, x.lon, &r, &st) == SunDay::Times);
    CHECK(std::llabs((long long)r - (long long)x.rise) <= 60 && std::llabs((long long)st - (long long)x.set) <= 60);
    CHECK(sunAt(r + 60, x.lat, x.lon) == SunState::Up && sunAt(r - 60, x.lat, x.lon) == SunState::Down);
    CHECK(sunAt(st - 60, x.lat, x.lon) == SunState::Up && sunAt(st + 60, x.lat, x.lon) == SunState::Down);
  }
  // the day by local solar time -- far from 0° E a UTC day would put rise / set in the wrong day (all three "up")
  struct L { uint32_t at; float lat, lon; uint32_t rise, set; };
  const L l[] = {
      {(uint32_t)unixFromUtc(2026, 6, 23, 1, 0, 0), 37.77f, -122.42f, 1782132495u, 1782185703u},   // San Francisco 18:00 PDT
      {(uint32_t)unixFromUtc(2026, 6, 21, 21, 0, 0), 35.68f, 139.69f, 1782069947u, 1782122420u},   // Tokyo 06:00 JST
      {(uint32_t)unixFromUtc(2026, 11, 9, 21, 0, 0), -33.87f, 151.21f, 1794250049u, 1794299439u},  // Sydney 07:00 AEST
      {(uint32_t)unixFromUtc(2026, 11, 10, 12, 0, 0), 52, 20, 1794289827u, 1794322647u},           // the generic point
  };
  for (const L& x : l) {
    uint32_t r = 0, st = 0;
    CHECK(sunTimes(x.at, x.lat, x.lon, &r, &st) == SunDay::Times && std::llabs((long long)r - (long long)x.rise) <= 60 &&
          std::llabs((long long)st - (long long)x.set) <= 60 && sunAt(x.at, x.lat, x.lon) == SunState::Up);
  }
  CHECK(sunTimes((uint32_t)unixFromUtc(2026, 6, 21, 12, 0, 0), 80, 20, nullptr, nullptr) == SunDay::PolarDay);
  CHECK(sunAt((uint32_t)unixFromUtc(2026, 12, 21, 12, 0, 0), 80, 20) == SunState::Down);
  CHECK(sunAt((uint32_t)unixFromUtc(2026, 11, 10, 12, 0, 0), NAN, 20) == SunState::Unknown);
}

// ---------------------------------------------------------------- the local outlook
// Hours built as the calibrated /api/hourly rows; a generic point 52 N 20 E for tide and sun (as tendency_vectors.js).
static OutHour oh(uint32_t hour, float t, double p, float wind = 10, float rain = 0, bool calm = false, uint16_t q = 0) {
  OutHour x = {};
  x.hour = hour, x.t = t, x.td = NAN, x.p = p, x.wind = wind, x.rain = rain, x.solar = NAN, x.rh = NAN, x.quality = q, x.calm = calm;
  return x;
}
static OutlookIn oin(const OutHour* h, int n, uint32_t now) {
  OutlookIn in = {};
  in.now = now, in.time_ok = true, in.has_baro = true, in.stale = false, in.lat = 52, in.lon = 20;
  in.h = h, in.n = n, in.open_rain = 0, in.t = n ? h[n - 1].t : NAN, in.td = NAN, in.rate = NAN, in.rain10 = NAN;
  in.gust10 = NAN, in.solar = NAN;
  return in;
}
static uint32_t U(int y, int mo, int d, int hh, int mm = 0) { return (uint32_t)unixFromUtc(y, mo, d, hh, mm, 0); }

static void testForecastGolden() {
  // (1) a falling day (a real station's hourly means; UTC = local - 1): at 12:00Z raw -1.7 is steady after the
  // tide (a known threshold miss), at 13:00Z falling
  {
    const double p[] = {1032.7, 1032.0, 1031.9, 1032.1, 1031.8, 1031.4, 1031.2, 1031.0, 1030.3, 1029.5, 1028.6, 1028.4};
    OutHour h[12];
    for (int i = 0; i < 12; i++) h[i] = oh(U(2026, 11, 5, 3 + i), 8, p[i]);
    const Outlook a = outlook(oin(h, 10, U(2026, 11, 5, 13, 10)));  // newest closed 12:00Z
    CHECK(a.present && a.has_tend && std::fabs(a.tend.raw + 1.7f) < .01f && a.tend.tide && a.next == ON_P_STEADY);
    const Outlook b = outlook(oin(h, 11, U(2026, 11, 5, 14, 10)));  // newest closed 13:00Z: -2.4 raw
    CHECK(b.has_tend && b.tend.code == TC_FALLING && !b.tend.fast && b.next == ON_P_FALLING);
    CHECK(a.state == OS_DRY && a.warn == FC_NONE && a.gate == FC_NONE);  // a day, no rain: the quiet "dry"
  }
  // (2) a calm evening (hourly means rebuilt from the spot readings, calm, sunset 15:04Z):
  // night_clear + calm, ground_frost at 17:10Z (T 0.5 / Td -1.6: not yet hoar frost), hoar_frost at 18:10Z (-1.3 / -3.0)
  {
    OutHour h[8];
    const float t[] = {10.0f, 9.0f, 8.4f, 6.8f, 5.0f, 2.1f, 0.5f, -1.3f};  // 11:00Z .. 18:00Z
    for (int i = 0; i < 8; i++) h[i] = oh(U(2026, 11, 6, 11 + i), t[i], 1029.5f, 0, 0, true);
    OutlookIn in = oin(h, 6, U(2026, 11, 6, 17, 10));  // newest closed 16:00Z (-2.9 in the hour)
    in.t = 0.5f, in.td = -1.6f;
    const Outlook a = outlook(in);
    CHECK(a.night == 1 && a.night_src == 1 && a.state == OS_NIGHT_CLEAR && a.qual == OQ_CALM && a.warn == OW_GROUND_FROST);
    CHECK(a.next == ON_COOLING_FAST);  // steady pressure, the newest hour fell 2.9 °C at night
    in = oin(h, 7, U(2026, 11, 6, 18, 10));
    in.t = -1.3f, in.td = -3.0f;
    const Outlook b = outlook(in);
    CHECK(b.state == OS_NIGHT_CLEAR && b.qual == OQ_FREEZING && b.warn == OW_HOAR_FROST);  // -1.3: the band outranks calm
  }
  // (3) a night: clear cooling until the +1.9 °C jump at 19:00Z (20:00 local), "dry" after, no frost (min 3.0)
  {
    OutHour h[6];
    const float t[] = {7.0f, 5.5f, 4.0f, 3.1f, 5.0f, 5.2f};  // 15:00Z .. 20:00Z
    for (int i = 0; i < 6; i++) h[i] = oh(U(2026, 11, 5, 15 + i), t[i], 1028.4, 2);
    OutlookIn in = oin(h, 4, U(2026, 11, 5, 19, 10));  // newest 18:00Z
    in.t = 3.1f, in.td = 1.6f;
    const Outlook a = outlook(in);
    CHECK(a.state == OS_NIGHT_CLEAR && a.warn == FC_NONE && a.cool_at == U(2026, 11, 5, 17));
    in = oin(h, 5, U(2026, 11, 5, 20, 10));  // newest 19:00Z: +1.9 -> silence, never "cloud"
    in.t = 5.0f;
    const Outlook b = outlook(in);
    CHECK(b.state == OS_DRY && b.warn == FC_NONE && b.cool_at == 0);
  }
  // (4) 04:00Z on a frosty night: the +1.1 °C mixing rise under a cloudless sky -> dry (calm), never a cloud word
  {
    OutHour h[7];
    const float t[] = {-4.0f, -5.2f, -5.8f, -6.1f, -6.3f, -6.3f, -5.2f};  // 22:00Z (11-08) .. 04:00Z
    for (int i = 0; i < 7; i++) h[i] = oh(U(2026, 11, 8, 22) + 3600u * (uint32_t)i, t[i], 1026.8f, .5f, 0, true);
    OutlookIn in = oin(h, 7, U(2026, 11, 9, 5, 10));
    in.t = -5.2f, in.td = -7.0f;
    const Outlook a = outlook(in);
    CHECK(a.night == 1 && a.state == FC_NONE && a.qual == OQ_FREEZING && a.warn == FC_NONE);  // gauge-blind at -5.2
  }
  // (5) the quiet high: 32 hours within ±1.1 hPa -> p_steady throughout
  {
    OutHour h[32];
    for (int i = 0; i < 32; i++) h[i] = oh(U(2026, 11, 6, 12) + 3600u * (uint32_t)i, 6, 1028.6 + .55 * std::sin(i * .4));
    int steady = 0;
    for (int n = 4; n <= 32; n++) steady += outlook(oin(h, n, h[n - 1].hour + 4200u)).next == ON_P_STEADY;
    CHECK(steady == 29);
  }
  // (6) synthetic: rain 1 / 5 / 10 mm/h, icy at 0.5 °C with 0.3 mm in 12 h, gust 29.9 / 30.0
  {
    OutHour h[4];
    for (int i = 0; i < 4; i++) h[i] = oh(U(2026, 11, 10, 8 + i), 6, 1015, 10, i >= 2 ? .3f : 0);
    OutlookIn in = oin(h, 4, U(2026, 11, 10, 12, 10));
    in.rate = 1;
    Outlook a = outlook(in);
    CHECK(a.state == OS_RAIN_LIGHT && a.since == U(2026, 11, 10, 10));
    in.rate = 5;
    CHECK(outlook(in).state == OS_RAIN_MODERATE);
    in.rate = 10;
    CHECK(outlook(in).state == OS_RAIN_HEAVY);
    in.rate = NAN, in.rain10 = .2f;  // no rate sent: the counter's 0.2 mm in 10 min x 6 = 1.2 mm/h
    CHECK(outlook(in).state == OS_RAIN_LIGHT);
    in.rain10 = NAN, in.t = .5f;
    a = outlook(in);
    CHECK(a.state == OS_RECENT_RAIN && a.warn == OW_ICY && a.rain12 > .55f);  // the 11:00Z hour rained: recent beats blind
    in.t = 6, in.gust10 = 29.9f;
    CHECK(outlook(in).warn == FC_NONE);
    in.gust10 = 30.0f;
    a = outlook(in);
    CHECK(a.warn == OW_GUSTS && std::fabs(a.gust - 30) < .01f);
  }
  // (6b) sleet_snow: a rain state at <= 1.0 °C as shown (core.js skyFx draws flakes at the same value) reads
  // "Sleet or snow." for every intensity; 1.1 °C keeps the intensity word; the qualifier and the warnings stay
  {
    OutHour h[4];
    for (int i = 0; i < 4; i++) h[i] = oh(U(2026, 11, 10, 8 + i), 1, 1015, 10, i >= 2 ? .3f : 0);
    OutlookIn in = oin(h, 4, U(2026, 11, 10, 12, 10));
    const float rates[3] = {1, 5, 10};
    const uint8_t rain[3] = {OS_RAIN_LIGHT, OS_RAIN_MODERATE, OS_RAIN_HEAVY};
    for (int k = 0; k < 3; k++) {
      in.rate = rates[k];
      in.t = 1.0f;
      Outlook a = outlook(in);
      CHECK(a.state == OS_SLEET_SNOW && a.since == U(2026, 11, 10, 10) && a.warn == OW_ICY && a.qual == FC_NONE);
      in.t = 1.04f;  // shown as 1.0
      CHECK(outlook(in).state == OS_SLEET_SNOW);
      in.t = 1.1f;
      a = outlook(in);
      CHECK(a.state == rain[k] && a.warn == FC_NONE);
      in.t = 1.05f;  // shown as 1.1
      CHECK(outlook(in).state == rain[k]);
      in.t = -0.4f;
      a = outlook(in);
      CHECK(a.state == OS_SLEET_SNOW && a.qual == OQ_FREEZING && a.warn == OW_ICY);
      char st[256], at[320];
      CHECK(outlookState(st, sizeof st, a) > 0 && strncmp(st, "Sleet or snow. Freezing. ", 25) == 0 &&
            contains(std::string(st), "Icy surfaces possible."));
      CHECK(outlookAttributes(at, sizeof at, a) > 0 && contains(std::string(at), "\"state\":\"sleet_snow\""));
      in.t = -12.0f;
      CHECK(outlook(in).state == OS_SLEET_SNOW && outlook(in).qual == OQ_SEVERE_FROST);
    }
    in.rate = NAN, in.rain10 = .2f, in.t = .5f;  // no rate sent: the counter's rise, at 0.5 °C
    CHECK(outlook(in).state == OS_SLEET_SNOW);
    in.rain10 = NAN;  // nothing falling: recent rain stays recent rain (no temperature rule)
    CHECK(outlook(in).state == OS_RECENT_RAIN);
    in.rate = 1, in.t = NAN;  // no temperature: the intensity word
    CHECK(outlook(in).state == OS_RAIN_LIGHT);
    in.stale = true, in.t = 0;  // the stale gate clears it as every state
    CHECK(outlook(in).state == FC_NONE);
  }
  // (7) gates
  {
    OutHour h[6];
    for (int i = 0; i < 6; i++) h[i] = oh(U(2026, 11, 10, 6 + i), 6, 1015);
    OutlookIn in = oin(h, 6, U(2026, 11, 10, 12, 10));
    CHECK(outlook(in).next == ON_P_STEADY);
    h[4].quality = 1u << Q_CLOCK_APPROX;
    CHECK(outlook(in).next == ON_P_PAUSED);
    h[4].quality = 1u << Q_BARO_UNCAL;
    CHECK(outlook(in).next == ON_P_PAUSED);
    h[4].quality = 0;
    in = oin(h + 3, 3, U(2026, 11, 10, 12, 10));  // 3 hours: collecting, 1 left
    Outlook a = outlook(in);
    CHECK(a.next == ON_COLLECTING && a.left_h == 1 && !a.has_tend);
    in = oin(h, 6, U(2026, 11, 10, 15, 10));  // newest closed hour 11:00Z, 3 h old: no trend, no next line
    a = outlook(in);
    CHECK(!a.has_tend && a.next == FC_NONE);
    in = oin(h, 6, U(2026, 11, 10, 12, 10));
    in.stale = true, in.gust10 = 40;
    a = outlook(in);
    CHECK(a.gate == OG_STALE && a.state == FC_NONE && a.warn == FC_NONE && a.next == ON_P_STEADY);
    in.stale = false, in.has_baro = false;
    CHECK(!outlook(in).present);
    in.has_baro = true, in.time_ok = false;
    a = outlook(in);
    CHECK(a.present && a.gate == OG_TIME && a.state == FC_NONE && a.next == FC_NONE && a.warn == FC_NONE);
  }
  // (8) every pressure case of tendency_vectors.js (no location: tide 0; now 0: no freshness gate)
  {
    struct V { double p3; float d; uint8_t code; bool fast; };
    const V v[] = {{1001.7, 1.7f, TC_RISING, false}, {1000.5, .5f, TC_STEADY, false}, {1001.54, 1.5f, TC_STEADY, false},
                   {1001.55, 1.6f, TC_RISING, false}, {1001.6, 1.6f, TC_RISING, false}, {996.4, -3.6f, TC_FALLING, true},
                   {1003.6, 3.6f, TC_RISING, true}, {998.45, -1.6f, TC_FALLING, false}, {998.46, -1.5f, TC_STEADY, false}};
    for (const V& x : v) {
      OutHour h[4];
      for (int i = 0; i < 4; i++) h[i] = oh(U(2026, 11, 8, i), 5, i == 3 ? x.p3 : 1000);
      Trend t = {};
      CHECK(trend(h, 4, NAN, NAN, 0, &t) && std::fabs(t.d - x.d) < .001f && t.code == x.code && t.fast == x.fast && !t.tide);
    }
    OutHour g[3] = {oh(U(2026, 11, 8, 1), 5, 1000), oh(U(2026, 11, 8, 2), 5, 1000), oh(U(2026, 11, 8, 3), 5, 1001)};
    Trend t = {};
    CHECK(!trend(g, 3, NAN, NAN, 0, &t));  // fewer than 4 hours: no answer
    // the rest of tendency_vectors.js, case by case
    OutHour h[5];
    for (int i = 0; i < 4; i++) h[i] = oh(U(2026, 11, 8, i), 5, 1000 + i);
    h[3].p = 1003.5;
    CHECK(trend(h, 4, NAN, NAN, 0, &t) && std::fabs(t.d - 3.5f) < .001f && t.code == TC_RISING && !t.fast);  // +3.5 not fast
    const double gap[5] = {1000, NAN, 1002, 1003, 1004};  // the hour 3 h before the newest is missing (by index: 4 h back)
    for (int i = 0; i < 5; i++) h[i] = oh(U(2026, 11, 8, i), 5, gap[i]);
    CHECK(!trend(h, 5, NAN, NAN, 0, &t));
    const double nm[5] = {1000, 1000, 1000, 1001, NAN};  // a row without a pressure mean: the newest with one decides
    for (int i = 0; i < 5; i++) h[i] = oh(U(2026, 11, 8, i), 5, nm[i]);
    CHECK(trend(h, 5, NAN, NAN, 0, &t) && std::fabs(t.d - 1) < .001f && t.code == TC_STEADY);
    const double inhg[4] = {30.00, 30.02, 30.04, 30.06};  // an inHg console: means stored in hPa -> +2.0 rising
    for (int i = 0; i < 4; i++) h[i] = oh(U(2026, 11, 8, i), 5, inhg[i] * 33.8639);
    CHECK(trend(h, 4, NAN, NAN, 0, &t) && std::fabs(t.d - 2) < .001f && t.code == TC_RISING);
    // tide flip at 52 N 20 E: raw -1.7 from 10:00 to 13:00 UTC is steady once the tide is taken out (the JS formula,
    // mid-hour, written out independently)
    const double tf[4] = {1001.7, 1001.2, 1000.6, 1000};
    for (int i = 0; i < 4; i++) h[i] = oh(U(2026, 11, 8, 10 + i), 5, tf[i]);
    const auto s2 = [](double hh, double lat, double lon) {
      const double c = std::cos(lat * 3.14159265358979323846 / 180);
      return 1.16 * c * c * c * std::cos(2 * 3.14159265358979323846 * (hh + .5 + lon / 15 - 10) / 12);
    };
    const double tp = s2(13, 52, 20) - s2(10, 52, 20);
    CHECK(tp <= -.15);
    CHECK(trend(h, 4, 52, 20, 0, &t) && t.tide && t.code == TC_STEADY && std::fabs(t.raw + 1.7f) < .01f &&
          std::fabs(t.d - (float)(std::round((-1.7 - tp) * 10) / 10)) < .001f);
    CHECK(trend(h, 4, 52, NAN, 0, &t) && !t.tide && t.code == TC_FALLING);  // half a location counts as none
    // freshness: the newest closed hour (03:00) must start at most 2 h before `now`
    const double fr[4] = {1000, 1000, 1000, 1001.7};
    for (int i = 0; i < 4; i++) h[i] = oh(U(2026, 11, 8, i), 5, fr[i]);
    CHECK(trend(h, 4, NAN, NAN, U(2026, 11, 8, 4, 59), &t) && t.code == TC_RISING);
    CHECK(trend(h, 4, NAN, NAN, U(2026, 11, 8, 5, 0), &t) && t.code == TC_RISING);  // exactly 2 h
    CHECK(!trend(h, 4, NAN, NAN, U(2026, 11, 8, 5, 1), &t));
    CHECK(trend(h, 4, NAN, NAN, 0, &t));  // no `now`: no gate
  }
  // the temperature bands on the value as shown (0.1 °C); none when stale or gated
  {
    OutHour h[4];
    for (int i = 0; i < 4; i++) h[i] = oh(U(2026, 11, 10, 8 + i), 6, 1015);
    OutlookIn in = oin(h, 4, U(2026, 11, 10, 12, 10));
    struct B { float t; uint8_t band, state; };
    const B b[] = {{.04f, OQ_FREEZING, FC_NONE}, {.06f, FC_NONE, FC_NONE}, {0, OQ_FREEZING, FC_NONE},
                   {-9.94f, OQ_FREEZING, FC_NONE}, {-9.96f, OQ_SEVERE_FROST, FC_NONE}, {-25, OQ_SEVERE_FROST, FC_NONE},
                   {2.04f, FC_NONE, FC_NONE}, {2.06f, FC_NONE, OS_DRY},  // "No rain." only above 2.0 °C as shown
                   // hot needs the newest hour's mean too (6 °C here): a sun spike is not a band
                   {29.94f, FC_NONE, OS_DRY}, {29.96f, FC_NONE, OS_DRY}, {35, FC_NONE, OS_DRY}, {12, FC_NONE, OS_DRY},
                   {NAN, FC_NONE, OS_DRY}};
    for (const B& x : b) {
      in.t = x.t;
      const Outlook o = outlook(in);
      CHECK(o.qual == x.band && o.state == x.state);
    }
    in.t = -5;
    Outlook a = outlook(in);
    char st[256];
    CHECK(outlookState(st, sizeof st, a) > 0 && strcmp(st, "Freezing. Pressure steady.") == 0);
    in.t = 12;
    for (int i = 0; i < 4; i++) h[i].calm = true;  // calm: the lowest qualifier
    CHECK(outlook(in).qual == OQ_CALM);
    in.t = -1;
    CHECK(outlook(in).qual == OQ_FREEZING);  // a band outranks calm
    in.stale = true;
    a = outlook(in);
    CHECK(a.qual == FC_NONE && outlookState(st, sizeof st, a) > 0 && strcmp(st, "Outdoor sensor not updating. Pressure steady.") == 0);
    in.stale = false, in.time_ok = false;
    CHECK(outlook(in).qual == FC_NONE);
  }
  // hot / windy / humid enter on the newest closed hour's mean and hold on the
  // live value with a margin; priority severe_frost > freezing > hot > windy > humid > calm; humid by day only, never wet
  {
    OutHour h[4];
    for (int i = 0; i < 4; i++) h[i] = oh(U(2026, 11, 10, 8 + i), 31, 1015);
    OutlookIn in = oin(h, 4, U(2026, 11, 10, 12, 10));  // midday at 52 N: day
    in.t = 30.0f;
    CHECK(outlook(in).qual == OQ_HOT);
    in.t = 29.9f;
    CHECK(outlook(in).qual == FC_NONE);  // the live value under the line
    h[3].t = 29.9f, in.t = 31;
    CHECK(outlook(in).qual == FC_NONE);  // the hour's mean under the line
    // windy: hour mean >= 20 and the 10-min mean >= 16
    for (int i = 0; i < 4; i++) h[i] = oh(U(2026, 11, 10, 8 + i), 12, 1015, 20);
    in.t = 12, in.wind10 = 16;
    CHECK(outlook(in).qual == OQ_WINDY);
    in.wind10 = 15.9f;
    CHECK(outlook(in).qual == FC_NONE);
    in.wind10 = 30, h[3].wind = 19.9f;
    CHECK(outlook(in).qual == FC_NONE);  // a gusty 10 min after a calmer hour: not yet
    in.wind10 = NAN, h[3].wind = 25;
    CHECK(outlook(in).qual == FC_NONE);  // no 10-min mean: silent
    // humid: hour mean >= 90 and live >= 88, by day, no rain state
    for (int i = 0; i < 4; i++) h[i] = oh(U(2026, 11, 10, 8 + i), 7, 1015, 3), h[i].rh = 92;
    in.t = 7, in.wind10 = 3, in.rh = 88;
    Outlook o = outlook(in);
    CHECK(o.qual == OQ_HUMID && o.state == OS_DRY);
    char st[64];
    CHECK(outlookState(st, sizeof st, o) > 0 && strncmp(st, "No rain. Humid.", 15) == 0);
    in.rh = 87.9f;
    CHECK(outlook(in).qual == FC_NONE);
    in.rh = 95, h[3].rh = 89.9f;
    CHECK(outlook(in).qual == FC_NONE);
    h[3].rh = 92;
    in.rate = 0.5f;  // raining: the band is redundant
    o = outlook(in);
    CHECK(o.state == OS_RAIN_LIGHT && o.qual == FC_NONE);
    in.rate = 0, h[3].rain = 0.2f;  // recent rain likewise
    o = outlook(in);
    CHECK(o.state == OS_RECENT_RAIN && o.qual == FC_NONE);
    h[3].rain = 0;
    OutlookIn nt = in;
    nt.now = U(2026, 11, 10, 22, 10);  // night: RH at the cap on every clear night carries no information
    for (int i = 0; i < 4; i++) h[i].hour = U(2026, 11, 10, 18 + i);
    CHECK(outlook(nt).qual != OQ_HUMID);
    for (int i = 0; i < 4; i++) h[i].hour = U(2026, 11, 10, 8 + i);
    // priority: windy over humid, humid over calm, freezing over everything below it
    for (int i = 0; i < 4; i++) h[i].wind = 22, h[i].calm = true;
    in.wind10 = 18;
    CHECK(outlook(in).qual == OQ_WINDY);
    for (int i = 0; i < 4; i++) h[i].wind = 3;
    CHECK(outlook(in).qual == OQ_HUMID);
    in.rh = 50;
    CHECK(outlook(in).qual == OQ_CALM);
    in.t = -0.5f, in.rh = 95;
    CHECK(outlook(in).qual == OQ_FREEZING);
  }
  // recent_rain: no rain falling, a tip in the open hour or the hour before; rows only (replay-stable)
  {
    // drizzle with sparse tips: one 0.2 mm tip every ~40 min, the console's rate 0 between tips -> never "No rain."
    OutHour h[6];
    for (int i = 0; i < 6; i++) h[i] = oh(U(2026, 11, 10, 6 + i), 6, 1015, 10, i >= 2 ? .2f : 0);  // tips from 08:00Z
    int no_rain = 0;
    for (int m = 0; m < 60; m += 5) {  // 12:00Z .. 12:55Z, between tips (rate 0): the 11:00Z hour's tip, then 12:40's
      OutlookIn in = oin(h, 6, U(2026, 11, 10, 12) + 60u * (uint32_t)m);
      in.rate = 0, in.open_rain = m >= 40 ? .2f : 0;
      no_rain += outlook(in).state != OS_RECENT_RAIN;
    }
    OutHour h2[7];  // an hour later: 12:00Z closed with its tip, open 13:xx before its first tip
    for (int i = 0; i < 6; i++) h2[i] = h[i];
    h2[6] = oh(U(2026, 11, 10, 12), 6, 1015, 10, .2f);
    for (int m = 0; m < 40; m += 5) {
      OutlookIn in = oin(h2, 7, U(2026, 11, 10, 13) + 60u * (uint32_t)m);
      in.rate = 0, in.open_rain = 0;
      no_rain += outlook(in).state != OS_RECENT_RAIN;
    }
    CHECK(no_rain == 0);
    // post-shower: rain in the 10:00Z hour only
    OutHour s[4];
    for (int i = 0; i < 4; i++) s[i] = oh(U(2026, 11, 10, 8 + i), 6, 1015, 10, i == 2 ? 1.5f : 0);
    OutlookIn in = oin(s, 3, U(2026, 11, 10, 11, 30));  // newest closed 10:00Z (rained), open 11:00
    in.rate = 0;
    CHECK(outlook(in).state == OS_RECENT_RAIN);
    in = oin(s, 4, U(2026, 11, 10, 12, 10));  // newest closed 11:00Z dry, open 12:00 dry
    in.rate = 0, in.open_rain = 0;
    CHECK(outlook(in).state == OS_DRY);
    in = oin(s, 3, U(2026, 11, 10, 12, 10));  // the 11:00Z hour missing: the 10:00Z rain is not "the hour before"
    in.rate = 0;
    CHECK(outlook(in).state == OS_DRY);
    // a restart inside the window: no live rate and no 10-min counter rise (both lost), the stored rows decide
    in = oin(s, 3, U(2026, 11, 10, 11, 5));
    CHECK(std::isnan(in.rate) && std::isnan(in.rain10) && outlook(in).state == OS_RECENT_RAIN);
    in.rate = 1;  // raining outranks recent
    CHECK(outlook(in).state == OS_RAIN_LIGHT);
  }
  // line 1 and the HA sentence: every state + qualifier pair <= 28 characters; the longest sentence <= 255 B
  {
    size_t l1 = 0, ha = 0;
    char st[256];
    for (int a = 0; a < OS_COUNT; a++)
      for (int q = 0; q < OQ_COUNT; q++) {
        // a qualifier only after a state of <= 14 characters; a T3 state stands alone
        l1 = std::max(l1, OUT_STATE_QUAL[a] ? strlen(OUT_STATE_TEXT[a]) + 1 + strlen(OUT_QUAL_TEXT[q]) : strlen(OUT_STATE_TEXT[a]));
        for (int n = 0; n < ON_COUNT; n++)
          for (int w = 0; w < OW_COUNT; w++) {
            Outlook x = {};
            x.present = true, x.gate = FC_NONE, x.state = (uint8_t)a, x.qual = (uint8_t)q, x.next = (uint8_t)n, x.warn = (uint8_t)w;
            x.left_h = 3, x.gust = 142;
            ha = std::max(ha, (size_t)outlookState(st, sizeof st, x));
          }
      }
    CHECK(l1 <= 28 && ha <= 255);
    printf("  info: outlook line 1 max %u chars, HA sentence max %u B\n", (unsigned)l1, (unsigned)ha);
  }
  // night from the solar reading when there is no location
  {
    OutHour h[4];
    for (int i = 0; i < 4; i++) h[i] = oh(U(2026, 11, 10, 8 + i), 6, 1015);
    OutlookIn in = oin(h, 4, U(2026, 11, 10, 12, 10));
    in.lat = in.lon = NAN, in.solar = 2;
    Outlook a = outlook(in);
    CHECK(a.night == 1 && a.night_src == 2);
    in.solar = 200;
    a = outlook(in);
    CHECK(a.night == 0 && a.night_src == 2 && a.state == OS_DRY);
  }
  // outlookInput reads the calibrated hours (= /api/hourly) and the daily counter's 10-min rise; a drop
  // (the midnight reset) is unknown, never a negative rain
  {
    static Stations all;
    all.reserve(MAX_STATIONS);
    PosixTz tz;
    parsePosixTz("UTC0", &tz);
    Station* st = all.findOrAdd("OI1", defaultStationConfig());
    CHECK(st != nullptr);
    const uint32_t t0 = U(2026, 11, 10, 10);
    char q[96];
    for (int i = 0; i < 70; i++) {  // 10:00 .. 11:09, one a minute: 10:00Z closes; rain 0.01 in a minute from 11:05
      snprintf(q, sizeof q, "ID=OI1&baromin=%.2f&tempf=%d&dailyrainin=%.2f", i % 2 ? 30.01 : 30.00, 40 + i % 3,
               i >= 65 ? .01 * (i - 64) : 0.0);
      Stations::ingest(*st, sampleFrom(q), t0 + 60u * (uint32_t)i, 1000 + 60000u * (uint32_t)i, 0, SrcIf::Ap, true, tz);
    }
    CHECK(st->hourly.size() == 1);
    HourRec raw = st->hourly.at(0);
    CalEntry c = calOf(st->cfg, 0);
    c.p = 500;  // +5.00 hPa from always
    CHECK(calAppend(&st->cal, c));
    OutHour buf[OUT_BUF_H];
    OutlookIn in = outlookInput(*st, 52, 20, true, buf, OUT_BUF_H);
    CHECK(in.n == 1 && in.now == t0 + 69 * 60 && in.has_baro && std::fabs(in.h[0].p - (raw.p_avg / 10.0 + 5)) < .051);
    CHECK(std::fabs(in.rain10 - .05f * 25.4f) < .05f);  // 0.00 -> 0.05 in within the last 10 min (0.1 mm packing)
    snprintf(q, sizeof q, "ID=OI1&baromin=30.00&tempf=40&dailyrainin=0.00");  // the counter drops (a reset): 0 -> 1.3 -> 0
    Stations::ingest(*st, sampleFrom(q), t0 + 70 * 60, 1000 + 60000u * 70, 0, SrcIf::Ap, true, tz);
    in = outlookInput(*st, 52, 20, true, buf, OUT_BUF_H);
    CHECK(std::isnan(in.rain10));
  }
  // HA state: the longest combination fits 255 B; attributes are codes only
  {
    Outlook x = {};
    x.present = true, x.gate = FC_NONE, x.qual = OQ_CALM, x.state = OS_NIGHT_CLEAR, x.next = ON_P_FALLING_FAST, x.warn = OW_GUSTS, x.gust = 123;
    char st[256], at[320];
    const int n = outlookState(st, sizeof st, x);
    CHECK(n > 0 && n <= 255 && contains(std::string(st), "Clear night. Calm. Pressure falling fast") && contains(std::string(st), "Gusts to 123 km/h."));
    x.qual = OQ_SEVERE_FROST;
    CHECK(outlookState(st, sizeof st, x) <= 255 &&
          strcmp(st, "Clear night. Severe frost. Pressure falling fast: unsettled spell possible. Gusts to 123 km/h.") == 0);
    x.next = ON_COLLECTING, x.left_h = 2;
    outlookState(st, sizeof st, x);
    CHECK(!contains(std::string(st), "Pressure outlook in"));  // a status line only when nothing else speaks
    Outlook z = {};
    z.present = true, z.gate = z.state = z.qual = z.warn = FC_NONE, z.next = ON_COLLECTING, z.left_h = 2;
    CHECK(outlookState(st, sizeof st, z) > 0 && strcmp(st, "Pressure outlook in 2 h.") == 0);
    z.state = OS_RAIN_LIGHT, z.next = ON_P_PAUSED;
    CHECK(outlookState(st, sizeof st, z) > 0 && strcmp(st, "Light rain.") == 0);
    CHECK(outlookAttributes(at, sizeof at, x) > 0 && contains(std::string(at), "\"state\":\"night_clear\"") &&
          contains(std::string(at), "\"warn\":\"gusts\",\"qual\":\"severe_frost\"") && contains(std::string(at), "\"d3\":null"));
    Outlook g = {};
    g.present = true, g.gate = OG_TIME, g.state = g.next = g.warn = g.qual = FC_NONE;
    CHECK(outlookState(st, sizeof st, g) > 0 && strcmp(st, "Outlook waits for a valid clock.") == 0);
    const EntitySpec e = fieldSpec(FIELD_OUTLOOK);
    CHECK(strcmp(e.key, "outlook") == 0 && strcmp(e.label, "Local outlook") == 0 && e.attributes && !e.device_class &&
          !e.state_class && !e.entity_category && e.icon && strcmp(e.icon, "mdi:text-box-outline") == 0);  // no sky icon
    Outlook y = x;
    CHECK(outlookSig(x) == outlookSig(y));
    y.next = ON_P_STEADY;
    CHECK(outlookSig(x) != outlookSig(y));
  }
}

#include "test_learn.inc"
#include "test_fwd.inc"
#include "test_metar.inc"
#include "test_netq.inc"
#include "test_airport.inc"
#include "test_metar_sched.inc"
#include "test_taf.inc"
#include "test_adopt.inc"
#include "test_t3.inc"
#include "test_rel.inc"
#include "wufc.h"
#include "wufc_sched.h"
#include "test_wufc.inc"

// An error is stored whole or counted as dropped, never cut; every stored text is the one added.
static void testErrorsWhole() {
  Errors e;
  std::string why(120, 'x');
  for (int i = 0; i < 8; i++) e.add("calibration_key_name_up_to_39_chars_xyz", why.c_str());  // 39 + 2 + 120 = 161 characters
  CHECK(e.n == 3 && e.dropped == 5);  // 3 x 162 B fit the 512-byte buffer
  for (int i = 0; i < e.n; i++) CHECK(strlen(e.at(i)) == 161 && strcmp(e.at(i) + 41, why.c_str()) == 0);
  e.add("k", "short");  // a short one still fits after a long one did not
  CHECK(e.n == 4 && strcmp(e.at(3), "k: short") == 0 && strlen(e.at(2)) == 161);
  Errors m;
  for (int i = 0; i < 10; i++) m.add("k", "v");
  CHECK(m.n == Errors::MAX && m.dropped == 2 && strcmp(m.at(7), "k: v") == 0);
  // a copy keeps its texts (offsets, not pointers)
  const Errors c = e;
  CHECK(c.n == 4 && strcmp(c.at(3), "k: short") == 0);
  // the rendered array carries every whole text and the dropped count
  std::string j;
  char b[64];
  Out o(toString, &j, b, sizeof b);
  StationConfig cfg = defaultStationConfig();
  renderStationConfig(o, "S1", false, cfg, &e);
  o.flush();
  CHECK(jsonParses(j) && contains(j, "\"k: short\"") && contains(j, "\"... 5 more\""));
}

// The admin PUT decides every refusal before a slot is taken (and before the glue starts a calibration log).
static void testStationPutAdmit() {
  static Stations all;
  all.reserve(MAX_STATIONS);
  ParkedList parked = {};
  const StationConfig initial = defaultStationConfig();
  Errors scratch;
  Station* out = nullptr;
  const char* cal = "{\"calib\":{\"temp_c_offset\":-0.5}}";
  const char* name = "{\"name\":\"Garden\"}";
  const int before = all.active();
  // a new id, clock not synced, a calibration change: refused, no slot
  CHECK(stationPutAdmit(all, parked, "NEW", initial, MAX_STATIONS, false, nullptr, cal, strlen(cal), &scratch, &out) ==
            PutAdmit::CalRefused &&
        !out && all.active() == before && !all.find("NEW"));
  // the same id, a body that changes no calibration: admitted while unsynced
  CHECK(stationPutAdmit(all, parked, "NEW", initial, MAX_STATIONS, false, nullptr, name, strlen(name), &scratch, &out) ==
            PutAdmit::Ok &&
        out && all.find("NEW") == out && all.active() == before + 1);
  // an existing slot: its log's last entry is the baseline; the same values are no change
  out->cal = CalLog{};
  StationConfig was = initial;
  was.temp_c_offset = -0.5f;
  calAppend(&out->cal, calOf(was, 100));
  Station* again = nullptr;
  CHECK(stationPutAdmit(all, parked, "NEW", initial, MAX_STATIONS, false, nullptr, cal, strlen(cal), &scratch, &again) ==
            PutAdmit::Ok &&
        again == out);
  const char* back = "{\"calib\":{\"temp_c_offset\":0}}";
  CHECK(stationPutAdmit(all, parked, "NEW", initial, MAX_STATIONS, false, nullptr, back, strlen(back), &scratch, &again) ==
        PutAdmit::CalRefused);
  // with a synced clock the change is admitted
  CHECK(stationPutAdmit(all, parked, "NEW", initial, MAX_STATIONS, true, nullptr, back, strlen(back), &scratch, &again) ==
        PutAdmit::Ok);
  // a parked id waits for the boot; full slots refuse before the probe
  parked.add("PARK");
  CHECK(stationPutAdmit(all, parked, "PARK", initial, MAX_STATIONS, true, nullptr, name, strlen(name), &scratch, &again) ==
            PutAdmit::Parked &&
        !again && !all.find("PARK"));
  const int used = all.active();
  CHECK(stationPutAdmit(all, parked, "FULL", initial, used, false, nullptr, cal, strlen(cal), &scratch, &again) ==
            PutAdmit::NoSlot &&
        !again && all.active() == used);
  // a new id with a stored log (its own stations, a free slot): the stored entry is the baseline, the same offset is no
  // change and another one is
  static Stations two;
  two.reserve(1);
  CalLog stored = {};
  calAppend(&stored, calOf(was, 100));
  CHECK(stationPutAdmit(two, parked, "OLD", initial, 1, false, &stored, back, strlen(back), &scratch, &again) ==
            PutAdmit::CalRefused &&
        !again && two.active() == 0);
  CHECK(stationPutAdmit(two, parked, "OLD", initial, 1, false, &stored, cal, strlen(cal), &scratch, &again) == PutAdmit::Ok &&
        again == two.find("OLD") && two.active() == 1);
}

// The low-battery binary_sensor's state is one of its two payloads whatever the console sends.
static void testBatteryState() {
  char v[16];
  const float in[] = {0.f, 1.f, 2.f, 85.f, 0.4f, 100.f};
  const char* want[] = {"0", "1", "1", "1", "0", "1"};
  for (int i = 0; i < 6; i++) CHECK(formatState(v, sizeof v, F_BATTERY_LOW, in[i]) == 1 && strcmp(v, want[i]) == 0);
  char a[16];
  for (int f = 0; f < F_COUNT; f++) {
    if (f == F_BATTERY_LOW) continue;
    CHECK(formatState(v, sizeof v, f, 12.34f) == formatValue(a, sizeof a, f, 12.34f) && strcmp(v, a) == 0);
  }
  CHECK(formatState(v, sizeof v, F_BATTERY_LOW, NAN) < 0);
}

// Every history ETag changes with the config revision (a hidden field changes the body at once).
static void testHistEtagCfg() {
  static Stations all;
  all.reserve(1);
  Station* s = all.findOrAdd("H", defaultStationConfig());
  const HistView views[] = {HistView::Recent, HistView::Hourly, HistView::Daily};
  for (HistView h : views) {
    char a[48], b[48], c[48];
    histEtag(a, 7, *s, h);
    histEtag(b, 7, *s, h);
    s->cfg_rev++;
    histEtag(c, 7, *s, h);
    CHECK(strcmp(a, b) == 0 && strcmp(a, c) != 0);
  }
}

// The live ETag's outlook word follows every field of the learners' snapshot.
static void testLearnUseEtag() {
  static Stations all;
  all.reserve(1);
  Station* s = all.findOrAdd("U", defaultStationConfig());
  const uint32_t w0 = outlookEtagIn(*s, 52.f, 20.f, true);
  CHECK(outlookEtagIn(*s, 52.f, 20.f, true) == w0);
  int changed = 0, total = 0;
  auto probe = [&](void (*edit)(LearnUse&)) {
    const LearnUse keep = s->use;
    edit(s->use);
    total++;
    changed += outlookEtagIn(*s, 52.f, 20.f, true) != w0;
    s->use = keep;
  };
  probe([](LearnUse& u) { u.hour = 3600; });
  probe([](LearnUse& u) { u.bits = LU_L1; });
  probe([](LearnUse& u) { u.sky_cls = 1; });
  probe([](LearnUse& u) { u.sky_ref = 500; });
  probe([](LearnUse& u) { u.sky_ref_max = 600; });
  probe([](LearnUse& u) { u.rh_sat = 970; });
  probe([](LearnUse& u) { u.cool = -50; });
  probe([](LearnUse& u) { u.min_lo = -20; });
  probe([](LearnUse& u) { u.min_hi = 10; });
  probe([](LearnUse& u) { u.night_end = 1794089238; });
  probe([](LearnUse& u) { u.l7_flag = 2; });
  probe([](LearnUse& u) { u.l7_stage = 1; });
  probe([](LearnUse& u) { u.l7_days = 9; });
  probe([](LearnUse& u) { u.l7_off = -35; });
  probe([](LearnUse& u) { u.metar = true; });
  CHECK(changed == total && total == 15);
  CHECK(outlookEtagIn(*s, 52.f, 20.f, true) == w0);
}

// A skipped notice never leaves a stray comma: both renders parse with the skip at every index.
static void testNoticesSkipped() {
  PosixTz tz;
  parsePosixTz(DEFAULT_TZ, &tz);
  for (int skip = 0; skip < 3; skip++) {
    Notices ns = {};
    for (int i = 0; i < 3; i++) ns.add((uint8_t)(i == skip ? NK_COUNT : i == 0 ? NK_BARO_UNCAL : NK_TIME_APPROX), 0);
    std::string j;
    char b[64];
    Out o(toString, &j, b, sizeof b);
    noticesJson(o, ns, tz, 0);
    o.flush();
    CHECK(jsonParses(j) && j.find("[,") == std::string::npos && j.find(",,") == std::string::npos);
    char attr[512];
    CHECK(noticesAttributes(attr, sizeof attr, ns) > 0 && jsonParses(attr) && !strstr(attr, "[,"));
  }
  Notices only = {};
  only.add((uint8_t)NK_COUNT, 0);
  std::string j;
  char b[64];
  Out o(toString, &j, b, sizeof b);
  noticesJson(o, only, tz, 0);
  o.flush();
  char attr[64];
  CHECK(j == "[]" && noticesAttributes(attr, sizeof attr, only) > 0 && strcmp(attr, "{\"items\":[]}") == 0);
}

int main() {
  testForecastGolden();
  testLearn();
  testFwd();
  testMetar();
  testNetq();
  testAirport();
  testMetarSched();
  testMetarEdges();
  testTaf();
  testMetarGridTaf();
  testMetarStore();
  testSun();
  testForgetByHeader();
  testCalibLog();
  testStaleCalm();
  testStaleCounters();
  testSmallFixes();
  testRainProfileSwitch();
  testRainFromHours();
  testConsoleClock();
  testNoticesApi();
  testErrorsWhole();
  testStationPutAdmit();
  testBatteryState();
  testHistEtagCfg();
  testLearnUseEtag();
  testNoticesSkipped();
  testNoticeTextLength();
  testStaleSensor();
  testBaroUncalibrated();
  testDerivedWind();
  testDerivedEntity();
  testDefaultEntityId();
  testRainRateDerived();
  testWindAvg10m();
  testBootLog();
  testProfilesFingerprint();
  testDedupe();
  testClockLadder();
  testReadBody();
  testSeenFromSnapshot();
  testUploadVerdictAndHtml();
  testSnapshotWorth();
  testFieldMaxima();
  testOpsecB5e();
  testRateLimit();
  testStoreOnUpload();
  testSetupIdle();
  testNo1970Hour();
  testParserB5e();
  testClassifySrc();
  testCapturedRequestLine();
  testReplayMetric();
  testSolarGarbageDropsOnlySolar();
  testUnpaddedDate();
  testParserTolerance();
  testRecentRing();
  testRecords();
  testHourlyAggregate();
  testCalmNight();
  testDailyFold();
  testDiscoveryPayload();
  testRender();
  testDnsWire();
  testDnsLogLimiter();
  testNtpWire();
  testJsonReader();
  testConfigJson();
  testConfigTolerance();
  testNameUtf8();
  testConfigWorstCaseSize();
  testFilterRules();
  testFilterJump();
  testFilterCalibration();
  testRainRule();
  testPosixTz();
  testDailyDst();
  testDailyRing();
  testStationIngestProfile();
  testPbkdf2();
  testSessions();
  testLockout();
  testAccountLock();
  testNonces();
  testAuthBody();
  testCookies();
  testSettings();
  testSecretBoundToEndpoint();
  testHostAllowed();
  testLogUri();
  testSetupApply();
  testWifiAttempt();
  testHoldButton();
  testLazyStations();
  testBridgeAddendum();
  testDiagAddendum();
  testForgetSlot();
  testLed();
  testPromoteByType();
  testMaxStations();
  testStationsPerProfile();
  testLogRules();
  testSnapCarryParked();
  testAdopt();
  testT3();
  testRelCheck();
  testWufc();
  testWufcSched();
  testRose();
  testFlashRing();
  testClockHold();
  testClockHold2();
  testStreamAbort();
  testReadRun();
  testRingBudget();
  testRowGolden();
  testFormatPlan();
  testSnapshotV7();
  testSnapshotV8();
  testStoreRetry();
  testHeapThresholds();
  testPagePace();
  testStationHistoryInputs();
  testConsoleRing();
  testMetricsEtag();
  testBridgeStatusAndLiveOne();
  testBridgeDevice();
  testRestampAtSync();
  testHistBack();
  testKnownParameters();
  testAnnPromos();
  testBatteryBinarySensor();
  testLiveEtagOnline();
  testSnapTimer();
  testMarkerScan();
  testAnnList();
  testTrust();
  testTlsSni();
  testRollbackAndWatchdogs();
  testBridgeStatusOta();
  printf("%s: %d passed, %d failed\n", g_fail ? "FAILED" : "OK", g_pass, g_fail);
  return g_fail ? 1 : 0;
}
