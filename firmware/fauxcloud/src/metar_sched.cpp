// metar_sched.cpp -- see metar_sched.h.
#include "metar_sched.h"

namespace fc {

static bool msBefore(uint32_t a, uint32_t b) { return (int32_t)(a - b) < 0; }

const char* metarOutcomeName(MetarOutcome o) {
  static const char* const N[] = {"ok", "nodata", "fail", "yield", "limited", "untrusted", "too_large", "no_memory", "reject"};
  return (unsigned)o < sizeof N / sizeof N[0] ? N[(int)o] : "?";
}
const char* metarReqName(MetarReq r) {
  static const char* const N[] = {"accepted", "too_soon", "busy", "off", "low_memory"};
  return (unsigned)r < sizeof N / sizeof N[0] ? N[(int)r] : "?";
}

const char* MetarSched::stateName() const {
  if (run_kind_ == FwdKind::Fetch || run_kind_ == FwdKind::RunOnce) return "running";
  if (paused_) return "paused";
  return st_ == St::Off ? "off" : st_ == St::Retry ? "retry" : "idle";
}

bool MetarSched::inWindow(uint32_t now) const {
  if (st_ == St::Retry) return !msBefore(now, retry_ms_) && msBefore(now, due_ms_ + METAR_WINDOW_MS);
  return !msBefore(now, due_ms_) && now - due_ms_ < METAR_WINDOW_MS;
}

// The cycle's window ended without a start: counted, the next due on the 900-s grid (every passed cycle counts)
void MetarSched::closeCycle(uint32_t now) {
  const uint32_t late = now - due_ms_;
  const uint32_t k = late / METAR_CYCLE_MS + 1;
  if (st_ == St::Retry) {  // the retry never found a free moment: this cycle missed, any later ones skipped
    n.missed++;
    n.skip += k - 1;
  } else if (last_why_ == FW_WHY_HEAP) {
    n.heap += k;
  } else {
    n.skip += k;
  }
  due_ms_ += k * METAR_CYCLE_MS;
  st_ = St::Idle;
  fails_ = 0;
}

void MetarSched::airportsChanged() {
  paused_ = false;
  nomem_run_ = 0;
  if (st_ != St::Off) st_ = St::Off;  // the next tick schedules the first due again
  // a fetch in flight would put the old grid back with its result: the restart waits for it
  restart_ = run_kind_ == FwdKind::Fetch || run_kind_ == FwdKind::RunOnce;
}

int MetarSched::offer(const MetarTickIn& in, NqCand* out) {
  const uint32_t now = in.now_ms;
  int k = 0;
  const bool fetchOk = in.can_run && in.have_airports;
  if (run_pending_ && !fetchOk) {
    run_pending_ = false;
    run_out_ = MetarUserOut::Off;
  }
  if (look_pending_ && !in.can_run) {
    look_pending_ = false;
    look_out_ = MetarUserOut::Off;
  }
  if (!fetchOk) {
    st_ = St::Off;
  } else if (st_ == St::Off) {
    st_ = St::Idle;
    if (in.now_unix) {  // the next :05 / :20 / :35 / :50 UTC at least 60 s away
      const uint32_t s = in.now_unix + METAR_FIRST_MS / 1000u;
      const uint32_t wait = (METAR_CYCLE_MS / 1000u - (s + METAR_CYCLE_MS / 1000u - METAR_GRID_OFFSET_S) % (METAR_CYCLE_MS / 1000u)) %
                            (METAR_CYCLE_MS / 1000u);
      due_ms_ = now + METAR_FIRST_MS + wait * 1000u;
      const uint32_t dueUnix = s + wait;
      const uint32_t slot = ((dueUnix - METAR_GRID_OFFSET_S) / (METAR_CYCLE_MS / 1000u)) % 4u;
      anchor_ms_ = due_ms_ - slot * METAR_CYCLE_MS;
      anchored_ = true;
    } else {
      due_ms_ = now + METAR_FIRST_MS + phase_ms_;
      anchored_ = false;
    }
    fails_ = 0;
    last_why_ = 0;
  }
  if (st_ != St::Off && run_kind_ != FwdKind::Fetch && run_kind_ != FwdKind::RunOnce) {
    if (!msBefore(now, due_ms_) && now - due_ms_ >= METAR_WINDOW_MS) closeCycle(now);  // the cycle's window is over
    if (!paused_ && inWindow(now)) {
      NqCand& c = out[k++];
      c = NqCand{NQ_SRC_METAR, 0, FwdKind::Fetch, NqPrio::Fetch, FB_ALL, 0, 0, st_ == St::Retry ? retry_ms_ : due_ms_};
      nqHeapGate(true, FwdTlsClass::Metar, c);
    }
  }
  if (run_pending_ && run_kind_ == FwdKind::None) {
    NqCand& c = out[k++];
    c = NqCand{NQ_SRC_METAR, 0, FwdKind::RunOnce, NqPrio::User, FB_ALL, 0, 0, run_req_ms_};
    nqHeapGate(true, FwdTlsClass::Metar, c);
  }
  if (look_pending_ && run_kind_ == FwdKind::None) {
    NqCand& c = out[k++];
    c = NqCand{NQ_SRC_METAR, 0, FwdKind::Lookup, NqPrio::User, FB_ALL, 0, 0, look_req_ms_};
    nqHeapGate(true, FwdTlsClass::Metar, c);
  }
  return k;
}

bool MetarSched::picked(const MetarTickIn& in, const NqCand* c, int n_, const NqPick& p, FwdKind* kind) {
  const uint32_t now = in.now_ms;
  for (int i = 0; i < n_ && i < NQ_MAX; i++) {
    if (c[i].src != NQ_SRC_METAR || i == p.idx) continue;
    if (c[i].kind == FwdKind::Fetch) last_why_ = p.why[i];
    else if (c[i].kind == FwdKind::RunOnce && now - run_req_ms_ > METAR_USER_WAIT_MS) {
      run_pending_ = false;
      run_out_ = MetarUserOut::Busy;
    } else if (c[i].kind == FwdKind::Lookup && now - look_req_ms_ > METAR_USER_WAIT_MS) {
      look_pending_ = false;
      look_out_ = MetarUserOut::Busy;
    }
  }
  if (p.idx < 0 || p.idx >= n_ || c[p.idx].src != NQ_SRC_METAR) return false;
  const FwdKind k = c[p.idx].kind;
  run_kind_ = k;
  job_ever_ = true;
  last_job_ms_ = now;
  run_taf_ = false;
  if (k == FwdKind::Fetch) {
    last_why_ = 0;
    run_taf_ = tafCycle(due_ms_);
    if (st_ == St::Idle) fails_ = 0;
  } else if (k == FwdKind::RunOnce) {
    run_pending_ = false;
  } else {
    look_pending_ = false;
  }
  *kind = k;
  return true;
}

void MetarSched::result(FwdKind kind, MetarOutcome r, uint32_t now, uint32_t retry_after_s) {
  if (kind != run_kind_) return;
  run_kind_ = FwdKind::None;
  if (kind == FwdKind::Lookup) return;  // the lookup's answer is the glue's (the list); nothing scheduled
  resultInner(kind, r, now, retry_after_s);
  if (restart_) {  // the airports changed meanwhile -- the first due again (60 s + phase), not the old grid
    restart_ = false;
    st_ = St::Off;
  }
}

void MetarSched::resultInner(FwdKind kind, MetarOutcome r, uint32_t now, uint32_t retry_after_s) {
  const bool scheduled = kind == FwdKind::Fetch;
  const uint32_t cycleEnd = due_ms_ + METAR_WINDOW_MS;
  auto nextCycle = [&]() {
    if (!scheduled) return;  // run once leaves the schedule as it is
    while (!msBefore(now, due_ms_)) due_ms_ += METAR_CYCLE_MS;
    st_ = St::Idle;
    fails_ = 0;
  };
  if (r != MetarOutcome::NoMemory) nomem_run_ = 0;
  switch (r) {
    case MetarOutcome::Ok:
    case MetarOutcome::NoData:
      (r == MetarOutcome::Ok ? n.ok : n.nodata)++;
      if (!scheduled) paused_ = false;  // run once lifts a pause
      if (!scheduled && st_ != St::Off && !msBefore(now, due_ms_) && !tafCycle(due_ms_)) {  // never the TAF cycle
        // a run once that answered while the cycle's window was open serves that cycle -- no second fetch
        while (!msBefore(now, due_ms_)) due_ms_ += METAR_CYCLE_MS;
        st_ = St::Idle;
        fails_ = 0;
      }
      nextCycle();
      return;
    case MetarOutcome::Limited: {
      n.limited++;
      const uint32_t wait = retry_after_s * 1000u > METAR_CYCLE_MS ? retry_after_s * 1000u : METAR_CYCLE_MS;
      if (st_ != St::Off) {
        const uint32_t earliest = now + wait;
        while (msBefore(due_ms_, earliest)) due_ms_ += METAR_CYCLE_MS;
        st_ = St::Idle;
        fails_ = 0;
      }
      return;
    }
    case MetarOutcome::Untrusted:
    case MetarOutcome::TooLarge:
      n.untrusted += r == MetarOutcome::Untrusted;
      n.fail += r == MetarOutcome::TooLarge;
      if (!paused_) n.paused++;
      paused_ = true;
      nextCycle();
      return;
    case MetarOutcome::Reject:
      n.fail++;
      nextCycle();
      return;
    case MetarOutcome::NoMemory:
      n.heap++;
      if (++nomem_run_ >= METAR_NOMEM_PAUSE) {
        if (!paused_) n.paused++;
        paused_ = true;
        nomem_run_ = 0;
        nextCycle();
        return;
      }
      break;  // retried like a failure
    case MetarOutcome::Fail:
    case MetarOutcome::Yield:
      (r == MetarOutcome::Yield ? n.yield : n.fail)++;
      break;
  }
  if (!scheduled) return;
  fails_++;
  const uint32_t at = now + (fails_ == 1 ? METAR_RETRY1_MS : METAR_RETRY2_MS);
  if (fails_ <= 2 && msBefore(at, cycleEnd)) {
    st_ = St::Retry;
    retry_ms_ = at;
  } else {
    n.missed++;
    nextCycle();
  }
}

// A press is judged by itself: busy while the same kind waits or runs; while another METAR job runs (the scheduled
// fetch, the TAF cycle, the other user job) it is queued and starts when that job is done; the 60-s gap only on an idle line.
static MetarReq userReq(bool usable, bool pending, bool same, bool other, bool ever, uint32_t now, uint32_t last) {
  if (!usable) return MetarReq::Off;
  if (pending || same) return MetarReq::Busy;
  if (other) return MetarReq::Accepted;
  if (ever && now - last < METAR_USER_GAP_MS) return MetarReq::TooSoon;
  return MetarReq::Accepted;
}

MetarReq MetarSched::requestRun(uint32_t now, bool usable) {
  const bool same = run_kind_ == FwdKind::RunOnce, other = run_kind_ != FwdKind::None && !same;
  const MetarReq r = userReq(usable, run_pending_, same, other, job_ever_, now, last_job_ms_);
  if (r == MetarReq::Accepted) {
    run_pending_ = true, run_req_ms_ = now, run_out_ = MetarUserOut::None;
    paused_ = false;  // accepted: the pause is lifted
    nomem_run_ = 0;
  }
  return r;
}
MetarReq MetarSched::requestLookup(uint32_t now, bool usable) {
  const bool same = run_kind_ == FwdKind::Lookup, other = run_kind_ != FwdKind::None && !same;
  const MetarReq r = userReq(usable, look_pending_, same, other, job_ever_, now, last_job_ms_);
  if (r == MetarReq::Accepted) look_pending_ = true, look_req_ms_ = now, look_out_ = MetarUserOut::None;
  return r;
}
MetarUserOut MetarSched::takeRunOutcome() {
  const MetarUserOut o = run_out_;
  run_out_ = MetarUserOut::None;
  return o;
}
MetarUserOut MetarSched::takeLookupOutcome() {
  const MetarUserOut o = look_out_;
  look_out_ = MetarUserOut::None;
  return o;
}

bool MetarSched::tafCycle(uint32_t due) const {
  return anchored_ && ((due - anchor_ms_) / METAR_CYCLE_MS) % 4u == METAR_TAF_SLOT;
}
bool MetarSched::tafCycleNext() const { return st_ != St::Off && tafCycle(due_ms_); }

long MetarSched::nextS(uint32_t now) const {
  if (st_ == St::Off || paused_) return -1;
  if (run_kind_ == FwdKind::Fetch || run_kind_ == FwdKind::RunOnce) return 0;
  const uint32_t at = st_ == St::Retry ? retry_ms_ : due_ms_;
  return msBefore(now, at) ? (long)((at - now + 999) / 1000) : 0;
}

}  // namespace fc
