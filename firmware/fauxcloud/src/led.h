// led.h -- the on-board status LED. Pure.
//
// The board has no screen, so one mono LED (GPIO2 on the WROOM-32 DevKit) is the only local cue. One pattern at a
// time, highest active condition wins:
//   ResetCommit  10 Hz strobe          factory reset triggered, about to wipe + reboot
//   ButtonHeld   one blink per second  BOOT is being held: 1, 2, 3, 4 (release at 1-2 = setup again, keeps data);
//                                      from 4.5 s the reset strobe: the next second erases
//   Setup        4 Hz fast blink       setup mode: "set me up" (join the fauxcloud-setup-XXXX Wi-Fi); also while
//                                      a firmware upload is written
//   Listening    steady on             listening to a new console (MQTT stopped on purpose: not an alarm)
//   Booting      heartbeat or 1 Hz     the first 12 s: boot transients (STA not up yet) are not alarms
//   Attention    triple pulse          run mode, something needs a look: MQTT configured but down, admin locked,
//                                      heap low
//   Connecting   1 Hz blink            run mode, the home Wi-Fi is down (the console's AP still works)
//   Ok           500 ms flash / 6 s    all good (calm, for an always-on appliance)
// LED dark = no power or hung.
#pragma once
#include <cstdint>

namespace fc {

enum class LedState : uint8_t { ResetCommit, ButtonHeld, Setup, Booting, Attention, Connecting, Ok, Listening };

struct LedInputs {
  bool reset_commit;
  bool button_held;
  bool setup_mode;
  bool sta_up;
  bool attention;
  uint32_t uptime_ms;
  bool ota;  // a firmware upload is being written: the setup pattern, 4 Hz
  bool capture;  // listening to a new console: a steady light (MQTT is stopped on purpose, not an alarm)
  uint32_t held_ms;  // how long BOOT has been held (HoldButton::heldMs)
};
constexpr uint32_t LED_HELD_STROBE_MS = 4500;

constexpr uint32_t LED_SETTLE_MS = 12000;

LedState ledState(const LedInputs& in);
// Whether the LED is lit at `now_ms` for the state (and, while booting, whether the STA is already up).
bool ledOn(LedState s, uint32_t now_ms, bool sta_up, uint32_t held_ms = 0);

}  // namespace fc
