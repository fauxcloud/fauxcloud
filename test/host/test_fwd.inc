// test_fwd.inc -- forwarding to online services: record builders and templates vs the oracle's vectors, validation,
// the response parser, the scheduler on a fake clock, target JSON, the bridge rule (forwarding with one station only).
// Included by test_fauxcloud.cpp.
#include "fwd.h"
#include "fwd_sched.h"

struct FwdVecRec {
  const char* name;
  const char* t;
  uint32_t min_s;
  const char* values;  // "key=text;..."
  const char* ages;    // "key=s;..." (absent = 0)
  const char* warming;
  const char* hidden;
  const char* wind10;  // nullptr = none
};
struct FwdVecCase {
  int rec;
  const char* type;
  bool https;
  const char* host;
  const char* url;
  int acct;
  const char* expect;
  const char* err;
};
struct FwdVecResp {  // tools/fwd_fake_endpoint.py CASES: the bytes the laptop's fake service sends
  const char* name;
  const char* type;
  const char* success;
  const char* bytes;
  size_t len;
  const char* result;
  int status;
  uint32_t retry;
};
#include "fwd_vectors.inc"

static uint32_t fwdIso(const char* s) {  // "YYYY-MM-DDTHH:MM:SSZ" -> unix
  int y, mo, d, h, mi, se;
  if (sscanf(s, "%d-%d-%dT%d:%d:%dZ", &y, &mo, &d, &h, &mi, &se) != 6) return 0;
  const int32_t days = ymdToDayNumber((uint32_t)(y * 10000 + mo * 100 + d));
  return (uint32_t)days * 86400u + (uint32_t)(h * 3600 + mi * 60 + se);
}

static uint32_t fwdMask(const char* list) {
  uint32_t m = 0;
  std::string s = list ? list : "";
  size_t p = 0;
  while (p < s.size()) {
    size_t q = s.find(',', p);
    if (q == std::string::npos) q = s.size();
    const int f = fieldByKey(s.substr(p, q - p).c_str());
    if (f >= 0) m |= 1u << f;
    p = q + 1;
  }
  return m;
}

static void fwdLiveFrom(const FwdVecRec& r, FwdLiveIn* in) {
  memset(in, 0, sizeof *in);
  in->last_seen = fwdIso(r.t);
  in->wind10 = r.wind10 ? strtof(r.wind10, nullptr) : NAN;
  in->hidden = fwdMask(r.hidden);
  in->warming = fwdMask(r.warming);
  std::string s = r.values;
  size_t p = 0;
  while (p < s.size()) {
    size_t q = s.find(';', p);
    if (q == std::string::npos) q = s.size();
    const std::string kv = s.substr(p, q - p);
    const size_t eq = kv.find('=');
    const int f = fieldByKey(kv.substr(0, eq).c_str());
    if (f >= 0) {
      in->v[f] = strtof(kv.c_str() + eq + 1, nullptr);
      in->present |= 1u << f;
      in->field_t[f] = in->last_seen;
      char b[24];
      formatValue(b, sizeof b, f, in->v[f]);  // the fixture's text is what /api/live would print
      CHECK(kv.substr(eq + 1) == b);
    }
    p = q + 1;
  }
  s = r.ages;
  p = 0;
  while (p < s.size()) {
    size_t q = s.find(';', p);
    if (q == std::string::npos) q = s.size();
    const std::string kv = s.substr(p, q - p);
    const size_t eq = kv.find('=');
    const int f = fieldByKey(kv.substr(0, eq).c_str());
    if (f >= 0) in->field_t[f] = in->last_seen - (uint32_t)atoi(kv.c_str() + eq + 1);
    p = q + 1;
  }
}

static void testFwdVectors() {
  PosixTz tz;
  CHECK(parsePosixTz(FWD_VEC_TZ, &tz));
  int n = 0, bad = 0;
  for (const FwdVecCase& c : FWD_VEC_CASES) {
    const FwdVecRec& r = FWD_VEC_RECS[c.rec];
    FwdLiveIn in;
    fwdLiveFrom(r, &in);
    FwdRecord rec;
    fwdRecordBuild(in, r.min_s, &rec);
    FwdType ty;
    CHECK(fwdTypeFromName(c.type, &ty));
    FwdTarget t = fwdDefaultTarget(ty);
    t.https = c.https;
    if (c.host) snprintf(t.wc.host, sizeof t.wc.host, "%s", c.host);
    if (c.url) {
      char why[64];
      // the oracle's https:// templates are refused at save on a FWD_TLS=0 board; the render stays byte-equal
      CHECK(fwdTemplateValid(c.url, why, sizeof why) || (!FWD_TLS && strncmp(c.url, "https://", 8) == 0));
      snprintf(t.url, sizeof t.url, "%s", c.url);
    }
    const FwdCtx ctx{&rec, FWD_VEC_ACCT[c.acct][0], FWD_VEC_ACCT[c.acct][1], FWD_VEC_SOFTWARE, &tz, false};
    static FwdUrl u;
    static char target[FWD_TARGET_MAX + 1];
    const FwdErr e = fwdRender(t, ctx, &u, target, sizeof target);
    std::string got;
    if (e == FwdErr::None) got = std::string(u.https ? "https://" : "http://") + u.host + target;
    const bool ok = c.err[0] ? (e == FwdErr::NoValue && strcmp(c.err, "no_value") == 0) : (e == FwdErr::None && got == c.expect);
    CHECK(ok);
    if (!ok && bad++ < 5) printf("  fwd vector %d (%s %s): err %s\n    got    %s\n    expect %s\n", n, r.name, c.type,
                                 fwdErrName(e), got.c_str(), c.expect);
    n++;
  }
  printf("  info: fwd vectors %d cases vs fwd_reference.py, %d differ\n", n, bad);
}

static FwdRecord fwdRec(uint32_t t) {
  FwdRecord r;
  memset(&r, 0, sizeof r);
  r.t = t;
  return r;
}
static std::string fwdVal(const FwdRecord& r, const char* name, const PosixTz* tz = nullptr) {
  const FwdCtx c{&r, "EXAMPLE1", "examplekey", "fauxcloud-test", tz, false};
  char b[128];
  const int n = fwdVar(c, name, strlen(name), b, sizeof b);
  return n < 0 ? (n == -1 ? "<absent>" : "<unknown>") : std::string(b, (size_t)n);
}

