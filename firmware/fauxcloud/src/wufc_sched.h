// wufc_sched.h -- the WU forecast source on the net-out queue (netq.h), a pure state machine on a
// fake clock, METAR's shape made smaller. Scheduled pull: hourly at :10 UTC (clear of METAR's :05/:20/:35/:50) plus a
// per-boot phase of 0-120 s, the first due at the next slot >= 120 s after it can run (switch on, key stored, the
// bridge's location, real time, a TLS build); a 20-min window from each due, at the lowest priority (NqPrio::Pull:
// below METAR's fetch, yields to a send window like a fetch). A window that ends without a start is a skip.
// Outcomes: ok / nodata -> the next hour; fail / yield -> one retry 5 min later inside the window, then missed;
// limited (429) -> the next slot >= max(1 h, Retry-After); refused (401/403) -> three in a row pause until the key
// changes; encoding / too_large / untrusted -> paused until the key or the switch is saved again; no_memory -> retried,
// three in a row pause; bad -> the next hour; off (nothing sent: the key or the location gone at the start, or an answer to
// settings saved since) -> counted as a skip, no streak touched, the next hour. "Fetch now" (the admin's key check, also with the switch off): user
// priority; never refused for a busy line -- one pending flag (a second press joins it), offered once 60 s
// have passed since the previous WU job, held only by Send one now's busy bits (FB_TEST: upload, restart, link) and the
// heap gate, given up as busy after 10 min; refused only without key / location / time (off) or below the heap gate at
// the press (low_memory). Accepted, it lifts a pause. The scheduled pull takes the full busy table (FB_ALL).
#pragma once
#include <cstdint>

#include "metar_sched.h"  // MetarReq / MetarUserOut: the same user-request vocabulary
#include "netq.h"

namespace fc {

constexpr uint32_t WUFC_CYCLE_MS = 3600000, WUFC_WINDOW_MS = 1200000, WUFC_RETRY_MS = 300000;
constexpr uint32_t WUFC_FIRST_S = 120, WUFC_GRID_OFFSET_S = 600, WUFC_PHASE_MAX_MS = 120000;
constexpr uint32_t WUFC_USER_WAIT_MS = 600000, WUFC_USER_GAP_MS = 60000;  // a queued Fetch now waits <= 10 min
constexpr uint32_t WUFC_ETA_JOB_S = 20, WUFC_ETA_START_S = 5;  // the "runs in about N s" estimate
constexpr uint8_t WUFC_REFUSED_PAUSE = 3, WUFC_NOMEM_PAUSE = 3;

enum class WufcOutcome : uint8_t { Ok, NoData, Fail, Yield, Limited, Refused, Encoding, TooLarge, Untrusted, NoMemory, Bad, Off };
// ok nodata fail yield limited refused encoding too_large untrusted no_memory bad off
const char* wufcOutcomeName(WufcOutcome o);
enum class WufcPause : uint8_t { None, Key, Encoding, TooLarge, Untrusted, Memory };
const char* wufcPauseName(WufcPause p);  // none key encoding too_large untrusted memory

struct WufcCounters {
  uint32_t ok, nodata, fail, limited, refused, skip, heap, missed, bad, paused;
};
struct WufcTickIn {
  uint32_t now_ms;
  bool can_run;   // scheduled pulls: the switch on + can_user
  bool can_user;  // "Fetch now": key stored, the location, real time, FWD_TLS (the switch may be off)
  uint32_t now_unix;  // real time (0 = none: can_run is false then)
};

// A queued Fetch now is held by Send one now's busy bits (FB_TEST: an upload, a restart, no link or time) or the byte
// heap below the WU start gate (nqHeapGate, the figures nqPick compares)
bool wufcRunHeld(uint16_t busy, uint32_t free8, uint32_t largest8);

class WufcSched {
 public:
  explicit WufcSched(uint32_t phase_ms = 0) : phase_ms_(phase_ms % (WUFC_PHASE_MAX_MS + 1)) {}
  int offer(const WufcTickIn& in, NqCand* out);  // at most 2
  bool picked(const WufcTickIn& in, const NqCand* c, int n, const NqPick& p, FwdKind* kind);
  void result(FwdKind kind, WufcOutcome r, uint32_t now_ms, uint32_t retry_after_s);
  MetarReq requestRun(uint32_t now_ms, bool usable, bool heap_ok);  // Accepted (queued or joined), Off, LowMemory
  // about when a queued Fetch now starts (s); held = its start gate holds it now (wufcRunHeld)
  uint32_t etaS(uint32_t now_ms, bool line_busy, bool held) const;
  MetarUserOut takeRunOutcome();
  void keyChanged();  // a new key or the switch saved: every pause lifted, the schedule restarts (first due again)
  long nextS(uint32_t now_ms) const;  // seconds to the next window (0 = in it), -1 = off / paused
  bool running() const { return run_kind_ != FwdKind::None; }
  FwdKind runningKind() const { return run_kind_; }
  WufcPause pause() const { return pause_; }
  bool runPending() const { return run_pending_; }
  uint16_t lastWhy() const { return last_why_; }
  const char* stateName() const;  // off idle retry running paused
  WufcCounters n = {};

 private:
  enum class St : uint8_t { Off, Idle, Retry };
  bool inWindow(uint32_t now) const;
  void closeCycle(uint32_t now);
  void nextCycle(uint32_t now);
  void setPause(WufcPause p);
  uint32_t phase_ms_;
  St st_ = St::Off;
  WufcPause pause_ = WufcPause::None;
  bool restart_ = false;
  uint8_t fails_ = 0, refused_run_ = 0, nomem_run_ = 0;
  uint32_t due_ms_ = 0, retry_ms_ = 0;
  uint16_t last_why_ = 0;
  FwdKind run_kind_ = FwdKind::None;
  bool run_pending_ = false, job_ever_ = false;
  uint32_t run_req_ms_ = 0, last_job_ms_ = 0;
  MetarUserOut run_out_ = MetarUserOut::None;
};

}  // namespace fc
