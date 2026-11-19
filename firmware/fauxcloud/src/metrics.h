// metrics.h -- small sliding windows and the ETag helper for the cache contract (docs/api.md §1.5). Plain C++.
#pragma once
#include <cstddef>
#include <cstdint>

namespace fc {

// Per-minute extreme over the last 10 minutes, read over all of them or the last few: the minimum free heap
// (heap_min_10m; heap_low reads a shorter span) and http_busy_ms_max (longest handler).
class MinuteWindow {
 public:
  static constexpr int MINUTES = 10;
  enum class Kind : uint8_t { Min, Max };
  explicit MinuteWindow(Kind k) : kind_(k) {}
  void feed(uint32_t now_ms, uint32_t v);
  // the extreme over the last `minutes` minutes (clamped to 1..MINUTES); false = no sample in them
  bool value(uint32_t now_ms, uint32_t* out, int minutes = MINUTES) const;

 private:
  Kind kind_;
  uint32_t minute_[MINUTES] = {};
  uint32_t v_[MINUTES] = {};
  bool used_[MINUTES] = {};
};

// Weak ETag text for counters: "\"<boot>-<a>-<b>-<c>\"". `boot` is random per boot, so a browser's ETag from the
// previous boot (counters restart at zero) never matches.
void makeEtag(char out[48], uint32_t boot, uint32_t a, uint32_t b, uint32_t c);
// Whether an If-None-Match header value matches `etag` (exact, or one of a comma list, or "*").
bool etagMatches(const char* if_none_match, const char* etag);

}  // namespace fc
