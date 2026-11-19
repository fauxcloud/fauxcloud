// calib.h -- the calibration log. History is stored raw (before the
// offsets and the factors); this log says which calibration applied from when, and the API applies it on read.
// NVS `cl:<id>` holds <= CAL_MAX entries; the first boot without a log writes {0, the current calibration}, a config change
// that alters any offset or factor appends {now, ...}; full -> the oldest drops (its rows then read with the next).
// Plain C++, host-compilable.
#pragma once
#include <cstdint>

#include "records.h"

namespace fc {

struct StationConfig;

struct CalEntry {
  uint32_t t_from;    // unix seconds the entry applies from (0 = always)
  int16_t temp;       // 0.01 °C
  int16_t rh;         // 0.1 %
  int16_t p;          // 0.01 hPa
  int16_t dir;        // 0.1 °
  uint16_t wind_f;    // wind factor x 0.001 (1000 = none)
  uint16_t rain_f;    // rain factor x 0.001
};
static_assert(sizeof(CalEntry) == 16, "calibration log entry layout (NVS)");
constexpr int CAL_MAX = 8;
struct CalLog {
  uint8_t n;
  uint32_t appends;     // RAM: entries added since boot (two changes in one second differ in calSig)
  CalEntry e[CAL_MAX];  // oldest first
};

CalEntry calOf(const StationConfig& c, uint32_t t_from);
bool calSame(const CalEntry& a, const CalEntry& b);  // the same calibration values (t_from ignored)
// Appends `e` when its values differ from the newest entry's (or the log is empty); full -> the oldest drops.
// Returns true when the log changed (the caller saves it).
bool calAppend(CalLog* log, const CalEntry& e);
// The entry for a row keyed at `t`: the largest t_from <= t, else the oldest (a dropped entry's rows); nullptr = none.
const CalEntry* calFor(const CalLog& log, uint32_t t);
uint32_t calSig(const CalLog& log);  // changes with every append (the history ETags)
// The row with the calibration applied: T, RH (clamped 0..100), pressure, direction (wrapped; 511 stays), the daily
// rose rotated by whole 45° sectors, wind and gust x wind factor, rain sum and rate x rain factor. Dew point, UV,
// solar and the channels untouched; sentinels stay sentinels.
void calHour(HourRec* r, const CalEntry* c);
void calDay(DayRec* r, const CalEntry* c);
// A daily row's key time for calFor: the local date's 00:00 as UTC (the entry in force at the day's start)
uint32_t calDayTime(uint32_t ymd);

}  // namespace fc
