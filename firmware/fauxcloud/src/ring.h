// ring.h -- a fixed history ring on flash (layout chosen on a littlefs bench): S one-block segment files
// of R fixed-size records each, keyed by the record's first u32 (an hour start, a YYYYMMDD date). Append = open "a",
// one record, close; a full segment rotates into the next index mod S, rewritten with truncate + the first record
// (inline while <= 512 B: no erase). Keys strictly increase. Plain C++ over FileIo, host-compilable; not thread-safe.
//
// Files (flat root, no subdirectories: each littlefs directory costs a 2-block metadata pair):
//   /h<k>_00 .. /h<k>_nn  hourly ring of station ring k: ring 0 37 x 85 x 48 B = 3 145 h ~ 131 days (PSRAM: ring 1 25 = 88 d)
//   /d<k>_00 .. /d<k>_nn  daily ring: ring 0 16 x 46 x 88 B = 736 days (PSRAM: ring 1 8 = 368 days)
//   /h<k>.hdr /d<k>.hdr   ring header: which station owns it (empty id = free)
// The rings per kind are the board profile's: WROOM-32 one ring on 70 blocks (LittleFS 280 KB), the
// PSRAM profile two on 104 blocks (416 KB, partitions-psram.csv).
#pragma once
#include <cstddef>
#include <cstdint>

#include "fileio.h"
#include "profile.h"
#include "wu_parse.h"

namespace fc {

#if FC_PROFILE_PSRAM
constexpr int RING_SLOTS = 2;  // rings per kind: the profile's; never the station count
#else
constexpr int RING_SLOTS = 1;
#endif
constexpr int RING_SEG_MAX = 37;  // the largest ring's segment count (ring 0's hourly on every profile)
constexpr int RING_REC_MAX = 88;
constexpr size_t RING_PATH_MAX = 12;

struct RingSpec {
  char kind;        // 'h' hourly, 'd' daily
  uint8_t k;        // ring slot 0 / 1
  uint8_t segs;     // S
  uint8_t per_seg;  // R records per segment (one 4 KB block)
  uint8_t rec;      // record size
};
// The geometry per ring: ring 0 keeps 131 days hourly + 2 years daily on every profile; the PSRAM profile's ring 1 gets
// 88 days + 1 year. The partition table the image expects (app_store.cpp tableOk; partitions*.csv, tools/check_partitions.py):
// WROOM-32: app slots 0x1D0000, coredump 40 KB, LittleFS 0x3BA000 / 280 KB = 70 blocks (year bench
// min free 9); PSRAM: app slots 0x1C0000, coredump 32 KB, LittleFS 0x398000 / 416 KB = 104 blocks (min free 10).
constexpr int HOUR_PER_SEG = 85, DAY_PER_SEG = 46;
#if FC_PROFILE_PSRAM
constexpr int HOUR_SEGS_K[RING_SLOTS] = {37, 25}, DAY_SEGS_K[RING_SLOTS] = {16, 8};
constexpr int LFS_BLOCKS = 104;
constexpr uint32_t TABLE_APP = 0x1C0000, TABLE_LFS_OFF = 0x398000;
#else
constexpr int HOUR_SEGS_K[RING_SLOTS] = {37}, DAY_SEGS_K[RING_SLOTS] = {16};
constexpr int LFS_BLOCKS = 70;
constexpr uint32_t TABLE_APP = 0x1D0000, TABLE_LFS_OFF = 0x3BA000;
#endif
constexpr uint32_t TABLE_LFS = (uint32_t)LFS_BLOCKS * 4096;
constexpr int HOUR_SEGS = 37, DAY_SEGS = 16;  // the largest ring's (paths, request caps): ring 0's
constexpr int ringBlocks() {
  int n = 0;
  for (int k = 0; k < RING_SLOTS; k++) n += HOUR_SEGS_K[k] + DAY_SEGS_K[k];
  return n;
}
// the segments + the snapshot + <= 4 metadata blocks + 8 free fit (a gross-edit check; the year bench is the real gate)
static_assert(ringBlocks() + 1 + 4 + 8 <= LFS_BLOCKS, "the rings do not fit the LittleFS partition (ring.h)");
static_assert(HOUR_SEGS_K[0] == HOUR_SEGS && DAY_SEGS_K[0] == DAY_SEGS && HOUR_SEGS <= RING_SEG_MAX, "ring 0 is the largest");
inline int ringSlot(int k) { return k >= 0 && k < RING_SLOTS ? k : 0; }  // a bounds guard (no ring k beyond the profile)
inline RingSpec hourSpec(int k) { return RingSpec{'h', (uint8_t)ringSlot(k), (uint8_t)HOUR_SEGS_K[ringSlot(k)], HOUR_PER_SEG, 48}; }
inline RingSpec daySpec(int k) { return RingSpec{'d', (uint8_t)ringSlot(k), (uint8_t)DAY_SEGS_K[ringSlot(k)], DAY_PER_SEG, 88}; }
// The partition table check as a pure function (the device reads the running app slot and the littlefs partition)
inline bool tableMatches(uint32_t lfs_off, uint32_t lfs_size, uint32_t app_size) {
  return lfs_off == TABLE_LFS_OFF && lfs_size == TABLE_LFS && app_size == TABLE_APP;
}

void ringSegPath(char out[RING_PATH_MAX], const RingSpec& s, int seg);  // "/h0_07"
void ringHeaderPath(char out[RING_PATH_MAX], const RingSpec& s);       // "/h0.hdr"

class FlashRing {
 public:
  void init(FileIo* io, const RingSpec& s);
  // Reads every segment's size and first key (one small read per non-empty segment) and the newest key; a size not
  // a multiple of R counts whole records only (the cut happens at the next append into that segment). Returns the
  // records in the ring.
  int scan();
  int count() const { return total_; }
  uint32_t newestKey() const { return newest_; }
  uint32_t oldestKey() const;  // 0 when empty
  enum class Add : uint8_t { Appended, Rotated, Replaced, Older, Failed };
  // The record's key is its first u32. Equal to the newest -> that record is replaced (debug roll); older -> dropped
  // and counted; a full current segment -> the next one is rewritten with this record first (the oldest R go).
  Add append(const void* rec);
  bool read(int i, void* rec) const;  // record i, 0 = oldest; raw bytes (the caller checks the CRC)
  // up to `max` consecutive records from record i, all in i's segment, in ONE file read (one open);
  // returns how many (0 = none / the read failed). Raw bytes, the caller checks each CRC.
  int readRun(int i, void* buf, int max) const;
  // Index of the first record with key >= `key` (count() when none): a binary search over the segments' first keys,
  // then inside one segment (a few 4-byte reads).
  int lowerBound(uint32_t key) const;
  bool forget();  // every segment truncated to 0 (no erase), the ring empty
  // records with a key > limit (a clock that went back after a wrong-forward seed) are cut from the
  // newest end, segment by segment (one rewrite each); returns the records dropped.
  // With `approx_only` (a clock behind the stored history is held, never trusted against it) only records whose
  // quality (u16 at offset 4, HourRec and DayRec alike) carries `approx_bit` are cut: the cut stops at the first record past the limit without it (keys rise, so
  // what goes is a tail of approximate records); that stop is counted once per newest key (futureKept). A
  // record that cannot be read stops the cut without writing.
  int dropFuture(uint32_t limit, bool approx_only = true, uint16_t approx_bit = 1u << 2);
  uint32_t dropped() const { return dropped_; }  // records refused for an older key since boot
  uint32_t cut() const { return cut_; }          // torn segments cut to whole records since boot
  uint32_t futureDropped() const { return future_; }  // records cut by dropFuture since boot
  uint32_t futureKept() const { return kept_; }       // cuts stopped at a non-approximate record since boot
  const RingSpec& spec() const { return s_; }

