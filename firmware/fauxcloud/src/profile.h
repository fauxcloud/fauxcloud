// profile.h -- the build profile's constants. Plain C++, host-compilable.
//
// How many consoles one bridge serves is a RAM question, so it is a build-profile constant, not a setting. The plain
// build (tools\build.ps1 -Sketch fauxcloud) is the WROOM-32 release: one station. The PSRAM profile's placeholder is
// -DFC_MAX_STATIONS=2 on the PSRAM board option (fqbn ...,PSRAM=enabled, which defines BOARD_HAS_PSRAM; CI builds it,
// test/host/run.ps1 / run.sh run the suite at 2). On the WROOM-32 option the heap assert below refuses 2 at compile
// time. FC_MAX_STATIONS is independent of FWD_TLS (fwd.h): two constants, never one macro. The station count never
// enters a stored format: the ring and snapshot fingerprints carry RING_SLOTS (ring.h), a profile constant:
// 1 on the WROOM-32, 2 on the PSRAM profile -- never the station count itself.
#pragma once

#ifndef FC_MAX_STATIONS
#define FC_MAX_STATIONS 1
#endif

// the board profile. On the device it is the board option (the PSRAM fqbn defines BOARD_HAS_PSRAM; its build uses
// partitions-psram.csv). On the host the st2 variant (FC_MAX_STATIONS=2) runs the PSRAM profile, the default
// run the WROOM-32 one. It picks the ring layout and the partition table the image expects (ring.h).
#if defined(ARDUINO)
#if defined(BOARD_HAS_PSRAM)
#define FC_PROFILE_PSRAM 1
#else
#define FC_PROFILE_PSRAM 0
#endif
#else
#define FC_PROFILE_PSRAM (FC_MAX_STATIONS > 1)
#endif

// Station slots the board's heap carries (one Station slot is reserved at boot per allowed station). WROOM-32 (no PSRAM):
// 1 -- measured on the board: "[HEAP] 1 of max_stations 1 station slot(s) reserved (9152 B each), free8 169448" at early boot,
// a steady idle free8 of about 52.7 KB and since-boot minima of 17-27 KB under page bursts: a second 9 KB slot does not
// fit. The PSRAM value is a placeholder until it is measured on a PSRAM board. The host has no
// such limit: the PSRAM value.
#if defined(ARDUINO) && !defined(BOARD_HAS_PSRAM)
#define PROFILE_HEAP_SLOTS_MAX 1
#else
#define PROFILE_HEAP_SLOTS_MAX 2  // placeholder until measured on a PSRAM board
#endif
