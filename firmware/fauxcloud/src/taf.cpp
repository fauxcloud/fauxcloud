// taf.cpp -- see taf.h. Integer maths only; no heap.
#include "taf.h"

#include <string.h>

#include "wu_parse.h"  // unixFromUtc

namespace fc {

namespace {

bool isD(char c) { return c >= '0' && c <= '9'; }
bool isU(char c) { return c >= 'A' && c <= 'Z'; }
bool allD(const char* p, size_t n) {
  if (!n) return false;
  for (size_t i = 0; i < n; i++)
    if (!isD(p[i])) return false;
  return true;
}
int num(const char* p, size_t n) {
  int v = 0;
  for (size_t i = 0; i < n; i++) v = v * 10 + (p[i] - '0');
  return v;
}
bool eq(const char* p, size_t n, const char* s) { return strlen(s) == n && memcmp(p, s, n) == 0; }
bool ends(const char* p, size_t n, const char* s) {
  const size_t l = strlen(s);
  return n >= l && memcmp(p + n - l, s, l) == 0;
}

void civilFromDays(int64_t z, int& y, int& m) {  // H. Hinnant's algorithm
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

// The issue time DDHHMMZ: this month or the last, <= 10 min ahead, <= 40 h old; 0 = not reconcilable
uint32_t issueTime(const char* p, uint32_t now) {
  const int day = num(p, 2), h = num(p + 2, 2), mi = num(p + 4, 2);
  if (!now || day < 1 || day > 31 || h > 23 || mi > 59) return 0;
  int y, m;
  civilFromDays((int64_t)(now / 86400u), y, m);
  for (int back = 0; back < 2; back++) {
    int yy = y, mm = m - back;
    if (mm < 1) mm = 12, yy--;
    if (day > daysIn(yy, mm)) continue;
    const int64_t at = unixFromUtc(yy, mm, day, h, mi, 0);
    if (back == 0 && at > (int64_t)now + 600) continue;
    if (at > (int64_t)now + 600 || (int64_t)now - at > (int64_t)TAF_MAX_AGE_S) return 0;
    return (uint32_t)at;
  }
  return 0;
}

bool isPeriod(const char* p, size_t n) { return n == 9 && allD(p, 4) && p[4] == '/' && allD(p + 5, 4); }

// the bits a state shows as "expected"; "possible" is the next bit up
uint16_t eBits(const TafParser::State& s) {
  uint16_t b = 0;
  if (s.wx & 1) b |= TB_E_TS;
  if (s.wx & 2) b |= TB_E_SN;
  if (s.wx & 4) b |= TB_E_RA;
  if ((s.wx & 8) && s.vis_m < 1000) b |= TB_E_FG;
  if (s.gust) b |= TB_E_GU;
  return b;
}

// present weather: [-+](descriptor|phenomenon)+, VC / RE ignored; bits 1 ts, 2 sn, 4 ra, 8 fog weather. -1 = not weather
int wxBits(const char* p, size_t n) {
  static const char kDesc[] = "MIBCPRDRBLSHTSFZ";
  static const char kPhen[] = "DZRASNSGICPLGRGSUPBRFGFUVADUSAHZPYPOSQFCSSDS";
  if (n >= 2 && ((p[0] == 'V' && p[1] == 'C') || (p[0] == 'R' && p[1] == 'E'))) return 0;  // vicinity / recent: not here
  if (n && (p[0] == '-' || p[0] == '+')) p++, n--;
  if (!n || n % 2) return -1;
  int b = 0;
  for (size_t i = 0; i < n; i += 2) {
    bool known = false;
    for (size_t k = 0; k + 1 < sizeof kDesc && !known; k += 2) known = p[i] == kDesc[k] && p[i + 1] == kDesc[k + 1];
    for (size_t k = 0; k + 1 < sizeof kPhen && !known; k += 2) known = p[i] == kPhen[k] && p[i + 1] == kPhen[k + 1];
    if (!known) return -1;
    const char a = p[i], c = p[i + 1];
    if (a == 'T' && c == 'S') b |= 1;
    if ((a == 'S' && c == 'N') || (a == 'S' && c == 'G') || (a == 'P' && c == 'L')) b |= 2;
    if ((a == 'R' && c == 'A') || (a == 'D' && c == 'Z') || (a == 'U' && c == 'P') || (a == 'G' && c == 'R') ||
        (a == 'G' && c == 'S') || (a == 'I' && c == 'C'))
      b |= 4;
    if ((a == 'F' && c == 'G') || (a == 'B' && c == 'R')) b |= 8;
  }
  return b;
}

// dddff[Gff]KT|MPS|KMH or VRBff..; true = a wind group (gust: its G is significant)
bool windTok(const char* p, size_t n, bool* gust) {
  int unit = -1;
  size_t ul = 0;
  if (ends(p, n, "KT")) unit = 0, ul = 2;
  else if (ends(p, n, "MPS")) unit = 1, ul = 3;
  else if (ends(p, n, "KMH")) unit = 2, ul = 3;
  if (unit < 0 || n < ul + 5) return false;
  const size_t body = n - ul;
  if (!(allD(p, 3) || memcmp(p, "VRB", 3) == 0)) return false;
  size_t i = 3, s = i;
  while (i < body && isD(p[i])) i++;
  if (i - s < 2 || i - s > 3) return false;
  *gust = false;
  if (i < body) {
    if (p[i] != 'G') return false;
    const size_t g = ++i;
    while (i < body && isD(p[i])) i++;
    if (i != body || i - g < 2 || i - g > 3) return false;
    const int gv = num(p + g, i - g);
    *gust = unit == 0 ? gv >= 22 : unit == 1 ? gv >= 12 : gv >= 40;  // 40 km/h in the group's own unit
  }
  return true;
}

}  // namespace

const char* tafWhatName(TafWhat w) {
  static const char* const N[] = {"thunder", "snow", "rain", "fog", "gusts"};
  return (unsigned)w < (unsigned)TafWhat::COUNT ? N[(int)w] : "";
}
const char* tafStatusName(TafStatus s) {
  static const char* const N[] = {"none", "ok", "nil", "cnl", "absent"};
  return (unsigned)s < 5 ? N[(int)s] : "";
}

bool tafCurrent(const TafObs& t, uint32_t now) {
  return t.status == (uint8_t)TafStatus::Ok && now < t.valid_to && now < t.grid_unix + TAF_SHOW_S && now + 3600u > t.grid_unix;
}
uint32_t tafHorizon(const TafObs& t) {
  const uint32_t g = t.grid_unix + (uint32_t)TAF_HOURS * 3600u;
  return t.valid_to < g ? t.valid_to : g;
}

int tafWindows(const TafObs& t, uint32_t now, TafWindow* out, int max) {
  if (!tafCurrent(t, now)) return 0;
  const uint32_t edge = now + 12u * 3600u < t.valid_to ? now + 12u * 3600u : t.valid_to;
  // where the cells stop: the 12-h edge, or the grid's end when a cycle was missed (shown up to TAF_SHOW_S late)
  const uint32_t hz = tafHorizon(t), stop = edge < hz ? edge : hz;
  int k = 0;
  for (int w = 0; w < (int)TafWhat::COUNT; w++) {
    const uint16_t e = (uint16_t)(1u << (2 * w)), pb = (uint16_t)(e << 1);
    for (int pass = 0; pass < 2; pass++) {  // expected runs, then possible runs where not expected
      bool in = false, started = false;
      uint32_t from = 0;
      for (int i = 0; i <= TAF_HOURS; i++) {
        const uint32_t c0 = t.grid_unix + (uint32_t)i * 3600u;
        bool on = false;
        if (i < TAF_HOURS && c0 + 3600u > now && c0 < edge) on = pass == 0 ? (t.cell[i] & e) : ((t.cell[i] & pb) && !(t.cell[i] & e));
        if (on && !in) in = true, started = c0 <= now, from = started ? now : c0;
        if (!on && in) {
          in = false;
          const uint32_t to = c0 < edge ? c0 : edge;
          if (k < max && to > from) out[k++] = TafWindow{(TafWhat)w, pass == 1, from, to, to >= stop && to != t.valid_to, started};
        }
      }
    }
  }
  // inside each phenomenon: by time (the expected and possible runs interleave)
  for (int i = 1; i < k; i++)
    for (int j = i; j > 0 && out[j].what == out[j - 1].what && out[j].from < out[j - 1].from; j--) {
      const TafWindow x = out[j];
      out[j] = out[j - 1], out[j - 1] = x;
    }
  return k;
}

TafParser::TafParser(const char* const* want, int nwant, uint32_t now)
    : want_(want), nwant_(nwant), now_(now), open_(false), stop_(false), pending_prob_(false), stage_(0), cur_(), st_(),
      from_(0), g_(), have_g_(false), vfrom_(0), got_(), n_(0), garbage_(0), other_(0), unwanted_(0) {}

uint32_t TafParser::ddhh(int dd, int hh, int mm) const {
  if (dd < 1 || dd > 31 || hh < 0 || hh > 24 || mm < 0 || mm > 59 || (hh == 24 && mm)) return 0;  // 24:00 is the only hour 24
  const uint32_t ref = cur_.issue_unix;
  int y, m;
  civilFromDays((int64_t)(ref / 86400u), y, m);
  int64_t best = -1;
  for (int k = -1; k <= 1; k++) {  // the first such time from a day before the issue (month and year roll)
    int yy = y, mo = m + k;
    if (mo < 1) mo = 12, yy--;
    if (mo > 12) mo = 1, yy++;
    if (dd < 1 || dd > daysIn(yy, mo)) continue;
    const int64_t t = unixFromUtc(yy, mo, dd, 0, 0, 0) + (int64_t)hh * 3600 + (int64_t)mm * 60;  // hh 24 = the next day
    if (t < (int64_t)ref - 86400) continue;
    if (best < 0 || t < best) best = t;
  }
  return best < 0 ? 0 : (uint32_t)best;
}

void TafParser::paint(uint32_t a, uint32_t b, uint16_t bits) {
  if (!bits) return;
  if (a < vfrom_) a = vfrom_;
  if (b > cur_.valid_to) b = cur_.valid_to;
  if (b <= a) return;
  const uint32_t g = cur_.grid_unix;
  if (b <= g) return;
  const uint32_t from = a > g ? (a - g) / 3600u : 0, to = (b - g + 3599u) / 3600u;  // outward to whole hours
  for (uint32_t i = from; i < to && i < (uint32_t)TAF_HOURS; i++) cur_.cell[i] |= bits;
}

void TafParser::closeGroup() {
  if (!have_g_) return;
  have_g_ = false;
  const Group& g = g_;
  State s = st_;
  if (g.kind == 0 || g.kind == 1) {  // the base, FM: everything replaced (a missing weather group = none)
    if (g.kind == 1) {
      if (!g.a) return;
      paint(from_, g.a, eBits(st_));
      from_ = g.a;
    } else {
      from_ = vfrom_;
    }
    st_.wx = g.cavok || g.nsw ? 0 : g.wx;
    st_.gust = g.has_wind && g.gust;
    st_.vis_m = g.cavok ? 10000 : g.has_vis ? g.vis_m : 10000;
    return;
  }
  if (!g.a || !g.b) return;  // a change group without its period
  if (g.kind == 2) {  // BECMG: what it states changes, from the period's end; during it the new state is possible
    if (g.has_wind) s.gust = g.gust;
    if (g.has_vis) s.vis_m = g.vis_m;
    if (g.cavok) s.vis_m = 10000, s.wx = 0;
    if (g.nsw) s.wx = 0;
    if (g.has_wx) s.wx = g.wx;
    paint(from_, g.b, eBits(st_));
    paint(g.a, g.b, (uint16_t)(eBits(s) << 1));
    st_ = s;
    from_ = g.b > from_ ? g.b : from_;
    return;
  }
  // TEMPO / INTER / PROBnn: an overlay of what the group itself states, "possible"; the state does not change
  State t{0, false, st_.vis_m};
  if (g.has_wx) t.wx = g.wx;
  if (g.has_wind) t.gust = g.gust;
  if (g.has_vis) t.vis_m = g.vis_m;
  if (g.cavok) t.vis_m = 10000, t.wx = 0;
  paint(g.a, g.b, (uint16_t)(eBits(t) << 1));
}

void TafParser::endBlock() {
  if (!open_) return;
  open_ = false;
  if (stage_ == 0 && cur_.status == (uint8_t)TafStatus::Ok) {  // a header with no validity is no forecast
    garbage_++;
    return;
  }
  if (stage_ == 1) {
    closeGroup();
    paint(from_, cur_.valid_to, eBits(st_));
  }
  for (int i = 0; i < n_; i++)
    if (!strcmp(got_[i].icao, cur_.icao)) {  // the same airport twice: the later issue wins
      if (cur_.issue_unix >= got_[i].issue_unix) got_[i] = cur_;
      return;
    }
  if (n_ < 3) got_[n_++] = cur_;
}

void TafParser::startBlock(const char* l, size_t n) {
  endBlock();
  stop_ = true;  // until the header reads
  size_t pos = 0;
  const char* tk[8];
  size_t tl[8];
  int nt = 0;
  while (pos < n && nt < 8) {  // the first few tokens
    while (pos < n && l[pos] == ' ') pos++;
    if (pos >= n) break;
    const size_t s = pos;
    while (pos < n && l[pos] != ' ') pos++;
    tk[nt] = l + s, tl[nt] = pos - s;
    while (tl[nt] && tk[nt][tl[nt] - 1] == '=') tl[nt]--;
    nt++;
  }
  int i = 1;  // tk[0] = "TAF"
  while (i < nt && (eq(tk[i], tl[i], "AMD") || eq(tk[i], tl[i], "COR") || eq(tk[i], tl[i], "RTD") ||
                    (tl[i] == 3 && tk[i][0] == 'C' && tk[i][1] == 'C' && isU(tk[i][2]))))
    i++;
  if (i + 1 >= nt || tl[i] != 4 || !isU(tk[i][0])) {
    garbage_++;
    return;
  }
  bool wanted = false;
  for (int k = 0; k < nwant_ && !wanted; k++) wanted = want_[k] && memcmp(want_[k], tk[i], 4) == 0 && want_[k][4] == 0;
  if (!wanted) {
    unwanted_++;
    return;
  }
  const char* tp = tk[i + 1];
  const uint32_t at = tl[i + 1] == 7 && allD(tp, 6) && tp[6] == 'Z' ? issueTime(tp, now_) : 0;
  if (!at) {
    garbage_++;
    return;
  }
  memset(&cur_, 0, sizeof cur_);
  memcpy(cur_.icao, tk[i], 4);
  cur_.issue_unix = at;
  cur_.grid_unix = now_ - now_ % 3600u;
  cur_.status = (uint8_t)TafStatus::Ok;
  st_ = State{0, false, 10000};
  have_g_ = false;
  pending_prob_ = false;
  stage_ = 0;
  open_ = true;
  stop_ = false;
  // the rest of the line as tokens (the validity, NIL / CNL, the base group)
  const size_t rest = (size_t)(tp + tl[i + 1] - l);
  pos = rest;
  while (pos < n && !stop_) {
    while (pos < n && l[pos] == ' ') pos++;
    if (pos >= n) break;
    const size_t s = pos;
    while (pos < n && l[pos] != ' ') pos++;
    size_t len = pos - s;
    while (len && l[s + len - 1] == '=') len--;
    if (len) token(l + s, len);
  }
}

void TafParser::token(const char* p, size_t n) {
  if (stop_ || !open_) return;
  if (stage_ == 0) {  // NIL, or the validity (then CNL or the base group)
    if (eq(p, n, "NIL")) {
      cur_.status = (uint8_t)TafStatus::Nil;
      stop_ = true;
      return;
    }
    if (!isPeriod(p, n)) {
      garbage_++;
      open_ = false;
      stop_ = true;
      return;
    }
    vfrom_ = ddhh(num(p, 2), num(p + 2, 2), 0);
    cur_.valid_to = ddhh(num(p + 5, 2), num(p + 7, 2), 0);
    if (!vfrom_ || !cur_.valid_to || cur_.valid_to <= vfrom_) {
      garbage_++;
      open_ = false;
      stop_ = true;
      return;
    }
    stage_ = 1;
    g_ = Group();
    g_.kind = 0;
    g_.vis_m = 10000;
    have_g_ = true;
    return;
  }
  if (eq(p, n, "CNL")) {
    cur_.status = (uint8_t)TafStatus::Cnl;
    stage_ = 0;
    stop_ = true;
    return;
  }
  if (eq(p, n, "RMK")) {
    closeGroup();
    stop_ = true;  // the remarks end what we read; endBlock paints the last state
    stage_ = 1;
    return;
  }
  const bool fm = n == 8 && p[0] == 'F' && p[1] == 'M' && allD(p + 2, 6);
  if (fm || eq(p, n, "BECMG") || eq(p, n, "TEMPO") || eq(p, n, "INTER") || eq(p, n, "PROB30") || eq(p, n, "PROB40")) {
    const bool tempo = eq(p, n, "TEMPO") || eq(p, n, "INTER");
    if (tempo && have_g_ && g_.kind == 4 && !g_.a) {  // PROBnn TEMPO (AWC: the PROB on its own line before it)
      g_.kind = 3, g_.prob = true;
      return;
    }
    closeGroup();
    g_ = Group();
    g_.vis_m = 10000;
    have_g_ = true;
    if (fm) g_.kind = 1, g_.a = ddhh(num(p + 2, 2), num(p + 4, 2), num(p + 6, 2));
    else if (eq(p, n, "BECMG")) g_.kind = 2;
    else if (tempo) g_.kind = 3;
    else g_.kind = 4;
    return;
  }
  Group& g = g_;
  if (!have_g_) return;
  if (isPeriod(p, n) && g.kind >= 2 && !g.a) {
    g.a = ddhh(num(p, 2), num(p + 2, 2), 0);
    g.b = ddhh(num(p + 5, 2), num(p + 7, 2), 0);
    if (g.b <= g.a) g.a = g.b = 0;
    return;
  }
  bool gust = false;
  if (windTok(p, n, &gust)) {
    g.has_wind = true, g.gust = gust;
    return;
  }
  if (eq(p, n, "CAVOK")) {
    g.cavok = true;
    return;
  }
  if (eq(p, n, "NSW")) {
    g.nsw = true;
    return;
  }
  if (n == 4 && allD(p, 4)) {  // visibility in metres (9999 = 10 km or more)
    const int v = num(p, 4);
    g.has_vis = true, g.vis_m = (uint16_t)(v >= 9999 ? 10000 : v);
    return;
  }
  if (n <= 2 && allD(p, n)) {  // the whole part of "1 1/2SM"
    g.whole = num(p, n);
    return;
  }
  if (ends(p, n, "SM") && n >= 3) {  // statute miles: P6SM, 3SM, 1/2SM, M1/4SM (after a whole part)
    const char* q = p;
    size_t m = n - 2;
    bool more = false;
    if (q[0] == 'P') more = true, q++, m--;
    else if (q[0] == 'M') q++, m--;
    const char* sl = (const char*)memchr(q, '/', m);
    uint32_t metres;
    if (more) {
      metres = 10000;
    } else if (!sl) {
      if (!allD(q, m) || m > 3) return;
      metres = (uint32_t)num(q, m) * 1609u;
    } else {
      const size_t a = (size_t)(sl - q), b = m - a - 1;
      if (!allD(q, a) || !allD(sl + 1, b) || a > 2 || b > 2) return;
      const uint32_t de = (uint32_t)num(sl + 1, b);
      if (!de) return;
      metres = (uint32_t)(((uint64_t)g.whole * de + (uint32_t)num(q, a)) * 1609u / de);
    }
    g.has_vis = true, g.vis_m = (uint16_t)(metres > 10000 ? 10000 : metres);
    g.whole = 0;
    return;
  }
  const int wx = wxBits(p, n);
  if (wx >= 0) {  // HZ, FU, VCSH ... restate the weather too (a BECMG with them ends earlier rain)
    g.has_wx = true, g.wx = (uint8_t)(g.wx | wx);
    return;
  }
  // anything else (clouds, TX / TN, QNH...INS, WS..., 5xxxxx) changes nothing
}

void TafParser::line(const char* l, size_t n, bool truncated) {
  if (truncated) {  // a line over the sink's 191 B lost its tail: its block is not trusted
    if (l[0] == ' ') {
      if (!open_ || stop_) return;  // after RMK / NIL (or no block): nothing of it is read anyway
      open_ = false;                // the open block is dropped, not committed
      stop_ = true;
      garbage_++;
      return;
    }
    endBlock();  // any other line ends the open block, as an uncut one does; a cut TAF header opens nothing
    if (n >= 5 && (memcmp(l, "METAR", 5) == 0 || memcmp(l, "SPECI", 5) == 0)) other_++;
    else garbage_++;
    return;
  }
  while (n && (l[n - 1] == ' ' || l[n - 1] == '\r')) n--;  // AWC's trailing spaces
  if (!n) return;
  if (l[0] == ' ') {  // a continuation of the open block
    if (!open_) return;
    size_t pos = 0;
    while (pos < n && !stop_) {
      while (pos < n && l[pos] == ' ') pos++;
      if (pos >= n) break;
      const size_t s = pos;
      while (pos < n && l[pos] != ' ') pos++;
      size_t len = pos - s;
      while (len && l[s + len - 1] == '=') len--;
      if (len) token(l + s, len);
    }
    return;
  }
  if (n >= 3 && memcmp(l, "TAF", 3) == 0 && (n == 3 || l[3] == ' ')) {
    startBlock(l, n);
    return;
  }
  endBlock();
  if ((n >= 5 && (memcmp(l, "METAR", 5) == 0 || memcmp(l, "SPECI", 5) == 0))) other_++;
  else garbage_++;
}

void TafParser::finish() { endBlock(); }

}  // namespace fc
