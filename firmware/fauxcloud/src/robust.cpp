// robust.cpp -- see robust.h.
#include "robust.h"

#include <cstring>

namespace fc {

MarkerScan::MarkerScan() {
  // assembled here, never written whole: this code must not match itself in the running image
  strcpy(pat_, "FAUXCLOUD");
  strcat(pat_, "-IMAGE:");
  patLen_ = strlen(pat_);
}

void MarkerScan::feed(const uint8_t* data, size_t len) {
  for (size_t i = 0; i < len; i++) {
    const char c = (char)data[i];
    if (copying_) {
      if (c == 0 || vlen_ + 1 >= sizeof version_) copying_ = false;
      else version_[vlen_++] = c;
      continue;
    }
    if (found_) continue;
    if (c == pat_[matched_]) {
      if (++matched_ == patLen_) {
        found_ = copying_ = true;
        matched_ = 0;
      }
    } else {
      // the pattern has no repeated prefix, so a mismatch restarts at 0 (or 1 when c starts the pattern)
      matched_ = c == pat_[0] ? 1 : 0;
    }
  }
}

bool shouldMarkValid(const RollbackInputs& in) {
  return in.pending && in.sta_up && in.http_served && in.uptime_s >= ROLLBACK_HEALTHY_S && !in.wdt_reset;
}

const char* otaStateName(OtaState s) {
  switch (s) {
    case OtaState::Pending: return "pending";
    case OtaState::RolledBack: return "rolled_back";
    default: return "stable";
  }
}

NetAction NetWatchdog::feed(uint32_t now_s, bool sta_ok, bool paused) {
  if (sta_ok) rebooted_ = false;  // the outage is over
  if (sta_ok || paused) {
    down_ = false;
    return NetAction::None;
  }
  if (!down_) {
    down_ = true;
    since_ = lastReinit_ = now_s;
    return NetAction::None;
  }
  if (now_s - since_ >= REBOOT_S && !rebooted_) {
    rebooted_ = true;
    return NetAction::Reboot;
  }
  if (now_s - lastReinit_ >= REINIT_S) {
    lastReinit_ = now_s;
    reinits_++;
    return NetAction::Reinit;
  }
  return NetAction::None;
}

BodyRead readAll(BodyRecv recv, BodyClock nowMs, void* ctx, char* buf, size_t cap, size_t len, size_t* got,
                 size_t drain_max) {
  *got = 0;
  const bool drain = len > cap;
  if (drain && len > drain_max) return BodyRead::TooBig;
  const uint32_t t0 = nowMs(ctx);
  int timeouts = 0;
  while (*got < len) {
    if (nowMs(ctx) - t0 >= BODY_DEADLINE_MS) return BodyRead::Stalled;
    // draining: each piece overwrites buf (cap bytes), the count goes on
    const size_t want = drain ? (len - *got < cap ? len - *got : cap) : len - *got;
    const int r = recv(ctx, drain ? buf : buf + *got, want);
    if (r == BODY_RECV_TIMEOUT) {
      if (++timeouts >= 2) return BodyRead::Stalled;
      continue;
    }
    if (r <= 0) return BodyRead::Lost;
    *got += (size_t)r;  // timeouts are not reset by data -- two per body, not two in a row
  }
  if (drain) return BodyRead::Drained;
  buf[*got] = 0;
  return BodyRead::Ok;
}

const char* bodyReadName(BodyRead r) {
  switch (r) {
    case BodyRead::Ok: return "ok";
    case BodyRead::TooBig: return "too big";
    case BodyRead::Stalled: return "stalled";
    case BodyRead::Drained: return "too big (read and discarded)";
    default: return "connection lost";
  }
}

// esp_reset_reason_t (ESP-IDF 5.x): 0 unknown, 1 power-on, 2 external pin, 3 software, 4 panic, 5 interrupt WDT,
// 6 task WDT, 7 other WDT, 8 deep sleep, 9 brownout, 10 SDIO, 11 USB, 12 JTAG, 13 eFuse, 14 power glitch, 15 CPU lockup
const char* resetReasonName(int r, const char* planned) {
  if (r == 3 && planned && planned[0]) return planned;
  static const char* const N[] = {"unknown", "power_on", "external", "software", "panic", "int_wdt", "task_wdt",
                                  "other_wdt", "deep_sleep", "brownout", "sdio", "usb", "jtag", "efuse",
                                  "power_glitch", "cpu_lockup"};
  return r >= 0 && r < (int)(sizeof N / sizeof N[0]) ? N[r] : "unknown";
}

// 7 (other WDT = the RTC WDT) is also what a reset through the EN pin / esptool's RTS reports on the ESP32, so it does
// not count: a hand reset of a freshly updated image must not block its health check.
bool resetIsWatchdog(int r) { return r == 5 || r == 6; }

int PageSessions::touch(int fd, uint32_t now_ms) {
  int i = 0;
  while (i < n_ && fd_[i] != fd) i++;
  if (i == n_) {
    if (n_ == SLOTS) return -1;  // cannot happen with max_open_sockets 10; never purge blind
    fd_[n_] = fd;
    closing_[n_] = false;
    n_++;
  }
  at_[i] = now_ms;
  int open = 0, idlest = -1;
  for (int k = 0; k < n_; k++) {
    if (closing_[k]) continue;
    open++;
    if (fd_[k] != fd && (idlest < 0 || (int32_t)(at_[k] - at_[idlest]) < 0)) idlest = k;
  }
  if (open <= CAP || idlest < 0) return -1;
  closing_[idlest] = true;  // counted out until its close callback drops it
  return fd_[idlest];
}

void PageSessions::drop(int fd) {
  for (int i = 0; i < n_; i++)
    if (fd_[i] == fd) {
      n_--;
      fd_[i] = fd_[n_];
      at_[i] = at_[n_];
      closing_[i] = closing_[n_];
      return;
    }
}

bool FragWatch::feed(uint32_t now_ms, uint32_t largest, uint32_t uploads, bool ota) {
  if (ota || largest >= FRAG_LARGEST_MIN || (on_ && uploads != uploads_)) {
    on_ = false;
    if (ota || largest >= FRAG_LARGEST_MIN) return false;
  }
  if (!on_) {
    on_ = true;
    since_ = now_ms;
    uploads_ = uploads;
    return false;
  }
  if (now_ms - since_ < FRAG_HOLD_MS || now_ms < quiet_) return false;
  on_ = false;  // fired: a new window starts if the restart does not come
  return true;
}

int effectiveResetReason(int r, bool magic) {
  if (magic) return r;
  // reasons that keep RTC memory and are self-evident: software (3), panic (4), int/task WDT (5, 6), brownout (9)
  if (r == 3 || r == 4 || r == 5 || r == 6 || r == 9) return r;
  return 1;
}

}  // namespace fc