static void testFwdBuilders() {
  // conversions and decimals on doubles parsed back from the /api/live text
  FwdRecord r = fwdRec(fwdIso("2026-01-01T00:00:00Z"));
  auto set = [&](int f, double v) { r.v[f] = v, r.has |= 1u << f; };
  set(F_TEMP_C, 21.5);         // 21.5 * 9 / 5 + 32 = 70.7
  set(F_PRESSURE_HPA, 1013.2);
  set(F_WIND_KMH, 9.0);
  set(F_GUST_KMH, 14.4);
  set(F_RAIN_MM, 0.25);
  set(F_HUMIDITY, 85);
  CHECK(fwdVal(r, "tempf") == "70.7");
  CHECK(fwdVal(r, "baromin") == "29.920");
  CHECK(fwdVal(r, "windspeedmph") == "5.6");
  CHECK(fwdVal(r, "wind_ms") == "2.5");
  CHECK(fwdVal(r, "gust_kmh") == "14.4");
  CHECK(fwdVal(r, "rainin") == "0.010");
  CHECK(fwdVal(r, "rain_1h_mm") == "0.25");
  CHECK(fwdVal(r, "humidity") == "85");
  CHECK(fwdVal(r, "UV") == "<absent>");
  CHECK(fwdVal(r, "nope") == "<unknown>");
  // .x5 boundaries: printf rounds the binary double (0.25 / 0.35 are not ties in binary except 0.25)
  set(F_UV, 0.25);
  CHECK(fwdVal(r, "UV") == "0.2");  // 0.25 is exact: round-half-even, as Python
  set(F_UV, 0.35);
  CHECK(fwdVal(r, "UV") == "0.3");  // 0.35 is 0.34999...: down, as Python
  set(F_UV, -0.04);
  CHECK(fwdVal(r, "UV") == "-0.0");  // Python prints -0.0 too
  set(F_UV, NAN);
  CHECK(fwdVal(r, "UV") == "<absent>");
  // the chains: 10-min mean -> 2-min -> latest; 10-min gust -> latest
  CHECK(fwdVal(r, "wind_mean_ms") == "2.5");
  set(F_WIND_AVG2M_KMH, 7.2);
  CHECK(fwdVal(r, "wind_mean_ms") == "2.0");
  r.wind10 = 3.6, r.has_wind10 = true;
  CHECK(fwdVal(r, "wind_mean_ms") == "1.0");
  CHECK(fwdVal(r, "wind_avg10m_ms") == "1.0");
  CHECK(fwdVal(r, "gust_max_ms") == "4.0");
  set(F_GUST_10M_KMH, 18.0);
  CHECK(fwdVal(r, "gust_max_ms") == "5.0");
  CHECK(fwdVal(r, "wind_mean_kn") == "1.9");  // 3.6 / 1.852 = 1.94
  CHECK(fwdVal(r, "gust_max_kn") == "9.7");   // 18.0 / 1.852 = 9.72
  // time: padding at a year boundary, compact, ISO, epoch
  r.t = fwdIso("2026-01-02T03:04:05Z");
  CHECK(fwdVal(r, "dateutc") == "2026-01-02 03:04:05");
  CHECK(fwdVal(r, "dateutc_iso") == "2026-01-02T03:04:05Z");
  CHECK(fwdVal(r, "dateutc_compact") == "202601020304");
  CHECK(fwdVal(r, "epoch") == std::to_string(r.t));
  CHECK(fwdVal(r, "softwaretype") == "fauxcloud-test");
  // local date / time across the DST change (CET -> CEST at 01:00 UTC on the last Sunday of March)
  PosixTz tz;
  CHECK(parsePosixTz("CET-1CEST,M3.5.0,M10.5.0/3", &tz));
  r.t = fwdIso("2026-03-29T00:59:59Z");
  CHECK(fwdVal(r, "date_local", &tz) == "2026-03-29" && fwdVal(r, "time_local", &tz) == "01:59:59");
  r.t = fwdIso("2026-03-29T01:00:00Z");
  CHECK(fwdVal(r, "time_local", &tz) == "03:00:00");
  r.t = fwdIso("2025-12-31T23:30:00Z");
  CHECK(fwdVal(r, "date_local", &tz) == "2026-01-01" && fwdVal(r, "dateutc") == "2025-12-31 23:30:00");
  r.t = 999999999;  // before EPOCH_MIN: no time variables
  CHECK(fwdVal(r, "dateutc") == "<absent>");
  // the drops: hidden, warming, unknown age, age > min_s (= min_s kept)
  FwdLiveIn in;
  memset(&in, 0, sizeof in);
  in.last_seen = 1800000000u;
  in.wind10 = 4.44f;
  for (int f : {F_TEMP_C, F_HUMIDITY, F_UV, F_RAIN_MM, F_WIND_KMH}) in.present |= 1u << f, in.field_t[f] = in.last_seen;
  in.v[F_TEMP_C] = -3.45f;  // the float the station holds; /api/live prints -3.5 or -3.4 -- whichever, the same text
  in.v[F_HUMIDITY] = 77;
  in.field_t[F_HUMIDITY] = in.last_seen - 300;
  in.field_t[F_UV] = 0;
  in.v[F_UV] = 1;
  in.v[F_RAIN_MM] = 0.5f;
  in.warming = 1u << F_RAIN_MM;
  FwdRecord rr;
  fwdRecordBuild(in, 300, &rr);
  char txt[24];
  formatValue(txt, sizeof txt, F_TEMP_C, in.v[F_TEMP_C]);
  CHECK(((rr.has >> F_TEMP_C) & 1u) && rr.v[F_TEMP_C] == strtod(txt, nullptr));
  CHECK((rr.has >> F_HUMIDITY) & 1u);
  CHECK(!((rr.has >> F_UV) & 1u));
  CHECK(!((rr.has >> F_RAIN_MM) & 1u));
  CHECK(rr.has_wind10 && rr.wind10 == 4.4);
  fwdRecordBuild(in, 299, &rr);
  CHECK(!((rr.has >> F_HUMIDITY) & 1u));
  in.hidden = 1u << F_WIND_KMH;
  fwdRecordBuild(in, 300, &rr);
  CHECK(!rr.has_wind10 && !((rr.has >> F_WIND_KMH) & 1u));
  // the data gate
  FwdGateIn g{true, true, false, false, true, 1800000000u, true, false, 0};
  CHECK(fwdGate(g, false) == FwdGate::Ok);
  g.ok_seen = g.last_seen;
  CHECK(fwdGate(g, false) == FwdGate::Same && fwdGate(g, true) == FwdGate::Ok);
  g.ok_seen = 0;
  g.has_temp = false;
  CHECK(fwdGate(g, false) == FwdGate::NoCore);
  g.has_wind = true;
  g.time_ok = false;
  CHECK(fwdGate(g, false) == FwdGate::NoTime);
  g.time_ok = true;
  g.restored = true;
  CHECK(fwdGate(g, false) == FwdGate::Restored);
  g.sensor_stale = true;
  CHECK(fwdGate(g, false) == FwdGate::Stale);
  g.online = false;
  CHECK(fwdGate(g, false) == FwdGate::Offline);
  g.exists = false;
  CHECK(fwdGate(g, true) == FwdGate::NoStation);
  g = FwdGateIn{true, true, false, false, true, 999999999u, true, true, 0};
  CHECK(fwdGate(g, false) == FwdGate::Old);
}

static void testFwdTemplates() {
  char why[64];
  CHECK(fwdTemplateValid("http://example.org/x?a={tempf}", why, sizeof why));
  // https:// only on a TLS board
  CHECK(fwdTemplateValid("https://example.org/x?a={tempf}", why, sizeof why) == (FWD_TLS != 0));
  if (!FWD_TLS) CHECK(strstr(why, "no encryption") != nullptr);
  CHECK(!fwdTemplateValid("http://example.org/x?a={nope}", why, sizeof why) && strstr(why, "{nope}"));
  CHECK(!fwdTemplateValid("http://example.org/{wc_temp}", why, sizeof why) && strstr(why, "wc_temp"));  // not a template field
  CHECK(!fwdTemplateValid("http://user:pw@example.org/x", why, sizeof why));
  CHECK(!fwdTemplateValid("http://example.org/x#frag", why, sizeof why));
  CHECK(!fwdTemplateValid("ftp://example.org/x", why, sizeof why));
  CHECK(!fwdTemplateValid("http://example.org?x=1", why, sizeof why));  // a path is needed
  CHECK(!fwdTemplateValid("http://example.org/a b", why, sizeof why));
  CHECK(!fwdTemplateValid("http://example.org/x?a=%C3", why, sizeof why));  // a half UTF-8 sequence
  CHECK(!fwdTemplateValid("http://exa_mple.org/x", why, sizeof why));
  CHECK(!fwdTemplateValid("http://example.org:99999/x", why, sizeof why));
  CHECK(fwdTemplateValid("http://example.org:8080/x?a=%7Btempf%7D", why, sizeof why));
  CHECK(!fwdTemplateValid("http://example.org/x?a=%7Bnope%7D", why, sizeof why));  // decoded placeholders count
  std::string big = "http://example.org/x?";
  while (big.size() < 380) big += "a";
  CHECK(fwdTemplateValid(big.c_str(), why, sizeof why));
  big += "aaaaa";
  CHECK(!fwdTemplateValid(big.c_str(), why, sizeof why));
  // a render over 768 B: "too long"; a missing path variable: "no value"
  FwdRecord r = fwdRec(1800000000u);
  r.v[F_TEMP_C] = 1.5, r.has = 1u << F_TEMP_C;
  std::string key(96, '%');  // 96 x "%25" = 288 B per use
  const FwdCtx c{&r, "EXAMPLE1", key.c_str(), "fauxcloud-test", nullptr, false};
  static FwdUrl u;
  static char tg[FWD_TARGET_MAX + 1];
  CHECK(fwdRenderUrl("https://example.org/a?k={key}&l={key}", c, &u, tg, sizeof tg) == FwdErr::None && strlen(tg) == 2 + 3 + 288 + 3 + 288);
  CHECK(fwdRenderUrl("https://example.org/a?k={key}&l={key}&m={key}", c, &u, tg, sizeof tg) == FwdErr::TooLong && !tg[0]);
  CHECK(fwdRenderUrl("https://example.org/{UV}?a=1", c, &u, tg, sizeof tg) == FwdErr::NoValue && strcmp(u.why, "UV") == 0);
  CHECK(fwdRenderUrl("https://example.org/a?x={bogus}", c, &u, tg, sizeof tg) == FwdErr::UnknownVar);
  {  // a 768-B target fits exactly, 769 does not (the cap is the request line's)
    std::string tpl = "https://example.org/";
    tpl += std::string(767, 'a');
    CHECK(fwdRenderUrl(tpl.c_str(), c, &u, tg, sizeof tg) == FwdErr::None && strlen(tg) == 768);
    tpl += "b";
    CHECK(fwdRenderUrl(tpl.c_str(), c, &u, tg, sizeof tg) == FwdErr::TooLong);
  }
  // the redacted target: {key} anywhere is a bare ***
  const FwdCtx red{&r, "EXAMPLE1", "examplekey", "fauxcloud-test", nullptr, true};
  CHECK(fwdRenderUrl("https://example.org/k/{key}?ID={id}&PASSWORD={key}&t={temp_c}&x=a{key}b", red, &u, tg, sizeof tg) ==
            FwdErr::None && strcmp(tg, "/k/***?ID=EXAMPLE1&PASSWORD=***&t=1.5&x=a***b") == 0);
  FwdTarget t = fwdDefaultTarget(FwdType::Wu);
  CHECK(fwdRender(t, red, &u, tg, sizeof tg) == FwdErr::None && strstr(tg, "PASSWORD=***&action=updateraw") &&
        u.https == (FWD_TLS && FWD_HTTPS_DEFAULT) &&
        strcmp(u.host, "rtupdate.wunderground.com") == 0);
  t = fwdDefaultTarget(FwdType::WuCompat);
  CHECK(fwdRender(t, red, &u, tg, sizeof tg) == FwdErr::BadUrl);  // no host yet
  // every name in the public list is known, and nothing else
  for (int i = 0; i < FWD_VAR_COUNT; i++) CHECK(fwdVarKnown(fwdVarName(i), strlen(fwdVarName(i))));
  CHECK(strcmp(fwdVarName(0), "tempf") == 0 && strcmp(fwdVarName(FWD_VAR_COUNT - 1), "key") == 0 && !fwdVarName(FWD_VAR_COUNT)[0]);
  CHECK(FWD_VAR_COUNT == 43);
}

