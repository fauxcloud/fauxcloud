// airport.cpp -- see airport.h.
#include "airport.h"

#include <string.h>

#include "json.h"

namespace fc {

namespace {
// round(cos(d) * 32768), d = 0..90 degrees
const uint16_t kCosQ15[91] = {
    32768, 32763, 32748, 32723, 32688, 32643, 32588, 32524, 32449, 32365, 32270, 32166, 32052, 31928, 31795, 31651,
    31499, 31336, 31164, 30983, 30792, 30592, 30382, 30163, 29935, 29698, 29452, 29197, 28932, 28660, 28378, 28088,
    27789, 27482, 27166, 26842, 26510, 26170, 25822, 25466, 25102, 24730, 24351, 23965, 23571, 23170, 22763, 22348,
    21926, 21498, 21063, 20622, 20174, 19720, 19261, 18795, 18324, 17847, 17364, 16877, 16384, 15886, 15384, 14876,
    14365, 13848, 13328, 12803, 12275, 11743, 11207, 10668, 10126, 9580,  9032,  8481,  7927,  7371,  6813,  6252,
    5690,  5126,  4560,  3993,  3425,  2856,  2286,  1715,  1144,  572,   0};

uint32_t cosQ15(int32_t centi) {  // |latitude| in centi-degrees, 0..9000
  if (centi < 0) centi = -centi;
  if (centi >= 9000) return 0;
  const int32_t i = centi / 100, f = centi % 100;
  return (uint32_t)((int32_t)kCosQ15[i] + ((int32_t)kCosQ15[i + 1] - (int32_t)kCosQ15[i]) * f / 100);
}

uint32_t isqrt64(uint64_t v) {
  uint64_t r = 0, bit = (uint64_t)1 << 62;
  while (bit > v) bit >>= 2;
  while (bit) {
    if (v >= r + bit) {
      v -= r + bit;
      r = (r >> 1) + bit;
    } else {
      r >>= 1;
    }
    bit >>= 2;
  }
  return (uint32_t)r;
}

bool isIcao4(const char* s) {
  if (strlen(s) != 4 || s[0] < 'A' || s[0] > 'Z') return false;
  for (int i = 1; i < 4; i++)
    if (!((s[i] >= 'A' && s[i] <= 'Z') || (s[i] >= '0' && s[i] <= '9'))) return false;
  return true;
}
}  // namespace

uint32_t geoKm(int32_t lat1, int32_t lon1, int32_t lat2, int32_t lon2) {
  // in milli-degrees, rounded at each step: centi-degree truncation alone cost up to 2 km
  const int64_t dlat = ((int64_t)lat2 - lat1) * 10;
  int64_t dlon = (int64_t)lon2 - lon1;
  while (dlon > 18000) dlon -= 36000;
  while (dlon < -18000) dlon += 36000;
  const int64_t xs = dlon * 10 * (int64_t)cosQ15((lat1 + lat2) / 2);
  const int64_t x = (xs >= 0 ? xs + 16384 : xs - 16384) / 32768;
  const uint64_t v = (uint64_t)(x * x + dlat * dlat);
  uint64_t r = isqrt64(v);
  if (v - r * r > r) r++;  // to the nearest
  return (uint32_t)((r * 1112u + 5000u) / 10000u);  // 0.001 deg = 0.1112 km
}

bool centiFromText(const char* s, int32_t* out) {
  bool neg = false;
  if (*s == '-') neg = true, s++;
  if (*s < '0' || *s > '9') return false;
  int64_t whole = 0;
  for (; *s >= '0' && *s <= '9'; s++) {
    whole = whole * 10 + (*s - '0');
    if (whole > 100000) return false;
  }
  int digits[3] = {0, 0, 0};
  if (*s == '.') {
    s++;
    if (*s < '0' || *s > '9') return false;
    for (int k = 0; *s >= '0' && *s <= '9'; s++, k++)
      if (k < 3) digits[k] = *s - '0';
  }
  if (*s) return false;  // an exponent or anything else
  int64_t v = whole * 100 + digits[0] * 10 + digits[1] + (digits[2] >= 5 ? 1 : 0);
  *out = (int32_t)(neg ? -v : v);
  return true;
}

// the nearest multiple of AIRPORT_BOX_SNAP (half away from zero)
static int32_t snapGrid(int32_t v) {
  const int32_t h = AIRPORT_BOX_SNAP / 2;
  return (v >= 0 ? (v + h) / AIRPORT_BOX_SNAP : -((-v + h) / AIRPORT_BOX_SNAP)) * AIRPORT_BOX_SNAP;
}

// the request carries a rough area, never the location -- the box is centred on the 0.1-deg grid point
// nearest to it (about 10 km) and widened by 0.1 deg, so the snap cannot cost an airport; distances and the nearest
// five are computed here, from the exact location
void airportBox(int32_t lat, int32_t lon, int32_t box[4]) {
  lat = snapGrid(lat), lon = snapGrid(lon);
  const uint32_t c = cosQ15(lat);
  int32_t dlon = c ? (int32_t)((int64_t)AIRPORT_BOX_LAT * 32768 / c) : AIRPORT_BOX_LON_MAX;
  if (dlon > AIRPORT_BOX_LON_MAX) dlon = AIRPORT_BOX_LON_MAX;
  dlon += AIRPORT_BOX_SNAP;
  const int32_t dlat = AIRPORT_BOX_LAT + AIRPORT_BOX_SNAP;
  box[0] = lat - dlat < -9000 ? -9000 : lat - dlat;
  box[1] = lon - dlon < -18000 ? -18000 : lon - dlon;
  box[2] = lat + dlat > 9000 ? 9000 : lat + dlat;
  box[3] = lon + dlon > 18000 ? 18000 : lon + dlon;
}

// the ICAO-prefix table, packed: 6 bytes a row -- the prefix padded with spaces to 3, the country (ISO 3166 alpha-2),
// its flag pack as one digit. Longest prefixes first (the first match wins), then by prefix. Generated from a prefix
// list and tz.json; test_table_sync keeps it equal to the flag packs. 134 rows, 805 B of flash.
// clang-format off
static const char ICAO_CC[] =
    "EKV" "FO6" "LYP" "ME0" "LYT" "ME0" "UMK" "RU0" "UTA" "TM1" "UTD" "TJ1" "WBS" "BN1" "BG " "GL3" "BI " "IS6"
    "DA " "DZ2" "DG " "GH2" "DN " "NG2" "DT " "TN2" "EB " "BE0" "ED " "DE0" "EE " "EE0" "EF " "FI0" "EG " "GB0"
    "EH " "NL0" "EI " "IE0" "EK " "DK0" "EL " "LU0" "EN " "NO0" "EP " "PL0" "ES " "SE0" "ET " "DE0" "EV " "LV0"
    "EY " "LT0" "FA " "ZA2" "FB " "BW2" "FY " "NA2" "GC " "ES0" "GM " "MA2" "GO " "SN2" "HA " "ET2" "HE " "EG2"
    "HK " "KE2" "HT " "TZ2" "LA " "AL0" "LB " "BG0" "LC " "CY1" "LD " "HR0" "LE " "ES0" "LF " "FR0" "LG " "GR0"
    "LH " "HU0" "LI " "IT0" "LJ " "SI0" "LK " "CZ0" "LL " "IL1" "LM " "MT0" "LO " "AT0" "LP " "PT0" "LQ " "BA0"
    "LR " "RO0" "LS " "CH0" "LT " "TR0" "LU " "MD0" "LW " "MK0" "LX " "GI0" "LY " "RS0" "LZ " "SK0" "MD " "DO3"
    "MK " "JM3" "MM " "MX3" "MU " "CU3" "NZ " "NZ5" "OB " "BH1" "OE " "SA1" "OI " "IR1" "OJ " "JO1" "OK " "KW1"
    "OL " "LB1" "OM " "AE1" "OO " "OM1" "OP " "PK1" "OR " "IQ1" "OT " "QA1" "PA " "US3" "PH " "US3" "RC " "TW1"
    "RJ " "JP1" "RK " "KR1" "RO " "JP1" "RP " "PH1" "SA " "AR3" "SB " "BR3" "SC " "CL3" "SE " "EC3" "SG " "PY3"
    "SK " "CO3" "SL " "BO3" "SP " "PE3" "SU " "UY3" "SV " "VE3" "TJ " "PR3" "UA " "KZ1" "UB " "AZ1" "UD " "AM1"
    "UE " "RU0" "UG " "GE1" "UH " "RU0" "UI " "RU0" "UK " "UA0" "UL " "RU0" "UM " "BY0" "UN " "RU0" "UO " "RU0"
    "UR " "RU0" "US " "RU0" "UT " "UZ1" "UU " "RU0" "UW " "RU0" "VA " "IN1" "VC " "LK1" "VE " "IN1" "VG " "BD1"
    "VH " "HK1" "VI " "IN1" "VM " "MO1" "VO " "IN1" "VT " "TH1" "VV " "VN1" "WA " "ID1" "WB " "MY1" "WI " "ID1"
    "WM " "MY1" "WS " "SG1" "ZK " "KP1" "ZM " "MN1" "C  " "CA3" "K  " "US3" "Y  " "AU4" "Z  " "CN1";
// clang-format on

bool airportCountry(const char* icao, char cc[3], uint8_t* pack) {
  for (const char* r = ICAO_CC; *r; r += 6) {
    const size_t n = r[2] != ' ' ? 3 : r[1] != ' ' ? 2 : 1;
    if (!strncmp(icao, r, n)) {
      cc[0] = r[3], cc[1] = r[4], cc[2] = 0;
      if (pack) *pack = (uint8_t)(r[5] - '0');
      return true;
    }
  }
  cc[0] = 0;
  if (pack) *pack = AIRPORT_PACK_NONE;
  return false;
}

void airportShortName(char* s) {
  static const char* const NOISE[] = {"Intl", "Arpt", "Airport", "International"};
  // a name of exactly 24 bytes may be an older stored, cut site text ("London/Heathrow Internat"): its last word, when it
  // is a proper prefix (>= 3 chars) of a noise word, is the cut noise word and goes too
  const bool maybeCut = strlen(s) == sizeof(Airport::name) - 1;
  const char* slash = strrchr(s, '/');
  const char* p = slash && slash[1] ? slash + 1 : s;
  char out[64];
  size_t o = 0;
  while (*p) {
    while (*p == ' ') p++;
    const char* w = p;
    while (*p && *p != ' ') p++;
    const size_t n = (size_t)(p - w);
    if (!n) break;
    bool noise = false;
    const char* q = p;
    while (*q == ' ') q++;
    const bool last = !*q;
    for (const char* x : NOISE) noise |= (strlen(x) == n || (maybeCut && last && n >= 3 && n < strlen(x))) && !strncmp(w, x, n);
    if (noise) continue;
    if (o && o < sizeof out - 1) out[o++] = ' ';
    const size_t c = n < sizeof out - 1 - o ? n : sizeof out - 1 - o;
    memcpy(out + o, w, c);
    o += c;
  }
  out[o] = 0;
  if (o) memcpy(s, out, o + 1);
}

bool airportFromJson(const char* obj, size_t n, Airport* out, bool* has_taf) {
  memset(out, 0, sizeof *out);
  if (has_taf) *has_taf = false;
  out->km = AIRPORT_KM_UNKNOWN;
  JsonReader r(obj, n);
  if (!r.beginObject()) return false;
  char key[16], val[64];
  bool metar = false, haveLat = false, haveLon = false;
  int32_t lat = 0, lon = 0;
  while (r.nextKey(key, sizeof key)) {
    const bool str = r.peek() == '"';
    if (!strcmp(key, "icaoId") && str) {
      if (!r.readString(val, sizeof val)) return false;
      if (strlen(val) < sizeof out->icao) memcpy(out->icao, val, strlen(val) + 1);
    } else if (!strcmp(key, "site") && str) {
      if (!r.readString(val, sizeof val)) return false;
      airportShortName(val);  // shorten the full text first, then cut (the useful part survives)
      utf8Truncate(val, sizeof out->name - 1);
      memcpy(out->name, val, strlen(val) + 1);
    } else if ((!strcmp(key, "lat") || !strcmp(key, "lon")) && r.peek() != 'n') {
      char t[24];
      int32_t v;
      if (!r.readNumberText(t, sizeof t) || !centiFromText(t, &v)) return false;
      if (key[1] == 'a') lat = v, haveLat = true;
      else lon = v, haveLon = true;
    } else if (!strcmp(key, "siteType") && r.peek() == '[') {
      if (!r.beginArray()) return false;
      while (r.nextElement()) {
        if (r.peek() == '"') {
          if (!r.readString(val, sizeof val)) return false;
          metar |= !strcmp(val, "METAR");
          if (has_taf && !strcmp(val, "TAF")) *has_taf = true;
        } else if (!r.skipValue()) {
          return false;
        }
      }
      if (r.failed()) return false;
    } else if (!r.skipValue()) {
      return false;
    }
  }
  if (r.failed() || !metar || !haveLat || !haveLon || !isIcao4(out->icao)) return false;
  if (lat < -9000 || lat > 9000 || lon < -18000 || lon > 18000) return false;
  out->lat = (int16_t)lat;
  out->lon = (int16_t)lon;
  return true;
}

AirportJsonSink::AirportJsonSink(bool have_ref, int32_t lat, int32_t lon)
    : have_ref_(have_ref), lat_(lat), lon_(lon), len_(0), depth_(0), array_(false), in_str_(false), esc_(false),
      over_(false), seen_(0), dropped_(0), rejected_(0), taf_(), n_(0) {}

void AirportJsonSink::object() {
  seen_++;
  Airport a;
  bool taf = false;
  if (!airportFromJson(buf_, len_, &a, &taf)) {
    rejected_++;
    return;
  }
  if (have_ref_) a.km = (uint16_t)geoKm(lat_, lon_, a.lat, a.lon);
  int at = n_;
  if (have_ref_)
    while (at > 0 && best_[at - 1].km > a.km) at--;  // after equal distances: the first seen stays first
  if (at >= AIRPORT_NEAR_MAX) return;
  const int last = n_ < AIRPORT_NEAR_MAX ? n_ : AIRPORT_NEAR_MAX - 1;
  for (int k = last; k > at; k--) best_[k] = best_[k - 1], taf_[k] = taf_[k - 1];
  best_[at] = a;
  taf_[at] = taf;
  if (n_ < AIRPORT_NEAR_MAX) n_++;
}

void AirportJsonSink::feed(const char* d, size_t n) {
  for (size_t i = 0; i < n; i++) {
    const char c = d[i];
    const bool capturing = array_ && depth_ >= 2;
    if (capturing) {
      if (len_ < OBJ_MAX - 1) buf_[len_++] = c;
      else over_ = true;
    }
    if (in_str_) {
      if (esc_) esc_ = false;
      else if (c == '\\') esc_ = true;
      else if (c == '"') in_str_ = false;
      continue;
    }
    if (c == '"') {
      in_str_ = true;
    } else if (c == '[' || c == '{') {
      if (depth_ == 0) array_ = c == '[';  // only an array of objects is read
      if (array_ && depth_ == 1 && c == '{') {
        len_ = 0;
        over_ = false;
        buf_[len_++] = c;
      }
      if (depth_ < 64) depth_++;
    } else if (c == ']' || c == '}') {
      if (depth_ > 0) depth_--;
      if (array_ && depth_ == 1 && c == '}') {
        buf_[len_] = 0;
        if (over_) seen_++, dropped_++;
        else object();
        len_ = 0;
        over_ = false;
      }
    }
  }
}

}  // namespace fc
