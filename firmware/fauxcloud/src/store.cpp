// store.cpp -- see store.h.
#include "store.h"

#include "station.h"

#include <cstddef>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <type_traits>

namespace fc {

static_assert(std::is_trivially_copyable<SnapEntry>::value && std::is_trivially_copyable<HourRec>::value &&
                  std::is_trivially_copyable<DayRec>::value,
              "flash records are raw copies");

static const char MARKER_MAGIC[4] = {'F', 'C', '7', 'M'};
static const char SNAP_MAGIC[4] = {'F', 'C', 'S', '7'};
static const uint8_t SNAP_VERSION = 8;  // 7 + SnapStale (7 is still read)

uint32_t crc32(const void* data, size_t len, uint32_t crc) {
  const uint8_t* p = (const uint8_t*)data;
  crc = ~crc;
  for (size_t i = 0; i < len; i++) {
    crc ^= p[i];
    for (int k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
  }
  return ~crc;
}

static void putU16(uint8_t* p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static uint16_t getU16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static void putU32(uint8_t* p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i)); }
static uint32_t getU32(const uint8_t* p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

uint32_t ringFingerprint() {
  // the per-ring geometry (a change formats once). The ring count is the profile's; the field order is fixed
  // (record layout, hourly segments per ring, R, daily segments per ring, R, ring count), so the PSRAM profile's
  // two-ring fingerprint is the earlier two-ring layout's and the WROOM-32's one-ring layout gets its own
  uint16_t layout[6 + 2 * RING_SLOTS + 3];
  int n = 0;
  for (uint16_t v : {(uint16_t)sizeof(HourRec), (uint16_t)offsetof(HourRec, ch_avg), (uint16_t)offsetof(HourRec, crc),
                     (uint16_t)sizeof(DayRec), (uint16_t)offsetof(DayRec, ch), (uint16_t)offsetof(DayRec, crc)})
    layout[n++] = v;
  for (int k = 0; k < RING_SLOTS; k++) layout[n++] = (uint16_t)HOUR_SEGS_K[k];
  layout[n++] = (uint16_t)HOUR_PER_SEG;
  for (int k = 0; k < RING_SLOTS; k++) layout[n++] = (uint16_t)DAY_SEGS_K[k];
  layout[n++] = (uint16_t)DAY_PER_SEG;
  layout[n++] = (uint16_t)RING_SLOTS;
  return crc32(layout, sizeof layout);
}

static uint32_t snapPrint(size_t entry) {
  const uint16_t layout[] = {(uint16_t)entry,             (uint16_t)sizeof(HourAcc),  (uint16_t)sizeof(FilterState),
                             (uint16_t)sizeof(SnapLive),  (uint16_t)sizeof(SnapKnown), (uint16_t)F_COUNT,
                             (uint16_t)CORE_FIELDS,       (uint16_t)RING_SLOTS};
  return crc32(layout, sizeof layout, ringFingerprint());
}
uint32_t snapFingerprint() { return snapPrint(sizeof(SnapEntry)); }

int snapRename(FileIo& io, const char* from, const char* to, SnapEntry* scratch) {
  if (!from || !from[0] || !to || !to[0]) return 0;
  const int n = snapRead(io, scratch, RING_SLOTS);
  if (n <= 0) return 0;
  int hit = -1;
  for (int i = 0; i < n; i++)
    if (strncmp(scratch[i].id, from, STATION_ID_LEN) == 0) hit = i;
  if (hit < 0) return 0;
  memset(scratch[hit].id, 0, sizeof scratch[hit].id);
  strncpy(scratch[hit].id, to, sizeof scratch[hit].id - 1);
  return snapWrite(io, scratch, n) ? 1 : -1;
}
uint32_t snapFingerprintV7() { return snapPrint(sizeof(SnapEntryV7)); }

// the entry size a header announces for its version (0 = a version this build does not read)
static size_t snapEntrySize(const uint8_t* h) {
  if (memcmp(h, SNAP_MAGIC, 4) != 0) return 0;
  if (h[4] == SNAP_VERSION && getU16(h + 6) == sizeof(SnapEntry) && getU32(h + 8) == snapFingerprint()) return sizeof(SnapEntry);
  if (h[4] == 7 && getU16(h + 6) == sizeof(SnapEntryV7) && getU32(h + 8) == snapFingerprintV7()) return sizeof(SnapEntryV7);
  return 0;
}

MarkerState markerRead(FileIo& io) {
  const long sz = io.size(MARKER_PATH);
  if (sz < 0) return MarkerState::Missing;
  uint8_t m[8];
  if (sz != (long)sizeof m || !io.read(MARKER_PATH, 0, m, sizeof m) || memcmp(m, MARKER_MAGIC, 4) != 0 ||
      getU32(m + 4) != ringFingerprint())
    return MarkerState::Foreign;
  return MarkerState::Valid;
}

bool buildLayout(FileIo& io) {
  const uint32_t fp = ringFingerprint();
  bool ok = true;
  for (int k = 0; k < RING_SLOTS && ok; k++) {
    for (const RingSpec& s : {hourSpec(k), daySpec(k)}) {
      ok = ok && ringHeaderWrite(io, s, fp, "");
      for (int seg = 0; ok && seg < s.segs; seg++) {  // empty files: the root's metadata is its full size from day one
        char p[RING_PATH_MAX];
        ringSegPath(p, s, seg);
        ok = io.write(p, nullptr, 0);
      }
    }
  }
  uint8_t m[8];
  memcpy(m, MARKER_MAGIC, 4);
  putU32(m + 4, fp);
  return ok && io.write(MARKER_PATH, m, sizeof m);  // last: a power cut before this formats again
}

// ---------------------------------------------------------------- shared snapshot

bool snapWrite(FileIo& io, const SnapEntry* entries, int n) {
  if (n < 0 || n > RING_SLOTS) return false;
  uint8_t h[SNAP_HEADER_SIZE] = {};
  memcpy(h, SNAP_MAGIC, 4);
  h[4] = SNAP_VERSION;
  h[5] = (uint8_t)n;
  putU16(h + 6, (uint16_t)sizeof(SnapEntry));
  putU32(h + 8, snapFingerprint());
  putU32(h + 12, crc32(entries, (size_t)n * sizeof(SnapEntry)));
  const FileIo::Part parts[] = {{h, sizeof h}, {entries, (size_t)n * sizeof(SnapEntry)}};
  // a power cut before the rename leaves the previous snapshot whole
  if (!io.writeParts(SNAP_TMP_PATH, parts, 2) || !io.rename(SNAP_TMP_PATH, SNAP_PATH)) {
    io.remove(SNAP_TMP_PATH);
    return false;
  }
  return true;
}

int snapRead(FileIo& io, SnapEntry* out, int max, uint8_t* version) {
  const long sz = io.size(SNAP_PATH);
  if (sz < 0) return 0;
  uint8_t h[SNAP_HEADER_SIZE];
  if (sz < (long)sizeof h || !io.read(SNAP_PATH, 0, h, sizeof h)) return -1;
  const size_t es = snapEntrySize(h);
  if (!es) return -1;
  const int n = h[5];
  if (n > RING_SLOTS || n > max || sz != (long)(sizeof h + (size_t)n * es)) return -1;
  if (n && !io.read(SNAP_PATH, sizeof h, out, (size_t)n * es)) return -1;
  if (crc32(out, (size_t)n * es) != getU32(h + 12)) return -1;
  if (es != sizeof(SnapEntry))  // v7: spread the packed entries from the last one down (dest >= src), tails zeroed
    for (int i = n - 1; i >= 0; i--) {
      memmove((void*)&out[i], (const uint8_t*)out + (size_t)i * es, es);
      memset((uint8_t*)&out[i] + es, 0, sizeof(SnapEntry) - es);
    }
  for (int i = 0; i < n; i++) out[i].id[STATION_ID_LEN - 1] = 0;
  if (version) *version = h[4];
  return n;
}

bool snapFind(FileIo& io, const char* id, SnapEntry* out, uint8_t* version) {
  const long sz = io.size(SNAP_PATH);
  uint8_t h[SNAP_HEADER_SIZE];
  if (sz < (long)sizeof h || !io.read(SNAP_PATH, 0, h, sizeof h)) return false;
  const size_t es = snapEntrySize(h);  // 8, or 7 read into the prefix
  if (!es) return false;
  const int n = h[5];
  if (n > RING_SLOTS || sz != (long)(sizeof h + (size_t)n * es)) return false;
  uint32_t crc = 0;
  int found = -1;
  for (int i = 0; i < n; i++) {
    if (!io.read(SNAP_PATH, sizeof h + (size_t)i * es, out, es)) return false;
    crc = crc32(out, es, crc);
    out->id[STATION_ID_LEN - 1] = 0;
    if (strcmp(out->id, id) == 0) found = i;
  }
  if (found < 0 || crc != getU32(h + 12)) return false;
  if (!io.read(SNAP_PATH, sizeof h + (size_t)found * es, out, es)) return false;
  memset((uint8_t*)out + es, 0, sizeof *out - es);
  out->id[STATION_ID_LEN - 1] = 0;
  if (version) *version = h[4];
  return true;
}

int snapCarryParked(FileIo& io, SnapEntry* out, int n, int cap, const ParkedList& parked) {
  for (int p = 0; p < parked.n && n < cap; p++) {
    bool have = false;
    for (int i = 0; i < n && !have; i++) have = strcmp(out[i].id, parked.ids[p]) == 0;
    if (!have && snapFind(io, parked.ids[p], &out[n])) n++;  // an older version reads into the prefix (zero tail)
  }
  return n;
}

bool snapshotWorthWriting(const Station& st) {
  return st.uploads || (st.flash_state & FLASH_LIVE_RESTORED) || st.hourly.acc().hour;
}

void takeSnapEntry(const Station& st, SnapEntry* out) {
  memset((void*)out, 0, sizeof *out);  // padding included: the CRC covers the bytes
  memcpy(out->id, st.id, sizeof out->id);
  out->last_seen = st.last_seen;
  out->hour = st.hourly.acc();
  out->fstate = st.fstate;
  if (st.uploads || (st.flash_state & FLASH_LIVE_RESTORED)) {  // a reading to keep (an accepted one, or the restored)
    out->live.s = st.live;
    out->live.rejected = st.live_rejected;
    out->live.promoted_mask = st.promoted_mask;
    memcpy(out->live.promoted, st.promoted, sizeof out->live.promoted);
  }
  memcpy(out->known.field_t, st.field_t, sizeof out->known.field_t);
  memcpy(out->known.promo_t, st.promo_t, sizeof out->known.promo_t);
  memcpy(out->known.keys, st.known, sizeof out->known.keys);
  // the stale-sensor state (the stale_* tuning counters stay RAM)
  out->stale.sensor_init = st.sensor_init, out->stale.sensor_stale = st.sensor_stale;
  out->stale.sensor_absent = st.sensor_absent, out->stale.same_n = st.same_n;
  out->stale.stale_since = st.stale_since, out->stale.last_sensor_present = st.last_sensor_present;
  memcpy(out->stale.last_sensor, st.last_sensor, sizeof out->stale.last_sensor);
}

bool applySnapEntry(Station& st, const SnapEntry& e, uint32_t newest_flash_hour) {
  if (st.uploads) return false;  // a station that already uploaded keeps what it has
  st.last_seen = e.last_seen;
  st.fstate = e.fstate;
  memcpy(st.known, e.known.keys, sizeof st.known);
  // a frozen set stays frozen across the restart (a v7 entry: all zero = no stale state)
  st.sensor_init = e.stale.sensor_init != 0, st.sensor_stale = e.stale.sensor_stale != 0;
  st.sensor_absent = e.stale.sensor_absent != 0, st.same_n = e.stale.same_n;
  st.stale_since = e.stale.stale_since, st.last_sensor_present = e.stale.last_sensor_present;
  memcpy(st.last_sensor, e.stale.last_sensor, sizeof st.last_sensor);
  bool hour = false;
  // power loss rule: the snapshot's open hour is dropped when flash has that hour closed already (flash wins)
  if (e.hour.hour && e.hour.hour > newest_flash_hour && e.hour.n) {
    HourAcc a = e.hour;
    a.quality |= 1u << Q_RESTART;  // the bridge restarted inside this hour
    st.hourly.restoreAcc(a);
    hour = true;
  }
  if (e.live.s.present) {
    memcpy(st.field_t, e.known.field_t, sizeof st.field_t);
    // every field the console ever sent counts as seen, so the first announce after a restart
    // covers it under the current topics
    for (int f = 0; f < F_COUNT; f++)
      if (st.field_t[f]) st.seen_fields |= 1u << f;
    memcpy(st.promo_t, e.known.promo_t, sizeof st.promo_t);
    st.fresh = 0;  // a restored reading is not news: MQTT publishes nothing from it
    st.promo_fresh = 0;
    // the reading also opens the recent ring at its own (unix) time, so "Now" starts with it after a restart
    if (e.last_seen >= EPOCH_MIN && !st.recent.size()) st.recent.push(e.live.s, e.last_seen);
    st.live = e.live.s;
    // a value an older snapshot kept beyond the field's bounds (1e22) is dropped at the restore
    for (int f = 0; f < F_COUNT; f++)
      if (st.live.has(f) && !fieldInBounds(f, st.live.v[f])) st.live.present &= ~(1u << f);
    st.live_rejected = e.live.rejected;
    st.promoted_mask = e.live.promoted_mask;
    memcpy(st.promoted, e.live.promoted, sizeof st.promoted);
    st.flash_state |= FLASH_LIVE_RESTORED;
  }
  return hour;
}

int snapClampFuture(SnapEntry* e, uint32_t now) {
  int n = 0;
  auto clamp = [&](uint32_t& t) {
    if (t > now) t = now, n++;
  };
  clamp(e->last_seen);
  for (uint32_t& t : e->known.field_t) clamp(t);
  for (uint32_t& t : e->known.promo_t) clamp(t);
  for (KnownKey& k : e->known.keys) clamp(k.t);
  for (int f = 0; f < CORE_FIELDS; f++)
    if ((e->fstate.have >> f) & 1u) clamp(e->fstate.last_t[f]);
  clamp(e->stale.stale_since);
  return n;
}

uint32_t trustedHourStart(const FlashRing& hourly, int max_reads) {
  HourRec r;
  for (int i = hourly.count() - 1, n = 0; i >= 0 && n < max_reads; i--, n++)
    if (hourly.read(i, &r) && hourValid(r) && !(r.quality & (1u << Q_CLOCK_APPROX))) return r.hour;
  return 0;
}

}  // namespace fc