static FwdResult fwdParse(FwdType t, const char* success, const std::vector<std::string>& parts, bool close = true,
                          FwdResp* out = nullptr) {
  static FwdResp p;
  p.begin(t, success);
  bool done = false;
  for (const std::string& s : parts)
    if (!done) done = p.feed(s.data(), s.size());
  if (!done && close) p.end();
  if (out) *out = p;
  return p.result();
}

static void testFwdResponses() {
  const std::string ok200 = "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: 8\r\n\r\nsuccess\n";
  CHECK(fwdParse(FwdType::Wu, nullptr, {ok200}) == FwdResult::Ok);
  CHECK(fwdParse(FwdType::Wu, nullptr, {"HTTP/1.1 200 OK\r\n\r\nINVALIDPASSWORDID|Password and/or id are incorrect\n"}) ==
        FwdResult::Refused);
  CHECK(fwdParse(FwdType::Wu, nullptr, {"HTTP/1.1 401 Unauthorized\r\nContent-Length: 12\r\n\r\nunauthorized"}) ==
        FwdResult::Refused);
  CHECK(fwdParse(FwdType::Wu, nullptr, {"HTTP/1.1 400 Bad Request\r\n\r\nbad request"}) == FwdResult::Reject);
  CHECK(fwdParse(FwdType::Wu, nullptr, {"HTTP/1.1 200 OK\r\n\r\nsomething else"}) == FwdResult::Reject);
  CHECK(fwdParse(FwdType::PwsWeather, nullptr,
                 {"HTTP/1.1 200 OK\r\nContent-Length: 29\r\n\r\n{\"error\":null,\"success\":true}"}) == FwdResult::Ok);
  CHECK(fwdParse(FwdType::PwsWeather, nullptr,
                 {"HTTP/1.1 200 OK\r\n\r\n{\"error\":{\"code\":\"invalid\"},\"success\":false}"}) == FwdResult::Reject);
  CHECK(fwdParse(FwdType::Windy, nullptr, {"HTTP/1.1 200 OK\r\n"}, false) == FwdResult::Ok);  // status line only
  CHECK(fwdParse(FwdType::Windy, nullptr, {"HTTP/1.1 409 Conflict\r\n"}, false) == FwdResult::Dup);
  FwdResp p;
  CHECK(fwdParse(FwdType::Windy, nullptr, {"HTTP/1.1 429 Too Many\r\nRetry-After: 240\r\n\r\n{\"retry_after\":240}"},
                 false, &p) == FwdResult::Limited && p.retryAfter() == 240 && p.bodyLen() == 0);
  CHECK(fwdParse(FwdType::Windy, nullptr, {"HTTP/1.1 429 Too Many\r\n\r\n"}, false, &p) == FwdResult::Limited &&
        p.retryAfter() == 0);
  CHECK(fwdParse(FwdType::Windy, nullptr, {"HTTP/1.1 500 Oops\r\n"}, false) == FwdResult::Fail);
  CHECK(fwdParse(FwdType::Windy, nullptr, {"HTTP/1.1 401 Unauthorized\r\n"}, false) == FwdResult::Refused);
  // WOW-BE: the status line only, the GeoJSON body (coordinates) is never read
  CHECK(fwdParse(FwdType::WowBe, nullptr, {"HTTP/1.1 200 OK\r\nContent-Type: application/geo+json\r\n\r\n{\"type\":\"Feature\""},
                 false, &p) == FwdResult::Ok && p.bodyLen() == 0 && p.done());
  // chunked, a status line split across reads
  CHECK(fwdParse(FwdType::Template, "accepted", {"HT", "TP/1.1 2", "00 OK\r\nTransfer-Encoding: chunked\r\n\r\n4\r\nacce",
                                                 "\r\n4\r\npted\r\n0\r\n\r\n"}, true, &p) == FwdResult::Ok &&
        strcmp(p.body(), "accepted") == 0);
  CHECK(fwdParse(FwdType::WuCompat, "success", {"HTTP/1.0 200 OK\r\n\r\nnope"}) == FwdResult::Reject);
  CHECK(fwdParse(FwdType::WuCompat, "", {"HTTP/1.0 204 No Content\r\n\r\n"}) == FwdResult::Ok);
  // headers over 1 KB: stop, judged without a body
  std::string big = "HTTP/1.1 200 OK\r\n";
  for (int i = 0; i < 40; i++) big += "X-Pad: 01234567890123456789012345678901\r\n";
  big += "\r\nsuccess";
  CHECK(fwdParse(FwdType::Wu, nullptr, {big}, true, &p) == FwdResult::Reject && p.done());
  // a body over 256 B: the first 256 only
  std::string longBody = "HTTP/1.1 200 OK\r\n\r\nsuccess" + std::string(400, 'x');
  CHECK(fwdParse(FwdType::Wu, nullptr, {longBody}, false, &p) == FwdResult::Ok && p.bodyLen() == 256 && p.done());
  // garbage; a connection closed before the status line; a short body
  CHECK(fwdParse(FwdType::Wu, nullptr, {"SSH-2.0-OpenSSH\r\n"}) == FwdResult::Reject);
  CHECK(fwdParse(FwdType::Wu, nullptr, {"HTTP/1.1 2"}) == FwdResult::Fail);
  CHECK(fwdParse(FwdType::Wu, nullptr, {""}) == FwdResult::Fail);
  CHECK(fwdParse(FwdType::Wu, nullptr, {"HTTP/1.1 200 OK\r\nContent-Length: 50\r\n\r\nsucc"}) == FwdResult::Fail);
  CHECK(fwdParse(FwdType::Wu, nullptr, {"HTTP/1.1 200 OK\r\n\r\nsuccess"}) == FwdResult::Ok);  // body until close
  CHECK(fwdParse(FwdType::Wu, nullptr, {"HTTP/1.1 503 Busy\r\n"}) == FwdResult::Fail);
  CHECK(fwdParse(FwdType::Wu, nullptr, {"HTTP/1.1 301 Moved\r\nLocation: x\r\n\r\n"}) == FwdResult::Reject);
  // the 1536-B cap
  std::string flood = "HTTP/1.1 200 OK\r\n";
  for (int i = 0; i < 30; i++) flood += "A: " + std::string(60, 'a') + "\r\n";
  CHECK(fwdParse(FwdType::PwsWeather, nullptr, {flood}, false, &p) == FwdResult::Reject && p.done());
  CHECK(strcmp(fwdResultName(FwdResult::TlsUntrusted), "tls_untrusted") == 0);
}

// ---------------------------------------------------------------- scheduler on a fake clock
struct FwdSim {
  FwdSched s;
  FwdTickIn in;
  int starts = 0, maxRunning = 0;
  FwdSim() {
    memset(&in, 0, sizeof in);
    in.now_ms = 1000;
    in.free = 100000;
    in.largest = 50000;
    in.netout_idle = true;
  }
  void on(int i, uint16_t min_s, bool tls = true, FwdType t = FwdType::Wu) {
    in.s[i].on = true;
    in.s[i].test_ok = true;
    in.s[i].tls = tls;
    in.s[i].min_s = min_s;
    FwdTarget x = fwdDefaultTarget(t);
    x.min_s = min_s;
    in.s[i].max_s = fwdMaxS(x);
    in.s[i].floor_s = fwdMinFloor(t);
    in.s[i].gate = in.s[i].gate_test = FwdGate::Ok;
  }
  FwdStart tick() {
    FwdStart st = s.tick(in);
    if (st.slot >= 0) starts++;
    return st;
  }
  void advance(uint32_t ms) { in.now_ms += ms; }
};

// the unconfirmed rule: a written request whose answer was lost is not retried inside the service minimum (a 30-s
// retry after a lost answer from Windy meets its 5-min limit with 429)
static void testFwdUnconfirmed() {
  // written = the request left whole and no status line came back
  CHECK(fwdWritten(true, -1, FwdResult::Fail));            // plain HTTP: written, then a time-out or a close
  CHECK(!fwdWritten(false, -1, FwdResult::Fail));          // TLS buffered but the flush failed: never sent
  CHECK(!fwdWritten(true, 503, FwdResult::Fail));          // an answer came: the service said no
  CHECK(!fwdWritten(true, -1, FwdResult::NoMemory) && !fwdWritten(true, 200, FwdResult::Ok));
  {  // Windy (floor 300 s): unconfirmed, no retry, no fail, no drop; the next window as usual
    FwdSim m;
    m.on(0, 310, true, FwdType::Windy);
    FwdStart st;
    for (int k = 0; k < 400 && (st = m.tick()).slot < 0; k++) m.advance(1000);  // the first window waits the floor
    CHECK(st.slot == 0 && st.kind == FwdKind::Send);
    const uint32_t t0 = m.in.now_ms;
    m.advance(5400);  // the read timeout
    m.s.result(0, FwdResult::Fail, m.in.now_ms, 0, 1, true);
    CHECK(m.s.s[0].n.unconfirmed == 1 && m.s.s[0].n.fail == 0 && m.s.s[0].n.drop == 0 && m.s.s[0].st == FwdSt::Idle);
    int starts = 0;
    while (m.in.now_ms < t0 + 309000) {
      m.advance(1000);
      starts += m.tick().slot >= 0;
    }
    CHECK(starts == 0);  // nothing inside the minimum
    for (int k = 0; k < 130 && m.tick().slot < 0; k++) m.advance(1000);
    CHECK(m.s.running() && m.in.now_ms - t0 >= 310000);  // the next regular window
  }
  {  // WU (floor 60 s): the 30-s retry would also be inside it -> unconfirmed
    FwdSim m;
    m.on(0, 300);
    CHECK(m.tick().slot == 0);
    m.advance(2000);
    m.s.result(0, FwdResult::Fail, m.in.now_ms, 0, 1, true);
    CHECK(m.s.s[0].n.unconfirmed == 1 && m.s.s[0].st == FwdSt::Idle);
  }
  {  // not written (connect, dns, a 5xx answer): the retry as before
    FwdSim m;
    m.on(0, 310, true, FwdType::Windy);
    FwdStart st;
    for (int k = 0; k < 400 && (st = m.tick()).slot < 0; k++) m.advance(1000);
    m.advance(1000);
    m.s.result(0, FwdResult::Fail, m.in.now_ms, 0, 1, false);
    CHECK(m.s.s[0].n.unconfirmed == 0 && m.s.s[0].n.fail == 1 && m.s.s[0].st == FwdSt::Retry);
  }
  {  // a service with no minimum (a custom template, floor 0): written or not, the retry as before
    FwdSim m;
    m.on(0, 300);
    m.in.s[0].floor_s = 0;
    CHECK(m.tick().slot == 0);
    m.advance(2000);
    m.s.result(0, FwdResult::Fail, m.in.now_ms, 0, 1, true);
    CHECK(m.s.s[0].n.unconfirmed == 0 && m.s.s[0].n.fail == 1 && m.s.s[0].st == FwdSt::Retry);
  }
}

