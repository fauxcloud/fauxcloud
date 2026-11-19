// config.cpp -- see config.h.
#include "config.h"

#include <cmath>
#include <cstdio>
#include <cstring>

#include "json.h"
#include "tz.h"
#include "web_render.h"

namespace fc {

// ---------------------------------------------------------------- profiles

// the outdoor sensor's fields, the same for both rows (tempf humidity dewptf winddir windspeedmph windgustmph
// UV solarRadiation); pressure, indoor and the console-computed rain fields are not the sensor's
static constexpr uint32_t SENSOR_OUTDOOR = 1u << F_TEMP_C | 1u << F_HUMIDITY | 1u << F_DEWPT_C | 1u << F_WIND_DIR |
                                           1u << F_WIND_KMH | 1u << F_GUST_KMH | 1u << F_UV | 1u << F_SOLAR_WM2;
// clang-format off
static const StationProfile RULES_GENERIC = {"generic", "Generic WU", 0.0f, 0.0f, false, false, SENSOR_OUTDOOR, NAN, false};
// docs/station-profiles.md: wind > 121 mph is junk, humidity < 3 % marks a junk sample;
// `rainin` is a rate; the factory pressure is 29.92 inHg = 1013.2 hPa.
static const StationProfile RULES_YT60234 = {"yt60234", "Vevor/Youtong 7-in-1", 195.0f, 3.0f, true, true, SENSOR_OUTDOOR,
                                             1013.2f, true};
// clang-format on

const StationProfile& profileRules(Profile p) { return p == Profile::Generic ? RULES_GENERIC : RULES_YT60234; }

const char* fingerprintName(Fingerprint f) {
  switch (f) {
    case Fingerprint::Match: return "match";
    case Fingerprint::Unknown: return "unknown";
    case Fingerprint::Mismatch: return "mismatch";
    default: return "";
  }
}

// docs/station-profiles.md: the YT60234's upload, in this order (the cold-start upload sends a subset)
static const char* const FP_PARAMS[] = {"ID",           "PASSWORD", "dateutc",     "baromin",     "tempf",
                                        "humidity",     "dewptf",   "rainin",      "dailyrainin", "winddir",
                                        "windspeedmph", "windgustmph", "UV",       "solarRadiation"};
static const uint8_t FP_OUIS[][3] = {{0xB8, 0x3D, 0xFB}, {0x1C, 0x78, 0x4B}};  // Bouffalo Lab (a weewx capture and a real console)

Fingerprint stationFingerprint(const char* ua, const char* query, const uint8_t* mac) {
  if (!ua || strncmp(ua, "lwIP/", 5) != 0) return Fingerprint::Mismatch;
  size_t next = 0;  // the first FP_PARAMS index the next name may match
  const size_t nParams = sizeof FP_PARAMS / sizeof FP_PARAMS[0];
  for (const char* p = query ? query : ""; *p;) {
    const char* end = p;
    while (*end && *end != '&') end++;
    const char* eq = p;
    while (eq < end && *eq != '=') eq++;
    char name[24];
    const size_t n = (size_t)(eq - p);
    if (n) {
      if (n >= sizeof name) return Fingerprint::Mismatch;
      memcpy(name, p, n);
      name[n] = 0;
      size_t i = next;
      while (i < nParams && !sameCi(name, FP_PARAMS[i])) i++;
      if (i == nParams) return Fingerprint::Mismatch;  // unknown, repeated or out of order
      next = i + 1;
    }
    p = *end ? end + 1 : end;
  }
  if (!mac) return Fingerprint::Unknown;
  for (const auto& o : FP_OUIS)
    if (memcmp(mac, o, 3) == 0) return Fingerprint::Match;
  return Fingerprint::Unknown;
}

bool profileFromName(const char* s, Profile* out) {
  if (strcmp(s, "yt60234") == 0) { *out = Profile::Yt60234; return true; }
  if (strcmp(s, "generic") == 0) { *out = Profile::Generic; return true; }
  return false;
}

// ---------------------------------------------------------------- defaults

StationConfig defaultStationConfig() {
  StationConfig c;
  memset(&c, 0, sizeof c);
  c.profile = Profile::Yt60234;
  c.publish = true;
  c.stale_s = 240;
  for (int f = 0; f < CORE_FIELDS; f++) {
    c.lim_min[f] = NAN;
    c.lim_max[f] = NAN;
    c.jump_per_min[f] = 0;
  }
  c.rain_factor = 1.0f;
  c.wind_factor = 1.0f;
  return c;
}

StationConfig profileDefaults(Profile p) {
  StationConfig c = defaultStationConfig();
  c.profile = p;  // the profile's rules are fixed data (profileRules), not config values
  return c;
}

void Errors::add(const char* key, const char* why) {
  const int room = TEXT - (int)used;
  if (n >= MAX || room < 2) {
    dropped++;
    return;
  }
  const int k = snprintf(text + used, (size_t)room, "%s: %s", key, why);
  if (k < 0 || k >= room) {  // whole or not at all
    text[used] = 0;
    dropped++;
    return;
  }
  off[n++] = used;
  used = (uint16_t)(used + k + 1);
}

// ---------------------------------------------------------------- helpers

static const char* const CONV_NAMES[] = {"none", "FtoC", "InHgToHpa", "MphToKmh", "InToMm", "scale"};

static bool convFromName(const char* s, PromoConv* out) {
  for (int i = 0; i < 6; i++)
    if (strcmp(s, CONV_NAMES[i]) == 0) { *out = (PromoConv)i; return true; }
  return false;
}

double promoteConvert(const Promote& p, double x) {
  switch (p.conv) {
    case PromoConv::FtoC: return (x - 32.0) / 1.8;
    case PromoConv::InHgToHpa: return x * 33.8639;
    case PromoConv::MphToKmh: return x * 1.609344;
    case PromoConv::InToMm: return x * 25.4;
    case PromoConv::Scale: return x * p.scale;
    case PromoConv::None: break;
  }
  return x;
}

// clang-format off
static const PromoTypeDef PROMO_TYPES[(int)PromoType::COUNT] = {
  // id                  label               unit      conv                 device_class            state_class    group     icon           span   max  dec
  {"temperature",       "Temperature",      "°C",     PromoConv::FtoC,     "temperature",          "measurement", "other",  "thermometer", 5,     NAN, 1},
  {"soil_temperature",  "Soil temperature", "°C",     PromoConv::FtoC,     "temperature",          "measurement", "garden", "thermometer", 5,     NAN, 1},
  {"humidity",          "Humidity",         "%",      PromoConv::None,     "humidity",             "measurement", "other",  "droplet",     20,    100, 0},
  {"soil_moisture",     "Soil moisture",    "%",      PromoConv::None,     "moisture",             "measurement", "garden", "sprout",      10,    100, 0},
  {"leaf_wetness",      "Leaf wetness",     "%",      PromoConv::None,     "moisture",             "measurement", "garden", "leaf",        20,    100, 0},
  {"pressure",          "Pressure",         "hPa",    PromoConv::InHgToHpa,"atmospheric_pressure", "measurement", "other",  "gauge",       10,    NAN, 1},
  {"wind_speed",        "Wind speed",       "km/h",   PromoConv::MphToKmh, "wind_speed",           "measurement", "other",  "wind",        20,    NAN, 1},
  {"rain",              "Rain amount",      "mm",     PromoConv::InToMm,   "precipitation",        "measurement", "other",  "cloud-rain",  1,     NAN, 1},
  {"pm25",              "PM2.5",            "µg/m³",  PromoConv::None,     "pm25",                 "measurement", "air",    "haze",        20,    NAN, 0},
  {"pm10",              "PM10",             "µg/m³",  PromoConv::None,     "pm10",                 "measurement", "air",    "haze",        20,    NAN, 0},
  {"co2",               "CO₂",              "ppm",    PromoConv::None,     "carbon_dioxide",       "measurement", "air",    "haze",        200,   NAN, 0},
  {"uv",                "UV index",         "UV",     PromoConv::None,     nullptr,                "measurement", "other",  "sun",         2,     NAN, 1},
  {"illuminance",       "Light level",      "lx",     PromoConv::None,     "illuminance",          "measurement", "other",  "sun",         1000,  NAN, 0},
  {"solar",             "Solar radiation",  "W/m²",   PromoConv::None,     "irradiance",           "measurement", "other",  "sun",         100,   NAN, 0},
  {"battery",           "Battery level",    "%",      PromoConv::None,     "battery",              "measurement", "other",  "battery",     20,    100, 0},
  {"voltage",           "Voltage",          "V",      PromoConv::None,     "voltage",              "measurement", "other",  "battery",     1,     NAN, 2},
  {"other",             "Something else",   "",       PromoConv::None,     nullptr,                "measurement", "other",  "activity",    1,     NAN, 1},
};
// clang-format on

const PromoTypeDef& promoType(PromoType t) {
  const int i = (int)t;
  return PROMO_TYPES[i >= 0 && i < (int)PromoType::COUNT ? i : (int)PromoType::Other];
}

bool promoTypeFromName(const char* s, PromoType* out) {
  for (int i = 0; i < (int)PromoType::COUNT; i++)
    if (strcmp(PROMO_TYPES[i].id, s) == 0) { *out = (PromoType)i; return true; }
  return false;
}

// One UTF-8 code point -> its ASCII fold ("" = drop). Covers Polish and the common Latin-1 letters.
static const char* foldCp(uint32_t cp) {
  struct F { uint32_t cp; const char* s; };
  static const F T[] = {
      {0x105, "a"}, {0x104, "a"}, {0x107, "c"}, {0x106, "c"}, {0x119, "e"}, {0x118, "e"}, {0x142, "l"}, {0x141, "l"},
      {0x144, "n"}, {0x143, "n"}, {0xF3, "o"},  {0xD3, "o"},  {0x15B, "s"}, {0x15A, "s"}, {0x17A, "z"}, {0x179, "z"},
      {0x17C, "z"}, {0x17B, "z"}, {0xDF, "ss"}, {0xE7, "c"},  {0xC7, "c"},  {0xF1, "n"},  {0xD1, "n"},  {0xFD, "y"},
      {0xFF, "y"},  {0xF8, "o"},  {0xD8, "o"},  {0xE6, "ae"}, {0xC6, "ae"}, {0x10D, "c"}, {0x10C, "c"}, {0x161, "s"},
      {0x160, "s"}, {0x17E, "z"}, {0x17D, "z"}, {0x159, "r"}, {0x158, "r"}, {0x11B, "e"}, {0x11A, "e"}, {0x16F, "u"},
      {0x16E, "u"}, {0x2082, "2"}, {0xB5, "u"},
  };
  for (const auto& f : T)
    if (f.cp == cp) return f.s;
  if (cp >= 0xE0 && cp <= 0xE5) return "a";
  if (cp >= 0xC0 && cp <= 0xC5) return "a";
  if (cp >= 0xE8 && cp <= 0xEB) return "e";
  if (cp >= 0xC8 && cp <= 0xCB) return "e";
  if (cp >= 0xEC && cp <= 0xEF) return "i";
  if (cp >= 0xCC && cp <= 0xCF) return "i";
  if (cp >= 0xF2 && cp <= 0xF6) return "o";
  if (cp >= 0xD2 && cp <= 0xD6) return "o";
  if (cp >= 0xF9 && cp <= 0xFC) return "u";
  if (cp >= 0xD9 && cp <= 0xDC) return "u";
  return "";
}

void promoSlug(const char* name, char out[24]) {
  size_t n = 0;
  bool sep = false;
  auto put = [&](char c) {
    if (c == '_') {
      sep = n > 0;  // collapse runs, no leading '_'
      return;
    }
    if (sep && n < 23) out[n++] = '_';
    sep = false;
    if (n < 23) out[n++] = c;
  };
  const unsigned char* p = (const unsigned char*)name;
  while (*p) {
    uint32_t cp = *p;
    int len = 1;
    if (cp >= 0xC0 && cp < 0xE0 && (p[1] & 0xC0) == 0x80) { cp = (cp & 0x1F) << 6 | (p[1] & 0x3F); len = 2; }
    else if (cp >= 0xE0 && cp < 0xF0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80) {
      cp = (cp & 0x0F) << 12 | (p[1] & 0x3F) << 6 | (p[2] & 0x3F);
      len = 3;
    } else if (cp >= 0x80) {
      len = 1;
      cp = 0;  // invalid / 4-byte: dropped
      while (p[len] && (p[len] & 0xC0) == 0x80) len++;
    }
    p += len;
    if (cp < 0x80) {
      char c = (char)cp;
      if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
      if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) put(c);
      else put('_');
    } else {
      const char* f = foldCp(cp);
      if (!*f) put('_');
      for (; *f; f++) put(*f);
    }
  }
  out[n] = 0;
}

