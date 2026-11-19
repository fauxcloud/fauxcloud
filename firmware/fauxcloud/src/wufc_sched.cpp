// wufc_sched.cpp -- see wufc_sched.h.
#include "wufc_sched.h"

namespace fc {

static bool msBefore(uint32_t a, uint32_t b) { return (int32_t)(a - b) < 0; }

const char* wufcOutcomeName(WufcOutcome o) {
  static const char* const N[] = {"ok",       "nodata",    "fail",      "yield",     "limited", "refused",
                                  "encoding", "too_large", "untrusted", "no_memory", "bad",      "off"};
  return (unsigned)o < sizeof N / sizeof N[0] ? N[(int)o] : "?";
}
const char* wufcPauseName(WufcPause p) {
  static const char* const N[] = {"none", "key", "encoding", "too_large", "untrusted", "memory"};
  return (unsigned)p < sizeof N / sizeof N[0] ? N[(int)p] : "?";
}

const char* WufcSched::stateName() const {
  if (run_kind_ == FwdKind::WuFetch || run_kind_ == FwdKind::WuRun) return "running";
  if (pause_ != WufcPause::None) return "paused";
  return st_ == St::Off ? "off" : st_ == St::Retry ? "retry" : "idle";
}

bool WufcSched::inWindow(uint32_t now) const {
  if (st_ == St::Retry) return !msBefore(now, retry_ms_) && msBefore(now, due_ms_ + WUFC_WINDOW_MS);
  return !msBefore(now, due_ms_) && now - due_ms_ < WUFC_WINDOW_MS;
}

void WufcSched::closeCycle(uint32_t now) {
  const uint32_t k = (now - due_ms_) / WUFC_CYCLE_MS + 1;
  if (st_ == St::Retry) {
    n.missed++;
    n.skip += k - 1;
  } else if (last_why_ == FW_WHY_HEAP) {
    n.heap += k;
  } else {
    n.skip += k;
  }
  due_ms_ += k * WUFC_CYCLE_MS;
  st_ = St::Idle;
  fails_ = 0;
}

void WufcSched::nextCycle(uint32_t now) {
  if (st_ == St::Off) return;
  while (!msBefore(now, due_ms_)) due_ms_ += WUFC_CYCLE_MS;
  st_ = St::Idle;
  fails_ = 0;
}

void WufcSched::setPause(WufcPause p) {
  if (pause_ == WufcPause::None) n.paused++;
  pause_ = p;
}

void WufcSched::keyChanged() {
  pause_ = WufcPause::None;
  refused_run_ = nomem_run_ = 0;
  st_ = St::Off;  // the next tick schedules the first due again
  restart_ = run_kind_ == FwdKind::WuFetch || run_kind_ == FwdKind::WuRun;
}

int WufcSched::offer(const WufcTickIn& in, NqCand* out) {
  const uint32_t now = in.now_ms;
  int k = 0;
  if (run_pending_ && !in.can_user) {
    run_pending_ = false;
    run_out_ = MetarUserOut::Off;
  }
  if (run_pending_ && now - run_req_ms_ > WUFC_USER_WAIT_MS) {  // queued for 10 min without a start
    run_pending_ = false;
    run_out_ = MetarUserOut::Busy;
  }
  if (!in.can_run || !in.now_unix) {
    st_ = St::Off;
  } else if (st_ == St::Off) {  // the next :10 UTC at least 120 s away, plus the boot's phase
    st_ = St::Idle;
    const uint32_t s = in.now_unix + WUFC_FIRST_S;
    const uint32_t wait = (3600u - (s + 3600u - WUFC_GRID_OFFSET_S) % 3600u) % 3600u;
    due_ms_ = now + (WUFC_FIRST_S + wait) * 1000u + phase_ms_;
    fails_ = 0;
    last_why_ = 0;
  }
  if (st_ != St::Off && run_kind_ == FwdKind::None) {
    if (!msBefore(now, due_ms_) && now - due_ms_ >= WUFC_WINDOW_MS) closeCycle(now);
    if (pause_ == WufcPause::None && inWindow(now)) {
      NqCand& c = out[k++];
      c = NqCand{NQ_SRC_WUFC, 0, FwdKind::WuFetch, NqPrio::Pull, FB_ALL, 0, 0, st_ == St::Retry ? retry_ms_ : due_ms_};
      nqHeapGate(true, FwdTlsClass::Wufc, c);
    }
  }
  // Send one now's busy bits (a page request does not hold it: the sheet polls while it waits), and the gap
  if (run_pending_ && run_kind_ == FwdKind::None && (!job_ever_ || now - last_job_ms_ >= WUFC_USER_GAP_MS)) {
    NqCand& c = out[k++];
    c = NqCand{NQ_SRC_WUFC, 0, FwdKind::WuRun, NqPrio::User, FB_TEST, 0, 0, run_req_ms_};
    nqHeapGate(true, FwdTlsClass::Wufc, c);
  }
  return k;
}

bool WufcSched::picked(const WufcTickIn& in, const NqCand* c, int n_, const NqPick& p, FwdKind* kind) {
  const uint32_t now = in.now_ms;
  for (int i = 0; i < n_ && i < NQ_MAX; i++) {
    if (c[i].src != NQ_SRC_WUFC || i == p.idx) continue;
    if (c[i].kind == FwdKind::WuFetch) last_why_ = p.why[i];
  }
  if (p.idx < 0 || p.idx >= n_ || c[p.idx].src != NQ_SRC_WUFC) return false;
  const FwdKind k = c[p.idx].kind;
  run_kind_ = k;
  job_ever_ = true;
  last_job_ms_ = now;
  if (k == FwdKind::WuFetch) {
    last_why_ = 0;
    if (st_ == St::Idle) fails_ = 0;
  } else {
    run_pending_ = false;
  }
  *kind = k;
  return true;
}

void WufcSched::result(FwdKind kind, WufcOutcome r, uint32_t now, uint32_t retry_after_s) {
  if (kind != run_kind_ || kind == FwdKind::None) return;
  run_kind_ = FwdKind::None;
  const bool scheduled = kind == FwdKind::WuFetch;
  if (r != WufcOutcome::Refused && r != WufcOutcome::Off) refused_run_ = 0;  // nothing was asked: no streak ends
  if (r != WufcOutcome::NoMemory && r != WufcOutcome::Off) nomem_run_ = 0;
  bool retry = false;
  switch (r) {
    case WufcOutcome::Ok:
    case WufcOutcome::NoData:
      (r == WufcOutcome::Ok ? n.ok : n.nodata)++;
      // a "Fetch now" answered while the hour's window was open serves that hour (no second pull)
      if (scheduled || (st_ != St::Off && !msBefore(now, due_ms_))) nextCycle(now);
      break;
    case WufcOutcome::Limited: {
      n.limited++;
      const uint32_t wait = retry_after_s * 1000u > WUFC_CYCLE_MS ? retry_after_s * 1000u : WUFC_CYCLE_MS;
      if (st_ != St::Off) {
        const uint32_t earliest = now + wait;
        while (msBefore(due_ms_, earliest)) due_ms_ += WUFC_CYCLE_MS;
        st_ = St::Idle;
        fails_ = 0;
      }
      break;
    }
    case WufcOutcome::Refused:
      n.refused++;
      if (++refused_run_ >= WUFC_REFUSED_PAUSE) setPause(WufcPause::Key), refused_run_ = 0;
      if (scheduled) nextCycle(now);
      break;
    case WufcOutcome::Encoding:
    case WufcOutcome::TooLarge:
    case WufcOutcome::Untrusted:
      n.bad++;
      setPause(r == WufcOutcome::Encoding ? WufcPause::Encoding : r == WufcOutcome::TooLarge ? WufcPause::TooLarge
                                                                                              : WufcPause::Untrusted);
      if (scheduled) nextCycle(now);
      break;
    case WufcOutcome::Bad:
      n.bad++;
      if (scheduled) nextCycle(now);
      break;
    case WufcOutcome::Off:
      n.skip++;
      if (scheduled) nextCycle(now);
      break;
    case WufcOutcome::NoMemory:
      n.heap++;
      if (++nomem_run_ >= WUFC_NOMEM_PAUSE) {
        setPause(WufcPause::Memory);
        nomem_run_ = 0;
        if (scheduled) nextCycle(now);
      } else {
        retry = true;
      }
      break;
    case WufcOutcome::Fail:
    case WufcOutcome::Yield:
      n.fail++;
      retry = true;
      break;
  }
  if (retry && scheduled) {
    const uint32_t at = now + WUFC_RETRY_MS;
    if (++fails_ <= 1 && msBefore(at, due_ms_ + WUFC_WINDOW_MS)) {
      st_ = St::Retry;
      retry_ms_ = at;
    } else {
      n.missed++;
      nextCycle(now);
    }
  }
  if (restart_) {  // the key or the switch changed while this pull ran: the first due again, not the old grid
    restart_ = false;
    st_ = St::Off;
  }
}

MetarReq WufcSched::requestRun(uint32_t now, bool usable, bool heap_ok) {
  if (!usable) return MetarReq::Off;
  if (run_pending_ || run_kind_ == FwdKind::WuRun) return MetarReq::Accepted;  // a second press joins the first
  if (!heap_ok) return MetarReq::LowMemory;
  run_pending_ = true, run_req_ms_ = now, run_out_ = MetarUserOut::None;  // queued behind a pull or the 60-s gap
  pause_ = WufcPause::None;  // the user's check lifts a pause (a still-wrong key pauses again after three)
  refused_run_ = nomem_run_ = 0;
  return MetarReq::Accepted;
}

bool wufcRunHeld(uint16_t busy, uint32_t free8, uint32_t largest8) {
  NqCand g{};
  nqHeapGate(true, FwdTlsClass::Wufc, g);
  return (busy & FB_TEST) || free8 < g.need8 || largest8 < g.largest8;
}

uint32_t WufcSched::etaS(uint32_t now, bool line_busy, bool held) const {
  uint32_t s = 0;
  if (job_ever_ && now - last_job_ms_ < WUFC_USER_GAP_MS) s = (WUFC_USER_GAP_MS - (now - last_job_ms_) + 999) / 1000;
  if ((line_busy || held || run_kind_ != FwdKind::None) && s < WUFC_ETA_JOB_S) s = WUFC_ETA_JOB_S;
  return s + WUFC_ETA_START_S;
}

MetarUserOut WufcSched::takeRunOutcome() {
  const MetarUserOut o = run_out_;
  run_out_ = MetarUserOut::None;
  return o;
}

long WufcSched::nextS(uint32_t now) const {
  if (st_ == St::Off || pause_ != WufcPause::None) return -1;
  if (run_kind_ == FwdKind::WuFetch) return 0;
  const uint32_t at = st_ == St::Retry ? retry_ms_ : due_ms_;
  return msBefore(now, at) ? (long)((at - now + 999) / 1000) : 0;
}

}  // namespace fc
