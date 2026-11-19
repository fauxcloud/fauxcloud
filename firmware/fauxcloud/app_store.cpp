// app_store.cpp -- LittleFS behind src/store.h and src/ring.h: mount and the format plan, the FileIo
// adapter, the rings of up to RING_SLOTS stations, the shared snapshot, and the station hooks (restore at allocation,
// records on a close, forget, factory reset). See app.h.
#include <LittleFS.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <esp_task_wdt.h>

#include "app.h"
#include "src/ring.h"
#include "src/store.h"

namespace app {

// Every store call is one locked LittleFS operation: the httpd task streams /api/daily?all=1 while loop() writes.
class LittleFsIo : public FileIo {
 public:
  void init() { lock_ = xSemaphoreCreateMutex(); }
  long size(const char* path) override {
    Guard g(lock_);
    File f = LittleFS.open(path, "r");
    if (!f) return -1;
    const long n = (long)f.size();
    f.close();
    return n;
  }
  bool read(const char* path, size_t off, void* buf, size_t len) override {
    Guard g(lock_);
    File f = LittleFS.open(path, "r");
    const bool ok = f && f.seek(off) && f.read((uint8_t*)buf, len) == len;
    if (f) f.close();
    return ok;
  }
  bool write(const char* path, const void* buf, size_t len) override { return put(path, "w", buf, len); }
  bool append(const char* path, const void* buf, size_t len) override { return put(path, "a", buf, len); }
  bool writeAt(const char* path, size_t off, const void* buf, size_t len) override {
    Guard g(lock_);
    File f = LittleFS.open(path, "r+");
    const bool ok = f && f.seek(off) && f.write((const uint8_t*)buf, len) == len;
    if (f) f.close();
    return ok;
  }
  bool rename(const char* from, const char* to) override {
    Guard g(lock_);
    return LittleFS.rename(from, to);  // littlefs replaces an existing `to` atomically: never remove it first
  }
  bool remove(const char* path) override {
    Guard g(lock_);
    return !LittleFS.exists(path) || LittleFS.remove(path);
  }
  bool writeParts(const char* path, const Part* parts, int n) override {
    Guard g(lock_);
    File f = LittleFS.open(path, "w", false);
    bool ok = (bool)f;
    for (int i = 0; ok && i < n; i++) ok = !parts[i].len || f.write((const uint8_t*)parts[i].buf, parts[i].len) == parts[i].len;
    if (f) f.close();
    return ok;
  }
  bool format() override {
    Guard g(lock_);
    return LittleFS.format();
  }
  int list(void (*fn)(void* ctx, const char* name, bool dir), void* ctx) override {
    Guard g(lock_);
    File root = LittleFS.open("/");
    if (!root || !root.isDirectory()) return -1;
    int n = 0;
    for (File f = root.openNextFile(); f; f = root.openNextFile()) {
      const char* name = f.name();
      if (name && *name == '/') name++;
      fn(ctx, name ? name : "", f.isDirectory());
      f.close();
      n++;
    }
    root.close();
    return n;
  }
  bool removeTree(const char* path) override {
    Guard g(lock_);
    return rmTree(path);
  }

