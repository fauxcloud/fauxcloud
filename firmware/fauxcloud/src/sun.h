// sun.h -- the outlook's night rule: sunrise / sunset from latitude and longitude, a 1:1 port of dash.js
// sunTimes (the NOAA-style approximation, sun centre at -0.833°). Within ~2 min of NOAA's tables at mid latitudes;
// polar day and night handled. Plain C++, host-compilable. The learners (learn.cpp) use it too.
#pragma once
#include <cstdint>

namespace fc {

enum class SunDay : uint8_t { Times, PolarDay, PolarNight, NoLocation };
enum class SunState : uint8_t { Unknown, Up, Down };

// The local solar day of `unix` (floor(unix / DAY + lon / 360)): rise and set in unix seconds (only for Times; else 0).
SunDay sunTimes(uint32_t unix, float lat, float lon, uint32_t* rise, uint32_t* set);
// Is the sun up at `unix`? Between that UTC day's rise and set; polar day / night; Unknown without a location.
SunState sunAt(uint32_t unix, float lat, float lon);

// for the learners: the sun's position, NOAA low-precision (the formulas of
// tools/forecast_trial.py sun_elevation, ~0.5°) + the azimuth. SunGeo = the declination and the equation of time at one
// instant, computed once per hour; sunPosG then needs only the hour angle per step. Degrees; azimuth from north,
// clockwise; `ha` = the hour angle (negative before solar noon). Double, atan2 only (no asin / acos: they are not linked, see sun.cpp).
struct SunGeo {
  double sd, cd, eqt;  // sin / cos of the declination, equation of time (minutes)
};
void sunGeo(uint32_t unix, SunGeo* g);
void sunPosG(const SunGeo& g, uint32_t unix, float lat, float lon, double* el, double* az, double* ha = nullptr);
void sunPos(uint32_t unix, float lat, float lon, double* el, double* az);
// Haurwitz clear-sky global horizontal irradiance (W/m²) at elevation `el` (deg): 1098 cosZ e^(-0.057 / cosZ); 0 at
// or below the horizon
double clearSky(double el);

}  // namespace fc
