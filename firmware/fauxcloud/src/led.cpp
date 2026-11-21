// led.cpp -- see led.h.
#include "led.h"

namespace fc {

LedState ledState(const LedInputs& in) {
  if (in.reset_commit) return LedState::ResetCommit;
  if (in.button_held) return in.held_ms >= LED_HELD_STROBE_MS ? LedState::ResetCommit : LedState::ButtonHeld;
  if (in.setup_mode || in.ota) return LedState::Setup;
  if (in.capture) return LedState::Listening;
  if (in.uptime_ms < LED_SETTLE_MS) return LedState::Booting;
  if (!in.sta_up) return LedState::Connecting;
  if (in.attention) return LedState::Attention;
  return LedState::Ok;
}

bool ledOn(LedState s, uint32_t now_ms, bool sta_up, uint32_t held_ms) {
  switch (s) {
    case LedState::ResetCommit: return now_ms % 100 < 50;
    case LedState::ButtonHeld: return held_ms >= 1000 && held_ms % 1000 < 200;  // one blink at each whole second
    case LedState::Setup: return now_ms % 250 < 125;
    case LedState::Booting: return sta_up ? now_ms % 6000 < 500 : now_ms % 1000 < 100;
    case LedState::Attention: {
      const uint32_t p = now_ms % 1600;  // three 120 ms blips, then dark
      return p < 120 || (p >= 240 && p < 360) || (p >= 480 && p < 600);
    }
    case LedState::Connecting: return now_ms % 1000 < 100;
    case LedState::Ok: return now_ms % 6000 < 500;
    case LedState::Listening: return true;
  }
  return false;
}

}  // namespace fc