static void testFwdScheduler() {
  {  // due -> idle -> send -> ok; next due = start + min_s
    FwdSim m;
    m.on(0, 300);
    FwdStart st = m.tick();
    CHECK(st.slot == 0 && st.kind == FwdKind::Send && m.s.running());
    const uint32_t t0 = m.in.now_ms;
    m.advance(2000);
    CHECK(m.tick().slot < 0);  // one at a time
    m.s.result(0, FwdResult::Ok, m.in.now_ms, 0, 1800000000u);
    CHECK(m.s.s[0].n.ok == 1 && m.s.s[0].ok_seen == 1800000000u && m.s.s[0].due_ms == t0 + 300000u);
    CHECK(m.s.nextS(0, m.in.now_ms) == 298);
    m.advance(297000);
    CHECK(m.tick().slot < 0);
    m.advance(1000);
    CHECK(m.tick().slot == 0);
  }
  {  // busy 10 min: every window skipped, nothing queued, one send after it (no burst)
    FwdSim m;
    m.on(0, 60);  // window 60..120 s
    m.in.busy = FB_HTTP;
    for (int k = 0; k < 600; k++) {
      CHECK(m.tick().slot < 0);
      m.advance(1000);
    }
    CHECK(m.s.s[0].n.skip >= 9 && m.s.s[0].n.skip <= 10 && m.s.s[0].n.nodata == 0);
    CHECK(strcmp(fwdBusyName(m.s.s[0].last_why), "http") == 0);
    m.in.busy = 0;
    int sends = 0;
    for (int k = 0; k < 60; k++) {
      FwdStart st = m.tick();
      if (st.slot >= 0) {
        sends++;
        m.s.result(st.slot, FwdResult::Ok, m.in.now_ms, 0, 1);
      }
      m.advance(1000);
    }
    CHECK(sends == 1);
  }
  {  // three targets, one connection: never two Starts while running; round robin; each within one send of its window
    FwdSim m;
    m.on(0, 300);
    m.on(1, 300, true, FwdType::WuCompat);  // floor 60: its window opens now (a 300-s floor waits)
    m.on(2, 300, false, FwdType::WowBe);
    int order[3] = {-1, -1, -1}, k = 0;
    uint32_t waited[3] = {0, 0, 0};
    const uint32_t open = m.in.now_ms;
    for (int t = 0; t < 40; t++) {
      FwdStart st = m.tick();
      if (st.slot >= 0) {
        CHECK(k < 3);
        if (k < 3) order[k++] = st.slot;
        waited[st.slot] = m.in.now_ms - open;
        m.advance(8000);  // a send takes 8 s
        CHECK(m.tick().slot < 0);
        m.s.result(st.slot, FwdResult::Ok, m.in.now_ms, 0, 1);
      } else {
        m.advance(1000);
      }
    }
    CHECK(order[0] == 0 && order[1] == 1 && order[2] == 2);
    CHECK(waited[1] <= 8000 && waited[2] <= 16000);
  }
  {  // retry 30 s -> 120 s -> drop (a third fail)
    FwdSim m;
    m.on(0, 600);
    FwdStart st = m.tick();
    m.s.result(0, FwdResult::Fail, m.in.now_ms, 0, 1);
    CHECK(m.s.s[0].st == FwdSt::Retry && m.s.s[0].retry_ms == m.in.now_ms + 30000);
    m.advance(29000);
    CHECK(m.tick().slot < 0);
    m.advance(1000);
    st = m.tick();
    CHECK(st.slot == 0 && st.kind == FwdKind::Retry);
    m.s.result(0, FwdResult::Fail, m.in.now_ms, 0, 1);
    CHECK(m.s.s[0].retry_ms == m.in.now_ms + 120000);
    m.advance(120000);
    st = m.tick();
    CHECK(st.kind == FwdKind::Retry);
    m.s.result(0, FwdResult::Fail, m.in.now_ms, 0, 1);
    CHECK(m.s.s[0].n.drop == 1 && m.s.s[0].n.fail == 3 && m.s.s[0].st == FwdSt::Idle);
  }
  {  // a retry crossing the next due -> drop, the due unchanged
    FwdSim m;
    m.on(0, 60);
    m.tick();
    const uint32_t due = m.s.s[0].due_ms;
    m.advance(40000);
    m.s.result(0, FwdResult::Fail, m.in.now_ms, 0, 1);  // +30 s = 70 s > 60 s
    CHECK(m.s.s[0].n.drop == 1 && m.s.s[0].st == FwdSt::Idle && m.s.s[0].due_ms == due);
    // a retry that waits for busy until the due: dropped then
    FwdSim b;
    b.on(0, 300);
    b.tick();
    b.s.result(0, FwdResult::Fail, b.in.now_ms, 0, 1);
    b.in.busy = FB_STORE;
    for (int k = 0; k < 310; k++) b.tick(), b.advance(1000);
    CHECK(b.s.s[0].n.drop == 1);
  }
  {  // 429 with retry_after; a reject keeps the due
    FwdSim m;
    m.on(0, 300, true, FwdType::Windy);
    CHECK(m.tick().slot < 0);  // the first tick opens the window: now + the 300-s floor
    m.advance(300000);
    CHECK(m.tick().slot == 0);
    m.s.result(0, FwdResult::Limited, m.in.now_ms, 900, 1);
    CHECK(m.s.s[0].n.limited == 1 && m.s.s[0].due_ms == m.in.now_ms + 900000);
    FwdSim r;
    r.on(0, 300);
    r.tick();
    const uint32_t due = r.s.s[0].due_ms;
    r.s.result(0, FwdResult::Reject, r.in.now_ms, 0, 1);
    CHECK(r.s.s[0].n.reject == 1 && r.s.s[0].due_ms == due);
  }
  {  // the first window of a 300-s-floor service (else Windy answers 429 after a restart and after a save)
    for (FwdType t : {FwdType::Windy, FwdType::PwsWeather}) {
      FwdSim m;  // after a boot nothing is known: now + floor
      m.on(0, 310, true, t);
      const uint32_t t0 = m.in.now_ms;
      CHECK(m.tick().slot < 0 && m.s.s[0].st == FwdSt::Idle && m.s.s[0].due_ms == t0 + 300000u);
      CHECK(m.s.nextS(0, m.in.now_ms) == 300);
      m.advance(299000);
      CHECK(m.tick().slot < 0);
      m.advance(1000);
      FwdStart st = m.tick();
      CHECK(st.slot == 0 && st.kind == FwdKind::Send);
      const uint32_t sent = m.in.now_ms;
      m.advance(2000);
      m.s.result(0, FwdResult::Ok, m.in.now_ms, 0, 1);
      // a save 60 s after the send: the window opens at the send + floor, not now
      m.advance(58000);
      m.s.reset(0);
      CHECK(m.tick().slot < 0 && m.s.s[0].due_ms == sent + 300000u);
      m.advance(239000);
      CHECK(m.tick().slot < 0);
      m.advance(1000);
      CHECK(m.tick().slot == 0);
      m.s.result(0, FwdResult::Ok, m.in.now_ms, 0, 2);
      // a save after the floor has passed: now
      m.advance(400000);
      m.s.reset(0);
      CHECK(m.tick().slot == 0);
      m.s.result(0, FwdResult::Ok, m.in.now_ms, 0, 3);
      CHECK(m.s.s[0].n.ok == 3 && m.s.s[0].n.limited == 0);
    }
    for (FwdType t : {FwdType::Wu, FwdType::WowBe, FwdType::WuCompat}) {  // a 60-s floor: a window opens now
      FwdSim m;
      m.on(0, 300, true, t);
      CHECK(m.tick().slot == 0);
    }
    FwdSim w;  // switched off and on again: the same rule (off -> on goes through Off)
    w.on(0, 310, true, FwdType::Windy);
    CHECK(w.tick().slot < 0);
    w.advance(300000);
    CHECK(w.tick().slot == 0);
    w.s.result(0, FwdResult::Ok, w.in.now_ms, 0, 1);
    const uint32_t sent = w.in.now_ms;
    w.in.s[0].on = false;
    w.advance(1000);
    w.tick();
    w.in.s[0].on = true;
    w.advance(1000);
    CHECK(w.tick().slot < 0 && w.s.s[0].due_ms == sent + 300000u);
  }
  {  // 401 x3 -> paused; a key change (reset) resumes
    FwdSim m;
    m.on(0, 60);
    for (int k = 0; k < 3; k++) {
      while (m.tick().slot < 0) m.advance(1000);
      m.s.result(0, FwdResult::Refused, m.in.now_ms, 0, 1);
    }
    CHECK(m.s.s[0].paused && m.s.s[0].pause_why == FP_KEY && m.s.s[0].n.refused == 3 && m.s.nextS(0, m.in.now_ms) == -1);
    for (int k = 0; k < 300; k++) {
      CHECK(m.tick().slot < 0);
      m.advance(1000);
    }
    m.s.reset(0);
    CHECK(m.tick().slot == 0);
  }
  {  // the data gate -> nodata, not skip; the same last_seen counts as nodata too
    FwdSim m;
    m.on(0, 60);
    m.in.s[0].gate = FwdGate::Offline;
    for (int k = 0; k < 130; k++) {
      CHECK(m.tick().slot < 0);
      m.advance(1000);
    }
    CHECK(m.s.s[0].n.nodata == 2 && m.s.s[0].n.skip == 0);
    for (FwdGate g : {FwdGate::Stale, FwdGate::Restored, FwdGate::Same, FwdGate::NoTime}) {
      FwdSim x;
      x.on(0, 60);
      x.in.s[0].gate = g;
      for (int k = 0; k < 125; k++) x.tick(), x.advance(1000);
      CHECK(x.s.s[0].n.nodata == 2);
    }
  }
  {  // resource floors per transport; heap windows counted as heap
    FwdSim m;
    m.on(0, 60, true);
    m.on(1, 60, false, FwdType::WuCompat);
    m.in.free = 25000;  // under the TLS gate (17 + 12 KB), over the plain floor (16 KB)
    FwdStart st = m.tick();
    CHECK(st.slot == 1);
    m.s.result(1, FwdResult::Ok, m.in.now_ms, 0, 1);
    for (int k = 0; k < 125; k++) m.tick(), m.advance(1000);
    CHECK(m.s.s[0].n.heap >= 1 && m.s.s[0].n.ok == 0);
    // the floors are on the byte heap (MALLOC_CAP_8BIT); the TLS gate per class is in test_tlsc.inc
    const FwdTlsClass C = FwdTlsClass::Custom;
    CHECK(fwdResourcesOk(false, C, 16 * 1024, 8 * 1024) && !fwdResourcesOk(false, C, 16 * 1024 - 1, 8 * 1024) &&
          !fwdResourcesOk(false, C, 16 * 1024, 8 * 1024 - 1));
    CHECK(fwdResourcesOk(false, C, 50240, 45044));  // the WROOM-32's byte heap at rest carries plain HTTP
  }
  {  // abort on heap, OTA, reboot, the 10-s deadline
    CHECK(!fwdAbortWhy(30000, 20000, false, false, 9999));
    CHECK(strcmp(fwdAbortWhy(8 * 1024 - 1, 20000, false, false, 0), "heap") == 0 && !fwdAbortWhy(8 * 1024, 20000, false, false, 0));
    CHECK(strcmp(fwdAbortWhy(30000, 8 * 1024 - 1, false, false, 0), "largest") == 0);
    CHECK(strcmp(fwdAbortWhy(90000, 50000, true, false, 0), "ota") == 0);
    CHECK(strcmp(fwdAbortWhy(90000, 50000, false, true, 0), "reboot") == 0);
    CHECK(strcmp(fwdAbortWhy(90000, 50000, false, false, 10000), "deadline") == 0);
  }
  {  // millis() wrap: a window across 2^32
    FwdSim m;
    m.in.now_ms = 0xFFFFFFFFu - 5000;
    m.on(0, 60);
    FwdStart st = m.tick();
    CHECK(st.slot == 0);
    m.s.result(0, FwdResult::Ok, m.in.now_ms, 0, 1);
    m.advance(59000);
    CHECK(m.tick().slot < 0);
    m.advance(1000);
    CHECK(m.in.now_ms < 60000 && m.tick().slot == 0);
  }
  {  // window clamps: max_s; the window closes at max_s - min_s
    FwdTarget t = fwdDefaultTarget(FwdType::Wu);
    t.min_s = 60;
    CHECK(fwdMaxS(t) == 120);
    t.min_s = 300;
    CHECK(fwdMaxS(t) == 420);
    t.max_s = 330;
    CHECK(fwdMaxS(t) == 330);
    FwdSim m;
    m.on(0, 300);
    m.in.s[0].max_s = 330;
    m.in.busy = FB_MQTT;
    for (int k = 0; k <= 31; k++) m.tick(), m.advance(1000);
    CHECK(m.s.s[0].n.skip == 1);
  }
  {  // no_memory is a heap skip (no retry inside the window); three in a row pause with their own reason
    FwdSim m;
    m.on(0, 300);
    for (int k = 0; k < 3; k++) {
      int guard = 0;
      while (m.tick().slot < 0 && ++guard < 1000) m.advance(1000);
      m.s.result(0, FwdResult::NoMemory, m.in.now_ms, 0, 1);
      CHECK(m.s.s[0].st == FwdSt::Idle && m.s.s[0].fails == 0);  // never a retry
    }
    CHECK(m.s.s[0].paused && m.s.s[0].pause_why == FP_MEMORY && m.s.s[0].n.heap == 3 && m.s.s[0].n.fail == 0 &&
          m.s.nextS(0, m.in.now_ms) == -1);
    m.s.reset(0);
    CHECK(!m.s.s[0].paused && m.s.s[0].pause_why == FP_NONE && m.s.s[0].nomem_run == 0);
    // an accepted send in between starts the count again
    FwdSim n;
    n.on(0, 300);
    const FwdResult seq[] = {FwdResult::NoMemory, FwdResult::NoMemory, FwdResult::Ok, FwdResult::NoMemory, FwdResult::NoMemory};
    for (FwdResult r : seq) {
      int guard = 0;
      while (n.tick().slot < 0 && ++guard < 1000) n.advance(1000);
      n.s.result(0, r, n.in.now_ms, 0, 1);
    }
    CHECK(!n.s.s[0].paused && n.s.s[0].nomem_run == 2 && n.s.s[0].n.heap == 4 && n.s.s[0].n.ok == 1);
  }
  {  // "Send one now": the service minimum since the last send; busy only for its guards; not counted
    FwdSim m;
    m.on(0, 300, true, FwdType::Windy);
    CHECK(m.tick().slot < 0);  // the first tick opens the window: now + the 300-s floor
    m.advance(300000);
    CHECK(m.tick().slot == 0);
    m.s.result(0, FwdResult::Ok, m.in.now_ms, 0, 7);
    m.advance(10000);
    CHECK(m.s.requestTest(0, m.in.now_ms, 300, true) == FwdTestReq::TooSoon);
    m.advance(290000);
    m.in.busy = FB_HTTP | FB_STORE;  // not the test's guards
    CHECK(m.s.requestTest(0, m.in.now_ms, 300, true) == FwdTestReq::Accepted);
    CHECK(m.s.requestTest(0, m.in.now_ms, 300, true) == FwdTestReq::Busy);
    FwdStart st = m.tick();
    CHECK(st.slot == 0 && st.kind == FwdKind::Test);
    m.s.result(0, FwdResult::Ok, m.in.now_ms, 0, 8);
    CHECK(m.s.s[0].n.ok == 1 && m.s.s[0].ok_seen == 8);
    // another target's send on the line: the test is accepted and starts after that send's result; this
    // target's own send: busy
    FwdSim o;
    o.on(0, 60);
    o.on(1, 60);
    o.in.s[1].on = false;  // slot 1 sends nothing on its own: only the test
    int og = 0;
    FwdStart os = o.tick();
    while (os.slot < 0 && ++og < 1000) o.advance(1000), os = o.tick();
    CHECK(os.slot == 0 && os.kind == FwdKind::Send);
    CHECK(o.s.requestTest(0, o.in.now_ms, 0, true) == FwdTestReq::Busy);
    CHECK(o.s.requestTest(1, o.in.now_ms, 0, true) == FwdTestReq::Accepted);
    o.advance(3000);
    CHECK(o.tick().slot < 0);  // the line is still taken
    o.s.result(0, FwdResult::Ok, o.in.now_ms, 0, 1);
    FwdStart ot = o.tick();
    CHECK(ot.slot == 1 && ot.kind == FwdKind::Test);
    // the upload guard holds a test; after 20 s it gives up as busy
    FwdSim b;
    b.on(0, 60);
    b.in.busy = FB_UPLOAD;
    CHECK(b.s.requestTest(0, b.in.now_ms, 60, true) == FwdTestReq::Accepted);
    int slot = -1;
    for (int k = 0; k < 22; k++) b.tick(), b.advance(1000);
    CHECK(b.s.takeTestOutcome(&slot) == FwdTestOutcome::Busy && slot == 0);
    // no fresh reading
    FwdSim d;
    d.on(0, 60);
    d.in.s[0].gate_test = FwdGate::Offline;
    d.in.busy = FB_HTTP;
    d.s.requestTest(0, d.in.now_ms, 60, true);
    d.tick();
    CHECK(d.s.takeTestOutcome(&slot) == FwdTestOutcome::NoData);
    CHECK(d.s.requestTest(0, d.in.now_ms, 60, false) == FwdTestReq::Off);
    // a test on a disabled target: no window opens, nothing counted
    FwdSim z;
    z.on(0, 60);
    z.in.s[0].on = false;
    z.tick();
    CHECK(z.s.requestTest(0, z.in.now_ms, 60, true) == FwdTestReq::Accepted);
    FwdStart zt = z.tick();
    CHECK(zt.slot == 0 && zt.kind == FwdKind::Test);
    z.s.result(0, FwdResult::Ok, z.in.now_ms, 0, 3);
    for (int k = 0; k < 200; k++) z.tick(), z.advance(1000);
    CHECK(z.s.s[0].st == FwdSt::Off && z.s.s[0].n.ok == 0 && z.s.s[0].n.nodata == 0 && z.s.s[0].n.skip == 0);
    // a test resumes a paused target
    FwdSim p;
    p.on(0, 60);
    for (int k = 0; k < 3; k++) {
      while (p.tick().slot < 0) p.advance(1000);
      p.s.result(0, FwdResult::Refused, p.in.now_ms, 0, 1);
    }
    p.advance(60000);
    CHECK(p.s.s[0].paused && p.s.requestTest(0, p.in.now_ms, 60, true) == FwdTestReq::Accepted);
    CHECK(p.tick().kind == FwdKind::Test);
    p.s.result(0, FwdResult::Ok, p.in.now_ms, 0, 2);
    CHECK(!p.s.s[0].paused);
  }
  {  // the close guard: 45 s before the boundary until store / learners are clear (>= 10 s after)
    FwdHourGuard g;
    const uint32_t h = 1800000000u - 1800000000u % 3600;  // a UTC hour boundary
    CHECK(!g.tick(h - 46, 0, false, false));
    CHECK(g.tick(h - 45, 0, false, false));
    CHECK(g.tick(h + 3, 0, false, false));
    CHECK(g.tick(h + 12, 0, true, false));
    CHECK(g.tick(h + 13, 0, false, true));
    CHECK(!g.tick(h + 14, 0, false, false));
    CHECK(!g.tick(h + 15, 0, true, false));  // a later store write is the store bit's business
    // a +5:30 zone (India). The hour closes on the UTC boundary, the day at local midnight (UTC hh:30)
    const int32_t in = 5 * 3600 + 1800;
    const uint32_t day = 1800000000u - (1800000000u + in) % 86400 + 86400;  // the next local midnight (unix)
    CHECK(day % 3600 == 1800);
    FwdHourGuard gi;
    CHECK(gi.tick(h - 30, in, false, false));    // the UTC hour close is guarded in a :30 zone
    CHECK(!gi.tick(h + 20, in, false, false));
    CHECK(!gi.tick(h + 1800 - 46, in, false, false));  // local :00 that is not midnight: not guarded
    CHECK(gi.tick(day - 40, in, false, false) && gi.boundary == day);  // local midnight: guarded
    CHECK(gi.tick(day + 5, in, false, false) && !gi.tick(day + 11, in, false, false));
    CHECK(FwdHourGuard::near(day - 45, in) && !FwdHourGuard::near(day - 46, in) && FwdHourGuard::near(h - 1, in));
    FwdSim m;
    m.on(0, 60);
    m.in.busy = FB_HOUR;
    CHECK(m.tick().slot < 0 && strcmp(fwdBusyName(m.s.s[0].last_why), "hour") == 0);
  }
  {  // a send across several samples never lowers the 10-minute window below the heap before it started
    FwdHeapFeed f = fwdHeapFeed(false, false, 70000, 90000, 0);
    CHECK(f.window == 70000 && !f.to_fwd);
    uint32_t win = UINT32_MAX, fwdMin = UINT32_MAX;
    struct S { bool running, ran; uint32_t low, free; } seq[] = {
        {false, false, 88000, 90000},  // before
        {true, true, 35000, 37000},    // inside the TLS dip, three samples
        {true, true, 34000, 36000},
        {true, true, 36000, 38000},
        {false, true, 35500, 89000},   // the first sample after the send: its local minimum still holds the dip
        {false, false, 88500, 89500},  // normal again
    };
    for (const S& x : seq) {
      f = fwdHeapFeed(x.running, x.ran, x.low, x.free, 90000);
      if (f.window < win) win = f.window;
      if (f.to_fwd && x.low < fwdMin) fwdMin = x.low;
    }
    CHECK(win == 88000 && win >= 60000 && fwdMin == 34000);
    CHECK(fwdHeapFeed(true, true, 30000, 31000, 0).window == 31000);  // no pre-send value: the instant's free heap
  }
  {  // a waiting test cleared by a switch-off / remove ends with an outcome
    FwdSim m;
    m.on(0, 60);
    m.in.busy = FB_UPLOAD;
    CHECK(m.s.requestTest(0, m.in.now_ms, 60, true) == FwdTestReq::Accepted);
    m.tick();
    m.in.s[0].on = m.in.s[0].test_ok = false;
    m.tick();
    int slot = -1;
    CHECK(m.s.takeTestOutcome(&slot) == FwdTestOutcome::Off && slot == 0 && !m.s.s[0].test_pending);
  }
  {  // a result of the old settings (reset while it ran) is dropped; the new settings get a fresh window
    FwdSim m;
    m.on(0, 60);
    CHECK(m.tick().slot == 0);
    m.s.reset(0);
    m.s.result(0, FwdResult::Refused, m.in.now_ms, 0, 1);
    CHECK(m.s.s[0].n.refused == 0 && m.s.s[0].refused_run == 0 && !m.s.running());
    CHECK(m.tick().slot == 0);
  }
  {  // a tick stall that passes several windows counts each
    FwdSim m;
    m.on(0, 60);
    m.in.busy = FB_HTTP;
    m.tick();
    m.advance(300000);  // 5 min without a tick
    m.tick();
    CHECK(m.s.s[0].n.skip == 4);
  }
  {  // switched off while idle: no start; back on: a window now
    FwdSim m;
    m.on(0, 60);
    m.in.s[0].on = false;
    CHECK(m.tick().slot < 0 && m.s.s[0].st == FwdSt::Off && m.s.nextS(0, m.in.now_ms) == -1);
    m.in.s[0].on = true;
    CHECK(m.tick().slot == 0);
  }
}

