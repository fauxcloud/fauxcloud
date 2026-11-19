// ring.cpp -- see ring.h.
#include "ring.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace fc {

static uint32_t keyOf(const void* rec) {
  uint32_t k;
  memcpy(&k, rec, sizeof k);  // little-endian on the ESP32 and the host
  return k;
}

void ringSegPath(char out[RING_PATH_MAX], const RingSpec& s, int seg) {
  snprintf(out, RING_PATH_MAX, "/%c%u_%02d", s.kind, (unsigned)s.k, seg);
}
void ringHeaderPath(char out[RING_PATH_MAX], const RingSpec& s) {
  snprintf(out, RING_PATH_MAX, "/%c%u.hdr", s.kind, (unsigned)s.k);
}

void FlashRing::init(FileIo* io, const RingSpec& s) {
  io_ = io;
  s_ = s;
  memset(cnt_, 0, sizeof cnt_);
  memset(torn_, 0, sizeof torn_);
  memset(first_, 0, sizeof first_);
  cur_ = -1;
  total_ = 0;
  newest_ = 0;
}

bool FlashRing::keyQualAt(int seg, int i, uint32_t* key, uint16_t* q) const {
  char p[RING_PATH_MAX];
  ringSegPath(p, s_, seg);
  uint8_t b[6];
  if (!io_->read(p, (size_t)i * s_.rec, b, 6)) return false;
  *key = keyOf(b);
  *q = (uint16_t)(b[4] | (b[5] << 8));
  return true;
}

bool FlashRing::keyAt(int seg, int i, uint32_t* key) const {
  char p[RING_PATH_MAX];
  ringSegPath(p, s_, seg);
  uint8_t b[4];
  if (!io_->read(p, (size_t)i * s_.rec, b, 4)) return false;
  *key = keyOf(b);
  return true;
}

int FlashRing::scan() {
  cur_ = -1;
  total_ = 0;
  newest_ = 0;
  for (int seg = 0; seg < s_.segs; seg++) {
    char p[RING_PATH_MAX];
    ringSegPath(p, s_, seg);
    const long sz = io_->size(p);
    int n = sz > 0 ? (int)(sz / s_.rec) : 0;
    if (n > s_.per_seg) n = s_.per_seg;  // never more than a block's worth
    torn_[seg] = sz > 0 && sz % s_.rec != 0;
    cnt_[seg] = (uint8_t)n;
    first_[seg] = 0;
    if (n && !keyAt(seg, 0, &first_[seg])) cnt_[seg] = 0;
    if (cnt_[seg] && (cur_ < 0 || first_[seg] > first_[cur_])) cur_ = seg;
    total_ += cnt_[seg];
  }
  if (cur_ >= 0 && !keyAt(cur_, cnt_[cur_] - 1, &newest_)) newest_ = first_[cur_];
  return total_;
}

uint32_t FlashRing::oldestKey() const {
  for (int j = 0; j < s_.segs; j++)
    if (cnt_[segAt(j)]) return first_[segAt(j)];
  return 0;
}

FlashRing::Add FlashRing::append(const void* rec) {
  const uint32_t key = keyOf(rec);
  char p[RING_PATH_MAX];
  if (cur_ >= 0 && key < newest_) {
    dropped_++;
    return Add::Older;
  }
  if (cur_ >= 0 && key == newest_) {  // the same hour / day again: the last record is replaced
    ringSegPath(p, s_, cur_);
    return io_->writeAt(p, (size_t)(cnt_[cur_] - 1) * s_.rec, rec, s_.rec) ? Add::Replaced : Add::Failed;
  }
  if (cur_ >= 0 && cnt_[cur_] < s_.per_seg) {
    ringSegPath(p, s_, cur_);
    bool ok;
    if (torn_[cur_]) {  // a power cut left a partial record: whole records + this one, in one commit
      const size_t whole = (size_t)cnt_[cur_] * s_.rec;
      uint8_t* buf = whole ? (uint8_t*)malloc(whole) : nullptr;  // rare: borrowed for this one write
      const FileIo::Part parts[] = {{buf, whole}, {rec, s_.rec}};
      ok = (!whole || (buf && io_->read(p, 0, buf, whole))) && io_->writeParts(p, parts, 2);
      free(buf);
      if (ok) {
        torn_[cur_] = false;
        cut_++;
      }
    } else {
      ok = io_->append(p, rec, s_.rec);
    }
    if (!ok) return Add::Failed;
    cnt_[cur_]++;
    total_++;
    newest_ = key;
    return Add::Appended;
  }
  // empty ring, or the current segment full: the next index starts over with this record
  const int next = cur_ < 0 ? 0 : (cur_ + 1) % s_.segs;
  ringSegPath(p, s_, next);
  if (!io_->write(p, rec, s_.rec)) return Add::Failed;
  total_ += 1 - cnt_[next];
  cnt_[next] = 1;
  torn_[next] = false;
  first_[next] = key;
  cur_ = next;
  newest_ = key;
  return Add::Rotated;
}

