// clock.cpp -- see clock.h.
#include "clock.h"

#include <cstring>

namespace fc {

const char* timeSrcName(TimeSrc s) {
  switch (s) {
    case TimeSrc::Persisted: return "persisted";
    case TimeSrc::Browser: return "browser";
    case TimeSrc::Dhcp: return "dhcp";
    case TimeSrc::Sntp: return "sntp";
    default: return "none";
  }
}

TimeSrc ClockLadder::best(uint32_t sys_now) const {
  if (timeValid(sys_now) && sys_src != TimeSrc::None) return sys_src;
  if (timeValid(persisted)) return TimeSrc::Persisted;
  return TimeSrc::None;
}

uint32_t ClockLadder::approx(uint32_t up_s) const {
  if (!timeValid(persisted)) return 0;
  return persisted + (up_s >= persisted_up ? up_s - persisted_up : 0);
}

uint32_t ClockLadder::bestTime(uint32_t sys_now, uint32_t up_s) const {
  if (timeValid(sys_now) && sys_src != TimeSrc::None) return sys_now;
  return approx(up_s);
}

bool ClockLadder::seedAccepted(uint64_t ms, uint32_t ceil, uint32_t sys_now) const {
  const uint64_t t = ms / 1000;
  const bool realClock = timeValid(sys_now) && (sys_src == TimeSrc::Sntp || sys_src == TimeSrc::Dhcp);
  return !realClock && t >= TIME_FLOOR && t <= ceil;
}

uint32_t unixFromBuildDate(const char* d) {
  static const char M[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
  if (!d || strlen(d) < 11) return 0;
  int mon = -1;
  for (int i = 0; i < 12; i++)
    if (strncmp(d, M + 3 * i, 3) == 0) mon = i + 1;
  const int day = (d[4] == ' ' ? 0 : d[4] - '0') * 10 + (d[5] - '0');
  const int year = (d[7] - '0') * 1000 + (d[8] - '0') * 100 + (d[9] - '0') * 10 + (d[10] - '0');
  if (mon < 0 || day < 1 || day > 31 || year < 2026) return 0;
  // days from civil (proleptic Gregorian), as wu_parse's helper
  const int y = year - (mon <= 2);
  const int era = y / 400, yoe = y - era * 400, mp = (mon + 9) % 12;
  const int doy = (153 * mp + 2) / 5 + day - 1, doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return (uint32_t)((int64_t)(era * 146097 + doe - 719468) * 86400);
}

bool ClockLadder::persistDue(uint32_t best_t, uint32_t up_s, bool force, bool authoritative) const {
  if (!timeValid(best_t)) return false;
  if (authoritative && !written_auth && (!wrote || best_t != written)) return true;  // real time wins, once
  if (wrote && best_t <= written) return false;               // never backwards (nor the same value twice)
  if (!wrote && timeValid(persisted) && best_t <= persisted) return false;  // not the restored value again
  return force || !wrote || up_s - written_up >= TIME_PERSIST_S;
}

const char* const SNTP_HOST_A = "pool.ntp.org";
const char* const SNTP_HOST_B = "time.google.com";

static bool sameName(const char* a, const char* b) { return a == b || (a && b && strcmp(a, b) == 0); }

int sntpPlan(const SntpSlot cur[SNTP_SLOTS], const char* want[SNTP_SLOTS], bool* move_dhcp) {
  *move_dhcp = sntpDhcpSlot(cur[0]);  // a fresh lease wrote its server into slot 0 (and cleared 1-2)
  const char* layout[SNTP_SLOTS] = {SNTP_HOST_A, SNTP_HOST_B, nullptr};  // slot 2: DHCP's address or nothing
  int n = *move_dhcp ? 1 : 0;
  for (int i = 0; i < SNTP_SLOTS; i++) {
    want[i] = layout[i] && !sameName(cur[i].name, layout[i]) ? layout[i] : nullptr;
    if (want[i]) n++;
  }
  return n;
}

NtpAnswer ntpAnswer(uint32_t sys_now) { return timeValid(sys_now) ? NtpAnswer::System : NtpAnswer::None; }

bool ClockHold::step(uint32_t sys_now, uint32_t up_s, uint32_t ref, bool public_sntp, bool* started) {
  const bool h = clockHeld(sys_now, ref);
  if (started) *started = h && !held;
  if (h && !held) held_n++;
  held = h;
  if (!h || !public_sntp) {
    sntp_since = 0;
    return false;
  }
  const int64_t off = (int64_t)sys_now - (int64_t)up_s;
  if (!sntp_since || off - sntp_offset > CLOCK_STEADY_S || sntp_offset - off > CLOCK_STEADY_S) {
    sntp_since = up_s ? up_s : 1;  // a new run (or the clock stepped): start over
    sntp_offset = off;
    return false;
  }
  if (up_s - sntp_since < CLOCK_ACCEPT_S) return false;
  sntp_since = 0;
  return true;
}

}  // namespace fc