static void testFwdCauses() {  // the per-target causes (the TLS verdict is tlscClassify, test_tlsc.inc)
  CHECK(strcmp(fwdResultName(FwdResult::NoMemory), "no_memory") == 0);
  // causes (scheduled attempts only; the app skips tests)
  CHECK(fwdCauseOf(FwdResult::Ok, 200, nullptr) == FwdCause::None && fwdCauseOf(FwdResult::Dup, 200, nullptr) == FwdCause::None);
  CHECK(fwdCauseOf(FwdResult::Refused, 401, nullptr) == FwdCause::None && fwdCauseOf(FwdResult::Reject, 400, nullptr) == FwdCause::None);
  CHECK(fwdCauseOf(FwdResult::Limited, 429, nullptr) == FwdCause::Limited);
  CHECK(fwdCauseOf(FwdResult::Fail, 503, nullptr) == FwdCause::Http5xx);
  CHECK(fwdCauseOf(FwdResult::Fail, -1, "dns") == FwdCause::Dns);
  CHECK(fwdCauseOf(FwdResult::Fail, -1, "connect") == FwdCause::Connect && fwdCauseOf(FwdResult::Fail, -1, "handshake") == FwdCause::Connect &&
        fwdCauseOf(FwdResult::Fail, -1, "write") == FwdCause::Connect);
  CHECK(fwdCauseOf(FwdResult::Fail, -1, "read") == FwdCause::Read && fwdCauseOf(FwdResult::Fail, -1, "deadline") == FwdCause::Read &&
        fwdCauseOf(FwdResult::Fail, -1, nullptr) == FwdCause::Read);
  CHECK(fwdCauseOf(FwdResult::NoMemory, -1, "no_memory") == FwdCause::Other &&
        fwdCauseOf(FwdResult::Fail, -1, "aborted_heap") == FwdCause::Other);
  const char* names[] = {"", "dns", "connect", "read", "5xx", "429", "other"};
  for (int c = 0; c < (int)FwdCause::COUNT; c++) CHECK(strcmp(fwdCauseName((FwdCause)c), names[c]) == 0);
}

