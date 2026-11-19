// tz.cpp -- see tz.h.
#include "tz.h"

#include <cctype>

#include "wu_parse.h"  // unixFromUtc (days-from-civil)

namespace fc {

static bool parseName(const char*& p) {
  if (*p == '<') {
    p++;
    while (*p && *p != '>') p++;
    if (*p != '>') return false;
    p++;
    return true;
  }
  const char* s = p;
  while (isalpha((unsigned char)*p)) p++;
  return p - s >= 3;
}

// [+-]hh[:mm[:ss]] -> seconds (POSIX sign: positive = west of Greenwich).
static bool parseOffset(const char*& p, int32_t* out, int max_hours) {
  int sign = 1;
  if (*p == '+' || *p == '-') sign = (*p++ == '-') ? -1 : 1;
  if (!isdigit((unsigned char)*p)) return false;
  int part[3] = {0, 0, 0};
  for (int i = 0; i < 3; i++) {
    if (i && *p != ':') break;
    if (i) p++;
    if (!isdigit((unsigned char)*p)) return false;
    int v = 0, digits = 0;
    while (isdigit((unsigned char)*p) && digits < 3) v = v * 10 + (*p++ - '0'), digits++;
    part[i] = v;
  }
  if (part[0] > max_hours || part[1] > 59 || part[2] > 59) return false;
  *out = sign * (part[0] * 3600 + part[1] * 60 + part[2]);
  return true;
}

static bool parseRule(const char*& p, PosixTz::Rule* r) {
  if (*p != 'M') return false;  // Jn and n forms are not supported
  p++;
  int v[3];
  for (int i = 0; i < 3; i++) {
    if (i) {
      if (*p != '.') return false;
      p++;
    }
    if (!isdigit((unsigned char)*p)) return false;
    v[i] = 0;
    while (isdigit((unsigned char)*p)) v[i] = v[i] * 10 + (*p++ - '0');
  }
  if (v[0] < 1 || v[0] > 12 || v[1] < 1 || v[1] > 5 || v[2] > 6) return false;
  r->month = (uint8_t)v[0];
  r->week = (uint8_t)v[1];
  r->wday = (uint8_t)v[2];
  r->time = 2 * 3600;
  if (*p == '/') {
    p++;
    if (!parseOffset(p, &r->time, 167)) return false;
  }
  return true;
}

bool parsePosixTz(const char* s, PosixTz* out) {
  if (!s) return false;
  const char* p = s;
  PosixTz tz = {};
  int32_t off;
  if (!parseName(p) || !parseOffset(p, &off, 24)) return false;
  tz.std_off = -off;
  if (*p == 0) {
    *out = tz;
    return true;
  }
  if (!parseName(p)) return false;
  tz.has_dst = true;
  tz.dst_off = tz.std_off + 3600;
  if (*p && *p != ',') {
    if (!parseOffset(p, &off, 24)) return false;
    tz.dst_off = -off;
  }
  if (*p != ',') return false;  // a DST zone without rules is ambiguous
  p++;
  if (!parseRule(p, &tz.start) || *p != ',') return false;
  p++;
  if (!parseRule(p, &tz.end) || *p != 0) return false;
  *out = tz;
  return true;
}

static int64_t floorDiv(int64_t a, int64_t b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }

// Local wall-clock seconds (as if UTC) of the rule's instant in year y.
static int64_t ruleLocal(const PosixTz::Rule& r, int y) {
  const int64_t first = unixFromUtc(y, r.month, 1, 0, 0, 0) / 86400;
  const int wdayFirst = (int)((first % 7 + 7 + 4) % 7);  // 1970-01-01 was a Thursday
  int day = 1 + (r.wday - wdayFirst + 7) % 7 + (r.week - 1) * 7;
  const int nextMonth = r.month == 12 ? 1 : r.month + 1;
  const int nextYear = r.month == 12 ? y + 1 : y;
  const int dim = (int)(unixFromUtc(nextYear, nextMonth, 1, 0, 0, 0) / 86400 - first);
  while (day > dim) day -= 7;  // week 5 = last
  return unixFromUtc(y, r.month, day, 0, 0, 0) + r.time;
}

static int yearOf(int64_t t) {
  // civil year of a unix time (UTC)
  int64_t days = floorDiv(t, 86400) + 719468;
  const int64_t era = (days >= 0 ? days : days - 146096) / 146097;
  const int64_t doe = days - era * 146097;
  const int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const int64_t mp = (5 * doy + 2) / 153;
  const int mo = (int)(mp < 10 ? mp + 3 : mp - 9);
  return (int)(yoe + era * 400 + (mo <= 2));
}

int32_t localOffset(const PosixTz& tz, int64_t utc) {
  if (!tz.has_dst) return tz.std_off;
  const int y = yearOf(utc + tz.std_off);
  // Start is given in standard wall time, end in daylight wall time.
  const int64_t startUtc = ruleLocal(tz.start, y) - tz.std_off;
  const int64_t endUtc = ruleLocal(tz.end, y) - tz.dst_off;
  const bool dst = startUtc < endUtc ? (utc >= startUtc && utc < endUtc)   // northern hemisphere
                                     : !(utc >= endUtc && utc < startUtc);  // southern hemisphere
  return dst ? tz.dst_off : tz.std_off;
}

int32_t localDayNumber(const PosixTz& tz, int64_t utc) {
  return (int32_t)floorDiv(utc + localOffset(tz, utc), 86400);
}

uint32_t dayNumberToYmd(int32_t days) {
  int64_t z = (int64_t)days + 719468;
  const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  const int64_t doe = z - era * 146097;
  const int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const int64_t mp = (5 * doy + 2) / 153;
  const int d = (int)(doy - (153 * mp + 2) / 5 + 1);
  const int mo = (int)(mp < 10 ? mp + 3 : mp - 9);
  const int y = (int)(yoe + era * 400 + (mo <= 2));
  return (uint32_t)(y * 10000 + mo * 100 + d);
}

int32_t ymdToDayNumber(uint32_t ymd) {
  return (int32_t)floorDiv(unixFromUtc((int)(ymd / 10000), (int)(ymd / 100 % 100), (int)(ymd % 100), 0, 0, 0), 86400);
}

}  // namespace fc
