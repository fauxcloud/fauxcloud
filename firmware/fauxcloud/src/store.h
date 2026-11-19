// store.h -- everything fauxcloud keeps on flash besides NVS: the history rings of up to RING_SLOTS
// stations (ring.h), the one shared snapshot of the open hours and last readings, and the layout marker. The file
// system sits behind FileIo: LittleFS on the ESP (app_store.cpp), a RAM double in the host tests. Plain C++,
// host-compilable; not thread-safe (the caller serializes).
//
// /fc7      marker (8 B): "FC7M" + the layout fingerprint; written last by the format, so a half-built layout formats
//           again. Another fingerprint = another layout: never formatted by this build (formatPlan), history RAM only.
// /s        shared snapshot (one file for every station), through /s.tmp + rename: on an hour / day close, every 10 min while
//           uploads arrive, before a planned restart. Header (16 B): "FCS7", version, entries, entry size, layout
//           fingerprint, CRC-32 of the entries; then one SnapEntry per station that owns a ring (<= 1 block).
#pragma once
#include <cstddef>
#include <cstdint>

#include "fileio.h"
#include "filter.h"
#include "history.h"
#include "ring.h"
#include "tz.h"

namespace fc {

constexpr const char* MARKER_PATH = "/fc7";
constexpr const char* SNAP_PATH = "/s";
constexpr const char* SNAP_TMP_PATH = "/s.tmp";

// The marker and the ring headers carry the ring fingerprint -- the record sizes and offsets and the ring
// geometry only -- so a new WU field or a bigger snapshot entry never makes the rings foreign. The snapshot carries the
// full one in its own header (a mismatch drops only the snapshot).
uint32_t ringFingerprint();
uint32_t snapFingerprint();
MarkerState markerRead(FileIo& io);
// After a format: every ring's header (free) and its empty segment files, the marker last
bool buildLayout(FileIo& io);

// The last accepted reading (after a restart the dashboard loads with data before any new upload).
struct SnapLive {
  Sample s;
  uint32_t rejected;
  uint8_t promoted_mask;
  float promoted[MAX_PROMOTE];
};
// when each field and promotion last arrived, and the unknown keys ever received
struct SnapKnown {
  uint32_t field_t[F_COUNT];
  uint32_t promo_t[MAX_PROMOTE];
  KnownKey keys[MAX_KNOWN];
};
// One station in the shared snapshot, version 7: the open hour, the filter state (rain baseline), the
// last reading. Kept verbatim: a v7 file is read and migrated (additive, never dropped).
struct SnapEntryV7 {
  char id[STATION_ID_LEN];
  uint32_t last_seen;  // last upload, ESP clock
  HourAcc hour;
  FilterState fstate;
  SnapLive live;
  SnapKnown known;
};
// the stale-sensor state, so a frozen set stays frozen after a reboot
struct SnapStale {
  uint8_t sensor_init, sensor_stale, sensor_absent, same_n;
  uint32_t stale_since, last_sensor_present;
  float last_sensor[CORE_FIELDS];
};
// SnapEntryV7's tail padding (it ends on an 8-byte boundary: HourAcc holds doubles), kept as bytes in version 8
constexpr size_t SNAP_V7_TAIL = sizeof(SnapEntryV7) - (offsetof(SnapEntryV7, known) + sizeof(SnapKnown));
// Version 8 = version 7 + SnapStale at the end. A v7 entry reads into the prefix with a zero tail (no stale state).
struct SnapEntry {
  char id[STATION_ID_LEN];
  uint32_t last_seen;
  HourAcc hour;
  FilterState fstate;
  SnapLive live;
  SnapKnown known;
  uint8_t v7_tail[SNAP_V7_TAIL ? SNAP_V7_TAIL : 1];  // zero (the assert below catches a layout without a tail)
  SnapStale stale;
};
static_assert(offsetof(SnapEntry, stale) == sizeof(SnapEntryV7) && offsetof(SnapEntry, known) == offsetof(SnapEntryV7, known) &&
                  offsetof(SnapEntry, live) == offsetof(SnapEntryV7, live) && offsetof(SnapEntry, hour) == offsetof(SnapEntryV7, hour) &&
                  offsetof(SnapEntry, fstate) == offsetof(SnapEntryV7, fstate),
              "version 8 is version 7 + SnapStale at the end");
constexpr size_t SNAP_HEADER_SIZE = 16;
static_assert(SNAP_HEADER_SIZE + RING_SLOTS * sizeof(SnapEntry) <= 4096,
              "the shared snapshot fits one 4 KB block");

bool snapWrite(FileIo& io, const SnapEntry* entries, int n);  // always the current version (8)
// The entries (at most max) of a valid snapshot; 0 when missing, -1 when unreadable / another layout. A
// version 7 file is read too (each entry migrated: v7 prefix, zero tail); *version = the version read.
int snapRead(FileIo& io, SnapEntry* out, int max, uint8_t* version = nullptr);
// One station's entry, read one entry at a time (one SnapEntry of RAM); false when missing, unreadable or not there.
bool snapFind(FileIo& io, const char* id, SnapEntry* out, uint8_t* version = nullptr);
// adopt (adopt.h): the snapshot entry of `from` renamed to `to` (the open hour, the filter state and the last reading carry
// on under the new id). 1 = renamed, 0 = no such entry (or no snapshot), -1 = the rewrite failed. Uses `scratch` (at
// least RING_SLOTS entries) for the read-modify-write.
int snapRename(FileIo& io, const char* from, const char* to, SnapEntry* scratch);
// before a rewrite, the current file's entries of parked stations (stored, no slot on this profile)
// are appended to out[n..cap) untouched, so a parked station's open hour and last reading survive (never erased).
// Returns the new count. An id already in out[] is not carried twice.
struct ParkedList;  // station.h
int snapCarryParked(FileIo& io, SnapEntry* out, int n, int cap, const ParkedList& parked);
uint32_t snapFingerprintV7();  // the fingerprint a version 7 snapshot carries

struct Station;
void takeSnapEntry(const Station& st, SnapEntry* out);
// false for a station with nothing to save (no upload since boot, nothing restored, nothing open)
bool snapshotWorthWriting(const Station& st);
// Back into a station at its allocation: the open hour (dropped when flash already has that hour closed: flash wins),
// the filter state, the last reading and the known keys. Nothing is overwritten once the station had an upload.
// Returns false when the open hour was dropped or there was none.
bool applySnapEntry(Station& st, const SnapEntry& e, uint32_t newest_flash_hour);
// the times of an entry later than `now` (last upload, field / promotion / key arrivals, the
// filter's last times, stale_since) clamped to now after a clock acceptance; returns how many changed
int snapClampFuture(SnapEntry* e, uint32_t now);

// Between hour closes the snapshot is rewritten every 10 min while uploads arrive (only when there was an upload
// since the last write), so the last reading on flash is at most 10 min old after a power cut. Hour / day closes
// write as well and restart the 10 min.
class SnapTimer {
 public:
  static constexpr uint32_t PERIOD_MS = 10UL * 60 * 1000;
  bool due(uint32_t now_ms, uint32_t uploads) const { return uploads != uploads_ && now_ms - at_ms_ >= PERIOD_MS; }
  void written(uint32_t now_ms, uint32_t uploads) {
    at_ms_ = now_ms;
    uploads_ = uploads;
  }

