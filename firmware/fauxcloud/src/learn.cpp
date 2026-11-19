// learn.cpp -- see learn.h. Every persisted value is an integer (E in fixed point, n_eff in 1/64 or 1/16, the EWMA in
// Q16) and the sun maths is quantised before any comparison (k to 1e-4), so a last-ulp libm difference between the host
// and the ESP32 cannot split a replay from the board.
#include "learn.h"

#include <cmath>
#include <cstdio>
#include <cstring>

#include "sun.h"
#include "tz.h"
#include "web_render.h"
#include "diag_bench.h"

namespace fc {

// flash size: the small update helpers stay one copy each, not inlined per learner
#if defined(__GNUC__)
#define LEARN_NOINLINE __attribute__((noinline))
#else
#define LEARN_NOINLINE
#endif

namespace {
// frugal steps (units of each learner's E; x4 while re-learning) and n_eff decay lambda in Q16 (2^(-1/half-life))
constexpr int L1_UP = 180, L1_DN = 20, L1_LO = 3500, L1_HI = 13000, L1_PRIOR = 5500;  // P90, eta 0.02 (1e-4)
constexpr uint32_t L1_LAM = 64039, L1_NMIN = 5 * 64, L1_NHI = 20 * 64;                // memory 30 counted hours
constexpr int L2_P50 = 25, L2_P90_UP = 45, L2_P90_DN = 5, L2_LO = 8500, L2_HI = 10000, L2_PRIOR = 10000;  // 0.01 %RH
constexpr uint32_t L2A_LAM = 64410, L2A_NMIN = 8 * 64, L2A_NHI = 32 * 64;   // memory 40 wet hours
constexpr uint32_t L2B_LAM = 64534, L2B_NMIN = 20 * 64, L2B_NHI = 49 * 64;  // memory 45 nights
constexpr int L3_UP = 25, L3_DN = 475, L3_LO = 3000, L3_HI = 50000, L3_PRIOR = 10000;  // P5, eta 0.05 km/h (1e-4)
constexpr uint32_t L3_LAM = 65445, L3_NMIN = 200 * 64, L3_NHI = 541 * 64;
constexpr int L4_50 = 50, L4_90_UP = 90, L4_90_DN = 10;  // eta 0.1 km/h (1e-3)
constexpr int L4_50_LO = 500, L4_50_HI = 20000, L4_90_LO = 3000, L4_90_HI = 50000, L4_50_PRIOR = 3000, L4_90_PRIOR = 12000;
constexpr uint32_t L4_LAM = 65504, L4_NMIN = 240 * 16, L4_NHI = 960 * 16;  // memory 60 days of hours, n in 1/16
// L7 is the exponentially weighted mean of the daily pressure, half-life 30 days (n_eff asymptote 44):
// alpha = 1 / W (a true running mean until the asymptote), floored at 1 - lambda. Stage 1 at n_eff 12 (14 admitted
// days) flags |off| >= 20 hPa, stage 2 at 30 (51 days) >= 12 hPa; a flag holds until |off| < 9 hPa (0.1 hPa below).
// The REL-step restart: two consecutive clean hours >= 4 hPa apart, and the next clean hour within 2 hPa of the second
// (a REL change in the console is a step that holds; weather that moves 4 hPa in an hour keeps moving).
constexpr uint32_t L7_LAM = 64039, L7_AFLOOR = 65536 - L7_LAM, L7_N1 = 12 * 64, L7_N2 = 30 * 64;
constexpr int L7_ENTER1 = 200, L7_ENTER2 = 120, L7_EXIT = 90, L7_STEP = 40, L7_HOLD = 20;
// the warm-up multiplier while a learner is below its use level (the relearn x4 is the same 4)
constexpr int WARM = 4;
// one gate counter up (bench builds only: LearnState::gates exists there)
#if FC_DIAG_BENCH
#define LGATE(s, g) ((s).gates[g]++)
#else
#define LGATE(s, g) ((void)0)
#endif
// L5: EWMA floor alpha 1/20 (Q16), x4 while re-learning; n_eff lambda 2^(-1/14); learned at 6 evenings
// per class, high at 15; priors clear 1.50 / overcast 0.50 °C/h; threshold bounds 0.50 .. 2.50 °C/h, default 1.00
constexpr uint32_t L5_ALPHA = 3277, L5_LAM = 62337, L5_NMIN = 6 * 64, L5_NHI = 15 * 64;
constexpr int L5_CE_PRIOR = 150, L5_OE_PRIOR = 50, L5_THR_LO = 50, L5_THR_HI = 250, L5_THR_DEF = 100, L5_SEP = 30;
// L6: lambda 2^(-1/40), re-learning 2^(-1/10); learned at W >= 15 nights and prospective SD <= 2.5 °C,
// high at 43 nights and SD <= 2.0; priors a 1.00 °C, b 0.40, c 0.20 °C/h; bounds a -2 .. 6 °C, b 0 .. 1, c 0 .. 0.6
constexpr uint32_t L6_LAM = 64410, L6_LAM_RE = 61147;
constexpr int L6_A_PRIOR = 100, L6_B_PRIOR = 4000, L6_C_PRIOR = 2000;
constexpr int L6_A_LO = -200, L6_A_HI = 600, L6_B_HI = 10000, L6_C_HI = 6000;
constexpr uint16_t Q_COMMON = (1u << Q_STALE) | (1u << Q_CLOCK_APPROX) | (1u << Q_RESTART) | (1u << Q_GAP);
constexpr uint16_t Q_BARO = (1u << Q_BARO_UNCAL) | (1u << Q_BARO_BOUNDARY) | (1u << Q_BARO_BRIDGE);
constexpr uint16_t Q_WET = (1u << Q_RAIN_PARTIAL) | (1u << Q_CONSOLE_RESET);

uint16_t day14(uint32_t day) { return (uint16_t)((day - LDAY_BASE) & 0x3FFF); }
uint16_t ageDays(uint16_t now14, uint16_t then14) { return (uint16_t)((now14 - then14) & 0x3FFF); }

LEARN_NOINLINE uint16_t neff(uint32_t n, uint32_t lam, uint32_t one) {
  const uint32_t v = (uint32_t)(((uint64_t)n * lam) >> 16) + one;
  return (uint16_t)(v > 65535 ? 65535 : v);
}
// sign-based frugal quantile step, then the bound; true = the step was clamped (the learner's `hit`)
LEARN_NOINLINE bool frugal(uint16_t* e, int x, int up, int dn, int lo, int hi) {
  int v = *e;
  if (x > v) v += up;
  else if (x < v) v -= dn;
  *e = (uint16_t)(v < lo ? lo : v > hi ? hi : v);
  return v < lo || v > hi;
}
// per L1 band the lowest full-sun ratio, below which an hour is never clear-sun evidence: it neither lowers E nor
// counts. From a measured full sun of 0.42-0.44 at 13-14 deg, 0.55 at 18, 0.64 at 21; held at or under 0.44 so a dim
// site (k 0.45) still converges, and over 0.40 so a steady bright overcast (k 0.30-0.40) never moves a cell. 1e-4.
constexpr uint16_t L1_FLOOR[6] = {4100, 4200, 4400, 4400, 4400, 4400};
// an hour is steady when its max/avg is at most a tolerance % of the clear sky's max/avg. A tighter 110 % refused
// every winter hour of a real sensor (max/avg 1.20 against the model's 1.02-1.09), so L1 never started; a
// broken-cloud hour (max/avg ~1.6) still fails. The tolerance follows what the cell knows (learnSteadyPct): 130 % for
// an empty cell, 115 % from its first bar. L1's gate and L5's clear label
constexpr long L1_STEADY_HI = 130, L1_STEADY_LO = 115;
// a cell at medium seeds its empty neighbours (one band up / down, same AM / PM) with its E, first at its
// first medium and again whenever their E is 0.05 or more away from it (1e-4)
constexpr int L1_SEED_MOVE = 500;
static_assert(fcst::SKY_STEADY_PCT == L1_STEADY_LO, "the sky words' steadiness is L1's known-cell tolerance");
// the per-day update cap (2 per day): day field = 14-bit day | count << 14; true = allowed (and counted)
LEARN_NOINLINE bool capTake(uint16_t* f, uint16_t today) {
  const uint16_t d = *f & 0x3FFF, n = *f >> 14;
  if (d != today) {
    *f = (uint16_t)(today | (1u << 14));
    return true;
  }
  if (n >= 2) return false;
  *f = (uint16_t)(today | ((n + 1u) << 14));
  return true;
}
uint32_t fnv(const void* p, size_t n, uint32_t h = 2166136261u) {
  for (size_t i = 0; i < n; i++) h = (h ^ ((const uint8_t*)p)[i]) * 16777619u;
  return h;
}
}  // namespace

#if FC_DIAG_BENCH  // the gate names reach only the bench JSON
static const char* const GATE_NAME[LG_COUNT] = {"quality", "readings", "physics", "frozen", "indoor", "geo", "dome",
                                                "iced", "stuck", "steady", "k_range", "k_low", "cap", "location", "baro",
                                                "scratch", "l5_label", "l5_evening", "l6_night", "melt"};
#endif
static const char* const LBL_NAME[6] = {"", "clear", "overcast", "mixed", "nocell", "none"};
static const char* const L6R_NAME[8] = {"ok", "rain", "wind", "rise", "quality", "not_clear", "evening", "missing"};
#if FC_DIAG_BENCH
const char* learnGateName(int g) { return g >= 0 && g < LG_COUNT ? GATE_NAME[g] : "?"; }
#endif
static const char* const CONF_NAME[4] = {"default", "low", "medium", "high"};
const char* learnConfName(int c) { return c >= 0 && c < 4 ? CONF_NAME[c] : "default"; }

// ---------------------------------------------------------------- the bars by the firmware's own count
namespace {
struct BarK {
  uint32_t lam;
  uint16_t one;  // 0 = L6 (W in Q16, judged on W >> 10)
  uint32_t t[3];
};
const BarK BARK[LBR_COUNT] = {{L1_LAM, 64, {L1_NMIN, 2 * L1_NMIN, L1_NHI}},
                              {L2A_LAM, 64, {L2A_NMIN, 2 * L2A_NMIN, L2A_NHI}},
                              {L2B_LAM, 64, {L2B_NMIN, 2 * L2B_NMIN, L2B_NHI}},
                              {L3_LAM, 64, {L3_NMIN, 2 * L3_NMIN, L3_NHI}},
                              {L4_LAM, 16, {L4_NMIN, 2 * L4_NMIN, L4_NHI}},
                              {L5_LAM, 64, {L5_NMIN, 2 * L5_NMIN, L5_NHI}},
                              {L6_LAM, 0, {15 * 64, 30 * 64, 43 * 64}},
                              {L7_LAM, 64, {L7_N1, L7_N2, L7_N2}}};
uint32_t l6Decay(uint32_t w, uint32_t lam) { return (uint32_t)(((uint64_t)w * lam + 32768) >> 16); }
}  // namespace

uint16_t learnNeed(int id, uint32_t n, int level) {
  if (id < 0 || id >= LBR_COUNT || level < 1 || level > 3) return 0;
  const BarK& k = BARK[id];
  const uint32_t t = k.t[level - 1];
  uint16_t steps = 0;
  if (!k.one) {
    while ((n >> 10) < t && steps < 2000) n = l6Decay(n, k.lam) + 65536u, steps++;
  } else {
    while (n < t && steps < 2000) n = neff(n, k.lam, k.one), steps++;
  }
  return steps;
}
uint16_t learnBarSamples(int id, int level) { return learnNeed(id, 0, level); }

int learnSteadyPct(uint16_t n) {
  const uint32_t m = n < L1_NMIN ? n : L1_NMIN;
  return (int)(L1_STEADY_HI - (L1_STEADY_HI - L1_STEADY_LO) * (long)m / (long)L1_NMIN);
}

uint32_t learnLocSig(float lat, float lon) {
  if (std::isnan(lat) || std::isnan(lon)) return 0;
  const int32_t q[2] = {(int32_t)std::lround(lat * 10.0), (int32_t)std::lround(lon * 10.0)};
  const uint32_t h = fnv(q, sizeof q);
  return h ? h : 1;
}

uint32_t learnDayOf(uint32_t unix, float lon) {
  return (uint32_t)std::floor(unix / 86400.0 + (std::isnan(lon) ? 0.0 : lon / 360.0));
}

static void priorsL1(LearnCore& c) {
  for (auto& x : c.l1) x = LearnCell{(uint16_t)L1_PRIOR, 0, 0};
}
static void priorsL5(LearnCore& c) {
  c.l5_ce = L5_CE_PRIOR, c.l5_oe = L5_OE_PRIOR;
  c.l5_cn = c.l5_on = c.l5_cd = c.l5_od = 0;
}
static void priorsL6(LearnCore& c) {
  c.l6_w = c.l6_e2 = c.l6_ea = 0;
  c.l6_s = c.l6_l = c.l6_y = c.l6_ss = c.l6_sl = c.l6_ll = c.l6_sy = c.l6_ly = 0;
  c.l6_a = L6_A_PRIOR, c.l6_b = L6_B_PRIOR, c.l6_c = L6_C_PRIOR, c.l6_day = 0;
}
// the priors of the learners in `mask` (LM_ALL = every learner: priorsAll)
static void priorsMasked(LearnCore& c, uint16_t mask) {
  if (mask & LM_L1) priorsL1(c);
  if (mask & LM_L2) c.l2_wet_e = c.l2_night_e = L2_PRIOR, c.l2_wet_n = c.l2_night_n = c.l2_wet_day = c.l2_night_day = 0;
  if (mask & LM_L3) c.l3_e = L3_PRIOR, c.l3_n = c.l3_day = 0;
  if (mask & LM_L4) c.l4_p50 = L4_50_PRIOR, c.l4_p90 = L4_90_PRIOR, c.l4_n = c.l4_day = 0;
  if (mask & LM_L7) c.l7_e = 0, c.l7_n = 0, c.l7_flag = 0;
  if (mask & LM_L5) priorsL5(c);
  if (mask & LM_L6) priorsL6(c);
}
static void priorsAll(LearnCore& c) { priorsMasked(c, LM_ALL); }

static void initCore(LearnCore* c) {
  memset(c, 0, sizeof *c);
  c->magic[0] = 'L', c->magic[1] = 'R', c->version = LEARN_VERSION, c->flags = LF_FRESH;
  c->icy_h = 255;  // no icy rain seen yet, so a dome set on the first night can melt that evening
  c->e_t0 = c->e_dt3 = c->n_tprev = I16_NONE;
  c->l7_pprev = I16_NONE;
  priorsAll(*c);
}
void learnInit(LearnState* s) {
  memset(s, 0, sizeof *s);
  initCore(&s->c);
}

// ---------------------------------------------------------------- blob migration (versions migrate, never reset)
// V1 and V2 -> V3. Every field the source version had is copied bit for bit; the V3 fields (l7_pprev, relearn_hour)
// and, from V1, L5 / L6 and the V2 scratch at init. The pass replays the learners new since the source version and the
// ones whose rules changed in V3 (L1, L2, L4: warm-up, seeds, melt gate), each from its priors.
bool learnMigrateCore(const void* raw, size_t len, LearnCore* out, uint8_t* from, uint16_t* replay) {
  static_assert(sizeof(LearnCoreV1) == sizeof(LearnCoreV2), "one stored length for V1 and V2");
  if (len != sizeof(LearnCoreV2)) return false;
  const uint8_t ver = ((const uint8_t*)raw)[offsetof(LearnCoreV2, version)];
  initCore(out);
  if (ver == 1) {
    LearnCoreV1 v1;
    memcpy(&v1, raw, sizeof v1);
#define LEARN_V1_COPY(f) memcpy(&out->f, &v1.f, sizeof out->f);
    LEARN_V1_SHARED(LEARN_V1_COPY)
#undef LEARN_V1_COPY
    *replay = LM_SINCE_V1 | LM_SINCE_V2;
  } else if (ver == 2) {
    LearnCoreV2 v2;
    memcpy(&v2, raw, sizeof v2);
#define LEARN_V2_COPY(f) memcpy(&out->f, &v2.f, sizeof out->f);
    LEARN_V2_SHARED(LEARN_V2_COPY)
#undef LEARN_V2_COPY
    *replay = LM_SINCE_V2;
  } else {
    return false;  // an unknown older version: priors + full replay
  }
  // L7's meaning changed in V3 (running mean, 30-day lambda, flags none/low/high): the
  // value stays as the start point, the flag is cleared and n capped below stage 1, so the check needs fresh days
  out->l7_flag = 0;
  if (out->l7_n > L7_N1 - 64) out->l7_n = (uint16_t)(L7_N1 - 64);
  out->version = LEARN_VERSION;
  *from = ver;
  if (!out->last_hour) *replay = 0;  // never fed: the normal full backfill feeds every learner
  return true;
}

void learnPassStart(LearnState* p, const LearnCore& migrated, uint16_t replay, const uint8_t* mask) {
  learnInit(p);
  const LearnCore init = p->c;
  p->c = migrated;
  LearnCore& c = p->c;
  priorsMasked(c, replay);  // the replayed learners start over
  c.flags &= (uint8_t)~LF_RELEARNED;  // a restart inside the walked hours is re-applied at relearn_hour
  c.l7_pprev = I16_NONE;
  c.l7_step = 0;  // no REL-step mark across a pass start
  if (mask) memcpy(p->mask, mask, sizeof p->mask);
  c.indoor = init.indoor, c.frozen_run = init.frozen_run, c.geo_days = init.geo_days, c.gbits = init.gbits;
  c.stuck_run = init.stuck_run, c.icy_h = init.icy_h, c.stuck_t0 = init.stuck_t0, c.gate_day = init.gate_day;
  c.prev_rain = init.prev_rain;
  c.n_start = init.n_start, c.n_len = init.n_len, c.n_rhmax = init.n_rhmax, c.n_hours = init.n_hours, c.n_ok = init.n_ok;
  c.n_flags = init.n_flags, c.n_calm = init.n_calm, c.n_t2 = init.n_t2, c.n_td2 = init.n_td2, c.n_t0 = init.n_t0;
  c.n_dt3 = init.n_dt3, c.n_tmin = init.n_tmin;
#define LEARN_INIT_COPY(f) c.f = init.f;
  LEARN_V2_SCRATCH(LEARN_INIT_COPY)
#undef LEARN_INIT_COPY
  c.last_hour = c.last_day = 0;
}

void learnPassMerge(LearnCore& dst, const LearnCore& p, uint16_t mask, uint32_t end) {
#define LEARN_MERGE(f) memcpy(&dst.f, &p.f, sizeof dst.f);
  if (mask & LM_L1) { LEARN_L1_FIELDS(LEARN_MERGE) }
  if (mask & LM_L2) { LEARN_L2_FIELDS(LEARN_MERGE) }
  if (mask & LM_L4) { LEARN_L4_FIELDS(LEARN_MERGE) }
  if (mask & LM_L5) { LEARN_L5_FIELDS(LEARN_MERGE) }
  if (mask & LM_L6) { LEARN_L6_FIELDS(LEARN_MERGE) }
  if (p.last_hour == end && p.n_start == dst.n_start) {  // the pass saw the same open night (or none): carried
    LEARN_V2_SCRATCH(LEARN_MERGE)
    dst.n_flags = (uint8_t)((dst.n_flags & ~LN_RISE) | (p.n_flags & LN_RISE));
  } else {  // that one evening / night is not judged (EB_MISS); n_w5 255 = calm hours unknown, not wind
    LearnCore init;
    initCore(&init);
#define LEARN_INIT_SCRATCH(f) dst.f = init.f;
    LEARN_V2_SCRATCH(LEARN_INIT_SCRATCH)
#undef LEARN_INIT_SCRATCH
    dst.e_flags = EB_MISS, dst.n_w5 = 255;
  }
#undef LEARN_MERGE
}

uint8_t learnConfOf(uint16_t n, uint16_t nmin, uint16_t nhigh, uint16_t last_day, uint16_t today) {
  int lvl = n < nmin ? LC_DEFAULT : n < 2 * nmin ? LC_LOW : n < nhigh ? LC_MEDIUM : LC_HIGH;
  const int age = ageDays(today, last_day);
  if (lvl && age > 60) {  // no counted sample for > 60 days: low at most, then one level less per 60 days
    lvl = lvl > LC_LOW ? LC_LOW : lvl;
    lvl -= age / 60 - 1;
  }
  return (uint8_t)(lvl < 0 ? 0 : lvl);
}

// the L1b cell of a sun position (rows by elevation, +1 deg edges): 3-21 2deg x 5deg, 21-41 4 x 10, 41-61 5 x 15, 61-81 10 x 30
int l1bCell(double el, double az) {
  if (!(el >= 3)) return -1;
  if (az < 0) az = 0;
  if (az >= 360) az = 359.999;
  if (el < 21) return (int)((el - 3) / 2) * 72 + (int)(az / 5);
  if (el < 41) return 648 + (int)((el - 21) / 4) * 36 + (int)(az / 10);
  if (el < 61) return 828 + (int)((el - 41) / 5) * 24 + (int)(az / 15);
  if (el < 81) return 924 + (int)((el - 61) / 10) * 12 + (int)(az / 30);
  return 948;
}

// sun = v >= 6, b >= 2, 10 b >= v; shade = v >= 6, 10 b < v; anything else unknown
uint8_t l1bClass(uint8_t cell) {
  const int v = cell >> 4, b = cell & 15;
  if (v < 6) return 0;
  if (10 * b < v) return 2;
  return b >= 2 ? 1 : 0;
}

static int l1Band(double el) { return el < 10 ? 0 : el < 15 ? 1 : el < 20 ? 2 : el < 30 ? 3 : el < 45 ? 4 : 5; }

// context changes: location -> L1 + L1b priors; profile -> all priors; console MAC -> L7 prior
static uint8_t context(LearnState& s, const LearnCtx& ctx, uint16_t mask) {
  LearnCore& c = s.c;
  const uint32_t sig = learnLocSig(ctx.lat, ctx.lon);
  uint8_t ev = 0;
  if (c.flags & LF_FRESH) {  // the first record fed: adopt the context, nothing to reset
    c.flags &= (uint8_t)~LF_FRESH;
    // an (empty) mask stored before the first hour carries loc_sig 0; it is stored again with the place now
    if (sig && c.loc_sig != sig) s.mask_dirty = true;
    if (sig) c.loc_sig = sig;  // an unknown place keeps the one the mask came with
    c.profile = ctx.profile, c.mac_hash = ctx.mac_hash;
    return 0;
  }
  // an unknown place (sig 0: no location set) judges no change -- nothing reset, the place
  // kept; the location learners pause meanwhile (no sun position: L1, L1b, the night, L5, L6 skip). A place set after
  // an unknown one is adopted, not a move.
  if (sig && !c.loc_sig) c.loc_sig = sig, s.mask_dirty = true;
  if (sig && sig != c.loc_sig) {
    if (mask & LM_L1) priorsL1(c);
    if (mask & LM_L1B) memset(s.mask, 0, sizeof s.mask), s.sc_n = 0, s.mask_dirty = true;
    c.loc_sig = sig;
    c.n_start = 0;
    if (mask & LM_L5) priorsL5(c);  // the evening and the night belong to the place
    if (mask & LM_L6) priorsL6(c);
    c.lbl = LBL_UNSET, c.e_flags = 0;
    ev |= LE_LOC;
  }
  if (ctx.profile != c.profile) {
    priorsMasked(c, mask);
    if (mask & LM_L1B) memset(s.mask, 0, sizeof s.mask), s.sc_n = 0, s.mask_dirty = true;  // L1b too
    c.profile = ctx.profile;
    ev |= LE_PROFILE;
  }
  if (ctx.mac_hash && ctx.mac_hash != c.mac_hash) {
    if (c.mac_hash) {
      if (mask & LM_L7) c.l7_e = 0, c.l7_n = 0, c.l7_flag = 0;
      ev |= LE_MAC;
    }
    c.mac_hash = ctx.mac_hash;
  }
  if (ev) s.core_dirty = true;
  return ev;
}

// a new sensor epoch or "Restart learning" -> values kept as the start point, every n_eff 0 (below the
// first bar: the fixed rules until new evidence, which the warm-up x4 then reaches fast; L2 re-seeds from its next
// sample), L6's sums scaled to 5 nights, L7 starts over, the mask cleared, eta x 4 for 14 days
static uint8_t relearn(LearnState& s, uint8_t epoch, uint16_t today, uint16_t mask) {
  LearnCore& c = s.c;
  if (mask & LM_L1)
    for (auto& x : c.l1) x.n = 0;
  if (mask & LM_L2) c.l2_wet_n = c.l2_night_n = 0;
  if (mask & LM_L3) c.l3_n = 0;
  if (mask & LM_L4) c.l4_n = 0;
  if (mask & LM_L5) c.l5_cn = c.l5_on = 0;
  if (mask & LM_L7) c.l7_e = 0, c.l7_n = 0, c.l7_flag = 0;
  // L6: every sum scaled to at most 5 nights (below the readiness weight of 15); (a, b, c) kept until the
  // next admitted night
  constexpr uint32_t L6_KEEP = 5u * 65536u;
  if ((mask & LM_L6) && c.l6_w > L6_KEEP) {
    const int64_t num = L6_KEEP, den = c.l6_w;
    int32_t* const S[8] = {&c.l6_s, &c.l6_l, &c.l6_y, &c.l6_ss, &c.l6_sl, &c.l6_ll, &c.l6_sy, &c.l6_ly};
    for (int32_t* p : S) *p = (int32_t)((int64_t)*p * num / den);
    c.l6_e2 = (uint32_t)((int64_t)c.l6_e2 * num / den), c.l6_ea = (uint32_t)((int64_t)c.l6_ea * num / den);
    c.l6_w = L6_KEEP;
  }
  if (mask & LM_L1B) memset(s.mask, 0, sizeof s.mask), s.sc_n = 0, s.mask_dirty = true;
  s.core_dirty = true;
  c.epoch = epoch;
  c.relearn_until = (uint16_t)((today + 14) & 0x3FFF);
  if (!c.relearn_until) c.relearn_until = 1;
  return LE_RELEARN;
}
static int mult(const LearnCore& c, uint16_t today) {
  return c.relearn_until && ageDays(c.relearn_until, today) <= 14 && c.relearn_until != today ? 4 : 1;
}
// the warm-up by the learner's count `n` against its use level `t` (same unit):
// x4 below three quarters of it, x2 up to eleven twelfths, x1 in the last twelfth (the jitter of a x4 step settles
// before the bar; the far end is travelled early), never under the relearn window's x4
static int warm(const LearnCore& c, uint16_t today, uint32_t n, uint32_t t) {
  const int r = n * 4 < t * 3 ? WARM : n * 12 < t * 11 ? WARM / 2 : 1, m = mult(c, today);
  return r > m ? r : m;
}

uint8_t learnRestart(LearnState& s, float lon) {
  LearnCore& c = s.c;
  const uint32_t h = c.last_hour;
  relearn(s, c.epoch, day14(learnDayOf(h + 1800, lon)), LM_ALL);
  if (!h) c.relearn_until = 0;  // never fed: nothing to re-learn, no x4 window
  c.relearn_hour = h, c.flags |= LF_RELEARNED;
  return LE_RELEARN;
}

// The hour's sun: mean elevation and clear sky over 12 five-minute mid-steps, the max, AM / PM at mid-hour
struct HourSun {
  bool ok;
  double el;        // mean elevation, deg
  double cs_avg, cs_max;
  bool pm;
  double bin_el[LEARN_BINS], bin_az[LEARN_BINS], bin_cs[LEARN_BINS];
  bool bin_pm[LEARN_BINS];
  int mid_cell;  // the L1b cell of the mid-hour sun position (-1 = none)
};
static LEARN_NOINLINE void hourSun(uint32_t hour, float lat, float lon, HourSun* h) {
  h->ok = !std::isnan(lat) && !std::isnan(lon);
  h->mid_cell = -1;
  if (!h->ok) return;
  SunGeo g;
  sunGeo(hour + 1800, &g);
  double se = 0, sc = 0, mx = 0, ha = 0;
  for (int i = 0; i < LEARN_BINS; i++) {
    const uint32_t t = hour + 150 + 300u * (uint32_t)i;
    sunPosG(g, t, lat, lon, &h->bin_el[i], &h->bin_az[i], &ha);
    h->bin_pm[i] = ha >= 0;
    h->bin_cs[i] = clearSky(h->bin_el[i]);
    se += h->bin_el[i], sc += h->bin_cs[i];
    if (h->bin_cs[i] > mx) mx = h->bin_cs[i];
  }
  double e, a;
  sunPosG(g, hour + 1800, lat, lon, &e, &a, &ha);
  h->pm = ha >= 0;
  h->mid_cell = l1bCell(e, a);
  h->el = se / LEARN_BINS, h->cs_avg = sc / LEARN_BINS, h->cs_max = mx;
}

// L1b: the hour's bins into the day scratch (one visit per cell per hour: its brightest bin); the day is clear when a
// bin reached 0.85 E of its L1 cell
static LEARN_NOINLINE void l1bHour(LearnState& s, const HourSun& hs, const LearnCtx& ctx) {
  int cellOf[LEARN_BINS];
  uint16_t kOf[LEARN_BINS];
  for (int i = 0; i < LEARN_BINS; i++) {
    cellOf[i] = -1, kOf[i] = 0;
    if (ctx.bin_n[i] < 3 || hs.bin_cs[i] <= 0) continue;
    const int cell = l1bCell(hs.bin_el[i], hs.bin_az[i]);
    if (cell < 0) continue;
    const double k = ctx.bin_sum[i] / (double)ctx.bin_n[i] / hs.bin_cs[i];
    const long kq = std::lround(k * 1e4);
    const LearnCell& lc = s.c.l1[l1Band(hs.bin_el[i] < 5 ? 5 : hs.bin_el[i]) * 2 + (hs.bin_pm[i] ? 1 : 0)];
    const int E = lc.n >= L1_NMIN ? lc.e : L1_PRIOR;
    if (s.l1b_dome) {
      if (kq * 10 >= (long)E * 8) s.l1b_dome = 0;  // the first bright bin melts it
      else continue;
    }
    if (kq * 100 >= (long)E * 85) s.sc_clear = 1;
    cellOf[i] = cell;
    kOf[i] = (uint16_t)(kq > 65535 ? 65535 : kq < 0 ? 0 : kq);
  }
  for (int i = 0; i < LEARN_BINS; i++) {
    if (cellOf[i] < 0) continue;
    int best = i;
    for (int j = i + 1; j < LEARN_BINS; j++)
      if (cellOf[j] == cellOf[i]) {
        if (kOf[j] > kOf[best]) best = j;
        cellOf[j] = -1;
      }
    const LearnCell& lc = s.c.l1[l1Band(hs.bin_el[best] < 5 ? 5 : hs.bin_el[best]) * 2 + (hs.bin_pm[best] ? 1 : 0)];
    const int E = lc.n >= L1_NMIN ? lc.e : L1_PRIOR;
    const uint8_t b = (uint8_t)((long)kOf[best] * 2 >= E);
    // one entry per cell per day, the brightest pass kept (a clear June day crosses ~60 cells in ~72 visits)
    int j = 0;
    while (j < s.sc_n && s.sc_cell[j] != cellOf[i]) j++;
    if (j < s.sc_n) {
      if (b > s.sc_b[j]) s.sc_b[j] = b;
    } else if (s.sc_n < L1B_SCRATCH) {
      s.sc_cell[s.sc_n] = (uint16_t)cellOf[i];
      s.sc_b[s.sc_n] = b;
      s.sc_n++;
    } else {
      LGATE(s, LG_SCRATCH);
    }
  }
}
// at sunset: a clear day commits its visits, any other day discards them
static uint8_t l1bSunset(LearnState& s) {
  uint8_t ev = 0;
  if (s.sc_n && s.sc_clear) {
    for (int i = 0; i < s.sc_n; i++) {
      uint8_t& m = s.mask[s.sc_cell[i]];
      int v = (m >> 4) + 1, b = (m & 15) + s.sc_b[i];
      if (v >= 15) v /= 2, b /= 2;
      m = (uint8_t)(v << 4 | (b > 15 ? 15 : b));
    }
    s.mask_dirty = true;
    ev = LE_L1B_COMMIT;
  }
  s.sc_n = 0, s.sc_clear = 0;
  return ev;
}

// ---------------------------------------------------------------- L5 and L6
int l5Threshold(const LearnCore& c, L5Src* src, bool* hit) {
  *hit = false;
  if (c.l5_cn < L5_NMIN || c.l5_on < L5_NMIN) {
    *src = L5Src::Default;
    return L5_THR_DEF;
  }
  if (c.l5_ce - c.l5_oe < L5_SEP) {  // classes merged or inverted: the label is not seeing the sky
    *src = L5Src::Inseparable;
    return L5_THR_DEF;
  }
  *src = L5Src::Learned;
  const int v = (c.l5_ce + c.l5_oe) / 2;
  *hit = v < L5_THR_LO || v > L5_THR_HI;
  return v < L5_THR_LO ? L5_THR_LO : v > L5_THR_HI ? L5_THR_HI : v;
}

// the bad bits of one hour of the evening window
static uint8_t eveBits(const HourRec& r, bool ok) {
  uint8_t b = ok ? 0 : EB_GATE;
  if (hasU16(r.rain_sum) && r.rain_sum) b |= EB_RAIN;
  if (hasU16(r.wind_avg) && r.wind_avg >= 50) b |= EB_WIND;
  if (!hasI16(r.t_avg)) b |= EB_MISS;
  return b;
}

// L5 at the evening's third step (E0 + 3): one sample of the day's class, the running mean until 20 evenings, then 1/20
static void l5Evening(LearnState& s, uint16_t dayE0, int m) {
  LearnCore& c = s.c;
  if (c.e_flags & EB_BAD) {
    LGATE(s, LG_L5_EVENING);
    return;
  }
  const bool clear = c.lbl == LBL_CLEAR;
  if (c.lbl_day != dayE0 || (!clear && c.lbl != LBL_OVERCAST)) {
    LGATE(s, LG_L5_LABEL);
    return;
  }
  int x = -(c.e_dt3 * 10) / 3;  // 0.01 °C/h, cooling positive
  x = x < -200 ? -200 : x > 500 ? 500 : x;
  int16_t* e = clear ? &c.l5_ce : &c.l5_oe;
  uint16_t* n = clear ? &c.l5_cn : &c.l5_on;
  uint32_t a = 65536u * 64u / (*n + 64u);
  if (a < L5_ALPHA * (uint32_t)m) a = L5_ALPHA * (uint32_t)m;
  *e = (int16_t)(*e + (((x - *e) * (int32_t)a + 32768) >> 16));
  *n = neff(*n, L5_LAM, 64);
  (clear ? c.l5_cd : c.l5_od) = dayE0;
}

static int32_t decayS(int32_t v, uint32_t lam) { return (int32_t)(((int64_t)v * lam + 32768) >> 16); }
static uint32_t decayU(uint32_t v, uint32_t lam) { return (uint32_t)(((uint64_t)v * lam + 32768) >> 16); }
// n / d rounded half away from zero (d > 0)
static int64_t divRound(int64_t n, int64_t d) { return n >= 0 ? (n + d / 2) / d : -((-n + d / 2) / d); }

// The ridge solve: (M + K) beta = r + K beta0 on the centred variables, in a transient
// double by Cramer's rule with + - x / only in this fixed order; no FMA contraction (pragma / attribute below; the ESP32
// has no double FMA anyway), no libm. Only the quantised integers are stored. MSVC: contraction stays off for the rest of
// this file too (the replay == board rule wants it everywhere here; /fp:precise has it off by default since
// VS 2022). GCC: the attribute on the one out-of-line copy (noinline, so no inlined copy follows the caller).
#if defined(_MSC_VER)
#pragma fp_contract(off)
#endif
#if defined(__GNUC__) && !defined(__clang__)
__attribute__((optimize("fp-contract=off")))
#endif
LEARN_NOINLINE bool l6Solve(LearnCore& c) {
  // K = diag(0, 5 x 30², 5 x 20²) (no intercept prior): 5 pseudo-nights at spreads 3.0 °C / 2.0 h on the slopes only
  constexpr double KA = 0.0, KB = 4500.0, KC = 2000.0, A0 = 54.0, B0 = 0.4, C0 = 0.2;
  const double w = c.l6_w / 65536.0;
  const double m00 = w + KA, m01 = c.l6_s / 256.0, m02 = c.l6_l / 256.0;
  const double m11 = c.l6_ss / 256.0 + KB, m12 = c.l6_sl / 256.0, m22 = c.l6_ll / 256.0 + KC;
  const double r0 = c.l6_y / 256.0 + KA * A0, r1 = c.l6_sy / 256.0 + KB * B0, r2 = c.l6_ly / 256.0 + KC * C0;
  const double c00 = m11 * m22 - m12 * m12, c01 = m01 * m22 - m12 * m02, c02 = m01 * m12 - m11 * m02;
  const double det = m00 * c00 - m01 * c01 + m02 * c02;
  const double q1 = r1 * m22 - m12 * r2, q2 = m01 * r2 - r1 * m02;
  const double d0 = r0 * c00 - m01 * q1 + m02 * (r1 * m12 - m11 * r2);
  const double d1 = m00 * q1 - r0 * c01 + m02 * q2;
  const double d2 = m00 * (m11 * r2 - r1 * m12) - m01 * q2 + r0 * c02;
  const double al = d0 / det, b = d1 / det, cc = d2 / det;
  const double a01 = al - b * 50.0 - cc * 120.0;  // back to uncentred, 0.1 °C
  const long qa = std::lround(a01 * 10.0), qb = std::lround(b * 1e4), qc = std::lround(cc * 1e4);
  const bool hit = qa < L6_A_LO || qa > L6_A_HI || qb < 0 || qb > L6_B_HI || qc < 0 || qc > L6_C_HI;
  c.l6_a = (int16_t)(qa < L6_A_LO ? L6_A_LO : qa > L6_A_HI ? L6_A_HI : qa);
  c.l6_b = (int16_t)(qb < 0 ? 0 : qb > L6_B_HI ? L6_B_HI : qb);
  c.l6_c = (int16_t)(qc < 0 ? 0 : qc > L6_C_HI ? L6_C_HI : qc);
  return hit;
}

// L6 at a night's close: the prediction made before the night is learned (JSON / replay scoring), the admission, then the
// prospective residual, the forgetting sums and the solve
int16_t l6Predict(int16_t t2, int16_t td2, uint8_t l_h, int a, int b, int c, int16_t* drop) {
  if (!hasI16(t2) || !hasI16(td2)) return I16_NONE;
  const int S = t2 - td2 < 0 ? 0 : t2 - td2 > 300 ? 300 : t2 - td2, Lq = l_h > 240 ? 240 : l_h;
  const int64_t yhat = divRound((int64_t)a * 1000 + (int64_t)b * S + (int64_t)c * Lq, 10000);
  if (drop) *drop = (int16_t)yhat;
  int64_t p = t2 - yhat;
  if (p < td2 - 50) p = td2 - 50;
  if (p > t2) p = t2;
  return (int16_t)p;
}

static void l6Night(LearnState& s, LearnNight& n, uint16_t today, int m) {
  LearnCore& c = s.c;
  const bool vals = hasI16(n.t2) && hasI16(n.td2);
  const int S = vals ? (n.t2 - n.td2 < 0 ? 0 : n.t2 - n.td2 > 300 ? 300 : n.t2 - n.td2) : 0;
  const int Lq = n.l_h > 240 ? 240 : n.l_h;
  int16_t yhat = 0;
  n.pred = l6Predict(n.t2, n.td2, n.l_h, c.l6_a, c.l6_b, c.l6_c, &yhat);
  L5Src src;
  bool thit;
  const int thr = l5Threshold(c, &src, &thit);
  uint8_t why = L6R_OK;
  if ((n.flags & LN_PARTIAL) || n.hours != n.len) why = L6R_MISSING;
  else if (!n.admitted || (n.flags & LN_RESTART) || n.ok != n.len) why = L6R_QUALITY;
  else if (n.flags & LN_RAIN) why = L6R_RAIN;
  else if (n.flags & LN_RISE) why = L6R_RISE;
  else if (n.w5 * 5 < n.len * 4) why = L6R_WIND;
  else if ((c.e_flags & EB_BAD) || !(c.e_flags & EB_DONE) || !hasI16(c.e_dt3)) why = L6R_EVENING;
  else if (-(c.e_dt3 * 10) / 3 < thr) why = L6R_NOT_CLEAR;
  else if (!vals || !hasI16(n.tmin) || n.td2 > n.t2 + 2) why = L6R_MISSING;
  n.l6 = why;
  if (why != L6R_OK) {
    LGATE(s, LG_L6_NIGHT);
    return;
  }
  int y = n.t2 - n.tmin;
  y = y < -100 ? -100 : y > 300 ? 300 : y;
  const int sv = S - 50, lv = Lq - 120;
  int e = (int)(y - yhat);
  e = e < -250 ? -250 : e > 250 ? 250 : e;
  const uint32_t lam = m > 1 ? L6_LAM_RE : L6_LAM;
  c.l6_e2 = decayU(c.l6_e2, lam) + (uint32_t)(e * e) * 256u;
  c.l6_ea = decayU(c.l6_ea, lam) + (uint32_t)(e < 0 ? -e : e) * 256u;
  c.l6_s = decayS(c.l6_s, lam) + sv * 256, c.l6_l = decayS(c.l6_l, lam) + lv * 256, c.l6_y = decayS(c.l6_y, lam) + y * 256;
  c.l6_ss = decayS(c.l6_ss, lam) + sv * sv * 256, c.l6_sl = decayS(c.l6_sl, lam) + sv * lv * 256;
  c.l6_ll = decayS(c.l6_ll, lam) + lv * lv * 256, c.l6_sy = decayS(c.l6_sy, lam) + sv * y * 256;
  c.l6_ly = decayS(c.l6_ly, lam) + lv * y * 256;
  c.l6_w = decayU(c.l6_w, lam) + 65536u;
  const bool hit = l6Solve(c);
  s.hit = (uint8_t)(hit ? s.hit | LH_L6 : s.hit & ~LH_L6);
  c.l6_day = today;
}

// the night summary: opens at the first hour starting >= sunset + 1 h, closes at the first one >= sunrise + 1 h.
// The evening window E0 .. E0 + 3 (E0 = the first hour starting at or after sunset = the hour before the night), L5 at
// its third step, L6 at the close.
static uint8_t night(LearnState& s, const HourRec& r, bool ok, float lat, float lon, int m, uint16_t mask) {
  LearnCore& c = s.c;
  uint32_t rise = 0, set = 0;
  if (c.n_start) {
    const uint32_t end = c.n_start + 3600u * c.n_len;
    if (r.hour < end) {
      c.n_hours++;
      if (ok) c.n_ok++;
      if (r.quality & (1u << Q_RESTART)) c.n_flags |= LN_RESTART;
      if (hasU16(r.rain_sum) && r.rain_sum) c.n_flags |= LN_RAIN;
      if (hasU8(r.rh_max) && (c.n_rhmax == U8_NONE || r.rh_max > c.n_rhmax)) c.n_rhmax = r.rh_max;
      if (calmHour(r)) c.n_calm++;
      const uint32_t k = (r.hour - c.n_start) / 3600;
      if (k == 1) c.n_t2 = r.t_avg, c.n_td2 = r.td_avg;
      if (k == 3 && hasI16(c.n_t0) && hasI16(r.t_avg)) c.n_dt3 = (int16_t)(r.t_avg - c.n_t0);
      if (hasI16(r.t_min) && (!hasI16(c.n_tmin) || r.t_min < c.n_tmin)) c.n_tmin = r.t_min;
      if (ok && hasI16(r.t_avg) && hasI16(r.td_avg) && r.t_avg - 20 <= r.td_avg && r.t_avg < 0) c.n_flags |= LN_HOAR;
      // calm-ish hours; a rise of 1.5 °C or more before sunrise (cloud arriving)
      if (hasU16(r.wind_avg) && r.wind_avg < 50 && c.n_w5 < 255) c.n_w5++;
      if (hasI16(r.t_avg) && hasI16(c.n_tprev) && r.t_avg - c.n_tprev >= 15 && r.hour + 7200 < end) c.n_flags |= LN_RISE;
      c.n_tprev = r.t_avg;
      if (!(c.e_flags & EB_DONE)) {  // the evening window's night hours (k 0 .. 2), judged once at the first k >= 2
        c.e_flags |= eveBits(r, ok);
        if (c.n_hours != k + 1) c.e_flags |= EB_MISS;  // an hour of the window is missing
        if (k >= 2) {
          if (k == 2 && hasI16(r.t_avg) && hasI16(c.e_t0)) c.e_dt3 = (int16_t)(r.t_avg - c.e_t0);
          else c.e_flags |= EB_MISS;
          c.e_flags |= EB_DONE;
          if (mask & LM_L5) l5Evening(s, day14(learnDayOf(c.n_start - 1800, lon)), m);
        }
      }
      s.core_dirty = true;
      return 0;
    }
    // close: >= 75 % of the night's hours pass the common gates, none carries `restart`
    LearnNight& n = s.last_night;
    n.start = c.n_start, n.len = c.n_len, n.rhmax = c.n_rhmax, n.hours = c.n_hours, n.ok = c.n_ok;
    n.flags = c.n_flags, n.calm = c.n_calm, n.t2 = c.n_t2, n.td2 = c.n_td2, n.t0 = c.n_t0, n.dt3 = c.n_dt3;
    n.tmin = c.n_tmin;
    n.l_h = c.n_L, n.w5 = c.n_w5;
    n.admitted = !(c.n_flags & LN_RESTART) && c.n_ok * 4 >= c.n_len * 3;
    c.n_start = 0;
    s.core_dirty = true;
    const uint16_t today = day14(learnDayOf(r.hour, lon));
    if (n.admitted) {
      if (n.flags & LN_HOAR) c.gbits |= LB_DOME, s.l1b_dome = 1;
      // L2 b: the nightly max of rh_max, P90 (stuck gate)
      // seeded from the first night (n 0), then x4 until its use level
      if ((mask & LM_L2) && hasU8(n.rhmax) && !(c.gbits & LB_STUCK)) {
        const int w = warm(c, today, c.l2_night_n, L2B_NMIN);
        if (!c.l2_night_n) {
          const int x = n.rhmax * 100;
          c.l2_night_e = (uint16_t)(x < 5000 ? 5000 : x > L2_HI ? L2_HI : x);
        } else {
          frugal(&c.l2_night_e, n.rhmax * 100, L2_P90_UP * w, L2_P90_DN * w, 5000, L2_HI);
        }
        c.l2_night_n = neff(c.l2_night_n, L2B_LAM, 64), c.l2_night_day = today;
      }
    }
    if (mask & LM_L6) l6Night(s, n, today, m);
    else n.l6 = L6R_OK, n.pred = I16_NONE;  // not judged (a pass without L6)
    return LE_NIGHT;
  }
  // open? the evening's day: the sunset of the hour's own solar day, else (after midnight) the day before
  if (sunTimes(r.hour, lat, lon, &rise, &set) != SunDay::Times) return 0;
  uint32_t ev_set = 0, next_rise = 0;
  if (r.hour >= set + 3600) {
    ev_set = set;
    uint32_t r2 = 0, s2 = 0;
    if (sunTimes(r.hour + 86400, lat, lon, &r2, &s2) != SunDay::Times) return 0;
    next_rise = r2;
  } else if (r.hour < rise + 3600) {
    uint32_t r0 = 0, s0 = 0;
    if (sunTimes(r.hour - 86400, lat, lon, &r0, &s0) != SunDay::Times || r.hour < s0 + 3600) return 0;
    ev_set = s0, next_rise = rise;
  } else {
    // E0, the first hour starting at or after sunset, opens the evening window (the night opens at E0 + 1 h)
    if (r.hour == learnE0(set)) {
      c.e_t0 = r.t_avg, c.e_dt3 = I16_NONE;
      c.e_flags = (uint8_t)(eveBits(r, ok) | EB_E0);
      s.core_dirty = true;
    }
    return 0;
  }
  const uint32_t start = (ev_set + 3600 + 3599) / 3600 * 3600, end = (next_rise + 3600 + 3599) / 3600 * 3600;
  if (end <= start) return 0;
  c.n_start = start, c.n_len = (uint8_t)((end - start) / 3600);
  c.n_rhmax = U8_NONE, c.n_hours = c.n_ok = c.n_flags = c.n_calm = 0;
  c.n_t2 = c.n_td2 = c.n_t0 = c.n_dt3 = c.n_tmin = I16_NONE;
  if (r.hour != start) c.n_flags |= LN_PARTIAL;
  c.n_t0 = r.hour == start ? r.t_avg : I16_NONE;
  // night length (0.1 h), calm-ish hours; the evening belongs to this night only when E0 was the hour just fed
  const uint32_t lh = (next_rise - ev_set + 180) / 360;
  c.n_L = (uint8_t)(lh > 255 ? 255 : lh), c.n_w5 = 0;
  const bool eve = r.hour == start && c.last_hour == start - 3600 && (c.e_flags & EB_E0);
  c.e_flags = eve ? (uint8_t)(c.e_flags & EB_BAD) : EB_MISS;
  if (!eve) c.e_t0 = I16_NONE;
  c.e_dt3 = I16_NONE, c.n_tprev = c.e_t0;
  return night(s, r, ok, lat, lon, m, mask);  // the hour itself is the night's first
}

uint8_t learnHour(LearnState& s, const HourRec& r, const LearnCtx& ctx, uint16_t mask) {
  LearnCore& c = s.c;
  if (r.hour <= c.last_hour) return 0;  // every hour key once, in increasing order
  // an hour stamped by an approximate clock may carry a wrong key: never a resume point, never learned from
  if (r.quality & (1u << Q_CLOCK_APPROX)) {
    LGATE(s, LG_QUALITY);
    return 0;
  }
  uint8_t ev = context(s, ctx, mask);
  const uint32_t dayN = learnDayOf(r.hour + 1800, ctx.lon);
  const uint16_t today = day14(dayN);
  if (r.epoch != c.epoch) ev |= relearn(s, r.epoch, today, mask);
  // a replay passing the "Restart learning" hour re-applies the restart (the live state has it applied)
  if (c.relearn_hour && !(c.flags & LF_RELEARNED) && c.last_hour <= c.relearn_hour && r.hour > c.relearn_hour) {
    ev |= relearn(s, c.epoch, day14(learnDayOf(c.relearn_hour + 1800, ctx.lon)), mask);
    c.flags |= LF_RELEARNED;
  }
  const int m = mult(c, today);
  s.hours_fed++;
  s.core_dirty = true;
  // the geo day count rolls over with the solar day
  if (c.gate_day != today) {
    c.geo_days = (c.gbits & LB_GEO_TODAY) ? (uint8_t)(c.geo_days < 255 ? c.geo_days + 1 : 255) : 0;
    c.gbits &= (uint8_t)~LB_GEO_TODAY;
    c.gate_day = today;
  }
  HourSun hs;
  hourSun(r.hour, ctx.lat, ctx.lon, &hs);
  const bool hasSolar = hasU16(r.solar_avg) && hasU16(r.solar_max);

  // trackers (every hour, admitted or not)
  if (hasI16(r.t_min) && hasI16(r.t_max)) c.frozen_run = r.t_min == r.t_max ? (uint8_t)(c.frozen_run < 255 ? c.frozen_run + 1 : 255) : 0;
  const bool icyRain = hasU16(r.rain_sum) && r.rain_sum > 0 && hasI16(r.t_avg) && r.t_avg <= 10;
  const uint16_t w = hasU16(r.wind_avg) ? r.wind_avg : 0, g = hasU16(r.gust_max) ? r.gust_max : 0;
  if ((c.gbits & LB_ICED) && (w > 0 || g > 0)) c.gbits &= (uint8_t)~LB_ICED;
  const bool icedHour = (c.gbits & LB_ICED) && w == 0 && g == 0;
  if (icyRain) c.gbits |= LB_ICED | LB_DOME, s.l1b_dome = 1, c.icy_h = 0;
  else if (c.icy_h < 255) c.icy_h++;
  if (hasU8(r.rh_min) && r.rh_min >= 99) {
    if (!c.stuck_run) c.stuck_t0 = r.t_avg;
    if (c.stuck_run < 255) c.stuck_run++;
    if (c.stuck_run > 12 && hasI16(r.t_avg) && hasI16(c.stuck_t0) && r.t_avg - c.stuck_t0 > 50 && hasSolar && r.solar_avg > 0)
      c.gbits |= LB_STUCK;
  } else {
    c.stuck_run = 0;
  }
  if (hasU8(r.rh_max) && r.rh_max < 99) c.gbits &= (uint8_t)~LB_STUCK;
  if (hs.ok && hs.el < 0 && c.icy_h >= 24) c.gbits &= (uint8_t)~LB_DOME;  // the dome melts by the evening
  // a REL change in the console restarts L7: a step >= 4 hPa between two consecutive clean hours marks it, and the next
  // consecutive clean hour confirms it when it stays within 2 hPa of the step hour (the new level holds). Any other hour
  // drops the mark -- a gap, a dirty hour, or a further move (a run of large moves is weather, so it opens no new mark).
  // Deliberate: a 4-hPa weather step directly followed by a REL change is missed (L7 then learns across the offset until its
  // own flag), the price of never restarting on a 4 + 4 + 2 hPa/h deepening low
  const bool pClean = !(r.quality & (Q_COMMON | Q_BARO)) && hasI16(r.p_avg);
  const bool next = pClean && hasI16(c.l7_pprev) && r.hour == c.last_hour + 3600;
  const int dp = next ? std::abs(r.p_avg - c.l7_pprev) : 0;
  const bool marked = c.l7_step != 0;
  if (next && marked && dp <= L7_HOLD && (mask & LM_L7) && c.l7_n) {
    c.l7_e = 0, c.l7_n = 0, c.l7_flag = 0;
    ev |= LE_L7_STEP;
  }
  c.l7_step = next && !marked && dp >= L7_STEP ? 1 : 0;
  c.l7_pprev = pClean ? r.p_avg : I16_NONE;
  bool geoHour = false;
  if (hs.ok && hasSolar) {
    if (hs.el >= 10) {  // indoor / covered: two dark day hours in a row, cleared by a day hour >= 0.1 clear sky
      if (r.solar_max <= 2) c.indoor = c.indoor >= 2 ? 2 : (uint8_t)(c.indoor + 1);
      else if (c.indoor < 2 || r.solar_avg * 10.0 >= hs.cs_avg) c.indoor = 0;
    }
    if (r.solar_avg > 5 && hs.el < -2) geoHour = true, c.gbits |= LB_GEO_TODAY;
  }

  // common admission
  int why = -1;
  if (r.quality & Q_COMMON) why = LG_QUALITY;
  else if (r.n < 10) why = LG_READINGS;
  else if ((hasU8(r.rh_avg) && (r.rh_avg == 0 || r.rh_avg > 100)) ||
           (hasI16(r.td_avg) && hasI16(r.t_avg) && r.td_avg > r.t_avg + 2) ||
           (hasU16(r.wind_avg) && hasU16(r.gust_max) && r.gust_max < r.wind_avg))
    why = LG_PHYSICS;
  else if (c.frozen_run >= 6) why = LG_FROZEN;
  else if (c.indoor >= 2) why = LG_INDOOR;
  const bool ok = why < 0;
  if (!ok) LGATE(s, why);
  if (hs.ok) ev |= night(s, r, ok, ctx.lat, ctx.lon, m, mask);
  // the day's late-afternoon sky as L5's class: every PM hour of mean elevation 5-15 deg labels the day (the last
  // one wins); a gated, dome or geo hour labels it "none"
  const bool lblBand = hs.ok && hs.pm && hs.el >= 5 && hs.el < 15;
  uint8_t lbl = LBL_NONE;

  if (ok) {
    // L1
    if (!hs.ok) LGATE(s, LG_LOCATION);
    else if (geoHour || c.geo_days >= 3) LGATE(s, LG_GEO);
    else if (hs.el >= 5 && hasSolar && hs.cs_avg > 0) {
      const long kq = std::lround(r.solar_avg * 1e4 / hs.cs_avg);
      const long rm = r.solar_avg ? std::lround(r.solar_max * 1e4 / r.solar_avg) : 0;
      const long rc = std::lround(hs.cs_max * 1e4 / hs.cs_avg);
      const int band = l1Band(hs.el), ci = band * 2 + (hs.pm ? 1 : 0);
      LearnCell& lc = c.l1[ci];
      const bool sunlit = kq >= L1_FLOOR[band];  // under the band's floor it is never clear sun
      if (c.gbits & LB_DOME) {
        if (kq * 10 >= (long)lc.e * 8) c.gbits &= (uint8_t)~LB_DOME;
      }
      const long steady = learnSteadyPct(lc.n);
      if (lblBand && !(c.gbits & LB_DOME)) {  // L5's label (the cell at medium confidence or better)
        // in a pass without L1 the converged cell is judged at age 0 (its day lies
        // after the replayed hour; the 14-bit age would wrap to "nocell")
        const uint16_t lday = (mask & LM_L1) ? today : (uint16_t)(lc.day & 0x3FFF);
        // the L1b class at the hour's mid sun position: brightness proves exposure (clear from a sun or an
        // unknown cell), darkness proves nothing unless the cell is known sun (overcast / mixed only there); shade: none
        const uint8_t cls = hs.mid_cell >= 0 ? l1bClass(s.mask[hs.mid_cell]) : 0;
        if (learnConfOf(lc.n, L1_NMIN, L1_NHI, lc.day & 0x3FFF, lday) < LC_MEDIUM) lbl = LBL_NOCELL;
        else if (cls == 2) lbl = LBL_NONE;
        else if (r.solar_avg && kq * 100 >= (long)lc.e * 85 && rm * 100 <= rc * steady) lbl = LBL_CLEAR;
        else if (cls != 1) lbl = LBL_NONE;
        else if (kq * 100 <= (long)lc.e * 30) lbl = LBL_OVERCAST;
        else lbl = LBL_MIXED;
      }
      if (!(mask & LM_L1)) {
        // L1 not fed (its day stamp included)
      } else if (c.gbits & LB_DOME) LGATE(s, LG_DOME);
      else if (!r.solar_avg || rm * 100 > rc * steady) LGATE(s, LG_STEADY);
      else if (kq < 500 || kq > 16000) LGATE(s, LG_K_RANGE);
      else if (kq * 2 < lc.e || (!sunlit && kq <= lc.e)) LGATE(s, LG_K_LOW);
      else if (!capTake(&lc.day, today)) LGATE(s, LG_CAP);
      else {
        const uint16_t n0 = lc.n;
        const int wm = warm(c, today, n0, 2 * L1_NMIN);  // the warm-up until medium
        if (frugal(&lc.e, (int)kq, L1_UP * wm, L1_DN * wm, L1_LO, L1_HI)) s.l1_hit |= (uint16_t)(1u << ci);
        else s.l1_hit &= (uint16_t)~(1u << ci);
        if (sunlit && kq * 100 >= (long)lc.e * 85) lc.n = neff(lc.n, L1_LAM, 64);  // only clear-sun evidence counts
        if (lc.n >= 2 * L1_NMIN) {  // the empty neighbours one band up / down, same half, take this E
          const int nb[2] = {ci - 2, ci + 2};
          for (const int j : nb) {
            if (j < 0 || j >= L1_CELLS || c.l1[j].n) continue;
            // only from the empty cell's better-known neighbour (the JSON's `from` rule)
            const int other = 2 * j - ci;
            if (other >= 0 && other < L1_CELLS && c.l1[other].n > lc.n) continue;
            const int d = c.l1[j].e - lc.e;
            if (n0 < 2 * L1_NMIN || d >= L1_SEED_MOVE || d <= -L1_SEED_MOVE) c.l1[j].e = lc.e;
          }
        }
      }
    }
    // L2 a: wet hours (>= 2 tips in this and the previous hour), P50 of rh_avg
    if ((mask & LM_L2) && hasU16(r.rain_sum) && r.rain_sum >= 40 && c.prev_rain >= 40 && hasU8(r.rh_avg)) {
      if (r.quality & Q_WET) LGATE(s, LG_QUALITY);
      else if (c.gbits & LB_STUCK) LGATE(s, LG_STUCK);
      else if (!hasI16(r.t_avg) || r.t_avg <= (int)std::lround(fcst::BLIND_T * 10) || c.icy_h < 24) LGATE(s, LG_MELT);  // melt tips
      else if (!capTake(&c.l2_wet_day, today)) LGATE(s, LG_CAP);
      else {
        bool h = false;
        if (!c.l2_wet_n) {  // seeded from the first wet hour, then x4 until its use level
          const int x = r.rh_avg * 100;
          c.l2_wet_e = (uint16_t)(x < L2_LO ? L2_LO : x > L2_HI ? L2_HI : x);
        } else {
          const int wm = warm(c, today, c.l2_wet_n, L2A_NMIN);
          h = frugal(&c.l2_wet_e, r.rh_avg * 100, L2_P50 * wm, L2_P50 * wm, L2_LO, L2_HI);
        }
        s.hit = (uint8_t)(h ? s.hit | LH_L2W : s.hit & ~LH_L2W);
        c.l2_wet_n = neff(c.l2_wet_n, L2A_LAM, 64);
      }
    }
    // L4: hourly mean wind, P50 + P90 (the warm-up until its first bar)
    if ((mask & LM_L4) && hasU16(r.wind_avg) && hasU16(r.gust_max)) {
      if (icedHour) LGATE(s, LG_ICED);
      else {
        const int wm = warm(c, today, c.l4_n, L4_NMIN);
        const bool h50 = frugal(&c.l4_p50, r.wind_avg * 100, L4_50 * wm, L4_50 * wm, L4_50_LO, L4_50_HI);
        const bool h90 = frugal(&c.l4_p90, r.wind_avg * 100, L4_90_UP * wm, L4_90_DN * wm, L4_90_LO, L4_90_HI);
        s.hit = (uint8_t)(h50 || h90 ? s.hit | LH_L4 : s.hit & ~LH_L4);
        c.l4_n = neff(c.l4_n, L4_LAM, 16), c.l4_day = today;
      }
    }
    // L3 (live): the readings 0 < wind < 10 km/h, back to raw, P5
    if ((mask & LM_L3) && ctx.live && ctx.wind_n) {
      if (icedHour) LGATE(s, LG_ICED);
      else
        for (int i = 0; i < ctx.wind_n && i < LEARN_WIND_MAX; i++) {
          const uint32_t raw = ctx.wind_f ? (uint32_t)ctx.wind[i] * 1000u / ctx.wind_f : ctx.wind[i];  // 0.1 km/h
          if (raw == 0 || raw >= 100) continue;
          const int wm = warm(c, today, c.l3_n, L3_NMIN);
          const bool h = frugal(&c.l3_e, (int)(raw * 1000), L3_UP * wm, L3_DN * wm, L3_LO, L3_HI);
          s.hit = (uint8_t)(h ? s.hit | LH_L3 : s.hit & ~LH_L3);
          c.l3_n = neff(c.l3_n, L3_LAM, 64), c.l3_day = today;
        }
    }
  }
  if (lblBand) c.lbl = lbl, c.lbl_day = today;
  // L1b (live): the day's visits; committed or dropped at sunset
  if ((mask & LM_L1B) && hs.ok && ctx.live) {
    if (hs.el < 0 || (s.sc_n && s.sc_day != today)) ev |= l1bSunset(s);
    if (hs.el >= 0 && ok && !geoHour && c.geo_days < 3) {
      s.sc_day = today;
      l1bHour(s, hs, ctx);
    }
  }
  c.prev_rain = (uint8_t)(hasU16(r.rain_sum) ? (r.rain_sum > 255 ? 255 : r.rain_sum) : 0);
  c.last_hour = r.hour;
  return ev;
}

uint8_t learnDay(LearnState& s, const DayRec& d, const LearnCtx& ctx, uint16_t mask) {
  LearnCore& c = s.c;
  if (d.ymd <= c.last_day) return 0;
  uint8_t ev = context(s, ctx, mask);
  s.days_fed++;
  s.core_dirty = true;
  c.last_day = d.ymd;
  // only hours change the epoch (the backfill feeds every hour before the days); a day of another epoch
  // is not learned from, so older days never pull the epoch back and re-learning starts once
  if (d.epoch != c.epoch) {
    LGATE(s, LG_QUALITY);
    return ev;
  }
  if (!(mask & LM_L7)) return ev;  // L7 not fed
  if (d.quality & (1u << Q_BARO_UNCAL)) {  // a REL reset: the pressure starts over
    c.l7_e = 0, c.l7_n = 0, c.l7_flag = 0;
    LGATE(s, LG_BARO);
    return ev;
  }
  if (d.quality & Q_BARO) {
    LGATE(s, LG_BARO);
    return ev;
  }
  if (d.hours < 20 || !hasI16(d.p_avg) || d.p_avg < 8700 || d.p_avg > 10850) {
    LGATE(s, d.hours < 20 ? LG_READINGS : LG_PHYSICS);
    return ev;
  }
  // the weighted running mean (alpha = 1 / W, W = n_eff after this day), floored at 1 - lambda
  const int64_t x = (int64_t)d.p_avg << 16;
  const uint16_t n1 = neff(c.l7_n, L7_LAM, 64);
  uint32_t a = 65536u * 64u / n1;
  if (a < L7_AFLOOR) a = L7_AFLOOR;
  if (!c.l7_n) c.l7_e = (int32_t)x;
  else c.l7_e = (int32_t)(c.l7_e + (((x - c.l7_e) * (int64_t)a + 32768) >> 16));
  c.l7_n = n1;
  // against the built-in normal, 15 days back (the mean's centre), with the bridge's offset in force
  int16_t norm;
  if (!learnMslpNormal(ctx.lat, ctx.lon, learnDoy(d.ymd) - 15, &norm)) {
    c.l7_flag = 0;
    return ev;
  }
  const int off = (int)((c.l7_e + 32768) >> 16) + (int)std::lround(ctx.p_off / 10.0) - norm, ao = off < 0 ? -off : off;
  const int enter = c.l7_n >= L7_N2 ? L7_ENTER2 : c.l7_n >= L7_N1 ? L7_ENTER1 : INT16_MAX;
  if (c.l7_flag ? ao >= L7_EXIT : ao >= enter) c.l7_flag = off < 0 ? 1 : 2;
  else c.l7_flag = 0;
  return ev;
}

// ---------------------------------------------------------------- the sea-level pressure normal
#include "mslp_norm.h"  // MSLP_NORM_PRESENT; MSLP_NORM[2 * MSLP_ROWS * MSLP_COLS] when 1 (tools/mslp_normals.py)

int learnDoy(uint32_t ymd) {
  static const uint16_t CUM[12] = {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};
  const uint32_t y = ymd / 10000, mo = ymd / 100 % 100, d = ymd % 100;
  if (mo < 1 || mo > 12) return 1;
  const bool leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
  return (int)(CUM[mo - 1] + d + (leap && mo > 2 ? 1 : 0));
}

#if MSLP_NORM_PRESENT
static const int8_t* gMslp = MSLP_NORM;
#else
static const int8_t* gMslp = nullptr;
#endif
void learnSetMslpTable(const int8_t* t) {
#if MSLP_NORM_PRESENT
  gMslp = t ? t : MSLP_NORM;
#else
  gMslp = t;
#endif
}
// the 5-deg high-ground bitmap (72 x 28 bits, lat -60 .. 80, lon 0 .. 355 E; bit row * 72 + col,
// LSB first): set where a 2.5-deg surface point of the box is over 1 000 m. A town on a high plateau inside a diluted
// 15 x 20 deg cell (Bogota, Addis Ababa, Nairobi) sets its console to the local QNH, which differs from the reduced
// sea-level pressure by more than stage 2's 12 hPa: L7 stays silent there. Older headers have no bitmap: none.
#ifndef MSLP_HIGH_PRESENT
#define MSLP_HIGH_PRESENT 0
#endif
#if MSLP_HIGH_PRESENT
static const uint8_t* gHigh = MSLP_HIGH;
#else
static const uint8_t* gHigh = nullptr;
#endif
void learnSetMslpHigh(const uint8_t* b) {
#if MSLP_HIGH_PRESENT
  gHigh = b ? b : MSLP_HIGH;
#else
  gHigh = b;
#endif
}
bool learnMslpHighAt(float lat, float lon, const uint8_t* bits) {
  if (!bits) bits = gHigh;
  if (!bits || std::isnan(lat) || std::isnan(lon)) return false;
  int r = (int)std::floor((lat + 60.0) / 5.0), c = (int)std::floor((lon < 0 ? lon + 360.0 : lon) / 5.0);
  if (r < 0 || r >= MSLP_HIGH_ROWS) return false;
  c = ((c % MSLP_HIGH_COLS) + MSLP_HIGH_COLS) % MSLP_HIGH_COLS;
  const int i = r * MSLP_HIGH_COLS + c;
  return bits[i / 8] >> (i % 8) & 1;
}

bool learnMslpBuiltIn() { return MSLP_NORM_PRESENT != 0; }
bool learnMslpHighBuiltIn() { return MSLP_HIGH_PRESENT != 0; }

bool learnMslpNormal(float lat, float lon, int doy, int16_t* out, const int8_t* tab) {
  const bool builtIn = !tab;
  if (!tab) tab = gMslp;
  if (!tab || std::isnan(lat) || std::isnan(lon) || lat > 70 || lat < -70) return false;
  if (builtIn && learnMslpHighAt(lat, lon)) return false;  // the station's own 5-deg box is high ground
  const double fy = (lat + 60.0) / 15.0, x = lon < 0 ? lon + 360.0 : lon, fx = x / 20.0;
  int r0 = (int)std::floor(fy), c0 = (int)std::floor(fx);
  double ty = fy - r0, tx = fx - c0;
  if (r0 < 0) r0 = 0, ty = 0;
  if (r0 >= MSLP_ROWS - 1) r0 = MSLP_ROWS - 2, ty = 1;
  c0 %= MSLP_COLS;
  const int c1 = (c0 + 1) % MSLP_COLS;
  double v[2];
  for (int s = 0; s < 2; s++) {
    const int8_t* t = tab + s * MSLP_ROWS * MSLP_COLS;
    // -128 = no normal (mean orography over 1 000 m: sea-level pressure there is an extrapolation and
    // a console set to the altimeter setting differs from it). A masked corner drops out of
    // the bilinear weights and the rest are renormalised when they weigh at least half; else none
    const int idx[4] = {r0 * MSLP_COLS + c0, r0 * MSLP_COLS + c1, (r0 + 1) * MSLP_COLS + c0, (r0 + 1) * MSLP_COLS + c1};
    if (t[idx[0]] != MSLP_NONE && t[idx[1]] != MSLP_NONE && t[idx[2]] != MSLP_NONE && t[idx[3]] != MSLP_NONE) {
      const double a = t[idx[0]] * (1 - tx) + t[idx[1]] * tx;
      const double b = t[idx[2]] * (1 - tx) + t[idx[3]] * tx;
      v[s] = a * (1 - ty) + b * ty;
      continue;
    }
    const double wk[4] = {(1 - tx) * (1 - ty), tx * (1 - ty), (1 - tx) * ty, tx * ty};
    double sw = 0, sv = 0;
    for (int k = 0; k < 4; k++)
      if (t[idx[k]] != MSLP_NONE) sw += wk[k], sv += wk[k] * t[idx[k]];
    if (sw < 0.5) return false;
    v[s] = sv / sw;
  }
  // the winter season's weight: 1 on 15 January, 0 on 16 July (a cosine over the year)
  const double w = 0.5 + 0.5 * std::cos(2 * 3.14159265358979323846 * (doy - 15) / 365.25);
  *out = (int16_t)std::lround((1013.0 + w * v[0] + (1 - w) * v[1]) * 10.0);
  return true;
}

// ---------------------------------------------------------------- blobs
void learnSealCore(LearnCore* c) {
  c->magic[0] = 'L', c->magic[1] = 'R', c->version = LEARN_VERSION;
  c->crc = 0;
  c->crc = crc32(c, sizeof *c);
}

BlobCheck learnCheckCore(const LearnCore& c, size_t len) {
  if (len > sizeof c) return BlobCheck::Newer;  // a later image's larger blob (not read)
  if (len != sizeof c || c.magic[0] != 'L' || c.magic[1] != 'R') return BlobCheck::Bad;
  LearnCore t = c;
  t.crc = 0;
  if (crc32(&t, sizeof t) != c.crc) return BlobCheck::Bad;
  if (c.version < LEARN_VERSION) return BlobCheck::Older;
  if (c.version > LEARN_VERSION) return BlobCheck::Newer;
  return BlobCheck::Ok;
}

void learnPackMask(const LearnState& s, LearnMaskBlob* b) {
  b->magic[0] = 'L', b->magic[1] = 'M', b->version = MASK_VERSION, b->spare = 0;
  b->loc_sig = s.c.loc_sig;
  memcpy(b->c, s.mask, sizeof b->c);
  b->crc = 0;
  b->crc = crc32(b, LEARN_MASK_BYTES);
}

BlobCheck learnUnpackMask(LearnState* s, const LearnMaskBlob& b, size_t len, uint32_t loc_sig) {
  if (len > LEARN_MASK_BYTES) return BlobCheck::Newer;  // a later image's larger blob
  if (len != LEARN_MASK_BYTES || b.magic[0] != 'L' || b.magic[1] != 'M') return BlobCheck::Bad;
  LearnMaskBlob t = b;
  t.crc = 0;
  if (crc32(&t, LEARN_MASK_BYTES) != b.crc) return BlobCheck::Bad;
  if (b.version < MASK_VERSION) return BlobCheck::Older;
  if (b.version > MASK_VERSION) return BlobCheck::Newer;
  if (b.loc_sig != loc_sig) return BlobCheck::Bad;  // learned at another place
  memcpy(s->mask, b.c, sizeof s->mask);
  // a fresh core (missing, bad, older) adopts the mask's place, so its first record keeps the mask
  if (s->c.flags & LF_FRESH) s->c.loc_sig = loc_sig;
  return BlobCheck::Ok;
}

// a missing or unusable mask blob is stored (empty) with the next write, so both blobs exist from the first one;
// judged on the mask alone, never when either blob is a newer image's (read-only)
void learnLoaded(LearnState* s, BlobCheck core, BlobCheck mask) {
  if (core != BlobCheck::Newer && mask != BlobCheck::Ok && mask != BlobCheck::Newer) s->mask_dirty = true;
}

bool learnClampResume(LearnState& s, uint32_t now_hour, uint32_t today_ymd, uint32_t newest_hour, uint32_t newest_day) {
  if (newest_hour > now_hour) now_hour = newest_hour;
  if (newest_day > today_ymd) today_ymd = newest_day;
  bool ch = false;
  if (s.c.last_hour > now_hour) s.c.last_hour = now_hour, ch = true;
  if (s.c.last_day > today_ymd) s.c.last_day = today_ymd, ch = true;
  if (ch) s.core_dirty = true;
  return ch;
}

// ---------------------------------------------------------------- graduation (what the outlook may read)
// L6's confidence: ready = 15 nights and SD <= 2.5 °C; high needs SD <= 2.0 °C (the JSON and the use agree)
static uint8_t l6Conf(const LearnCore& c, uint16_t today) {
  const bool ready = c.l6_w >= 15u * 65536u && (int64_t)c.l6_e2 * 256 <= 625ll * c.l6_w;
  uint8_t l = ready ? learnConfOf((uint16_t)(c.l6_w >> 10), 15 * 64, 43 * 64, c.l6_day, today) : (uint8_t)LC_DEFAULT;
  if (l == LC_HIGH && (int64_t)c.l6_e2 * 256 > 400ll * c.l6_w) l = LC_MEDIUM;
  return l;
}
// L2's S (0.01 %RH) and where it comes from: the night P90 (>= 92 %) at its first bar, else the wet P50 at its first
// bar, else none (0); never while RH is stuck at its cap
static int l2Sat(const LearnCore& c, uint16_t today, const char** src) {
  const uint8_t wc = learnConfOf(c.l2_wet_n, L2A_NMIN, L2A_NHI, c.l2_wet_day & 0x3FFF, today);
  const uint8_t nc = learnConfOf(c.l2_night_n, L2B_NMIN, L2B_NHI, c.l2_night_day & 0x3FFF, today);
  if (nc >= LEARN_USE_AT[LBR_L2B] && c.l2_night_e >= 9200) return *src = "night", c.l2_night_e;
  if (wc >= LEARN_USE_AT[LBR_L2A]) return *src = "wet", c.l2_wet_e;
  return *src = "prior", 0;
}
static uint8_t l7Stage(const LearnCore& c) { return c.l7_n >= L7_N2 ? 2 : c.l7_n >= L7_N1 ? 1 : 0; }
// the admitted days behind L7's n_eff (the smallest k with neff^k(0) >= n); 61 = over 60
uint16_t learnL7Days(uint16_t n) {
  uint32_t v = 0;
  for (uint16_t k = 0; k <= 60; k++, v = neff(v, L7_LAM, 64))
    if (v >= n) return k;
  return 61;
}

void learnUseOf(const LearnState& s, float lat, float lon, int16_t cal_t, int16_t cal_rh, LearnUse* u) {
  memset(u, 0, sizeof *u);
  u->sky_cls = 255;
  const LearnCore& c = s.c;
  u->hour = c.last_hour;
  if (!c.last_hour || (c.flags & LF_FRESH)) return;
  const uint16_t today = day14(learnDayOf(c.last_hour + 1800, lon));
  // L1 at its cell's medium: E x the clear sky of the newest hour, the L1b class there; no sky words under 10 deg, in
  // the dome / geo / indoor gates
  HourSun hs;
  hourSun(c.last_hour, lat, lon, &hs);
  if (hs.ok && hs.el >= 10 && hs.cs_avg > 0 && !(c.gbits & LB_DOME) && c.geo_days < 3 && c.indoor < 2) {
    const LearnCell& lc = c.l1[l1Band(hs.el) * 2 + (hs.pm ? 1 : 0)];
    if (learnConfOf(lc.n, L1_NMIN, L1_NHI, lc.day & 0x3FFF, today) >= LEARN_USE_AT[LBR_L1]) {
      u->bits |= LU_L1;
      u->sky_ref = (uint16_t)std::lround(lc.e * hs.cs_avg / 1e4);
      u->sky_ref_max = (uint16_t)std::lround(lc.e * hs.cs_max / 1e4);
      u->sky_cls = hs.mid_cell >= 0 ? l1bClass(s.mask[hs.mid_cell]) : 0;
    }
  }
  // L2 at its first bar, calibrated (0.1 %)
  const char* src;
  const int sat = (c.gbits & LB_STUCK) ? 0 : l2Sat(c, today, &src);
  if (sat) {
    const int v = (sat + 5) / 10 + cal_rh;
    u->bits |= LU_L2, u->rh_sat = (uint16_t)(v < 850 ? 850 : v > 1000 ? 1000 : v);
  }
  // L5 at medium in both classes and separable
  L5Src l5s;
  bool hit;
  const int thr = l5Threshold(c, &l5s, &hit);
  if (l5s == L5Src::Learned && learnConfOf(c.l5_cn, L5_NMIN, L5_NHI, c.l5_cd, today) >= LEARN_USE_AT[LBR_L5] &&
      learnConfOf(c.l5_on, L5_NMIN, L5_NHI, c.l5_od, today) >= LEARN_USE_AT[LBR_L5])
    u->bits |= LU_L5, u->cool = (int16_t)thr;
  // L6 at high: tonight's minimum (the open night, from sunset + 2 h) +- SD, rounded outward to whole °C
  if (c.n_start && hasI16(c.n_t2) && hasI16(c.n_td2) && l6Conf(c, today) >= LEARN_USE_AT[LBR_L6]) {
    const int p = l6Predict(c.n_t2, c.n_td2, c.n_L, c.l6_a, c.l6_b, c.l6_c) + (int)std::lround(cal_t / 10.0);
    const int sd = (int)std::lround(std::sqrt((double)c.l6_e2 * 256.0 / c.l6_w));
    const auto fl = [](int v) { return v >= 0 ? v / 10 * 10 : -((-v + 9) / 10 * 10); };  // floor to a whole °C
    u->bits |= LU_L6, u->min_lo = (int16_t)fl(p - sd), u->min_hi = (int16_t)-fl(-(p + sd));
    u->night_end = c.n_start + 3600u * c.n_len;
  }
  // L7 (the notice): the flag, the stage, the raw mean minus the normal
  u->l7_flag = c.l7_flag, u->l7_stage = l7Stage(c), u->l7_days = learnL7Days(c.l7_n);  // days, not n_eff
  int16_t norm;
  if (c.l7_n && c.last_day && learnMslpNormal(lat, lon, learnDoy(c.last_day) - 15, &norm))
    u->l7_off = (int16_t)(((c.l7_e + 32768) >> 16) - norm);
  else u->l7_flag = 0;
}

bool learnHourSky(uint32_t hour, float lat, float lon, int* l1cell, int* l1bcell, double* cs_avg, double* cs_max) {
  HourSun hs;
  hourSun(hour, lat, lon, &hs);
  *l1cell = hs.ok && hs.el >= 10 && hs.cs_avg > 0 ? l1Band(hs.el) * 2 + (hs.pm ? 1 : 0) : -1;
  *l1bcell = hs.ok ? hs.mid_cell : -1;
  *cs_avg = hs.ok ? hs.cs_avg : 0, *cs_max = hs.ok ? hs.cs_max : 0;
  return *l1cell >= 0;
}

// ---------------------------------------------------------------- JSON
static void b64(Out& o, const uint8_t* p, size_t n) {
  static const char T[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  char q[5] = {};
  for (size_t i = 0; i < n; i += 3) {
    const uint32_t v = (uint32_t)p[i] << 16 | (i + 1 < n ? (uint32_t)p[i + 1] << 8 : 0) | (i + 2 < n ? p[i + 2] : 0);
    q[0] = T[v >> 18 & 63], q[1] = T[v >> 12 & 63], q[2] = i + 1 < n ? T[v >> 6 & 63] : '=', q[3] = i + 2 < n ? T[v & 63] : '=';
    o.raw(q);
  }
}
static void t01(Out& o, const char* k, int16_t v, bool comma = true) {
  if (hasI16(v)) o.printf("%s\"%s\":%.1f", comma ? "," : "", k, v / 10.0);
  else o.printf("%s\"%s\":null", comma ? "," : "", k);
}
// the night's keys (with its length, calm-ish hours and rise); the caller closes the object
static void nightJson(Out& o, uint32_t start, uint8_t len, uint8_t rhmax, uint8_t hours, uint8_t okh, uint8_t flags,
                      uint8_t calm, int16_t t2, int16_t td2, int16_t t0, int16_t dt3, int16_t tmin, uint8_t l_h, uint8_t w5) {
  o.printf("{\"start\":%lu,\"len_h\":%u,\"hours\":%u,\"ok\":%u,\"calm\":%u,\"rain\":%s,\"hoar\":%s,\"restart\":%s,\"rh_max\":",
           (unsigned long)start, len, hours, okh, calm, flags & LN_RAIN ? "true" : "false", flags & LN_HOAR ? "true" : "false",
           flags & LN_RESTART ? "true" : "false");
  if (hasU8(rhmax)) o.printf("%u", rhmax);
  else o.raw("null");
  t01(o, "t2", t2), t01(o, "td2", td2), t01(o, "t0", t0), t01(o, "dt3", dt3), t01(o, "tmin", tmin);
  o.printf(",\"l_h\":%.1f,\"w5\":%u,\"rise\":%s", l_h / 10.0, w5, flags & LN_RISE ? "true" : "false");
}

void learnView(const LearnState& s, LearnView* v) {
  memset(v, 0, sizeof *v);
  v->c = s.c;
  for (int i = 0; i < L1B_CELLS; i++) {
    const uint8_t k = l1bClass(s.mask[i]);
    v->cls[k]++;
    v->packed[i / 4] |= (uint8_t)(k << (2 * (i % 4)));
  }
#if FC_DIAG_BENCH
  memcpy(v->gates, s.gates, sizeof v->gates);
#endif
  v->last_night = s.last_night;
  v->sc_n = s.sc_n, v->sc_clear = s.sc_clear;
  v->l1_hit = s.l1_hit, v->hit = s.hit;
}

// a learner's level by its count alone (the age rule aside), the samples to the next one, its sheet state
static int levelN(int id, uint32_t n) {
  const BarK& k = BARK[id];
  const uint32_t v = k.one ? n : n >> 10;
  return v < k.t[0] ? 0 : v < k.t[1] ? 1 : v < k.t[2] ? 2 : 3;
}
static const char* const STATE_NAME[4] = {"default", "learned", "in_use", "info"};
// ,"need":k,"state":"..." -- state: default below the first bar (age rule included), info for a learner the outlook
// never reads (L3, L4, L7), in_use when the caller says the outlook reads it, else learned
static void needState(Out& o, int id, uint32_t n, uint8_t conf, bool inUse) {
  const int lv = levelN(id, n);
  const int st = !conf ? 0 : LEARN_USE_AT[id] == LC_NEVER ? 3 : inUse ? 2 : 1;
  o.printf(",\"need\":%u,\"state\":\"%s\"", lv < 3 ? (unsigned)learnNeed(id, n, lv + 1) : 0u, STATE_NAME[st]);
}

void learnJson(Out& o, const LearnView& s, const LearnMeta& m) {
  const LearnCore& c = s.c;
  const uint16_t today = day14(learnDayOf(m.today_unix, m.lon));
  o.printf("{\"v\":1,\"state\":\"%s\",\"why\":", m.state);
  if (m.why) o.printf("\"%s\"", m.why);
  else o.raw("null");
  // one printf <= 191 chars (Out::printf truncates)
  o.printf(",\"epoch\":%u,\"last_hour\":%lu,\"last_day\":%lu,\"relearn\":%s", c.epoch, (unsigned long)c.last_hour,
           (unsigned long)c.last_day, mult(c, today) > 1 ? "true" : "false");
  // migrated_from before the backfill; in a bench build backfill.replay is always present (zeros and [] without a
  // migration). backfill.ms, replay{} and gates{} are bench keys -- absent from the release body
  o.printf(",\"migrated_from\":%u,\"backfill\":{\"hours\":%lu,\"days\":%lu,", (unsigned)m.migrated_from,
           (unsigned long)m.backfill_hours, (unsigned long)m.backfill_days);
#if FC_DIAG_BENCH
  o.printf("\"ms\":%lu,", (unsigned long)m.backfill_ms);
#endif
  o.printf("\"done\":%s", m.backfill_done ? "true" : "false");
#if FC_DIAG_BENCH
  o.printf(",\"replay\":{\"hours\":%lu,\"ms\":%lu,\"learners\":[", (unsigned long)m.replay_hours,
           (unsigned long)m.replay_ms);
  static const char* const LM_NAME[8] = {"l1", "l1b", "l2", "l3", "l4", "l5", "l6", "l7"};
  for (int b = 0, n = 0; b < 8; b++)
    if (m.replay_mask >> b & 1u) o.printf("%s\"%s\"", n++ ? "," : "", LM_NAME[b]);
  o.raw("]}");
#endif
  o.raw("}");
  o.printf(",\"written\":%lu,\"writes\":%lu,\"nvs\":{\"used\":%lu,\"free_entries\":%lu}", (unsigned long)m.written,
           (unsigned long)m.writes, (unsigned long)m.nvs_used, (unsigned long)m.nvs_free);
#if FC_DIAG_BENCH
  o.raw(",\"gates\":{");
  for (int g = 0; g < LG_COUNT; g++) o.printf("%s\"%s\":%u", g ? "," : "", GATE_NAME[g], s.gates[g]);
  o.raw("}");
#endif
  o.printf(",\"l1\":{\"def\":%.4f,\"lo\":%.2f,\"hi\":%.2f,\"cells\":[", L1_PRIOR / 1e4, L1_LO / 1e4, L1_HI / 1e4);
  static const uint8_t EL[6] = {5, 10, 15, 20, 30, 45};
  for (int i = 0; i < L1_CELLS; i++) {
    const LearnCell& x = c.l1[i];
    const uint8_t cf = learnConfOf(x.n, L1_NMIN, L1_NHI, x.day & 0x3FFF, today);
    o.printf("%s{\"el\":%u,\"pm\":%s,\"e\":%.4f,\"n\":%.1f,\"conf\":\"%s\",\"last\":%u,\"hit\":%s", i ? "," : "",
             EL[i / 2], i & 1 ? "true" : "false", x.e / 1e4, x.n / 64.0, CONF_NAME[cf],
             (unsigned)(x.n ? (x.day & 0x3FFF) + LDAY_BASE : 0), x.n && (s.l1_hit >> i & 1) ? "true" : "false");
    needState(o, LBR_L1, x.n, cf, cf >= LEARN_USE_AT[LBR_L1]);
    // an empty cell whose E is not the prior was seeded by a neighbour: the band of the one with the highest n
    int from = -1;
    const int nb[2] = {i - 2, i + 2};
    if (!x.n && x.e != L1_PRIOR)
      for (const int j : nb)
        if (j >= 0 && j < L1_CELLS && c.l1[j].n && (from < 0 || c.l1[j].n > c.l1[from].n)) from = j;
    if (from >= 0 && x.e != c.l1[from].e) from = -1;  // its own value (a restart kept it), not a seed
    if (from >= 0) o.printf(",\"from\":%u}", EL[from / 2]);
    else o.raw(",\"from\":null}");
  }
  o.printf("]},\"l1b\":{\"sun\":%u,\"shade\":%u,\"unknown\":%u,\"today\":%u,\"clear_today\":%s,\"mask\":\"", s.cls[1],
           s.cls[2], s.cls[0], s.sc_n, s.sc_clear ? "true" : "false");
  b64(o, s.packed, sizeof s.packed);
  const uint8_t wc = learnConfOf(c.l2_wet_n, L2A_NMIN, L2A_NHI, c.l2_wet_day & 0x3FFF, today);
  const uint8_t nc = learnConfOf(c.l2_night_n, L2B_NMIN, L2B_NHI, c.l2_night_day & 0x3FFF, today);
  // S = the night P90 (>= 92 %) at its first bar, else the wet P50 at its first bar, else 100 (prior)
  const char* src;
  const int sat = l2Sat(c, today, &src);
  const bool l2use = sat && !(c.gbits & LB_STUCK);
  o.printf("\"},\"l2\":{\"wet\":{\"e\":%.2f,\"n\":%.1f,\"conf\":\"%s\",\"hit\":%s", c.l2_wet_e / 100.0, c.l2_wet_n / 64.0,
           CONF_NAME[wc], c.l2_wet_n && (s.hit & LH_L2W) ? "true" : "false");
  needState(o, LBR_L2A, c.l2_wet_n, wc, l2use && *src == 'w');
  o.printf("},\"night\":{\"e\":%.2f,\"n\":%.1f,\"conf\":\"%s\"", c.l2_night_e / 100.0, c.l2_night_n / 64.0, CONF_NAME[nc]);
  needState(o, LBR_L2B, c.l2_night_n, nc, l2use && *src == 'n');
  o.printf("},\"s\":%.2f,\"src\":\"%s\",\"stuck\":%s,\"state\":\"%s\"}", sat ? sat / 100.0 : 100.0, src,
           c.gbits & LB_STUCK ? "true" : "false", STATE_NAME[l2use ? 2 : sat ? 1 : 0]);  // learned only with a measured S
  const uint8_t c3 = learnConfOf(c.l3_n, L3_NMIN, L3_NHI, c.l3_day, today);
  o.printf(",\"l3\":{\"e\":%.2f,\"n\":%.1f,\"conf\":\"%s\",\"hit\":%s", c.l3_e / 1e4, c.l3_n / 64.0, CONF_NAME[c3],
           c.l3_n && (s.hit & LH_L3) ? "true" : "false");
  needState(o, LBR_L3, c.l3_n, c3, false);
  const uint8_t c4 = learnConfOf(c.l4_n, L4_NMIN, L4_NHI, c.l4_day, today);
  o.printf("},\"l4\":{\"p50\":%.2f,\"p90\":%.2f,\"n\":%.1f,\"conf\":\"%s\",\"hit\":%s", c.l4_p50 / 1e3, c.l4_p90 / 1e3,
           c.l4_n / 16.0, CONF_NAME[c4], c.l4_n && (s.hit & LH_L4) ? "true" : "false");
  needState(o, LBR_L4, c.l4_n, c4, false);
  // L7 against the normal: stage (0 learning, 1 >= 14 days: 20 hPa, 2 >= 51 days: 12 hPa), flag none / low /
  // high, off = the raw mean minus the normal (hPa; null without a normal), the normal itself
  o.raw("},\"l7\":{\"e\":");
  if (c.l7_n) o.printf("%.1f", c.l7_e / 65536.0 / 10.0);
  else o.raw("null");
  static const char* const L7F[3] = {"none", "low", "high"};
  int16_t norm = 0;
  const bool hasNorm = c.last_day && learnMslpNormal(m.lat, m.lon, learnDoy(c.last_day) - 15, &norm);
  // `off` with the bridge's pressure offset in force and `flag` re-judged as the notice does (held
  // while >= 9 hPa, none under baro_uncalibrated)
  const long offv = hasNorm && c.l7_n ? ((c.l7_e + 32768) >> 16) - norm + m.p_off : 0;  // p_off in 0.1 hPa
  const int fl = m.baro_uncal || !hasNorm || !c.l7_n || (offv < 90 && offv > -90) ? 0 : c.l7_flag ? (offv < 0 ? 1 : 2) : 0;
  o.printf(",\"n\":%.1f,\"days\":%u,\"stage\":%u,\"flag\":\"%s\",\"uncal\":%s,\"off\":", c.l7_n / 64.0,
           learnL7Days(c.l7_n), l7Stage(c), L7F[fl], m.baro_uncal ? "true" : "false");
  if (hasNorm && c.l7_n) o.printf("%.1f", offv / 10.0);
  else o.raw("null");
  o.raw(",\"norm\":");
  if (hasNorm) o.printf("%.1f", norm / 10.0);
  else o.raw("null");
  needState(o, LBR_L7, c.l7_n, l7Stage(c), false);
  o.raw("}");
  // L5 / L6
  L5Src l5s;
  bool thit;
  const int thr = l5Threshold(c, &l5s, &thit);
  static const char* const SRC[3] = {"default", "learned", "inseparable"};
  const uint8_t c5c = learnConfOf(c.l5_cn, L5_NMIN, L5_NHI, c.l5_cd, today);
  const uint8_t c5o = learnConfOf(c.l5_on, L5_NMIN, L5_NHI, c.l5_od, today);
  const bool l5use = l5s == L5Src::Learned && c5c >= LEARN_USE_AT[LBR_L5] && c5o >= LEARN_USE_AT[LBR_L5];
  o.printf(",\"l5\":{\"clear\":{\"e\":%.2f,\"n\":%.1f,\"conf\":\"%s\"", c.l5_ce / 100.0, c.l5_cn / 64.0, CONF_NAME[c5c]);
  needState(o, LBR_L5, c.l5_cn, c5c, l5use);
  o.printf("},\"overcast\":{\"e\":%.2f,\"n\":%.1f,\"conf\":\"%s\"", c.l5_oe / 100.0, c.l5_on / 64.0, CONF_NAME[c5o]);
  needState(o, LBR_L5, c.l5_on, c5o, l5use);
  o.printf("},\"thr\":%.2f,\"src\":\"%s\",\"hit\":%s,\"state\":\"%s\"}", thr / 100.0, SRC[(int)l5s], thit ? "true" : "false",
           STATE_NAME[l5use ? 2 : c5c && c5o ? 1 : 0]);
  const bool ready = c.l6_w >= 15u * 65536u && (int64_t)c.l6_e2 * 256 <= 625ll * c.l6_w;
  const uint8_t l6c = l6Conf(c, today);  // high needs SD <= 2.0 °C
  o.printf(",\"l6\":{\"a\":%.2f,\"b\":%.3f,\"c\":%.3f,\"n\":%.1f,\"sd\":", c.l6_a / 100.0, c.l6_b / 1e4, c.l6_c / 1e4,
           c.l6_w / 65536.0);
  if (c.l6_w >= 3u * 65536u) o.printf("%.1f,\"mae\":%.1f", std::sqrt((double)c.l6_e2 * 256.0 / c.l6_w) / 10.0,
                                      (double)c.l6_ea * 256.0 / c.l6_w / 10.0);
  else o.raw("null,\"mae\":null");
  o.printf(",\"conf\":\"%s\",\"learned\":%s,\"hit\":%s", CONF_NAME[l6c], ready ? "true" : "false",
           c.l6_w && (s.hit & LH_L6) ? "true" : "false");
  needState(o, LBR_L6, c.l6_w, l6c, l6c >= LEARN_USE_AT[LBR_L6]);
  o.raw("}");
  o.printf(",\"relearn_hour\":%lu", (unsigned long)c.relearn_hour);  // 0 = never restarted
  // the one table of counts (real samples to one, two, three bars)
  static const char* const BAR_KEY[LBR_COUNT] = {"l1", "l2_wet", "l2_night", "l3", "l4", "l5", "l6", "l7"};
  o.raw(",\"bars\":{");
  for (int i = 0; i < LBR_COUNT; i++)
    o.printf("%s\"%s\":[%u,%u,%u]", i ? "," : "", BAR_KEY[i], LEARN_BARS[i].low, LEARN_BARS[i].med, LEARN_BARS[i].high);
  o.raw("}");
  // the station's snapshot of what the outlook may read (calibrated units)
  if (m.use) {
    const LearnUse& u = *m.use;
    o.printf(",\"use\":{\"hour\":%lu,\"l1\":%s,\"l2\":%s,\"l5\":%s,\"l6\":%s", (unsigned long)u.hour,
             u.bits & LU_L1 ? "true" : "false", u.bits & LU_L2 ? "true" : "false", u.bits & LU_L5 ? "true" : "false",
             u.bits & LU_L6 ? "true" : "false");
    o.printf(",\"sky_ref\":%u,\"sky_ref_max\":%u,\"sky_cls\":%u,\"rh_sat\":%.1f,\"cool\":%.2f", u.sky_ref, u.sky_ref_max,
             u.sky_cls, u.rh_sat / 10.0, u.cool / 100.0);
    o.printf(",\"min_lo\":%.0f,\"min_hi\":%.0f,\"night_end\":%lu,\"metar\":%s}", u.min_lo / 10.0, u.min_hi / 10.0,
             (unsigned long)u.night_end, u.metar ? "true" : "false");  // metar: METAR eligible, L7's notice is silent
  }
  o.printf(",\"gate_state\":{\"indoor\":%s,\"frozen_run\":%u,\"geo_days\":%u,\"dome\":%s,\"iced\":%s}",
           c.indoor >= 2 ? "true" : "false", c.frozen_run, c.geo_days, c.gbits & LB_DOME ? "true" : "false",
           c.gbits & LB_ICED ? "true" : "false");
  o.raw(",\"night\":{\"open\":");
  if (c.n_start) {
    nightJson(o, c.n_start, c.n_len, c.n_rhmax, c.n_hours, c.n_ok, c.n_flags, c.n_calm, c.n_t2, c.n_td2, c.n_t0, c.n_dt3,
              c.n_tmin, c.n_L, c.n_w5);
    t01(o, "e_dt3", c.e_dt3);
    o.printf(",\"e_ok\":%s}", c.e_flags & EB_BAD ? "false" : "true");
  } else {
    o.raw("null");
  }
  o.raw(",\"last\":");
  const LearnNight& n = s.last_night;
  if (n.start) {
    nightJson(o, n.start, n.len, n.rhmax, n.hours, n.ok, n.flags, n.calm, n.t2, n.td2, n.t0, n.dt3, n.tmin, n.l_h, n.w5);
    o.printf(",\"l6\":\"%s\"", L6R_NAME[n.l6 < 8 ? n.l6 : 0]);
    t01(o, "pred", n.pred);
    o.printf("},\"last_admitted\":%s", n.admitted ? "true" : "false");
  } else {
    o.raw("null");
  }
  o.raw("},\"evening\":{\"label\":");
  if (c.lbl && c.lbl < 6) o.printf("\"%s\",\"label_day\":%u}}", LBL_NAME[c.lbl], (unsigned)(c.lbl_day + LDAY_BASE));
  else o.raw("null,\"label_day\":null}}");
}

}  // namespace fc