 private:
  // every file operation also pets the Task WDT when the calling task is watched (the loop task)
  struct Guard {
    SemaphoreHandle_t m;
    explicit Guard(SemaphoreHandle_t x) : m(x) {
      if (esp_task_wdt_status(nullptr) == ESP_OK) esp_task_wdt_reset();
      if (m) xSemaphoreTake(m, portMAX_DELAY);
    }
    ~Guard() { if (m) xSemaphoreGive(m); }
  };
  bool put(const char* path, const char* mode, const void* buf, size_t len) {
    Guard g(lock_);
    File f = LittleFS.open(path, mode, false);  // a flat root, no directories to create
    const bool ok = f && (!len || f.write((const uint8_t*)buf, len) == len);
    if (f) f.close();
    return ok;
  }
  static bool rmTree(const char* path) {  // under lock_
    File d = LittleFS.open(path);
    if (!d) return true;
    if (!d.isDirectory()) {
      d.close();
      return LittleFS.remove(path);
    }
    char child[64];  // per level
    for (File f = d.openNextFile(); f; f = d.openNextFile()) {
      snprintf(child, sizeof child, "%s/%s", path, f.name());
      const bool dir = f.isDirectory();
      f.close();
      if (dir) rmTree(child);
      else LittleFS.remove(child);
    }
    d.close();
    return LittleFS.rmdir(path);
  }
  SemaphoreHandle_t lock_ = nullptr;
};

static LittleFsIo gIo;
// Off: no partition (mount failed); Wait: the image is Pending and the layout is missing (RAM only until the
// valid mark); Foreign: another layout's marker (never formatted by this build); Ready: the rings are in use.
enum class StoreMode : uint8_t { Off, Wait, Foreign, Ready };
static StoreMode gStoreMode = StoreMode::Off;
static bool gMounted = false;
static uint32_t gStoreMsMax = 0, gStoreFail = 0;
// The store's one fault line that repeats every pass (a record not written): a spell's first line, then one per
// 10 min with the count. The append back-off line is bounded by the back-off itself and prints
// every step; the older-record drop is not a fault spell and prints once per boot (gDropSaid). Loop task only.
static RateLimitedLine gStoreLine;
static bool gDropSaid = false;
static FlashRing gHour[RING_SLOTS], gDay[RING_SLOTS];
static char gOwner[RING_SLOTS][STATION_ID_LEN];
static SemaphoreHandle_t gRingLock = nullptr;  // the ring indexes: loop task appends, httpd copies
// the loop task's snapshot scratch (2 KB): written from here; entry 0 doubles as the read buffer of storeRestore,
// which never runs while a write is being built (storeTick restores first, then takes the entries)
static SnapEntry gSnapOut[RING_SLOTS];
static uint8_t gSnapFrom = 0;  // the snapshot version the first restore read (0 = none yet)
uint8_t storeSnapFrom() { return gSnapFrom; }
static char gForgotten[RING_SLOTS][STATION_ID_LEN];  // Forget: not restored from the snapshot until it is rewritten
FileIo& storeIo() { return gIo; }
bool storeMounted() { return gStoreMode == StoreMode::Ready; }
void storeRingOwner(int k, char out[STATION_ID_LEN]) {  // the boot's load order (loop task, after storeBegin)
  out[0] = 0;
  if (k >= 0 && k < RING_SLOTS && gStoreMode == StoreMode::Ready) memcpy(out, gOwner[k], STATION_ID_LEN);
  out[STATION_ID_LEN - 1] = 0;
}
bool storeLayoutBad() { return gStoreMode == StoreMode::Foreign; }
uint32_t storeMsMax() { return gStoreMsMax; }
uint32_t storeFails() { return gStoreFail; }

struct RingLock {
  RingLock() { if (gRingLock) xSemaphoreTake(gRingLock, portMAX_DELAY); }
  ~RingLock() { if (gRingLock) xSemaphoreGive(gRingLock); }
};

static void logListing() {
  static char line[400];
  struct Ctx {
    char* b;
    size_t cap, len;
    int n;
  } c{line, sizeof line, 0, 0};
  line[0] = 0;
  gIo.list(
      [](void* p, const char* name, bool dir) {
        Ctx* x = (Ctx*)p;
        if (x->len + strlen(name) + 3 < x->cap) x->len += snprintf(x->b + x->len, x->cap - x->len, "%s%s%s", x->n ? " " : "", name, dir ? "/" : "");
        x->n++;
      },
      &c);
  logf("STORE", "root before the cleanup (%d entries): %s", c.n, line);
}

// Entries outside the ring names (an older layout's /d/... after a downgrade and a re-update) are removed, the
// listing logged once
static void cleanup() {
  struct Ctx {
    char names[16][32];
    bool dir[16];
    int n;
  };
  static Ctx c;
  c.n = 0;
  gIo.list(
      [](void* p, const char* name, bool dir) {
        Ctx* x = (Ctx*)p;
        if (b7Name(name) || x->n >= 16) return;
        if (strlen(name) >= sizeof x->names[0]) {  // never logged as removed when it was not
          logf("STORE", "leftover with a long name not removed: %.24s...", name);
          return;
        }
        snprintf(x->names[x->n], sizeof x->names[0], "%s", name);
        x->dir[x->n++] = dir;
      },
      &c);
  if (!c.n) return;
  logListing();  // the full root, once, before anything goes
  for (int i = 0; i < c.n; i++) {
    char p[40];
    snprintf(p, sizeof p, "/%s", c.names[i]);
    const bool ok = c.dir[i] ? gIo.removeTree(p) : gIo.remove(p);
    logf("STORE", "leftover %s%s removed%s", p, c.dir[i] ? "/" : "", ok ? "" : " -- FAILED");
  }
}

static void ready() {
  cleanup();
  const uint32_t fp = ringFingerprint();
  RingLock rl;
  for (int k = 0; k < RING_SLOTS; k++) {
    RingHeader h;
    gOwner[k][0] = 0;
    if (ringHeaderRead(gIo, hourSpec(k), fp, &h)) memcpy(gOwner[k], h.id, sizeof gOwner[k]);
    else ringHeaderWrite(gIo, hourSpec(k), fp, "");
    gHour[k].init(&gIo, hourSpec(k));
    gDay[k].init(&gIo, daySpec(k));
    const uint32_t t0 = millis();
    const int nh = gHour[k].scan(), nd = gDay[k].scan();
    const uint32_t ref = trustedHourStart(gHour[k]);  // the clock's reference (a few reads from the newest end)
    storeNoteHour(ref, 0);
    logf("STORE", "ring %d: %s, %d hour(s), %d day(s), trusted to %lu (scan %lu ms)", k, gOwner[k][0] ? gOwner[k] : "free",
         nh, nd, (unsigned long)ref, (unsigned long)(millis() - t0));
  }
  gStoreMode = StoreMode::Ready;
  logl(Log::Boot, "STORE", "history on flash: %u of %u KB used, snapshot %ld B", (unsigned)(LittleFS.usedBytes() / 1024),
       (unsigned)(LittleFS.totalBytes() / 1024), gIo.size(SNAP_PATH));
}

// The loop task's Task WDT (30 s) is fed by every gIo file operation (Guard); LittleFS.format() is
// one call that cannot be fed inside, so it is fed right before and after and its own time is logged (measured: < 7.3 s)
static bool formatAndBuild() {
  const uint32_t t0 = millis();
  if (esp_task_wdt_status(nullptr) == ESP_OK) esp_task_wdt_reset();
  bool ok = LittleFS.format();
  const uint32_t tf = millis() - t0;
  if (esp_task_wdt_status(nullptr) == ESP_OK) esp_task_wdt_reset();
  if (ok) {
    LittleFS.end();
    gMounted = ok = LittleFS.begin(false, "/littlefs", 4, "littlefs");
  }
  ok = ok && buildLayout(gIo);
  logf("STORE", "LittleFS formatted for the history layout %s (%lu ms, the format call %lu ms): %u of %u KB used",
       ok ? "done" : "FAILED", (unsigned long)(millis() - t0), (unsigned long)tf, (unsigned)(LittleFS.usedBytes() / 1024),
       (unsigned)(LittleFS.totalBytes() / 1024));
  return ok;
}

// The partition table this build's ring geometry needs (the profile's,
// ring.h TABLE_*). Another table (an OTA of this image onto a board never re-flashed over USB) never formats: history
// stays RAM only, loudly.
static bool tableOk() {
  const esp_partition_t* fs = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "littlefs");
  const esp_partition_t* app = esp_ota_get_running_partition();
  if (fs && app && tableMatches(fs->address, fs->size, app->size)) return true;
  logf("STORE", "PARTITION TABLE MISMATCH: littlefs %lu B at 0x%lx (want %lu at 0x%lx), app slot %lu B (want %lu): history RAM "
                "only, nothing formatted; flash this firmware once over USB with its partition table",
       (unsigned long)(fs ? fs->size : 0), (unsigned long)(fs ? fs->address : 0), (unsigned long)TABLE_LFS,
       (unsigned long)TABLE_LFS_OFF, (unsigned long)(app ? app->size : 0), (unsigned long)TABLE_APP);
  return false;
}

