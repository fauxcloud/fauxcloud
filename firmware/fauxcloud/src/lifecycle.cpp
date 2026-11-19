// lifecycle.cpp -- see lifecycle.h.
#include "lifecycle.h"

namespace fc {

SetupIdle setupIdleAction(uint32_t idle_ms, uint32_t limit_ms, bool settings_complete) {
  if (idle_ms <= limit_ms) return SetupIdle::Wait;
  return settings_complete ? SetupIdle::BackToRun : SetupIdle::Restart;
}

const char* wifiFailCode(int reason, bool associated) {
  switch (reason) {
    // wrong password / rejected credentials (IDF wifi_err_reason_t)
    case 2:    // AUTH_EXPIRE
    case 14:   // MIC_FAILURE
    case 15:   // 4WAY_HANDSHAKE_TIMEOUT
    case 17:   // IE_IN_4WAY_DIFFERS
    case 23:   // 802_1X_AUTH_FAILED
    case 202:  // AUTH_FAIL
    case 204:  // HANDSHAKE_TIMEOUT
      return "auth";
    // nothing to join
    case 201:  // NO_AP_FOUND
    case 210:  // NO_AP_FOUND_W_COMPATIBLE_SECURITY
    case 211:  // NO_AP_FOUND_IN_AUTHMODE_THRESHOLD
    case 212:  // NO_AP_FOUND_IN_RSSI_THRESHOLD
      return "no_ap";
    default:
      break;
  }
  return associated ? "dhcp" : "timeout";
}

void WifiAttempt::start(uint32_t now_ms) {
  state_ = State::Trying;
  start_ = now_ms;
  last_reason_ = 0;
  associated_ = false;
  code_ = "";
}

WifiAttempt::State WifiAttempt::tick(uint32_t now_ms, bool connected, int reason, bool associated) {
  if (state_ != State::Trying) return state_;
  if (reason) last_reason_ = reason;
  associated_ |= associated;
  if (connected) {
    state_ = State::Succeeded;
  } else if (now_ms - start_ >= TIMEOUT_MS) {
    state_ = State::Failed;
    code_ = wifiFailCode(last_reason_, associated_);
  }
  return state_;
}

int HoldButton::feed(uint32_t now_ms, bool pressed) {
  if (pressed != raw_) {
    raw_ = pressed;
    raw_since_ = now_ms;
  }
  if (raw_ != stable_ && now_ms - raw_since_ >= DEBOUNCE_MS) {
    stable_ = raw_;
    if (stable_) {
      press_start_ = raw_since_;
      last_reported_ = 0;
      fired_ = false;
    } else if (!fired_) {
      const uint32_t held = raw_since_ - press_start_;  // until the release edge, not the debounce end
      if (held >= SHORT_MIN_MS && held < SHORT_MAX_MS) return SHORT;
    }
  }
  if (!stable_ || fired_) return NONE;
  const uint32_t held = now_ms - press_start_;
  if (held >= HOLD_MS) {
    fired_ = true;
    return FIRED;
  }
  const int secs = (int)(held / 1000);
  if (secs > last_reported_) {
    last_reported_ = secs;
    return secs;
  }
  return NONE;
}

}  // namespace fc