static bool sameF(float a, float b) { return (std::isnan(a) && std::isnan(b)) || a == b; }

// "temp_c_min" -> field + is_max; only core fields.
static bool limitKey(const char* k, int* field, bool* is_max) {
  size_t n = strlen(k);
  if (n < 5) return false;
  const bool mx = strcmp(k + n - 4, "_max") == 0, mn = strcmp(k + n - 4, "_min") == 0;
  if (!mx && !mn) return false;
  char base[32];
  if (n - 4 >= sizeof base) return false;
  memcpy(base, k, n - 4);
  base[n - 4] = 0;
  int f = fieldByKey(base);
  if (f < 0 || f >= CORE_FIELDS) return false;
  *field = f;
  *is_max = mx;
  return true;
}

static bool jumpKey(const char* k, int* field) {
  size_t n = strlen(k);
  if (n < 9 || strcmp(k + n - 8, "_per_min") != 0) return false;
  char base[32];
  if (n - 8 >= sizeof base) return false;
  memcpy(base, k, n - 8);
  base[n - 8] = 0;
  int f = fieldByKey(base);
  if (f < 0 || f >= CORE_FIELDS) return false;
  *field = f;
  return true;
}

struct CalibKey {
  const char* key;
  float StationConfig::*member;
  float lo, hi, def;
};
static const CalibKey CALIB[] = {
    {"temp_c_offset", &StationConfig::temp_c_offset, -20, 20, 0},
    {"humidity_offset", &StationConfig::humidity_offset, -50, 50, 0},
    {"pressure_hpa_offset", &StationConfig::pressure_hpa_offset, -100, 100, 0},
    {"wind_dir_offset", &StationConfig::wind_dir_offset, -360, 360, 0},
    {"rain_factor", &StationConfig::rain_factor, 0.1f, 10, 1},
    {"wind_factor", &StationConfig::wind_factor, 0.1f, 10, 1},
};

