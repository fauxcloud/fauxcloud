// metar.cpp -- METAR / SPECI line parser and the streaming line splitter. Integer maths only.
#include "metar.h"

#include <string.h>

#include "wu_parse.h"  // unixFromUtc

namespace fc {

namespace {

struct Tok {
  const char* p;
  size_t n;
};

bool isD(char c) { return c >= '0' && c <= '9'; }
bool isU(char c) { return c >= 'A' && c <= 'Z'; }
bool allD(const char* p, size_t n) {
  if (!n) return false;
  for (size_t i = 0; i < n; i++)
    if (!isD(p[i])) return false;
  return true;
}
bool allSlash(const Tok& t) {
  if (!t.n) return false;
  for (size_t i = 0; i < t.n; i++)
    if (t.p[i] != '/') return false;
  return true;
}
int num(const char* p, size_t n) {
  int v = 0;
  for (size_t i = 0; i < n; i++) v = v * 10 + (p[i] - '0');
  return v;
}
bool eq(const Tok& t, const char* s) {
  const size_t l = strlen(s);
  return t.n == l && memcmp(t.p, s, l) == 0;
}
bool startsWith(const Tok& t, const char* s) {
  const size_t l = strlen(s);
  return t.n >= l && memcmp(t.p, s, l) == 0;
}
bool endsWith(const Tok& t, const char* s) {
  const size_t l = strlen(s);
  return t.n >= l && memcmp(t.p + t.n - l, s, l) == 0;
}

// The next space-separated token; a trailing '=' (end of report) is dropped, so a token can come back empty.
bool nextTok(const char* line, size_t n, size_t& pos, Tok& t) {
  while (pos < n && (line[pos] == ' ' || line[pos] == '\t' || line[pos] == '\r' || line[pos] == '\n')) pos++;
  if (pos >= n) return false;
  const size_t s = pos;
  while (pos < n && !(line[pos] == ' ' || line[pos] == '\t' || line[pos] == '\r' || line[pos] == '\n')) pos++;
  t.p = line + s;
  t.n = pos - s;
  while (t.n && t.p[t.n - 1] == '=') t.n--;
  return true;
}

bool isIcao(const Tok& t) {
  if (t.n != 4 || !isU(t.p[0])) return false;
  for (int i = 1; i < 4; i++)
    if (!isU(t.p[i]) && !isD(t.p[i])) return false;
  return true;
}
bool isTime(const Tok& t) { return t.n == 7 && allD(t.p, 6) && t.p[6] == 'Z'; }

// days since 1970-01-01 -> civil date (H. Hinnant's algorithm)
void civilFromDays(int64_t z, int& y, int& m) {
  z += 719468;
  const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  const int64_t doe = z - era * 146097;
  const int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const int64_t mp = (5 * doy + 2) / 153;
  m = (int)(mp < 10 ? mp + 3 : mp - 9);
  y = (int)(yoe + era * 400 + (m <= 2 ? 1 : 0));
}
int daysIn(int y, int m) {
  static const uint8_t d[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  const bool leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
  return m == 2 && leap ? 29 : d[m - 1];
}

// DDHHMMZ -> unix: this month, else the previous one; 0 = not reconcilable
uint32_t reportTime(const Tok& t, uint32_t now) {
  const int day = num(t.p, 2), h = num(t.p + 2, 2), mi = num(t.p + 4, 2);
  if (!now || day < 1 || day > 31 || h > 23 || mi > 59) return 0;
  int y, m;
  civilFromDays((int64_t)(now / 86400u), y, m);
  for (int back = 0; back < 2; back++) {
    int yy = y, mm = m - back;
    if (mm < 1) mm = 12, yy--;
    if (day > daysIn(yy, mm)) continue;
    const int64_t at = unixFromUtc(yy, mm, day, h, mi, 0);
    if (back == 0 && at > (int64_t)now + 600) continue;  // a day later than today: last month's
    if (at > (int64_t)now + 600 || (int64_t)now - at > 86400) return 0;
    return (uint32_t)at;
  }
  return 0;
}

// speed in the report's unit -> km/h x10
uint32_t toDkmh(int v, int unit) { return unit == 0 ? ((uint32_t)v * 1852u + 50u) / 100u : unit == 1 ? (uint32_t)v * 36u : (uint32_t)v * 10u; }

// dddff(Gff)KT|MPS|KMH, VRBffKT, 00000KT, /////KT; false = not a wind group
bool windGroup(const Tok& t, MetarObs& o, uint8_t& bad) {
  int unit;
  size_t ul;
  if (endsWith(t, "KT")) unit = 0, ul = 2;
  else if (endsWith(t, "MPS")) unit = 1, ul = 3;
  else if (endsWith(t, "KMH")) unit = 2, ul = 3;
  else return false;
  const char* p = t.p;
  const size_t n = t.n - ul;
  if (n < 5) return false;
  int dir;  // -1 VRB, -2 unknown
  if (memcmp(p, "VRB", 3) == 0) dir = -1;
  else if (memcmp(p, "///", 3) == 0) dir = -2;
  else if (allD(p, 3)) dir = num(p, 3);
  else return false;
  size_t i = 3;
  while (i < n && isD(p[i])) i++;
  const size_t sl = i - 3;
  bool spUnknown = false;
  if (sl == 0) {
    if (i + 2 > n || p[i] != '/' || p[i + 1] != '/') return false;
    spUnknown = true;
    i += 2;
  } else if (sl < 2 || sl > 3) {
    return false;
  }
  int gust = -1;
  if (i < n) {
    if (p[i] != 'G') return false;
    const size_t g = ++i;
    while (i < n && isD(p[i])) i++;
    if (i != n || i - g < 2 || i - g > 3) return false;
    gust = num(p + g, i - g);
  }
  if (dir == -2 || spUnknown) return true;  // reported unknown: the fields stay unset
  if (dir > 360) {
    bad++;
    return true;
  }
  const uint32_t sp = toDkmh(num(p + 3, sl), unit);
  if (sp > 3000) {
    bad++;
    return true;
  }
  o.wind_dkmh = (uint16_t)sp;
  o.wdir = dir == -1 ? METAR_WDIR_VRB : (sp == 0 && dir == 0) ? METAR_WDIR_NONE : (uint16_t)dir;
  if (gust >= 0) {
    const uint32_t g = toDkmh(gust, unit);
    if (g < sp || g > 3000) bad++;
    else o.gust_dkmh = (uint16_t)g;
  }
  return true;
}

// COR, a delayed correction CCA..CCZ (Canada) or RTD (a late report): it may replace the report of its own time
bool isCorrection(const Tok& t) {
  if (eq(t, "COR") || eq(t, "RTD")) return true;
  return t.n == 3 && t.p[0] == 'C' && t.p[1] == 'C' && t.p[2] >= 'A' && t.p[2] <= 'Z';
}

bool isCompass(const char* p, size_t n) {
  static const char* const c[8] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};
  for (const char* s : c)
    if (strlen(s) == n && memcmp(s, p, n) == 0) return true;
  return false;
}

void setVis(MetarObs& o, uint32_t m, bool ge) {
  if (m >= 10000) m = 10000, ge = true;
  o.vis_m = (uint16_t)m;
  if (ge) o.flags |= MO_VIS_GE;
}

// statute miles: [P|M](n | n/d)SM, plus a whole part from the previous token (1 1/2SM); false = not an SM group
bool smGroup(const Tok& t, int whole, MetarObs& o, uint8_t& bad) {
  if (!endsWith(t, "SM") || t.n < 3) return false;
  const char* p = t.p;
  size_t n = t.n - 2;
  bool ge = false;
  if (p[0] == 'P') ge = true, p++, n--;
  else if (p[0] == 'M') p++, n--;
  const char* sl = (const char*)memchr(p, '/', n);
  uint32_t nu, de;
  if (!sl) {
    if (!allD(p, n) || n > 3) return false;  // 100SM (a mountain station) is >= 10 km
    nu = (uint32_t)num(p, n), de = 1;
  } else {
    const size_t a = (size_t)(sl - p), b = n - a - 1;
    if (!allD(p, a) || !allD(sl + 1, b) || a > 2 || b > 2) return false;
    nu = (uint32_t)num(p, a), de = (uint32_t)num(sl + 1, b);
    if (!de || nu >= de * 10) {
      bad++;
      return true;
    }
  }
  if (whole > 0) {
    if (!sl || nu >= de) {  // a mixed number has a proper fraction ("26 95/99SM" is nonsense)
      bad++;
      return true;
    }
    nu += (uint32_t)whole * de;
  }
  setVis(o, (uint32_t)(((uint64_t)nu * 1609344u + de * 500u) / (de * 1000u)), ge);  // 64-bit: never wraps
  return true;
}

// present weather: [-+VC|RE](descriptor|phenomenon)+; false = not a weather group
bool wxGroup(const Tok& t, MetarObs& o, bool& haveInt) {
  static const char kDesc[] = "MIBCPRDRBLSHTSFZ";
  static const char kPhen[] = "DZRASNSGICPLGRGSUPBRFGFUVADUSAHZPYPOSQFCSSDS";
  const char* p = t.p;
  size_t n = t.n;
  int inten = 0;
  bool ignore = false;
  if (n >= 2 && p[0] == 'R' && p[1] == 'E') ignore = true, p += 2, n -= 2;  // recent weather
  else if (n >= 2 && p[0] == 'V' && p[1] == 'C') ignore = true, p += 2, n -= 2;  // in the vicinity, not here
  else if (n && p[0] == '-') inten = -1, p++, n--;
  else if (n && p[0] == '+') inten = 1, p++, n--;
  if (!n || n % 2) return false;
  uint8_t bits = 0;
  for (size_t i = 0; i < n; i += 2) {
    bool known = false;
    for (size_t k = 0; k + 1 < sizeof kDesc && !known; k += 2) known = p[i] == kDesc[k] && p[i + 1] == kDesc[k + 1];
    for (size_t k = 0; k + 1 < sizeof kPhen && !known; k += 2) known = p[i] == kPhen[k] && p[i + 1] == kPhen[k + 1];
    if (!known) return false;
    const char a = p[i], b = p[i + 1];
    if (a == 'T' && b == 'S') bits |= MW_THUNDER;
    if (a == 'F' && b == 'Z') bits |= MW_FREEZING;
    if (a == 'F' && b == 'G') bits |= MW_FOG;  // fog and mist, drizzle and rain apart
    if (a == 'B' && b == 'R') bits |= MW_MIST;
    if (a == 'D' && b == 'Z') bits |= MW_DRIZZLE;
    if (a == 'R' && b == 'A') bits |= MW_RAIN;
    if ((a == 'S' && b == 'N') || (a == 'S' && b == 'G')) bits |= MW_SNOW;
    if ((a == 'D' && b == 'Z') || (a == 'R' && b == 'A') || (a == 'S' && b == 'N') || (a == 'S' && b == 'G') ||
        (a == 'I' && b == 'C') || (a == 'P' && b == 'L') || (a == 'G' && b == 'R') || (a == 'G' && b == 'S') ||
        (a == 'U' && b == 'P'))
      bits |= MW_PRECIP;
  }
  if (ignore) return true;
  o.wx |= bits;
  if (bits & MW_PRECIP) {
    if (!haveInt || inten > o.wx_int) o.wx_int = (int8_t)inten;
    haveInt = true;
  }
  return true;
}

// (FEW|SCT|BKN|OVC|VV)(ddd|///)(CB|TCU|///)?; false = not a cloud group
bool cloudGroup(const Tok& t, MetarObs& o) {
  uint8_t cls;
  size_t i;
  if (startsWith(t, "FEW")) cls = 1, i = 3;
  else if (startsWith(t, "SCT")) cls = 2, i = 3;
  else if (startsWith(t, "BKN")) cls = 3, i = 3;
  else if (startsWith(t, "OVC")) cls = 4, i = 3;
  else if (startsWith(t, "VV")) cls = 4, i = 2;
  else return false;
  if (t.n < i + 3) return false;
  int base = -1;
  if (allD(t.p + i, 3)) base = num(t.p + i, 3) * 100;
  else if (memcmp(t.p + i, "///", 3) != 0) return false;
  const Tok rest{t.p + i + 3, t.n - i - 3};
  const bool cb = eq(rest, "CB");
  if (rest.n && !cb && !eq(rest, "TCU") && !eq(rest, "///")) return false;
  if (o.cover == METAR_COVER_UNSET || cls > o.cover) o.cover = cls;
  if (cb) o.flags |= MO_CB;
  if (cls >= 3 && base >= 0 && (o.ceil_ft == METAR_U16_UNSET || base < o.ceil_ft)) o.ceil_ft = (uint16_t)base;
  return true;
}

// (M)dd or // or empty -> deci-degrees; false = not this form
bool tempPart(const char* p, size_t n, int& v, bool& missing) {
  missing = false;
  if (n == 0 || (n == 2 && p[0] == '/' && p[1] == '/')) {
    missing = true;
    return true;
  }
  bool neg = false;
  if (p[0] == 'M') neg = true, p++, n--;
  if (n != 2 || !allD(p, 2)) return false;
  v = num(p, 2) * 10 * (neg ? -1 : 1);
  return true;
}

// T/Td: (M)dd/(M)dd; false = not a temperature group
bool tempGroup(const Tok& t, MetarObs& o, uint8_t& bad) {
  const char* sl = (const char*)memchr(t.p, '/', t.n);
  if (!sl) return false;
  const size_t a = (size_t)(sl - t.p);
  int tv = 0, dv = 0;
  bool tm, dm;
  if (!tempPart(t.p, a, tv, tm) || !tempPart(sl + 1, t.n - a - 1, dv, dm)) return false;
  if (!tm) {
    if (tv < -800 || tv > 600) bad++;
    else o.t_dc = (int16_t)tv;
  }
  if (!dm) {
    if (dv < -800 || dv > 600 || (o.t_dc != METAR_I16_UNSET && dv > o.t_dc + 10)) bad++;
    else o.td_dc = (int16_t)dv;
  }
  return true;
}

// Qdddd hPa, Adddd inHg x100; false = not a pressure group
bool qnhGroup(const Tok& t, MetarObs& o, uint8_t& bad) {
  if (t.n != 5 || (t.p[0] != 'Q' && t.p[0] != 'A')) return false;
  if (memcmp(t.p + 1, "////", 4) == 0) return true;
  if (!allD(t.p + 1, 4)) return false;
  const uint32_t v = (uint32_t)num(t.p + 1, 4);
  const uint32_t d = t.p[0] == 'Q' ? v * 10u : (v * 338639u + 50000u) / 100000u;
  if (d < 8700 || d > 10850) bad++;
  else if (o.qnh_dhpa == METAR_I16_UNSET) o.qnh_dhpa = (int16_t)d;
  return true;
}

}  // namespace

const char* metarWhyName(MetarWhy w) {
  switch (w) {
    case MetarWhy::Ok: return "ok";
    case MetarWhy::Empty: return "empty";
    case MetarWhy::Garbage: return "garbage";
    case MetarWhy::Unwanted: return "unwanted";
    case MetarWhy::Nil: return "nil";
    case MetarWhy::Time: return "time";
  }
  return "?";
}

MetarResult metarParse(const char* line, size_t n, const char* const* want, int nwant, uint32_t now_unix, MetarObs& o) {
  memset(&o, 0, sizeof o);
  o.qnh_dhpa = o.t_dc = o.td_dc = METAR_I16_UNSET;
  o.wdir = METAR_WDIR_NONE;
  o.wind_dkmh = o.gust_dkmh = o.vis_m = o.ceil_ft = METAR_U16_UNSET;
  o.cover = METAR_COVER_UNSET;
  MetarResult r{MetarWhy::Garbage, 0};
  size_t pos = 0;
  Tok t;
  do {
    if (!nextTok(line, n, pos, t)) {
      r.why = MetarWhy::Empty;
      return r;
    }
  } while (!t.n);
  // header: METAR|SPECI [COR] CCCC DDHHMMZ, or CCCC DDHHMMZ directly (the tgftp form)
  if (eq(t, "METAR") || eq(t, "SPECI")) {
    if (t.p[0] == 'S') o.flags |= MO_SPECI;
    if (!nextTok(line, n, pos, t)) return r;
    if (isCorrection(t)) {
      o.flags |= MO_COR;
      if (!nextTok(line, n, pos, t)) return r;
    }
  }
  if (!isIcao(t)) return r;
  const Tok icao = t;
  if (!nextTok(line, n, pos, t) || !isTime(t)) return r;
  const Tok tm = t;
  bool wanted = false;
  for (int i = 0; i < nwant && !wanted; i++) wanted = want[i] && memcmp(want[i], icao.p, 4) == 0 && want[i][4] == 0;
  if (!wanted) {
    r.why = MetarWhy::Unwanted;
    return r;
  }
  memcpy(o.icao, icao.p, 4);
  const size_t afterTime = pos;
  if (nextTok(line, n, pos, t) && eq(t, "NIL")) {
    r.why = MetarWhy::Nil;
    return r;
  }
  pos = afterTime;
  const uint32_t at = reportTime(tm, now_unix);
  if (!at) {
    r.why = MetarWhy::Time;
    return r;
  }
  o.obs_unix = at;
  o.got_unix = now_unix;
  // groups, order-free, up to the remarks / trend
  bool wind = false, vis = false, haveInt = false, ncd = false, clr = false;
  int whole = 0;  // a 1-2 digit token just before an SM fraction (1 1/2SM)
  while (nextTok(line, n, pos, t)) {
    const int prevWhole = whole;
    whole = 0;
    if (!t.n) continue;
    if (eq(t, "RMK") || eq(t, "TEMPO") || eq(t, "BECMG") || eq(t, "NOSIG")) break;
    if (eq(t, "AUTO")) {
      o.flags |= MO_AUTO;
      continue;
    }
    if (isCorrection(t)) {  // COR, CCA..CCZ, RTD
      o.flags |= MO_COR;
      continue;
    }
    if (allSlash(t)) continue;  // a group reported missing
    if (!wind && windGroup(t, o, r.field_bad)) {
      wind = true;
      continue;
    }
    // a wind group we cannot read (E18010KT, 3006KT, 33007G12KTKT) is counted, never guessed
    if (!wind && (endsWith(t, "KT") || endsWith(t, "MPS") || endsWith(t, "KMH"))) {
      if (r.field_bad < 255) r.field_bad++;
      continue;
    }
    if (t.n == 7 && allD(t.p, 3) && t.p[3] == 'V' && allD(t.p + 4, 3)) continue;  // variable direction
    if (eq(t, "CAVOK")) {
      o.flags |= MO_CAVOK;
      setVis(o, 10000, true);
      vis = true;
      if (o.cover == METAR_COVER_UNSET) o.cover = 0;
      continue;
    }
    if (t.n >= 4 && allD(t.p, 4) && (t.n == 4 || eq(Tok{t.p + 4, t.n - 4}, "NDV"))) {
      if (!vis) {
        const uint32_t m = (uint32_t)num(t.p, 4);
        setVis(o, m == 9999 ? 10000 : m, m == 9999);
        vis = true;
      }
      continue;
    }
    if (t.n > 4 && allD(t.p, 4) && isCompass(t.p + 4, t.n - 4)) continue;  // directional minimum
    if (t.n <= 2 && allD(t.p, t.n)) {
      whole = num(t.p, t.n);
      continue;
    }
    if (t.n >= 3 && t.n <= 4 && endsWith(t, "KM") && allD(t.p, t.n - 2)) {  // whole km (14KM)
      if (!vis) {
        setVis(o, (uint32_t)num(t.p, t.n - 2) * 1000u, false);
        vis = true;
      }
      continue;
    }
    if (endsWith(t, "SM")) {
      if (!vis && smGroup(t, prevWhole, o, r.field_bad)) vis = true;
      continue;
    }
    if (t.n > 2 && t.p[0] == 'R' && isD(t.p[1]) && memchr(t.p, '/', t.n)) continue;  // runway visual range
    if (eq(t, "SKC") || eq(t, "CLR") || eq(t, "NSC") || eq(t, "NCD")) {
      if (o.cover == METAR_COVER_UNSET) o.cover = 0;
      ncd |= t.p[0] == 'N';
      clr |= t.p[0] == 'C';
      continue;
    }
    if (cloudGroup(t, o)) continue;
    if (qnhGroup(t, o, r.field_bad)) continue;
    if (tempGroup(t, o, r.field_bad)) continue;
    if (wxGroup(t, o, haveInt)) continue;
    // anything else (NSW, WS, sea-state groups, ...) is not read
  }
  // CLR is the automatic stations' "no cloud below 12 000 ft" (FMH-1); NCD / NSC in an AUTO report the same
  if (clr || (ncd && (o.flags & MO_AUTO))) o.flags |= MO_LOW_CONF;
  o.flags |= MO_VALID;
  r.why = MetarWhy::Ok;
  return r;
}

bool metarKeep(MetarObs* got, uint8_t& ngot, int cap, const MetarObs& o) {
  for (int i = 0; i < ngot; i++)
    if (!strcmp(got[i].icao, o.icao)) {
      if (o.obs_unix > got[i].obs_unix || (o.obs_unix == got[i].obs_unix && (o.flags & MO_COR))) got[i] = o;
      return true;
    }
  if (ngot >= cap) return false;
  got[ngot++] = o;
  return true;
}

void MetarLineSink::emit() {
  if (over_) trunc_++;
  buf_[len_] = 0;
  if (len_) fn_(ctx_, buf_, len_, over_);
  len_ = 0;
  over_ = false;
}

void MetarLineSink::feed(const char* d, size_t n) {
  for (size_t i = 0; i < n; i++) {
    const char c = d[i];
    if (c == '\n') {
      emit();
    } else if (c != '\r') {
      if (len_ < LINE_CAP - 1) buf_[len_++] = c;
      else over_ = true;
    }
  }
}

void MetarLineSink::finish() {
  if (len_ || over_) emit();
}

void MetarLineSink::drop() {
  if ((len_ || over_) && trunc_ < UINT16_MAX) trunc_++;
  len_ = 0;
  over_ = false;
}

}  // namespace fc
