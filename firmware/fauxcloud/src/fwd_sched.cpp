// fwd_sched.cpp -- see fwd_sched.h.
#include "fwd_sched.h"

#include "netq.h"

#include <cstring>

namespace fc {

static const char* const BUSY_NAMES[] = {"upload", "http", "store", "learn", "hour", "mqtt", "restart", "link"};
const char* fwdBusyName(uint16_t m) {
  for (int i = 0; i < 8; i++)
    if ((m >> i) & 1u) return BUSY_NAMES[i];
  if (m & FW_WHY_HEAP) return "heap";
  if (m & FW_WHY_DATA) return "data";
  if (m & FW_WHY_NETOUT) return "netout";
  if (m & FW_WHY_TURN) return "turn";
  return "";
}

static const char* const ST_NAMES[] = {"off", "idle", "running", "retry"};
const char* fwdStName(FwdSt s) { return ST_NAMES[(int)s]; }

static const char* const CLASS_NAMES[] = {"metar", "preset_nomfl", "wowbe", "custom", "wufc"};
const char* fwdTlsClassName(FwdTlsClass c) { return (int)c < (int)FwdTlsClass::COUNT ? CLASS_NAMES[(int)c] : ""; }
FwdTlsClass fwdTlsClassOf(FwdType t) {
  switch (t) {
    case FwdType::Wu:
    case FwdType::PwsWeather:
    case FwdType::Windy: return FwdTlsClass::PresetNoMfl;
    case FwdType::WowBe: return FwdTlsClass::WowBe;
    default: return FwdTlsClass::Custom;
  }
}

bool fwdResourcesOk(bool tls, FwdTlsClass cls, uint32_t free8, uint32_t largest8) {
  if (!tls) return free8 >= FWD_PLAIN_FREE_MIN && largest8 >= FWD_PLAIN_LARGEST_MIN;
  const int c = (int)cls < (int)FwdTlsClass::COUNT ? (int)cls : (int)FwdTlsClass::Custom;
  return free8 >= FWD_TLS_P8[c] + FWD_TLS_MARGIN8 && largest8 >= FWD_TLS_LARGEST_MIN;
}

const char* fwdAbortWhy(uint32_t free8, uint32_t largest8, bool ota, bool reboot, uint32_t elapsed_ms) {
  if (ota) return "ota";
  if (reboot) return "reboot";
  if (free8 < FWD_ABORT_FREE_MIN) return "heap";
  if (largest8 < FWD_ABORT_LARGEST_MIN) return "largest";
  if (elapsed_ms >= FWD_SEND_MAX_MS) return "deadline";
  return nullptr;
}

FwdResult fwdTlsResult(TlscWhy w) {
  switch (w) {
    case TlscWhy::Ok: return FwdResult::Ok;  // the caller takes the response's own result
    case TlscWhy::Untrusted: return FwdResult::TlsUntrusted;
    case TlscWhy::TooLarge: return FwdResult::Reject;
    case TlscWhy::NoMemory: return FwdResult::NoMemory;
    default: return FwdResult::Fail;  // dns, connect, handshake, read, clock, aborted: retried
  }
}

static const char* const CAUSE_NAMES[] = {"", "dns", "connect", "read", "5xx", "429", "other"};
const char* fwdCauseName(FwdCause c) { return (int)c < (int)FwdCause::COUNT ? CAUSE_NAMES[(int)c] : ""; }

FwdCause fwdCauseOf(FwdResult r, int status, const char* why) {
  if (r == FwdResult::Limited) return FwdCause::Limited;
  if (r == FwdResult::Reject && why && strcmp(why, "too_large") == 0) return FwdCause::Other;  // a record too large
  if (r != FwdResult::Fail && r != FwdResult::NoMemory) return FwdCause::None;  // ok, or counted on its own (refused...)
  if (status >= 500) return FwdCause::Http5xx;
  const char* w = why ? why : "";
  if (strcmp(w, "dns") == 0) return FwdCause::Dns;
  if (strcmp(w, "connect") == 0 || strcmp(w, "handshake") == 0 || strcmp(w, "write") == 0) return FwdCause::Connect;
  if (strcmp(w, "read") == 0 || strcmp(w, "deadline") == 0 || (!w[0] && status < 0)) return FwdCause::Read;
  return FwdCause::Other;  // no_memory, aborted_*, an unexpected status
}

static uint32_t toHour(uint32_t unix_s) { return 3600 - unix_s % 3600; }  // 1..3600 s to the next UTC hour
static uint32_t toDay(uint32_t unix_s, int32_t off) {                      // 1..86400 s to the next local midnight
  const int64_t local = (int64_t)unix_s + off;
  return (uint32_t)(86400 - ((local % 86400) + 86400) % 86400);
}

bool FwdHourGuard::near(uint32_t unix_s, int32_t off) {
  return toHour(unix_s) <= FWD_HOUR_GUARD_S || toDay(unix_s, off) <= FWD_HOUR_GUARD_S;
}

bool FwdHourGuard::tick(uint32_t unix_s, int32_t off, bool store_busy, bool learn_busy) {
  const uint32_t h = toHour(unix_s), d = toDay(unix_s, off);
  if (h <= FWD_HOUR_GUARD_S || d <= FWD_HOUR_GUARD_S) {
    boundary = unix_s + (h <= FWD_HOUR_GUARD_S ? h : d);
    return true;
  }
  if (!boundary) return false;
  if (unix_s < boundary) {  // the clock stepped back
    boundary = 0;
    return false;
  }
  const uint32_t after = unix_s - boundary;
  if (after < 600 && (after < 10 || store_busy || learn_busy)) return true;
  boundary = 0;
  return false;
}

FwdHeapFeed fwdHeapFeed(bool running, bool ran_since, uint32_t local_low, uint32_t free_now, uint32_t pre_send) {
  if (running) return {pre_send ? pre_send : free_now, true};
  if (ran_since) return {free_now, true};
  return {local_low, false};
}

static bool before(uint32_t a, uint32_t b) { return (int32_t)(a - b) < 0; }  // a earlier than b (wrap-safe)

FwdSched::FwdSched() : run_slot_(-1), run_kind_(FwdKind::None), run_gen_(0), last_served_(-1), test_out_slot_(-1),
                       test_out_(FwdTestOutcome::None) {
  memset(s, 0, sizeof s);
}

void FwdSched::reset(int i) {
  if (i < 0 || i >= FWD_SLOTS) return;
  s[i].gen++;                                           // a running attempt's result is dropped
  if (s[i].st != FwdSt::Running) s[i].st = FwdSt::Off;  // the next tick opens a window now
  s[i].paused = false;
  s[i].pause_why = FP_NONE;
  s[i].refused_run = 0;
  s[i].nomem_run = 0;
  s[i].fails = 0;
  s[i].gate_seen = false;
  s[i].last_why = 0;
}

bool FwdSched::inWindow(int i, const FwdSlotIn& in, uint32_t now) const {
  const FwdSlot& x = s[i];
  if (x.st == FwdSt::Retry) return !before(now, x.retry_ms) && before(now, x.due_ms);
  if (before(now, x.due_ms)) return false;
  return now - x.due_ms <= (uint32_t)(in.max_s - in.min_s) * 1000u;
}

// The window passed without a start: counted; the next due = due + k * min_s, the first one whose window has not
// ended yet (= the first due after now whenever max_s < 2 min_s): nothing queued, no catch-up burst.
void FwdSched::closeWindow(int i, const FwdSlotIn& in, uint32_t now) {
  FwdSlot& x = s[i];
  const uint32_t step = (uint32_t)in.min_s * 1000u, win = (uint32_t)(in.max_s - in.min_s) * 1000u;
  const uint32_t late = now - x.due_ms - win;  // > 0 here
  const uint32_t k0 = (late + step - 1) / step, k = k0 ? k0 : 1;
  // every window that passed counts (a tick stall can pass more than one)
  if (!x.gate_seen) x.n.nodata += k;
  else if (x.last_why == FW_WHY_HEAP) x.n.heap += k;
  else x.n.skip += k;
  x.due_ms += k * step;
  x.gate_seen = false;
  x.last_why = 0;
}

// the windows, gates and outcomes of one tick, and the candidates for the net-out queue (netq.h): the sends in
// round-robin order (earliest due, ties to the slot after the last one served), then the tests ("Send one now", slot order).
bool fwdHttpBusy(uint32_t now_ms, int in_flight, uint32_t last_page_ms, uint32_t shed_ms) {
  return in_flight > 0 || (last_page_ms && now_ms - last_page_ms < FWD_QUIET_MS) || (shed_ms && now_ms - shed_ms < FWD_SHED_MS);
}

int FwdSched::offer(const FwdTickIn& in, NqCand* out) {
  const uint32_t now = in.now_ms;
  int n = 0;
  NqCand sends[FWD_SLOTS];
  int ns = 0;
  NqCand tests[FWD_SLOTS];
  int nt = 0;
  for (int i = 0; i < FWD_SLOTS; i++) {
    FwdSlot& x = s[i];
    const FwdSlotIn& c = in.s[i];
    if (x.st == FwdSt::Running) continue;
    if (!c.test_ok && x.test_pending) {  // switched off or removed while waiting: the test ends with an outcome
      x.test_pending = false;
      test_out_ = FwdTestOutcome::Off, test_out_slot_ = i;
    }
    // "Send one now": the upload / restart / link guards, the heap floor and a free net-out task, nothing else
    if (x.test_pending) {
      if (c.gate_test != FwdGate::Ok) {
        x.test_pending = false;
        test_out_ = FwdTestOutcome::NoData, test_out_slot_ = i;
      } else {
        NqCand& t = tests[nt++];
        t = NqCand{NQ_SRC_FWD, (uint8_t)i, FwdKind::Test, NqPrio::User, FB_TEST, 0, 0, now};
        nqHeapGate(c.tls, c.tls_class, t);
      }
    }
    if (!c.on) {
      x.st = FwdSt::Off;
      continue;
    }
    if (x.st == FwdSt::Off) {  // switched on, reset or booted: a window opens
      x.st = FwdSt::Idle;
      // a service with a 5-min limit (Windy, PWSweather) answers 429 when the first window opens "now"
      // (after an OTA, after a save). After a save the last start is known: the window opens at last start + floor
      // (now if that has passed); after a boot or in a fresh slot nothing is known: now + floor.
      x.due_ms = now;
      if (c.floor_s >= 300) {
        const uint32_t floor_ms = (uint32_t)c.floor_s * 1000u;
        if (!x.sent_ever) x.due_ms = now + floor_ms;
        else if (now - x.last_send_ms < floor_ms) x.due_ms = x.last_send_ms + floor_ms;
      }
      x.gate_seen = false;
    }
    if (x.paused) continue;
    // a retry that can no longer start before the next due is dropped (the sample is gone, the due stays)
    if (x.st == FwdSt::Retry && !before(now, x.due_ms)) {
      x.n.drop++;
      x.st = FwdSt::Idle;
      x.fails = 0;
      x.gate_seen = false;
    }
    if (x.st == FwdSt::Idle && !before(now, x.due_ms) && !inWindow(i, c, now)) closeWindow(i, c, now);
    if (!inWindow(i, c, now)) continue;
    if (c.gate != FwdGate::Ok) {
      x.last_why = FW_WHY_DATA;
      continue;
    }
    x.gate_seen = true;
    NqCand& d = sends[ns++];
    const bool retry = x.st == FwdSt::Retry;
    d = NqCand{NQ_SRC_FWD, (uint8_t)i, retry ? FwdKind::Retry : FwdKind::Send, NqPrio::Send, FB_ALL, 0, 0,
               retry ? x.retry_ms : x.due_ms};
    nqHeapGate(c.tls, c.tls_class, d);
  }
  // the round-robin order: the slot after the last one served first (stable for equal due times in nqPick)
  for (int k = 1; k <= FWD_SLOTS; k++)
    for (int j = 0; j < ns; j++)
      if (sends[j].slot == (last_served_ + k + FWD_SLOTS) % FWD_SLOTS) out[n++] = sends[j];
  for (int j = 0; j < nt; j++) out[n++] = tests[j];
  return n;
}

// after the queue's pick -- why each of ours waited, a test that waited too long, and our own start
FwdStart FwdSched::picked(const FwdTickIn& in, const NqCand* c, int n, const NqPick& p) {
  for (int i = 0; i < n && i < NQ_MAX; i++) {
    if (c[i].src != NQ_SRC_FWD || i == p.idx) continue;
    FwdSlot& x = s[c[i].slot];
    if (c[i].kind != FwdKind::Test) {
      x.last_why = p.why[i];
    } else if (in.now_ms - x.test_req_ms > FWD_TEST_WAIT_MS) {
      x.test_pending = false;
      test_out_ = FwdTestOutcome::Busy, test_out_slot_ = c[i].slot;
    }
  }
  if (p.idx < 0 || p.idx >= n || c[p.idx].src != NQ_SRC_FWD) return {-1, FwdKind::None};
  return start(c[p.idx].slot, c[p.idx].kind, in);
}

long FwdSched::sendDueS(uint32_t now_ms) const {
  long best = -1;
  for (int i = 0; i < FWD_SLOTS; i++) {
    if (s[i].st != FwdSt::Idle && s[i].st != FwdSt::Retry) continue;
    const long v = nextS(i, now_ms);
    if (v >= 0 && (best < 0 || v < best)) best = v;
  }
  return best;
}

FwdStart FwdSched::start(int slot, FwdKind kind, const FwdTickIn& in) {
  const uint32_t now = in.now_ms;
  FwdSlot& x = s[slot];
  const uint32_t nextDue = now + (uint32_t)in.s[slot].min_s * 1000u;
  if (kind == FwdKind::Test) {  // the next scheduled send keeps the interval after a test
    x.test_pending = false;
    x.test_prev = x.st;
    if (x.st != FwdSt::Retry && before(x.due_ms, nextDue)) x.due_ms = nextDue;
  } else {
    x.last_why = 0;
    if (kind == FwdKind::Send) x.sample_ms = now, x.fails = 0, x.due_ms = nextDue;  // the next regular due
    last_served_ = slot;
  }
  x.st = FwdSt::Running;
  run_gen_ = x.gen;
  x.start_ms = now;
  x.last_send_ms = now;
  x.floor_ms = (uint32_t)in.s[slot].floor_s * 1000u;
  x.sent_ever = true;
  run_slot_ = slot;
  run_kind_ = kind;
  return {slot, kind};
}

// Forwarding alone (for the host tests): offer, the queue's pick, the outcome.
FwdStart FwdSched::tick(const FwdTickIn& in) {
  NqCand c[NQ_MAX];
  const int n = offer(in, c);
  const NqIn q{in.now_ms, in.busy, in.free, in.largest, in.netout_idle && run_slot_ < 0, -1};
  const NqPick p = nqPick(q, c, n);
  return picked(in, c, n, p);
}

void FwdSched::result(int i, FwdResult r, uint32_t now, uint32_t retry_after_s, uint32_t seen, bool written) {
  if (i < 0 || i >= FWD_SLOTS || run_slot_ != i) return;
  const FwdKind kind = run_kind_;
  run_slot_ = -1;
  run_kind_ = FwdKind::None;
  FwdSlot& x = s[i];
  if (x.st != FwdSt::Running) return;
  if (x.gen != run_gen_) {  // the settings changed while it ran: its result is not the new target's
    x.st = FwdSt::Off;      // the next tick opens a fresh window
    return;
  }
  if (kind == FwdKind::Test) {  // not counted; an accepted test resumes a pause and keeps the service minimum
    x.st = x.test_prev == FwdSt::Running ? FwdSt::Idle : x.test_prev;
    if (r == FwdResult::Ok || r == FwdResult::Dup) {
      x.paused = false;
      x.pause_why = FP_NONE;
      x.refused_run = 0;
      x.nomem_run = 0;
      x.ok_seen = seen;
    }
    return;
  }
  // x.due_ms already holds the next regular due (the sample's first start + min_s, set at its Start)
  x.st = FwdSt::Idle;
  if (r != FwdResult::Refused) x.refused_run = 0;
  if (r != FwdResult::NoMemory) x.nomem_run = 0;
  switch (r) {
    case FwdResult::Ok:
    case FwdResult::Dup:
      if (r == FwdResult::Ok) x.n.ok++;
      else x.n.dup++;
      x.ok_seen = seen;
      x.fails = 0;
      break;
    case FwdResult::Limited:
      x.n.limited++;
      x.fails = 0;
      if (retry_after_s && before(x.due_ms, now + retry_after_s * 1000u)) x.due_ms = now + retry_after_s * 1000u;
      break;
    case FwdResult::Refused:
      x.n.refused++;
      x.fails = 0;
      if (++x.refused_run >= FWD_PAUSE_AFTER) x.paused = true, x.pause_why = FP_KEY;
      break;
    case FwdResult::NoMemory:  // like a heap skip -- the sample is let go, no retry inside the window
      x.n.heap++;
      x.fails = 0;
      if (++x.nomem_run >= FWD_PAUSE_AFTER) x.paused = true, x.pause_why = FP_MEMORY;
      break;
    case FwdResult::Fail: {
      const uint32_t at = now + (x.fails == 0 ? FWD_RETRY1_MS : FWD_RETRY2_MS);
      if (written && at - x.last_send_ms < x.floor_ms) {  // the answer lost, a retry would come inside the minimum
        x.n.unconfirmed++;
        x.fails = 0;
        break;
      }
      x.n.fail++;
      x.fails++;
      if (x.fails >= 3 || !before(at, x.due_ms)) {
        x.n.drop++;
        x.fails = 0;
      } else {
        x.retry_ms = at;
        x.st = FwdSt::Retry;
      }
      break;
    }
    default:  // reject, tls_untrusted, none
      x.n.reject++;
      x.fails = 0;
      break;
  }
}

FwdTestReq FwdSched::requestTest(int i, uint32_t now, uint16_t floor_s, bool on) {
  if (i < 0 || i >= FWD_SLOTS || !on) return FwdTestReq::Off;
  FwdSlot& x = s[i];
  // another target's send on the line: the test waits for it (FWD_TEST_WAIT_MS); this target's own send or a test of it
  // already waiting: busy
  if (run_slot_ == i || x.test_pending) return FwdTestReq::Busy;
  if (x.sent_ever && now - x.last_send_ms < (uint32_t)floor_s * 1000u) return FwdTestReq::TooSoon;
  x.test_pending = true;
  x.test_req_ms = now;
  return FwdTestReq::Accepted;
}

FwdTestOutcome FwdSched::takeTestOutcome(int* slot) {
  const FwdTestOutcome o = test_out_;
  if (slot) *slot = test_out_slot_;
  test_out_ = FwdTestOutcome::None;
  test_out_slot_ = -1;
  return o;
}

long FwdSched::nextS(int i, uint32_t now) const {
  const FwdSlot& x = s[i];
  if (x.st == FwdSt::Off || x.paused) return -1;
  if (x.st == FwdSt::Running) return 0;
  const uint32_t at = x.st == FwdSt::Retry ? x.retry_ms : x.due_ms;
  return before(now, at) ? (long)((at - now + 999) / 1000) : 0;
}

}  // namespace fc