static void testFwdConfig() {
  // a new target needs a type; defaults; the key never in the JSON; partial PUTs
  FwdTarget t;
  memset(&t, 0, sizeof t);
  Errors e;
  FwdChange ch;
  const char* j1 = "{\"station\":\"ST1\"}";
  CHECK(fwdFromJson(j1, strlen(j1), &t, &e, &ch) && e.n == 1 && !t.used);
  e = Errors();
  const char* j2 = "{\"type\":\"windy\",\"station\":\"ST1\",\"id\":\"EXAMPLE1\",\"key\":\"examplekey\",\"enabled\":true}";
  CHECK(fwdFromJson(j2, strlen(j2), &t, &e, &ch) && e.n == 0 && t.used && t.type == FwdType::Windy);
  CHECK(t.min_s == 310 && fwdMaxS(t) == 430 && t.https == (FWD_TLS && FWD_HTTPS_DEFAULT) && t.enabled && ch.key_given &&
        strcmp(ch.key, "examplekey") == 0);
  std::string js;
  char ob[128];
  Out o2(toString, &js, ob, sizeof ob);
  fwdToJson(o2, t);
  o2.flush();
  // https always; a new target's value is the build's default (true in the TLS release)
  CHECK(js == std::string("{\"type\":\"windy\",\"station\":\"ST1\",\"id\":\"EXAMPLE1\",\"https\":") +
                  (FWD_TLS && FWD_HTTPS_DEFAULT ? "true" : "false") + ",\"enabled\":true}");
  CHECK(js.find("examplekey") == std::string::npos);
  // the NVS round trip
  FwdTarget back;
  memset(&back, 0, sizeof back);
  e = Errors();
  CHECK(fwdFromJson(js.c_str(), js.size(), &back, &e, &ch) && e.n == 0 && memcmp(&back, &t, sizeof t) == 0 && !ch.key_given);
  // "<set>" and "" leave the key alone; null clears it; the fixed interval passes quietly
  const char* j3 = "{\"key\":\"<set>\",\"min_s\":310}";
  e = Errors();
  CHECK(fwdFromJson(j3, strlen(j3), &t, &e, &ch) && e.n == 0 && !ch.key_given && !ch.key_clear && t.min_s == 310 &&
        fwdMaxS(t) == 430);
  const char* j4 = "{\"key\":null}";
  CHECK(fwdFromJson(j4, strlen(j4), &t, &e, &ch) && ch.key_clear);
  // another interval or window is noted and replaced; null and max_s 0 pass
  const char* j5 = "{\"min_s\":600,\"max_s\":700}";
  e = Errors();
  CHECK(fwdFromJson(j5, strlen(j5), &t, &e, &ch) && e.n == 2 && t.min_s == 310 && t.max_s == 0 && fwdMaxS(t) == 430);
  // the longest fixed text arrives whole (an error is stored whole or counted as dropped, never cut)
  CHECK(strcmp(e.at(0), "min_s: fixed by fauxcloud: every 5 minutes, or a service's own minimum + 10 s") == 0);
  const char* j6 = "{\"min_s\":null,\"max_s\":0}";
  e = Errors();
  CHECK(fwdFromJson(j6, strlen(j6), &t, &e, &ch) && e.n == 0 && t.min_s == 310);
  // an older record (WU at 60 s) loads at the fixed interval; the change shows (the next save writes it)
  FwdTarget old;
  memset(&old, 0, sizeof old);
  const char* j7 = "{\"type\":\"wu\",\"min_s\":60,\"max_s\":120,\"https\":false,\"enabled\":true}";
  e = Errors();
  CHECK(fwdFromJson(j7, strlen(j7), &old, &e, &ch) && e.n == 2 && old.min_s == 300 && old.max_s == 0 &&
        fwdMaxS(old) == 420 && !old.https && old.enabled);
  CHECK(fwdIntervalS(FwdType::Wu) == 300 && fwdIntervalS(FwdType::WowBe) == 300 && fwdIntervalS(FwdType::Windy) == 310 &&
        fwdIntervalS(FwdType::PwsWeather) == 310 && fwdIntervalS(FwdType::WuCompat) == 300 &&
        fwdIntervalS(FwdType::Template) == 300);
  // HTTPS per variant: the default, a PUT asking for it, an old record that had it
  FwdTarget hs = fwdDefaultTarget(FwdType::Wu);
  CHECK(hs.https == (FWD_TLS && FWD_HTTPS_DEFAULT));  // the build's default (HTTPS on the release)
  const char* jh = "{\"https\":true}";
  e = Errors();
  CHECK(fwdFromJson(jh, strlen(jh), &hs, &e, &ch) && hs.https == (FWD_TLS != 0) && e.n == (FWD_TLS ? 0 : 1));
  {  // a type change without "https" keeps the target's choice; a template takes its URL's scheme
    FwdTarget tc = fwdDefaultTarget(FwdType::Windy);
    tc.used = true;
    tc.https = false;
    const char* jt = "{\"type\":\"wu_compat\",\"host\":\"example.org\"}";
    e = Errors();
    CHECK(fwdFromJson(jt, strlen(jt), &tc, &e, &ch) && tc.type == FwdType::WuCompat && !tc.https);
    tc.https = FWD_TLS != 0;
    const char* jw = "{\"type\":\"windy\"}";
    e = Errors();
    CHECK(fwdFromJson(jw, strlen(jw), &tc, &e, &ch) && tc.type == FwdType::Windy && tc.https == (FWD_TLS != 0));
  }
  memset(&old, 0, sizeof old);
  const char* jo = "{\"type\":\"windy\",\"https\":true}";
  e = Errors();
  CHECK(fwdFromJson(jo, strlen(jo), &old, &e, &ch) && old.https == (FWD_TLS != 0));
  {  // "https" is always stored; a record from before that keeps meaning HTTPS, never the build's default
    FwdTarget st;
    Errors le;
    const char* older = "{\"type\":\"wu\",\"id\":\"EXAMPLE1\",\"enabled\":true}";  // an older TLS build, HTTPS on: key left out
    CHECK(fwdLoadStored(older, strlen(older), &st, &le) && st.used && st.https == (FWD_TLS != 0));
    const char* http = "{\"type\":\"wu\",\"id\":\"EXAMPLE1\",\"https\":false,\"enabled\":true}";
    CHECK(fwdLoadStored(http, strlen(http), &st, &le) && !st.https);  // an HTTP preset never flips
    std::string w;
    char wb[64];
    Out ow(toString, &w, wb, sizeof wb);
    fwdToJson(ow, st);
    ow.flush();
    CHECK(w.find("\"https\":false") != std::string::npos);
    const char* tpl = "{\"type\":\"template\",\"url\":\"http://example.org/a?t={tempf}\"}";
    CHECK(fwdLoadStored(tpl, strlen(tpl), &st, &le) && !st.https);  // a template's scheme decides
  }
  // the key follows a target only while type / host / path / url stay
  FwdTarget a = fwdDefaultTarget(FwdType::WuCompat);
  strcpy(a.wc.host, "example.org");
  FwdTarget b = a;
  CHECK(fwdKeyFollows(a, b));
  const char* j9 = "{\"host\":\"example.net\"}";
  e = Errors();
  CHECK(fwdFromJson(j9, strlen(j9), &b, &e, &ch) && e.n == 0 && !fwdKeyFollows(a, b) && ch.any);
  const char* j10 = "{\"type\":\"wu\"}";
  b = a;
  CHECK(fwdFromJson(j10, strlen(j10), &b, &e, &ch) && b.type == FwdType::Wu && !b.wc.host[0] && !fwdKeyFollows(a, b));
  // field rules per type
  const char* j11 = "{\"host\":\"exa mple\",\"path\":\"nopath\",\"url\":\"https://x.org/\",\"success\":\"ok\"}";
  e = Errors();
  b = a;
  CHECK(fwdFromJson(j11, strlen(j11), &b, &e, &ch) && e.n == 3 && strcmp(b.wc.host, "example.org") == 0);
  FwdTarget tp = fwdDefaultTarget(FwdType::Template);
  const char* j12 = "{\"url\":\"http://example.org/a?t={tempf}\",\"success\":\"OK\",\"https\":true}";
  e = Errors();
  CHECK(fwdFromJson(j12, strlen(j12), &tp, &e, &ch) && e.n == 1 && !tp.https && strcmp(tp.success, "OK") == 0);
  const char* j13 = "{\"url\":\"https://example.org/a?t={wc_temp}\"}";
  e = Errors();
  CHECK(fwdFromJson(j13, strlen(j13), &tp, &e, &ch) && e.n == 1 && !tp.https);
  const char* j14 = "{\"remove\":true}";
  CHECK(fwdFromJson(j14, strlen(j14), &tp, &e, &ch) && ch.remove);
  // the NVS form stays under 600 B for the largest target
  FwdTarget big = fwdDefaultTarget(FwdType::Template);
  std::string url = "https://example.org/a?x=";
  while (url.size() < FWD_URL_MAX) url += "a";
  snprintf(big.url, sizeof big.url, "%s", url.c_str());
  memset(big.id, 'x', FWD_ID_MAX);
  memset(big.success, 'x', FWD_SUCCESS_MAX);
  strcpy(big.station, "STATION_ID_1234");
  big.min_s = 3600, big.max_s = 7000, big.enabled = true;
  big.verified = true;  // the verified key counts in the worst case
  js.clear();
  Out o3(toString, &js, ob, sizeof ob);
  fwdToJson(o3, big);
  o3.flush();
  printf("  info: fwd target NVS JSON worst case = %u bytes (budget %u)\n", (unsigned)js.size(), (unsigned)FWD_NVS_JSON_MAX);
  CHECK(js.size() <= FWD_NVS_JSON_MAX);
  FwdTarget q = fwdDefaultTarget(FwdType::Wu);
  const char* jq = "{\"id\":\"a\\\"b\",\"success\":\"x\"}";  // a quote in the id; success is fixed for presets
  e = Errors();
  CHECK(fwdFromJson(jq, strlen(jq), &q, &e, &ch) && e.n == 2 && !q.id[0]);
  // the enable plan with 0/1/2 stations
  const char* ids[2] = {"ST1", "ST2"};
  FwdEnablePlan p = fwdEnablePlan(ids, 0, nullptr);
  CHECK(p.ok && p.forget == -1);
  p = fwdEnablePlan(ids, 1, nullptr);
  CHECK(p.ok && p.forget == -1);
  p = fwdEnablePlan(ids, 2, nullptr);
  CHECK(!p.ok && p.why);
  p = fwdEnablePlan(ids, 2, "ST9");
  CHECK(!p.ok);
  p = fwdEnablePlan(ids, 2, "ST2");
  CHECK(p.ok && p.forget == 0);
  p = fwdEnablePlan(ids, 2, "ST1");
  CHECK(p.ok && p.forget == 1);
  // the bridge rule: on => max_stations 1 + lan off; either back refused while on; refused with 2 stations in use
  BridgeConfig bc = defaultBridgeConfig();
  CHECK(!bc.forwarding);
  bc.lan_uploads = true;
  e = Errors();
  const char* b1 = "{\"forwarding\":true}";
  BridgeConfig x = bc;
#if FC_MAX_STATIONS > 1
  bridgeFromJson(b1, strlen(b1), &x, &e, 2);  // two stations in use: keep one first (never at one station)
  CHECK(!x.forwarding && e.n == 1 && x.lan_uploads && x.max_stations == 2);
  e = Errors();
#endif
  x = bc;
  bridgeFromJson(b1, strlen(b1), &x, &e, 1);
  CHECK(x.forwarding && e.n == 0 && !x.lan_uploads && x.max_stations == 1);
  e = Errors();
  const char* b2 = "{\"lan_uploads\":true,\"max_stations\":2}";
  bridgeFromJson(b2, strlen(b2), &x, &e, 1);
  CHECK(x.forwarding && e.n == 2 && !x.lan_uploads && x.max_stations == 1);
  CHECK(strstr(e.at(0), "off while forwarding is on") || strstr(e.at(1), "off while forwarding is on"));
#if FC_MAX_STATIONS == 1
  CHECK(strstr(e.at(0), "fixed at 1 on this board"));  // the profile's answer comes first
#endif
  e = Errors();
  const char* b3 = "{\"forwarding\":false,\"lan_uploads\":true,\"max_stations\":2}";
  bridgeFromJson(b3, strlen(b3), &x, &e, 1);
#if FC_MAX_STATIONS > 1
  CHECK(!x.forwarding && e.n == 0 && x.lan_uploads && x.max_stations == 2);
#else
  CHECK(!x.forwarding && e.n == 1 && x.lan_uploads && x.max_stations == 1);  // fixed at 1
#endif
  const char* b4 = "{\"forwarding\":null}";
  x.forwarding = true;
  bridgeFromJson(b4, strlen(b4), &x, &e, 1);
  CHECK(!x.forwarding);
  // the stored form round trip, and the 384-B saveBridge buffer with every key at its longest
  BridgeConfig w = defaultBridgeConfig();
  w.forwarding = true, w.max_stations = 1;
  memset(w.tz, 'A', sizeof w.tz - 1);
  w.tz[sizeof w.tz - 1] = 0;
  memset(w.tz_name, 'a', sizeof w.tz_name - 1);
  w.tz_name[sizeof w.tz_name - 1] = 0;
  w.lat = -89.1234f, w.lon = -179.1234f, w.auto_publish_new_stations = false, w.lan_uploads = false;
  w.mqtt_interval_last_seen_s = 3600;
  js.clear();
  Out o4(toString, &js, ob, sizeof ob);
  bridgeToJson(o4, w);
  o4.flush();
  printf("  info: bridge NVS JSON worst case with forwarding = %u bytes (saveBridge buffer 384)\n", (unsigned)js.size());
  CHECK(js.size() < 384 && js.find("\"forwarding\":true") != std::string::npos);
  BridgeConfig rt = defaultBridgeConfig();
  const char* sj = "{\"forwarding\":true,\"max_stations\":1}";
  e = Errors();
  bridgeFromJson(sj, strlen(sj), &rt, &e, 0);
  CHECK(rt.forwarding && rt.max_stations == 1 && e.n == 0);
}

