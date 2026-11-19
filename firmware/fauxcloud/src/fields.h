// fields.h -- the canonical field set and the Weather Underground parameter mapping table.
//
// The table is data, not code: one row per known upload parameter. Everything downstream (history, MQTT,
// API, page) iterates this table instead of naming fields. Plain C++, no Arduino/IDF includes.
#pragma once
#include <cstdint>

namespace fc {

// Canonical fields. The first CORE_FIELDS are the ones the YT60234 sends; they are packed into the recent
// ring and the hourly record. The rest are understood (sibling consoles send them) but only kept live.
enum Field : uint8_t {
  F_TEMP_C,
  F_DEWPT_C,
  F_HUMIDITY,
  F_PRESSURE_HPA,
  F_WIND_KMH,
  F_GUST_KMH,
  F_WIND_DIR,
  F_RAIN_MM,
  F_RAIN_DAILY_MM,
  F_UV,
  F_SOLAR_WM2,
  // ---- extended (live only)
  F_INDOOR_TEMP_C,
  F_INDOOR_HUMIDITY,
  F_WINDCHILL_C,
  F_HEATINDEX_C,
  F_PRESSURE_ABS_HPA,
  F_RAIN_WEEKLY_MM,
  F_RAIN_MONTHLY_MM,
  F_RAIN_YEARLY_MM,
  F_BATTERY_LOW,
  // ---- extended fields (the WU upload article): appended so the bits of the older fields never move (the
  // `disabled` masks in NVS are by index)
  F_GUST_DIR,
  F_WIND_AVG2M_KMH,
  F_WIND_DIR_AVG2M,
  F_GUST_10M_KMH,
  F_GUST_DIR_10M,
  F_VISIBILITY_KM,
  // ---- the 7-in-1's `rainin` is a rate; Ecowitt's name for the parameter
  F_RAIN_RATE_MMH,
  F_COUNT
};
constexpr int CORE_FIELDS = F_SOLAR_WM2 + 1;
static_assert(F_COUNT <= 32, "field masks are uint32_t");

bool sameCi(const char* a, const char* b);  // ASCII case-insensitive equality
enum class Conv : uint8_t { None, FtoC, InHgToHpa, MphToKmh, InToMm, NmToKm };
enum class Agg : uint8_t { MinAvgMax, Max, VectorMean, RainSum, Last };
// pressure 300-1100 hPa, directions -360..720 (wrapped by the filter), visibility 0-200 km; every
// value must also be finite
// every field has a maximum (checked after conversion): wind 0..500 km/h, rain counters 0..2000 mm,
// week/month/year 0..30000 mm, rain rate 0..1000 mm/h, UV 0..30, solar 0..2000 W/m2, lowbatt 0..100
enum class Bound : uint8_t { Temp, Humidity, Pressure, Direction, Visibility, Wind, RainSum, RainLong, RainRate, Uv, Solar, Battery };

struct FieldDef {
  const char* param;         // upload parameter name (matched case-insensitively)
  const char* key;           // canonical name: JSON key, MQTT topic leaf, HA object id
  const char* label;         // HA entity name / page label
  const char* unit;          // unit after conversion ("" = none)
  Conv conv;
  Agg agg;
  Bound bound;
  int16_t scale;             // fixed-point factor for the recent ring / hourly record
  uint8_t decimals;          // decimals when published / rendered
  const char* device_class;  // HA sensor device_class (nullptr = none)
  const char* state_class;   // HA state_class (nullptr = none)
};

extern const FieldDef FIELDS[F_COUNT];

// Field index for an upload parameter (any case: solarRadiation = solarradiation), or -1.
int fieldByParam(const char* param);
// WU protocol flags that carry no measurement (action, softwaretype, realtime, rtfreq): never extras.
bool isProtocolFlag(const char* param);
// Field index for a canonical key, or -1.
int fieldByKey(const char* key);

}  // namespace fc
