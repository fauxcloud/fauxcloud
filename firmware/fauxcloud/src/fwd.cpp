// fwd.cpp -- see fwd.h.
#include "fwd.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "ha_discovery.h"
#include "history.h"
#include "json.h"
#include "settings.h"
#include "web_render.h"

namespace fc {

// ---------------------------------------------------------------- types
static const char* const TYPE_NAMES[(int)FwdType::COUNT] = {"wu", "windy", "wowbe", "pwsweather", "wu_compat",
                                                             "template"};
const char* fwdTypeName(FwdType t) { return (int)t < (int)FwdType::COUNT ? TYPE_NAMES[(int)t] : ""; }
bool fwdTypeFromName(const char* s, FwdType* out) {
  for (int i = 0; i < (int)FwdType::COUNT; i++)
    if (strcmp(s, TYPE_NAMES[i]) == 0) {
      *out = (FwdType)i;
      return true;
    }
  return false;
}
uint16_t fwdMinFloor(FwdType t) { return t == FwdType::Windy || t == FwdType::PwsWeather ? 300 : 60; }
uint16_t fwdIntervalS(FwdType t) {
  const uint16_t f = fwdMinFloor(t);
  return f < FWD_INTERVAL_S ? FWD_INTERVAL_S : (uint16_t)(f + FWD_MARGIN_S);
}
bool fwdIsPreset(FwdType t) { return t != FwdType::WuCompat && t != FwdType::Template; }

FwdTarget fwdDefaultTarget(FwdType t) {
  FwdTarget x;
  memset(&x, 0, sizeof x);
  x.used = true;
  x.type = t;
  x.https = FWD_TLS && FWD_HTTPS_DEFAULT;  // HTTPS for new targets
  x.min_s = fwdIntervalS(t);
  if (t == FwdType::WuCompat) strcpy(x.wc.path, FWD_WU_PATH);
  return x;
}

uint16_t fwdMaxS(const FwdTarget& t) {
  if (t.max_s) return t.max_s;
  const uint32_t a = (uint32_t)t.min_s + 120, b = 2u * t.min_s;
  return (uint16_t)(a < b ? a : b);
}

const char* const FWD_VERIFY_FIRST = "Send one now first: it checks the id and key.";
bool fwdVerifyApply(const FwdTarget& before, FwdTarget* t, const FwdChange& ch, Errors* err) {
  if (!before.used) t->verified = false;  // a new target
  else if (t->verified && (!fwdKeyFollows(before, *t) || strcmp(before.id, t->id) != 0 || ch.key_given || ch.key_clear ||
                            strcmp(before.success, t->success) != 0))  // what "accepted" means
    t->verified = false;
  const bool cleared = before.used && before.verified && !t->verified;  // a type change has reset it already (fwdFromJson)
  if (t->enabled && !t->verified) {
    if (!cleared) err->add("enabled", FWD_VERIFY_FIRST);  // asked to switch on; a running target just changed goes off
    t->enabled = false;
  }
  return cleared;
}

bool fwdKeyFollows(const FwdTarget& a, const FwdTarget& b) {
  if (a.type != b.type) return false;
  if (a.type == FwdType::Template) return strcmp(a.url, b.url) == 0;
  if (a.type == FwdType::WuCompat) return strcmp(a.wc.host, b.wc.host) == 0 && strcmp(a.wc.path, b.wc.path) == 0;
  return true;
}

// ---------------------------------------------------------------- variables
namespace {
enum Src : int8_t { S_WMEAN = -1, S_GMAX = -2, S_W10 = -3 };
enum Cv : uint8_t { C_NONE, C_F, C_INHG, C_MPH, C_MS, C_IN, C_KN };
struct VarRow {
  const char* name;
  int8_t src;  // a Field, or Src
  Cv cv;
  uint8_t dec;
};
// clang-format off
const VarRow ROWS[] = {
  // WU / imperial names (the WU protocol's own names and meanings: rainin = the past 60 min)
  {"tempf", F_TEMP_C, C_F, 1},                {"dewptf", F_DEWPT_C, C_F, 1},
  {"humidity", F_HUMIDITY, C_NONE, 0},        {"baromin", F_PRESSURE_HPA, C_INHG, 3},
  {"windspeedmph", F_WIND_KMH, C_MPH, 1},     {"windgustmph", F_GUST_KMH, C_MPH, 1},
  {"winddir", F_WIND_DIR, C_NONE, 0},         {"windspdmph_avg2m", F_WIND_AVG2M_KMH, C_MPH, 1},
  {"winddir_avg2m", F_WIND_DIR_AVG2M, C_NONE, 0}, {"windgustmph_10m", F_GUST_10M_KMH, C_MPH, 1},
  {"rainin", F_RAIN_MM, C_IN, 3},             {"dailyrainin", F_RAIN_DAILY_MM, C_IN, 3},
  {"rainratein", F_RAIN_RATE_MMH, C_IN, 3},   {"UV", F_UV, C_NONE, 1},
  {"solarradiation", F_SOLAR_WM2, C_NONE, 1},
  // the period's mean / max, best available
  {"windmean_mph", S_WMEAN, C_MPH, 1},        {"gustmax_mph", S_GMAX, C_MPH, 1},
  {"wind_mean_ms", S_WMEAN, C_MS, 1},         {"gust_max_ms", S_GMAX, C_MS, 1},
  // metric
  {"temp_c", F_TEMP_C, C_NONE, 1},            {"dewpt_c", F_DEWPT_C, C_NONE, 1},
  {"pressure_hpa", F_PRESSURE_HPA, C_NONE, 1},{"wind_ms", F_WIND_KMH, C_MS, 1},
  {"gust_ms", F_GUST_KMH, C_MS, 1},           {"wind_avg2m_ms", F_WIND_AVG2M_KMH, C_MS, 1},
  {"wind_avg10m_ms", S_W10, C_MS, 1},         {"gust_10m_ms", F_GUST_10M_KMH, C_MS, 1},
  {"wind_kmh", F_WIND_KMH, C_NONE, 1},        {"gust_kmh", F_GUST_KMH, C_NONE, 1},
  {"rain_1h_mm", F_RAIN_MM, C_NONE, 2},       {"rain_daily_mm", F_RAIN_DAILY_MM, C_NONE, 1},
  {"rain_rate_mmh", F_RAIN_RATE_MMH, C_NONE, 1},
  // not in fwd_reference.py: knots
  {"wind_mean_kn", S_WMEAN, C_KN, 1},         {"gust_max_kn", S_GMAX, C_KN, 1},
};
// clang-format on
constexpr int N_ROWS = (int)(sizeof ROWS / sizeof ROWS[0]);
enum Special { X_DATEUTC, X_ISO, X_EPOCH, X_SOFTWARE, X_COMPACT, X_DATE_LOCAL, X_TIME_LOCAL, X_ID, X_KEY, X_COUNT };
const char* const SPECIAL[X_COUNT] = {"dateutc",   "dateutc_iso", "epoch", "softwaretype", "dateutc_compact",
                                      "date_local", "time_local", "id",    "key"};

bool nameIs(const char* name, size_t n, const char* s) { return strlen(s) == n && memcmp(name, s, n) == 0; }

// The record's value of a row's source (the "best available" chains of fwd_reference.py variables()).
bool srcValue(const FwdRecord& r, int8_t src, double* v) {
  auto f = [&](int k) -> bool {
    if (!((r.has >> k) & 1u)) return false;
    *v = r.v[k];
    return true;
  };
  switch (src) {
    case S_WMEAN:
      if (r.has_wind10) {
        *v = r.wind10;
        return true;
      }
      return f(F_WIND_AVG2M_KMH) || f(F_WIND_KMH);
    case S_GMAX: return f(F_GUST_10M_KMH) || f(F_GUST_KMH);
    case S_W10:
      if (!r.has_wind10) return false;
      *v = r.wind10;
      return true;
    default: return f(src);
  }
}

// Python's left-to-right operations on doubles (fwd_reference.py): c * 9 / 5 + 32, k / 1.609344, ...
double convert(double x, Cv cv) {
  switch (cv) {
    case C_F: return x * 9 / 5 + 32;
    case C_INHG: return x / 33.8638866667;
    case C_MPH: return x / 1.609344;
    case C_MS: return x / 3.6;
    case C_IN: return x / 25.4;
    case C_KN: return x / 1.852;
    default: return x;
  }
}

int copyOut(char* buf, size_t cap, const char* s) {
  const size_t n = strlen(s);
  if (n >= cap) return -1;
  memcpy(buf, s, n + 1);
  return (int)n;
}
}  // namespace

const int FWD_VAR_COUNT = N_ROWS + X_COUNT;
const char* fwdVarName(int i) {
  return i < 0 ? "" : i < N_ROWS ? ROWS[i].name : i < N_ROWS + X_COUNT ? SPECIAL[i - N_ROWS] : "";
}

bool fwdVarKnown(const char* name, size_t n) {
  for (int i = 0; i < N_ROWS; i++)
    if (nameIs(name, n, ROWS[i].name)) return true;
  for (const char* s : SPECIAL)
    if (nameIs(name, n, s)) return true;
  return false;
}

int fwdVar(const FwdCtx& c, const char* name, size_t n, char* buf, size_t cap) {
  for (int i = 0; i < N_ROWS; i++) {
    if (!nameIs(name, n, ROWS[i].name)) continue;
    double v;
    if (!c.r || !srcValue(*c.r, ROWS[i].src, &v)) return -1;
    const double x = convert(v, ROWS[i].cv);
    if (!std::isfinite(x)) return -1;
    const int k = snprintf(buf, cap, "%.*f", (int)ROWS[i].dec, x);
    return k > 0 && (size_t)k < cap ? k : -1;
  }
  int x = -1;
  for (int i = 0; i < X_COUNT; i++)
    if (nameIs(name, n, SPECIAL[i])) x = i;
  if (x < 0) return -2;
  if (x == X_ID) return copyOut(buf, cap, c.id ? c.id : "");
  if (x == X_KEY) return copyOut(buf, cap, c.key ? c.key : "");
  if (x == X_SOFTWARE) return copyOut(buf, cap, c.software ? c.software : "fauxcloud");
  if (!c.r || c.r->t < EPOCH_MIN) return -1;
  char iso[24];
  const uint32_t t = c.r->t;
  if (x == X_EPOCH) {
    const int k = snprintf(buf, cap, "%lu", (unsigned long)t);
    return k > 0 && (size_t)k < cap ? k : -1;
  }
  if (x == X_DATE_LOCAL || x == X_TIME_LOCAL) {
    const int64_t local = (int64_t)t + (c.tz ? localOffset(*c.tz, t) : 0);
    if (formatIsoUtc(iso, sizeof iso, local) != 20) return -1;
    iso[10] = 0;
    iso[19] = 0;
    return copyOut(buf, cap, x == X_DATE_LOCAL ? iso : iso + 11);
  }
  if (formatIsoUtc(iso, sizeof iso, t) != 20) return -1;  // YYYY-MM-DDTHH:MM:SSZ
  if (x == X_ISO) return copyOut(buf, cap, iso);
  if (x == X_DATEUTC) {
    iso[10] = ' ';
    iso[19] = 0;
    return copyOut(buf, cap, iso);
  }
  // X_COMPACT: YYYYMMDDHHMM
  const char out[13] = {iso[0], iso[1], iso[2], iso[3], iso[5], iso[6], iso[8], iso[9], iso[11], iso[12], iso[14],
                        iso[15], 0};
  return copyOut(buf, cap, out);
}

// ---------------------------------------------------------------- the clean record
void fwdRecordBuild(const FwdLiveIn& in, uint32_t min_s, FwdRecord* r) {
  memset(r, 0, sizeof *r);
  r->t = in.last_seen;
  const uint32_t drop = in.hidden | in.warming;
  for (int f = 0; f < F_COUNT; f++) {
    if (!((in.present >> f) & 1u) || ((drop >> f) & 1u)) continue;
    const uint32_t ft = in.field_t[f];
    if (!ft || ft > in.last_seen || in.last_seen - ft > min_s) continue;  // unknown or too old an age
    char b[24];
    if (formatValue(b, sizeof b, f, in.v[f]) <= 0) continue;  // the /api/live text
    r->v[f] = strtod(b, nullptr);
    r->has |= 1u << f;
  }
  if (std::isfinite(in.wind10) && !((drop >> F_WIND_KMH) & 1u)) {
    char b[24];
    snprintf(b, sizeof b, "%.1f", (double)in.wind10);  // outlookJson's "wind10"
    r->wind10 = strtod(b, nullptr);
    r->has_wind10 = true;
  }
}

static const char* const GATE_NAMES[] = {"ok", "no_station", "offline", "stale", "restored", "no_time", "old",
                                         "no_core", "same"};
const char* fwdGateName(FwdGate g) { return GATE_NAMES[(int)g]; }

FwdGate fwdGate(const FwdGateIn& g, bool test) {
  if (!g.exists) return FwdGate::NoStation;
  if (!g.online) return FwdGate::Offline;
  if (g.sensor_stale) return FwdGate::Stale;
  if (g.restored) return FwdGate::Restored;
  if (!g.time_ok) return FwdGate::NoTime;
  if (g.last_seen < EPOCH_MIN) return FwdGate::Old;
  if (!g.has_temp && !g.has_wind) return FwdGate::NoCore;
  if (!test && g.ok_seen && g.ok_seen == g.last_seen) return FwdGate::Same;
  return FwdGate::Ok;
}

// ---------------------------------------------------------------- rendering (fwd_reference.py render())
namespace {
const char HEX[] = "0123456789ABCDEF";
bool unreserved(unsigned char ch) {
  return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '_' || ch == '.' ||
         ch == '-' || ch == '~';
}
int hexVal(char ch) {
  if (ch >= '0' && ch <= '9') return ch - '0';
  if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
  if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
  return -1;
}
bool phChar(char ch) {
  return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '_';
}
// A placeholder {name} at s[i] (name = [A-Za-z0-9_]+): its length incl. braces, 0 = none.
size_t placeholder(const char* s, size_t n, size_t i) {
  if (s[i] != '{') return 0;
  size_t j = i + 1;
  while (j < n && phChar(s[j])) j++;
  return j > i + 1 && j < n && s[j] == '}' ? j - i + 1 : 0;
}

struct Buf {
  char* p;
  size_t cap, n;
  bool over;
  void put(char ch) {
    if (n + 1 < cap) p[n++] = ch;
    else over = true;
  }
  void puts(const char* s, size_t k) {
    for (size_t i = 0; i < k; i++) put(s[i]);
  }
  void enc(const char* s, size_t k, bool plus) {  // quote(safe="") / quote_plus
    for (size_t i = 0; i < k; i++) {
      const unsigned char ch = (unsigned char)s[i];
      if (unreserved(ch)) put((char)ch);
      else if (plus && ch == ' ') put('+');
      else {
        put('%');
        put(HEX[ch >> 4]);
        put(HEX[ch & 15]);
      }
    }
  }
};

// parse_qsl's unquote: '+' -> space, valid %XX -> byte, anything else literal. In place into out (cap >= k + 1).
size_t qsDecode(const char* s, size_t k, char* out) {
  size_t n = 0;
  for (size_t i = 0; i < k; i++) {
    if (s[i] == '+') out[n++] = ' ';
    else if (s[i] == '%' && i + 2 < k && hexVal(s[i + 1]) >= 0 && hexVal(s[i + 2]) >= 0) {
      out[n++] = (char)(hexVal(s[i + 1]) * 16 + hexVal(s[i + 2]));
      i += 2;
    } else out[n++] = s[i];
  }
  out[n] = 0;
  return n;
}

bool utf8Valid(const char* s, size_t n) {
  for (size_t i = 0; i < n;) {
    const unsigned char c = (unsigned char)s[i];
    size_t len;
    uint32_t cp;
    if (c < 0x80) {
      i++;
      continue;
    } else if ((c & 0xE0) == 0xC0) len = 2, cp = c & 0x1F;
    else if ((c & 0xF0) == 0xE0) len = 3, cp = c & 0x0F;
    else if ((c & 0xF8) == 0xF0) len = 4, cp = c & 0x07;
    else return false;
    if (i + len > n) return false;
    for (size_t k = 1; k < len; k++) {
      const unsigned char d = (unsigned char)s[i + k];
      if ((d & 0xC0) != 0x80) return false;
      cp = cp << 6 | (d & 0x3F);
    }
    if ((len == 2 && cp < 0x80) || (len == 3 && cp < 0x800) || (len == 4 && (cp < 0x10000 || cp > 0x10FFFF)) ||
        (cp >= 0xD800 && cp <= 0xDFFF))
      return false;
    i += len;
  }
  return true;
}

struct Split {
  bool https;
  const char *host, *path, *query, *query2;  // query2: a second segment, as if after an '&' (the shared WU fields)
  size_t host_n, path_n, query_n, query2_n;
};
// "http(s)://host[:port]/path?query" (validated forms only; a fragment is cut)
bool splitUrl(const char* u, Split* s) {
  if (strncmp(u, "https://", 8) == 0) s->https = true, u += 8;
  else if (strncmp(u, "http://", 7) == 0) s->https = false, u += 7;
  else return false;
  s->host = u;
  while (*u && *u != '/' && *u != '?' && *u != '#') u++;
  s->host_n = (size_t)(u - s->host);
  s->path = u;
  while (*u && *u != '?' && *u != '#') u++;
  s->path_n = (size_t)(u - s->path);
  s->query = *u == '?' ? u + 1 : u;
  if (*u == '?') u++;
  while (*u && *u != '#') u++;
  s->query_n = (size_t)(u - s->query);
  s->query2 = "", s->query2_n = 0;
  return s->host_n > 0 && s->path_n > 0 && s->path[0] == '/';
}
}  // namespace

static const char* const ERR_NAMES[] = {"", "no_value", "too_long", "bad_url", "unknown_var"};
const char* fwdErrName(FwdErr e) { return ERR_NAMES[(int)e]; }

namespace {
// parse_qsl's unquote as a byte stream: '+' -> space, a valid %XX -> its byte, anything else as it is. No buffer: the
// query is decoded twice (presence check, then output) instead of being copied (RAM budget).
struct Dec {
  const char* s;
  size_t n, i;
  bool end() const { return i >= n; }
  int next() {
    const char c = s[i++];
    if (c == '+') return ' ';
    if (c == '%' && i + 1 < n && hexVal(s[i]) >= 0 && hexVal(s[i + 1]) >= 0) {
      const int v = hexVal(s[i]) * 16 + hexVal(s[i + 1]);
      i += 2;
      return v;
    }
    return (unsigned char)c;
  }
};
// A placeholder {name} on the decoded stream at '{' (just read): the name (first `cap - 1` bytes) and true, d past
// the '}'; false = not a placeholder, d unchanged.
bool decPlaceholder(Dec& d, char* name, size_t cap, size_t* len) {
  Dec t = d;
  size_t k = 0;
  while (!t.end()) {
    const Dec before = t;
    const int c = t.next();
    if (c == '}' && k) {
      d = t;
      name[k < cap ? k : cap - 1] = 0;
      *len = k < cap - 1 ? k : cap - 1;
      return true;
    }
    if (c < 0 || c > 127 || !phChar((char)c)) {
      (void)before;
      return false;
    }
    if (k < cap - 1) name[k] = (char)c;
    k++;
  }
  return false;
}

struct Render {
  const FwdCtx& c;
  FwdUrl* out;
  Buf b;
  char val[FWD_KEY_MAX + 8];
  int len;
  bool raw;  // the redacted key: "***" goes out as it is
  ~Render() { memset(val, 0, sizeof val); }  // the key's copy never stays on the stack
  // the value of {name}: None + len (-1 = absent), or UnknownVar
  FwdErr lookup(const char* name, size_t n) {
    raw = false;
    if (c.redact && nameIs(name, n, "key")) {
      strcpy(val, "***");
      len = 3;
      raw = true;
      return FwdErr::None;
    }
    len = fwdVar(c, name, n, val, sizeof val);
    if (len == -2) {
      snprintf(out->why, sizeof out->why, "%.*s", (int)(n < 32 ? n : 32), name);
      return FwdErr::UnknownVar;
    }
    return FwdErr::None;
  }
};
}  // namespace

// fwd_reference.py render() on the parts of a URL: the path strictly (quote(safe="")), the query through parse_qsl +
// urlencode (quote_plus), a pair with an absent variable dropped.
static FwdErr renderParts(const Split& s, const FwdCtx& c, FwdUrl* out, char* target, size_t cap) {
  out->why[0] = 0;
  if (!target || cap < 2) return FwdErr::TooLong;
  target[0] = 0;
  if (!s.host_n || s.host_n >= sizeof out->host || !s.path_n || s.path[0] != '/') return FwdErr::BadUrl;
  out->https = s.https;
  memcpy(out->host, s.host, s.host_n);
  out->host[s.host_n] = 0;
  Render R{c, out, Buf{target, cap > FWD_TARGET_MAX + 1 ? FWD_TARGET_MAX + 1 : cap, 0, false}, {}, 0, false};
  for (size_t i = 0; i < s.path_n;) {
    const size_t ph = placeholder(s.path, s.path_n, i);
    if (!ph) {
      R.b.put(s.path[i++]);
      continue;
    }
    const FwdErr e = R.lookup(s.path + i + 1, ph - 2);
    if (e != FwdErr::None) return e;
    if (R.len < 0) {
      snprintf(out->why, sizeof out->why, "%.*s", (int)(ph - 2 < 32 ? ph - 2 : 32), s.path + i + 1);
      return FwdErr::NoValue;
    }
    if (R.raw) R.b.puts(R.val, (size_t)R.len);
    else R.b.enc(R.val, (size_t)R.len, false);
    i += ph;
  }
  bool first = true;
  char name[33];
  for (int seg = 0; seg < 2; seg++) {
  const char* q = seg ? s.query2 : s.query;
  const size_t qn = seg ? s.query2_n : s.query_n;
  for (size_t i = 0; i < qn;) {
    size_t j = i;
    while (j < qn && q[j] != '&') j++;
    if (j > i) {
      size_t eq = i;
      while (eq < j && q[eq] != '=') eq++;
      const Dec val0{q + (eq < j ? eq + 1 : j), eq < j ? j - eq - 1 : 0, 0};
      // pass 1: every placeholder of the decoded value must have a value, else the pair is dropped
      bool drop = false;
      for (Dec d = val0; !d.end() && !drop;) {
        size_t nl;
        if (d.next() == '{' && decPlaceholder(d, name, sizeof name, &nl)) {
          const FwdErr e = R.lookup(name, nl);
          if (e != FwdErr::None) return e;
          drop = R.len < 0;
        }
      }
      if (!drop) {  // pass 2: key=value, each quote_plus'd; placeholders substituted (never re-scanned)
        R.b.put(first ? '?' : '&');
        first = false;
        for (Dec d{q + i, eq - i, 0}; !d.end();) {
          const char ch = (char)d.next();
          R.b.enc(&ch, 1, true);
        }
        R.b.put('=');
        for (Dec d = val0; !d.end();) {
          const int ch = d.next();
          size_t nl;
          if (ch == '{' && decPlaceholder(d, name, sizeof name, &nl)) {
            R.lookup(name, nl);
            if (R.raw) R.b.puts(R.val, (size_t)R.len);
            else R.b.enc(R.val, (size_t)R.len, true);
          } else {
            const char cc = (char)ch;
            R.b.enc(&cc, 1, true);
          }
        }
      }
    }
    i = j + 1;
  }
  }
  if (R.b.over) {
    target[0] = 0;
    return FwdErr::TooLong;
  }
  target[R.b.n] = 0;
  return FwdErr::None;
}

FwdErr fwdRenderUrl(const char* tpl, const FwdCtx& c, FwdUrl* out, char* target, size_t cap) {
  Split s;
  if (!splitUrl(tpl, &s)) {
    out->why[0] = 0;
    if (target && cap) target[0] = 0;
    return FwdErr::BadUrl;
  }
  return renderParts(s, c, out, target, cap);
}

// fwd_reference.py PRESETS (the same query order); https unless the target says http
#define FWD_WU_FIELDS                                                                                             \
  "dateutc={dateutc}&tempf={tempf}&dewptf={dewptf}&humidity={humidity}&baromin={baromin}"                       \
  "&windspeedmph={windspeedmph}&windgustmph={windgustmph}&winddir={winddir}"                                     \
  "&windspdmph_avg2m={windspdmph_avg2m}&winddir_avg2m={winddir_avg2m}&windgustmph_10m={windgustmph_10m}"         \
  "&rainin={rainin}&dailyrainin={dailyrainin}&UV={UV}&solarradiation={solarradiation}"                           \
  "&softwaretype={softwaretype}"
static const char* const WU_FIELDS = FWD_WU_FIELDS;  // once in flash, the second segment of wu / pwsweather / wu_compat
static const char* const WU_QUERY = "ID={id}&PASSWORD={key}&action=updateraw";
static const char* const PWS_QUERY = "ID={id}&PASSWORD={key}";
static const char* const WOWBE_QUERY =
    "ID={id}&PASSWORD={key}&action=updateraw"
    "&dateutc={dateutc}&tempf={tempf}&dewptf={dewptf}&humidity={humidity}&baromin={baromin}"
    "&windspeedmph={windspeedmph}&windgustmph={windgustmph}&winddir={winddir}"
    "&rainin={rainratein}&dailyrainin={dailyrainin}&UV={UV}&solarradiation={solarradiation}"
    "&softwaretype={softwaretype}";
static const char* const WINDY_QUERY =
    "id={id}&PASSWORD={key}&time={dateutc_iso}"
    "&temp={temp_c}&dewpoint={dewpt_c}&humidity={humidity}&mbar={pressure_hpa}&wind={wind_mean_ms}"
    "&gust={gust_max_ms}&winddir={winddir}&precip={rain_1h_mm}&uv={UV}&solarradiation={solarradiation}"
    "&softwaretype={softwaretype}";

FwdErr fwdRender(const FwdTarget& t, const FwdCtx& c, FwdUrl* out, char* target, size_t cap) {
  Split s;
  const char *host = nullptr, *path = nullptr, *query = nullptr, *query2 = "";
  switch (t.type) {
    case FwdType::Wu: host = "rtupdate.wunderground.com", path = FWD_WU_PATH, query = WU_QUERY, query2 = WU_FIELDS; break;
    case FwdType::PwsWeather:
      host = "pwsupdate.pwsweather.com", path = "/api/v1/submitwx", query = PWS_QUERY, query2 = WU_FIELDS;
      break;
    case FwdType::WowBe: host = "wow.meteo.be", path = "/api/v2/send", query = WOWBE_QUERY; break;
    case FwdType::Windy: host = "stations.windy.com", path = "/api/v2/observation/update", query = WINDY_QUERY; break;
    case FwdType::WuCompat:
      host = t.wc.host, path = t.wc.path[0] ? t.wc.path : FWD_WU_PATH, query = WU_QUERY, query2 = WU_FIELDS;
      break;
    case FwdType::Template: return fwdRenderUrl(t.url, c, out, target, cap);
    default: break;
  }
  if (!host || !host[0]) {
    out->why[0] = 0;
    if (target && cap) target[0] = 0;
    return FwdErr::BadUrl;
  }
  s.https = t.https;
  s.host = host, s.host_n = strlen(host);
  s.path = path, s.path_n = strlen(path);
  s.query = query, s.query_n = strlen(query);
  s.query2 = query2, s.query2_n = strlen(query2);
  return renderParts(s, c, out, target, cap);
}

// ---------------------------------------------------------------- validation
bool fwdHostValid(const char* h) {
  const char* colon = strchr(h, ':');
  const size_t n = colon ? (size_t)(colon - h) : strlen(h);
  if (n < 1 || n > FWD_HOST_MAX || h[0] == '.' || h[0] == '-') return false;
  for (size_t i = 0; i < n; i++) {
    const char ch = h[i];
    if (!((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '.' || ch == '-'))
      return false;
  }
  if (!colon) return true;
  const char* p = colon + 1;
  if (!*p || strlen(p) > 5) return false;
  long port = 0;
  for (; *p; p++) {
    if (*p < '0' || *p > '9') return false;
    port = port * 10 + (*p - '0');
  }
  return port >= 1 && port <= 65535;
}

static bool printableAscii(const char* s) {
  for (; *s; s++)
    if ((unsigned char)*s <= 0x20 || (unsigned char)*s >= 0x7F) return false;
  return true;
}

bool fwdPathValid(const char* p) { return p[0] == '/' && strlen(p) <= FWD_PATH_MAX && printableAscii(p); }

bool fwdTemplateValid(const char* url, char* why, size_t cap) {
  auto no = [&](const char* w) {
    snprintf(why, cap, "%s", w);
    return false;
  };
  if (strlen(url) > FWD_URL_MAX) return no("longer than 384 bytes");
  if (!printableAscii(url)) return no("spaces, control or non-ASCII characters (use %XX)");
  if (strpbrk(url, "\"\\")) return no("no quote or backslash (use %22, %5C)");
  if (strncmp(url, "https://", 8) != 0 && strncmp(url, "http://", 7) != 0) return no("must start with http:// or https://");
  if (!FWD_TLS && strncmp(url, "https://", 8) == 0) return no("this build has no encryption: use http://");  // the HTTP-only fallback
  if (strchr(url, '#')) return no("no #fragment");
  Split s;
  if (!splitUrl(url, &s)) return no("needs a host and a path (at least /)");
  char host[FWD_HOST_MAX + 8];
  if (s.host_n >= sizeof host) return no("host too long");
  memcpy(host, s.host, s.host_n);
  host[s.host_n] = 0;
  if (strchr(host, '@')) return no("no user:password@ in the URL");
  if (!fwdHostValid(host)) return no("not a valid host[:port]");
  if (s.path_n > FWD_PATH_MAX + 128) return no("path too long");
  for (size_t i = 0; i < s.path_n; i++) {
    const size_t ph = placeholder(s.path, s.path_n, i);
    if (ph && !fwdVarKnown(s.path + i + 1, ph - 2)) {
      snprintf(why, cap, "unknown variable {%.*s}", (int)(ph - 2 < 32 ? ph - 2 : 32), s.path + i + 1);
      return false;
    }
  }
  char d[FWD_URL_MAX + 1];  // httpd task / host: on the stack
  for (size_t i = 0; i < s.query_n;) {
    size_t j = i;
    while (j < s.query_n && s.query[j] != '&') j++;
    size_t eq = i;
    while (eq < j && s.query[eq] != '=') eq++;
    if (!utf8Valid(d, qsDecode(s.query + i, eq - i, d))) return no("a %XX escape is not valid UTF-8");
    if (eq < j) {
      const size_t vn = qsDecode(s.query + eq + 1, j - eq - 1, d);
      if (!utf8Valid(d, vn)) return no("a %XX escape is not valid UTF-8");
      for (size_t k = 0; k < vn; k++) {
        const size_t ph = placeholder(d, vn, k);
        if (ph && !fwdVarKnown(d + k + 1, ph - 2)) {
          snprintf(why, cap, "unknown variable {%.*s}", (int)(ph - 2 < 32 ? ph - 2 : 32), d + k + 1);
          return false;
        }
      }
    }
    i = j + 1;
  }
  return true;
}

// ---------------------------------------------------------------- target JSON
static bool fwdStationIdOk(const char* v) {  // Sample::station_id's alphabet, as the admin's validId
  if (!v[0] || strlen(v) >= STATION_ID_LEN) return false;
  for (; *v; v++)
    if (!((*v >= 'A' && *v <= 'Z') || (*v >= 'a' && *v <= 'z') || (*v >= '0' && *v <= '9') || *v == '_' || *v == '-'))
      return false;
  return true;
}

static bool textOk(const char* s) {  // printable, valid UTF-8 (the key)
  for (const char* p = s; *p; p++)
    if ((unsigned char)*p < 0x20 || *p == 0x7F) return false;
  return utf8Valid(s, strlen(s));
}
// id / success: also no quote or backslash (they would double in the stored JSON: fw<n> stays <= 600 B)
static bool idOk(const char* s) { return textOk(s) && !strpbrk(s, "\"\\"); }

bool fwdFromJson(const char* json, size_t len, FwdTarget* t, Errors* err, FwdChange* ch) {
  Errors dummyE;
  FwdChange dummyC;
  if (!err) err = &dummyE;
  if (!ch) ch = &dummyC;
  memset(ch, 0, sizeof *ch);
  // pass 1: "type" first (a type sets the defaults the rest applies to)
  FwdTarget x = *t;
  {
    JsonReader r(json, len);
    if (!r.beginObject()) {
      err->add("$", "not a JSON object");
      return false;
    }
    char k[16];
    while (r.nextKey(k, sizeof k)) {
      if (strcmp(k, "type") == 0 && r.peek() == '"') {
        char v[16];
        bool trunc = false;
        FwdType ty;
        if (!r.readString(v, sizeof v, &trunc)) break;
        if (trunc || !fwdTypeFromName(v, &ty)) {
          err->add("type", "unknown service type");
          continue;
        }
        if (!x.used || ty != x.type) {
          const FwdTarget old = x;
          x = fwdDefaultTarget(ty);
          if (old.used) {  // the station, account id, switch and HTTPS choice stay; the service fields reset
            memcpy(x.station, old.station, sizeof x.station);
            memcpy(x.id, old.id, sizeof x.id);
            x.enabled = old.enabled;
            if (ty != FwdType::Template) x.https = FWD_TLS && old.https;  // a template: its URL decides
          }
        }
      } else if (!r.skipValue()) {
        break;
      }
    }
  }
  if (!x.used) {
    err->add("type", "required for a new service");
    return true;
  }
  JsonReader r(json, len);
  r.beginObject();
  char k[16];
  while (r.nextKey(k, sizeof k)) {
    const char p = r.peek();
    auto str = [&](char* dst, size_t cap, bool (*ok)(const char*), const char* why) {
      char v[FWD_URL_MAX + 2];
      bool trunc = false;
      if (p == 'n') {
        r.skipValue();
        v[0] = 0;
      } else if (p != '"') {
        err->add(k, "not a string");
        r.skipValue();
        return;
      } else if (!r.readString(v, sizeof v, &trunc)) {
        return;
      }
      if (trunc || strlen(v) >= cap || (v[0] && ok && !ok(v))) {
        err->add(k, why);
        return;
      }
      memset(dst, 0, cap);  // whole field: a PUT of the same value compares equal (FwdChange::any)
      memcpy(dst, v, strlen(v));  // < cap (checked above)
    };
    if (strcmp(k, "type") == 0) {
      r.skipValue();
    } else if (strcmp(k, "station") == 0) {
      str(x.station, sizeof x.station, fwdStationIdOk, "not a station id");
    } else if (strcmp(k, "host") == 0 || strcmp(k, "path") == 0) {
      if (x.type != FwdType::WuCompat) {
        err->add(k, "only for a WU-compatible server");
        r.skipValue();
      } else if (k[0] == 'h') {
        str(x.wc.host, sizeof x.wc.host, fwdHostValid, "not a valid host[:port]");
      } else {
        str(x.wc.path, sizeof x.wc.path, fwdPathValid, "must start with / (printable ASCII, <= 128)");
        if (!x.wc.path[0]) strcpy(x.wc.path, FWD_WU_PATH);
      }
    } else if (strcmp(k, "url") == 0) {
      if (x.type != FwdType::Template) {
        err->add(k, "only for a custom URL template");
        r.skipValue();
        continue;
      }
      char v[FWD_URL_MAX + 2], why[64];
      bool trunc = false;
      if (p != '"') {
        err->add(k, "not a string");
        r.skipValue();
        continue;
      }
      if (!r.readString(v, sizeof v, &trunc)) break;
      if (trunc || !fwdTemplateValid(v, why, sizeof why)) {
        err->add(k, trunc ? "longer than 384 bytes" : why);
        continue;
      }
      memset(x.url, 0, sizeof x.url);
      strcpy(x.url, v);
      x.https = strncmp(v, "https://", 8) == 0;
    } else if (strcmp(k, "success") == 0) {
      if (fwdIsPreset(x.type)) {
        err->add(k, "fixed for this service");
        r.skipValue();
      } else {
        str(x.success, sizeof x.success, idOk, "printable text, <= 16 bytes, no quote");
      }
    } else if (strcmp(k, "id") == 0) {
      str(x.id, sizeof x.id, idOk, "printable text, <= 64 bytes, no quote");
    } else if (strcmp(k, "key") == 0) {
      if (p == 'n') {
        r.skipValue();
        ch->key_clear = true;
        continue;
      }
      if (p != '"') {
        err->add(k, "not a string");
        r.skipValue();
        continue;
      }
      char v[FWD_KEY_MAX + 2];
      bool trunc = false;
      if (!r.readString(v, sizeof v, &trunc)) break;
      if (!v[0] || strcmp(v, SECRET_SET) == 0) continue;  // unchanged (settings rule)
      if (trunc || strlen(v) > FWD_KEY_MAX || !textOk(v)) {
        err->add(k, "printable text, <= 96 bytes");
        memset(v, 0, sizeof v);
        continue;
      }
      strcpy(ch->key, v);
      memset(v, 0, sizeof v);
      ch->key_given = true;
    } else if (strcmp(k, "min_s") == 0 || strcmp(k, "max_s") == 0) {
      // the interval is fixed. Old records and API callers may still carry the keys: null or the fixed
      // value pass quietly, anything else is noted (and replaced below)
      if (p == 'n') {
        r.skipValue();
        continue;
      }
      double v;
      if (!r.readNumber(&v)) {
        err->add(k, "not a number");
        if (r.failed()) break;
        r.skipValue();
        continue;
      }
      const FwdTarget fixed = fwdDefaultTarget(x.type);
      if (v != (k[1] == 'i' ? (double)fixed.min_s : (double)fwdMaxS(fixed)) && !(k[1] == 'a' && v == 0))
        err->add(k, "fixed by fauxcloud: every 5 minutes, or a service's own minimum + 10 s");
    } else if (strcmp(k, "https") == 0 || strcmp(k, "enabled") == 0) {
      bool b;
      if (!r.readBool(&b)) {
        err->add(k, "not true/false");
        if (r.failed()) break;
        r.skipValue();
        continue;
      }
      if (k[0] == 'e') x.enabled = b;
      else if (x.type == FwdType::Template) {
        if (b != x.https) err->add(k, "the URL's scheme decides");
      } else if (b && !FWD_TLS) {
        err->add(k, "this build has no encryption: it sends plain HTTP");
        x.https = false;
      } else x.https = b;
    } else if (strcmp(k, "verified") == 0) {  // the stored form only (fwdLoadStored applies it)
      bool b = false;
      if (r.readBool(&b)) ch->has_verified = true, ch->verified = b;
      else if (r.failed()) break;
    } else if (strcmp(k, "remove") == 0) {
      bool b = false;
      if (r.readBool(&b) && b) ch->remove = true;
      else if (r.failed()) break;
    } else {
      err->add(k, "unknown key, ignored");
      r.skipValue();
    }
    if (r.failed()) break;
  }
  if (r.failed()) err->add("$", "malformed JSON, the rest of the document was ignored");
  x.min_s = fwdIntervalS(x.type);  // fixed, whatever an old record said
  x.max_s = 0;
  if (!FWD_TLS) x.https = false;  // an old record's HTTPS (FWD_LEGACY_HTTPS) becomes plain HTTP on this build
  ch->any = memcmp(&x, t, sizeof x) != 0;
  *t = x;
  return true;
}

bool fwdLoadStored(const char* json, size_t len, FwdTarget* t, Errors* err) {
  memset(t, 0, sizeof *t);
  FwdChange ch;
  if (!fwdFromJson(json, len, t, err, &ch)) return false;
  bool has = false;  // was "https" written?
  {
    JsonReader r(json, len);
    char k[16];
    if (r.beginObject())
      while (r.nextKey(k, sizeof k)) {
        if (strcmp(k, "https") == 0) has = true;
        if (!r.skipValue()) break;
      }
  }
  if (!has && t->used && t->type != FwdType::Template) t->https = FWD_LEGACY_HTTPS && FWD_TLS;
  t->verified = ch.has_verified ? ch.verified : t->enabled;  // an older record migrates (never reset)
  if (t->enabled && !t->verified) t->enabled = false;      // never on without it
  memset(ch.key, 0, sizeof ch.key);
  return true;
}

void fwdToJson(Out& o, const FwdTarget& t) {
  const FwdTarget d = fwdDefaultTarget(t.type);
  o.raw("{\"type\":");
  o.jsonStr(fwdTypeName(t.type));
  auto s = [&](const char* k, const char* v, const char* dv) {
    if (strcmp(v, dv) == 0) return;
    o.printf(",\"%s\":", k);
    o.jsonStr(v);
  };
  s("station", t.station, d.station);
  if (t.type == FwdType::WuCompat) {
    s("host", t.wc.host, d.wc.host);
    s("path", t.wc.path, d.wc.path);
  }
  if (t.type == FwdType::Template) s("url", t.url, d.url);
  s("success", t.success, d.success);
  s("id", t.id, d.id);
  if (t.min_s != d.min_s) o.printf(",\"min_s\":%u", (unsigned)t.min_s);
  if (t.max_s) o.printf(",\"max_s\":%u", (unsigned)t.max_s);
  (void)d;
  if (t.type != FwdType::Template) o.printf(",\"https\":%s", t.https ? "true" : "false");  // always (FWD_LEGACY_HTTPS)
  if (t.enabled) o.raw(",\"enabled\":true");
  if (t.verified) o.raw(",\"verified\":true");
  o.raw("}");
}

// ---------------------------------------------------------------- results and the response
static const char* const RESULT_NAMES[] = {"",     "ok",   "dup",           "limited",  "refused",
                                            "reject", "fail", "tls_untrusted", "no_memory"};
const char* fwdResultName(FwdResult r) { return RESULT_NAMES[(int)r]; }

void FwdResp::begin(FwdType t, const char* success) {
  type_ = t;
  snprintf(success_, sizeof success_, "%s", success ? success : "");
  st_ = St::Status;
  line_n_ = total_ = head_n_ = 0;
  status_ = -1;
  retry_ = 0;
  clen_ = -1;
  chunked_ = false;
  chunk_left_ = 0;
  body_[0] = 0;
  body_n_ = 0;
  done_ = overflow_ = false;
  result_ = FwdResult::None;
  sink_ = nullptr;
  sink_ctx_ = nullptr;
  stream_cap_ = stream_n_ = 0;
  limit_ = CAP_TOTAL;
  cut_ = false;
}

void FwdResp::stream(Sink sink, void* ctx, size_t cap) {
  sink_ = sink;
  sink_ctx_ = ctx;
  stream_cap_ = cap;
  limit_ = CAP_LINE + 2 + CAP_HEAD + cap;  // the status line, the headers, the body cap
}

bool FwdResp::needBody() const { return sink_ || (type_ != FwdType::Windy && type_ != FwdType::WowBe); }

bool FwdResp::bodyByte(char ch) {
  if (sink_) {
    // without a length (chunked, or until the close) a body exactly `cap` long is whole -- only a
    // byte past the cap says it was cut
    if (stream_n_ >= stream_cap_) {
      cut_ = true;
      return false;
    }
    sink_(sink_ctx_, &ch, 1);
    stream_n_++;
    if (clen_ >= 0) {
      if ((long)stream_n_ >= clen_) return false;  // the body's own end
      if (stream_n_ >= stream_cap_) {              // more announced than the cap
        cut_ = true;
        return false;
      }
    }
    return true;
  }
  if (body_n_ < CAP_BODY) body_[body_n_++] = ch;
  return !(body_n_ >= CAP_BODY || (clen_ >= 0 && (long)body_n_ >= clen_));
}

static bool ciStarts(const char* s, const char* p) {
  for (; *p; s++, p++) {
    char a = *s, b = *p;
    if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
    if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
    if (a != b) return false;
  }
  return true;
}
static bool ciContains(const char* s, const char* p) {
  for (; *s; s++)
    if (ciStarts(s, p)) return true;
  return false;
}

void FwdResp::classify() {
  const int s = status_;
  FwdResult r;
  if (s < 0) r = FwdResult::Reject;  // a reply that is not HTTP
  else if (type_ == FwdType::Wu && strstr(body_, "INVALIDPASSWORDID")) r = FwdResult::Refused;
  else if (s == 401 || s == 403) r = FwdResult::Refused;
  else if (s == 429) r = FwdResult::Limited;
  else if (s == 409 && type_ == FwdType::Windy) r = FwdResult::Dup;
  else if (s >= 500 && s <= 599) r = FwdResult::Fail;
  else if (s >= 200 && s <= 299) {
    switch (type_) {
      case FwdType::Wu: r = strncmp(body_, "success", 7) == 0 ? FwdResult::Ok : FwdResult::Reject; break;
      case FwdType::PwsWeather: r = strstr(body_, "\"success\":true") ? FwdResult::Ok : FwdResult::Reject; break;
      case FwdType::WuCompat:
      case FwdType::Template:
        r = !success_[0] || strstr(body_, success_) ? FwdResult::Ok : FwdResult::Reject;
        break;
      default: r = FwdResult::Ok; break;
    }
  } else r = FwdResult::Reject;
  result_ = r;
}

void FwdResp::finish() {
  if (done_) return;
  body_[body_n_] = 0;
  classify();
  st_ = St::Done;
  done_ = true;
}

bool FwdResp::feed(const char* d, size_t n) {
  for (size_t i = 0; i < n && !done_; i++) {
    const char ch = d[i];
    if (++total_ > limit_) {
      overflow_ = true;
      if (status_ < 0) {
        st_ = St::Done, done_ = true, result_ = FwdResult::Reject;
        return true;
      }
      finish();
      return true;
    }
    switch (st_) {
      case St::Status:
        if (ch == '\n') {
          line_[line_n_] = 0;
          // "HTTP/1.x NNN ..."
          if (line_n_ >= 12 && strncmp(line_, "HTTP/1.", 7) == 0 && line_[8] == ' ' && line_[9] >= '0' &&
              line_[9] <= '9' && line_[10] >= '0' && line_[10] <= '9' && line_[11] >= '0' && line_[11] <= '9') {
            status_ = (line_[9] - '0') * 100 + (line_[10] - '0') * 10 + (line_[11] - '0');
          } else {
            st_ = St::Done, done_ = true, result_ = FwdResult::Reject;  // garbage
            return true;
          }
          line_n_ = 0;
          if (status_ == 429 || needBody()) st_ = St::Headers;
          else finish();
        } else if (line_n_ < CAP_LINE) {
          line_[line_n_++] = ch;
        } else {
          st_ = St::Done, done_ = true, result_ = FwdResult::Reject;  // no status line in 128 B
          return true;
        }
        break;
      case St::Headers:
        if (++head_n_ > CAP_HEAD) {
          overflow_ = true;
          finish();
          return true;
        }
        if (ch == '\n') {
          if (line_n_ && line_[line_n_ - 1] == '\r') line_n_--;
          line_[line_n_] = 0;
          if (line_n_ == 0) {  // end of the headers
            // a 204 / 304 has no body whatever its headers say (AWC: no report in the window -> 204)
            if (!needBody() || clen_ == 0 || status_ == 204 || status_ == 304) finish();
            else st_ = chunked_ ? St::ChunkSize : St::Body;
          } else if (ciStarts(line_, "retry-after:")) {
            const char* p = line_ + 12;
            while (*p == ' ' || *p == '\t') p++;
            unsigned long v = 0;
            bool digits = *p >= '0' && *p <= '9';
            for (; *p >= '0' && *p <= '9'; p++) v = v * 10 + (unsigned long)(*p - '0');
            retry_ = digits && v < 86400 ? (uint32_t)v : 0;
          } else if (ciStarts(line_, "content-length:")) {
            clen_ = strtol(line_ + 15, nullptr, 10);
            if (clen_ < 0) clen_ = -1;
          } else if (ciStarts(line_, "transfer-encoding:")) {
            chunked_ = ciContains(line_ + 18, "chunked");
          }
          line_n_ = 0;
        } else if (line_n_ < CAP_LINE) {
          line_[line_n_++] = ch;  // a longer header line is cut (only the three above are read)
        }
        break;
      case St::Body:
        if (!bodyByte(ch)) finish();
        break;
      case St::ChunkSize:
        if (ch == '\n') {
          line_[line_n_] = 0;
          unsigned long v = 0;
          int digits = 0;
          for (const char* p = line_; hexVal(*p) >= 0 && digits < 8; p++, digits++) v = v * 16 + (unsigned long)hexVal(*p);
          line_n_ = 0;
          if (!digits || v == 0) {
            finish();
          } else {
            chunk_left_ = (uint32_t)v;
            st_ = St::ChunkData;
          }
        } else if (line_n_ < CAP_LINE) {
          line_[line_n_++] = ch;
        }
        break;
      case St::ChunkData: {
        const long keep = clen_;
        clen_ = -1;  // a chunked body ends at its last chunk, never at a Content-Length
        const bool more = bodyByte(ch);
        clen_ = keep;
        if (!more) {
          finish();
          break;
        }
        if (--chunk_left_ == 0) st_ = St::ChunkCrlf;
        break;
      }
      case St::ChunkCrlf:
        if (ch == '\n') st_ = St::ChunkSize;
        break;
      case St::Done: break;
    }
  }
  return done_;
}

void FwdResp::end() {
  if (done_) return;
  if (status_ < 0) {  // closed before a status line
    st_ = St::Done, done_ = true, result_ = FwdResult::Fail;
    return;
  }
  // the body cut short where the answer depends on it: a short read (retryable)
  const bool bodyNeeded = needBody() && status_ >= 200 && status_ <= 299;
  const size_t got = sink_ ? stream_n_ : body_n_;
  const bool shortBody = (st_ == St::Headers) || (st_ == St::Body && clen_ >= 0 && (long)got < clen_) ||
                         st_ == St::ChunkSize || st_ == St::ChunkData || st_ == St::ChunkCrlf;
  if (bodyNeeded && shortBody && !overflow_) {
    body_[body_n_] = 0;
    st_ = St::Done, done_ = true, result_ = FwdResult::Fail;
    return;
  }
  finish();
}

// ---------------------------------------------------------------- the enable plan (one station)
FwdEnablePlan fwdEnablePlan(const char* const ids[], int n, const char* keep) {
  if (n <= 1) return {true, -1, nullptr};
  if (!keep || !keep[0]) return {false, -1, "keep: choose the station to keep"};
  int k = -1;
  for (int i = 0; i < n; i++)
    if (strcmp(ids[i], keep) == 0) k = i;
  if (k < 0) return {false, -1, "keep: no such station"};
  if (n > 2) return {false, -1, "keep: more than two stations"};
  return {true, k == 0 ? 1 : 0, nullptr};
}

int fwdRenameStation(FwdTarget* t, int n, const char* from, const char* to, uint32_t* mask) {
  int k = 0;
  if (mask) *mask = 0;
  if (!from || !from[0] || !to || !to[0]) return 0;
  for (int i = 0; i < n; i++) {
    if (!t[i].used || strncmp(t[i].station, from, sizeof t[i].station) != 0) continue;
    snprintf(t[i].station, sizeof t[i].station, "%s", to);
    if (mask) *mask |= 1u << i;
    k++;
  }
  return k;
}

}  // namespace fc