static bool validPromoteKey(const char* k) {
  size_t n = strlen(k);
  if (n == 0 || n > 23) return false;
  for (const char* p = k; *p; p++)
    if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '_')) return false;
  // must not shadow a canonical field or a reserved topic leaf
  if (fieldByKey(k) >= 0 || strcmp(k, "last_seen") == 0 || strcmp(k, "availability") == 0 ||
      strncmp(k, "rej_", 4) == 0)
    return false;
  return true;
}

// Strips control characters and replaces every invalid UTF-8 sequence with '?' (the name goes into MQTT discovery,
// where invalid UTF-8 makes Home Assistant drop the payload). Returns true when something was replaced.
static bool cleanName(char* s) {
  bool replaced = false;
  char* w = s;
  const unsigned char* r = (const unsigned char*)s;
  while (*r) {
    const unsigned char c = *r;
    if (c < 0x20 || c == 0x7F) { r++; continue; }
    int n = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : (c >> 3) == 0x1E ? 4 : 0;
    bool ok = n > 0 && !(n == 2 && c < 0xC2);  // no overlong 2-byte forms
    for (int i = 1; ok && i < n; i++) ok = (r[i] & 0xC0) == 0x80;
    if (!ok) {
      *w++ = '?';
      r++;
      replaced = true;
      continue;
    }
    for (int i = 0; i < n; i++) *w++ = (char)r[i];
    r += n;
  }
  *w = 0;
  return replaced;
}

// A value of the wrong type is skipped and reported; only a syntax error stops the document.
static bool readNumberOrError(JsonReader& r, const char* key, double* v, Errors* err) {
  const char c = r.peek();
  if (!((c >= '0' && c <= '9') || c == '-' || c == '+' || c == '.')) {
    r.skipValue();
    err->add(key, "not a number");
    return false;
  }
  if (r.readNumber(v) && std::isfinite(*v)) return true;
  err->add(key, "not a number");
  return false;
}

static bool readBoolOrError(JsonReader& r, const char* key, bool* v, Errors* err) {
  const char c = r.peek();
  if (c != 't' && c != 'f') {
    r.skipValue();
    err->add(key, "not a bool");
    return false;
  }
  return r.readBool(v);
}

