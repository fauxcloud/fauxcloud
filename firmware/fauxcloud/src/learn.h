// learn.h -- the station's self-calibration: L1 clear-sky envelope, L1b horizon mask, L2 RH at saturation, L3 anemometer
// start-up, L4 site wind exposure, L5 clear-evening cooling, L6 frost relation, L7 pressure sanity. One pure
// pipeline per closed hour (learnHour) and per closed day (learnDay), the same live and in a replay of the rings:
// admission gates -> frugal / EWMA update in integers -> bounds -> confidence. The outlook reads only learnUseOf's
// snapshot (LearnUse), each learner from its use level (LEARN_USE_AT); L7 feeds a notice. The core state is the NVS
// blob "lr:" (256 B), the mask "lm:" (961 B). Plain C++, host-compilable, no heap.
#pragma once
#include <cstddef>
#include <cstdint>

#include "diag_bench.h"  // FC_DIAG_BENCH: the gate counters are bench only
#include "forecast.h"  // LearnUse
#include "records.h"
#include "store.h"  // crc32

namespace fc {

class Out;

constexpr int L1_CELLS = 12;     // mean sun elevation 5-10, 10-15, 15-20, 20-30, 30-45, >= 45 deg x AM / PM
constexpr int L1B_CELLS = 949;   // the horizon mask in sun coordinates (l1bCell)
constexpr int L1B_SCRATCH = 64;  // visits of one day, committed at sunset when the day was clear
constexpr int LEARN_BINS = 12;   // five-minute bins of an hour (L1's clear-sky integral, L1b's visits)
constexpr int LEARN_WIND_MAX = 60;
constexpr uint32_t LDAY_BASE = 20454;  // 2026-01-01: day numbers are stored as 14 bits from here
// core v2 put L5 / L6 in v1's reserve; v3 adds L7's REL-step tracker and the "Restart learning" hour. An older blob is
// migrated (learnMigrateCore) and a replay pass re-derives the learners new or changed since its version (from v2:
// L1 / L2 / L4); an unknown older version -> priors + a full ring replay.
constexpr uint8_t LEARN_VERSION = 3, MASK_VERSION = 1;

// Gate reasons (counted per hour or per learner, RAM; the JSON names are learnGateName)
enum LearnGate : uint8_t {
  LG_QUALITY,   // stale, clock_approx, restart, gap (L7: + the pressure bits; L2 a: + rain_partial, console_reset)
  LG_READINGS,  // fewer than 10 readings
  LG_PHYSICS,   // RH, dew point, gust vs wind, pressure out of physics
  LG_FROZEN,    // t_min == t_max for 6 hours
  LG_INDOOR,    // a dark day: sensor indoors or covered
  LG_GEO,       // solar at night: time or location wrong
  LG_DOME,      // frosted dome / snow on the light sensor
  LG_ICED,      // iced anemometer
  LG_STUCK,     // RH stuck at the cap
  LG_STEADY,    // L1: broken cloud (not steady)
  LG_K_RANGE,   // L1: k outside 0.05 .. 1.6
  LG_K_LOW,     // L1: k < 0.5 E (cloud)
  LG_CAP,       // per-day cap (L1, L2 a)
  LG_LOCATION,  // L1 / L1b / night: no location
  LG_BARO,      // L7: a day with the console's factory pressure
  LG_SCRATCH,   // L1b: a cell visit dropped because the day's scratch was full
  LG_L5_LABEL,    // L5: an evening without a usable label of the day (clear / overcast)
  LG_L5_EVENING,  // L5: an evening window with rain, wind, a gated or a missing hour
  LG_L6_NIGHT,    // L6: a closed night not admitted (the reason: night.last.l6)
  LG_MELT,        // L2 a: a wet hour at or below 2 °C or within 24 h of icy rain (melt tips in dry air)
  LG_COUNT
};
const char* learnGateName(int g);  // FC_DIAG_BENCH builds only (diag_bench.h)

enum LearnConf : uint8_t { LC_DEFAULT, LC_LOW, LC_MEDIUM, LC_HIGH };
const char* learnConfName(int c);

// One L1 cell: E in 1e-4 of the clear-sky model, n_eff in 1/64, the last counted day (14 bits) + updates that day (2 bits)
struct LearnCell {
  uint16_t e, n, day;
};

// The core blob "lr:<id>": 256 B, versioned, CRC-32 over everything with `crc` = 0
struct LearnCore {
  char magic[2];            // "LR"
  uint8_t version, epoch;   // LEARN_VERSION; the sensor epoch the values belong to
  uint8_t profile, flags;   // the station profile the values were learned under; LF_* bits
  uint16_t relearn_until;   // day (14-bit form) until which re-learning (eta x 4) runs; 0 = none
  uint32_t last_hour, last_day;  // resume points: the newest hour key / day (YYYYMMDD) consumed
  uint32_t loc_sig, mac_hash;    // FNV of lat/lon rounded to 0.1 deg (0 = none); the console MAC's FNV (0 = unknown)
  uint32_t crc;
  // gate state (12 B)
  uint8_t indoor, frozen_run, geo_days, gbits, stuck_run, prev_rain, icy_h, spare0;
  int16_t stuck_t0;
  uint16_t gate_day;
  // night summary scratch (20 B; L2 b and the L5 / L6 inputs)
  uint32_t n_start;         // the night's first hour (sunset + 1 h), 0 = no night open
  uint8_t n_len, n_rhmax, n_hours, n_ok, n_flags, n_calm;
  int16_t n_t2, n_td2, n_t0, n_dt3, n_tmin;  // 0.1 °C: T / Td at sunset + 2 h, the first hour's T, its 3-h change, T min
  LearnCell l1[L1_CELLS];
  uint16_t l2_wet_e, l2_wet_n, l2_wet_day, l2_night_e, l2_night_n, l2_night_day;  // 0.01 %RH, 1/64, day(+cnt)
  uint16_t l3_e, l3_n, l3_day, l3_spare;   // 1e-4 km/h, 1/64
  uint16_t l4_p50, l4_p90, l4_n, l4_day;   // 1e-3 km/h, n in 1/16
  int32_t l7_e;                            // Q16 of 0.1 hPa
  uint16_t l7_n;                           // days in 1/64
  uint8_t l7_flag, l7_spare;
  // v2 (in v1's reserve[80]). L6: forgetting sums of the centred night variables (Q8 of 0.1-unit products, W in
  // Q16 = n_eff), the prospective residual sums (e² and |e|, Q8), the quantised fit and its last day
  uint32_t l6_w;
  int32_t l6_s, l6_l, l6_y, l6_ss, l6_sl, l6_ll, l6_sy, l6_ly;
  uint32_t l6_e2, l6_ea;
  int16_t l6_a, l6_b, l6_c;  // 0.01 °C, 1e-4, 1e-4 °C/h
  uint16_t l6_day;
  // L5: clear / overcast evening cooling rate EWMAs (0.01 °C/h), n_eff (1/64), last days
  int16_t l5_ce, l5_oe;
  uint16_t l5_cn, l5_on, l5_cd, l5_od;
  // evening / night v2 scratch: the evening's first T and its 3-step change, the previous night hour's T (0.1 °C); the
  // label's day; EB_* bits, the label, the night length (0.1 h), the calm-ish night hours
  int16_t e_t0, e_dt3, n_tprev;
  uint16_t lbl_day;
  uint8_t e_flags, lbl, n_L, n_w5;
  // V3 (in V2's reserve2 / pad): L7's last clean hour's pressure (0.1 hPa as stored, I16_NONE = none: the REL-step
  // restart), the REL step's mark (1 = the last clean hour stepped >= 4 hPa from the one before; the next hour confirms
  // it; 0 = none, the value every V3 blob before it holds: the first of V3's two spare bytes), a spare, the "Restart
  // learning" hour (the newest hour consumed at the press; 0 = none: a replay re-applies the restart when it passes that
  // hour), pad
  int16_t l7_pprev;
  uint8_t l7_step;
  uint8_t reserve3[1];
  uint32_t relearn_hour;
  uint8_t pad[4];
};
static_assert(sizeof(LearnCore) == 256, "the core blob is 256 B");
static_assert(offsetof(LearnCore, l7_pprev) == 244 && offsetof(LearnCore, l7_step) == 246 &&
                  offsetof(LearnCore, reserve3) == 247 && offsetof(LearnCore, relearn_hour) == 248,
              "the V3 fields (l7_step takes the first spare byte: zero in every earlier V3 blob)");
// LearnCore above is V3. Layout rule (blob versions migrate, never reset): additive only -- a field is never moved or
// re-typed, a retired field keeps its bytes; `reserve3` and `pad` remain (5 B). A LEARN_VERSION bump ships with its migration function and the version-pair
// host test.

// the V2 core, verbatim, so a V2 blob is read field by field and migrated
struct LearnCoreV2 {
  char magic[2];
  uint8_t version, epoch;
  uint8_t profile, flags;
  uint16_t relearn_until;
  uint32_t last_hour, last_day;
  uint32_t loc_sig, mac_hash;
  uint32_t crc;
  uint8_t indoor, frozen_run, geo_days, gbits, stuck_run, prev_rain, icy_h, spare0;
  int16_t stuck_t0;
  uint16_t gate_day;
  uint32_t n_start;
  uint8_t n_len, n_rhmax, n_hours, n_ok, n_flags, n_calm;
  int16_t n_t2, n_td2, n_t0, n_dt3, n_tmin;
  LearnCell l1[L1_CELLS];
  uint16_t l2_wet_e, l2_wet_n, l2_wet_day, l2_night_e, l2_night_n, l2_night_day;
  uint16_t l3_e, l3_n, l3_day, l3_spare;
  uint16_t l4_p50, l4_p90, l4_n, l4_day;
  int32_t l7_e;
  uint16_t l7_n;
  uint8_t l7_flag, l7_spare;
  uint32_t l6_w;
  int32_t l6_s, l6_l, l6_y, l6_ss, l6_sl, l6_ll, l6_sy, l6_ly;
  uint32_t l6_e2, l6_ea;
  int16_t l6_a, l6_b, l6_c;
  uint16_t l6_day;
  int16_t l5_ce, l5_oe;
  uint16_t l5_cn, l5_on, l5_cd, l5_od;
  int16_t e_t0, e_dt3, n_tprev;
  uint16_t lbl_day;
  uint8_t e_flags, lbl, n_L, n_w5;
  uint8_t reserve2[4];
  uint8_t pad[8];
};
static_assert(sizeof(LearnCoreV2) == 256 && offsetof(LearnCoreV2, reserve2) == 244, "the V2 core as stored by older images");

// the V1 core, verbatim, so an older blob is read field by field and migrated
struct LearnCoreV1 {
  char magic[2];
  uint8_t version, epoch;
  uint8_t profile, flags;
  uint16_t relearn_until;
  uint32_t last_hour, last_day;
  uint32_t loc_sig, mac_hash;
  uint32_t crc;
  uint8_t indoor, frozen_run, geo_days, gbits, stuck_run, prev_rain, icy_h, spare0;
  int16_t stuck_t0;
  uint16_t gate_day;
  uint32_t n_start;
  uint8_t n_len, n_rhmax, n_hours, n_ok, n_flags, n_calm;
  int16_t n_t2, n_td2, n_t0, n_dt3, n_tmin;
  LearnCell l1[L1_CELLS];
  uint16_t l2_wet_e, l2_wet_n, l2_wet_day, l2_night_e, l2_night_n, l2_night_day;
  uint16_t l3_e, l3_n, l3_day, l3_spare;
  uint16_t l4_p50, l4_p90, l4_n, l4_day;
  int32_t l7_e;
  uint16_t l7_n;
  uint8_t l7_flag, l7_spare;
  uint8_t reserve[80];
  uint8_t pad[8];
};
static_assert(sizeof(LearnCoreV1) == 256 && offsetof(LearnCoreV1, reserve) == 168, "the V1 core as stored by older images");
// Every V1 field a V2 core keeps (magic, version, crc, reserve and pad aside; spares included: bit for bit)
#define LEARN_V1_SHARED(X)                                                                                              \
  X(epoch) X(profile) X(flags) X(relearn_until) X(last_hour) X(last_day) X(loc_sig) X(mac_hash) X(indoor) X(frozen_run)  \
  X(geo_days) X(gbits) X(stuck_run) X(prev_rain) X(icy_h) X(spare0) X(stuck_t0) X(gate_day) X(n_start) X(n_len)        \
  X(n_rhmax) X(n_hours) X(n_ok) X(n_flags) X(n_calm) X(n_t2) X(n_td2) X(n_t0) X(n_dt3) X(n_tmin) X(l1) X(l2_wet_e)       \
  X(l2_wet_n) X(l2_wet_day) X(l2_night_e) X(l2_night_n) X(l2_night_day) X(l3_e) X(l3_n) X(l3_day) X(l3_spare)           \
  X(l4_p50) X(l4_p90) X(l4_n) X(l4_day) X(l7_e) X(l7_n) X(l7_flag) X(l7_spare)
#define LEARN_V1_ASSERT(f)                                                                                              \
  static_assert(offsetof(LearnCoreV1, f) == offsetof(LearnCore, f) && sizeof(LearnCoreV1::f) == sizeof(LearnCore::f), \
                "a V1 field moved or changed size in V2: " #f);
LEARN_V1_SHARED(LEARN_V1_ASSERT)
// the learners new in V2 and the V2 evening / night scratch (the merge of a migration pass)
#define LEARN_L5_FIELDS(X) X(l5_ce) X(l5_oe) X(l5_cn) X(l5_on) X(l5_cd) X(l5_od)
#define LEARN_L6_FIELDS(X)                                                                                              \
  X(l6_w) X(l6_s) X(l6_l) X(l6_y) X(l6_ss) X(l6_sl) X(l6_ll) X(l6_sy) X(l6_ly) X(l6_e2) X(l6_ea) X(l6_a) X(l6_b) X(l6_c) \
  X(l6_day)
#define LEARN_V2_SCRATCH(X) X(e_t0) X(e_dt3) X(n_tprev) X(lbl_day) X(e_flags) X(lbl) X(n_L) X(n_w5)
static_assert(offsetof(LearnCore, l6_w) == 168, "the v2 layout fills the v1 reserve");
// every V2 field a V3 core keeps (magic, version, crc, reserve2 and pad aside), bit for bit
#define LEARN_V2_SHARED(X) LEARN_V1_SHARED(X) LEARN_L5_FIELDS(X) LEARN_L6_FIELDS(X) LEARN_V2_SCRATCH(X)
#define LEARN_V2_ASSERT(f)                                                                                              \
  static_assert(offsetof(LearnCoreV2, f) == offsetof(LearnCore, f) && sizeof(LearnCoreV2::f) == sizeof(LearnCore::f), \
                "a V2 field moved or changed size in V3: " #f);
LEARN_V2_SHARED(LEARN_V2_ASSERT)
// the learners a V3 pass re-derives: L1, L2, L4 (their fields, day stamps included)
#define LEARN_L1_FIELDS(X) X(l1)
#define LEARN_L2_FIELDS(X) X(l2_wet_e) X(l2_wet_n) X(l2_wet_day) X(l2_night_e) X(l2_night_n) X(l2_night_day)
#define LEARN_L4_FIELDS(X) X(l4_p50) X(l4_p90) X(l4_n) X(l4_day)
// gbits
constexpr uint8_t LB_DOME = 1, LB_ICED = 2, LB_GEO_TODAY = 4, LB_STUCK = 8;
// n_flags
constexpr uint8_t LN_RESTART = 1, LN_RAIN = 2, LN_HOAR = 4, LN_PARTIAL = 8, LN_RISE = 16;
// e_flags: bad bits of the evening window (EB_BAD) + two internal marks (E0 seen; the evening judged)
constexpr uint8_t EB_GATE = 1, EB_RAIN = 2, EB_WIND = 4, EB_MISS = 8, EB_BAD = 15, EB_E0 = 16, EB_DONE = 32;
// lbl: the day's late-afternoon sky as L5's class (0 = never set)
enum LearnLabel : uint8_t { LBL_UNSET, LBL_CLEAR, LBL_OVERCAST, LBL_MIXED, LBL_NOCELL, LBL_NONE };
// L6 admission of a closed night (JSON night.last.l6)
enum L6Reason : uint8_t { L6R_OK, L6R_RAIN, L6R_WIND, L6R_RISE, L6R_QUALITY, L6R_NOT_CLEAR, L6R_EVENING, L6R_MISSING };
// flags
constexpr uint8_t LF_FRESH = 1;      // never fed (priors, no context adopted yet)
constexpr uint8_t LF_RELEARNED = 2;  // the restart at relearn_hour is applied to this state (a pass clears it)

// The mask blob "lm:<id>": 12 B header + one byte per cell (visits v << 4 | bright b)
struct LearnMaskBlob {
  char magic[2];  // "LM"
  uint8_t version, spare;
  uint32_t loc_sig, crc;
  uint8_t c[L1B_CELLS];
};
constexpr size_t LEARN_MASK_BYTES = 12 + L1B_CELLS;  // stored length (the struct pads to 964)
static_assert(offsetof(LearnMaskBlob, c) == 12 && LEARN_MASK_BYTES == 961, "the mask blob is 961 B");

// A closed night (RAM copy of the last one, for /api/learn)
struct LearnNight {
  uint32_t start;
  uint8_t len, rhmax, hours, ok, flags, calm;
  int16_t t2, td2, t0, dt3, tmin;
  bool admitted;
  uint8_t l_h, w5, l6;  // night length (0.1 h), calm-ish hours, L6's reason (L6R_*)
  int16_t pred;         // L6's minimum predicted before the night was learned (0.1 °C; I16_NONE = none)
};

struct LearnState {
  LearnCore c;
  uint8_t mask[L1B_CELLS];
  uint16_t sc_cell[L1B_SCRATCH];
  uint8_t sc_b[L1B_SCRATCH];
  uint8_t sc_n, sc_clear, l1b_dome;
  uint16_t sc_day;
#if FC_DIAG_BENCH  // why hours were not admitted, per gate: R&D only (nothing in the release reads them)
  uint16_t gates[LG_COUNT];
#endif
  LearnNight last_night;
  uint32_t hours_fed, days_fed;
  bool core_dirty, mask_dirty;
  // the learner's last step was clamped at a bound (RAM; shown only with n > 0)
  uint16_t l1_hit;  // bit per L1 cell
  uint8_t hit;      // LH_* bits
};
constexpr uint8_t LH_L2W = 1, LH_L3 = 2, LH_L4 = 4, LH_L6 = 8;

// E0, the first hour starting at or after sunset (its own helper, tested at the hour edges)
inline uint32_t learnE0(uint32_t set) { return (set + 3599) / 3600 * 3600; }
// L5 threshold (0.01 °C/h): the learned midpoint of the two classes, else the default 1.00 (= fcst::COOL_DT)
enum class L5Src : uint8_t { Default, Learned, Inseparable };
int l5Threshold(const LearnCore& c, L5Src* src, bool* hit);
// L6 ridge solve of the sums into (l6_a, l6_b, l6_c); true = a coefficient was clamped. Exposed for the golden vector.
bool l6Solve(LearnCore& c);
// L6's minimum for a night (0.1 °C; I16_NONE without T0 / Td0) with coefficients (a 0.01 °C, b 1e-4, c 1e-4 °C/h);
// `drop` (optional) gets the unclamped predicted drop. The firmware's prospective prediction and the replay's prior column.
int16_t l6Predict(int16_t t2, int16_t td2, uint8_t l_h, int a, int b, int c, int16_t* drop = nullptr);

// What a closed hour is learned with. Live: the station's slot; replay: the bridge JSON (no bins, no wind values).
struct LearnCtx {
  float lat, lon;           // NaN = no location
  uint8_t profile;
  uint32_t mac_hash;        // 0 = unknown (no change judged)
  // live only (L1b, L3): the Recent ring's readings of the hour
  bool live;
  uint16_t bin_sum[LEARN_BINS];  // solar W/m² summed per five-minute bin
  uint8_t bin_n[LEARN_BINS];
  uint8_t wind_n;
  uint8_t wind[LEARN_WIND_MAX];  // 0.1 km/h as received (calibrated), 255 = 25.5 or more (only < 10 km/h is used)
  uint16_t wind_f;          // the wind factor in force x 0.001 (1000 = none): wind / factor = raw
  int16_t p_off;            // L7: the bridge's pressure offset in force (0.01 hPa), added to the raw mean
};

// Events learnHour / learnDay report (the app logs them once and writes the blobs)
constexpr uint8_t LE_LOC = 1, LE_PROFILE = 2, LE_MAC = 4, LE_RELEARN = 8, LE_L1B_COMMIT = 16, LE_NIGHT = 32;
constexpr uint8_t LE_L7_STEP = 64;  // a REL step restarted L7

uint32_t learnLocSig(float lat, float lon);
uint32_t learnDayOf(uint32_t unix, float lon);  // the local solar day number (floor(unix / 86400 + lon / 360))
void learnInit(LearnState* s);                  // priors, empty mask, fresh
// the learners a feed may write. LM_ALL is the normal path; a migration replays only the learners new to
// the blob layout. A masked-out learner gets no write of any of its fields (day stamps included); the inputs always
// run: trackers and gate state, the common admission, the night summary and evening window, the L5 label, the resume
// points, the RAM gate counters.
constexpr uint16_t LM_L1 = 1, LM_L1B = 2, LM_L2 = 4, LM_L3 = 8, LM_L4 = 16, LM_L5 = 32, LM_L6 = 64, LM_L7 = 128, LM_ALL = 0xFF;
constexpr uint16_t LM_SINCE_V1 = LM_L5 | LM_L6;  // the learners new in V2 (the replay mask of a V1 blob)
// the learners a V2 blob re-derives from the rings (V3's warm-up, seeds and melt gate change their values)
constexpr uint16_t LM_SINCE_V2 = LM_L1 | LM_L2 | LM_L4;
static_assert(((LM_SINCE_V1 | LM_SINCE_V2) & LM_L7) == 0, "no day learner is replayed, so the pass walks hours "
                                                          "only (a future day learner adds the day walk)");
uint8_t learnHour(LearnState& s, const HourRec& r, const LearnCtx& ctx, uint16_t mask = LM_ALL);
uint8_t learnDay(LearnState& s, const DayRec& d, const LearnCtx& ctx, uint16_t mask = LM_ALL);
uint8_t learnConfOf(uint16_t n, uint16_t nmin, uint16_t nhigh, uint16_t last_day, uint16_t today);

// one table of the real samples each learner needs for its levels (n_eff counts with forgetting, so a bar needs
// more samples than its n). Firmware and sheet read this table; a host test iterates the firmware's own neff from 0
// against every entry (learnBarSamples), so a constant change cannot leave it stale. L5 counts per class (clear and
// overcast evenings each); L6 in admitted nights (its SD gate aside); L7 in admitted days: stage 1, stage 2 (= high).
enum LearnBarId : uint8_t { LBR_L1, LBR_L2A, LBR_L2B, LBR_L3, LBR_L4, LBR_L5, LBR_L6, LBR_L7, LBR_COUNT };
struct LearnBar {
  uint8_t id;
  uint16_t low, med, high;
};
constexpr LearnBar LEARN_BARS[LBR_COUNT] = {
    {LBR_L1, 6, 12, 27},      {LBR_L2A, 9, 19, 47}, {LBR_L2B, 24, 63, 92}, {LBR_L3, 237, 591, 1019},
    {LBR_L4, 265, 569, 1358}, {LBR_L5, 7, 18, 27},  {LBR_L6, 18, 42, 78},  {LBR_L7, 14, 51, 51}};
// the confidence from which the outlook reads a learner; LC_NEVER = information only, or a notice (L7)
constexpr uint8_t LC_NEVER = 4;
constexpr uint8_t LEARN_USE_AT[LBR_COUNT] = {LC_MEDIUM, LC_LOW, LC_LOW, LC_NEVER, LC_NEVER, LC_MEDIUM, LC_HIGH, LC_NEVER};
// The real samples (from n_eff 0) that reach `level` (1 low, 2 medium, 3 high) by the firmware's own update; the host
// test's reference for LEARN_BARS
uint16_t learnBarSamples(int id, int level);
// Real samples still missing from the learner's count `n` (its own unit: 1/64, L4 1/16, L6 W in Q16) to `level`;
// 0 = reached. At most ~1 400 steps (JSON time only).
uint16_t learnNeed(int id, uint32_t n, int level);
// L1's steadiness tolerance (% of the clear sky's max/avg) for a cell at n_eff `n` (1/64): 130 for an
// empty cell, linear to 115 at its first bar (an empty cell takes any plausibly sunny hour, a known one is picky)
int learnSteadyPct(uint16_t n);
// the built-in long-term mean sea-level pressure (0.1 hPa) at a place and day of year (1..366), from
// src/mslp_norm.h (NCEP/NCAR R1 1991-2020, two seasons, bilinear, a cosine blend by day); false = none (no table
// built in, no place, |lat| > 70). `tab` (tests): another table of the same shape.
bool learnMslpNormal(float lat, float lon, int doy, int16_t* out, const int8_t* tab = nullptr);
int learnDoy(uint32_t ymd);  // day of year 1 .. 366 of YYYYMMDD
uint16_t learnL7Days(uint16_t n);  // the admitted days behind L7's n_eff; 61 = over 60
// for tools (forecast_replay --what-if): the sun of `hour` as learnUseOf reads it -- the L1 cell (band * 2 + pm; -1 under
// 10 deg or without a location), the L1b cell of the mid-hour position (-1 none), the clear sky's mean / max (W/m2)
bool learnHourSky(uint32_t hour, float lat, float lon, int* l1cell, int* l1bcell, double* cs_avg, double* cs_max);
void learnSetMslpTable(const int8_t* t);  // host tests only: another table (nullptr = the built-in one, if any)
bool learnMslpBuiltIn();                  // the generated table is built in (src/mslp_norm.h MSLP_NORM_PRESENT)
// The table: [season DJF, JJA][row: lat -60 .. 75 every 15 deg][col: lon 0 .. 340 E every 20 deg], int8 hPa - 1013
constexpr int MSLP_ROWS = 10, MSLP_COLS = 18;
constexpr int8_t MSLP_NONE = -128;  // a cell over high ground (mean orography > 1 000 m): no normal
// the 5-deg high-ground bitmap's grid (lat -60 .. 80, lon 0 .. 355 E), 252 B
constexpr int MSLP_HIGH_ROWS = 28, MSLP_HIGH_COLS = 72;
bool learnMslpHighAt(float lat, float lon, const uint8_t* bits = nullptr);  // the station's 5-deg box is high ground
void learnSetMslpHigh(const uint8_t* b);  // host tests only: another bitmap (nullptr = the built-in one, if any)
bool learnMslpHighBuiltIn();               // the generated header carries the bitmap (MSLP_HIGH_PRESENT)
int l1bCell(double el, double az);  // -1 below 3 deg
uint8_t l1bClass(uint8_t cell);     // 0 unknown, 1 sun, 2 shade

// Blobs: seal = magic, version, CRC; check = Ok / Bad (missing, short, magic, CRC) / Older / Newer. `len` is the stored
// length: longer than ours = Newer without reading it (an older image never overwrites a newer blob).
enum class BlobCheck : uint8_t { Ok, Bad, Older, Newer };
void learnSealCore(LearnCore* c);
BlobCheck learnCheckCore(const LearnCore& c, size_t len);
void learnPackMask(const LearnState& s, LearnMaskBlob* b);
// The mask on its own: kept whatever the core's state when it was learned at `loc_sig` (the bridge's now)
BlobCheck learnUnpackMask(LearnState* s, const LearnMaskBlob& b, size_t len, uint32_t loc_sig);
void learnLoaded(LearnState* s, BlobCheck core, BlobCheck mask);  // after a load: marks a missing mask for writing
// resume points in the future (a clock that once ran ahead) are pulled back to now; true = changed.
// The target is the later of now and the newest key the station's rings hold (newest_hour/day; 0 =
// none): a resume point never moves below a stored key, so a clock that went back cannot make the live pass feed the
// RAM hours a second time
bool learnClampResume(LearnState& s, uint32_t now_hour, uint32_t today_ymd, uint32_t newest_hour = 0, uint32_t newest_day = 0);

// migration of an Older core blob to V3. Version 2 -> every V2 field copied (LEARN_V2_SHARED), l7_pprev none,
// relearn_hour 0, *replay = LM_SINCE_V2; version 1 -> every V1 field copied (LEARN_V1_SHARED), L5 / L6 at their priors,
// the V2 scratch at init, *replay = LM_SINCE_V1 | LM_SINCE_V2. *from = the source version; *replay = 0 when the blob was
// never fed (the normal full backfill feeds every learner). L7's flag is cleared and its n capped below stage 1. Any
// other older version -> false (the caller starts from priors with a full replay). `raw` = the 256 bytes as stored.
bool learnMigrateCore(const void* raw, size_t len, LearnCore* out, uint8_t* from, uint16_t* replay);
// The replayed learners start over from their priors in the pass.
// The pass state: learnInit, then the migrated core with the gate state, night summary, V2 scratch and prev_rain back at
// their init values, the resume points at 0 and the learners in `replay` at their priors; the context (loc_sig, profile,
// mac_hash, epoch, relearn_until, relearn_hour, flags but LF_RELEARNED) kept so context() fires nothing. `mask`:
// the live state's L1b mask, which L5's label reads, as on the board (nullptr = empty).
void learnPassStart(LearnState* p, const LearnCore& migrated, uint16_t replay = LM_SINCE_V1, const uint8_t* mask = nullptr);
// The pass's learners in `mask` into `dst`; the V2 scratch and n_flags' LN_RISE only when the pass ended at `end` with
// the same open night (or none), else that evening / night is marked missing (EB_MISS: judged `evening`).
void learnPassMerge(LearnCore& dst, const LearnCore& p, uint16_t mask, uint32_t end);

// "Restart learning" (admin): every learner back below its first bar (values kept as the start point; L2 re-seeds
// from its next sample; L6's sums scaled to 5 nights; L7 starts over; the L1b mask cleared), x4 steps for 14 days.
// relearn_hour = the newest hour consumed, so a later replay of the rings re-applies the restart there. LE_RELEARN.
uint8_t learnRestart(LearnState& s, float lon);

// what the outlook may read (LearnUse, forecast.h), in calibrated units: the calibration in force (temperature
// 0.01 °C, humidity 0.1 %) is the caller's. Built from the newest hour consumed; nothing below its use level is set.
void learnUseOf(const LearnState& s, float lat, float lon, int16_t cal_t, int16_t cal_rh, LearnUse* u);

// /api/learn body (also the replay's output). `state` / `why`: learning|backfill|paused|read_only, null|no_location|...
struct LearnMeta {
  const char* state;
  const char* why;  // nullptr = null
  uint32_t backfill_hours;
  bool backfill_done;
  uint32_t written, writes;  // the last blob write (unix), writes since boot
  uint32_t nvs_used, nvs_free;
  uint32_t today_unix;       // for the confidence age
  float lat, lon;            // only to judge the age day and the night sun times; never printed
  uint32_t backfill_days, backfill_ms;  // days fed by the backfill, its wall time
  // the blob version migrated from at this boot's load (0 = none), the learners the pass replayed (LM_*;
  // 0 = none, or the pass state could not be allocated), its hours and wall time
  uint8_t migrated_from;
  uint16_t replay_mask;
  uint32_t replay_hours, replay_ms;
  const LearnUse* use;  // the station's snapshot (the "use" block; nullptr = omitted)
  int16_t p_off;        // the bridge's pressure offset in force (0.1 hPa, rounded once as the notice)
  bool baro_uncal;      // the console's pressure at its factory value: L7's flag none
};
// What /api/learn shows, copied out of the state under the learner lock (≈ 0.57 KB instead of the 1.5 KB state)
struct LearnView {
  LearnCore c;
  uint8_t packed[(L1B_CELLS + 3) / 4];  // L1b classes, 2 bits per cell
  uint16_t cls[3];                      // unknown / sun / shade counts
#if FC_DIAG_BENCH
  uint16_t gates[LG_COUNT];
#endif
  LearnNight last_night;
  uint8_t sc_n, sc_clear;
  uint16_t l1_hit;
  uint8_t hit;
  LearnUse use;  // the station's snapshot (learnCopy fills it; LearnMeta.use points here)
};
void learnView(const LearnState& s, LearnView* v);
void learnJson(Out& o, const LearnView& v, const LearnMeta& m);

}  // namespace fc
