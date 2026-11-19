// forecast.cpp -- see forecast.h (the local outlook).
#include "forecast.h"

#include <cmath>
#include <cstdio>
#include <cstring>

#include "calib.h"
#include "ha_discovery.h"
#include "records.h"
#include "station.h"
#include "sun.h"
#include "tz.h"
#include "web_render.h"

namespace fc {

const char* const OUT_STATE[OS_COUNT] = {"dry", "night_clear", "rain_light", "rain_moderate", "rain_heavy", "recent_rain",
                                         "wx_snow_near", "wx_rain_near", "wx_drizzle_near", "day_sunny", "day_spells", "day_nosun",
                                         "sleet_snow"};
const char* const OUT_NEXT[ON_COUNT] = {"p_steady", "p_rising", "p_rising_fast", "p_falling", "p_falling_fast",
                                        "cooling_fast", "collecting", "p_paused", "min_range"};
const char* const OUT_WARN[OW_COUNT] = {"icy", "gusts", "hoar_frost", "ground_frost", "thunder_near", "fog_near", "mist_near",
                                        "fog_possible"};
const char* const OUT_GATE[OG_COUNT] = {"time", "stale"};
const char* const OUT_QUAL[OQ_COUNT] = {"severe_frost", "freezing", "hot", "windy", "humid", "calm"};
static const char* const TEND_CODE[3] = {"rising", "steady", "falling"};

// The HA state's English (one table; the page's words are web/outlook.js, kept equal by test_table_sync.py)
const char* const OUT_STATE_TEXT[OS_COUNT] = {"No rain.", "Clear night.", "Light rain.", "Moderate rain.", "Heavy rain.",
                                              "Recent rain.", "Snow reported nearby.", "Rain reported nearby.",
                                              "Drizzle reported nearby.", "Sunny.", "Sunny spells.", "No sun.",
                                              "Sleet or snow."};
// the day sky words keep the qualifier (their own observation, <= 14 characters); the T3 states never do
const bool OUT_STATE_QUAL[OS_COUNT] = {true, true, true, true, true, true, false, false, false, true, true, true, true};
const char* const OUT_NEXT_TEXT[ON_COUNT] = {
    "Pressure steady.", "Pressure rising.", "Pressure rising fast.",
    "Pressure falling: less settled weather possible.", "Pressure falling fast: unsettled spell possible.",
    "Pressure steady, cooling fast.", "Pressure outlook in %s h.", "Pressure outlook paused (calibration or clock).",
    "Minimum around %s tonight."};
const char* const OUT_WARN_TEXT[OW_COUNT] = {"Icy surfaces possible.", "Gusts to %s.",
                                             "Hoar frost likely on cold surfaces.", "Ground frost possible.",
                                             "Thunder reported nearby.", "Fog reported nearby.", "Mist reported nearby.",
                                             "Mist or fog possible."};

int outWarnRank(uint8_t w) {
  switch (w) {
    case OW_ICY: return 0;
    case OW_THUNDER_NEAR: return 1;
    case OW_GUSTS: return 2;
    case OW_HOAR_FROST: return 3;
    case OW_GROUND_FROST: return 4;
    case OW_FOG_NEAR: return 5;
    // 6 fog_likely (reserved)
    case OW_FOG_POSSIBLE: return 7;  // L2: an inference, below the observed fog
    case OW_MIST_NEAR: return 8;
    // 9 uv_high (reserved)
    default: return 99;
  }
}
const char* const OUT_QUAL_TEXT[OQ_COUNT] = {"Severe frost.", "Freezing.", "Hot.", "Windy.", "Humid.", "Calm."};
const char* const OUT_GATE_TEXT[OG_COUNT] = {"Outlook waits for a valid clock.", "Outdoor sensor not updating."};

static bool has(float x) { return !std::isnan(x); }

// the S2 tide (hPa): 1.16 cos³(lat), maxima at 10:00 and 22:00 local mean solar time (tendency.js s2, tools/forecast_trial.py)
double s2Tide(double unix_s, float lat, float lon) {
  const double c = std::cos(lat * 3.14159265358979323846 / 180);
  return 1.16 * c * c * c * std::cos(2 * 3.14159265358979323846 * (unix_s / 3600.0 + lon / 15.0 - 10) / 12);
}

// tendency.js r1: sign(d) * Math.round(|d| * 10 + 1e-6) / 10 (Math.round = floor(x + 0.5)), in double like JS
static double r1(double d) {
  const double a = std::floor(std::fabs(d) * 10 + 1e-6 + 0.5) / 10;
  return d < 0 ? -a : a;
}
static bool hasP(double x) { return !std::isnan(x); }

static int hourAt(const OutHour* h, int n, uint32_t key) {
  for (int i = n - 1; i >= 0; i--)
    if (h[i].hour == key) return i;
  return -1;
}

bool trend(const OutHour* h, int n, float lat, float lon, uint32_t now, Trend* out) {
  int k = -1;
  for (int i = n - 1; i >= 0; i--)
    if (hasP(h[i].p) && (k < 0 || h[i].hour > h[k].hour)) k = i;
  if (k < 0) return false;
  const uint32_t t0 = h[k].hour, t1 = t0 - 3 * 3600u;
  const int j = hourAt(h, n, t1);
  if (j < 0 || !hasP(h[j].p)) return false;
  if (now && t0 + fcst::TEND_FRESH_S < now) return false;  // freshness gate: an old trend is no trend
  Trend r = {};
  const double raw = h[k].p - h[j].p;
  r.raw = (float)raw;
  r.tide = has(lat) && has(lon);
  const double tide = r.tide ? s2Tide(t0 + 1800.0, lat, lon) - s2Tide(t1 + 1800.0, lat, lon) : 0.0;
  r.d = (float)r1(raw - tide);
  r.code = r.d >= fcst::TEND_THR_HPA ? TC_RISING : r.d <= -fcst::TEND_THR_HPA ? TC_FALLING : TC_STEADY;
  r.fast = r.code != TC_STEADY && std::fabs(r.d) >= fcst::TEND_FAST_HPA;
  *out = r;
  return true;
}

// the change of the hourly mean temperature in the hour that starts at h[i] (vs the hour before, by timestamp)
static float dtOf(const OutHour* h, int n, int i) {
  const int j = hourAt(h, n, h[i].hour - 3600u);
  return j >= 0 && has(h[i].t) && has(h[j].t) ? h[i].t - h[j].t : NAN;
}

// L1: the day sky words on the newest closed hour, when the learners consumed it, by day, its cell at medium
// (LU_L1), not gated (sky_cls 255): k = solar / (E x clear sky), k_max = solar_max / (E x clear-sky max). "Sunny." needs
// a steady hour and a cell that is not shade; "No sun." a known sun cell (darkness in an unknown or shaded cell proves
// nothing). Stateless exits on the live solar. 0 = none (OS_DRY is 0: never returned).
static uint8_t skyWord(const OutlookIn& in, const LearnUse* u, const OutHour* nh, bool night) {
  if (!u || !(u->bits & LU_L1) || !nh || u->hour != nh->hour || u->sky_cls > 2 || night || !u->sky_ref ||
      !u->sky_ref_max || !has(nh->solar) || !has(nh->solar_max))
    return 0;
  const float k = nh->solar / u->sky_ref, kmax = nh->solar_max / u->sky_ref_max;
  const bool steady = nh->solar > 0 && nh->solar_max * u->sky_ref * 100.0f <= fcst::SKY_STEADY_PCT * u->sky_ref_max * nh->solar;
  const float exitW = fcst::SKY_EXIT_K * u->sky_ref;
  if (k >= fcst::SKY_SUN_K && steady && u->sky_cls != 2 && !(has(in.solar) && in.solar < exitW)) return OS_DAY_SUNNY;
  if (kmax >= fcst::SKY_SUN_K && k > fcst::SKY_SPELLS_K && k < fcst::SKY_SUN_K && u->sky_cls != 2) return OS_DAY_SPELLS;
  if (k <= fcst::SKY_SPELLS_K && kmax < fcst::SKY_NOSUN_KMAX && u->sky_cls == 1 && !(has(in.solar) && in.solar > exitW))
    return OS_DAY_NOSUN;
  return 0;
}

// L2: "Mist or fog possible." -- the newest closed hour's RH' = RH x 100 / S >= 98 % with light wind, no rain
// state (nor recent rain), no tip in the last 3 closed hours or the open one; held only while the live RH' >= 97 %
static bool fogPossible(const OutlookIn& in, const LearnUse* u, const OutHour* h, int n, const OutHour* nh, uint8_t state,
                        bool raining) {
  if (!u || !(u->bits & LU_L2) || !u->rh_sat || !nh || !has(nh->rh) || !has(in.rh)) return false;
  if (state != FC_NONE && state != OS_DRY && state != OS_NIGHT_CLEAR && state != OS_DAY_SUNNY &&
      state != OS_DAY_SPELLS && state != OS_DAY_NOSUN)
    return false;  // rain now or recent rain
  if (raining || (has(in.open_rain) && in.open_rain > 0)) return false;
  for (int i = 0; i < n; i++)
    if (h[i].hour + (uint32_t)fcst::FOG_DRY_H * 3600u > nh->hour && has(h[i].rain) && h[i].rain > 0) return false;
  const float s = u->rh_sat / 10.0f;
  return nh->rh * 100.0f / s >= fcst::FOG_ENTER_PCT && has(nh->wind) && nh->wind < fcst::CLEAR_WIND_KMH &&
         in.rh * 100.0f / s >= fcst::FOG_STAY_PCT;
}

// clear night: since `from`, a cooling hour (dt <= coolDt) with light wind, no later rise (>= WARM_DT) or rain, no rain now
static bool clearScan(const OutHour* h, int n, uint32_t from, float coolDt, bool raining, float openRain, uint32_t* at) {
  bool clear = false;
  *at = 0;
  for (int i = 0; i < n; i++) {
    if (h[i].hour < from) continue;
    const float dt = dtOf(h, n, i);
    if (has(dt) && dt <= coolDt && has(h[i].wind) && h[i].wind < fcst::CLEAR_WIND_KMH) {
      clear = true;
      *at = h[i].hour;
    }
    if (clear && h[i].hour > *at) {
      if ((has(dt) && dt >= fcst::WARM_DT) || (has(h[i].rain) && h[i].rain > 0)) clear = false, *at = 0;
    }
    if (clear && h[i].hour == *at && has(h[i].rain) && h[i].rain > 0) clear = false, *at = 0;
  }
  if (clear && (raining || (has(openRain) && openRain > 0))) clear = false, *at = 0;
  return clear;
}

Outlook outlook(const OutlookIn& in) {
  Outlook o = {};
  o.gate = o.state = o.next = o.warn = o.qual = FC_NONE;
  o.gust = NAN;
  o.in_t = in.t, o.in_td = in.td, o.gust10 = in.gust10, o.in_rh = in.rh, o.wind10 = in.wind10;
  o.rate = o.wind_h = o.dt_h = o.rain12 = NAN;
  o.cool_thr = o.rh_sat = o.rh_rel = NAN;
  o.night = -1;
  o.t = in.now;
  if (!in.has_baro) return o;  // the barometer gate: no barometer, no outlook
  // the learners in use (none: every rule below is the fixed one)
  const LearnUse* u = in.use && in.use->bits ? in.use : nullptr;
  const bool useL5 = u && (u->bits & LU_L5);
  const float coolDt = useL5 ? -u->cool / 100.0f : fcst::COOL_DT;
  o.present = true;
  if (!in.time_ok) {  // the time gate
    o.gate = OG_TIME;
    return o;
  }
  // the window: the newest WINDOW_H closed hours
  const OutHour* h = in.h;
  int n = in.n;
  if (n > fcst::WINDOW_H) h += n - fcst::WINDOW_H, n = fcst::WINDOW_H;
  const OutHour* nh = n ? &h[n - 1] : nullptr;  // the newest closed hour
  if (nh) {
    o.as_of = nh->hour + 3600u;
    o.wind_h = nh->wind;
    o.calm = nh->calm;
    o.dt_h = dtOf(h, n, n - 1);
  }

  // ---- night: the sun at `now`, else the solar sensor, else unknown (the night rules off)
  const SunState sun = sunAt(in.now, in.lat, in.lon);
  if (sun != SunState::Unknown) o.night = sun == SunState::Down, o.night_src = 1;
  else if (has(in.solar)) o.night = in.solar <= fcst::SOLAR_NIGHT_WM2, o.night_src = 2;
  const bool night = o.night == 1;

  // ---- rain now: the live rate, else the counter's rise over 10 min x 6
  o.rate = has(in.rate) ? in.rate : has(in.rain10) ? in.rain10 * 6 : NAN;
  const bool raining = has(o.rate) && o.rate > 0;
  float r12 = has(in.open_rain) ? in.open_rain : 0;
  for (int i = 0; i < n; i++)
    if (h[i].hour + 12 * 3600u > in.now && has(h[i].rain)) r12 += h[i].rain;
  o.rain12 = r12;

  // ---- clear night: since sunset (or the dark hours by the solar sensor), a cooling hour with light wind, no
  // later rise, no rain since
  bool clear = false, clearFix = false;  // clearFix = the fixed rule (COOL_DT), the frost warnings' gate
  if (night) {
    uint32_t from = 0;  // hours whose end is after sunset
    if (o.night_src == 1) {
      uint32_t rise = 0, set = 0;
      if (sunTimes(in.now, in.lat, in.lon, &rise, &set) == SunDay::Times) {
        if (in.now < rise) sunTimes(in.now - 86400u, in.lat, in.lon, &rise, &set);
        from = set > 3600u ? set - 3600u : 0;
      }
    } else {
      for (int i = n - 1; i >= 0 && has(h[i].solar) && h[i].solar <= fcst::SOLAR_NIGHT_WM2; i--) from = h[i].hour;
      if (!from) from = in.now;  // no dark hour known: nothing qualifies
    }
    clear = clearScan(h, n, from, coolDt, raining, in.open_rain, &o.cool_at);
    uint32_t atFix = 0;
    clearFix = useL5 ? clearScan(h, n, from, fcst::COOL_DT, raining, in.open_rain, &atFix) : clear;
  }

  // ---- state
  if (raining) {
    o.state = o.rate < fcst::RAIN_MODERATE ? OS_RAIN_LIGHT : o.rate <= fcst::RAIN_HEAVY ? OS_RAIN_MODERATE : OS_RAIN_HEAVY;
    // at or below SLEET_T (as shown) the gauge's precipitation may be sleet or snow: one state, no intensity word
    if (has(in.t) && r1(in.t) <= fcst::SLEET_T) o.state = OS_SLEET_SNOW;
    uint32_t since = nh ? nh->hour + 3600u : in.now - in.now % 3600u;  // the open hour
    for (int i = n - 1; i >= 0 && has(h[i].rain) && h[i].rain > 0 && h[i].hour + 3600u == since; i--) since = h[i].hour;
    o.since = since;
  } else if ((has(in.open_rain) && in.open_rain > 0) ||
             (nh && nh->hour + 3600u == in.now - in.now % 3600u && has(nh->rain) && nh->rain > 0)) {
    // no rain falling, but a tip in the open hour or the hour before (stored rows only: replay-stable, and it
    // survives a restart through the snapshot) -- ends the "No rain." flicker between sparse drizzle tips
    o.state = OS_RECENT_RAIN;
  } else if (clear) {
    o.state = OS_NIGHT_CLEAR;
  } else if (const uint8_t sky = skyWord(in, u, nh, night)) {
    o.state = sky;  // L1: rain > recent rain > the sky words > no rain (ranked as night_clear)
  } else if (!has(in.t) || r1(in.t) > fcst::BLIND_T) {
    o.state = OS_DRY;  // at or below BLIND_T the gauge may be blind (snow, sleet): no state, never "No rain."
  }
  // the qualifier, first that fires: severe_frost > freezing > hot > windy > humid > calm. The
  // frost bands on the value as shown (0.1 °C, so "0.0 °C" is always "Freezing."); hot, windy and humid
  // enter on the newest closed hour's mean and hold on the live value (stateless hysteresis)
  const double tq = has(in.t) ? r1(in.t) : NAN;
  const bool wet = o.state == OS_RAIN_LIGHT || o.state == OS_RAIN_MODERATE || o.state == OS_RAIN_HEAVY ||
                   o.state == OS_SLEET_SNOW || o.state == OS_RECENT_RAIN;
  if (tq <= fcst::SEVERE_FROST_T) o.qual = OQ_SEVERE_FROST;
  else if (tq <= fcst::FREEZING_T) o.qual = OQ_FREEZING;
  else if (tq >= fcst::HOT_T && nh && has(nh->t) && nh->t >= fcst::HOT_T) o.qual = OQ_HOT;
  else if (nh && has(nh->wind) && nh->wind >= fcst::WINDY_KMH && has(in.wind10) && in.wind10 >= fcst::WINDY_LIVE_KMH) o.qual = OQ_WINDY;
  else if (o.night == 0 && !wet && nh && has(nh->rh) && nh->rh >= fcst::HUMID_RH && has(in.rh) && in.rh >= fcst::HUMID_LIVE_RH)
    o.qual = OQ_HUMID;
  else if (nh && nh->calm) o.qual = OQ_CALM;

  // ---- next: collecting, paused, the pressure tendency, cooling fast
  int k = -1;
  for (int i = n - 1; i >= 0; i--)
    if (hasP(h[i].p)) { k = i; break; }
  int run = 0;
  for (int i = k; i >= 0 && hasP(h[i].p) && h[i].hour == h[k].hour - 3600u * (uint32_t)run; i--) run++;
  if (run < fcst::TEND_HOURS) {
    o.next = ON_COLLECTING;
    o.left_h = (uint8_t)(fcst::TEND_HOURS - run);
  } else {
    const uint16_t paused = (1u << Q_BARO_UNCAL) | (1u << Q_BARO_BOUNDARY) | (1u << Q_BARO_BRIDGE) | (1u << Q_CLOCK_APPROX);
    bool p = false;
    for (int i = k; i > k - fcst::TEND_HOURS; i--) p = p || (h[i].quality & paused);
    if (p) {
      o.next = ON_P_PAUSED;
    } else if (trend(h, n, in.lat, in.lon, in.now, &o.tend)) {
      o.has_tend = true;
      if (o.tend.code == TC_RISING) o.next = o.tend.fast ? ON_P_RISING_FAST : ON_P_RISING;
      else if (o.tend.code == TC_FALLING) o.next = o.tend.fast ? ON_P_FALLING_FAST : ON_P_FALLING;
      else if (night && has(o.dt_h) && o.dt_h <= coolDt) o.next = ON_COOLING_FAST;
      else o.next = ON_P_STEADY;
    }  // else: an end hour missing or not fresh -> no next line
  }

  // ---- one warning: icy > gusts > hoar_frost > ground_frost > fog_possible
  // the frost warnings keep the fixed clear-night rule (a safety rule never changes): a learned L5 threshold
  // moves the state and cooling_fast only; their gate = no rain state and the fixed rule's clear night
  const bool clearNight = clearFix && o.state != OS_RAIN_LIGHT && o.state != OS_RAIN_MODERATE && o.state != OS_RAIN_HEAVY &&
                          o.state != OS_SLEET_SNOW && o.state != OS_RECENT_RAIN;
  const bool light = nh && has(nh->wind) && nh->wind < fcst::CLEAR_WIND_KMH;
  if (has(in.t) && in.t <= fcst::ICY_T && (raining || r12 >= fcst::ICY_RAIN12_MM)) {
    o.warn = OW_ICY;
  } else if (has(in.gust10) && in.gust10 >= fcst::GUST_WARN_KMH) {
    o.warn = OW_GUSTS;
    o.gust = in.gust10;
  } else if (clearNight && light && has(in.t) && has(in.td) && in.t - fcst::HOAR_SPREAD <= in.td && in.td <= 0) {
    o.warn = OW_HOAR_FROST;
  } else if (clearNight && light && has(in.t) && in.t <= fcst::FROST_T) {
    o.warn = OW_GROUND_FROST;
  } else if (fogPossible(in, u, h, n, nh, o.state, raining)) {  // L2: an inference, after every frost warning
    o.warn = OW_FOG_POSSIBLE;
    o.rh_sat = u->rh_sat / 10.0f, o.rh_rel = nh->rh * 1000.0f / u->rh_sat;
  }

  // ---- T3: the main airport's report, scoped "reported nearby", after our own rules (they ran on
  // our own inputs: a T3 code never feeds a rule of ours), before the stale gate
  if (in.near.ok) {
    const uint8_t w = in.near.wx;
    const uint8_t t3 = (w & NW_SNOW) ? (uint8_t)OS_SNOW_NEAR : (w & NW_RAIN) ? (uint8_t)OS_RAIN_NEAR : (w & NW_DRIZZLE) ? (uint8_t)OS_DRIZZLE_NEAR : FC_NONE;
    // local first: rain_* / sleet_snow / recent_rain / night_clear and day_sunny / day_spells (sun here contradicts
    // precipitation here) keep the slot; T3 replaces `dry`, the gauge-blind null and day_nosun ("No sun." says nothing
    // about precipitation)
    bool shown = false;
    if (t3 != FC_NONE && (o.state == OS_DRY || o.state == FC_NONE || o.state == OS_DAY_NOSUN)) o.state = t3, shown = true;
    const uint8_t nw = (w & NW_THUNDER) ? (uint8_t)OW_THUNDER_NEAR : (w & NW_FOG) ? (uint8_t)OW_FOG_NEAR : (w & NW_MIST) ? (uint8_t)OW_MIST_NEAR : FC_NONE;
    if (nw != FC_NONE && outWarnRank(nw) < outWarnRank(o.warn)) {
      if (o.warn == OW_GUSTS) o.gust = NAN;
      if (o.warn == OW_FOG_POSSIBLE) o.rh_sat = o.rh_rel = NAN;  // fog_near displaces fog_possible
      o.warn = nw;
      shown = true;
    }
    if (shown) o.near = in.near;
  }
  if (o.state != FC_NONE && o.state < OS_COUNT && !OUT_STATE_QUAL[o.state]) o.qual = FC_NONE;  // no qualifier under T3

  if (in.stale) {  // the stale gate: the outdoor set is frozen; the console's barometer (next) is still valid
    o.gate = OG_STALE;
    o.state = o.warn = o.qual = FC_NONE;
    o.gust = NAN;
    o.since = 0;
    o.near = OutNear{};  // T3 too
    o.rh_sat = o.rh_rel = NAN;
  }
  // L6: tonight's minimum range, only on a clear night before the night's end and never over a pressure tendency
  if (u && (u->bits & LU_L6) && o.state == OS_NIGHT_CLEAR && in.now < u->night_end &&
      (o.next == ON_P_STEADY || o.next == ON_COOLING_FAST)) {
    o.next = ON_MIN_RANGE;
    o.min_lo = (int8_t)(u->min_lo / 10), o.min_hi = (int8_t)(u->min_hi / 10);
    o.learned |= LU_L6;
  }
  if (useL5 && (o.state == OS_NIGHT_CLEAR || o.next == ON_COOLING_FAST)) o.learned |= LU_L5, o.cool_thr = u->cool / 100.0f;
  if (o.warn == OW_FOG_POSSIBLE) o.learned |= LU_L2;
  if (o.state == OS_DAY_SUNNY || o.state == OS_DAY_SPELLS || o.state == OS_DAY_NOSUN) o.learned |= LU_L1;
  return o;
}

// ---------------------------------------------------------------- input from a station
OutHour* outlookBuffer() {
  static OutHour buf[OUT_BUF_H];  // callers hold gLock (see forecast.h)
  return buf;
}

OutlookIn outlookInput(const Station& st, float lat, float lon, bool time_ok, OutHour* buf, int cap) {
  OutlookIn in = {};
  in.now = st.last_seen;
  in.time_ok = time_ok;
  in.stale = st.sensor_stale;
  in.lat = lat, in.lon = lon;
  in.has_baro = (st.seen_fields >> F_PRESSURE_HPA) & 1u;
  const int from = st.hourly.size() > cap ? st.hourly.size() - cap : 0;
  int n = 0;
  for (int i = from; i < st.hourly.size(); i++) {
    HourRec r = st.hourly.at(i);
    calHour(&r, calFor(st.cal, r.hour));  // the calibrated rows (= /api/hourly)
    OutHour& x = buf[n++];
    x.hour = r.hour;
    x.t = hasI16(r.t_avg) ? r.t_avg / 10.0f : NAN;
    x.td = hasI16(r.td_avg) ? r.td_avg / 10.0f : NAN;
    x.p = hasI16(r.p_avg) ? r.p_avg / 10.0 : NAN;
    x.wind = hasU16(r.wind_avg) ? r.wind_avg / 10.0f : NAN;
    x.rain = hasU16(r.rain_sum) ? r.rain_sum / 100.0f : NAN;
    x.solar = hasU16(r.solar_avg) ? (float)r.solar_avg : NAN;
    x.solar_max = hasU16(r.solar_max) ? (float)r.solar_max : NAN;  // L1's k_max
    x.rh = hasU8(r.rh_avg) ? (float)r.rh_avg : NAN;  // `humid` (calibrated by calHour, as the tile shows); L2's RH'
    x.quality = r.quality;
    x.calm = calmHour(r);
    if (hasP(x.p)) in.has_baro = true;
  }
  in.h = buf;
  in.n = n;
  HourRec open;
  in.open_rain = NAN;
  if (st.hourly.openRecord(&open)) {
    calHour(&open, calFor(st.cal, open.hour));
    if (hasU16(open.rain_sum)) in.open_rain = open.rain_sum / 100.0f;
  }
  const Sample& v = st.live;
  in.t = v.has(F_TEMP_C) ? v.v[F_TEMP_C] : NAN;
  in.td = v.has(F_DEWPT_C) ? v.v[F_DEWPT_C] : NAN;
  in.rate = v.has(F_RAIN_RATE_MMH) ? v.v[F_RAIN_RATE_MMH] : NAN;
  in.gust10 = v.has(F_GUST_10M_KMH) ? v.v[F_GUST_10M_KMH] : v.has(F_GUST_KMH) ? v.v[F_GUST_KMH] : NAN;
  in.solar = v.has(F_SOLAR_WM2) && !((st.live_rejected >> F_SOLAR_WM2) & 1u) && !st.sensor_stale ? v.v[F_SOLAR_WM2] : NAN;
  // the daily counter's rise over the last 10 min (recent ring, same day: a drop = the midnight reset -> unknown)
  in.rain10 = NAN;
  const Recent& rc = st.recent;
  float newest = NAN, oldest = NAN;
  bool drop = false;  // any step down inside the window (0 -> x -> 0 is a reset, not "no rain")
  for (int i = rc.size() - 1; i >= 0; i--) {
    const PackedSample& p = rc.at(i);
    if (p.t + 600u < in.now || p.t > in.now) break;
    if (!hasValue(p.v[F_RAIN_DAILY_MM])) continue;
    const float x = unpack(F_RAIN_DAILY_MM, p.v[F_RAIN_DAILY_MM]);
    if (!has(newest)) newest = x;
    else if (x > oldest) drop = true;
    oldest = x;
  }
  if (has(newest) && !drop) in.rain10 = newest - oldest;
  in.rh = v.has(F_HUMIDITY) ? v.v[F_HUMIDITY] : NAN;
  // the mean wind of the last 10 min (recent ring; the `windy` qualifier's live value)
  in.wind10 = recentFieldMean(rc, F_WIND_KMH, in.now, 600);
  in.use = st.use.bits ? &st.use : nullptr;  // the learners' snapshot (the caller holds gLock)
  return in;
}

// shared with forwarding (its wind10 is the same number); float sum, newest first
float recentFieldMean(const Recent& rc, int field, uint32_t now, uint32_t span_s, int* n) {
  float ws = 0;
  int wn = 0;
  for (int i = rc.size() - 1; i >= 0; i--) {
    const PackedSample& p = rc.at(i);
    if (p.t + span_s < now || p.t > now) break;
    if (hasValue(p.v[field])) ws += unpack(field, p.v[field]), wn++;
  }
  if (n) *n = wn;
  return wn ? ws / wn : NAN;
}

// ---------------------------------------------------------------- output
static void code(Out& o, const char* key, uint8_t c, const char* const* table, int count) {
  o.printf(",\"%s\":", key);
  if (c == FC_NONE || c >= count) o.raw("null");
  else o.printf("\"%s\"", table[c]);
}
static void num(Out& o, const char* key, float x, int dec) {
  o.printf(",\"%s\":", key);
  if (!has(x)) o.raw("null");
  else o.printf("%.*f", dec, (double)x);
}
static void iso(Out& o, const char* key, uint32_t t) {
  o.printf(",\"%s\":", key);
  if (t < EPOCH_MIN) {
    o.raw("null");
    return;
  }
  char b[24];
  formatIsoUtc(b, sizeof b, t);
  o.printf("\"%s\"", b);
}

void outlookJson(Out& o, const Outlook& x) {
  o.raw("{\"v\":1");
  iso(o, "t", x.t);
  iso(o, "as_of", x.as_of);
  code(o, "gate", x.gate, OUT_GATE, OG_COUNT);
  code(o, "state", x.state, OUT_STATE, OS_COUNT);
  code(o, "next", x.next, OUT_NEXT, ON_COUNT);
  code(o, "warn", x.warn, OUT_WARN, OW_COUNT);
  code(o, "qual", x.qual, OUT_QUAL, OQ_COUNT);
  o.raw(x.learned ? ",\"conf\":\"learned\",\"tend\":" : ",\"conf\":\"default\",\"tend\":");
  if (x.has_tend)
    o.printf("{\"d\":%.1f,\"raw\":%.2f,\"tide\":%s,\"code\":\"%s\",\"fast\":%s}", (double)x.tend.d, (double)x.tend.raw,
             x.tend.tide ? "true" : "false", TEND_CODE[x.tend.code], x.tend.fast ? "true" : "false");
  else o.raw("null");
  o.printf(",\"left_h\":%u,\"p\":{\"gust\":", (unsigned)x.left_h);
  if (has(x.gust)) o.printf("%.1f", (double)x.gust);
  else o.raw("null");
  o.raw(",\"since\":");
  if (x.since >= EPOCH_MIN) {
    char b[24];
    formatIsoUtc(b, sizeof b, x.since);
    o.printf("\"%s\"", b);
  } else o.raw("null");
  if (x.next == ON_MIN_RANGE) o.printf(",\"min_lo\":%d,\"min_hi\":%d", x.min_lo, x.min_hi);  // L6 (whole °C)
  o.raw("},\"in\":{\"night\":");
  o.raw(x.night < 0 ? "null" : x.night ? "true" : "false");
  o.printf(",\"night_src\":\"%s\"", x.night_src == 1 ? "sun" : x.night_src == 2 ? "solar" : "none");
  num(o, "t", x.in_t, 1);
  num(o, "td", x.in_td, 1);
  num(o, "rate", x.rate, 1);
  num(o, "gust10", x.gust10, 1);
  num(o, "wind_h", x.wind_h, 1);
  o.printf(",\"calm\":%s", x.calm ? "true" : "false");
  num(o, "dt_h", x.dt_h, 1);
  num(o, "rain12", x.rain12, 2);
  num(o, "rh", x.in_rh, 0);
  num(o, "wind10", x.wind10, 1);
  iso(o, "cool_at", x.cool_at);
  o.raw(",\"near\":");  // the source of a shown T3 code (the ⓘ sentence)
  if (x.near.ok) {
    o.printf("{\"icao\":\"%s\",\"name\":", x.near.icao);
    o.jsonStr(x.near.name);
    o.printf(",\"km\":%u,\"age_s\":%lu,\"auto\":%s}", (unsigned)x.near.km,
             (unsigned long)(x.t > x.near.obs ? x.t - x.near.obs : 0), x.near.autom ? "true" : "false");
  } else {
    o.raw("null");
  }
  if (x.learned) {  // the learners whose value was used for a code, and those values (absent otherwise)
    static const char* const LN[4] = {"l1", "l2", "l5", "l6"};
    o.raw(",\"learn\":[");
    for (int b = 0, k = 0; b < 4; b++)
      if (x.learned >> b & 1) o.printf("%s\"%s\"", k++ ? "," : "", LN[b]);
    o.raw("]");
    if (has(x.cool_thr)) num(o, "cool_thr", x.cool_thr, 2);
    if (has(x.rh_sat)) num(o, "rh_sat", x.rh_sat, 1), num(o, "rh_rel", x.rh_rel, 1);
  }
  o.raw("}}");
}

int outlookState(char* buf, size_t cap, const Outlook& x) {
  if (!cap) return -1;
  buf[0] = 0;
  size_t n = 0;
  char tmp[96], arg[16];
  const auto add = [&](const char* fmt, const char* a) {
    const int k = a ? snprintf(tmp, sizeof tmp, fmt, a) : snprintf(tmp, sizeof tmp, "%s", fmt);
    if (k <= 0) return;
    const int w = snprintf(buf + n, cap - n, "%s%s", n ? " " : "", tmp);
    if (w > 0 && n + (size_t)w < cap) n += (size_t)w;
  };
  if (x.gate < OG_COUNT && OUT_GATE_TEXT[x.gate][0]) add(OUT_GATE_TEXT[x.gate], nullptr);
  if (x.state < OS_COUNT) add(OUT_STATE_TEXT[x.state], nullptr);
  if (x.qual < OQ_COUNT) add(OUT_QUAL_TEXT[x.qual], nullptr);
  // the pressure line's status codes are left out when another sentence speaks (they are not weather); alone they
  // keep the entity from going blank. The codes stay in the attributes.
  const bool status = x.next == ON_COLLECTING || x.next == ON_P_PAUSED;
  if (x.next < ON_COUNT && !(status && n)) {
    char rg[40];
    snprintf(arg, sizeof arg, "%u", (unsigned)x.left_h);
    if (x.next == ON_MIN_RANGE) {  // L6: "−9 to −5 °C" (a real minus sign, metric)
      char lo[8], hi[8];
      const auto t = [](char* b, size_t c, int v) { snprintf(b, c, v < 0 ? "\xE2\x88\x92%d" : "%d", v < 0 ? -v : v); };
      t(lo, sizeof lo, x.min_lo), t(hi, sizeof hi, x.min_hi);
      snprintf(rg, sizeof rg, "%s to %s \xC2\xB0" "C", lo, hi);
    }
    add(OUT_NEXT_TEXT[x.next], x.next == ON_COLLECTING ? arg : x.next == ON_MIN_RANGE ? rg : nullptr);
  }
  if (x.warn < OW_COUNT) {
    snprintf(arg, sizeof arg, "%.0f km/h", has(x.gust) ? (double)x.gust : 0.0);
    add(OUT_WARN_TEXT[x.warn], x.warn == OW_GUSTS ? arg : nullptr);
  }
  return (int)n;
}

int outlookAttributes(char* buf, size_t cap, const Outlook& x) {
  const auto c = [](uint8_t v, const char* const* t, int count) { return v == FC_NONE || v >= count ? nullptr : t[v]; };
  const char* s = c(x.state, OUT_STATE, OS_COUNT);
  const char* nx = c(x.next, OUT_NEXT, ON_COUNT);
  const char* w = c(x.warn, OUT_WARN, OW_COUNT);
  const char* g = c(x.gate, OUT_GATE, OG_COUNT);
  const char* tb = c(x.qual, OUT_QUAL, OQ_COUNT);
  char asof[24] = "";
  if (x.as_of >= EPOCH_MIN) formatIsoUtc(asof, sizeof asof, x.as_of);
  char d3[16] = "null";
  if (x.has_tend) snprintf(d3, sizeof d3, "%.1f", (double)x.tend.d);
  char mn[32] = "";  // L6: ,"min":[lo,hi] with min_range only (absent otherwise)
  if (x.next == ON_MIN_RANGE) snprintf(mn, sizeof mn, ",\"min\":[%d,%d]", x.min_lo, x.min_hi);
  const int k = snprintf(buf, cap,
                         "{\"v\":1,\"state\":%s%s%s,\"next\":%s%s%s,\"warn\":%s%s%s,\"qual\":%s%s%s,\"conf\":\"%s\",\"gate\":%s%s%s,"
                         "\"tend\":%s%s%s,\"tend_fast\":%s,\"d3\":%s,\"as_of\":%s%s%s,\"near\":%s%s%s%s}",
                         s ? "\"" : "", s ? s : "null", s ? "\"" : "", nx ? "\"" : "", nx ? nx : "null", nx ? "\"" : "",
                         w ? "\"" : "", w ? w : "null", w ? "\"" : "", tb ? "\"" : "", tb ? tb : "null", tb ? "\"" : "",
                         x.learned ? "learned" : "default", g ? "\"" : "", g ? g : "null", g ? "\"" : "",
                         x.has_tend ? "\"" : "", x.has_tend ? TEND_CODE[x.tend.code] : "null", x.has_tend ? "\"" : "",
                         x.has_tend && x.tend.fast ? "true" : "false", d3, asof[0] ? "\"" : "", asof[0] ? asof : "null",
                         asof[0] ? "\"" : "", x.near.ok ? "\"" : "", x.near.ok ? x.near.icao : "null", x.near.ok ? "\"" : "", mn);
  return k < 0 || (size_t)k >= cap ? -1 : k;
}

uint32_t outlookSig(const Outlook& x) {
  uint32_t h = 2166136261u;
  const auto mix = [&](uint32_t v) {
    for (int b = 0; b < 4; b++) h = (h ^ ((v >> (8 * b)) & 0xffu)) * 16777619u;
  };
  mix(x.present), mix(x.gate), mix(x.state), mix(x.next), mix(x.warn), mix(x.left_h), mix(x.qual);
  mix(x.has_tend ? (uint32_t)(int32_t)std::lround(x.tend.d * 10) : 0x7fffffffu);
  mix(has(x.gust) ? (uint32_t)std::lround(x.gust) : 0u);
  for (int i = 0; x.near.ok && i < 4 && x.near.icao[i]; i++) mix((uint8_t)x.near.icao[i]);  // the T3 source
  if (x.next == ON_MIN_RANGE) mix((uint32_t)(uint8_t)x.min_lo << 8 | (uint8_t)x.min_hi);  // min_range only
  return h;
}

uint32_t learnUseSig(const LearnUse& u) {
  // field by field (the struct has padding); FNV-1a over the words
  const uint32_t w[8] = {u.hour,
                         (uint32_t)u.bits | (uint32_t)u.sky_cls << 8 | (uint32_t)u.l7_flag << 16 | (uint32_t)u.l7_stage << 24,
                         (uint32_t)u.sky_ref | (uint32_t)u.sky_ref_max << 16,
                         (uint32_t)u.rh_sat | (uint32_t)(uint16_t)u.cool << 16,
                         (uint32_t)(uint16_t)u.min_lo | (uint32_t)(uint16_t)u.min_hi << 16,
                         u.night_end,
                         (uint32_t)u.l7_days | (uint32_t)(uint16_t)u.l7_off << 16,
                         u.metar ? 1u : 0u};
  uint32_t h = 2166136261u;
  for (uint32_t v : w)
    for (int b = 0; b < 4; b++) h = (h ^ ((v >> (8 * b)) & 0xffu)) * 16777619u;
  return h;
}

}  // namespace fc
