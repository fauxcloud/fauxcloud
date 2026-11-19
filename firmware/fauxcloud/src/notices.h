// notices.h -- fauxcloud "comments" in plain English, one aggregator
// per station and one for the bridge. The same items feed the dashboard chips (/api/live, /api/bridge/status) and
// the HA "Notices" sensors (state = the sentences joined by " ", attributes = the ids and since when). The machine
// flags stay the problem binary_sensors. Plain C++, host-compilable.
#pragma once
#include <cstddef>
#include <cstdint>

#include "tz.h"

namespace fc {

class Out;
struct Station;

// One table: id (stable, for the attributes) -> text. %s = local HH:MM, %lu = a count.
enum NoticeKind : uint8_t {
  NK_SENSOR_STALE,   // sensor_stale
  NK_SENSOR_ABSENT,  // sensor_absent: the set absent (a console cold start without it)
  NK_BARO_UNCAL,     // baro_uncalibrated
  NK_KEY_MISMATCH,   // key_mismatch
  NK_CONSOLE_CLOCK,  // console_clock (count = the skew in seconds, as int32)
  NK_TIME_APPROX,    // time_approximate (bridge)
  NK_TIME_NONE,      // time_none (bridge)
  NK_HEAP_LOW,       // heap_low (bridge)
  NK_STORE_FAIL,     // store_fail (bridge)
  NK_HEAP_FRAG,      // heap_frag_restart (bridge): this boot came from the fragmentation watchdog
  NK_PROFILE_CHANGED,  // profile_changed: the station's profile changed, for 24 h (RAM)
  NK_STORE_LAYOUT,   // store_layout (bridge): another partition table or ring layout, history RAM only
  NK_CLOCK_BEHIND,   // clock_behind (bridge): a valid clock behind the stored history is held
  NK_BARO_REF,       // baro_ref: the station's pressure differs from the nearby airports (a = d, b = target, count = ref_t)
  NK_BARO_CHECK,     // baro_check (L7): the pressure has averaged far from the usual here (count: baroCheckCount)
  NK_COUNT
};
// a chip is a short label; the sentence is the chip's tooltip and the HA state
struct NoticeDef {
  const char* id;
  const char* chip;
  const char* text;
};
extern const NoticeDef NOTICES[NK_COUNT];

constexpr int NOTICES_MAX = 6;  // 5 station notices possible, one spare (one pressure notice at
                                // most: baro_uncalibrated > baro_ref > baro_check)
constexpr size_t NOTICE_TEXT_MAX = 208;  // noticeText's buffers (baro_ref's sentence is 188 B at its worst case, host-printed;
                                         // baro_check's 191 B with the largest count)
constexpr size_t NOTICE_STATE_MAX = 255;  // HA state limit; longer is cut with "…"
struct Notice {
  uint8_t kind;
  uint32_t since;  // unix seconds (or uptime seconds before the clock is set); 0 = unknown
  uint32_t count;  // for %lu; baro_ref: the reference report's time; baro_uncalibrated: the last good value's time
  // two pressures in 0.1 hPa (REL_NONE = absent): baro_ref a = d (station - airports, median of 3), b = the target;
  // baro_uncalibrated a = the last good value, b = its target (both REL_NONE when none <= 3 h)
  int16_t a, b;
  int16_t q;  // baro_ref's reference QNH (0.1 hPa), the airports' own number for the sheet; else 0
};
struct Notices {
  uint8_t n;
  Notice item[NOTICES_MAX];
  void add(uint8_t kind, uint32_t since, uint32_t count = 0, int16_t a = 0, int16_t b = 0, int16_t q = 0) {
    if (n < NOTICES_MAX) item[n++] = Notice{kind, since, count, a, b, q};
  }
};

int32_t clockSkewShown(int32_t skew_s);  // the skew rounded to the text's unit
// baro_check's count: the offset rounded to 5 hPa (signed, hPa) and the days behind it, packed
uint32_t baroCheckCount(int off_hpa5, uint32_t days);
// A change signature over the items (kind, since, count): the MQTT side publishes on change.
uint32_t noticesSig(const Notices& ns);
// The sentence of one notice. Its time is HH:MM in the local zone, with the day when it is not today's ("yesterday 14:05",
// "16 Nov 14:05"; `now` = unix seconds, 0 = no real time: HH:MM alone); "--:--" before the clock was set.
int noticeText(char* buf, size_t cap, const Notice& x, const PosixTz& tz, uint32_t now);
// HA state: the sentences joined by " " (every text is a sentence), "OK" when there are none; when they pass 255
// bytes, the chips joined instead ("Waiting for time. Low memory."), and only those cut with "…" (UTF-8 safe).
int noticesState(char* buf, size_t cap, const Notices& ns, const PosixTz& tz, uint32_t now);
// the codes and their parameters, in the attributes and the API alike: "since" (ISO UTC, omitted when
// unknown), "n" (key_mismatch, store_fail), "skew_s" (console_clock = clockSkewShown); baro_ref "d_hpa",
// "qnh_hpa", "target_hpa" (null while the reference is not current), "ref_t"; baro_uncalibrated "last_hpa", "target_hpa",
// "last_t" (only with a last good value <= 3 h old); baro_check "off_hpa", "days".
// HA attributes: {"items":[{"id":"...","since":"<ISO UTC>","n":12},...]}
int noticesAttributes(char* buf, size_t cap, const Notices& ns);
// /api: [{"id":"...","since":"...","chip":"...","text":"..."},...] (chip/text: English convenience, kept)
void noticesJson(Out& o, const Notices& ns, const PosixTz& tz, uint32_t now);

constexpr uint32_t PROFILE_NOTICE_S = 86400;  // profile_changed shows for 24 h (counted on the station's uploads)
// The station's items now: sensor stale / absent, pressure at the factory value, uploads with another key, the
// console clock, a profile change in the last 24 h (measured at the last upload, so the live ETag holds).
Notices stationNotices(const Station& st);
// The bridge's: time approximate / none (time_src), low memory, history writes given up.
struct BridgeNoticeIn {
  bool time_valid, time_approx;  // system clock valid; else the persisted approximate time
  uint32_t boot_t;               // "since" for the time items (the boot, unix or 0)
  bool heap_low;
  uint32_t heap_low_since;
  uint32_t store_fail;
  bool frag_restart;  // this boot is a "heap_frag" restart and the uptime is < 24 h ("since" = boot_t)
  bool store_layout;  // the flash does not match this firmware's layout (never formatted): history RAM only
  bool clock_held;    // the clock is behind the stored history and held (replaces time_none then)
};
Notices bridgeNotices(const BridgeNoticeIn& in);

}  // namespace fc