static bool isNull(JsonReader& r) {
  if (r.peek() != 'n') return false;
  r.skipValue();
  return true;
}

// ---------------------------------------------------------------- station JSON in

static void readLimits(JsonReader& r, StationConfig* c, Errors* err) {
  if (r.peek() != '{') { err->add("limits", "not an object"); r.skipValue(); return; }
  r.beginObject();
  char k[40];
  while (r.nextKey(k, sizeof k)) {
    int f;
    bool mx;
    if (!limitKey(k, &f, &mx)) { err->add(k, "unknown limit"); r.skipValue(); continue; }
    float* slot = mx ? &c->lim_max[f] : &c->lim_min[f];
    if (isNull(r)) { *slot = NAN; continue; }
    double v;
    if (!readNumberOrError(r, k, &v, err)) { if (r.failed()) return; continue; }
    *slot = (float)v;
  }
  for (int f = 0; f < CORE_FIELDS; f++) {
    if (!std::isnan(c->lim_min[f]) && !std::isnan(c->lim_max[f]) && c->lim_min[f] > c->lim_max[f]) {
      char k2[40];
      snprintf(k2, sizeof k2, "%s_min/_max", FIELDS[f].key);
      err->add(k2, "min > max, both cleared");
      c->lim_min[f] = c->lim_max[f] = NAN;
    }
  }
}

static void readJumps(JsonReader& r, StationConfig* c, Errors* err) {
  if (r.peek() != '{') { err->add("jumps", "not an object"); r.skipValue(); return; }
  r.beginObject();
  char k[40];
  while (r.nextKey(k, sizeof k)) {
    int f;
    if (!jumpKey(k, &f)) { err->add(k, "unknown jump"); r.skipValue(); continue; }
    if (isNull(r)) { c->jump_per_min[f] = 0; continue; }
    double v;
    if (!readNumberOrError(r, k, &v, err)) { if (r.failed()) return; continue; }
    if (v < 0) { err->add(k, "must be >= 0"); continue; }
    c->jump_per_min[f] = (float)v;
  }
}

static void readCalib(JsonReader& r, StationConfig* c, Errors* err) {
  if (r.peek() != '{') { err->add("calib", "not an object"); r.skipValue(); return; }
  r.beginObject();
  char k[40];
  while (r.nextKey(k, sizeof k)) {
    const CalibKey* ck = nullptr;
    for (const auto& x : CALIB)
      if (strcmp(x.key, k) == 0) ck = &x;
    if (!ck) { err->add(k, "unknown calibration"); r.skipValue(); continue; }
    if (isNull(r)) { c->*(ck->member) = ck->def; continue; }
    double v;
    if (!readNumberOrError(r, k, &v, err)) { if (r.failed()) return; continue; }
    if (v < ck->lo || v > ck->hi) { err->add(k, "out of range"); continue; }
    c->*(ck->member) = (float)v;
  }
}

static void readDisabled(JsonReader& r, StationConfig* c, Errors* err) {
  if (r.peek() != '[') { err->add("disabled", "not an array"); r.skipValue(); return; }
  r.beginArray();
  uint32_t mask = 0;
  while (r.nextElement()) {
    char k[32];
    if (!r.readString(k, sizeof k)) { err->add("disabled", "not a string"); return; }
    int f = fieldByKey(k);
    if (f < 0) { err->add(k, "unknown field in disabled"); continue; }
    mask |= 1u << f;
  }
  if (!r.failed()) c->disabled = mask;  // unknown keys are skipped, the known ones still apply
}