// After storeBegin (adopt.h), before the stations load -- the rings owned by `from` are renamed in their headers
// (the records carry no id) and in RAM, and the snapshot entry follows. Idempotent (the journal may be replayed).
bool adoptBootStore() {
  AdoptJournal j;
  if (!adoptJournalRead(&j)) return true;
  if (gStoreMode != StoreMode::Ready) return true;  // history RAM only (or waiting): no header to rename on flash
  int headers = 0;
  bool ok = true;
  for (int k = 0; k < RING_SLOTS; k++) {
    const int n = ringRenameOwner(gIo, ringFingerprint(), k, j.from, j.to);
    if (n > 0) headers += n;
    if (n < 0) ok = false;
    if (strncmp(gOwner[k], j.from, STATION_ID_LEN) == 0) snprintf(gOwner[k], sizeof gOwner[k], "%s", j.to);
  }
  const int snap = snapRename(gIo, j.from, j.to, gSnapOut);
  logf("ADMIN", "adopt %s -> %s: %d ring header(s) renamed, snapshot entry %s", j.from, j.to, headers,
       snap > 0 ? "renamed" : snap == 0 ? "none" : "NOT renamed");
  memset(j.key, 0, sizeof j.key);
  return ok && snap >= 0;
}

void storeBegin() {
  gIo.init();
  gRingLock = xSemaphoreCreateMutex();
  if (!tableOk()) {
    gStoreMode = StoreMode::Foreign;
    return;
  }
  const bool pending = otaPending();
  gMounted = LittleFS.begin(false, "/littlefs", 4, "littlefs");  // never formats on its own
  if (!gMounted && pending) {
    gStoreMode = StoreMode::Wait;
    logf("STORE", "LittleFS not mountable and this image is not confirmed yet: history RAM only until it is");
    return;
  }
  if (!gMounted) {
    if (!formatAndBuild()) {
      logf("STORE", "LittleFS unusable: history is RAM only until the next boot");
      return;
    }
    ready();
    return;
  }
  switch (formatPlan(markerRead(gIo), pending)) {
    case FormatPlan::Use: ready(); break;
    case FormatPlan::WaitValid:
      gStoreMode = StoreMode::Wait;
      logf("STORE", "no history layout yet and this image is not confirmed: no flash access, history RAM only until the "
                    "valid mark (then the partition is formatted once)");
      break;
    case FormatPlan::RamOnlyForeign:
      gStoreMode = StoreMode::Foreign;  // the store_layout notice says it (not a store_fail count)
      logf("STORE", "LittleFS HOLDS ANOTHER LAYOUT (marker of another build): not formatted, history RAM only");
      break;
    case FormatPlan::Format:
      if (formatAndBuild()) ready();
      break;
  }
}

// The image was just marked valid (app_sys.cpp): a waiting store formats once and builds the layout; the stations
// already in RAM get their rings at the next storeRestore (everySecond).
void storeImageValid() {
  if (gStoreMode != StoreMode::Wait) return;
  if (!gMounted) gMounted = LittleFS.begin(false, "/littlefs", 4, "littlefs");
  if (formatAndBuild()) ready();
}

bool storeFormat() {  // the factory reset's format, fed and timed like formatAndBuild
  if (!tableOk()) return false;  // never on another partition table
  RingLock rl;
  gStoreMode = StoreMode::Off;
  if (!gMounted) gMounted = LittleFS.begin(false, "/littlefs", 4, "littlefs");
  const bool ok = gMounted && formatAndBuild();  // factory reset: empty rings, marker last; fed, timed, logged
  for (int k = 0; k < RING_SLOTS; k++) gOwner[k][0] = 0;
  return ok;
}

// The ring slot of a station: its own, else a free one (claimed: the header gets its id), else -1 (RAM only).
static int ringOf(const char* id, bool claim) {
  for (int k = 0; k < RING_SLOTS; k++)
    if (gOwner[k][0] && strcmp(gOwner[k], id) == 0) return k;
  if (!claim) return -1;
  for (int k = 0; k < RING_SLOTS; k++)
    if (!gOwner[k][0]) {
      if (gHour[k].count() || gDay[k].count()) {  // a "free" ring with records is not taken over
        static bool said[RING_SLOTS];
        if (!said[k]) logf("STORE", "ring %d has no owner but %d hour(s) / %d day(s): not claimed", k, gHour[k].count(), gDay[k].count());
        said[k] = true;
        continue;
      }
      if (!ringHeaderWrite(gIo, hourSpec(k), ringFingerprint(), id)) return -1;
      ringHeaderWrite(gIo, daySpec(k), ringFingerprint(), id);
      snprintf(gOwner[k], sizeof gOwner[k], "%s", id);
      logf("STORE", "%s: ring %d claimed", id, k);
      return k;
    }
  return -1;
}

