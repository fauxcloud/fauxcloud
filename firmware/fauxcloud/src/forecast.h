// forecast.h -- the local outlook from this station's own readings, as codes + params (the page and HA hold the
// words). One pure function over the calibrated hourly rows (calHour, = /api/hourly) + the live values:
// a state (rain / clear night / the day sky / dry) and its qualifier, what comes next (the 3-h pressure tendency, a fast
// cooling, tonight's minimum, or why not), at most one warning (icy, gusts, hoar frost, ground frost, mist or fog),
// gates (time, stale), plus the main airport's report scoped "reported nearby" (T3). No Zambretti or Sager line, no
// amounts, nothing persisted. Thresholds are named defaults in one table (a learner in use replaces a default, never
// edits it; LearnUse). The code tables are append-only: a new source adds inputs and codes and may override a state
// without an API shape change; renderers treat an unknown code as absent. Plain C++, host-compilable, no heap, no
// static state.
#pragma once
#include <cstddef>
#include <cstdint>

namespace fc {

class Out;
struct Station;
class Recent;

namespace fcst {
// thresholds (defaults)
constexpr float TEND_THR_HPA = 1.6f;     // ±1.6 hPa / 3 h (Met Office "steady")
constexpr float TEND_FAST_HPA = 3.6f;    // "quickly" (as tendency.js)
constexpr uint32_t TEND_FRESH_S = 7200;  // the newest closed hour starts at most 2 h before now (freshness gate)
constexpr int TEND_HOURS = 4;            // a 3-h window = 4 hourly means
constexpr float RAIN_MODERATE = 2.5f;    // mm/h
constexpr float RAIN_HEAVY = 7.6f;       // mm/h
constexpr float SOLAR_NIGHT_WM2 = 5.0f;  // dark without a location (dash.js skyState)
constexpr float COOL_DT = -1.0f;         // °C per hour: a clear-sky cooling (L5 default)
constexpr float WARM_DT = 0.5f;          // °C per hour: a later rise ends "clear night" (silence, never "cloud")
constexpr float CLEAR_WIND_KMH = 5.0f;   // the cooling and the frost rules need light wind
constexpr float ICY_T = 1.0f;            // °C
constexpr float ICY_RAIN12_MM = 0.2f;    // one tip ~0.225 mm in the last 12 h
constexpr float GUST_WARN_KMH = 30.0f;   // the 10-min gust
constexpr float HOAR_SPREAD = 2.0f;      // grass ~ T2m - 2: T - 2 <= Td
constexpr float FROST_T = 2.0f;          // °C at 2 m: grass <= 0
constexpr int WINDOW_H = 18;             // closed hours read (a winter night at 52° N + 2)
// line 1's qualifier: factual bands on the air temperature, shown as read
// (0.1 °C), silent between; `calm` from the newest closed hour
constexpr float FREEZING_T = 0.0f;       // °C, <=
constexpr float SEVERE_FROST_T = -10.0f; // °C, <=
constexpr float HOT_T = 30.0f;           // °C, >= live AND the newest closed hour's mean (a sun spike is not a band)
// stateless hysteresis: a band enters on the newest closed hour's mean and
// holds on the live value with a margin -- no memory in outlook() (replay parity)
constexpr float WINDY_KMH = 20.0f;       // km/h, the newest closed hour's mean wind (Beaufort 4) ...
constexpr float WINDY_LIVE_KMH = 16.0f;  // ... and the last 10 min's mean (in.wind10)
constexpr float HUMID_RH = 90.0f;        // %, the newest closed hour's mean, by day only, never with a rain state ...
constexpr float HUMID_LIVE_RH = 88.0f;   // ... and the live value
constexpr float BLIND_T = FROST_T;       // °C, <=: "No rain." is silent (the unheated gauge cannot see snow or sleet)
constexpr float SLEET_T = 1.0f;          // °C, <=, as shown: a rain state reads "Sleet or snow." (core.js skyFx draws flakes
                                         // at the same value; stateless like the frost bands, so the words and the sky agree)
// the rules a learner in use feeds (the learned value replaces a default; a safety rule never changes)
constexpr float FOG_ENTER_PCT = 98.0f;   // the newest closed hour's RH' = RH * 100 / S, >=
constexpr float FOG_STAY_PCT = 97.0f;    // live RH', >= (stateless exit margin)
constexpr int FOG_DRY_H = 3;             // no tip in the last 3 closed hours (nor the open one)
constexpr float SKY_SUN_K = 0.85f;       // "Sunny.": k = solar / (E x clear sky) >=, steady; "Sunny spells.": k_max >=
constexpr float SKY_SPELLS_K = 0.30f;    // "Sunny spells.": k >; "No sun.": k <=
constexpr float SKY_NOSUN_KMAX = 0.50f;  // "No sun.": k_max <
constexpr float SKY_EXIT_K = 0.50f;      // live solar vs E x clear sky: under it "Sunny." ends, over it "No sun." ends
constexpr int SKY_STEADY_PCT = 115;      // the hour's max/avg <= 115 % of the clear sky's (L1's gate at a known cell)
}  // namespace fcst

// what the outlook may read of the learners (RAM, calibrated units, written by the app after a learner tick
// under gLock; learnUseOf). A bit is set only at the learner's use level; with no bit set the fixed rules decide.
constexpr uint8_t LU_L1 = 1, LU_L2 = 2, LU_L5 = 4, LU_L6 = 8;
struct LearnUse {
  uint32_t hour;                  // the newest hour the learners consumed (its start); 0 = none
  uint8_t bits;                   // LU_*
  uint8_t sky_cls;                // L1: the L1b class at the hour's mid sun position (0 unknown, 1 sun, 2 shade);
                                  // 255 = no sky words (sun < 10 deg, no location, the dome gate)
  uint16_t sky_ref, sky_ref_max;  // L1: E x the clear sky of `hour`, mean / max (W/m²)
  uint16_t rh_sat;                // L2: the humidity this sensor shows in saturated air (0.1 %)
  int16_t cool;                   // L5: the clear-evening cooling threshold (0.01 °C/h, cooling positive)
  int16_t min_lo, min_hi;         // L6: tonight's minimum, rounded outward to whole °C (0.1 °C)
  uint32_t night_end;             // L6: the end of the night the range is for (unix); 0 = none
  // L7 (a notice, never the outlook): the learner's flag (0 none, 1 low, 2 high), stage (0 .. 2), days, the raw
  // mean minus the normal (0.1 hPa; the bridge's offset in force is added at render), METAR eligible (L7's notice silent)
  uint8_t l7_flag, l7_stage;
  uint16_t l7_days;
  int16_t l7_off;
  bool metar;
};
// A word over every field of the snapshot (the live ETag: a learner tick changes the outlook and the notices).
uint32_t learnUseSig(const LearnUse& u);

// Codes (API, MQTT attributes; English forever). NONE = null.
constexpr uint8_t FC_NONE = 0xFF;
// line 1 = state (<= 14 characters) + qual (<= 13), two sentences: <= 28 by construction (host test over every pair);
// the T3 states (the main airport's report, "reported nearby") are longer and never carry a qualifier
// (OUT_STATE_QUAL). Append-only: the T3 codes follow the first ones; the learners' codes after them: the day sky
// words (L1), the minimum range (L6), mist or fog possible (L2); then sleet_snow (a rain state at <= SLEET_T).
enum OutState : uint8_t { OS_DRY, OS_NIGHT_CLEAR, OS_RAIN_LIGHT, OS_RAIN_MODERATE, OS_RAIN_HEAVY, OS_RECENT_RAIN,
                          OS_SNOW_NEAR, OS_RAIN_NEAR, OS_DRIZZLE_NEAR, OS_DAY_SUNNY, OS_DAY_SPELLS, OS_DAY_NOSUN,
                          OS_SLEET_SNOW, OS_COUNT };
enum OutNext : uint8_t { ON_P_STEADY, ON_P_RISING, ON_P_RISING_FAST, ON_P_FALLING, ON_P_FALLING_FAST, ON_COOLING_FAST,
                         ON_COLLECTING, ON_P_PAUSED, ON_MIN_RANGE, ON_COUNT };
// with the T3 warnings and fog_possible. The priority is an explicit chain (outWarnRank), not the code order.
enum OutWarn : uint8_t { OW_ICY, OW_GUSTS, OW_HOAR_FROST, OW_GROUND_FROST, OW_THUNDER_NEAR, OW_FOG_NEAR, OW_MIST_NEAR,
                         OW_FOG_POSSIBLE, OW_COUNT };
enum OutGate : uint8_t { OG_TIME, OG_STALE, OG_COUNT };
// qualifier priority: severe_frost > freezing > hot > windy > humid > calm
enum OutQual : uint8_t { OQ_SEVERE_FROST, OQ_FREEZING, OQ_HOT, OQ_WINDY, OQ_HUMID, OQ_CALM, OQ_COUNT };
enum TendCode : uint8_t { TC_RISING, TC_STEADY, TC_FALLING };
extern const char* const OUT_STATE[OS_COUNT];
extern const char* const OUT_NEXT[ON_COUNT];
extern const char* const OUT_WARN[OW_COUNT];
extern const char* const OUT_GATE[OG_COUNT];
extern const char* const OUT_QUAL[OQ_COUNT];
// a state may carry the qualifier only when its table entry is at most 14 characters (never a T3 state)
extern const bool OUT_STATE_QUAL[OS_COUNT];
// the one warning line, by rank (lower wins): icy > thunder_near > gusts > hoar_frost > ground_frost >
// fog_near > (fog_likely: reserved) > fog_possible (L2) > mist_near > (uv_high: reserved)
int outWarnRank(uint8_t warn);

// T3: the main airport's report as admitted by nearFromStore (metar_store.h): what it reports, mapped to
// the strip's phenomena. No metar.h here: the outlook stays a pure function of plain input.
constexpr uint8_t NW_SNOW = 1u << 0, NW_RAIN = 1u << 1, NW_DRIZZLE = 1u << 2, NW_THUNDER = 1u << 3, NW_FOG = 1u << 4,
                  NW_MIST = 1u << 5;
struct OutNear {
  bool ok;          // admitted (module on, main airport, current, <= 60 km of the bridge's location)
  uint8_t wx;       // NW_* bits
  bool autom;       // an automatic (AUTO) report
  char icao[5];
  char name[25];
  uint16_t km;      // from the bridge's location, at admission
  uint32_t obs;     // the report's time (unix)
};

// One closed hour, calibrated (calHour); NaN = no value. `calm` = calmHour(); quality = HourRec.quality.
struct OutHour {
  uint32_t hour;  // unix start
  double p;       // hPa; double like tendency.js, so the 0.1 rounding agrees with FCT at every boundary
  float t, td, wind, rain, solar;
  float rh;       // the hour's mean humidity (%, calibrated), NaN = none; also L2's RH'
  uint16_t quality;
  bool calm;
  float solar_max;  // the hour's solar max (W/m²), NaN = none
};

struct OutlookIn {
  uint32_t now;           // the station's last upload (unix): the result is a function of what the live ETag covers
  bool time_ok;           // a real clock (SNTP / DHCP / browser), not the persisted one (the time gate)
  bool has_baro;          // the station ever sent pressure (no barometer, no outlook)
  bool stale;             // sensor_stale (the stale gate)
  float lat, lon;         // the bridge's location (tide, sun); NaN = none
  const OutHour* h;       // closed hours, oldest first (the newest <= WINDOW_H are read)
  int n;
  float open_rain;        // rain so far in the open hour (mm), NaN unknown
  float t, td;            // live
  float rate;             // live rain_rate_mmh, NaN = not sent
  float rain10;           // the daily counter's rise over the last 10 min (mm), NaN unknown
  float gust10;           // 10-min gust (gust_10m_kmh, else gust_kmh), NaN unknown
  float solar;            // live solar W/m², NaN = not sent, rejected or stale
  float rh;               // live humidity (%), NaN unknown (`humid`)
  float wind10;           // mean wind of the recent ring's last 10 min (km/h), NaN unknown (`windy`)
  OutNear near;           // T3: the admitted main-airport report (near.ok false = none)
  const LearnUse* use;    // the learners in use (nullptr or no bit = the fixed rules alone)
};

struct Trend {
  float d, raw;   // hPa, d rounded to 0.1 after the tide
  bool tide;
  uint8_t code;   // TendCode
  bool fast;
};

struct Outlook {
  bool present;            // false: omitted (no barometer)
  uint8_t gate, state, next, warn;  // codes or FC_NONE
  uint8_t qual;            // line 1's qualifier (OutQual) after the state; FC_NONE = silent
  uint8_t left_h;          // collecting: hours still missing
  bool has_tend;
  Trend tend;
  float gust;              // warn gusts: the gust (km/h), NaN otherwise
  uint32_t since;          // a rain state: start of the newest run of wet hours; 0 = none
  uint32_t as_of;          // the newest closed hour's end (the data basis); 0 = none
  uint32_t t;              // = in.now
  // the inputs behind the lines (the ⓘ explanation, outlook-why.js); NaN = unknown
  int8_t night;            // 1 night, 0 day, -1 unknown
  uint8_t night_src;       // 0 none, 1 sun (location), 2 solar sensor
  float in_t, in_td, rate, gust10, wind_h, dt_h, rain12, in_rh, wind10;
  bool calm;
  uint32_t cool_at;        // the clear-night cooling hour's start; 0 = none
  OutNear near;            // the source of a shown T3 code (near.ok false = no T3 code shows)
  // the learners whose learned value was used for a code (LU_*; 0 = conf "default", no "learn" key) and their values
  uint8_t learned;
  float cool_thr, rh_sat, rh_rel;  // °C/h (L5), % (L2: S and the hour's RH'); NaN unless used
  int8_t min_lo, min_hi;           // ON_MIN_RANGE: whole °C
};

// The FCT.trend port (tendency.js, same vectors): pressure means by timestamp, newest closed hour vs 3 h back, tide S2
// = 1.16 cos³(lat) at mid-hour, ±1.6, fast >= 3.6 when not steady, 0.1 rounding half away from zero. `now` = 0: no
// freshness gate. False when an end hour is missing or the newest is not fresh.
bool trend(const OutHour* h, int n, float lat, float lon, uint32_t now, Trend* out);
double s2Tide(double unix_s, float lat, float lon);

Outlook outlook(const OutlookIn& in);

// The station's input (calibrated hourly ring + live + flags), into `buf` (cap hours, the newest kept).
OutlookIn outlookInput(const Station& st, float lat, float lon, bool time_ok, OutHour* buf, int cap);
// The one hour buffer the app uses: the window + the hour before its oldest (that hour's change). The httpd's
// /api/live and the loop's MQTT pass both compute under gLock, so one buffer serves both.
constexpr int OUT_BUF_H = fcst::WINDOW_H + 1;
OutHour* outlookBuffer();
// the mean of a core field over the recent ring's readings in [now - span_s, now] (NaN = none); the outlook's
// wind10 and forwarding's 10-min mean wind.
float recentFieldMean(const Recent& rc, int field, uint32_t now, uint32_t span_s, int* n = nullptr);  // *n readings

// /api/live: "outlook":{...} (the caller writes the key). Metric.
void outlookJson(Out& o, const Outlook& x);
// HA state: the English phrases joined by " " (one code -> text table, like NOTICES), <= 255 B; "" when nothing.
int outlookState(char* buf, size_t cap, const Outlook& x);
// HA attributes: {"v":1,"state","next","warn","qual","conf","gate","tend","tend_fast","d3","as_of","near"} (codes only;
// "near" = the ICAO behind a shown T3 code, else null), + "min" [lo, hi] (whole °C) with min_range
int outlookAttributes(char* buf, size_t cap, const Outlook& x);
uint32_t outlookSig(const Outlook& x);  // FNV-1a over the codes + d (MQTT publishes on change)
// The firmware's English text per code (HA state); %s = the gust ("45 km/h") or the hours left
extern const char* const OUT_STATE_TEXT[OS_COUNT];
extern const char* const OUT_NEXT_TEXT[ON_COUNT];
extern const char* const OUT_WARN_TEXT[OW_COUNT];
extern const char* const OUT_QUAL_TEXT[OQ_COUNT];
extern const char* const OUT_GATE_TEXT[OG_COUNT];  // the HA state while gated ("" = the other lines speak)

}  // namespace fc
