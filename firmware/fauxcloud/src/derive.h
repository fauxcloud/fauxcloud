// derive.h -- fields the console does not send, computed over the recent ring by time window.
// Box means and sums over (now - window, now], no EMA. Plain C++, host-compilable.
#pragma once
#include <cstdint>

#include "history.h"

namespace fc {

constexpr uint32_t RAIN_HOUR_S = 3600;  // the derived rain_mm ("Rain (last hour)")
constexpr uint32_t RAIN_SPAN_MIN_S = 3300;  // the ring must reach back this far for the hour to count as warm
// a previous hour with fewer readings (75 % of 3600 / 65 s, the 7-in-1's cadence, the
// only rate profile) is not used for the "last hour" estimate after a restart
constexpr uint8_t PROFILE_HOUR_N_MIN = 42;

struct WindowSum {
  float sum;
  int n;  // readings in the window that carry the field
};
// The sum of a core field over the ring's readings with now - window_s < t <= now.
WindowSum recentSum(const Recent& r, int field, uint32_t now, uint32_t window_s);
// the derived wind fields -- the 2-min mean of wind_kmh, the 2-min vector mean of wind_dir (the
// hourly tier's method), the 10-min max of gust_kmh. n = 0: no reading in the window, v undefined.
constexpr uint32_t WIND_AVG_S = 120, GUST_MAX_S = 600;
struct WindowValue {
  float v;
  int n;
};
WindowValue recentMean(const Recent& r, int field, uint32_t now, uint32_t window_s);
WindowValue recentMax(const Recent& r, int field, uint32_t now, uint32_t window_s);
// degrees, 0..359; weighted by the reading's wind speed, readings under CALM_KMH left out: n = 0 -> none
WindowValue recentVectorMean(const Recent& r, int field, uint32_t now, uint32_t window_s);
enum class WinKind : uint8_t { Mean, VectorMean, Max };
struct DerivedDef {
  uint8_t field, source;  // the WU field filled, from this core field of the ring
  WinKind kind;
  uint32_t window_s;
};
constexpr int WIND_DERIVED_N = 3;
extern const DerivedDef WIND_DERIVED[WIND_DERIVED_N];  // windspdmph_avg2m, winddir_avg2m, windgustmph_10m
// The ring still holds the whole window: not full yet, or its oldest reading is at least window_s old.
bool recentSpans(const Recent& r, uint32_t now, uint32_t window_s);

}  // namespace fc