// The newest keys of a station's flash rings (0 when it has none); any task, no gLock
void storeNewestKeys(const char* id, uint32_t* hour, uint32_t* ymd) {
  *hour = *ymd = 0;
  if (gStoreMode != StoreMode::Ready) return;
  RingLock rl;
  const int k = ringOf(id, false);
  if (k >= 0) *hour = gHour[k].newestKey(), *ymd = gDay[k].newestKey();
}

// Caller holds gLock (released around each flash read). The newest 48 hours and 30 days into RAM one record at a
// time (no buffer), the open day reopened, then the station's snapshot entry (flash wins over a snapshot hour that is
// already closed).
void storeRestore(Station& st) {
  if (gStoreMode != StoreMode::Ready) return;
  if (!(st.flash_state & FLASH_RINGS_LOADED)) {
    st.flash_state |= FLASH_RINGS_LOADED;
    char id[STATION_ID_LEN];
    memcpy(id, st.id, sizeof id);
    const uint32_t t0 = millis();
    int k, hFrom = 0, hTo = 0, dFrom = 0, dTo = 0;
    xSemaphoreGive(gLock);
    {
      RingLock rl;
      k = ringOf(id, true);
      if (k >= 0) {
        hTo = gHour[k].count();
        hFrom = hTo > HOURLY_LEN ? hTo - HOURLY_LEN : 0;
        dTo = gDay[k].count();
        dFrom = dTo > DAILY_LEN ? dTo - DAILY_LEN : 0;
      }
    }
    xSemaphoreTake(gLock, portMAX_DELAY);
    if (k < 0) {
      logf("STORE", "%s: no free ring (%d per bridge): its history is RAM only", id, RING_SLOTS);
    } else if (!st.uploads) {  // a station that uploaded meanwhile keeps its RAM rings
      st.hourly.clear();
      st.daily.clear();
      int nh = 0, nd = 0, bad = 0;
      static HourRec h;  // loop task only
      static DayRec d;
      for (int i = hFrom; i < hTo; i++) {
        xSemaphoreGive(gLock);
        bool ok;
        {
          RingLock rl;
          ok = gHour[k].read(i, &h) && hourValid(h);
        }
        xSemaphoreTake(gLock, portMAX_DELAY);
        if (ok && !st.uploads) st.hourly.pushRaw(h), nh++;
        else bad += !ok;
      }
      for (int i = dFrom; i < dTo; i++) {
        xSemaphoreGive(gLock);
        bool ok;
        {
          RingLock rl;
          ok = gDay[k].read(i, &d) && dayValid(d);
        }
        xSemaphoreTake(gLock, portMAX_DELAY);
        if (ok && !st.uploads) st.daily.pushClosed(d), nd++;
        else bad += !ok;
      }
      st.daily.reopen(st.hourly, gTz);
      st.flash_rev++;
      logl(Log::Boot, "STORE", "%s: %d hour(s) and %d day(s) back from ring %d%s (%lu ms)", id, nh, nd, k,
           bad ? " (records with a bad CRC skipped)" : "", (unsigned long)(millis() - t0));
    }
  }
  if (!(st.flash_state & FLASH_SNAP_DONE)) {
    st.flash_state |= FLASH_SNAP_DONE;
    bool forgotten = false;
    for (const auto& f : gForgotten) forgotten = forgotten || strcmp(f, st.id) == 0;
    SnapEntry& e = gSnapOut[0];
    char id[STATION_ID_LEN];
    memcpy(id, st.id, sizeof id);
    xSemaphoreGive(gLock);  // no flash read under gLock
    uint8_t ver = 0;
    const bool found = !forgotten && snapFind(gIo, id, &e, &ver);
    if (found && !gSnapFrom) gSnapFrom = ver;  // diag snap_from
    xSemaphoreTake(gLock, portMAX_DELAY);
    if (found) {
      uint32_t newest = 0;
      {
        RingLock rl;
        const int k = ringOf(st.id, false);
        if (k >= 0) newest = gHour[k].newestKey();
      }
      const bool hour = applySnapEntry(st, e, newest);
      st.flash_rev++;
      logf("STORE", "%s: snapshot applied (%s%s)", st.id, hour ? "open hour" : "no open hour",
           e.live.s.present ? ", last reading" : "");
    }
  }
}

// loop(), outside gLock: due bits ask for the snapshot; records follow the RAM rings (every hour / day newer than the
// ring's newest is appended, a re-closed newest replaces it), copied one at a time under the lock.
static volatile uint8_t gDue[MAX_STATIONS];
bool storeBusy() {  // forwarding waits while history writes are due
  for (int i = 0; i < MAX_STATIONS; i++)
    if (gDue[i]) return true;
  return false;
}
void storeMarkDue(int slot, bool hour, bool day) {
  if (slot < 0 || slot >= MAX_STATIONS) return;
  gDue[slot] |= (hour ? 1 : 0) | (day ? 3 : 0);  // a close also writes the snapshot
}

