// log.h -- the log path's pure rules. Plain C++, host-compilable; the sinks (UART, the RTC boot
// log) stay in app_core.cpp.
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace fc {

// whether a line goes anywhere. An Event line always (UART + boot log); a Chatter line only with the detailed
// log on (the admin's switch). Off, a chatter line is not even formatted: the UART has no TX buffer, so a 250-B
// line on it holds the loop tick ~10 ms (measured).
inline bool logKeeps(bool chatter, bool verbose) { return !chatter || verbose; }

// an Event line that repeats while a fault lasts (MQTT down, a signed-out admin page polling, SNTP below the
// floor, store write failures). The first line of a spell is printed; then at most one per WINDOW_MS, carrying the count
// of events since the previous printed line; a spell ends after WINDOW_MS without the event (the next one is a first
// line again). The events after a spell's last printed line are not reported. One instance per site, one task each.
template <uint32_t W>
struct RateLimitedLineT {
  static constexpr uint32_t WINDOW_MS = W;
  uint32_t last_ms;  // the last event (valid while on)
  uint32_t emit_ms;  // the last printed line
  uint16_t held;     // events not printed since emit_ms
  bool on;           // a spell is running
  // True = print this event. *n = the events since the previous printed line, this one included (1 = a spell's first);
  // *min = the minutes those n events spanned (0 for a first line).
  bool hit(uint32_t now_ms, uint32_t* n, uint32_t* min) {
    if (!on || now_ms - last_ms >= WINDOW_MS) {
      on = true, last_ms = emit_ms = now_ms, held = 0;
      *n = 1, *min = 0;
      return true;
    }
    last_ms = now_ms;
    if (now_ms - emit_ms < WINDOW_MS) {
      if (held < UINT16_MAX) held++;
      return false;
    }
    *n = (uint32_t)held + 1, *min = (now_ms - emit_ms) / 60000;
    emit_ms = now_ms, held = 0;
    return true;
  }
};
using RateLimitedLine = RateLimitedLineT<600000>;
// a source's scheduled jobs while they fail (one forwarding target, the METAR / TAF fetch, the WU pull): the first
// failure of a spell is an Event; the repeats are counted and go out as Chatter, with one Event per REMIND_MS carrying
// the count (rateSuffix), so a service down for hours does not fill the 2-KB boot-log tail; the first success after a
// spell is one Event and ends it. A spell also ends after REMIND_MS without a failure (a target switched off meanwhile).
// One instance per source; its callers never run at the same time.
struct FailSpell {
  static constexpr uint32_t REMIND_MS = 3600000;
  RateLimitedLineT<REMIND_MS> line;
  // a failure: true = an Event (*n, *min as RateLimitedLine::hit), false = Chatter
  bool fail(uint32_t now_ms, uint32_t* n, uint32_t* min) { return line.hit(now_ms, n, min); }
  // a success: true = an Event (the first after a spell's failures), false = Chatter
  bool ok(uint32_t now_ms) {
    const bool was = line.on && now_ms - line.last_ms < REMIND_MS;
    line.on = false;
    return was;
  }
};
// the MQTT state lines. A change of direction (up <-> down) prints, so the log ends on the true
// state; a repeat of the same direction (one more failed retry while down) goes through a RateLimitedLine (the count is
// retries, one DISCONNECTED each -- the ERROR of a retry only stores its detail). A broker that flaps: after
// FLAP_LINES printed changes within WINDOW_MS the further changes are held, and settle() (the heartbeat) prints one
// "now up / down (xN changes)" line once the state has been stable for STABLE_MS. One instance; the caller serializes
// event() (MQTT task) and settle() (loop task).
struct MqttStateLine {
  static constexpr uint32_t WINDOW_MS = RateLimitedLine::WINDOW_MS, STABLE_MS = 60000;
  static constexpr uint8_t FLAP_LINES = 3;
  enum Out : uint8_t { None, Change, Repeat };
  bool have, cur_up, printed_up;
  uint8_t win_lines;   // direction lines printed in the current window
  uint16_t held;       // direction changes not printed (a flap)
  uint32_t cur_ms;     // the true state's last change
  uint32_t win_ms;     // the window's start
  RateLimitedLine rep; // the same direction again
  // Change: print the state line (plain). Repeat: print it with rateSuffix(n, min). None: nothing.
  Out event(bool up, uint32_t now_ms, uint32_t* n, uint32_t* min) {
    if (have && up == cur_up) return rep.hit(now_ms, n, min) && *n > 1 ? Repeat : None;
    cur_up = up, cur_ms = now_ms;
    if (!have || now_ms - win_ms >= WINDOW_MS) win_ms = now_ms, win_lines = 0;
    have = true;
    if (win_lines >= FLAP_LINES) {
      if (held < UINT16_MAX) held++;
      return None;
    }
    win_lines++, held = 0, printed_up = up;
    rep = RateLimitedLine{};
    rep.hit(now_ms, n, min);  // this line opens the repeat spell
    return Change;
  }
  // True = print "MQTT now up|down (xN changes)": changes were held and the state has been stable for STABLE_MS.
  bool settle(uint32_t now_ms, uint32_t* n, bool* up) {
    if (!held || now_ms - cur_ms < STABLE_MS) return false;
    *n = held, *up = cur_up;
    held = 0, printed_up = cur_up;
    uint32_t rn, rm;
    rep = RateLimitedLine{};
    rep.hit(now_ms, &rn, &rm);  // this line opens the repeat spell of the settled state
    return true;
  }
};
// the AP's client lines. A change -- the other direction (joined <-> left), or the same direction from another client
// (MAC) -- prints, so the log ends on the true state and a second client's join is not taken for a repeat; only a repeat
// (the same direction from the same client) goes through a RateLimitedLine. A client that flaps: after FLAP_LINES printed
// changes within WINDOW_MS the further changes are held, and settle() (the heartbeat) prints one "now joined / left
// (xN changes)" line once the state has been stable for STABLE_MS (the log still ends on the true state, and a flap
// every 30 s no longer fills the 2-KB tail). The DHCP lease rides with the join: lease() is true once after a printed
// "joined" line, never on its own. The caller serializes event() (Wi-Fi event task) and settle() (loop task).
// Scope: one last-client record, not a map -- two clients alternating (a phone and the console) count as changes and share
// one cap, the settle line names the last client only, and a window's held count is dropped by the next printed change
// (as MqttStateLine's), so a long flap's "xN" undercounts.
struct ApLine {
  static constexpr uint32_t WINDOW_MS = RateLimitedLine::WINDOW_MS, STABLE_MS = 60000;
  static constexpr uint8_t FLAP_LINES = 4;
  enum Out : uint8_t { None, Change, Repeat };
  bool have, joined, lease_due;
  uint8_t mac[6];       // the client of the last event
  uint8_t win_lines;    // change lines printed in the current window
  uint16_t held;        // changes not printed (a flap)
  uint32_t cur_ms;      // the true state's last change
  uint32_t win_ms;      // the window's start
  RateLimitedLine rep;  // the same client, the same direction again
  // Change: print the line (plain). Repeat: print it with rateSuffix(n, min). None: nothing.
  Out event(bool join, const uint8_t m[6], uint32_t now_ms, uint32_t* n, uint32_t* min) {
    Out o;
    bool same = have && join == joined;
    for (int i = 0; same && i < 6; i++) same = mac[i] == m[i];
    if (same) {
      o = rep.hit(now_ms, n, min) ? (*n > 1 ? Repeat : Change) : None;
    } else {
      if (!have || now_ms - win_ms >= WINDOW_MS) win_ms = now_ms, win_lines = 0;
      have = true, joined = join, cur_ms = now_ms;
      for (int i = 0; i < 6; i++) mac[i] = m[i];
      if (win_lines >= FLAP_LINES) {
        if (held < UINT16_MAX) held++;
        o = None;
      } else {
        win_lines++, held = 0;
        rep = RateLimitedLine{};
        rep.hit(now_ms, n, min);  // this line opens the repeat spell
        o = Change;
      }
    }
    lease_due = join && o != None;
    return o;
  }
  // True = print "now joined|left MAC <mac> (xN changes)": changes were held and the state has been stable for STABLE_MS.
  bool settle(uint32_t now_ms, uint32_t* n, bool* join, uint8_t m[6]) {
    if (!held || now_ms - cur_ms < STABLE_MS) return false;
    *n = held, *join = joined;
    for (int i = 0; i < 6; i++) m[i] = mac[i];
    held = 0;
    uint32_t rn, rm;
    rep = RateLimitedLine{};
    rep.hit(now_ms, &rn, &rm);  // this line opens the repeat spell of the settled state
    return true;
  }
  bool lease() {
    const bool due = lease_due;
    lease_due = false;
    return due;
  }
};
// " (x12 in 10 min)" after a summary line, "" after a spell's first line. buf: at least 32 B.
inline const char* rateSuffix(char* buf, size_t cap, uint32_t n, uint32_t min) {
  if (n <= 1) return "";
  snprintf(buf, cap, " (x%lu in %lu min)", (unsigned long)n, (unsigned long)min);
  return buf;
}

}  // namespace fc
