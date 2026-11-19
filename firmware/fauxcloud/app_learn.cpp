// app_learn.cpp -- the learners: one LearnState per station slot, fed
// from the flash rings at first boot (backfill) and after a reset (catch-up), then from the RAM rings at every hour /
// day close, persisted in NVS ("lr:" core, "lm:" mask) every 6 h, before a planned restart and after a reset event.
// Loop task only, outside gLock (the station copies take it briefly); /api/learn copies a view under gLearnLock.
// After a pass that fed anything, the learners' LearnUse snapshot (what the outlook may read, in
// calibrated units) is copied into the Station under gLock (O(1)); the outlook reads only what is at its use level.
// "Restart learning" (admin) is a request the loop applies in the live phase.
#include <esp_task_wdt.h>

#include <algorithm>
#include <cmath>

#include "app.h"
#include "src/calib.h"
#include "src/history.h"
#include "src/learn.h"

namespace app {

namespace {
// Migrate = the one-off replay pass of the learners new to the blob layout, before the catch-up
enum class Phase : uint8_t { Off, Migrate, FlashHours, FlashDays, Live };
struct Slot {
  char id[STATION_ID_LEN];
  uint32_t gen;
  Phase phase;
  bool read_only, no_store, backfilled;
  uint32_t bf_hours, bf_days, bf_t0;
  uint32_t bf_ms;                 // the backfill's wall time, set once at its end
  uint32_t closes, days;          // the station's counters seen
  uint32_t written, writes;       // the last blob write (unix), writes since boot
  uint32_t write_ms;              // millis() of the last write (the 6-h cadence)
  uint8_t ev;                     // reset events not yet written
  RingCursor cur;                 // the backfill's place in the flash ring
  // The migration of an older core blob: the version it came from, the learners replayed (0 = none / the
  // pass state could not be allocated), the pass's end (the blob's last_hour), its place, hours and wall time
  uint8_t mig_from;
  uint16_t mig_mask;
  uint32_t mig_end, mig_after, mig_hours, mig_t0, mig_ms;
  bool snap;                      // the LearnUse snapshot is due (load, a restart)
  volatile bool relearn_req;      // "Restart learning" asked (httpd), applied by the loop in the live phase
};
LearnState gL[MAX_STATIONS];
Slot gS[MAX_STATIONS];
SemaphoreHandle_t gLearnLock = nullptr;
constexpr uint32_t WRITE_EVERY_MS = 6 * 3600 * 1000u;
constexpr int BATCH = 16;                 // records per pass
constexpr uint32_t PASS_BUDGET_MS = 150;  // passes per tick while backfilling, the watchdog fed between them
// The pass state (~1.5 KB) comes from the heap only while a pass runs -- a deliberate, one-off
// exception to the "no heap allocation" rule, limited to a boot that migrates; one slot at a time owns it
LearnState* gPass = nullptr;
int gMigOwner = -1;
void passFree(int i) {
  if (gMigOwner != i) return;
  free(gPass);
  gPass = nullptr, gMigOwner = -1;
  storeRunRelease();
}

struct LLock {
  LLock() { xSemaphoreTake(gLearnLock, portMAX_DELAY); }
  ~LLock() { xSemaphoreGive(gLearnLock); }
};
void clampResume(int i, uint32_t nh, uint32_t nd);
void ringRefs(int i, const char* id, uint32_t* nh, uint32_t* nd);

uint32_t macHash(const Station& st) { return st.has_src_mac ? adoptMacHash(st.src_mac) : 0; }  // one rule (adopt.h)

// the station's context (caller holds gLock); live: the Recent ring's readings of `hour` into bins and wind values
void ctxOf(const Station& st, uint32_t hour, bool live, LearnCtx* c) {
  memset(c, 0, sizeof *c);
  c->lat = gBridge.lat, c->lon = gBridge.lon;
  c->profile = (uint8_t)st.cfg.profile;
  c->mac_hash = macHash(st);
  const CalEntry* ce = calFor(st.cal, hour);
  c->wind_f = ce && ce->wind_f ? ce->wind_f : 1000;
  c->p_off = ce ? ce->p : 0;  // L7: the bridge's pressure offset in force (0.01 hPa)
  if (!live) return;
  for (int i = 0; i < st.recent.size(); i++) {
    const PackedSample& p = st.recent.at(i);
    if (p.t < hour || p.t >= hour + 3600) continue;
    const int b = (int)((p.t - hour) / 300);
    if (hasValue(p.v[F_SOLAR_WM2])) {
      const long w = lroundf(unpack(F_SOLAR_WM2, p.v[F_SOLAR_WM2]));
      c->bin_sum[b] = (uint16_t)(c->bin_sum[b] + (w < 0 ? 0 : w > 2000 ? 2000 : w));
      c->bin_n[b]++;
    }
    if (hasValue(p.v[F_WIND_KMH]) && c->wind_n < LEARN_WIND_MAX) {
      const long w = lroundf(unpack(F_WIND_KMH, p.v[F_WIND_KMH]) * 10);
      c->wind[c->wind_n++] = (uint8_t)(w < 0 ? 0 : w > 255 ? 255 : w);
    }
  }
  c->live = true;
}

void writeBlobs(int i, const char* why) {
  Slot& s = gS[i];
  if (s.read_only || !s.id[0] || s.phase == Phase::Migrate) return;  // the V1 blob stays until the pass ends
  // the blobs are packed and written under the learner lock (no static copies: 1.2 KB on the loop stack instead)
  LearnCore core;
  LearnMaskBlob mask;
  char k[16];
  bool ok, maskDirty;
  {
    LLock l;
    core = gL[i].c;
    maskDirty = gL[i].mask_dirty;
    if (maskDirty) learnPackMask(gL[i], &mask);
    gL[i].core_dirty = gL[i].mask_dirty = false;
    learnSealCore(&core);
    nvsLearnKey(s.id, 'r', k);
    ok = nvsPutBlob(k, &core, sizeof core);
    if (maskDirty) {
      nvsLearnKey(s.id, 'm', k);
      ok = nvsPutBlob(k, &mask, LEARN_MASK_BYTES) && ok;
    }
    if (!ok) gL[i].core_dirty = true, gL[i].mask_dirty = gL[i].mask_dirty || maskDirty;  // retried next time
  }
  s.written = nowS(), s.writes++, s.write_ms = millis(), s.ev = 0;
  logf("LEARN", "%s: blobs written (%s%s)%s", s.id, why, maskDirty ? ", mask" : "", ok ? "" : " FAILED");
}

void load(int i, const char* id, uint32_t gen) {
  storeRunRelease();  // a walk of the slot's previous station ends here
  passFree(i);        // a pass of the slot's previous station ends here
  Slot& s = gS[i];
  memset(&s, 0, sizeof s);
  snprintf(s.id, sizeof s.id, "%s", id);
  s.gen = gen;
  LearnCore core;  // loop stack (1.2 KB, transient)
  LearnMaskBlob mask;
  char k[16];
  nvsLearnKey(id, 'r', k);
  const size_t lc = nvsGetBlob(k, &core, sizeof core);
  nvsLearnKey(id, 'm', k);
  const size_t lm = nvsGetBlob(k, &mask, LEARN_MASK_BYTES);
  xSemaphoreTake(gLock, portMAX_DELAY);
  const uint32_t sig = learnLocSig(gBridge.lat, gBridge.lon);
  xSemaphoreGive(gLock);
  uint32_t nh, nd;
  ringRefs(i, id, &nh, &nd);
  LLock l;
  learnInit(&gL[i]);
  const BlobCheck cc = lc ? learnCheckCore(core, lc) : BlobCheck::Bad;
  if (cc == BlobCheck::Ok) gL[i].c = core;
  // An older known blob is migrated field by field (not marked dirty: it stays in NVS until the pass ends,
  // so a restart mid-pass migrates again from it); an unknown older version starts from priors
  LearnCore migrated;
  if (cc == BlobCheck::Older && learnMigrateCore(&core, lc, &migrated, &s.mig_from, &s.mig_mask)) gL[i].c = migrated;
  s.read_only = cc == BlobCheck::Newer;
  // the mask on its own, whatever the core's state (it cannot be rebuilt from history)
  // with no location set (sig 0) the mask is read against its own place (kept, not judged a move)
  const BlobCheck mc = lm ? learnUnpackMask(&gL[i], mask, lm, sig ? sig : mask.loc_sig) : BlobCheck::Bad;
  s.read_only = s.read_only || mc == BlobCheck::Newer;
  learnLoaded(&gL[i], cc, mc);
  s.no_store = !storeMounted();
  s.phase = s.no_store ? Phase::Live : s.mig_mask ? Phase::Migrate : Phase::FlashHours;
  if (s.no_store) s.mig_mask = 0;
  s.bf_t0 = millis(), s.write_ms = millis();
  s.closes = UINT32_MAX;  // the first live pass always looks (RAM hours not on flash yet)
  s.snap = true;           // and writes the first LearnUse snapshot
  s.cur = RingCursor{-1, 0, 0};
  static const char* const CHK[4] = {"ok", "missing or bad", "older", "newer"};
  logl(Log::Boot, "LEARN", "%s: core %s, mask %s; last hour %lu%s", id, lc ? CHK[(int)cc] : "none", lm ? CHK[(int)mc] : "empty",
       (unsigned long)gL[i].c.last_hour, s.read_only ? " (read-only: a newer image wrote it)" : s.no_store ? " (no store: RAM rings only)" : "");
  if (s.mig_from) logf("LEARN", "%s: core migrated v%u -> v%u", id, (unsigned)s.mig_from, (unsigned)LEARN_VERSION);
  clampResume(i, nh, nd);
}

// The newest hour / day the station's rings hold (RAM and flash), taken outside the learner lock
void ringRefs(int i, const char* id, uint32_t* nh, uint32_t* nd) {
  storeNewestKeys(id, nh, nd);
  xSemaphoreTake(gLock, portMAX_DELAY);
  if (const Station* st = gStations.at(i)) {
    if (const int n = st->hourly.size()) *nh = std::max(*nh, st->hourly.at(n - 1).hour);
    if (const int n = st->daily.size()) *nd = std::max(*nd, st->daily.at(n - 1).ymd);
  }
  xSemaphoreGive(gLock);
}

// Caller holds the learner lock: resume points ahead of the clock (a clock that once ran forward) back to
// now -- never below the newest key the rings hold
void clampResume(int i, uint32_t nh, uint32_t nd) {
  const uint32_t now = nowS(), lh = gL[i].c.last_hour, ld = gL[i].c.last_day;
  if (learnClampResume(gL[i], now - now % 3600, dayNumberToYmd(localDayNumber(gTz, now)), nh, nd))
    logf("LEARN", "%s: resume point in the future (hour %lu, day %lu): reset to %lu / %lu", gS[i].id, (unsigned long)lh,
         (unsigned long)ld, (unsigned long)gL[i].c.last_hour, (unsigned long)gL[i].c.last_day);
}

void logEvents(const char* id, uint8_t ev) {
  logf("LEARN", "%s: reset%s%s%s%s%s", id, ev & LE_LOC ? " location (L1, mask)" : "", ev & LE_PROFILE ? " profile (all)" : "",
       ev & LE_MAC ? " console (L7)" : "", ev & LE_RELEARN ? " re-learning (14 d)" : "", ev & LE_L7_STEP ? " pressure (REL step)" : "");
}

void feed(const Slot& s, int i, const HourRec* h, const DayRec* d, const LearnCtx& c) {
  uint8_t ev;
  {
    LLock l;
    ev = h ? learnHour(gL[i], *h, c) : learnDay(gL[i], *d, c);
  }
  const uint8_t resets = ev & (LE_LOC | LE_PROFILE | LE_MAC | LE_RELEARN | LE_L7_STEP);
  if (resets) {
    logEvents(s.id, resets);
    gS[i].ev |= resets;
  }
  if (ev & LE_L1B_COMMIT) gS[i].ev |= LE_L1B_COMMIT;  // a clear day's sky cells are written that evening
}

// The LearnUse snapshot (calibrated with the entry in force at the newest hour consumed) into the Station
void snapshot(int i, uint32_t gen) {
  float lat, lon;
  int16_t ct = 0, crh = 0;
  uint32_t last;
  {
    LLock l;
    last = gL[i].c.last_hour;
  }
  xSemaphoreTake(gLock, portMAX_DELAY);
  lat = gBridge.lat, lon = gBridge.lon;
  if (const Station* st = gStations.at(i))
    if (const CalEntry* ce = calFor(st->cal, last)) ct = ce->temp, crh = ce->rh;
  xSemaphoreGive(gLock);
  LearnUse u;
  {
    LLock l;
    learnUseOf(gL[i], lat, lon, ct, crh, &u);
  }
  xSemaphoreTake(gLock, portMAX_DELAY);
  u.metar = metarEligibleLocked();  // METAR eligible, not merely on; read in the hold that writes it (a toggle cannot slip between)
  Station* st = gStations.at(i);
  if (st && st->gen == gen) st->use = u;
  xSemaphoreGive(gLock);
}

// True when the pass budget is spent (the caller skips the remaining slots this tick)
bool slotTick(int i, uint32_t t0) {
  Slot& s = gS[i];
  char id[STATION_ID_LEN];
  uint32_t gen, closes, days;
  xSemaphoreTake(gLock, portMAX_DELAY);
  const Station* st = gStations.at(i);
  if (st) memcpy(id, st->id, sizeof id), gen = st->gen, closes = st->hour_closes, days = st->daily.size();
  xSemaphoreGive(gLock);
  if (!st) {
    if (s.id[0]) {
      passFree(i);
      LLock l;
      s.id[0] = 0, s.phase = Phase::Off;
    }
    return false;
  }
  if (!s.id[0] || strcmp(s.id, id) != 0 || s.gen != gen) load(i, id, gen);
  static HourRec h;
  static DayRec d;
  LearnCtx c;
  if (s.phase == Phase::Migrate) {  // the replay pass of the new learners over the ring up to mig_end
    if (gMigOwner >= 0 && gMigOwner != i) return false;  // the other slot's pass runs first
    if (!gPass) {
      gPass = (LearnState*)malloc(sizeof(LearnState));
      if (!gPass) {
        logf("LEARN", "%s: no memory for the replay pass (%u B): the migrated values are kept", id, (unsigned)sizeof(LearnState));
        LLock l;
        // the V1 evening / night scratch as a non-carried merge leaves it (judged `evening`)
        learnPassMerge(gL[i].c, gL[i].c, 0, UINT32_MAX);
        s.mig_mask = 0, s.phase = Phase::FlashHours, s.cur = RingCursor{-1, 0, 0};
        return false;
      }
      gMigOwner = i;
      {
        LLock l;
        learnPassStart(gPass, gL[i].c, s.mig_mask, gL[i].mask);  // the replayed learners from priors, the live mask
        s.mig_end = gL[i].c.last_hour;
      }
      s.mig_after = 0, s.mig_hours = 0, s.cur = RingCursor{-1, 0, 0}, s.mig_t0 = millis();
    }
    for (;;) {
      const bool got = storeNextHour(id, s.mig_after, &s.cur, &h);
      if (!got || h.hour > s.mig_end) break;
      s.mig_after = h.hour;
      xSemaphoreTake(gLock, portMAX_DELAY);
      st = gStations.at(i);
      const bool same = st && st->gen == gen;
      if (same) ctxOf(*st, h.hour, false, &c);
      xSemaphoreGive(gLock);
      if (!same) {
        passFree(i);
        return millis() - t0 > PASS_BUDGET_MS;
      }
      learnHour(*gPass, h, c, s.mig_mask);  // the pass state is private: no lock, events ignored
      s.mig_hours++;
      if (millis() - t0 > PASS_BUDGET_MS) {
        if (esp_task_wdt_status(nullptr) == ESP_OK) esp_task_wdt_reset();
        return true;
      }
      if (s.mig_hours % BATCH == 0) {  // yield like the FlashHours loop
        if (esp_task_wdt_status(nullptr) == ESP_OK) esp_task_wdt_reset();
        vTaskDelay(1);
      }
    }
    {
      LLock l;
      learnPassMerge(gL[i].c, gPass->c, s.mig_mask, s.mig_end);
      gL[i].core_dirty = true;
      s.phase = Phase::FlashHours, s.cur = RingCursor{-1, 0, 0}, s.bf_t0 = millis();
      s.mig_ms = millis() - s.mig_t0;
    }
    passFree(i);
    logf("LEARN", "%s: replay of %s%s%s%s%s: %lu hour(s) in %lu ms", id, s.mig_mask & LM_L1 ? "l1," : "",
         s.mig_mask & LM_L2 ? "l2," : "", s.mig_mask & LM_L4 ? "l4," : "", s.mig_mask & LM_L5 ? "l5," : "",
         s.mig_mask & LM_L6 ? "l6" : "", (unsigned long)s.mig_hours, (unsigned long)s.mig_ms);
    writeBlobs(i, "migration");
    if (millis() - t0 > PASS_BUDGET_MS) return true;
  }
  // backfill / catch-up from the flash rings, BATCH records per pass, passes while the budget lasts
  while (s.phase == Phase::FlashHours || s.phase == Phase::FlashDays) {
    for (int n = 0; n < BATCH; n++) {
      const bool hours = s.phase == Phase::FlashHours;
      uint32_t after;
      {
        LLock l;
        after = hours ? gL[i].c.last_hour : gL[i].c.last_day;
      }
      if (hours ? !storeNextHour(id, after, &s.cur, &h) : !storeNextDay(id, after, &s.cur, &d)) {
        s.phase = hours ? Phase::FlashDays : Phase::Live;
        s.cur = RingCursor{-1, 0, 0};  // the day ring starts its own walk
        if (!hours) {
          s.backfilled = true;
          s.bf_ms = millis() - s.bf_t0;
          bool dirty;
#if FC_DIAG_BENCH  // the gate figure is bench only
          uint32_t rej = 0;
#endif
          {
            LLock l;
#if FC_DIAG_BENCH
            for (uint16_t g : gL[i].gates) rej += g;
#endif
            dirty = gL[i].core_dirty || gL[i].mask_dirty;
          }
#if FC_DIAG_BENCH
          logf("LEARN", "%s: backfill done: %lu hour(s), %lu day(s) in %lu ms, %lu gate rejection(s)", id,
               (unsigned long)s.bf_hours, (unsigned long)s.bf_days, (unsigned long)s.bf_ms, (unsigned long)rej);
#else
          logf("LEARN", "%s: backfill done: %lu hour(s), %lu day(s) in %lu ms", id, (unsigned long)s.bf_hours,
               (unsigned long)s.bf_days, (unsigned long)s.bf_ms);
#endif
          if (dirty) writeBlobs(i, "backfill");  // nothing fed, nothing written (a crash loop stays quiet)
        }
        break;
      }
      xSemaphoreTake(gLock, portMAX_DELAY);
      st = gStations.at(i);
      const bool same = st && st->gen == gen;
      if (same) ctxOf(*st, hours ? h.hour : calDayTime(d.ymd), false, &c);
      xSemaphoreGive(gLock);
      if (!same) {
        storeRunRelease();  // the walk ends: its run buffer back
        return millis() - t0 > PASS_BUDGET_MS;
      }
      feed(s, i, hours ? &h : nullptr, hours ? nullptr : &d, c);
      if (hours) s.bf_hours++;
      else s.bf_days++;
      if (millis() - t0 > PASS_BUDGET_MS) return true;  // checked per record; the next second goes on
    }
    if (esp_task_wdt_status(nullptr) == ESP_OK) esp_task_wdt_reset();
    if (millis() - t0 > PASS_BUDGET_MS) return true;  // the next second goes on
    vTaskDelay(1);
  }
  // "Restart learning", applied in the live phase (a backfill or a migration finishes first)
  if (s.phase == Phase::Live && s.relearn_req && !s.read_only) {
    float lon;
    xSemaphoreTake(gLock, portMAX_DELAY);
    lon = gBridge.lon;
    xSemaphoreGive(gLock);
    uint32_t at;
    {
      LLock l;
      learnRestart(gL[i], lon);
      at = gL[i].c.relearn_hour;
      s.relearn_req = false;
    }
    s.ev |= LE_RELEARN, s.snap = true;
    logf("LEARN", "%s: restart of learning at hour %lu (asked in the admin)", id, (unsigned long)at);
    writeBlobs(i, "restart learning");
  }
  // live: the RAM rings' records newer than the resume points (a close, or the first pass after the backfill)
  if (s.phase == Phase::Live && (closes != s.closes || days != s.days || s.snap)) {
    s.closes = closes, s.days = days;
    uint32_t nh, nd;
    ringRefs(i, id, &nh, &nd);
    {
      LLock l;
      clampResume(i, nh, nd);
    }
    int n = 0;
    uint32_t seen = 0;  // an hour learnHour declined (approximate clock) is passed over in this pass
    for (; n < BATCH; n++) {
      uint32_t lh, ld;
      {
        LLock l;
        lh = gL[i].c.last_hour, ld = gL[i].c.last_day;
      }
      if (seen > lh) lh = seen;
      bool isHour = false, isDay = false;
      xSemaphoreTake(gLock, portMAX_DELAY);
      st = gStations.at(i);
      if (st && st->gen == gen) {
        for (int j = 0; j < st->hourly.size() && !isHour; j++)
          if (st->hourly.at(j).hour > lh) h = st->hourly.at(j), isHour = true, seen = h.hour;
        if (!isHour)
          for (int j = 0; j < st->daily.size() && !isDay; j++)
            if (st->daily.at(j).ymd > ld) d = st->daily.at(j), isDay = true;
        if (isHour || isDay) ctxOf(*st, isHour ? h.hour : calDayTime(d.ymd), isHour, &c);
      }
      xSemaphoreGive(gLock);
      if (!isHour && !isDay) break;
      feed(s, i, isHour ? &h : nullptr, isHour ? nullptr : &d, c);
    }
    if (n == BATCH) s.closes = closes - 1;  // more may wait: look again at the next pass
    if (n || s.snap) snapshot(i, gen);  // what the outlook may read, after anything was fed
    s.snap = false;
    bool dirty;
    {
      LLock l;
      dirty = gL[i].core_dirty || gL[i].mask_dirty;
    }
    if (dirty && (s.ev || millis() - s.write_ms >= WRITE_EVERY_MS))
      writeBlobs(i, s.ev & ~LE_L1B_COMMIT ? "reset" : s.ev ? "clear day" : "6 h");
  }
  return millis() - t0 > PASS_BUDGET_MS;
}
}  // namespace

// For forwarding: a migration or backfill pass is running (loop task, like learnTick: no lock needed)
bool learnBusy() {
  for (const Slot& s : gS)
    if (s.id[0] && (s.phase == Phase::Migrate || s.phase == Phase::FlashHours || s.phase == Phase::FlashDays)) return true;
  return false;
}

void learnTick() {
  if (learnBusy()) heapCtxMark(HC_LEARN);  // a pass runs this tick (the heap minimum's context)
  if (!gLearnLock) {
    gLearnLock = xSemaphoreCreateMutex();
    uint32_t used = 0, free_e = 0;
    if (nvsStats(&used, &free_e)) logf("LEARN", "NVS: %lu entries used, %lu free", (unsigned long)used, (unsigned long)free_e);
  }
  if (!timeSynced() || otaActive()) return;
  // One budget for the whole tick; once spent the remaining slots wait, and the first slot alternates per
  // tick so a long backfill of one station never starves the other
  static uint8_t first = 0;
  const uint32_t t0 = millis();
  for (int k = 0; k < MAX_STATIONS; k++)
    if (slotTick((first + k) % MAX_STATIONS, t0)) break;
  first = (uint8_t)((first + 1) % MAX_STATIONS);
}

void learnFlushAll() {
  if (!gLearnLock) return;
  for (int i = 0; i < MAX_STATIONS; i++) {
    bool dirty;
    {
      LLock l;
      dirty = gS[i].id[0] && (gL[i].core_dirty || gL[i].mask_dirty);
    }
    if (dirty) writeBlobs(i, "restart");
  }
}

void learnDelete(const char* id) {
  char k[16];
  nvsLearnKey(id, 'r', k);
  nvsDelKey(k);
  nvsLearnKey(id, 'm', k);
  nvsDelKey(k);
  if (!gLearnLock) return;
  for (int i = 0; i < MAX_STATIONS; i++)
    if (gS[i].id[0] && strcmp(gS[i].id, id) == 0) passFree(i);
  LLock l;
  for (auto& s : gS)
    if (s.id[0] && strcmp(s.id, id) == 0) s.id[0] = 0, s.phase = Phase::Off, s.read_only = false;
}

// The persisted core's console MAC hash of a loaded station (0 = unknown). Read without the learner lock: one
// aligned 32-bit word, written only when the core is (re)loaded or a MAC change is judged; a torn id compare costs at
// worst one missed offer, never a wrong adopt (the admin confirms, the route re-checks).
uint32_t learnMacHash(const char* id) {
  for (int i = 0; i < MAX_STATIONS; i++)
    if (gS[i].id[0] && strncmp(gS[i].id, id, STATION_ID_LEN) == 0) return gL[i].c.mac_hash;
  return 0;
}

// "Restart learning" for a station (httpd): the loop applies it in the live phase; false = no learning state
// for it, or a newer image's (read-only)
bool learnRelearn(const char* id) {
  if (!gLearnLock) return false;
  LLock l;
  for (int i = 0; i < MAX_STATIONS; i++)
    if (gS[i].id[0] && strcmp(gS[i].id, id) == 0 && !gS[i].read_only) {
      gS[i].relearn_req = true;
      return true;
    }
  return false;
}

bool learnCopy(const char* id, LearnView* v, LearnMeta* m) {
  if (!gLearnLock) return false;
  xSemaphoreTake(gLock, portMAX_DELAY);
  const float lat = gBridge.lat, lon = gBridge.lon;
  xSemaphoreGive(gLock);
  int i = 0;
  {
    LLock l;
    while (i < MAX_STATIONS && !(gS[i].id[0] && strcmp(gS[i].id, id) == 0)) i++;
    if (i == MAX_STATIONS) return false;
    learnView(gL[i], v);
    const Slot& s = gS[i];
    const bool noLoc = std::isnan(lat) || std::isnan(lon);
    m->state = s.read_only ? "read_only" : s.phase == Phase::Migrate || s.phase == Phase::FlashHours || s.phase == Phase::FlashDays ? "backfill"
               : noLoc                   ? "paused"
                                         : "learning";
    m->why = s.read_only ? "blob_newer" : s.no_store ? "no_store" : noLoc ? "no_location" : nullptr;
    m->backfill_hours = s.bf_hours, m->backfill_done = s.backfilled || s.no_store;
    m->backfill_days = s.bf_days;
    m->backfill_ms = s.backfilled ? s.bf_ms : s.phase == Phase::FlashHours || s.phase == Phase::FlashDays ? millis() - s.bf_t0 : 0;
    m->written = s.written, m->writes = s.writes;
    m->migrated_from = s.mig_from, m->replay_mask = s.mig_mask, m->replay_hours = s.mig_hours;
    m->replay_ms = s.phase == Phase::Migrate && gMigOwner == i ? millis() - s.mig_t0 : s.mig_ms;
  }
  xSemaphoreTake(gLock, portMAX_DELAY);  // the station's LearnUse snapshot (the "use" block)
  if (const Station* st = gStations.at(i)) {
    v->use = st->use, m->use = &v->use;
    // L7's sentence re-judged as the notice (the offset in force, baro_uncalibrated)
    m->p_off = (int16_t)lroundf(st->cfg.pressure_hpa_offset * 10.0f), m->baro_uncal = st->baro_uncal;  // 0.1 hPa
  }
  xSemaphoreGive(gLock);
  m->nvs_used = m->nvs_free = 0;
  nvsStats(&m->nvs_used, &m->nvs_free);
  m->today_unix = nowS();
  m->lat = lat, m->lon = lon;
  return true;
}

}  // namespace app
