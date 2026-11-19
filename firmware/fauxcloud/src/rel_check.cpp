// rel_check.cpp -- see rel_check.h (the REL hint).
#include "rel_check.h"

#include <cmath>
#include <cstring>

#include "calib.h"
#include "forecast.h"
#include "metar_store.h"  // METAR_CURRENT_S (constants only: no airport symbol is referenced here)
#include "records.h"
#include "station.h"

namespace fc {

void relReset(RelState* r) {
  const uint32_t seen = r->seen_t;
  *r = RelState{};
  r->seen_t = seen;
}

void relSeen(RelState* r, uint32_t ref_t) {
  if (ref_t > r->seen_t) r->seen_t = ref_t;
}

void relStep(RelState* r, int16_t d, int16_t qnh, uint32_t ref_t) {
  if (!ref_t || ref_t <= r->seen_t) return;
  r->seen_t = r->last_t = r->ref_t = ref_t;
  r->qnh_dhpa = qnh;
  for (int i = REL_HINT_N - 1; i > 0; i--) r->d[i] = r->d[i - 1];
  r->d[0] = d;
  const int a = d < 0 ? -d : d;
  const int8_t sg = d < 0 ? -1 : 1;
  if (a < REL_EXIT_DHPA) {  // a re-calibration clears within one report
    r->n = 0, r->sign = 0, r->active = false, r->since = 0;
    return;
  }
  if (r->n && sg != r->sign) {  // a sign flip: the run restarts (and an active hint ends)
    r->active = false;
    r->n = 0;
  }
  if (a < REL_HINT_DHPA) {  // 1.5 .. 2.0: an active run holds; an inactive one is broken
    if (!r->active) r->n = 0, r->sign = 0, r->since = 0;
    return;
  }
  if (!r->n) r->since = ref_t, r->sign = sg;
  if (r->n < 255) r->n++;
  if (r->n >= REL_HINT_N) r->active = true;
}

void relExpire(RelState* r, uint32_t now) {
  if (r->last_t && now > r->last_t && now - r->last_t > REL_SILENCE_S) relReset(r);
}

bool relShown(const RelState& r, uint32_t now) {
  return r.active && !(now > r.last_t && now - r.last_t > REL_SILENCE_S);
}

int16_t relMedian(const RelState& r) {
  const int k = r.n < REL_HINT_N ? r.n : REL_HINT_N;
  if (k <= 0) return r.d[0];
  int16_t v[REL_HINT_N];
  memcpy(v, r.d, sizeof v);
  for (int i = 1; i < k; i++)  // insertion sort of k <= 3
    for (int j = i; j > 0 && v[j] < v[j - 1]; j--) {
      const int16_t t = v[j];
      v[j] = v[j - 1];
      v[j - 1] = t;
    }
  return k == 2 ? (int16_t)((v[0] + v[1]) / 2) : v[k / 2];
}

int16_t relTarget(const RelState& r, float offset_hpa, uint32_t now) {
  if (!r.ref_t || r.ref_t > now || now - r.ref_t > METAR_CURRENT_S) return REL_NONE;
  return (int16_t)lroundf(r.qnh_dhpa - offset_hpa * 10.0f);
}

uint32_t relSig(const RelState& r) {
  if (!r.active) return 0;
  uint32_t h = 2166136261u;
  const uint32_t w[3] = {r.since, r.ref_t, (uint32_t)(uint16_t)relMedian(r)};
  for (uint32_t v : w)
    for (int b = 0; b < 4; b++) h = (h ^ ((v >> (8 * b)) & 0xffu)) * 16777619u;
  return h | 1u;
}

// the mean of one field over [from, to] of the recent ring, newest first -- samples newer than `to`
// are skipped (the METAR job sees a report 14-15 min old, so the ring's newest uploads are always past the window);
// stops at the first sample older than `from`. recentFieldMean() (forwarding's wind10) is left as it is.
static float ringMeanIn(const Recent& rc, int field, uint32_t from, uint32_t to, int* n) {
  float s = 0;
  int k = 0;
  for (int i = rc.size() - 1; i >= 0; i--) {
    const PackedSample& p = rc.at(i);
    if (p.t > to) continue;
    if (p.t < from) break;
    if (hasValue(p.v[field])) s += unpack(field, p.v[field]), k++;
  }
  *n = k;
  return k ? s / k : NAN;
}

bool relStationValue(const Station& st, float lat, float lon, uint32_t ref_t, float* p) {
  // no sensor_stale term -- the pressure is the console's, not the outdoor set's; the ring window
  // already needs uploads around the report
  if (st.baro_uncal || !((st.seen_fields >> F_PRESSURE_HPA) & 1u)) return false;
  const Outlook o = outlook(outlookInput(st, lat, lon, true, outlookBuffer(), OUT_BUF_H));
  if (!o.has_tend || !(std::fabs(o.tend.d) < fcst::TEND_THR_HPA)) return false;  // no front between us and the airport
  int n = 0;
  const float m = ringMeanIn(st.recent, F_PRESSURE_HPA, ref_t - REL_WINDOW_S, ref_t + REL_WINDOW_S, &n);
  if (!n || m != m) return false;
  *p = m;
  return true;
}

bool lastGoodPressure(const Station& st, float* p, uint32_t* t) {
  const uint32_t now = st.last_seen;
  if (!st.good_t || (now >= st.good_t && now - st.good_t > REL_LAST_GOOD_S)) return false;
  *p = st.good_p, *t = st.good_t;
  return true;
}

bool lastGoodFromHours(const Station& st, float* p, uint32_t* t) {
  const uint32_t now = st.last_seen;
  // after a reboot: the newest closed hour without the factory mark, not at the factory value itself, and not just
  // before such an hour (the BARO_UNCAL_N readings before the mark carry none and may share an hour with good ones);
  // raw p_avg, before the calibration
  const float factory = profileRules(st.cfg.profile).baro_factory_hpa;
  bool newerBad = false;
  for (int i = st.hourly.size() - 1; i >= 0; i--) {
    HourRec r = st.hourly.at(i);
    const uint32_t end = r.hour + 3600u;
    if (now > end && now - end > REL_LAST_GOOD_S) break;
    const bool bad = (r.quality & (1u << Q_BARO_UNCAL)) ||
                     (hasI16(r.p_avg) && factory == factory && std::fabs(r.p_avg / 10.0f - factory) <= 0.1f);
    const bool skip = bad || newerBad;
    newerBad = bad;
    if (skip) continue;
    calHour(&r, calFor(st.cal, r.hour));
    if (!hasI16(r.p_avg)) continue;
    *p = r.p_avg / 10.0f, *t = end;
    return true;
  }
  return false;
}

}  // namespace fc