bool FlashRing::read(int i, void* rec) const {
  if (i < 0 || i >= total_) return false;
  for (int j = 0; j < s_.segs; j++) {
    const int seg = segAt(j);
    if (i < cnt_[seg]) {
      char p[RING_PATH_MAX];
      ringSegPath(p, s_, seg);
      return io_->read(p, (size_t)i * s_.rec, rec, s_.rec);
    }
    i -= cnt_[seg];
  }
  return false;
}

int FlashRing::readRun(int i, void* buf, int max) const {
  if (i < 0 || i >= total_ || max <= 0) return 0;
  for (int j = 0; j < s_.segs; j++) {
    const int seg = segAt(j);
    if (i < cnt_[seg]) {
      const int n = cnt_[seg] - i < max ? cnt_[seg] - i : max;
      char p[RING_PATH_MAX];
      ringSegPath(p, s_, seg);
      return io_->read(p, (size_t)i * s_.rec, buf, (size_t)n * s_.rec) ? n : 0;
    }
    i -= cnt_[seg];
  }
  return 0;
}

int FlashRing::lowerBound(uint32_t key) const {
  if (!total_ || key > newest_) return total_;
  // the segments in order from the oldest: the last one whose first key is < key holds the boundary
  int base = 0, segIdx = -1, segBase = 0;
  for (int j = 0; j < s_.segs; j++) {
    const int seg = segAt(j);
    if (!cnt_[seg]) continue;
    if (first_[seg] >= key) break;
    segIdx = seg;
    segBase = base;
    base += cnt_[seg];
  }
  if (segIdx < 0) return 0;  // the oldest record is already >= key
  int lo = 1, hi = cnt_[segIdx];  // record 0 is < key; find the first >= key in [1, cnt]
  while (lo < hi) {
    const int mid = (lo + hi) / 2;
    uint32_t k;
    if (!keyAt(segIdx, mid, &k)) return segBase + mid;  // unreadable: stop there
    if (k < key) lo = mid + 1;
    else hi = mid;
  }
  return segBase + lo;
}

int FlashRing::dropFuture(uint32_t limit, bool approx_only, uint16_t approx_bit) {
  int dropped = 0;
  while (cur_ >= 0 && newest_ > limit) {
    const int seg = cur_;
    char p[RING_PATH_MAX];
    ringSegPath(p, s_, seg);
    // from the newest end: keys rise inside a segment, so the records past the limit are its tail
    int keep = cnt_[seg];
    bool stop = false;
    while (keep > 0) {
      uint32_t k;
      uint16_t q;
      if (!keyQualAt(seg, keep - 1, &k, &q)) {  // unreadable -> no cut from here
        stop = true;
        break;
      }
      if (k <= limit) break;
      if (approx_only && !(q & approx_bit)) {  // a trusted record past the limit: kept, the cut ends here
        stop = true;
        if (k != kept_key_) kept_++, kept_key_ = k;
        break;
      }
      keep--;
    }
    if (keep == cnt_[seg]) break;  // nothing to cut in this segment
    bool ok;
    if (!keep) {
      ok = io_->write(p, nullptr, 0);
    } else {
      const size_t len = (size_t)keep * s_.rec;
      uint8_t* buf = (uint8_t*)malloc(len);  // rare: borrowed for this one rewrite
      ok = buf && io_->read(p, 0, buf, len) && io_->write(p, buf, len);
      free(buf);
    }
    if (!ok) break;
    dropped += cnt_[seg] - keep;
    scan();  // the newest segment and key again
    if (stop || (cur_ == seg && keep == cnt_[seg])) break;
  }
  future_ += (uint32_t)dropped;
  return dropped;
}

