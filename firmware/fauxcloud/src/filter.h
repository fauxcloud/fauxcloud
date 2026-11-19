// filter.h -- the per-station filter chain. Plain C++, host-compilable.
//
// Order: parsed + converted (wu_parse) -> profile rules -> limits (metric) -> jump vs last accepted ->
// calibration -> rain counter -> disabled fields removed -> promotions. A rule only ever drops a field or the
// whole sample; the HTTP reply was sent long before. Runs in loop(), never in the httpd task.
#pragma once
#include <cstdint>

#include "config.h"
#include "wu_parse.h"

namespace fc {

enum Rule : uint8_t { R_PARSE, R_BOUNDS, R_PROFILE_WIND, R_PROFILE_HUMIDITY, R_LIMITS, R_JUMP, R_RAIN_GLITCH, R_COUNT };
const char* ruleName(int rule);  // "parse", "bounds", "profile_wind", ... (API keys, MQTT "rej_<name>")

constexpr float RAIN_RESET_MAX_MM = 0.5f;  // a drop of the daily counter to <= this is the midnight reset
// a rise of the daily counter above this in one upload is a glitch too (a "900 L/m2" spike), not rain;
// after RAIN_REBASE_AFTER glitches in a row the counter is taken as it is (a reset that already showed rain).
constexpr float RAIN_DELTA_MAX_MM = 100.0f;
constexpr uint8_t RAIN_REBASE_AFTER = 3;
constexpr float JUMP_MIN_INTERVAL_MIN = 10.0f / 60.0f;  // floor for the interval used to scale jump limits

// Last accepted values per station (pre-calibration, like the limits they are compared with).
// Written raw into the today snapshot (store.h): bump SNAP_VERSION in store.cpp when this layout changes.
struct FilterState {
  float last_v[CORE_FIELDS];
  uint32_t last_t[CORE_FIELDS];
  uint32_t have;     // field mask: last_v/last_t valid
  bool have_daily;
  uint8_t rain_glitches;  // glitches in a row (sits in have_daily's padding: the snapshot layout is unchanged)
  float last_daily;  // calibrated daily rain counter
};

struct Filtered {
  Sample s;                       // after the chain: `present` = accepted fields, values calibrated
  // the history's raw values -- s.v before the whole calibration step, i.e.
  // before the wind and rain factors and before the offsets (the calibration log corrects on read)
  float raw_v[CORE_FIELDS];
  bool rain_reset;                // the daily counter reset this upload (the midnight rule took it as all new)
  bool sample_dropped;            // profile_humidity: nothing of this upload is used
  uint32_t rule_fields[R_COUNT];  // per rule: fields it rejected in this upload (R_PROFILE_HUMIDITY: all present)
  bool rain_delta_valid;          // false on the first counter value after boot and on a glitch
  float rain_delta_mm;            // rain fallen since the previous upload, from the daily counter
  uint8_t promoted_mask;          // bit i: promote[i] has a value
  float promoted[MAX_PROMOTE];

  bool fired(int rule) const { return rule == R_RAIN_GLITCH ? rain_glitch : rule_fields[rule] != 0; }
  uint32_t rejectedFields() const;  // union over all rules
  bool rain_glitch;
};

void runFilters(const Sample& in, const StationConfig& cfg, uint32_t now, FilterState* st, Filtered* out);

}  // namespace fc
