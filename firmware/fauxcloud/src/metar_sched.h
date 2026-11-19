// metar_sched.h -- the METAR source on the net-out queue (netq.h), a pure state machine on a fake
// clock. Scheduled fetch: the first due 60 s (+ a per-boot phase of 0-60 s) after the module can run (its switch on, at
// least one airport, a TLS build, real time), then every 900 s; a 300-s window from each due; a window that ends without
// a start is a skip (counted with its last reason), nothing queued. Retries on `fail` / `yield` only, 30 s then 120 s,
// inside the window; then the cycle is missed. 429: the cycle skipped, Retry-After honoured (at least one cycle).
// `untrusted` / `too_large`, or three `no_memory` in a row: paused until the airports change or "run once" is accepted.
// "Run once" and "Find airports" (lookup): user priority, wait <= 30 s for a free moment (else busy), one at a time each;
// a press while another METAR job runs is queued behind it, on an idle line >= 60 s after the previous METAR job of any
// kind (else too soon). Every METAR job takes the full busy table (FB_ALL).
#pragma once
#include <cstdint>

#include "netq.h"

namespace fc {

constexpr uint32_t METAR_FIRST_MS = 60000, METAR_CYCLE_MS = 900000, METAR_WINDOW_MS = 300000;
constexpr uint32_t METAR_RETRY1_MS = 30000, METAR_RETRY2_MS = 120000;
constexpr uint32_t METAR_USER_WAIT_MS = 30000, METAR_USER_GAP_MS = 60000;
constexpr uint8_t METAR_NOMEM_PAUSE = 3;

// How a METAR job ended (the glue maps FwdResp / tlsc to these)
enum class MetarOutcome : uint8_t { Ok, NoData, Fail, Yield, Limited, Untrusted, TooLarge, NoMemory, Reject };
const char* metarOutcomeName(MetarOutcome o);  // ok nodata fail yield limited untrusted too_large no_memory reject
enum class MetarReq : uint8_t { Accepted, TooSoon, Busy, Off, LowMemory };  // LowMemory: WU's Fetch now only
const char* metarReqName(MetarReq r);  // accepted too_soon busy off low_memory
enum class MetarUserOut : uint8_t { None, Busy, Off };  // a run once / lookup that could not start

struct MetarCounters {
  uint32_t ok, nodata, fail, yield, skip, heap, limited, untrusted, paused, missed;
};

struct MetarTickIn {
  uint32_t now_ms;
  bool can_run;       // switch on, TLS build, real time
  bool have_airports;  // at least one chosen
  uint32_t now_unix = 0;  // real time (0 = unknown: the boot-relative first due + phase, no TAF slot)
};
// with real time the 900-s grid sits on :05 / :20 / :35 / :50
// UTC (5 min after the half-hourly METARs), the first due at the next of them >= 60 s away; the :50 cycle of every hour
// fetches the TAFs (it brings no new METAR: the reports are issued at :00 and :30), the other three the METARs.
constexpr uint32_t METAR_GRID_OFFSET_S = 300, METAR_TAF_SLOT = 3;

class MetarSched {
 public:
  explicit MetarSched(uint32_t phase_ms = 0) : phase_ms_(phase_ms % 60001u) {}
  // The loop tick, before nqPick: the state of this second and our candidates (at most 3).
  int offer(const MetarTickIn& in, NqCand* out);
  // After nqPick: why ours waited, the user jobs' time-outs, our start (true = the loop starts this job).
  bool picked(const MetarTickIn& in, const NqCand* c, int n, const NqPick& p, FwdKind* kind);
  void result(FwdKind kind, MetarOutcome r, uint32_t now_ms, uint32_t retry_after_s);
  MetarReq requestRun(uint32_t now_ms, bool usable);
  MetarReq requestLookup(uint32_t now_ms, bool usable);
  MetarUserOut takeRunOutcome();
  MetarUserOut takeLookupOutcome();
  void airportsChanged();  // a pause lifted, the schedule restarts (first due again)
  long nextS(uint32_t now_ms) const;  // seconds to the next window (0 = in it), -1 = off / paused
  bool running() const { return run_kind_ != FwdKind::None; }
  bool runTaf() const { return run_taf_; }  // the running scheduled fetch is the hour's TAF cycle
  bool tafCycleNext() const;                 // the next scheduled cycle is a TAF cycle (the admin view)
  FwdKind runningKind() const { return run_kind_; }
  bool paused() const { return paused_; }
  bool runPending() const { return run_pending_; }
  bool lookupPending() const { return look_pending_; }
  uint16_t lastWhy() const { return last_why_; }
  const char* stateName() const;  // off idle retry running paused
  MetarCounters n = {};

 private:
  enum class St : uint8_t { Off, Idle, Retry };
  bool inWindow(uint32_t now) const;
  void closeCycle(uint32_t now);
  void resultInner(FwdKind kind, MetarOutcome r, uint32_t now, uint32_t retry_after_s);
  uint32_t phase_ms_;
  St st_ = St::Off;
  bool paused_ = false;
  bool restart_ = false;  // the airports changed while a fetch ran -- its result keeps the restart
  bool anchored_ = false, run_taf_ = false;  // the grid on UTC; the running fetch is the TAF cycle
  uint32_t anchor_ms_ = 0;                   // a :05 UTC slot on the ms clock (the grid's slot 0)
  bool tafCycle(uint32_t due) const;  // the cycle at `due` is the hour's TAF cycle (the :50 slot)
  uint8_t fails_ = 0, nomem_run_ = 0;
  uint32_t due_ms_ = 0, retry_ms_ = 0;
  uint16_t last_why_ = 0;
  FwdKind run_kind_ = FwdKind::None;
  bool run_pending_ = false, look_pending_ = false, job_ever_ = false;
  uint32_t run_req_ms_ = 0, look_req_ms_ = 0, last_job_ms_ = 0;
  MetarUserOut run_out_ = MetarUserOut::None, look_out_ = MetarUserOut::None;
};

}  // namespace fc
