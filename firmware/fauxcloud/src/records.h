// records.h -- the history records: one closed hour in 48 B, one
// closed local day in 88 B. The same struct is the RAM ring slot, the flash ring record and the API row source (one
// shape). Little-endian, every field naturally aligned, a CRC-8 in the last byte. Values are raw: before the
// station's calibration offsets (applied on read from the calibration log). Plain C++, host-compilable.
#pragma once
#include <cstddef>
#include <cstdint>

namespace fc {

// Sentinels: a field with no value in the record
constexpr int16_t I16_NONE = INT16_MIN;
constexpr uint16_t U16_NONE = 0xFFFF;
constexpr uint8_t U8_NONE = 0xFF;
constexpr uint16_t DIR_NONE = 511;  // wdir_steady's 9-bit direction when there was none (calm, or no wind sent)
constexpr int HIST_CH = 4;          // extra channels per record

// Quality bits (hourly; a day ORs bits 0-9 of its hours and adds bit 10). The names are the API codes.
enum QBit : uint8_t {
  Q_STALE,          // the outdoor set was frozen at a reading of the hour (its fields are not in the record)
  Q_BARO_UNCAL,     // the console's pressure was its factory value
  Q_CLOCK_APPROX,   // the hour was stamped on a browser-seeded clock
  Q_RESTART,        // the bridge booted inside the hour
  Q_BARO_BOUNDARY,  // reserved (an API code), never set: written 0
  Q_BARO_BRIDGE,    // reserved (an API code), never set: written 0
  Q_GAP,            // >= 600 s without a reading, the hour's edges included
  Q_CONSOLE_RESET,  // the daily rain counter reset outside local midnight, or the outdoor set absent
  Q_REJECTED,       // a filter rejected at least one field (or a whole sample)
  Q_RAIN_PARTIAL,   // a rain delta of the hour is unknown (the sum misses it)
  Q_EPOCH_SPLIT,    // daily only: hours of two epochs (never set: the epoch is always 0)
  Q_COUNT
};
const char* qualityName(int bit);
constexpr uint16_t Q_UNCLEAN = 0x7F;  // bits 0-6: an hour with none of them is "clean"
constexpr uint16_t Q_HOUR_MASK = (1u << Q_EPOCH_SPLIT) - 1;

// One closed clock hour (key = its UTC start). 85 fit a 4 KB block.
struct HourRec {
  uint32_t hour;          // 0  unix start of the hour, UTC (the ring key)
  uint16_t quality;       // 4  QBit bits 0-11; bits 12-15 rev (0)
  uint8_t n;              // 6  readings (saturates at 255)
  uint8_t epoch;          // 7  sensor epoch (written 0)
  int16_t t_avg, t_min, t_max;  // 8  0.1 °C
  int16_t td_avg;         // 14 0.1 °C
  int16_t p_avg;          // 16 0.1 hPa
  uint16_t wind_avg;      // 18 0.1 km/h
  uint16_t gust_max;      // 20 0.1 km/h
  uint16_t wdir_steady;   // 22 direction 0..359 (9 bits, 511 none) | R 0..127 << 9 (steadiness)
  uint16_t rain_sum;      // 24 0.01 mm
  uint16_t solar_avg;     // 26 1 W/m²
  uint16_t solar_max;     // 28
  uint8_t rh_avg;         // 30 1 %
  uint8_t rh_max;         // 31
  uint8_t rh_min;         // 32
  uint8_t calm_n;         // 33 calm readings (wind and its 2-min mean < CALM_KMH)
  uint8_t uv_max;         // 34 0.1
  uint8_t rain_rate_max;  // 35 0.5 mm/h
  int16_t ch_avg[HIST_CH];  // 36 channel means, scale per channel (10^decimals)
  uint8_t ch_ver;         // 44 the channel map version these were written under (NVS ch:<id>)
  uint8_t reserved[2];    // 45 0
  uint8_t crc;            // 47 CRC-8 of bytes 0..46
};
static_assert(sizeof(HourRec) == 48, "48 B per hour");
static_assert(offsetof(HourRec, td_avg) == 14 && offsetof(HourRec, wdir_steady) == 22 && offsetof(HourRec, rh_avg) == 30 &&
                  offsetof(HourRec, rain_rate_max) == 35 && offsetof(HourRec, ch_avg) == 36 && offsetof(HourRec, ch_ver) == 44 &&
                  offsetof(HourRec, crc) == 47,
              "the hourly record layout is fixed: a stored history depends on it");

// One closed local day (key = its local date as YYYYMMDD). 46 fit a 4 KB block. A pure fold over the day's hours.
struct DayRec {
  uint32_t ymd;           // 0  local date, YYYYMMDD (the ring key)
  uint16_t quality;       // 4  OR of the hours' bits 0-9, + bit 10
  uint16_t n;             // 6  readings
  uint8_t hours;          // 8  hourly records in the day
  uint8_t clean_hours;    // 9  ... with none of the bits 0-6
  uint8_t epoch;          // 10
  uint8_t calm_hours;     // 11 hours with a calm majority or no direction
  int16_t t_min, t_avg, t_max;     // 12 0.1 °C
  int16_t td_min, td_avg, td_max;  // 18 of the hourly means
  int16_t p_min, p_avg, p_max;     // 24 0.1 hPa, of the hourly means
  uint8_t rh_min, rh_avg, rh_max;  // 30
  uint8_t uv_max;         // 33 0.1
  uint16_t wind_avg;      // 34 0.1 km/h
  uint16_t gust_max;      // 36
  uint16_t wdir_steady;   // 38 as HourRec, from the hours' R-weighted vectors
  uint8_t rose[8];        // 40 non-calm hours per 45° sector (N, NE, ... NW), by the raw hourly direction
  uint16_t rain_sum;      // 48 0.01 mm
  uint16_t solar_avg;     // 50
  uint16_t solar_max;     // 52
  uint8_t rain_rate_max;  // 54 0.5 mm/h
  uint8_t rain_hours;     // 55 hours with rain
  int16_t ch[HIST_CH][3]; // 56 channel min / avg / max of the hourly means
  uint8_t ch_ver;         // 80
  uint8_t reserved[6];    // 81 0
  uint8_t crc;            // 87
};
static_assert(sizeof(DayRec) == 88, "88 B per day");
static_assert(offsetof(DayRec, t_min) == 12 && offsetof(DayRec, rh_min) == 30 && offsetof(DayRec, wind_avg) == 34 &&
                  offsetof(DayRec, rose) == 40 && offsetof(DayRec, rain_sum) == 48 && offsetof(DayRec, ch) == 56 &&
                  offsetof(DayRec, ch_ver) == 80 && offsetof(DayRec, crc) == 87,
              "the daily record layout is fixed: a stored history depends on it");

// CRC-8, poly 0x07, init 0xFF, not reflected
uint8_t crc8(const void* data, size_t len);
void sealHour(HourRec* r);  // computes crc
void sealDay(DayRec* r);
bool hourValid(const HourRec& r);  // crc matches
bool dayValid(const DayRec& r);
void clearHour(HourRec* r, uint32_t hour);  // every field to its sentinel
void clearDay(DayRec* r, uint32_t ymd);

// Fixed-point helpers (round to nearest, saturating; NaN -> the sentinel)
int16_t toI16(float v, float scale);
uint16_t toU16(float v, float scale);
uint8_t toU8(float v, float scale);
inline bool hasI16(int16_t x) { return x != I16_NONE; }
inline bool hasU16(uint16_t x) { return x != U16_NONE; }
inline bool hasU8(uint8_t x) { return x != U8_NONE; }

// wdir_steady: direction 0..359 or DIR_NONE, R = steadiness 0..127
inline uint16_t packDir(uint16_t dir, uint8_t r) { return (uint16_t)((dir & 0x1FF) | ((uint16_t)(r & 0x7F) << 9)); }
inline uint16_t dirOf(uint16_t w) { return w & 0x1FF; }
inline uint8_t steadyOf(uint16_t w) { return (uint8_t)(w >> 9); }
// Calm hour: a calm majority of the readings, or no direction at all
inline bool calmHour(const HourRec& h) { return h.calm_n * 2 > h.n || dirOf(h.wdir_steady) == DIR_NONE; }

// below this the wind has no direction worth naming (the dashboard's CALM_KMH, dash.js)
constexpr float CALM_KMH = 1.0f;

// which keys a record's channels are (a field key "indoor_temp_c" or a promotion key) under map version
// `ver`; scale = 10^decimals. NVS ch:<id> keeps the versions so old rows keep their keys.
constexpr size_t CH_KEY_LEN = 24;
struct ChMap {
  uint8_t ver;  // 0 = no map
  char key[HIST_CH][CH_KEY_LEN];
  float scale[HIST_CH];
};
constexpr int CH_VERSIONS = 4;  // versions kept in NVS (the oldest drops)

}  // namespace fc
