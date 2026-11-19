// netq.cpp -- see netq.h.
#include "netq.h"

namespace fc {

static bool nqBefore(uint32_t a, uint32_t b) { return (int32_t)(a - b) < 0; }

void nqHeapGate(bool tls, FwdTlsClass cls, NqCand& c) {
  if (!tls) {
    c.need8 = FWD_PLAIN_FREE_MIN;
    c.largest8 = FWD_PLAIN_LARGEST_MIN;
    return;
  }
  const int k = (int)cls < (int)FwdTlsClass::COUNT ? (int)cls : (int)FwdTlsClass::Custom;
  c.need8 = FWD_TLS_P8[k] + FWD_TLS_MARGIN8;
  c.largest8 = FWD_TLS_LARGEST_MIN;
}

NqPick nqPick(const NqIn& in, const NqCand* c, int n) {
  NqPick p;
  p.idx = -1;
  for (int i = 0; i < NQ_MAX; i++) p.why[i] = 0;
  if (n > NQ_MAX) n = NQ_MAX;
  const bool sendSoon = in.send_due_s >= 0 && in.send_due_s <= NQ_SEND_SOON_S;
  for (int i = 0; i < n; i++) {
    const NqCand& x = c[i];
    uint16_t why = 0;
    if (in.busy & x.mask) why = in.busy & x.mask;
    else if (in.free8 < x.need8 || in.largest8 < x.largest8) why = FW_WHY_HEAP;
    else if (!in.netout_idle) why = FW_WHY_NETOUT;
    else if ((x.prio == NqPrio::Fetch || x.prio == NqPrio::Pull) && sendSoon) why = FW_WHY_SEND_SOON;  // a pull too
    p.why[i] = why;
    if (why) continue;
    if (p.idx < 0) {
      p.idx = i;
      continue;
    }
    const NqCand& b = c[p.idx];
    if (x.prio < b.prio || (x.prio == b.prio && nqBefore(x.key, b.key))) p.idx = i;
  }
  for (int i = 0; i < n; i++)
    if (!p.why[i] && i != p.idx) p.why[i] = FW_WHY_TURN;
  return p;
}

}  // namespace fc