static void testFwdFakeEndpoint() {
  int n = 0;
  for (const FwdVecResp& c : FWD_VEC_RESP) {
    FwdType ty;
    CHECK(fwdTypeFromName(c.type, &ty));
    for (int split = 0; split < 2; split++) {
      static FwdResp p;
      p.begin(ty, c.success);
      bool done = false;
      if (split) {
        for (size_t i = 0; i < c.len && !done; i++) done = p.feed(c.bytes + i, 1);
      } else {
        done = p.feed(c.bytes, c.len);
      }
      if (!done) p.end();
      const bool ok = strcmp(fwdResultName(p.result()), c.result) == 0 && p.status() == c.status && p.retryAfter() == c.retry;
      CHECK(ok);
      if (!ok) printf("  fake endpoint %s (%s): got %s %d %lu\n", c.name, split ? "1 B reads" : "whole",
                      fwdResultName(p.result()), p.status(), (unsigned long)p.retryAfter());
    }
    n++;
  }
  printf("  info: fwd fake-endpoint answers %d (whole + 1-byte reads)\n", n);
}

#include "test_tlsc.inc"  // BearSSL client vs an in-process server (uses FwdSim above)

// a target is switched on only after an accepted "Send one now" with the current settings
static void testFwdVerified() {
  auto put = [](const FwdTarget& before, const char* json, FwdTarget* t, FwdChange* ch, Errors* e) {
    *t = before;
    fwdFromJson(json, strlen(json), t, e, ch);
    return fwdVerifyApply(before, t, *ch, e);
  };
  auto hasErr = [](const Errors& e, const char* s) {
    for (int i = 0; i < e.n; i++)
      if (strstr(e.at(i), s)) return true;
    return false;
  };
  FwdTarget a = fwdDefaultTarget(FwdType::Wu), t;
  strcpy(a.id, "EXAMPLE1");
  FwdChange ch;
  Errors e;
  // a new target: never verified, and asking it on is refused with the one template; the rest is kept
  FwdTarget none;
  memset(&none, 0, sizeof none);
  CHECK(!put(none, "{\"type\":\"windy\",\"id\":\"x1\",\"enabled\":true}", &t, &ch, &e) && !t.verified && !t.enabled);
  CHECK(hasErr(e, "Send one now first: it checks the id and key.") && strcmp(t.id, "x1") == 0);
  // unverified, switched on: refused; verified: allowed
  e = Errors();
  CHECK(!put(a, "{\"enabled\":true}", &t, &ch, &e) && !t.enabled && hasErr(e, "Send one now first"));
  a.verified = true;
  e = Errors();
  CHECK(!put(a, "{\"enabled\":true}", &t, &ch, &e) && t.enabled && t.verified && e.n == 0);
  // the clear matrix, on a verified running target: id, key given, key cleared, type -> cleared and off, no error;
  // the station, the switch itself and https keep it
  a.enabled = true;
  struct { const char* json; bool clears; } M[] = {
      {"{\"id\":\"OTHER2\"}", true},       {"{\"key\":\"newkey\"}", true}, {"{\"key\":null}", true},
      {"{\"type\":\"windy\"}", true},      {"{\"station\":\"ST2\"}", false}, {"{\"enabled\":false}", false},
      {"{\"id\":\"EXAMPLE1\"}", false},    {"{\"key\":\"<set>\"}", false},
  };
  for (auto& m : M) {
    e = Errors();
    const bool cl = put(a, m.json, &t, &ch, &e);
    CHECK(cl == m.clears && t.verified == !m.clears && !hasErr(e, "Send one now first"));
    if (m.clears) CHECK(!t.enabled);
  }
  // wu_compat: host / path changes clear it (fwdKeyFollows), as a template's URL does
  FwdTarget c = fwdDefaultTarget(FwdType::WuCompat);
  strcpy(c.wc.host, "example.org");
  c.verified = c.enabled = true;
  e = Errors();
  CHECK(put(c, "{\"host\":\"example.net\"}", &t, &ch, &e) && !t.verified && !t.enabled);
  e = Errors();
  CHECK(put(c, "{\"path\":\"/other.php\"}", &t, &ch, &e) && !t.verified);
  strcpy(c.success, "success");  // the success text decides "accepted"
  e = Errors();
  CHECK(put(c, "{\"success\":\"OK\"}", &t, &ch, &e) && !t.verified && !t.enabled);
  e = Errors();
  CHECK(!put(c, "{\"success\":\"success\"}", &t, &ch, &e) && t.verified && t.enabled);
  // a PUT cannot set it
  a.verified = a.enabled = false;
  e = Errors();
  CHECK(!put(a, "{\"verified\":true,\"enabled\":true}", &t, &ch, &e) && !t.verified && !t.enabled);
  // the stored form: written when set, read back; an older record migrates: on -> verified, off -> not
  {
    Errors le;
    FwdTarget st;
    const char* old_on = "{\"type\":\"wu\",\"id\":\"EXAMPLE1\",\"https\":false,\"enabled\":true}";
    CHECK(fwdLoadStored(old_on, strlen(old_on), &st, &le) && st.enabled && st.verified && le.n == 0);
    const char* old_off = "{\"type\":\"wu\",\"id\":\"EXAMPLE1\",\"https\":false}";
    CHECK(fwdLoadStored(old_off, strlen(old_off), &st, &le) && !st.enabled && !st.verified);
    const char* odd = "{\"type\":\"wu\",\"https\":false,\"enabled\":true,\"verified\":false}";
    CHECK(fwdLoadStored(odd, strlen(odd), &st, &le) && !st.enabled && !st.verified);  // never on without it
    st = fwdDefaultTarget(FwdType::Windy);
    st.verified = true;
    std::string w;
    char wb[64];
    Out ow(toString, &w, wb, sizeof wb);
    fwdToJson(ow, st);
    ow.flush();
    FwdTarget back;
    CHECK(w.find("\"verified\":true") != std::string::npos && fwdLoadStored(w.c_str(), w.size(), &back, &le) && back.verified &&
          !back.enabled);
  }
}

static void testFwd() {
  testFwdFakeEndpoint();
  testFwdVectors();
  testFwdBuilders();
  testFwdTemplates();
  testFwdResponses();
  testFwdScheduler();
  testFwdUnconfirmed();
  testFwdConfig();
  testFwdVerified();
  testFwdCauses();
  testTlsc();
}
