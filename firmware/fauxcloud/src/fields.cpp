// fields.cpp -- the mapping table. Add a row here to understand a new upload parameter.
// HA device/state classes need Home Assistant 2025.4 or newer (wind_direction + measurement_angle); tested on
// 2026.x.
#include "fields.h"

#include <cstring>

namespace fc {

// clang-format off
const FieldDef FIELDS[F_COUNT] = {
  // param            key                 label                unit     conv              agg              bound               scale dec device_class            state_class
  {"tempf",          "temp_c",           "Temperature",       "°C",    Conv::FtoC,       Agg::MinAvgMax,  Bound::Temp,        10,  1, "temperature",          "measurement"},
  {"dewptf",         "dewpt_c",          "Dew point",         "°C",    Conv::FtoC,       Agg::MinAvgMax,  Bound::Temp,        10,  1, "temperature",          "measurement"},
  {"humidity",       "humidity",         "Humidity",          "%",     Conv::None,       Agg::MinAvgMax,  Bound::Humidity,    10,  0, "humidity",             "measurement"},
  {"baromin",        "pressure_hpa",     "Pressure",          "hPa",   Conv::InHgToHpa,  Agg::MinAvgMax,  Bound::Pressure,        10,  1, "atmospheric_pressure", "measurement"},
  {"windspeedmph",   "wind_kmh",         "Wind speed",        "km/h",  Conv::MphToKmh,   Agg::MinAvgMax,  Bound::Wind,        10,  1, "wind_speed",           "measurement"},
  {"windgustmph",    "gust_kmh",         "Wind gust",         "km/h",  Conv::MphToKmh,   Agg::Max,        Bound::Wind,        10,  1, "wind_speed",           "measurement"},
  {"winddir",        "wind_dir",         "Wind direction",    "°",     Conv::None,       Agg::VectorMean, Bound::Direction,         1,  0, "wind_direction",       "measurement_angle"},
  {"rainin",         "rain_mm",          "Rain (last hour)",  "mm",    Conv::InToMm,     Agg::RainSum,    Bound::RainSum,     100, 2, "precipitation",        "measurement"},
  {"dailyrainin",    "rain_daily_mm",    "Rain today",        "mm",    Conv::InToMm,     Agg::Last,       Bound::RainSum,     10,  1, "precipitation",        "total_increasing"},
  {"UV",             "uv",               "UV index",          "",      Conv::None,       Agg::MinAvgMax,  Bound::Uv,          10,  1, nullptr,                "measurement"},
  {"solarRadiation", "solar_wm2",        "Solar radiation",   "W/m²",  Conv::None,       Agg::MinAvgMax,  Bound::Solar,        1,  0, "irradiance",           "measurement"},
  // ---- extended: understood, kept live, not in the recent ring / hourly record yet
  {"indoortempf",    "indoor_temp_c",    "Indoor temperature","°C",    Conv::FtoC,       Agg::MinAvgMax,  Bound::Temp,        10,  1, "temperature",          "measurement"},
  {"indoorhumidity", "indoor_humidity",  "Indoor humidity",   "%",     Conv::None,       Agg::MinAvgMax,  Bound::Humidity,    10,  0, "humidity",             "measurement"},
  {"windchillf",     "windchill_c",      "Wind chill",        "°C",    Conv::FtoC,       Agg::MinAvgMax,  Bound::Temp,        10,  1, "temperature",          "measurement"},
  {"heatindexf",     "heatindex_c",      "Heat index",        "°C",    Conv::FtoC,       Agg::MinAvgMax,  Bound::Temp,        10,  1, "temperature",          "measurement"},
  {"absbaromin",     "pressure_abs_hpa", "Absolute pressure", "hPa",   Conv::InHgToHpa,  Agg::MinAvgMax,  Bound::Pressure,        10,  1, "atmospheric_pressure", "measurement"},
  {"weeklyrainin",   "rain_weekly_mm",   "Rain this week",    "mm",    Conv::InToMm,     Agg::Last,       Bound::RainLong,    10,  1, "precipitation",        "total_increasing"},
  {"monthlyrainin",  "rain_monthly_mm",  "Rain this month",   "mm",    Conv::InToMm,     Agg::Last,       Bound::RainLong,    10,  1, "precipitation",        "total_increasing"},
  {"yearlyrainin",   "rain_yearly_mm",   "Rain this year",    "mm",    Conv::InToMm,     Agg::Last,       Bound::RainLong,    10,  1, "precipitation",        "total_increasing"},
  {"lowbatt",        "battery_low",      "Battery low",       "",      Conv::None,       Agg::Last,       Bound::Battery,      1,  0, nullptr,                nullptr},
  // ---- extended fields (the WU upload article), live only
  {"windgustdir",      "gust_dir",       "Wind gust direction",       "°",    Conv::None,     Agg::VectorMean, Bound::Direction,         1,  0, "wind_direction", "measurement_angle"},
  {"windspdmph_avg2m", "wind_avg2m_kmh", "Wind speed, 2-min average", "km/h", Conv::MphToKmh, Agg::MinAvgMax,  Bound::Wind,        10,  1, "wind_speed",     "measurement"},
  {"winddir_avg2m",    "wind_dir_avg2m", "Wind direction, 2-min average", "°", Conv::None,   Agg::VectorMean, Bound::Direction,         1,  0, "wind_direction", "measurement_angle"},
  {"windgustmph_10m",  "gust_10m_kmh",   "Wind gust, 10 min",         "km/h", Conv::MphToKmh, Agg::Max,        Bound::Wind,        10,  1, "wind_speed",     "measurement"},
  {"windgustdir_10m",  "gust_dir_10m",   "Wind gust direction, 10 min", "°",  Conv::None,     Agg::VectorMean, Bound::Direction,         1,  0, "wind_direction", "measurement_angle"},
  {"visibility",       "visibility_km",  "Visibility",                "km",   Conv::NmToKm,   Agg::MinAvgMax,  Bound::Visibility, 10,  1, "distance",       "measurement"},
  // ---- a rate profile's `rainin` moves here (Stations::ingest); `rainratein` also reaches it directly
  {"rainratein",       "rain_rate_mmh",  "Rain rate",                 "mm/h", Conv::InToMm,   Agg::Last,       Bound::RainRate,    10, 1, "precipitation_intensity", "measurement"},
};
// clang-format on

// ASCII case-insensitive equality (no locale: parameter names are ASCII)
bool sameCi(const char* a, const char* b) {
  for (; *a && *b; a++, b++) {
    char x = *a, y = *b;
    if (x >= 'A' && x <= 'Z') x = (char)(x - 'A' + 'a');
    if (y >= 'A' && y <= 'Z') y = (char)(y - 'A' + 'a');
    if (x != y) return false;
  }
  return *a == *b;
}

int fieldByParam(const char* param) {
  for (int i = 0; i < F_COUNT; i++)
    if (sameCi(FIELDS[i].param, param)) return i;
  return -1;
}

bool isProtocolFlag(const char* param) {
  static const char* const FLAGS[] = {"action", "softwaretype", "realtime", "rtfreq"};
  for (const char* f : FLAGS)
    if (sameCi(f, param)) return true;
  return false;
}

int fieldByKey(const char* key) {
  for (int i = 0; i < F_COUNT; i++)
    if (strcmp(FIELDS[i].key, key) == 0) return i;
  return -1;
}

}  // namespace fc
