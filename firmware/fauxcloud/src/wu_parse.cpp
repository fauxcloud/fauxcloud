// wu_parse.cpp -- see wu_parse.h.
#include "wu_parse.h"

#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace fc {

static int hexVal(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

void urlDecode(char* s) {
  char* w = s;
  for (const char* r = s; *r; r++) {
    if (*r == '+') {
      *w++ = ' ';
    } else if (*r == '%' && hexVal(r[1]) >= 0 && hexVal(r[2]) >= 0) {
      *w++ = (char)(hexVal(r[1]) << 4 | hexVal(r[2]));
      r += 2;
    } else {
      *w++ = *r;
    }
  }
  *w = 0;
}

bool parseNumber(const char* s, double* out) {
  while (*s == ' ') s++;
  // Only plain decimal syntax: strtod alone would also take "0x1A", "inf", "nan".
  bool digit = false;
  for (const char* p = s; *p; p++) {
    char c = *p;
    if (c >= '0' && c <= '9') digit = true;
    else if (c != '-' && c != '+' && c != '.' && c != 'e' && c != 'E' && c != ' ') return false;
  }
  if (!digit) return false;
  char* end = nullptr;
  double v = strtod(s, &end);
  if (end == s) return false;
  while (*end == ' ') end++;
  if (*end != 0) return false;  // trailing garbage, e.g. "-117.-5"
  if (!std::isfinite(v)) return false;
  *out = v;
  return true;
}

int64_t unixFromUtc(int y, int mo, int d, int h, int mi, int s) {
  // H. Hinnant's days_from_civil.
  y -= mo <= 2;
  const int64_t era = (y >= 0 ? y : y - 399) / 400;
  const int64_t yoe = y - era * 400;
  const int64_t doy = (153 * (mo + (mo > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  const int64_t days = era * 146097 + doe - 719468;
  return days * 86400 + h * 3600 + mi * 60 + s;
}

bool parseDateUtc(const char* s, int64_t* unix_out) {
  int f[6];
  int n = 0;
  const char* p = s;
  while (*p && n < 6) {
    if (!isdigit((unsigned char)*p)) { p++; continue; }
    long v = 0;
    int digits = 0;
    while (isdigit((unsigned char)*p)) {
      if (++digits > 4) return false;
      v = v * 10 + (*p++ - '0');
    }
    f[n++] = (int)v;
  }
  while (*p && !isdigit((unsigned char)*p)) p++;
  if (n != 6 || *p) return false;  // fewer fields, or extra numbers after the seconds
  if (f[0] < 2000 || f[0] > 2099 || f[1] < 1 || f[1] > 12 || f[2] < 1 || f[2] > 31 || f[3] > 23 ||
      f[4] > 59 || f[5] > 60)
    return false;
  *unix_out = unixFromUtc(f[0], f[1], f[2], f[3], f[4], f[5]);
  return true;
}

static double convert(Conv c, double x) {
  switch (c) {
    case Conv::FtoC: return (x - 32.0) / 1.8;
    case Conv::InHgToHpa: return x * 33.8639;
    case Conv::MphToKmh: return x * 1.609344;
    case Conv::InToMm: return x * 25.4;
    case Conv::NmToKm: return x * 1.852;
    case Conv::None: break;
  }
  return x;
}

static bool inBounds(Bound b, double x) {
  switch (b) {
    case Bound::Temp: return x >= -60.0 && x <= 80.0;
    case Bound::Humidity: return x >= 0.0 && x <= 100.0;
    case Bound::Pressure: return x >= 300.0 && x <= 1100.0;
    case Bound::Direction: return x >= -360.0 && x <= 720.0;  // the filter wraps it (-10 -> 350)
    case Bound::Visibility: return x >= 0.0 && x <= 200.0;
    case Bound::Wind: return x >= 0.0 && x <= 500.0;
    case Bound::RainSum: return x >= 0.0 && x <= 2000.0;
    case Bound::RainLong: return x >= 0.0 && x <= 30000.0;
    case Bound::RainRate: return x >= 0.0 && x <= 1000.0;
    case Bound::Uv: return x >= 0.0 && x <= 30.0;
    case Bound::Solar: return x >= 0.0 && x <= 2000.0;
    case Bound::Battery: return x >= 0.0 && x <= 100.0;
  }
  return false;
}

bool fieldInBounds(int f, double x) {
  return f >= 0 && f < F_COUNT && std::isfinite(x) && std::isfinite((float)x) && inBounds(FIELDS[f].bound, x);
}

static void setStationId(Sample* out, const char* raw) {
  int n = 0;
  for (const char* p = raw; *p; p++) {
    if (n >= STATION_ID_LEN - 1) { out->id_sanitized = true; break; }
    char c = *p;
    if (isalnum((unsigned char)c) || c == '_' || c == '-') {
      out->station_id[n++] = c;
    } else {
      out->station_id[n++] = '_';
      out->id_sanitized = true;
    }
  }
  out->station_id[n] = 0;
}

static void copyTrunc(char* dst, size_t cap, const char* src) {
  size_t n = strlen(src);
  if (n >= cap) n = cap - 1;
  memcpy(dst, src, n);
  dst[n] = 0;
}

void queryValueCi(const char* query, const char* key, char* out, size_t cap) {
  out[0] = 0;
  const size_t kl = strlen(key);
  for (const char* p = query ? query : ""; *p;) {
    const char* end = strchr(p, '&');
    if (!end) end = p + strlen(p);
    const char* eq = (const char*)memchr(p, '=', (size_t)(end - p));
    if (eq && (size_t)(eq - p) == kl) {
      size_t i = 0;
      while (i < kl && tolower((unsigned char)p[i]) == tolower((unsigned char)key[i])) i++;
      if (i == kl) {
        size_t n = (size_t)(end - eq - 1);
        if (n >= cap) n = cap - 1;
        memcpy(out, eq + 1, n);
        out[n] = 0;
        return;
      }
    }
    p = *end ? end + 1 : end;
  }
}

void parseQuery(const char* query, Sample* out) {
  memset(out, 0, sizeof *out);
  char buf[640];
  copyTrunc(buf, sizeof buf, query ? query : "");

  char* next = buf;
  while (next) {
    char* kv = next;
    next = strchr(kv, '&');
    if (next) *next++ = 0;
    char* val = strchr(kv, '=');
    if (val) *val++ = 0;
    else val = kv + strlen(kv);  // "flag" without '=' -> empty value
    urlDecode(kv);
    urlDecode(val);
    if (!*kv) continue;

    // ID and PASSWORD in any case, like every field (a "password=" must never become an extra)
    if (sameCi(kv, "ID")) { setStationId(out, val); continue; }
    if (sameCi(kv, "PASSWORD")) continue;  // never stored, never logged
    // dateutc in any case, like ID and every field
    if (sameCi(kv, "dateutc")) { out->has_dateutc = parseDateUtc(val, &out->dateutc); continue; }
    if (isProtocolFlag(kv)) continue;  // action=updateraw, realtime=1, ...: not measurements, not extras

    int f = fieldByParam(kv);
    if (f < 0) {
      if (out->n_extras < MAX_EXTRAS) {
        Extra& e = out->extras[out->n_extras++];
        copyTrunc(e.key, sizeof e.key, kv);
        copyTrunc(e.val, sizeof e.val, val);
      } else if (out->extras_dropped < 255) {
        out->extras_dropped++;
      }
      continue;
    }
    const uint32_t bit = 1u << f;
    out->present &= ~bit;  // last occurrence wins
    out->rejected_parse &= ~bit;
    out->rejected_bounds &= ~bit;
    if (!*val) continue;  // "UV=" -> absent, not an error
    double x;
    if (!parseNumber(val, &x)) { out->rejected_parse |= bit; continue; }
    x = convert(FIELDS[f].conv, x);
    // 1e308 parses, and inf or a float overflow would print as "inf" in JSON and MQTT
    if (!std::isfinite(x) || !std::isfinite((float)x) || !inBounds(FIELDS[f].bound, x)) {
      out->rejected_bounds |= bit;
      continue;
    }
    out->v[f] = (float)x;
    out->present |= bit;
  }
  if (!out->station_id[0]) copyTrunc(out->station_id, sizeof out->station_id, DEFAULT_STATION_ID);
}

}  // namespace fc