 private:
  uint32_t at_ms_ = 0, uploads_ = 0;
};

uint32_t crc32(const void* data, size_t len, uint32_t crc = 0);

// the wait before retrying a station's failed ring append, by failures in a row (1 = the first):
// 60, 120, 240, 480, then 600 s
inline uint32_t storeRetryS(uint8_t fails) {
  if (fails <= 1) return 60;
  const uint32_t s = fails >= 5 ? 600u : 60u << (fails - 1);
  return s > 600 ? 600 : s;
}

// the key (the hour's START) of the newest hourly record without
// Q_CLOCK_APPROX, walking back from the newest end of an hourly ring (bad CRC skipped; at most max_reads reads, so a
// ring whose newest >= 256 hours are all approximate reports 0); 0 = none.
uint32_t trustedHourStart(const FlashRing& hourly, int max_reads = 256);
// The same for one record / an open hour (0 when approximate or empty).
inline uint32_t trustedStart(uint32_t hour, uint16_t quality) {
  return hour && !(quality & (1u << Q_CLOCK_APPROX)) ? hour : 0;
}
// what the 24-h acceptance keeps -- every hourly key up to `hour` and every day up to `ymd`. The
// open hour and the open (today's) day belong to RAM and come again from the accepted clock, so they and everything
// later are cut: the next real close is then newer than the ring's newest.
struct AcceptLimits {
  uint32_t hour, ymd;
};
inline AcceptLimits clockAcceptLimits(uint32_t now, const PosixTz& tz) {
  return AcceptLimits{now - now % 3600 - 1, dayNumberToYmd(localDayNumber(tz, (int64_t)now) - 1)};
}

}  // namespace fc
