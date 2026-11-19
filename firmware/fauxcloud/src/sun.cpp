// sun.cpp -- see sun.h. Follows dash.js sunTimes term by term (same constants, same day number), in double.
// Double, because the image already links the double sin / cos / atan2 / fmod / lround (float variants would
// add their own); sin/cos of the declination directly and acos as atan2, so asin and acos are not linked (-2.3 KB).
#include "sun.h"

#include <cmath>

#include "tz.h"

namespace fc {

SunDay sunTimes(uint32_t unix, float lat, float lon, uint32_t* rise, uint32_t* set) {
  if (rise) *rise = 0;
  if (set) *set = 0;
  if (std::isnan(lat) || std::isnan(lon)) return SunDay::NoLocation;
  const double rad = 3.14159265358979323846 / 180, DAY = 86400.0;
  // the day by local mean solar time: a UTC day put rise or set in the wrong day far from 0° E
  const double n = std::floor(unix / DAY + lon / 360.0) - 10957 + .0008, J = n - lon / 360.0;
  const double Mn = std::fmod(357.5291 + .98560028 * J, 360.0);
  const double Cc = 1.9148 * std::sin(Mn * rad) + .02 * std::sin(2 * Mn * rad) + .0003 * std::sin(3 * Mn * rad);
  const double lam = std::fmod(Mn + Cc + 180 + 102.9372, 360.0);
  const double Jt = J + .0053 * std::sin(Mn * rad) - .0069 * std::sin(2 * lam * rad);
  const double sd = std::sin(lam * rad) * std::sin(23.44 * rad), cd = std::sqrt(1 - sd * sd);
  const double cw = (std::sin(-.833 * rad) - std::sin(lat * rad) * sd) / (std::cos(lat * rad) * cd);
  if (cw < -1) return SunDay::PolarDay;
  if (cw > 1) return SunDay::PolarNight;
  const double w = std::atan2(std::sqrt(1 - cw * cw), cw) / rad / 360.0;
  const auto toS = [&](double j) { return (uint32_t)std::lround((j + 10957 - .0008 + .5) * DAY); };
  if (rise) *rise = toS(Jt - w);
  if (set) *set = toS(Jt + w);
  return SunDay::Times;
}

SunState sunAt(uint32_t unix, float lat, float lon) {
  uint32_t r = 0, s = 0;
  switch (sunTimes(unix, lat, lon, &r, &s)) {
    case SunDay::NoLocation: return SunState::Unknown;
    case SunDay::PolarDay: return SunState::Up;
    case SunDay::PolarNight: return SunState::Down;
    case SunDay::Times: break;
  }
  return unix >= r && unix < s ? SunState::Up : SunState::Down;
}

void sunGeo(uint32_t unix, SunGeo* out) {
  // day of the year (UTC), through the date helpers the history already links
  const int32_t days = (int32_t)(unix / 86400);
  const int32_t doy = days - ymdToDayNumber(dayNumberToYmd(days) / 10000 * 10000 + 101) + 1;
  const double hr = (unix % 86400) / 3600.0;
  const double g = 2 * 3.14159265358979323846 / 365 * (doy - 1 + (hr - 12) / 24);
  const double decl = .006918 - .399912 * std::cos(g) + .070257 * std::sin(g) - .006758 * std::cos(2 * g) +
                      .000907 * std::sin(2 * g) - .002697 * std::cos(3 * g) + .00148 * std::sin(3 * g);
  out->sd = std::sin(decl);
  out->cd = std::cos(decl);
  out->eqt = 229.18 * (.000075 + .001868 * std::cos(g) - .032077 * std::sin(g) - .014615 * std::cos(2 * g) -
                       .040849 * std::sin(2 * g));
}

void sunPosG(const SunGeo& g, uint32_t unix, float lat, float lon, double* el, double* az, double* ha_out) {
  const double rad = 3.14159265358979323846 / 180;
  const double tst = (unix % 86400) / 60.0 + g.eqt + 4.0 * lon;  // true solar time, minutes
  const double ha = (tst / 4 - 180) * rad;
  const double sl = std::sin(lat * rad), cl = std::cos(lat * rad);
  const double cz = sl * g.sd + cl * g.cd * std::cos(ha);
  *el = std::atan2(cz, std::sqrt(cz < 1 ? 1 - cz * cz : 0)) / rad;
  double a = std::atan2(std::sin(ha), std::cos(ha) * sl - g.sd / g.cd * cl) / rad + 180;
  *az = a >= 360 ? a - 360 : a;
  if (ha_out) {
    double h = tst / 4 - 180;
    while (h > 180) h -= 360;
    while (h < -180) h += 360;
    *ha_out = h;
  }
}

void sunPos(uint32_t unix, float lat, float lon, double* el, double* az) {
  SunGeo g;
  sunGeo(unix, &g);
  sunPosG(g, unix, lat, lon, el, az);
}

double clearSky(double el) {
  if (el <= 0) return 0;
  const double cz = std::sin(el * 3.14159265358979323846 / 180);
  return 1098 * cz * std::exp(-.057 / cz);
}

}  // namespace fc
