// netq.h -- the net-out job queue. Every online job (forwarding
// sends, "Send one now", METAR fetches, run once, airport lookups) goes through the one net-out task, one job at a time;
// each source offers candidates on the loop tick and this pure arbiter picks at most one start per tick:
//   NQ_SEND (forwarding send / retry) > NQ_USER (Send one now, METAR run once, airport lookup, WU "Fetch now") >
//   NQ_FETCH (scheduled METAR) > NQ_PULL (the hourly WU forecast pull, the lowest).
// (Reserved, not built: nearby-station pulls -- WU v3/location/near + v2/pws/observations/current, about three stations
// hourly, RAM / JSON only -- would sit below NQ_FETCH; no source, no job kind and no anchor exist for them.)
// A candidate starts when its busy mask is clear, its heap gate passes and the net-out task is idle; the highest
// priority that passes starts, so a higher one held by its heap gate never holds back a lower one that fits. A fetch
// also yields when a send window opens within NQ_SEND_SOON_S. Within a priority: the smaller key first (wrap-safe;
// forwarding passes the due time), equal keys in the offered order (forwarding offers its round-robin order).
#pragma once
#include <cstdint>

#include "fwd_sched.h"

namespace fc {

enum class NqPrio : uint8_t { Send, User, Fetch, Pull };
enum NqSrc : uint8_t { NQ_SRC_FWD, NQ_SRC_METAR, NQ_SRC_WUFC };
constexpr int NQ_MAX = 2 * FWD_SLOTS + 5;  // a send and a test per target; METAR fetch, run once, lookup; WU pull, Fetch now
constexpr long NQ_SEND_SOON_S = 15;
// NqPick.why beyond fwd_sched.h's codes: a fetch held for a send window that opens soon
enum : uint16_t { FW_WHY_SEND_SOON = 0x1000 };

struct NqCand {
  uint8_t src, slot;
  FwdKind kind;
  NqPrio prio;
  uint16_t mask;             // busy bits that must be clear (FB_ALL, or FB_TEST for "Send one now")
  uint32_t need8, largest8;  // the heap gate: free8 and the largest block at least these
  uint32_t key;              // the order within a priority (smaller first, wrap-safe)
};
struct NqIn {
  uint32_t now_ms;
  uint16_t busy;  // FwdBusy bits
  uint32_t free8, largest8;
  bool netout_idle;  // no job running and no unread result
  long send_due_s;   // seconds until the next send window opens (0 = open now), -1 = none
};
struct NqPick {
  int idx;               // the candidate that starts, -1 = none
  uint16_t why[NQ_MAX];  // per candidate: 0 (it starts), the busy bits that held it, or FW_WHY_HEAP / NETOUT / TURN / SEND_SOON
};

// The heap gate of a plain or TLS job (the floors of fwdResourcesOk).
void nqHeapGate(bool tls, FwdTlsClass cls, NqCand& c);
NqPick nqPick(const NqIn& in, const NqCand* c, int n);

}  // namespace fc