// What the rings refused or repaired since boot (diag store_drop_old / store_cut / store_fail_append /
// store_future_dropped)
static uint32_t gDropOld = 0, gFailAppend = 0, gFutureDropped = 0;
uint32_t storeDropOld() { return gDropOld; }
uint32_t storeFailAppend() { return gFailAppend; }
uint32_t storeFutureDropped() { return gFutureDropped; }
uint32_t storeCut() {
  RingLock rl;
  uint32_t n = 0;
  for (int k = 0; k < RING_SLOTS; k++) n += gHour[k].cut() + gDay[k].cut();
  return n;
}

// The clock's reference, the end of the newest trusted hour (atomic max; any task reads)
static std::atomic<uint32_t> gClockRef{0};
uint32_t storeClockRef() { return gClockRef.load(); }
void storeNoteHour(uint32_t hour, uint16_t quality) {
  const uint32_t end = trustedStart(hour, quality);
  uint32_t cur = gClockRef.load();
  while (end > cur && !gClockRef.compare_exchange_weak(cur, end)) {
  }
}
uint32_t storeFutureKept() {
  RingLock rl;
  uint32_t n = 0;
  for (int k = 0; k < RING_SLOTS; k++) n += gHour[k].futureKept() + gDay[k].futureKept();
  return n;
}
static bool gStoreFrozen = false;  // after the 24-h clock acceptance, until the planned restart (loop task)

// A ring whose newest key is in the future (a clock that went back after a wrong-forward seed) would refuse every
// real close as "older": its future records are cut first (logged once per boot). Only records
// stamped on an approximate clock; with the hold gate this runs only on a plausible clock.
static void dropFuture(FlashRing& ring, uint32_t limit, const char* id) {
  if (!ring.count() || ring.newestKey() <= limit) return;
  const uint32_t kept = ring.futureKept();
  const int n = ring.dropFuture(limit, true, 1u << Q_CLOCK_APPROX);
  if (ring.futureKept() != kept)
    logf("STORE", "%s: a trusted %s record past the clock (%lu) kept: not cut", id, ring.spec().kind == 'h' ? "hourly" : "daily",
         (unsigned long)ring.newestKey());
  if (!n) return;
  gFutureDropped += (uint32_t)n;
  static bool said = false;
  if (!said) logf("STORE", "%s: %d record(s) with a key in the future cut from the %s ring (newest now %lu)", id, n,
                  ring.spec().kind == 'h' ? "hourly" : "daily", (unsigned long)ring.newestKey());
  said = true;
}

// Appends the RAM records newer than the ring's newest (the re-closed newest too). A record closed since the last
// pass (key > *seen) but older than the ring's newest is a drop (counted, the first one since boot logged). Stops at a failed
// write so the next pass retries from that record: false then.
template <typename Rec, typename Get>
static bool appendNewer(FlashRing& ring, int n, bool reclosed, uint32_t* seen, Get get, const char* id) {
  static Rec r;  // loop task only
  uint32_t newest;
  {
    RingLock rl;
    newest = ring.newestKey();
  }
  if (!*seen) *seen = newest;  // the first pass: what flash already has is not "closed since" (no false drops)
  for (int j = 0; j < n; j++) {
    xSemaphoreTake(gLock, portMAX_DELAY);
    const bool have = get(j, &r);
    xSemaphoreGive(gLock);
    if (!have) break;  // the station went (forget) or the ring moved on: the next pass catches up
    uint32_t key;
    memcpy(&key, &r, sizeof key);
    if (key < newest) {
      if (key > *seen) {  // closed since the last pass, yet older than flash: refused
        if (!gDropSaid)  // once per boot, every ring and kind (store_drop_old counts them all)
          logf("STORE", "%s: record %lu older than the ring's newest %lu: dropped", id, (unsigned long)key, (unsigned long)newest);
        gDropSaid = true;
        gDropOld++;
        *seen = key;
      }
      continue;
    }
    if (key == newest && !(reclosed && j == n - 1)) continue;
    FlashRing::Add a;
    {
      RingLock rl;
      a = ring.append(&r);
    }
    if (a == FlashRing::Add::Failed) {
      gFailAppend++;
      uint32_t nn, min;
      char sx[32];
      if (gStoreLine.hit(millis(), &nn, &min))
        logf("STORE", "%s: %s record %lu NOT written: retried at the next pass%s", id, ring.spec().kind == 'h' ? "hourly" : "daily",
             (unsigned long)key, rateSuffix(sx, sizeof sx, nn, min));
      return false;
    }
    if (key > *seen) *seen = key;
    if (ring.spec().kind == 'h') {
      uint16_t q;
      memcpy(&q, (const uint8_t*)&r + offsetof(HourRec, quality), sizeof q);
      storeNoteHour(key, q);
    }
    // routine, verbose only (the ring and the API hold the facts)
    if (a == FlashRing::Add::Rotated && ring.spec().kind == 'h') logl(Log::Chatter, "STORE", "%s: hourly ring rotated", id);
    if (ring.spec().kind == 'd') logl(Log::Chatter, "STORE", "%s: day %lu written", id, (unsigned long)key);
  }
  return true;
}

// A station whose append failed waits storeRetryS() before the next try; a pass that only retries does
// not rewrite the snapshot (written only when a station has its own 10-min / close / day due bit)
static uint8_t gRetryBits[MAX_STATIONS], gRetryFails[MAX_STATIONS];
static uint32_t gRetryAt[MAX_STATIONS];  // millis() of the next try
static uint32_t gRetrySteps = 0;          // back-off steps since boot (diag store_retry)
uint32_t storeRetrySteps() { return gRetrySteps; }