// "hist": ["indoor_temp_c", "soil_t", ...] -- up to 4 keys of [a-z0-9_]; [] / null = automatic
static void readHist(JsonReader& r, StationConfig* c, Errors* err) {
  if (r.peek() != '[') { err->add("hist", "not an array"); r.skipValue(); return; }
  r.beginArray();
  char keys[HIST_KEYS][HIST_KEY_LEN] = {};
  int n = 0;
  while (r.nextElement()) {
    char k[HIST_KEY_LEN];
    if (!r.readString(k, sizeof k)) { err->add("hist", "not a string"); return; }
    bool ok = k[0] != 0;
    for (const char* p = k; ok && *p; p++) ok = (*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '_';
    if (!ok) { err->add(k, "not a key in hist"); continue; }
    if (n >= HIST_KEYS) { err->add(k, "hist holds 4 keys, ignored"); continue; }
    snprintf(keys[n++], HIST_KEY_LEN, "%s", k);
  }
  if (!r.failed()) memcpy(c->hist, keys, sizeof keys);
}

static void readPromote(JsonReader& r, StationConfig* c, Errors* err) {
  if (r.peek() != '{') { err->add("promote", "not an object"); r.skipValue(); return; }
  r.beginObject();
  Promote list[MAX_PROMOTE];
  memset(list, 0, sizeof list);
  int n = 0;
  char param[40];
  while (r.nextKey(param, sizeof param)) {
    if (n >= MAX_PROMOTE) { err->add(param, "more than 4 promotions"); r.skipValue(); continue; }
    if (strlen(param) > 15) { err->add(param, "not promotable (too long)"); r.skipValue(); continue; }
    // a key the parser now maps (case-insensitive, the new extended fields) or ignores (protocol flags) cannot
    // stay a promotion; read it to the end anyway so its HA object id is known for the retraction
    const char* stale = fieldByParam(param) >= 0 ? "built in now" : isProtocolFlag(param) ? "protocol flag" : nullptr;
    // Read everything first: the type's defaults apply before the explicit (Advanced) overrides.
    Promote p;
    memset(&p, 0, sizeof p);
    strcpy(p.param, param);
    p.type = PromoType::Other;
    char unit[12] = "", dc[24] = "", sc[20] = "", typeName[24] = "";
    bool hasUnit = false, hasConv = false, hasScale = false, hasDc = false, hasSc = false, bad = false;
    PromoConv conv = PromoConv::None;
    float scale = 1.0f;
    bool ok = r.beginObject();
    char k[24];
    while (ok && r.nextKey(k, sizeof k)) {
      bool trunc = false;
      if (strcmp(k, "as") == 0) ok = r.readString(p.key, sizeof p.key, &trunc);
      else if (strcmp(k, "name") == 0) {
        char tmp[96];
        ok = r.readString(tmp, sizeof tmp, &trunc);
        if (ok) {
          cleanName(tmp);
          if (trunc || strlen(tmp) > PROMO_NAME_MAX_BYTES) err->add(param, "name truncated to 31 bytes");
          utf8Truncate(tmp, PROMO_NAME_MAX_BYTES);
          strcpy(p.name, tmp);
          trunc = false;
        }
      } else if (strcmp(k, "type") == 0) ok = r.readString(typeName, sizeof typeName, &trunc);
      else if (strcmp(k, "unit") == 0) { ok = r.readString(unit, sizeof unit, &trunc); hasUnit = true; }
      else if (strcmp(k, "device_class") == 0) { ok = r.readString(dc, sizeof dc, &trunc); hasDc = true; }
      else if (strcmp(k, "state_class") == 0) { ok = r.readString(sc, sizeof sc, &trunc); hasSc = true; }
      else if (strcmp(k, "conv") == 0) {
        char cv[16];
        ok = r.readString(cv, sizeof cv);
        if (ok && !convFromName(cv, &conv)) { err->add(param, "unknown conv"); bad = true; }
        hasConv = true;
      } else if (strcmp(k, "scale") == 0) {
        double v;
        ok = r.readNumber(&v);
        if (ok) { scale = (float)v; hasScale = true; }
      } else {
        err->add(k, "unknown promote key");
        ok = r.skipValue();
      }
      if (trunc) { err->add(param, "value too long"); bad = true; }
    }
    if (!ok || r.failed()) { err->add(param, "malformed promotion"); return; }
    if (typeName[0] && !promoTypeFromName(typeName, &p.type)) { err->add(param, "unknown type"); bad = true; }
    if (bad) continue;
    const PromoTypeDef& t = promoType(p.type);
    snprintf(p.unit, sizeof p.unit, "%s", hasUnit ? unit : t.unit);
    p.conv = hasConv ? conv : t.conv;
    p.scale = hasScale ? scale : 1.0f;
    snprintf(p.device_class, sizeof p.device_class, "%s", hasDc ? dc : (t.device_class ? t.device_class : ""));
    snprintf(p.state_class, sizeof p.state_class, "%s", hasSc ? sc : (t.state_class ? t.state_class : ""));
    if (!p.name[0]) snprintf(p.name, sizeof p.name, "%.*s", (int)sizeof p.name - 1, p.key[0] ? p.key : param);
    const bool explicitKey = p.key[0] != 0;
    if (!explicitKey) {
      promoSlug(p.name, p.key);
      if (!validPromoteKey(p.key)) promoSlug(param, p.key);
      // a derived key is made unique by a suffix
      char base[24];
      snprintf(base, sizeof base, "%.20s", p.key);
      for (int suffix = 2; suffix < 10; suffix++) {
        bool clash = false;
        for (int i = 0; i < n; i++) clash |= strcmp(list[i].key, p.key) == 0;
        if (!clash && validPromoteKey(p.key)) break;
        snprintf(p.key, sizeof p.key, "%.20s_%d", base, suffix % 10);
      }
    }
    if (stale) {
      char why[96];
      snprintf(why, sizeof why, "promotion %.20s dropped: %.48s", p.key, stale);
      err->add(param, why);
      if (err->n_stale < 4) snprintf(err->stale_keys[err->n_stale++], sizeof err->stale_keys[0], "%s", p.key);
      continue;
    }
    if (!validPromoteKey(p.key)) { err->add(param, "'as' must be [a-z0-9_]{1,23} and not a reserved key"); continue; }
    bool dup = false;
    for (int i = 0; i < n; i++) dup |= strcmp(list[i].key, p.key) == 0;
    if (dup) { err->add(param, "duplicate 'as'"); continue; }
    p.used = true;
    list[n++] = p;
  }
  if (!r.failed()) memcpy(c->promote, list, sizeof list);  // the object replaces the whole list
}

bool stationFromJson(const char* json, size_t len, StationConfig* cfg, Errors* err) {
  Errors dummy;
  if (!err) err = &dummy;
  JsonReader r(json, len);
  if (!r.beginObject()) { err->add("$", "not a JSON object"); return false; }
  StationConfig c = *cfg;
  const StationConfig def = defaultStationConfig();
  char k[40];
  while (r.nextKey(k, sizeof k)) {
    if (strcmp(k, "name") == 0) {
      if (isNull(r)) { c.name[0] = 0; continue; }
      char tmp[200];
      bool trunc;
      if (r.peek() != '"') { err->add(k, "not a string"); r.skipValue(); continue; }
      if (!r.readString(tmp, sizeof tmp, &trunc)) break;
      if (cleanName(tmp)) err->add(k, "invalid UTF-8 replaced with '?'");
      if (trunc || strlen(tmp) > NAME_MAX_BYTES) err->add(k, "truncated to 47 bytes");
      utf8Truncate(tmp, NAME_MAX_BYTES);
      strcpy(c.name, tmp);
    } else if (strcmp(k, "profile") == 0) {
      if (isNull(r)) { c.profile = def.profile; continue; }
      char p[16];
      Profile pr;
      if (!r.readString(p, sizeof p) || !profileFromName(p, &pr)) { err->add(k, "unknown profile"); if (r.failed()) break; continue; }
      c.profile = pr;
    } else if (strcmp(k, "publish") == 0) {
      if (isNull(r)) { c.publish = def.publish; continue; }
      bool b;
      if (readBoolOrError(r, k, &b, err)) c.publish = b;
    } else if (strcmp(k, "stale_s") == 0) {
      if (isNull(r)) { c.stale_s = def.stale_s; continue; }
      double v;
      if (!readNumberOrError(r, k, &v, err)) { if (r.failed()) break; continue; }
      if (v < 60 || v > 86400) { err->add(k, "must be 60..86400"); continue; }
      c.stale_s = (uint32_t)v;
    } else if (strcmp(k, "limits") == 0) {
      if (isNull(r)) { memcpy(c.lim_min, def.lim_min, sizeof c.lim_min); memcpy(c.lim_max, def.lim_max, sizeof c.lim_max); continue; }
      readLimits(r, &c, err);
    } else if (strcmp(k, "jumps") == 0) {
      if (isNull(r)) { memcpy(c.jump_per_min, def.jump_per_min, sizeof c.jump_per_min); continue; }
      readJumps(r, &c, err);
    } else if (strcmp(k, "calib") == 0) {
      if (isNull(r)) { for (const auto& x : CALIB) c.*(x.member) = x.def; continue; }
      readCalib(r, &c, err);
    } else if (strcmp(k, "disabled") == 0) {
      if (isNull(r)) { c.disabled = 0; continue; }
      readDisabled(r, &c, err);
    } else if (strcmp(k, "promote") == 0) {
      if (isNull(r)) { memset(c.promote, 0, sizeof c.promote); continue; }
      readPromote(r, &c, err);
    } else if (strcmp(k, "hist") == 0) {
      if (isNull(r)) { memset(c.hist, 0, sizeof c.hist); continue; }
      readHist(r, &c, err);
    } else {
      err->add(k, "unknown key, ignored");
      r.skipValue();
    }
    if (r.failed()) break;
  }
  if (r.failed()) err->add("$", "malformed JSON, the rest of the document was ignored");
  *cfg = c;  // everything accepted up to the first syntax error applies
  return true;
}

StationConfig mergeStation(const char* overrides_json, Errors* err) {
  StationConfig c = defaultStationConfig();
  if (overrides_json && *overrides_json) stationFromJson(overrides_json, strlen(overrides_json), &c, err);
  return c;
}

// ---------------------------------------------------------------- station JSON out

static void num(Out& o, float v) {
  char b[24];
  snprintf(b, sizeof b, "%g", (double)v);
  o.raw(b);
}

static void key(Out& o, bool& first, const char* k) {
  if (!first) o.raw(",");
  first = false;
  o.jsonStr(k);
  o.raw(":");
}

static void writeStation(Out& o, const StationConfig& c, bool all) {
  const StationConfig d = profileDefaults(c.profile);
  bool first = true;
  o.raw("{");
  if (all || strcmp(c.name, d.name) != 0) { key(o, first, "name"); o.jsonStr(c.name); }
  if (all || c.profile != Profile::Yt60234) { key(o, first, "profile"); o.jsonStr(profileRules(c.profile).name); }
  if (all || c.publish != d.publish) { key(o, first, "publish"); o.raw(c.publish ? "true" : "false"); }
  if (all || c.stale_s != d.stale_s) { key(o, first, "stale_s"); o.printf("%lu", (unsigned long)c.stale_s); }

  bool anyLim = false;
  for (int f = 0; f < CORE_FIELDS; f++)
    anyLim |= !sameF(c.lim_min[f], d.lim_min[f]) || !sameF(c.lim_max[f], d.lim_max[f]);
  if (all || anyLim) {
    key(o, first, "limits");
    o.raw("{");
    bool f1 = true;
    for (int f = 0; f < CORE_FIELDS; f++) {
      char k[40];
      if (!std::isnan(c.lim_min[f])) { snprintf(k, sizeof k, "%s_min", FIELDS[f].key); key(o, f1, k); num(o, c.lim_min[f]); }
      if (!std::isnan(c.lim_max[f])) { snprintf(k, sizeof k, "%s_max", FIELDS[f].key); key(o, f1, k); num(o, c.lim_max[f]); }
    }
    o.raw("}");
  }
  bool anyJump = false;
  for (int f = 0; f < CORE_FIELDS; f++) anyJump |= c.jump_per_min[f] != d.jump_per_min[f];
  if (all || anyJump) {
    key(o, first, "jumps");
    o.raw("{");
    bool f1 = true;
    for (int f = 0; f < CORE_FIELDS; f++) {
      if (c.jump_per_min[f] <= 0) continue;
      char k[40];
      snprintf(k, sizeof k, "%s_per_min", FIELDS[f].key);
      key(o, f1, k);
      num(o, c.jump_per_min[f]);
    }
    o.raw("}");
  }
  bool anyCal = false;
  for (const auto& x : CALIB) anyCal |= c.*(x.member) != d.*(x.member);
  if (all || anyCal) {
    key(o, first, "calib");
    o.raw("{");
    bool f1 = true;
    for (const auto& x : CALIB) {
      if (!all && c.*(x.member) == d.*(x.member)) continue;
      key(o, f1, x.key);
      num(o, c.*(x.member));
    }
    o.raw("}");
  }
  if (all || c.disabled != d.disabled) {
    key(o, first, "disabled");
    o.raw("[");
    bool f1 = true;
    for (int f = 0; f < F_COUNT; f++) {
      if (!((c.disabled >> f) & 1u)) continue;
      if (!f1) o.raw(",");
      f1 = false;
      o.jsonStr(FIELDS[f].key);
    }
    o.raw("]");
  }
  if (all || c.hist[0][0]) {  // only when chosen by hand (the automatic map lives in NVS ch:<id>)
    key(o, first, "hist");
    o.raw("[");
    for (int i = 0; i < HIST_KEYS && c.hist[i][0]; i++) {
      if (i) o.raw(",");
      o.jsonStr(c.hist[i]);
    }
    o.raw("]");
  }
  bool anyPromo = false;
  for (const auto& p : c.promote) anyPromo |= p.used;
  if (all || anyPromo) {
    key(o, first, "promote");
    o.raw("{");
    bool f1 = true;
    for (const auto& p : c.promote) {
      if (!p.used) continue;
      const PromoTypeDef& t = promoType(p.type);
      key(o, f1, p.param);
      o.raw("{\"name\":");
      o.jsonStr(p.name);
      o.raw(",\"type\":");
      o.jsonStr(t.id);
      o.raw(",\"as\":");
      o.jsonStr(p.key);
      // Advanced overrides: only what differs from the type (all of it in the effective view)
      if (all || strcmp(p.unit, t.unit) != 0) { o.raw(",\"unit\":"); o.jsonStr(p.unit); }
      if (all || p.conv != t.conv) { o.raw(",\"conv\":"); o.jsonStr(CONV_NAMES[(int)p.conv]); }
      if (p.conv == PromoConv::Scale) { o.raw(",\"scale\":"); num(o, p.scale); }
      if (all || strcmp(p.device_class, t.device_class ? t.device_class : "") != 0) {
        o.raw(",\"device_class\":");
        o.jsonStr(p.device_class);
      }
      if (all || strcmp(p.state_class, t.state_class ? t.state_class : "") != 0) {
        o.raw(",\"state_class\":");
        o.jsonStr(p.state_class);
      }
      if (all) {
        o.raw(",\"group\":");
        o.jsonStr(t.group);
        o.raw(",\"icon\":");
        o.jsonStr(t.icon);
        o.raw(",\"span\":");
        num(o, t.span);
        o.raw(",\"max\":");
        if (std::isnan(t.max)) o.raw("null");
        else num(o, t.max);
        o.printf(",\"dec\":%u", (unsigned)t.dec);
      }
      o.raw("}");
    }
    o.raw("}");
  }
  o.raw("}");
}

void stationToJson(Out& o, const StationConfig& cfg) { writeStation(o, cfg, false); }
void stationEffectiveJson(Out& o, const StationConfig& cfg) { writeStation(o, cfg, true); }

static bool countBytes(void* ctx, const char*, size_t n) {
  *(size_t*)ctx += n;
  return true;
}

bool stationConfigured(const StationConfig& cfg) {
  size_t n = 0;
  char buf[64];
  Out o(countBytes, &n, buf, sizeof buf);
  stationToJson(o, cfg);
  o.flush();
  return n > 2;  // more than "{}"
}

// ---------------------------------------------------------------- bridge

BridgeConfig defaultBridgeConfig() {
  BridgeConfig b;
  memset(&b, 0, sizeof b);
  strcpy(b.tz, DEFAULT_TZ);
  b.lat = NAN;
  b.lon = NAN;
  b.auto_publish_new_stations = true;
  b.mqtt_interval_last_seen_s = 300;
  b.max_stations = DEFAULT_MAX_STATIONS;
  return b;
}

bool bridgeFromJson(const char* json, size_t len, BridgeConfig* cfg, Errors* err, int in_use) {
  Errors dummy;
  if (!err) err = &dummy;
  JsonReader r(json, len);
  if (!r.beginObject()) { err->add("$", "not a JSON object"); return false; }
  BridgeConfig c = *cfg;
  const BridgeConfig d = defaultBridgeConfig();
  char k[40];
  bool setLan = false, setMax = false, setFwd = false;
  while (r.nextKey(k, sizeof k)) {
    if (strcmp(k, "tz") == 0) {
      if (isNull(r)) { strcpy(c.tz, d.tz); continue; }
      char t[64];
      bool trunc;
      if (r.peek() != '"') { err->add(k, "not a string"); r.skipValue(); continue; }
      if (!r.readString(t, sizeof t, &trunc)) break;
      PosixTz parsed;
      if (trunc || !parsePosixTz(t, &parsed)) { err->add(k, "not a supported POSIX TZ string"); continue; }
      strcpy(c.tz, t);
    } else if (strcmp(k, "tz_name") == 0) {
      if (isNull(r)) { c.tz_name[0] = 0; continue; }
      if (r.peek() != '"') { err->add(k, "not a string"); r.skipValue(); continue; }
      char t[64];
      bool trunc;
      if (!r.readString(t, sizeof t, &trunc)) break;
      bool ok = !trunc && strlen(t) < sizeof c.tz_name;
      for (const char* p = t; ok && *p; p++)
        ok = (*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '/' ||
             *p == '_' || *p == '-' || *p == '+';
      if (!ok) { err->add(k, "not an IANA zone name"); continue; }
      strcpy(c.tz_name, t);
    } else if (strcmp(k, "lat") == 0 || strcmp(k, "lon") == 0) {
      const bool isLat = k[1] == 'a';
      float& dst = isLat ? c.lat : c.lon;
      if (isNull(r)) { dst = NAN; continue; }
      double v;
      if (!readNumberOrError(r, k, &v, err)) { if (r.failed()) break; continue; }
      if (isLat ? (v < -90 || v > 90) : (v < -180 || v > 180)) { err->add(k, "out of range"); continue; }
      dst = (float)v;
    } else if (strcmp(k, "auto_publish_new_stations") == 0) {
      if (isNull(r)) { c.auto_publish_new_stations = d.auto_publish_new_stations; continue; }
      bool b;
      if (readBoolOrError(r, k, &b, err)) c.auto_publish_new_stations = b;
    } else if (strcmp(k, "lan_uploads") == 0) {
      setLan = true;
      if (isNull(r)) { c.lan_uploads = d.lan_uploads; continue; }
      bool b;
      if (readBoolOrError(r, k, &b, err)) c.lan_uploads = b;
    } else if (strcmp(k, "forwarding") == 0) {
      setFwd = true;
      if (isNull(r)) { c.forwarding = d.forwarding; continue; }
      bool b;
      if (readBoolOrError(r, k, &b, err)) c.forwarding = b;
    } else if (strcmp(k, "log_verbose") == 0) {
      if (isNull(r)) { c.log_verbose = d.log_verbose; continue; }
      bool b;
      if (readBoolOrError(r, k, &b, err)) c.log_verbose = b;
    } else if (strcmp(k, "max_stations_cap") == 0) {  // read-only (the profile's), a PUT of the effective view passes
      r.skipValue();
    } else if (strcmp(k, "max_stations") == 0) {
      setMax = true;
      if (isNull(r)) { c.max_stations = d.max_stations; continue; }
      double v;
      if (!readNumberOrError(r, k, &v, err)) { if (r.failed()) break; continue; }
      if (MAX_STATIONS == 1 && v != 1) { err->add(k, "fixed at 1 on this board"); continue; }  // the profile's station count
      if (v < 1 || v > MAX_STATIONS || v != std::floor(v)) { err->add(k, "must be 1..2"); continue; }
      if ((int)v < in_use) { err->add(k, "below the number of stations in use: forget one first"); continue; }
      c.max_stations = (uint8_t)v;
    } else if (strcmp(k, "mqtt_interval_last_seen_s") == 0) {
      if (isNull(r)) { c.mqtt_interval_last_seen_s = d.mqtt_interval_last_seen_s; continue; }
      double v;
      if (!readNumberOrError(r, k, &v, err)) { if (r.failed()) break; continue; }
      if (v < 60 || v > 3600) { err->add(k, "must be 60..3600"); continue; }
      c.mqtt_interval_last_seen_s = (uint32_t)v;
    } else {
      err->add(k, "unknown key, ignored");
      r.skipValue();
    }
    if (r.failed()) break;
  }
  if (r.failed()) err->add("$", "malformed JSON, the rest of the document was ignored");
  // forwarding on => one station and no LAN uploads -- the WROOM-32's heap carries one TLS send,
  // not that plus a second console or LAN uploads
  static const char* const NO_MEM = "off while forwarding is on (not enough memory)";
  if (c.forwarding && setFwd && !cfg->forwarding) {  // turned on by this document
    if (MAX_STATIONS > 1 && in_use > 1) {  // at 1 station the choice cannot arise
      err->add("forwarding", "keep one station first (choose which)");
      c.forwarding = false;
    } else {
      c.max_stations = 1;
      c.lan_uploads = false;
    }
  } else if (c.forwarding) {
    if (setLan && c.lan_uploads) err->add("lan_uploads", NO_MEM);
    if (setMax && c.max_stations > 1) err->add("max_stations", NO_MEM);
    c.lan_uploads = false;
    c.max_stations = 1;
  }
  *cfg = c;
  return true;
}

static void writeBridge(Out& o, const BridgeConfig& c, bool all) {
  const BridgeConfig d = defaultBridgeConfig();
  bool first = true;
  o.raw("{");
  if (all || strcmp(c.tz, d.tz) != 0) { key(o, first, "tz"); o.jsonStr(c.tz); }
  if (all || c.tz_name[0]) { key(o, first, "tz_name"); o.jsonStr(c.tz_name); }
  if (all || !std::isnan(c.lat)) {
    key(o, first, "lat");
    if (std::isnan(c.lat)) o.raw("null");
    else o.printf("%.4f", (double)c.lat);
  }
  if (all || !std::isnan(c.lon)) {
    key(o, first, "lon");
    if (std::isnan(c.lon)) o.raw("null");
    else o.printf("%.4f", (double)c.lon);
  }
  if (all || c.auto_publish_new_stations != d.auto_publish_new_stations) {
    key(o, first, "auto_publish_new_stations");
    o.raw(c.auto_publish_new_stations ? "true" : "false");
  }
  if (all || c.lan_uploads != d.lan_uploads) {
    key(o, first, "lan_uploads");
    o.raw(c.lan_uploads ? "true" : "false");
  }
  if (all || c.max_stations != d.max_stations) {
    key(o, first, "max_stations");
    o.printf("%u", (unsigned)c.max_stations);
  }
  if (all) {  // the profile's ceiling, so one web bundle knows "never more" (not stored)
    key(o, first, "max_stations_cap");
    o.printf("%d", MAX_STATIONS);
  }
  if (all || c.mqtt_interval_last_seen_s != d.mqtt_interval_last_seen_s) {
    key(o, first, "mqtt_interval_last_seen_s");
    o.printf("%lu", (unsigned long)c.mqtt_interval_last_seen_s);
  }
  if (all || c.forwarding != d.forwarding) {
    key(o, first, "forwarding");
    o.raw(c.forwarding ? "true" : "false");
  }
  if (all || c.log_verbose != d.log_verbose) {
    key(o, first, "log_verbose");
    o.raw(c.log_verbose ? "true" : "false");
  }
  o.raw("}");
}

void bridgeToJson(Out& o, const BridgeConfig& cfg) { writeBridge(o, cfg, false); }
void bridgeEffectiveJson(Out& o, const BridgeConfig& cfg) { writeBridge(o, cfg, true); }

}  // namespace fc