 private:
  int segAt(int j) const { return (cur_ + 1 + j) % s_.segs; }  // j-th segment from the oldest end
  bool keyAt(int seg, int i, uint32_t* key) const;
  bool keyQualAt(int seg, int i, uint32_t* key, uint16_t* q) const;  // key + quality (6 bytes)
  FileIo* io_ = nullptr;
  RingSpec s_ = {};
  uint8_t cnt_[RING_SEG_MAX] = {};
  bool torn_[RING_SEG_MAX] = {};
  uint32_t first_[RING_SEG_MAX] = {};
  int cur_ = -1;  // the segment with the newest key, -1 = empty ring
  int total_ = 0;
  uint32_t newest_ = 0, dropped_ = 0, cut_ = 0, future_ = 0, kept_ = 0, kept_key_ = 0;
};

// Ring header (inline file, 28 B): "FCR1", version, record size, segments, records per segment, layout fingerprint,
// the owning station id ("" = a free ring).
struct RingHeader {
  char magic[4];
  uint8_t version, rec, segs, per_seg;
  uint32_t fingerprint;
  char id[STATION_ID_LEN];
};
static_assert(sizeof(RingHeader) == 28, "ring header layout");
bool ringHeaderRead(FileIo& io, const RingSpec& s, uint32_t fingerprint, RingHeader* out);  // false: missing/foreign
bool ringHeaderWrite(FileIo& io, const RingSpec& s, uint32_t fingerprint, const char* id);
// frees ring slot `hour.spec().k` when its hourly header names `id` (a station with or without a RAM slot):
// both rings emptied (truncate, no erase), both headers written free. False (nothing touched) for another owner.
bool ringForgetOwner(FileIo& io, uint32_t fingerprint, const char* id, FlashRing* hour, FlashRing* day);
// adopt (adopt.h): ring k's hourly and daily headers that still name `from` now name `to` (records carry no id: nothing else
// moves). Idempotent: a header already naming `to` (or anyone else) is left alone. Returns the headers rewritten, -1 on a
// failed write.
int ringRenameOwner(FileIo& io, uint32_t fingerprint, int k, const char* from, const char* to);

// what the store does with the partition at boot. Marker missing + image Pending (OTA rollback armed) -> RAM only, no
// flash access until the image is marked valid; missing + valid -> format once and build the layout; another layout's
// marker -> never format, RAM only; ours -> use it (and remove root entries outside this layout's names).
enum class MarkerState : uint8_t { Missing, Foreign, Valid };
enum class FormatPlan : uint8_t { WaitValid, Format, RamOnlyForeign, Use };
FormatPlan formatPlan(MarkerState m, bool image_pending);
// A root entry (name without '/') that belongs to this layout: ring segments and headers of slots 0-1, the shared
// snapshot "s" (+ "s.tmp") and the marker "fc7". Anything else is a leftover (an older layout's "d" directory, .old
// files ...).
bool b7Name(const char* name);

}  // namespace fc