void storeTick() {
  if (gStoreMode != StoreMode::Ready || gStoreFrozen || !timeSynced() || otaActive()) return;  // writes wait for the end of a firmware upload
  if (storeBusy()) heapCtxMark(HC_STORE);  // history writes this tick (the heap minimum's context)
  static SnapTimer timers[MAX_STATIONS];
  static uint32_t closesSeen[MAX_STATIONS];
  static uint32_t seenH[MAX_STATIONS], seenD[MAX_STATIONS];  // the newest RAM keys handled (drop accounting)
  const uint32_t ms = millis();
  bool anyDue = false, ownDue = false;
  uint8_t retryNow[MAX_STATIONS] = {};
  xSemaphoreTake(gLock, portMAX_DELAY);
  for (int i = 0; i < MAX_STATIONS; i++) {
    const Station* st = gStations.at(i);
    if (st && timers[i].due(ms, st->uploads)) gDue[i] |= 1;  // the 10 min rewrite: the last reading stays fresh
    if (gRetryBits[i] && (int32_t)(ms - gRetryAt[i]) >= 0) retryNow[i] = gRetryBits[i];
    ownDue = ownDue || gDue[i];
    anyDue = anyDue || gDue[i] || retryNow[i];
  }
  xSemaphoreGive(gLock);
  if (!anyDue) return;
  const uint32_t t0 = millis();
  bool okAll = true;
  for (int i = 0; i < MAX_STATIONS; i++) {
    if (gRetryBits[i] && !retryNow[i]) {  // backing off: its own due bits wait with it (the snapshot still follows)
      gRetryBits[i] |= gDue[i];
      gDue[i] = 0;
      continue;
    }
    const uint8_t due = gDue[i] | retryNow[i];
    if (!due) continue;
    gDue[i] = 0;
    gRetryBits[i] = 0;
    char id[STATION_ID_LEN] = "";
    int nh = 0, nd = 0, k = -1;
    bool reclosed = false;
    uint32_t gen = 0;
    xSemaphoreTake(gLock, portMAX_DELAY);
    Station* st = gStations.at(i);
    if (st) {
      storeRestore(*st);  // never write before the station's rings and snapshot entry were read
      memcpy(id, st->id, sizeof id);
      gen = st->gen;
      nh = st->hourly.size();
      nd = st->daily.size();
      reclosed = st->hour_closes != closesSeen[i];
      closesSeen[i] = st->hour_closes;
      timers[i].written(ms, st->uploads);  // also after a close: the 10 min start again
    }
    xSemaphoreGive(gLock);
    if (!st) continue;
    {
      RingLock rl;
      k = ringOf(id, true);
    }
    if (k < 0) continue;
    {  // future keys (a clock that went back) cut before the closes are appended
      const uint32_t now = nowS();
      RingLock rl;
      dropFuture(gHour[k], now + 3600, id);
      dropFuture(gDay[k], dayNumberToYmd(localDayNumber(gTz, (int64_t)now) + 1), id);
    }
    // the station by slot and generation: a forget in between stops the copy
    auto station = [&]() -> Station* {
      Station* s = gStations.at(i);
      return s && s->gen == gen ? s : nullptr;
    };
    const bool okH = appendNewer<HourRec>(gHour[k], nh, reclosed, &seenH[i], [&](int j, HourRec* out) {
              Station* s = station();
              if (!s || j >= s->hourly.size()) return false;
              *out = s->hourly.at(j);
              return true;
            }, id);
    const bool okD = appendNewer<DayRec>(gDay[k], nd, (due & 2) != 0, &seenD[i], [&](int j, DayRec* out) {
              Station* s = station();
              if (!s || j >= s->daily.size()) return false;
              *out = s->daily.at(j);
              return true;
            }, id);
    if (!okH || !okD) {  // retried after a back-off (the re-close and day flags kept)
      gRetryBits[i] = due | 1;
      if (gRetryFails[i] < 255) gRetryFails[i]++;
      const uint32_t wait = storeRetryS(gRetryFails[i]);
      gRetryAt[i] = millis() + wait * 1000u;
      gRetrySteps++;
      logf("STORE", "%s: append failed %u time(s) in a row: next try in %lu s", id, (unsigned)gRetryFails[i], (unsigned long)wait);
      if (reclosed) closesSeen[i]--;  // still "re-closed" at the retry
      okAll = false;
    } else {
      gRetryFails[i] = 0;
    }
  }
  if (!ownDue) {  // a retry-only pass: no snapshot rewrite
    if (!okAll) gStoreFail++;
    return;
  }
  // the shared snapshot: every station that owns a ring and has something to keep
  int n = 0;
  xSemaphoreTake(gLock, portMAX_DELAY);
  for (int i = 0; i < MAX_STATIONS && n < RING_SLOTS; i++) {
    const Station* st = gStations.at(i);
    if (!st || !snapshotWorthWriting(*st)) continue;
    bool owns;
    {
      RingLock rl;
      owns = ringOf(st->id, false) >= 0;
    }
    if (owns) takeSnapEntry(*st, &gSnapOut[n++]);
  }
  const ParkedList parked = gParked;  // their entries ride through the rewrite untouched
  xSemaphoreGive(gLock);
  n = snapCarryParked(gIo, gSnapOut, n, RING_SLOTS, parked);  // no flash read under gLock
  const bool okSnap = snapWrite(gIo, gSnapOut, n);
  if (okSnap) memset(gForgotten, 0, sizeof gForgotten);  // the forgotten entries are gone from the file now
  const uint32_t ms2 = millis() - t0;
  if (ms2 > gStoreMsMax) gStoreMsMax = ms2;
  if (!okSnap || !okAll) {
    gStoreFail++;
    logf("STORE", "history write%s FAILED (%lu ms)", okAll ? " (snapshot)" : "", (unsigned long)ms2);
  }
}

