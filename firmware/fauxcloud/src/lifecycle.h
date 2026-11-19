// lifecycle.h -- pure state machines of the device lifecycle. Host-compilable.
//
// WifiAttempt: "try these credentials for 60 s" -- used by setup (fail = stay in setup) and by the admin Wi-Fi
// change (fail = revert to the old credentials). The sketch owns the radio; it feeds connection events and the
// clock in and acts on the returned verdict. Failures use a fixed vocabulary:
//   auth (wrong password), no_ap (network not found), dhcp (associated but no address), timeout.
// HoldButton: BOOT (GPIO0) held for 5 s = factory reset, debounced, with a once-per-second countdown.
#pragma once
#include <cstdint>

namespace fc {

// Maps an IDF wifi_err_reason_t value (0 = none seen) and "the STA associated at least once" to the vocabulary.
const char* wifiFailCode(int reason, bool associated);

class WifiAttempt {
 public:
  static constexpr uint32_t TIMEOUT_MS = 60000;
  enum class State : uint8_t { Idle, Trying, Succeeded, Failed };

  void start(uint32_t now_ms);
  // Feed every loop: `connected` = the STA has an IP with the credentials under test; `reason` = the last
  // disconnect reason code seen during this attempt (0 = none); `associated` = the STA reached the AP at least
  // once. Succeeded / Failed are sticky until start() or reset().
  State tick(uint32_t now_ms, bool connected, int reason, bool associated);
  void reset() { state_ = State::Idle; }
  State state() const { return state_; }
  const char* reason() const { return code_; }  // the failure code once Failed

 private:
  State state_ = State::Idle;
  uint32_t start_ = 0;
  int last_reason_ = 0;
  bool associated_ = false;
  const char* code_ = "";
};

// setup mode idle for the time box. Settings complete (an accidental BOOT press, or setup again left
// alone): back to run mode with them; otherwise a restart into setup again (no settings to run with).
enum class SetupIdle : uint8_t { Wait, BackToRun, Restart };
SetupIdle setupIdleAction(uint32_t idle_ms, uint32_t limit_ms, bool settings_complete);

class HoldButton {
 public:
  static constexpr uint32_t HOLD_MS = 5000;
  static constexpr uint32_t DEBOUNCE_MS = 50;
  // released after >= 1 s and < 3 s = setup mode keeping the data; shorter or 3..5 s does nothing
  static constexpr uint32_t SHORT_MIN_MS = 1000, SHORT_MAX_MS = 3000;
  enum : int { NONE = -1, FIRED = 0, SHORT = -2 };
  // Feed samples (any rate >= 20 Hz). Returns NONE, FIRED once when the hold reaches HOLD_MS, SHORT once on a
  // release in the short-press window, or the number of whole seconds held (1..4) once per second for the
  // countdown log. Releasing re-arms.
  int feed(uint32_t now_ms, bool pressed);
  // How long the button has been held (debounced), 0 when released: drives the LED count.
  uint32_t heldMs(uint32_t now_ms) const { return stable_ && !fired_ ? now_ms - press_start_ : 0; }

 private:
  bool raw_ = false, stable_ = false, fired_ = false;
  uint32_t raw_since_ = 0, press_start_ = 0;
  int last_reported_ = 0;
};

}  // namespace fc
