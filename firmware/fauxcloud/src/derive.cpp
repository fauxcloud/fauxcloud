// derive.cpp -- see derive.h.
#include "derive.h"

#include <cmath>

#include "records.h"

namespace fc {

static bool inWindow(uint32_t t, uint32_t now, uint32_t window_s) { return t <= now && now - t < window_s; }

WindowSum recentSum(const Recent& r, int field, uint32_t now, uint32_t window_s) {
  WindowSum w = {0.0f, 0};
  for (int i = 0; i < r.size(); i++) {
    const PackedSample& p = r.at(i);
    if (!inWindow(p.t, now, window_s) || !hasValue(p.v[field])) continue;
    w.sum += unpack(field, p.v[field]);
    w.n++;
  }
  return w;
}

WindowValue recentMean(const Recent& r, int field, uint32_t now, uint32_t window_s) {
  const WindowSum w = recentSum(r, field, now, window_s);
  return WindowValue{w.n ? w.sum / (float)w.n : 0.0f, w.n};
}

WindowValue recentMax(const Recent& r, int field, uint32_t now, uint32_t window_s) {
  WindowValue m = {0.0f, 0};
  for (int i = 0; i < r.size(); i++) {
    const PackedSample& p = r.at(i);
    if (!inWindow(p.t, now, window_s) || !hasValue(p.v[field])) continue;
    const float x = unpack(field, p.v[field]);
    if (!m.n || x > m.v) m.v = x;
    m.n++;
  }
  return m;
}

// speed-weighted and calm-gated -- each direction counts with its reading's wind speed, a reading under
// CALM_KMH (or without a speed) not at all, so a parked vane in still air cannot steer the 2-min direction
WindowValue recentVectorMean(const Recent& r, int field, uint32_t now, uint32_t window_s) {
  const double PI = 3.14159265358979323846;
  double sx = 0, sy = 0;
  int n = 0;
  for (int i = 0; i < r.size(); i++) {
    const PackedSample& p = r.at(i);
    if (!inWindow(p.t, now, window_s) || !hasValue(p.v[field]) || !hasValue(p.v[F_WIND_KMH])) continue;
    const double w = unpack(F_WIND_KMH, p.v[F_WIND_KMH]);
    if (w < CALM_KMH) continue;
    const double x = unpack(field, p.v[field]);
    sx += w * std::sin(x * PI / 180.0);
    sy += w * std::cos(x * PI / 180.0);
    n++;
  }
  if (!n) return WindowValue{0.0f, 0};
  double deg = std::atan2(sx, sy) * 180.0 / PI;
  if (deg < 0) deg += 360.0;
  return WindowValue{(float)((int)std::lround(deg) % 360), n};
}

const DerivedDef WIND_DERIVED[WIND_DERIVED_N] = {
    {F_WIND_AVG2M_KMH, F_WIND_KMH, WinKind::Mean, WIND_AVG_S},
    {F_WIND_DIR_AVG2M, F_WIND_DIR, WinKind::VectorMean, WIND_AVG_S},
    {F_GUST_10M_KMH, F_GUST_KMH, WinKind::Max, GUST_MAX_S},
};

bool recentSpans(const Recent& r, uint32_t now, uint32_t window_s) {
  if (r.size() < RECENT_LEN) return true;
  const uint32_t oldest = r.at(0).t;
  return oldest <= now && now - oldest >= window_s;
}

}  // namespace fc
