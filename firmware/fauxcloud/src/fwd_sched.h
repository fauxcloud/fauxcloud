// fwd_sched.h -- the forwarding scheduler: a pure state machine, one tick a second from the loop,
// a fake clock on the host. At most one Start per tick, never two sends at once; a target sends only inside its own
// window [due, due + (max_s - min_s)] and only while the core is idle; a window that ends without a start is skipped
// (nothing queued, no catch-up burst); two retries (30 s, 120 s) on `fail` only, dropped when they would cross the
// next due time; three refused keys (or, FWD_TLS, three no_memory results) in a row pause the target until its settings
// change or "Send one now" is accepted.
#pragma once
#include <cstdint>

#include "fwd.h"
#include "robust.h"
#include "tlsc.h"

namespace fc {

struct NqCand;  // netq.h
struct NqPick;

// ---------------------------------------------------------------- the busy table: every bit must be clear
enum FwdBusy : uint16_t {
  FB_UPLOAD = 1u << 0,   // an upload queued or processed < 3 s ago
  FB_HTTP = 1u << 1,     // a request in flight, the last page request < 3 s ago, or http_shed rose in the last 60 s
  FB_STORE = 1u << 2,    // history writes due
  FB_LEARN = 1u << 3,    // a learner backfill
  FB_HOUR = 1u << 4,     // 45 s before a UTC hour boundary / local midnight until the close is written after it
  FB_MQTT = 1u << 5,     // HA discovery / retraction pending, or the publish stall
  FB_RESTART = 1u << 6,  // a planned reboot, a factory reset, an OTA, setup mode
  FB_LINK = 1u << 7,     // no home Wi-Fi, or no real time
  FB_CAPTURE = 1u << 8,  // the bridge listens to a new console: nothing goes online
};
constexpr uint16_t FB_ALL = 0x1FF;
constexpr uint16_t FB_TEST = FB_UPLOAD | FB_RESTART | FB_LINK | FB_CAPTURE;  // "Send one now" obeys only these (+ the heap floor)
const char* fwdBusyName(uint16_t mask);  // the first set bit: upload http store learn hour mqtt restart link capture
// FB_HTTP (one place for the rule, forwarding and METAR alike): a request in flight, the last page request
// less than FWD_QUIET_MS ago (0 = none yet), or http_shed rose less than FWD_SHED_MS ago (0 = never)
bool fwdHttpBusy(uint32_t now_ms, int in_flight, uint32_t last_page_ms, uint32_t shed_ms);

// Resource floors, on the byte-addressable heap (MALLOC_CAP_8BIT free / largest block; ESP.getFreeHeap() counts
// INTERNAL, ~40 KB of it IRAM that no buffer can use). The one place they live.
// Plain HTTP: a send costs ~1.5 KB (measured: 49.5 -> 48.2 KB). Abort: the httpd's own shed line for the largest block.
constexpr uint32_t FWD_PLAIN_FREE_MIN = 16u * 1024, FWD_PLAIN_LARGEST_MIN = 8u * 1024;
constexpr uint32_t FWD_ABORT_FREE_MIN = 8u * 1024, FWD_ABORT_LARGEST_MIN = SHED_LARGEST_MIN;
// TLS (FWD_TLS=1, BearSSL): a job starts only when free8 >= P[class] + FWD_TLS_MARGIN8 and the largest block
// leaves the shed line's block whole after our biggest allocation (the record buffer). P = the byte heap one TLS
// connection takes inside the net-out task: the three blocks (contexts 6.5 KB + buffer 4.4 KB) + the server's flight in
// the lwIP / Wi-Fi RX queue (<= 6.4 KB).
enum class FwdTlsClass : uint8_t { Metar, PresetNoMfl, WowBe, Custom, Wufc, COUNT };  // aviationweather.gov / WU, PWSweather, Windy / WOW-BE / other / api.weather.com
// Metar 17.5 KB as measured (the quiet median P 17 428 B); the presets keep the estimate above (17 KB)
// Wufc = the METAR figure (17.5 KB): not measured on its own
constexpr uint32_t FWD_TLS_P8[(int)FwdTlsClass::COUNT] = {17920u, 17u * 1024, 17u * 1024, 17u * 1024, 17920u};
constexpr uint32_t FWD_TLS_MARGIN8 = 12u * 1024;
// all three blocks (buffer + both contexts) may come out of the largest block: it must hold them and still
// leave the shed line's block
constexpr uint32_t FWD_TLS_LARGEST_MIN = SHED_LARGEST_MIN + TLSC_BLOCKS_MAX;
static_assert(FWD_TLS_LARGEST_MIN >= SHED_LARGEST_MIN + TLSC_CTX_BYTES + TLSC_BUF_LEN, "the shed block stays whole");
const char* fwdTlsClassName(FwdTlsClass c);  // metar preset_nomfl wowbe custom wufc
FwdTlsClass fwdTlsClassOf(FwdType t);
constexpr uint32_t FWD_SEND_MAX_MS = 10000;                 // the whole send, hard
constexpr uint32_t FWD_RETRY1_MS = 30000, FWD_RETRY2_MS = 120000;
constexpr uint32_t FWD_QUIET_MS = 3000, FWD_SHED_MS = 60000;  // the idle gaps after an upload / a page / a shed
constexpr uint32_t FWD_HOUR_GUARD_S = 45;
constexpr uint32_t FWD_TEST_WAIT_MS = 20000;                // "Send one now" waits this long for a free moment
constexpr uint8_t FWD_PAUSE_AFTER = 3;                      // refused keys in a row

bool fwdResourcesOk(bool tls, FwdTlsClass cls, uint32_t free8, uint32_t largest8);
// Why a running send must stop now (nullptr = go on): ota / reboot / capture / heap / largest / deadline.
const char* fwdAbortWhy(uint32_t free8, uint32_t largest8, bool ota, bool reboot, uint32_t elapsed_ms, bool capture = false);

// A TLS connection's end (tlsc) as a send result: untrusted -> TlsUntrusted (not retried), too_large -> Reject
// (not retried, why too_large), no_memory -> NoMemory (the pause after three), the rest -> Fail (retried).
FwdResult fwdTlsResult(TlscWhy w);

// Why an attempt was not delivered (per-target counters in the admin JSON)
enum class FwdCause : uint8_t { None, Dns, Connect, Read, Http5xx, Limited, Other, COUNT };
const char* fwdCauseName(FwdCause c);  // "" dns connect read 5xx 429 other
FwdCause fwdCauseOf(FwdResult r, int status, const char* why);

// The close guard: from 45 s before a close until store and learners are clear after it (at least 10 s after the
// boundary, at most 10 min). Hours close on UTC boundaries, days at local midnight (:30 / :45 zones differ).
struct FwdHourGuard {
  uint32_t boundary = 0;  // the boundary being guarded (unix), 0 = none
  bool tick(uint32_t unix_s, int32_t offset_s, bool store_busy, bool learn_busy);
  static bool near(uint32_t unix_s, int32_t offset_s);  // within 45 s before either boundary
};

// heap_low during a send: a sample taken while a send runs feeds the 10-minute window the
// free heap read just before the send started; the first sample after it ended feeds the free heap at its instant; in
// both cases the allocator's local minimum (the TLS dip) goes to fwd_heap_min. Every other sample is unchanged.
struct FwdHeapFeed {
  uint32_t window;  // the value for the 10-minute minimum
  bool to_fwd;      // `low` belongs to the send's low-water
};
FwdHeapFeed fwdHeapFeed(bool running, bool ran_since, uint32_t local_low, uint32_t free_now, uint32_t pre_send);

// ---------------------------------------------------------------- per target
// Fetch (scheduled METAR), RunOnce (METAR, user), Lookup (airport list, user) -- metar_sched.h; WuFetch / WuRun: the
// WU forecast pull and its "Fetch now" (wufc_sched.h)
enum class FwdKind : uint8_t { None, Send, Retry, Test, Fetch, RunOnce, Lookup, WuFetch, WuRun };
enum FwdPause : uint8_t { FP_NONE, FP_KEY, FP_MEMORY };  // three refused keys / three no_memory in a row
enum class FwdSt : uint8_t { Off, Idle, Running, Retry };
const char* fwdStName(FwdSt s);  // off idle running retry

// the unconfirmed rule's precondition -- the request left whole (`sent`: plain HTTP written, TLS
// flushed) and no status line came back (a read time-out, a close, an abort while waiting): the service may have it.
inline bool fwdWritten(bool sent, int status, FwdResult r) { return sent && status < 0 && r == FwdResult::Fail; }

struct FwdCounters {
  uint32_t ok, dup, skip, nodata, reject, refused, limited, drop, heap, fail;
  uint32_t unconfirmed;  // the request was written whole, the answer was lost, a retry would break the service minimum
};

// Why the last tick in a window did not start the target: a busy bit (FwdBusy), or one of these
enum : uint16_t { FW_WHY_HEAP = 0x200, FW_WHY_DATA = 0x400, FW_WHY_NETOUT = 0x800, FW_WHY_TURN = 0x2000 };  // above the busy bits

struct FwdSlotIn {
  bool on;            // master switch, target enabled and configured
  bool test_ok;       // a "Send one now" may run (master switch on, target configured; it may be disabled)
  bool tls;
  FwdTlsClass tls_class;  // the heap gate's host class
  uint16_t min_s, max_s, floor_s;
  FwdGate gate;       // the data gate now (scheduled send)
  FwdGate gate_test;  // ... for "Send one now" (the same reading may go again)
};
struct FwdTickIn {
  uint32_t now_ms;
  uint16_t busy;      // FwdBusy bits
  uint32_t free, largest;
  bool netout_idle;   // the net-out task has no job and no unread result
  FwdSlotIn s[FWD_SLOTS];
};
struct FwdStart {
  int slot;           // -1 = nothing starts this tick
  FwdKind kind;
};

struct FwdSlot {
  FwdSt st;
  uint32_t due_ms;         // the next window starts here
  uint32_t retry_ms;       // FwdSt::Retry: the retry may start from here (until due_ms)
  uint32_t start_ms;       // the running attempt's start
  uint32_t sample_ms;      // the first attempt of this sample (the base of the next due)
  uint8_t fails;           // failed attempts of this sample
  uint8_t refused_run;     // refused results in a row
  uint8_t gen;             // bumped by reset(): a result of the old settings is dropped
  bool paused;             // until the settings change or a test is accepted
  uint8_t pause_why;       // FwdPause
  uint8_t nomem_run;       // no_memory results in a row
  bool gate_seen;          // the data gate passed at least once in this window
  uint16_t last_why;       // the reason of the last refusal (FwdBusy bit or FW_WHY_*), 0 = none
  bool sent_ever;
  uint32_t last_send_ms;   // the last attempt that started (any kind): "Too soon" for a test
  uint32_t floor_ms;       // the service minimum of the running attempt (the unconfirmed rule)
  uint32_t ok_seen;        // last_seen of the last accepted send (the data gate's "same")
  FwdCounters n;
  // test ("Send one now")
  bool test_pending;
  uint32_t test_req_ms;
  FwdSt test_prev;
};

enum class FwdTestReq : uint8_t { Accepted, TooSoon, Busy, Off };
enum class FwdTestOutcome : uint8_t { None, NoData, Busy, Off };  // Off: the switch went off / the target was removed

class FwdSched {
 public:
  FwdSched();
  void reset(int slot);  // settings changed / switched on: a fresh window now, a pause lifted, counters kept
  // Forwarding alone: offer() + the queue's pick + picked() (host tests; the loop calls the parts).
  FwdStart tick(const FwdTickIn& in);
  // the net-out queue (netq.h): the windows, gates and outcomes of this tick and our candidates (sends in slot
  // order, then tests; at most 2 x FWD_SLOTS); after nqPick: why ours waited, test time-outs, our start if it is ours.
  int offer(const FwdTickIn& in, NqCand* out);
  FwdStart picked(const FwdTickIn& in, const NqCand* c, int n, const NqPick& p);
  long sendDueS(uint32_t now_ms) const;  // seconds until the next send window opens (0 = open), -1 = none
  // The running attempt ended. seen = the record's last_seen it carried. written: the request went out whole and
  // only the answer was lost -- the service may well have the reading; a retry inside its minimum (Windy, PWSweather:
  // 300 s) only meets 429, so the reading is counted `unconfirmed` and not sent again.
  void result(int slot, FwdResult r, uint32_t now_ms, uint32_t retry_after_s, uint32_t seen, bool written = false);
  // "Send one now": accepted while another target sends (it waits for the line, up to FWD_TEST_WAIT_MS); busy while
  // this target sends or a test of it waits; too soon inside the service minimum since its last send.
  FwdTestReq requestTest(int slot, uint32_t now_ms, uint16_t floor_s, bool on);
  // A test that could not start (read once after each tick, then cleared): the slot and why.
  FwdTestOutcome takeTestOutcome(int* slot);
  bool running() const { return run_slot_ >= 0; }
  int runningSlot() const { return run_slot_; }
  FwdKind runningKind() const { return run_kind_; }
  // seconds until the next window opens (0 = in it now), -1 = off / paused
  long nextS(int slot, uint32_t now_ms) const;

  FwdSlot s[FWD_SLOTS];

 private:
  bool inWindow(int i, const FwdSlotIn& in, uint32_t now) const;
  void closeWindow(int i, const FwdSlotIn& in, uint32_t now);
  FwdStart start(int slot, FwdKind kind, const FwdTickIn& in);
  int run_slot_;
  FwdKind run_kind_;
  uint8_t run_gen_;
  int last_served_;
  int test_out_slot_;
  FwdTestOutcome test_out_;
};

}  // namespace fc