// A clock held for 24 h of steady public SNTP is accepted; the records newer than it
// (approximate or not) are cut once, the snapshot's future open hours dropped, and the store waits for the restart
// the caller plans (the RAM rings come back from the cut flash).
int storeClockAccept(uint32_t now) {
  if (gStoreMode != StoreMode::Ready) {
    gClockRef.store(0);  // RAM only: nothing on flash to protect; the RAM rings reload at the restart
    gStoreFrozen = true;
    return 0;
  }
  const AcceptLimits lim = clockAcceptLimits(now, gTz);  // the open hour / day go too
  int cut = 0;
  uint32_t ref = 0;
  {
    RingLock rl;
    for (int k = 0; k < RING_SLOTS; k++) {
      cut += gHour[k].dropFuture(lim.hour, false) + gDay[k].dropFuture(lim.ymd, false);
      const uint32_t r = trustedHourStart(gHour[k]);
      if (r > ref) ref = r;
    }
  }
  const int n = snapRead(gIo, gSnapOut, RING_SLOTS);
  int open = 0, changed = 0;
  for (int i = 0; i < n; i++) {
    if (gSnapOut[i].hour.hour > lim.hour) memset(&gSnapOut[i].hour, 0, sizeof gSnapOut[i].hour), open++;
    changed += snapClampFuture(&gSnapOut[i], now);  // last_seen and the arrival times
  }
  if (n > 0 && (open || changed)) snapWrite(gIo, gSnapOut, n);
  gClockRef.store(ref);
  gStoreFrozen = true;
  logf("STORE", "clock accepted after 24 h of internet time: %d newer record(s) cut, %d open hour(s) dropped", cut, open);
  return cut;
}

// Before a planned restart (OTA, admin, setup again, sta_lost, Wi-Fi change) every station's snapshot
// is written now, not at the next 10 min mark. Loop task (the restart runs from coreLoop()).
void storeFlushAll() {
  for (int i = 0; i < MAX_STATIONS; i++) gDue[i] |= 1;
  storeTick();
}

// Forget: the rings truncated to 0 (no erase), the header freed, the snapshot entry gone at the next write
void storeForget(const char* id) {
  if (gStoreMode != StoreMode::Ready) return;
  RingLock rl;
  const int k = ringOf(id, false);
  for (auto& f : gForgotten)
    if (!f[0]) {
      snprintf(f, sizeof f, "%s", id);
      break;
    }
  for (int i = 0; i < MAX_STATIONS; i++) gDue[i] |= 1;  // the snapshot is rewritten without it at the next tick
  if (k < 0) return;
  if (!ringForgetOwner(gIo, ringFingerprint(), id, &gHour[k], &gDay[k]))  // by the header's id
    logf("STORE", "%s: ring %d header did not match or a write failed", id, k);
  gOwner[k][0] = 0;
  logf("STORE", "%s: ring %d emptied and freed", id, k);
}

// The owned rings (id by header, records held), for /api/admin/rings; returns the count
int storeRingList(StoreRingRow out[RING_SLOTS]) {
  if (gStoreMode != StoreMode::Ready) return 0;
  RingLock rl;
  int n = 0;
  for (int k = 0; k < RING_SLOTS; k++) {
    if (!gOwner[k][0]) continue;
    memcpy(out[n].id, gOwner[k], sizeof out[n].id);
    out[n].hours = gHour[k].count();
    out[n].days = gDay[k].count();
    n++;
  }
  return n;
}

bool storeOwns(const char* id) {
  if (gStoreMode != StoreMode::Ready || !id[0]) return false;
  RingLock rl;
  return ringOf(id, false) >= 0;
}

// Learners (loop task): one record at a time under the ring lock, by key (the backfill / catch-up reads)
// nextOf's run cache (loop task only)
constexpr size_t RUN_CACHE_BYTES = 384;
struct RunCache {
  uint8_t* buf;
  const FlashRing* ring;
  int count, at, have;
  uint32_t oldest, newest;
};
static RunCache gRun = {};
// A walk that stops before its end (a forget, a slot reload) gives the buffer back
void storeRunRelease() {
  free(gRun.buf);
  gRun = RunCache{};
}
template <typename R>
static bool nextOf(FlashRing* rings, const char* id, uint32_t after, RingCursor* cur, R* out, bool (*valid)(const R&)) {
  if (gStoreMode != StoreMode::Ready) return false;
  RingLock rl;
  const int k = ringOf(id, false);
  if (k < 0) return false;
  const FlashRing& r = rings[k];
  const int n = r.count();
  const uint32_t oldest = r.oldestKey();
  int i = cur->i >= 0 && cur->count == n && cur->oldest == oldest ? cur->i : r.lowerBound(after + 1);
  cur->count = n, cur->oldest = oldest;
  // The records come in runs of one file read each (8 hourly / 4 daily), cached on the heap while a walk
  // goes on (freed at its end; a failed allocation falls back to one read per record)
  constexpr int RUN = (int)(RUN_CACHE_BYTES / sizeof(R));
  if (!gRun.buf) gRun.buf = (uint8_t*)malloc(RUN_CACHE_BYTES);
  for (; i < n; i++) {
    bool ok;
    if (gRun.buf) {
      const bool hit = gRun.ring == &r && gRun.count == n && gRun.oldest == oldest && gRun.newest == r.newestKey() &&
                       i >= gRun.at && i < gRun.at + gRun.have;
      if (!hit) {
        gRun.ring = &r, gRun.count = n, gRun.oldest = oldest, gRun.newest = r.newestKey(), gRun.at = i;
        gRun.have = r.readRun(i, gRun.buf, RUN);
      }
      ok = i < gRun.at + gRun.have;
      if (ok) memcpy(out, gRun.buf + (size_t)(i - gRun.at) * sizeof(R), sizeof(R));
    } else {
      ok = r.read(i, out);
    }
    if (ok && valid(*out) && *(const uint32_t*)out > after) {
      cur->i = i + 1;
      return true;
    }
  }
  cur->i = i;
  storeRunRelease();  // the walk is over
  return false;
}
bool storeNextHour(const char* id, uint32_t after, RingCursor* cur, HourRec* out) {
  return nextOf<HourRec>(gHour, id, after, cur, out, hourValid);
}
bool storeNextDay(const char* id, uint32_t after, RingCursor* cur, DayRec* out) {
  return nextOf<DayRec>(gDay, id, after, cur, out, dayValid);
}