bool FlashRing::forget() {
  bool ok = true;
  for (int seg = 0; seg < s_.segs; seg++) {
    char p[RING_PATH_MAX];
    ringSegPath(p, s_, seg);
    if (io_->size(p) > 0) ok = io_->write(p, nullptr, 0) && ok;
    cnt_[seg] = 0;
    torn_[seg] = false;
    first_[seg] = 0;
  }
  cur_ = -1;
  total_ = 0;
  newest_ = 0;
  return ok;
}

// ---------------------------------------------------------------- header, format plan

static const char RING_MAGIC[4] = {'F', 'C', 'R', '1'};
static const uint8_t RING_VERSION = 1;

bool ringHeaderRead(FileIo& io, const RingSpec& s, uint32_t fingerprint, RingHeader* out) {
  char p[RING_PATH_MAX];
  ringHeaderPath(p, s);
  if (io.size(p) != (long)sizeof *out || !io.read(p, 0, out, sizeof *out)) return false;
  out->id[sizeof out->id - 1] = 0;
  return memcmp(out->magic, RING_MAGIC, 4) == 0 && out->version == RING_VERSION && out->rec == s.rec &&
         out->segs == s.segs && out->per_seg == s.per_seg && out->fingerprint == fingerprint;
}

bool ringHeaderWrite(FileIo& io, const RingSpec& s, uint32_t fingerprint, const char* id) {
  RingHeader h;
  memset(&h, 0, sizeof h);
  memcpy(h.magic, RING_MAGIC, 4);
  h.version = RING_VERSION;
  h.rec = s.rec;
  h.segs = s.segs;
  h.per_seg = s.per_seg;
  h.fingerprint = fingerprint;
  snprintf(h.id, sizeof h.id, "%s", id ? id : "");
  char p[RING_PATH_MAX];
  ringHeaderPath(p, s);
  return io.write(p, &h, sizeof h);
}

bool ringForgetOwner(FileIo& io, uint32_t fingerprint, const char* id, FlashRing* hour, FlashRing* day) {
  RingHeader h;
  if (!id || !id[0] || !ringHeaderRead(io, hour->spec(), fingerprint, &h) || strcmp(h.id, id) != 0) return false;
  hour->forget();
  day->forget();
  const bool a = ringHeaderWrite(io, hour->spec(), fingerprint, "");
  const bool b = ringHeaderWrite(io, day->spec(), fingerprint, "");
  return a && b;
}

int ringRenameOwner(FileIo& io, uint32_t fingerprint, int k, const char* from, const char* to) {
  if (!from || !from[0] || !to || !to[0]) return 0;
  int n = 0;
  const RingSpec specs[2] = {hourSpec(k), daySpec(k)};
  for (const RingSpec& s : specs) {
    RingHeader h;
    if (!ringHeaderRead(io, s, fingerprint, &h) || strcmp(h.id, from) != 0) continue;
    if (!ringHeaderWrite(io, s, fingerprint, to)) return -1;
    n++;
  }
  return n;
}

FormatPlan formatPlan(MarkerState m, bool image_pending) {
  switch (m) {
    case MarkerState::Valid: return FormatPlan::Use;
    case MarkerState::Foreign: return FormatPlan::RamOnlyForeign;
    case MarkerState::Missing: break;
  }
  return image_pending ? FormatPlan::WaitValid : FormatPlan::Format;
}

static bool digits(const char* s, int n) {
  for (int i = 0; i < n; i++)
    if (s[i] < '0' || s[i] > '9') return false;
  return s[n] == 0;
}

bool b7Name(const char* name) {
  if (!name) return false;
  if (!strcmp(name, "s") || !strcmp(name, "s.tmp") || !strcmp(name, "fc7")) return true;
  if ((name[0] != 'h' && name[0] != 'd') || name[1] < '0' || name[1] >= '0' + RING_SLOTS) return false;  // the profile's rings
  if (!strcmp(name + 2, ".hdr")) return true;
  if (name[2] != '_' || !digits(name + 3, 2)) return false;
  const int seg = (name[3] - '0') * 10 + (name[4] - '0'), k = name[1] - '0';
  return seg < (name[0] == 'h' ? HOUR_SEGS_K[k] : DAY_SEGS_K[k]);
}

}  // namespace fc
