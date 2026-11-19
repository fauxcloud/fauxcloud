// records.cpp -- see records.h.
#include "records.h"

#include <cmath>
#include <cstring>

namespace fc {

const char* qualityName(int bit) {
  static const char* const NAMES[Q_COUNT] = {"stale", "baro_uncal", "clock_approx", "restart", "baro_boundary",
                                             "baro_bridge", "gap", "console_reset", "rejected", "rain_partial",
                                             "epoch_split"};
  return bit >= 0 && bit < Q_COUNT ? NAMES[bit] : "";
}

uint8_t crc8(const void* data, size_t len) {
  const uint8_t* p = (const uint8_t*)data;
  uint8_t c = 0xFF;
  for (size_t i = 0; i < len; i++) {
    c ^= p[i];
    for (int k = 0; k < 8; k++) c = (uint8_t)(c & 0x80 ? (c << 1) ^ 0x07 : c << 1);
  }
  return c;
}

void sealHour(HourRec* r) { r->crc = crc8(r, sizeof *r - 1); }
void sealDay(DayRec* r) { r->crc = crc8(r, sizeof *r - 1); }
bool hourValid(const HourRec& r) { return r.crc == crc8(&r, sizeof r - 1); }
bool dayValid(const DayRec& r) { return r.crc == crc8(&r, sizeof r - 1); }

void clearHour(HourRec* r, uint32_t hour) {
  memset(r, 0, sizeof *r);
  r->hour = hour;
  r->t_avg = r->t_min = r->t_max = r->td_avg = r->p_avg = I16_NONE;
  r->wind_avg = r->gust_max = r->rain_sum = r->solar_avg = r->solar_max = U16_NONE;
  r->wdir_steady = packDir(DIR_NONE, 0);
  r->rh_avg = r->rh_max = r->rh_min = r->uv_max = r->rain_rate_max = U8_NONE;
  for (int16_t& c : r->ch_avg) c = I16_NONE;
}

void clearDay(DayRec* r, uint32_t ymd) {
  memset(r, 0, sizeof *r);
  r->ymd = ymd;
  r->t_min = r->t_avg = r->t_max = r->td_min = r->td_avg = r->td_max = r->p_min = r->p_avg = r->p_max = I16_NONE;
  r->rh_min = r->rh_avg = r->rh_max = r->uv_max = r->rain_rate_max = U8_NONE;
  r->wind_avg = r->gust_max = r->rain_sum = r->solar_avg = r->solar_max = U16_NONE;
  r->wdir_steady = packDir(DIR_NONE, 0);
  for (auto& c : r->ch)
    for (int16_t& x : c) x = I16_NONE;
}

int16_t toI16(float v, float scale) {
  if (std::isnan(v)) return I16_NONE;
  const double x = std::round((double)v * scale);
  return (int16_t)(x > 32767 ? 32767 : x < -32767 ? -32767 : x);  // -32768 is the sentinel
}

uint16_t toU16(float v, float scale) {
  if (std::isnan(v)) return U16_NONE;
  const double x = std::round((double)v * scale);
  return (uint16_t)(x > 65534 ? 65534 : x < 0 ? 0 : x);
}

uint8_t toU8(float v, float scale) {
  if (std::isnan(v)) return U8_NONE;
  const double x = std::round((double)v * scale);
  return (uint8_t)(x > 254 ? 254 : x < 0 ? 0 : x);
}

}  // namespace fc
