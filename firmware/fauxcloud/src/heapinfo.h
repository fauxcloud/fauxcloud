// heapinfo.h -- one meaning for every heap figure and
// threshold, the byte-addressable heap (MALLOC_CAP_8BIT). ESP.getFreeHeap() and MALLOC_CAP_INTERNAL also count ~40-43 KB
// of IRAM that only 32-bit allocations can take (measured: INTERNAL free ~93.5 KB while 8BIT free was 50 240 B,
// largest 47 092 B). The sketch reads the IDF heap API in one place (app_core.cpp heapNow / heapSample); these are the
// constants and the pure predicates, host-tested. Plain C++, no IDF include.
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstdio>

#include "diag_bench.h"
#include "robust.h"

namespace fc {

// One reading. min8: the allocator's local minimum since heapSample last restarted its monitor (heapNow fills it;
// the monitor keeps running through a forwarding send).
struct HeapNow {
  uint32_t free8, largest8, min8;
};

// "Low memory" (diag heap_low, the notice, the LED, the admin callout): the minimum of min8 over the last
// HEAP_LOW_MINUTES below 18 KB.
// Derivation: >= the shed line 8 KB + one MQTT client re-create ~8 KB = 16 KB (below that a reconnect pushes the board
// into shedding); <= the worst normal minimum 24 KB (measured with forwarding: idle 50 KB - a 26 KB work dip) minus ~6 KB
// (one dashboard cold load). In INTERNAL terms: 60 KB - the 43 KB IRAM offset = ~17 KB, the same line.
constexpr uint32_t HEAP_LOW_FREE8 = 18 * 1024;
// A short window: the warning is state, so a dip that is over clears it within minutes (heap_min_10m keeps the history).
constexpr int HEAP_LOW_MINUTES = 3;
// The "tight" diagnosis log (every 10 s): starts before the alarm line. 80 KB INTERNAL - 43 KB = ~37 KB -> 2 x the alarm.
constexpr uint32_t HEAP_TIGHT_FREE8 = 2 * HEAP_LOW_FREE8, HEAP_TIGHT_LARGEST8 = 16384;
// OTA start guard: the write needs ~25 KB in all (an estimate: ~14 KB fixed -- 8 KB task stack, 4 KB sector, 1.4 KB chunk,
// the async request copy -- + lwIP / Wi-Fi RX; measured deltas of 18-35 KB from idle in INTERNAL terms, which
// transfer 1:1 to the byte heap) + ~3 KB for what runs meanwhile (page polls, a console upload reply). The 8 KB task stack
// is the largest single allocation: one 12 KB block. diag.ota_heap (start8 - min8) measures the real need.
constexpr uint32_t OTA_MIN_FREE8 = 28 * 1024, OTA_MIN_BLOCK = 12 * 1024;

static_assert(FragWatch::FRAG_LARGEST_MIN < SHED_LARGEST_MIN, "the watchdog acts below the shed line");
static_assert(HEAP_LOW_FREE8 >= SHED_LARGEST_MIN + 8192, "low memory warns before an MQTT re-create reaches shedding");
static_assert(OTA_MIN_FREE8 >= OTA_MIN_BLOCK + 13 * 1024, "the OTA guard covers the stack block and the rest of the write");
static_assert(HEAP_TIGHT_FREE8 > HEAP_LOW_FREE8, "the diagnosis log starts before the alarm");

inline bool heapLowBy(uint32_t min8) { return min8 < HEAP_LOW_FREE8; }
inline bool otaHeapOk(uint32_t free8, uint32_t largest8) { return free8 >= OTA_MIN_FREE8 && largest8 >= OTA_MIN_BLOCK; }
inline bool heapTight(uint32_t free8, uint32_t largest8) { return free8 < HEAP_TIGHT_FREE8 || largest8 < HEAP_TIGHT_LARGEST8; }

// The heap an OTA write really had (no serial log on the board): kept in RTC memory by the write, read at the next boot
// into diag.ota_heap. 16 B; lost on a power cut (then null).
struct OtaHeapRec {
  uint32_t magic, start8, min8, largest8_min;
};
constexpr uint32_t OTA_HEAP_MAGIC = 0x4f485031;  // "OHP1"
// One progress step of the write: the minimum of free8 and of largest8 so far
inline void otaHeapStep(OtaHeapRec* r, uint32_t free8, uint32_t largest8) {
  if (free8 < r->min8) r->min8 = free8;
  if (largest8 < r->largest8_min) r->largest8_min = largest8;
}

// (a ~50 KB one-second dip to 2 812 B outside any send was seen once): who was busy in the second the since-boot
// 8-bit minimum was reached. heapSample builds the mask once a second from counters and flags that already exist (no
// cost on a request), and keeps it, with the time and the open httpd sockets, only when the minimum is new.
enum HeapCtx : uint16_t {
  HC_HTTP = 1 << 0,    // a page request ended in that second or was being handled at the sample
  HC_ACCEPT = 1 << 1,  // httpd accepted a socket in that second (a client connecting; any interface)
  HC_FWD = 1 << 2,     // a forwarding send ran in that second
  HC_MQTT = 1 << 3,    // an MQTT publish in that second
  HC_STORE = 1 << 4,   // history writes were due in that second
  HC_LEARN = 1 << 5,   // a learner migration / backfill pass ran in that second
  HC_OTA = 1 << 6,     // an OTA write was running
  HC_UPLOAD = 1 << 7,  // a console upload arrived in that second
};
constexpr int HEAP_CTX_BITS = 8;
inline const char* heapCtxName(int bit) {
  static const char* const N[HEAP_CTX_BITS] = {"http", "accept", "fwd", "mqtt", "store", "learn", "ota", "upload"};
  return bit >= 0 && bit < HEAP_CTX_BITS ? N[bit] : "";
}

// The since-boot 8-bit minimum with its time (uptime s), context mask and open httpd sockets (-1 = unknown): a lower
// value replaces all four together, an equal or higher one leaves them.
struct HeapDip {
  uint32_t min8 = UINT32_MAX;
  uint32_t up_s = 0;
  uint16_t ctx = 0;
  int8_t socks = -1;
  bool have() const { return min8 != UINT32_MAX; }
  bool lower(uint32_t low8) const { return low8 < min8; }
  bool note(uint32_t low8, uint32_t up, uint16_t c, int s) {
    if (!lower(low8)) return false;
    min8 = low8;
    up_s = up;
    ctx = c;
    socks = (int8_t)(s < -1 ? -1 : s > 127 ? 127 : s);
    return true;
  }
};

// The context names comma-joined ("-" for none) into out (cap >= 44 holds all eight).
inline void heapCtxJoin(uint16_t ctx, char* out, size_t cap) {
  size_t n = 0;
  if (cap) out[0] = 0;
  for (int b = 0; b < HEAP_CTX_BITS; b++) {
    if (!(ctx & (1u << b))) continue;
    const int w = snprintf(out + n, cap > n ? cap - n : 0, "%s%s", n ? "," : "", heapCtxName(b));
    if (w < 0 || n + (size_t)w >= cap) break;
    n += (size_t)w;
  }
  if (!n && cap > 1) snprintf(out, cap, "-");
}

// The `[HEAP] low-water` log line's message (with the dip's context, so a burst that ends in a restart is
// still attributable from the RTC boot log). Its widest case stays inside the boot log's 200-B line (host-tested).
struct HeapLowLine {
  uint32_t min8, free8, largest8;
  int rssi, ap_clients;
  bool mqtt_up;
  uint32_t busy10m_ms;  // printed only at FC_DIAG_BENCH=1 (0 in the release)
  HeapDip dip;
};
inline int heapLowLine(char* out, size_t cap, const HeapLowLine& l) {
  char ctx[48];
  heapCtxJoin(l.dip.ctx, ctx, sizeof ctx);
#if FC_DIAG_BENCH
  return snprintf(out, cap, "low-water min8 %lu B (free8 %lu, largest8 %lu) rssi %d ap %d mqtt %s busy10m %lu ms ctx %s "
                  "socks %d", (unsigned long)l.min8, (unsigned long)l.free8, (unsigned long)l.largest8, l.rssi, l.ap_clients, l.mqtt_up ? "up" : "down",
                  (unsigned long)l.busy10m_ms, ctx, (int)l.dip.socks);
#else  // http_busy_ms_max is bench, and its busy10m figure leaves the release line with it
  return snprintf(out, cap, "low-water min8 %lu B (free8 %lu, largest8 %lu) rssi %d ap %d mqtt %s ctx %s socks %d",
                  (unsigned long)l.min8, (unsigned long)l.free8, (unsigned long)l.largest8, l.rssi, l.ap_clients,
                  l.mqtt_up ? "up" : "down", ctx, (int)l.dip.socks);
#endif
}

}  // namespace fc
