// filter.cpp -- see filter.h.
#include "filter.h"

#include <cmath>
#include <cstring>

namespace fc {

static const char* const RULE_NAMES[R_COUNT] = {"parse",  "bounds", "profile_wind", "profile_humidity",
                                                "limits", "jump",   "rain_glitch"};

const char* ruleName(int rule) { return rule >= 0 && rule < R_COUNT ? RULE_NAMES[rule] : "?"; }

uint32_t Filtered::rejectedFields() const {
  uint32_t m = 0;
  for (int r = 0; r < R_COUNT; r++) m |= rule_fields[r];
  return m;
}

static float wrap360(float d) {
  float w = std::fmod(d, 360.0f);
  if (w < 0) w += 360.0f;
  if (w >= 359.5f) w = 0;  // so 359.6 never prints as 360
  return w;
}

void runFilters(const Sample& in, const StationConfig& cfg, uint32_t now, FilterState* st, Filtered* out) {
  memset(out, 0, sizeof *out);
  out->s = in;
  Sample& s = out->s;
  out->rule_fields[R_PARSE] = in.rejected_parse;
  out->rule_fields[R_BOUNDS] = in.rejected_bounds;
  const ProfileRules& pr = profileRules(cfg.profile);
  auto drop = [&](int f, int rule) {
    if (!s.has(f)) return;
    s.present &= ~(1u << f);
    out->rule_fields[rule] |= 1u << f;
  };

  // ---- profile rules
  if (pr.humidity_drop_pct > 0 && s.has(F_HUMIDITY) && s.v[F_HUMIDITY] < pr.humidity_drop_pct) {
    out->sample_dropped = true;
    out->rule_fields[R_PROFILE_HUMIDITY] = s.present;
    s.present = 0;
    return;  // a junk sample says nothing; the state of the last accepted values stays
  }
  if (pr.wind_max_kmh > 0 && ((s.has(F_WIND_KMH) && s.v[F_WIND_KMH] > pr.wind_max_kmh) ||
                              (s.has(F_GUST_KMH) && s.v[F_GUST_KMH] > pr.wind_max_kmh))) {
    drop(F_WIND_KMH, R_PROFILE_WIND);
    drop(F_GUST_KMH, R_PROFILE_WIND);
    drop(F_WIND_DIR, R_PROFILE_WIND);
  }
  if (s.has(F_WIND_DIR)) s.v[F_WIND_DIR] = wrap360(s.v[F_WIND_DIR]);  // every profile

  // ---- limits (metric, before calibration)
  for (int f = 0; f < CORE_FIELDS; f++) {
    if (!s.has(f)) continue;
    if ((!std::isnan(cfg.lim_min[f]) && s.v[f] < cfg.lim_min[f]) ||
        (!std::isnan(cfg.lim_max[f]) && s.v[f] > cfg.lim_max[f]))
      drop(f, R_LIMITS);
  }

  // ---- jump vs the last accepted value, scaled by the real interval
  for (int f = 0; f < CORE_FIELDS; f++) {
    if (!s.has(f) || cfg.jump_per_min[f] <= 0 || !((st->have >> f) & 1u)) continue;
    float dt = (float)(now - st->last_t[f]) / 60.0f;
    if (dt < JUMP_MIN_INTERVAL_MIN) dt = JUMP_MIN_INTERVAL_MIN;
    float d = std::fabs(s.v[f] - st->last_v[f]);
    if (f == F_WIND_DIR && d > 180.0f) d = 360.0f - d;
    if (d > cfg.jump_per_min[f] * dt) drop(f, R_JUMP);
  }
  for (int f = 0; f < CORE_FIELDS; f++) {
    if (!s.has(f)) continue;
    st->last_v[f] = s.v[f];
    st->last_t[f] = now;
    st->have |= 1u << f;
  }

  // ---- calibration (the history keeps the values before the factors and the offsets)
  for (int f = 0; f < CORE_FIELDS; f++) out->raw_v[f] = s.v[f];
  if (s.has(F_TEMP_C)) s.v[F_TEMP_C] += cfg.temp_c_offset;
  if (s.has(F_HUMIDITY)) s.v[F_HUMIDITY] = std::fmin(100.0f, std::fmax(0.0f, s.v[F_HUMIDITY] + cfg.humidity_offset));
  if (s.has(F_PRESSURE_HPA)) s.v[F_PRESSURE_HPA] += cfg.pressure_hpa_offset;
  if (s.has(F_WIND_DIR)) s.v[F_WIND_DIR] = wrap360(s.v[F_WIND_DIR] + cfg.wind_dir_offset);
  if (s.has(F_WIND_KMH)) s.v[F_WIND_KMH] *= cfg.wind_factor;
  if (s.has(F_GUST_KMH)) s.v[F_GUST_KMH] *= cfg.wind_factor;
  if (s.has(F_RAIN_MM)) s.v[F_RAIN_MM] *= cfg.rain_factor;
  if (s.has(F_RAIN_RATE_MMH)) s.v[F_RAIN_RATE_MMH] *= cfg.rain_factor;  // a rate profile's rainin
  if (s.has(F_RAIN_DAILY_MM)) s.v[F_RAIN_DAILY_MM] *= cfg.rain_factor;

  // ---- rain: the daily counter drives our own hourly/daily sums
  if (s.has(F_RAIN_DAILY_MM)) {
    const float daily = s.v[F_RAIN_DAILY_MM];
    if (st->have_daily) {
      float d = daily - st->last_daily;
      bool glitch = false;
      if (d < -0.05f) {
        if (!pr.rain_monotonic || daily <= RAIN_RESET_MAX_MM) {
          d = daily;  // midnight reset: all of it is new
          out->rain_reset = true;
        } else {
          glitch = true;
        }
      } else if (d < 0) {
        d = 0;  // rounding jitter
      } else if (d > RAIN_DELTA_MAX_MM) {
        glitch = true;  // a spike is not rain (and must not become the baseline the next values fall below)
      }
      if (glitch && st->rain_glitches + 1 >= RAIN_REBASE_AFTER) {
        // the counter stayed off the baseline: it is the console's truth now (a reset that already showed rain)
        out->rain_glitch = true;
        st->rain_glitches = 0;
        st->last_daily = daily;
      } else if (glitch) {
        out->rain_glitch = true;  // delta 0; the counter value itself is still published
        st->rain_glitches++;
      } else {
        st->rain_glitches = 0;
        out->rain_delta_valid = true;
        out->rain_delta_mm = d;
        st->last_daily = daily;
      }
    } else {
      st->have_daily = true;
      st->last_daily = daily;
    }
  }

  // ---- hidden fields (cfg.disabled) stay in the sample: recorded in recent / hourly / daily / snapshot,
  // so un-hiding shows the history without a gap; /api/live and MQTT skip them (web_render.cpp, app_mqtt.cpp)

  // ---- promotions: an extra parameter becomes a value
  for (int i = 0; i < MAX_PROMOTE; i++) {
    const Promote& p = cfg.promote[i];
    if (!p.used) continue;
    for (int k = 0; k < s.n_extras; k++) {
      if (strcmp(s.extras[k].key, p.param) != 0) continue;
      double x;
      if (parseNumber(s.extras[k].val, &x) && promotedOk(promoteConvert(p, x))) {  // finite and in range
        out->promoted[i] = (float)promoteConvert(p, x);
        out->promoted_mask |= (uint8_t)(1u << i);
      }
      break;
    }
  }
}

}  // namespace fc