// httpd: a copy of a station's ring index ('h' / 'd'); false when the station has no ring
bool storeRingCopy(const char* id, char kind, FlashRing* out) {
  if (gStoreMode != StoreMode::Ready) return false;
  RingLock rl;
  const int k = ringOf(id, false);
  if (k < 0) return false;
  *out = kind == 'h' ? gHour[k] : gDay[k];
  return true;
}

#ifdef FAUXCLOUD_TEST
// /api/admin/debug/seed-days (test build): fills a station's rings past wrap -- whole segments written directly (one
// write per segment), then `extra` real appends that rotate. Keys end at the given hour / day.
bool storeSeed(const char* id, uint32_t last_hour, uint32_t last_ymd_day, int extra, char* msg, size_t cap) {
  if (gStoreMode != StoreMode::Ready) {
    snprintf(msg, cap, "store not ready");
    return false;
  }
  RingLock rl;
  const int k = ringOf(id, true);
  if (k < 0) {
    snprintf(msg, cap, "no free ring");
    return false;
  }
  const uint32_t t0 = millis();
  uint8_t* blk = (uint8_t*)malloc(4096);  // test build: borrowed for the seed, no static 4 KB
  if (!blk) {
    snprintf(msg, cap, "no memory");
    return false;
  }
  const int HS = hourSpec(k).segs, DS = daySpec(k).segs;  // per ring
  const int hTotal = HS * HOUR_PER_SEG + extra, dTotal = DS * DAY_PER_SEG + extra;
  uint32_t h0 = last_hour - (uint32_t)(hTotal - 1) * 3600u;
  for (int seg = 0; seg < HS; seg++) {
    for (int j = 0; j < HOUR_PER_SEG; j++) {
      HourRec r;
      clearHour(&r, h0 + (uint32_t)(seg * HOUR_PER_SEG + j) * 3600u);
      r.n = 55;
      r.t_avg = (int16_t)(50 + (seg * 7 + j) % 100);
      r.t_min = r.t_avg - 5;
      r.t_max = r.t_avg + 5;
      r.p_avg = 10132;
      r.rh_avg = r.rh_min = r.rh_max = 80;
      r.wind_avg = 30;
      r.gust_max = 60;
      r.wdir_steady = packDir((uint16_t)((seg * 30 + j) % 360), 100);
      r.rain_sum = 0;
      sealHour(&r);
      memcpy(blk + j * sizeof r, &r, sizeof r);
    }
    char p[RING_PATH_MAX];
    ringSegPath(p, hourSpec(k), seg);
    gIo.write(p, blk, HOUR_PER_SEG * sizeof(HourRec));
  }
  const int32_t lastDay = ymdToDayNumber(last_ymd_day);
  for (int seg = 0; seg < DS; seg++) {
    for (int j = 0; j < DAY_PER_SEG; j++) {
      DayRec r;
      clearDay(&r, dayNumberToYmd(lastDay - (dTotal - 1) + seg * DAY_PER_SEG + j));
      r.n = 1300;
      r.hours = 24;
      r.clean_hours = 24;
      r.t_min = -20;
      r.t_avg = 50;
      r.t_max = 120;
      r.rain_sum = 120;
      sealDay(&r);
      memcpy(blk + j * sizeof r, &r, sizeof r);
    }
    char p[RING_PATH_MAX];
    ringSegPath(p, daySpec(k), seg);
    gIo.write(p, blk, DAY_PER_SEG * sizeof(DayRec));
  }
  free(blk);
  gHour[k].scan();
  gDay[k].scan();
  const uint32_t t1 = millis();
  for (int i = 0; i < extra; i++) {
    HourRec h;
    clearHour(&h, h0 + (uint32_t)(HS * HOUR_PER_SEG + i) * 3600u);
    h.n = 50;
    sealHour(&h);
    gHour[k].append(&h);
    DayRec d;
    clearDay(&d, dayNumberToYmd(lastDay - (extra - 1) + i));
    d.n = 1200;
    sealDay(&d);
    gDay[k].append(&d);
  }
  snprintf(msg, cap, "ring %d: %d hours, %d days (segments %lu ms, %d appends past wrap %lu ms)", k, gHour[k].count(),
           gDay[k].count(), (unsigned long)(t1 - t0), extra, (unsigned long)(millis() - t1));
  logf("STORE", "debug seed %s: %s", id, msg);
  return true;
}
#endif

}  // namespace app
