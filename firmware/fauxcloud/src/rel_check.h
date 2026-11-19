// rel_check.h -- the REL hint. The console uploads REL (ABS +
// the user's offset); when it reads off against the nearby airports' QNH on 3 admitted reports in a row, the pressure
// tile says so with the number to type into the console. Pure and host-tested; no airport type here (the reference
// side is metar_store.h's relReference, compiled out with FWD_TLS=0). RAM only, empty after a reboot.
#pragma once
#include <cstdint>

namespace fc {

struct Station;

// The three thresholds (one place, no setting, no learner default). 2.0, not 1.0: ICAO QNH is rounded down (mean -0.5),
// the real gradient over 30-60 km is often 0.5-1.5 hPa and the console steps 0.34, so 1.0 would fire on good consoles
constexpr int16_t REL_HINT_DHPA = 20;      // enter: |station - QNH| >= 2.0 hPa ...
constexpr uint8_t REL_HINT_N = 3;          // ... on 3 admitted reports in a row, the same sign
constexpr int16_t REL_EXIT_DHPA = 15;      // exit: the first admitted report under 1.5 hPa
constexpr uint32_t REL_SAMPLE_S = 3600;    // a reference report <= 60 min old (internal: the recent ring holds ~65 min)
constexpr uint32_t REL_SILENCE_S = 10800;  // no admitted report for 3 h: exit (silence, not "fixed")
constexpr uint32_t REL_WINDOW_S = 300;     // the station's value: the recent ring's mean over +-5 min of the report time
constexpr uint32_t REL_LAST_GOOD_S = 10800;  // baro_uncalibrated's "last good value" shows when <= 3 h old
constexpr int16_t REL_NONE = INT16_MIN;    // a Notice parameter that is absent

struct RelState {
  uint32_t seen_t;   // the newest reference report already stepped (once per report)
  uint32_t last_t;   // the newest admitted report (the silence clock)
  uint32_t since;    // the first report of the current run
  uint32_t ref_t;    // the report behind d / qnh (the target number's age)
  int16_t d[REL_HINT_N];  // station - QNH (0.1 hPa), newest first
  int16_t qnh_dhpa;  // the reference of the newest admitted report
  uint8_t n;         // reports in the run (saturates)
  int8_t sign;       // the run's sign, 0 = none
  bool active;       // the notice may show
};

void relReset(RelState* r);  // seen_t kept: the same report is not stepped twice
// One admitted report (d = the station as shown - QNH, 0.1 hPa): enter on REL_HINT_N same-sign reports >= REL_HINT_DHPA,
// a sign flip restarts the run, the first one under REL_EXIT_DHPA clears. Between the two (1.5 .. 2.0) an active run
// holds (hysteresis) and an inactive one restarts. A report at or before seen_t is ignored.
void relStep(RelState* r, int16_t d_dhpa, int16_t qnh_dhpa, uint32_t ref_t);
// A report that was seen but not admitted (the station side said no): only marks it seen.
void relSeen(RelState* r, uint32_t ref_t);
// The 3-h silence: clears the run when the newest admitted report is older than REL_SILENCE_S at `now`.
void relExpire(RelState* r, uint32_t now);
bool relShown(const RelState& r, uint32_t now);    // active and not silent at `now`
int16_t relMedian(const RelState& r);              // the median of the last min(n, 3) d values
// The target: QNH - the bridge's pressure offset (the console's LCD shows REL without our offset), 0.1 hPa; REL_NONE
// unless the reference report is current (METAR_CURRENT_S) at `now`.
int16_t relTarget(const RelState& r, float offset_hpa, uint32_t now);
// A change word for the live ETag (the job steps between uploads)
uint32_t relSig(const RelState& r);

// The station side of an admitted sample for a report at ref_t: pressure seen, the outdoor set not stale, not baro_uncal,
// its own 3-h tendency steady (|d| < TEND_THR_HPA, which also needs the last 3 closed hours free of Q_BARO_* /
// Q_CLOCK_APPROX: a paused tendency is none), and the recent ring's calibrated mean over ref_t +- REL_WINDOW_S.
// The caller holds gLock (outlookBuffer).
bool relStationValue(const Station& st, float lat, float lon, uint32_t ref_t, float* p_hpa);

// baro_uncalibrated's last good value: good_p / good_t (the calibrated pressure of the last upload not at
// the factory value), only when <= REL_LAST_GOOD_S old at the last upload. Cheap: stationNotices runs every second.
bool lastGoodPressure(const Station& st, float* p_hpa, uint32_t* t);
// after a reboot (good_t 0) the newest clean closed hour, the same age rule; walked by the upload path
// (station.cpp) once per upload while the factory value is up and nothing is known, which then fills good_p / good_t.
bool lastGoodFromHours(const Station& st, float* p_hpa, uint32_t* t);

}  // namespace fc
