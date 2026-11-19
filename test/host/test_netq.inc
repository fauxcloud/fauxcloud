// test_netq.inc -- the net-out job queue, alone and with the forwarding scheduler.
#include "netq.h"

namespace {
NqCand nqc(uint8_t src, uint8_t slot, FwdKind kind, NqPrio prio, uint16_t mask, bool tls, uint32_t key) {
  NqCand c{src, slot, kind, prio, mask, 0, 0, key};
  nqHeapGate(tls, tls ? FwdTlsClass::Metar : FwdTlsClass::Custom, c);
  return c;
}
NqIn nqi(uint16_t busy = 0, uint32_t free8 = 60000, uint32_t largest8 = 50000, bool idle = true, long due = -1) {
  return NqIn{1000, busy, free8, largest8, idle, due};
}
}  // namespace

static void testNetq() {
  const NqCand send = nqc(NQ_SRC_FWD, 0, FwdKind::Send, NqPrio::Send, FB_ALL, false, 100);
  const NqCand test = nqc(NQ_SRC_FWD, 1, FwdKind::Test, NqPrio::User, FB_TEST, true, 100);
  const NqCand runOnce = nqc(NQ_SRC_METAR, 0, FwdKind::RunOnce, NqPrio::User, FB_ALL, true, 100);
  const NqCand fetch = nqc(NQ_SRC_METAR, 0, FwdKind::Fetch, NqPrio::Fetch, FB_ALL, true, 100);
  {  // priority: send > user > fetch, whatever the offered order; the others wait their turn
    const NqCand c[4] = {fetch, runOnce, test, send};
    NqPick p = nqPick(nqi(), c, 4);
    CHECK(p.idx == 3 && p.why[3] == 0 && p.why[0] == FW_WHY_TURN && p.why[1] == FW_WHY_TURN && p.why[2] == FW_WHY_TURN);
    p = nqPick(nqi(), c, 3);
    CHECK(p.idx == 1);  // user: the first offered of equal keys (run once before the test here)
    p = nqPick(nqi(), c, 1);
    CHECK(p.idx == 0);
    const NqCand u[2] = {test, runOnce};
    CHECK(nqPick(nqi(), u, 2).idx == 0);
  }
  {  // masks: a page request (FB_HTTP) holds sends and every METAR kind, never "Send one now"
    const NqCand c[4] = {send, test, runOnce, fetch};
    NqPick p = nqPick(nqi(FB_HTTP), c, 4);
    CHECK(p.idx == 1 && p.why[0] == FB_HTTP && p.why[2] == FB_HTTP && p.why[3] == FB_HTTP);
    p = nqPick(nqi(FB_UPLOAD | FB_STORE), c, 4);  // the test's own guard holds it too
    CHECK(p.idx == -1 && p.why[1] == FB_UPLOAD && p.why[0] == (FB_UPLOAD | FB_STORE));
    for (uint16_t b : {FB_STORE, FB_LEARN, FB_HOUR, FB_MQTT}) {
      const NqCand f[1] = {fetch};
      CHECK(nqPick(nqi(b), f, 1).idx == -1);
    }
  }
  {  // the FB_HTTP rule: a page request 2 s ago -> no fetch; 3.1 s ago -> fetch; in flight; a shed 59 s ago
    const uint32_t now = 500000;
    CHECK(fwdHttpBusy(now, 0, now - 2000, 0) && !fwdHttpBusy(now, 0, now - 3100, 0) && !fwdHttpBusy(now, 0, 0, 0));
    CHECK(fwdHttpBusy(now, 1, 0, 0) && fwdHttpBusy(now, 0, 0, now - 59000) && !fwdHttpBusy(now, 0, 0, now - 61000));
    const NqCand f[1] = {fetch};
    CHECK(nqPick(nqi(fwdHttpBusy(now, 0, now - 2000, 0) ? FB_HTTP : 0), f, 1).idx == -1);
    CHECK(nqPick(nqi(fwdHttpBusy(now, 0, now - 3100, 0) ? FB_HTTP : 0), f, 1).idx == 0);
  }
  {  // a fetch yields to a send window opening within 15 s; a user job does not
    const NqCand c[2] = {fetch, runOnce};
    NqPick p = nqPick(nqi(0, 60000, 50000, true, 15), c, 1);
    CHECK(p.idx == -1 && p.why[0] == FW_WHY_SEND_SOON);
    CHECK(nqPick(nqi(0, 60000, 50000, true, 0), c, 1).idx == -1);
    CHECK(nqPick(nqi(0, 60000, 50000, true, 16), c, 1).idx == 0);
    CHECK(nqPick(nqi(0, 60000, 50000, true, -1), c, 1).idx == 0);
    CHECK(nqPick(nqi(0, 60000, 50000, true, 5), c + 1, 1).idx == 0);
  }
  {  // a busy net-out task: nothing starts; one start per tick at most
    const NqCand c[3] = {send, test, fetch};
    const NqPick p = nqPick(nqi(0, 60000, 50000, false), c, 3);
    CHECK(p.idx == -1 && p.why[0] == FW_WHY_NETOUT && p.why[1] == FW_WHY_NETOUT && p.why[2] == FW_WHY_NETOUT);
    CHECK(nqPick(nqi(), c, 0).idx == -1);
  }
  {  // heap: a TLS test held by its gate does not hold back a plain send or a fetch that fits; a TLS fetch's gate
    const NqCand plainSend = nqc(NQ_SRC_FWD, 2, FwdKind::Send, NqPrio::Send, FB_ALL, false, 100);
    const NqCand c[2] = {test, plainSend};
    NqPick p = nqPick(nqi(0, 20000, 15000), c, 2);  // 20 KB: plain fits (16 / 8 KB), TLS needs 29 KB + 19.8 KB block
    CHECK(p.idx == 1 && p.why[0] == FW_WHY_HEAP);
    const NqCand tlsSend = nqc(NQ_SRC_FWD, 0, FwdKind::Send, NqPrio::Send, FB_ALL, true, 100);
    const NqCand plainLookup = nqc(NQ_SRC_METAR, 0, FwdKind::Lookup, NqPrio::User, FB_ALL, false, 100);
    const NqCand d[2] = {tlsSend, plainLookup};
    p = nqPick(nqi(0, 20000, 15000), d, 2);
    CHECK(p.idx == 1 && p.why[0] == FW_WHY_HEAP);
    const NqCand f[1] = {fetch};
    CHECK(fetch.need8 == FWD_TLS_P8[(int)FwdTlsClass::Metar] + FWD_TLS_MARGIN8 && fetch.largest8 == FWD_TLS_LARGEST_MIN);
    CHECK(nqPick(nqi(0, fetch.need8 - 1, 50000), f, 1).why[0] == FW_WHY_HEAP);
    CHECK(nqPick(nqi(0, fetch.need8, fetch.largest8 - 1), f, 1).why[0] == FW_WHY_HEAP);
    CHECK(nqPick(nqi(0, fetch.need8, fetch.largest8), f, 1).idx == 0);
  }
  {  // within a priority: the smaller key first (wrap-safe), equal keys in the offered order
    NqCand a = send, b = send;
    a.slot = 0, a.key = 0xFFFFFF00u;  // before the wrap
    b.slot = 1, b.key = 0x00000100u;  // after it
    const NqCand c[2] = {b, a};
    CHECK(nqPick(nqi(), c, 2).idx == 1);
    b.key = a.key;
    const NqCand d[2] = {b, a};
    CHECK(nqPick(nqi(), d, 2).idx == 0);
  }
  {  // with the forwarding scheduler: a METAR job picked -> nothing of ours starts, our due send waits its turn
    FwdSim m;
    m.on(0, 300, false);
    NqCand c[NQ_MAX];
    int n = m.s.offer(m.in, c);
    CHECK(n == 1 && c[0].src == NQ_SRC_FWD && c[0].kind == FwdKind::Send && c[0].prio == NqPrio::Send);
    c[n++] = runOnce;
    NqIn q{m.in.now_ms, FB_HTTP, m.in.free, m.in.largest, true, m.s.sendDueS(m.in.now_ms)};
    NqPick p = nqPick(q, c, n);  // the page request holds both
    CHECK(p.idx == -1 && m.s.picked(m.in, c, n, p).slot < 0 && m.s.s[0].last_why == FB_HTTP);
    q.busy = 0;
    p = nqPick(q, c, n);
    CHECK(p.idx == 0 && m.s.picked(m.in, c, n, p).slot == 0 && m.s.running());  // the send outranks run once
    m.s.result(0, FwdResult::Ok, m.in.now_ms, 0, 1);
    m.advance(1000);
    n = m.s.offer(m.in, c);
    CHECK(n == 0 && m.s.sendDueS(m.in.now_ms) == 299);
    c[n++] = fetch;
    q.now_ms = m.in.now_ms, q.send_due_s = m.s.sendDueS(m.in.now_ms);
    p = nqPick(q, c, n);
    CHECK(p.idx == 0 && m.s.picked(m.in, c, n, p).slot < 0 && !m.s.running());  // the fetch is the loop's to start
    // the send window opens within 15 s: the fetch yields
    m.advance(285000);
    n = m.s.offer(m.in, c);
    c[n++] = fetch;
    q.now_ms = m.in.now_ms, q.send_due_s = m.s.sendDueS(m.in.now_ms);
    CHECK(q.send_due_s == 14 && nqPick(q, c, n).why[n - 1] == FW_WHY_SEND_SOON);
  }
  {  // "Send one now" and a due send on the same tick: the send first, the test on a later tick
    FwdSim m;
    m.on(0, 300, false);
    CHECK(m.tick().kind == FwdKind::Send);
    m.s.result(0, FwdResult::Ok, m.in.now_ms, 0, 1);
    m.on(1, 300, false);
    m.advance(70000);
    CHECK(m.s.requestTest(0, m.in.now_ms, 60, true) == FwdTestReq::Accepted);
    FwdStart st = m.tick();  // slot 1's first window and slot 0's test
    CHECK(st.slot == 1 && st.kind == FwdKind::Send);
    m.advance(2000);
    CHECK(m.tick().slot < 0);  // one connection
    m.s.result(1, FwdResult::Ok, m.in.now_ms, 0, 1);
    st = m.tick();
    CHECK(st.slot == 0 && st.kind == FwdKind::Test);
  }
}
