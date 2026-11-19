// board.h -- the board profile: every GPIO the firmware touches comes from here, nowhere else.
//
// Adding a board is a small job, not a development:
//   1. Add an entry below: a FC_BOARD_ID_<name> number and the `#elif` with its BoardProfile (pins + polarity).
//   2. Add a profile to sketch.yaml with the board's fqbn (PartitionScheme=custom keeps partitions.csv) and build
//      it with the flag: `tools\build.ps1 fauxcloud -Profile <name> -Define FAUXCLOUD_BOARD=<name>` (build.ps1
//      passes it as compiler.cpp.extra_flags, in a build directory of its own).
//   3. Nothing else: led.cpp / lifecycle.cpp are pin-free logic; app_core.cpp and fauxcloud.ino read BOARD.
// The default is the WROOM-32 DevKit, the only board that exists so far (no second profile until one does).
#pragma once

struct BoardProfile {
  const char* name;
  int led_pin;
  bool led_active_low;
  int button_pin;          // hold 5 s = factory reset
  bool button_active_low;  // pressed reads LOW (pull-up enabled)
};

#ifndef FAUXCLOUD_BOARD
#define FAUXCLOUD_BOARD esp32dev
#endif

#define FC_BOARD_ID_esp32dev 1
#define FC_BOARD_CAT_(a, b) a##b
#define FC_BOARD_CAT(a, b) FC_BOARD_CAT_(a, b)

#if FC_BOARD_CAT(FC_BOARD_ID_, FAUXCLOUD_BOARD) == 1
// ESP32-WROOM-32 DevKit: on-board blue LED on GPIO2 (active high, safe as an output after boot), BOOT on GPIO0.
constexpr BoardProfile BOARD = {"esp32dev", 2, false, 0, true};
#else
#error "Unknown FAUXCLOUD_BOARD: add its entry to board.h"
#endif
