// clock.h -- the clock ladder, pure: which time source wins, when a browser seed is taken, when the
// last-known time is persisted, and what the NTP responder answers. Plain C++, host-compilable; app_core.cpp and
// app_net.cpp wire it to settimeofday, SNTP and NVS (fc_sys "t_last").
//
// Ladder, best first: SNTP (public hosts) > SNTP from DHCP option 42 > browser seed (setup page, admin login) >
// persisted last-known time + uptime ("approximate") > none. The first three set the system clock; the persisted
// time never does (it is off by the whole power-off): it serves our own ordering and the "time approximate" notice,
// never the console.
#pragma once
#include <cstdint>

namespace fc {

// 2026-01-01 00:00 UTC: a clock or a persisted value earlier than this is "no valid time"
constexpr uint32_t TIME_FLOOR = 1767225600u;
constexpr uint32_t TIME_PERSIST_S = 15 * 60;  // t_last cadence
// a browser seed later than the build date + 10 years is a wrong clock, not a time
constexpr uint32_t SEED_SPAN_S = 315576000u;  // 10 x 365.25 days
// Unix seconds of a __DATE__ string ("Nov  4 2026"), 00:00 UTC; 0 when it does not parse.
uint32_t unixFromBuildDate(const char* date);

enum class TimeSrc : uint8_t { None, Persisted, Browser, Dhcp, Sntp };
const char* timeSrcName(TimeSrc s);  // none | persisted | browser | dhcp | sntp
inline bool timeValid(uint32_t unix_s) { return unix_s >= TIME_FLOOR; }

// plausibility against the stored history. `ref` =
// the START (key) of the newest hourly record WITHOUT Q_CLOCK_APPROX in the rings (flash + every station's RAM ring
// and open hour). A valid clock more than CLOCK_BACK_MAX_S behind it is implausible: it is "held" -- treated like no
// real time yet (history writes, NTP answers and t_last wait; nothing is cut; the readings meanwhile reach no hourly
// record: a history gap by design, only the recent ring and live times are re-stamped). A clock minutes off never
// holds. Released when the clock moves forward past ref - 3600, by a seed that itself would not be held,
// or by CLOCK_ACCEPT_S of steady public SNTP (then the records of the open hour and day and later are cut once).
constexpr uint32_t CLOCK_BACK_MAX_S = 3600;
constexpr uint32_t CLOCK_ACCEPT_S = 86400;
constexpr uint32_t CLOCK_STEADY_S = 120;  // "steady": the clock's offset to uptime moved less than this meanwhile
inline bool clockHeld(uint32_t sys_now, uint32_t ref) {
  return timeValid(sys_now) && ref && ref > sys_now + CLOCK_BACK_MAX_S;
}
// the system time a browser seed is judged against. A held clock is replaced only by a seed that
// releases the hold (returns 0 = "no valid clock"); otherwise the clock as it is (an SNTP clock is never overwritten
// by a browser).
inline uint32_t seedJudgeTime(uint32_t sys_now, uint32_t ref, uint32_t seed_s) {
  return clockHeld(sys_now, ref) && !clockHeld(seed_s, ref) ? 0 : sys_now;
}
// The first-sync re-stamp (Stations::clockSynced) is re-armed when a hold starts (readings carry uptime again until the release)
inline bool restampRearm(bool restamped, bool held) { return restamped && held; }
// The hold's bookkeeping (loop(), once a second): counts holds, and accepts a held clock after CLOCK_ACCEPT_S of
// public SNTP whose offset to uptime stayed within CLOCK_STEADY_S. step() returns true once, when it accepts.
struct ClockHold {
  bool held = false;
  uint32_t held_n = 0;           // holds since boot
  uint32_t sntp_since = 0;       // uptime s when the steady public-SNTP run began (0 = none)
  int64_t sntp_offset = 0;       // sys - up at that moment
  bool step(uint32_t sys_now, uint32_t up_s, uint32_t ref, bool public_sntp, bool* started = nullptr);
};

// The state behind the ladder. `sys_src` is what last set the system clock (Browser, Dhcp, Sntp; None = never this
// run); `persisted` is t_last as restored at boot, taken at uptime `persisted_up`.
struct ClockLadder {
  TimeSrc sys_src = TimeSrc::None;
  uint32_t persisted = 0, persisted_up = 0;
  uint32_t written = 0, written_up = 0;  // the last t_last written and when (uptime s); 0 = not yet this run
  bool wrote = false;
  bool written_auth = false;  // what was written came from SNTP/DHCP (a seed or the restored value may be ahead)

  // The source in charge now, given the system clock.
  TimeSrc best(uint32_t sys_now) const;
  // persisted + uptime since the restore; 0 when there is no valid persisted time.
  uint32_t approx(uint32_t up_s) const;
  // The best time available (system clock, else approximate), 0 = none.
  uint32_t bestTime(uint32_t sys_now, uint32_t up_s) const;
  // Browser seed: taken only while neither SNTP nor DHCP set the clock (the SNTP callback marks the
  // source at once), and only a value in [floor, ceil] (ms checked before any narrowing).
  // Also accepted whenever the system clock is invalid, whoever marked it last -- lwIP checks
  // nothing in an NTP reply, so an unsynced router may set a time below the floor
  bool seedAccepted(uint64_t ms, uint32_t ceil, uint32_t sys_now) const;
  // t_last write due: the best time is valid, never earlier than what was written, and 15 min since the last
  // write (or `force`, the planned-restart paths). `authoritative` (the clock is from SNTP/DHCP): the first such
  // time is written at once even when it is earlier (a seed ahead of real time is repaired, not pinned).
  // The caller writes it and calls markWritten.
  bool persistDue(uint32_t best_t, uint32_t up_s, bool force, bool authoritative = false) const;
  void markWritten(uint32_t t, uint32_t up_s, bool authoritative = false) {
    written = t;
    written_up = up_s;
    wrote = true;
    written_auth = written_auth || authoritative;
  }
};

// the SNTP server table. lwIP's dhcp_set_ntp_servers (core 3.3.12's liblwip.a, disassembled) writes
// the router's option-42 server into slot 0 and then clears every slot after it, so the public hosts must be put
// back after each lease. The wanted layout follows the ladder, public SNTP before DHCP:
// the public hosts in slots 0-1, DHCP's server (an address, no name) moved to slot 2 (lwIP polls from slot 0).
constexpr int SNTP_SLOTS = 3;
extern const char* const SNTP_HOST_A;  // "pool.ntp.org"
extern const char* const SNTP_HOST_B;  // "time.google.com"
struct SntpSlot {
  const char* name;  // the configured host name, nullptr = none
  bool has_addr;     // a non-any address (DHCP's server, or a resolved name)
};
// want[i]: the name slot i should carry (nullptr = leave it); *move_dhcp: copy slot 0's DHCP address to slot 2 first.
// Returns the number of changes (names to set + the move).
int sntpPlan(const SntpSlot cur[SNTP_SLOTS], const char* want[SNTP_SLOTS], bool* move_dhcp);
inline bool sntpDhcpSlot(const SntpSlot& s0) { return !s0.name && s0.has_addr; }

// The NTP responder: the system clock when it is valid -- set only by SNTP,
// DHCP-NTP or a browser seed -- else no answer. Never the persisted approximate time: the YT60234 keeps the first
// answer after a cold start and ignores its 100-min re-syncs (5-month and 23-h forward steps not applied), so a
// wrong answer would stay on the console until its next cold start or an NTP off/on in its SET menu.
enum class NtpAnswer : uint8_t { None, System };
NtpAnswer ntpAnswer(uint32_t sys_now);

}  // namespace fc
