// calib.cpp -- see calib.h.
#include "calib.h"

#include <cmath>
#include <cstring>

#include "config.h"
#include "tz.h"

namespace fc {

static int16_t q16(float v, float scale) {
  const float x = std::round(v * scale);
  return (int16_t)(x > 32767 ? 32767 : x < -32767 ? -32767 : x);
}
static uint16_t qf(float f) {
  if (!(f > 0)) return 1000;
  const float x = std::round(f * 1000);
  return (uint16_t)(x > 65535 ? 65535 : x < 1 ? 1 : x);
}

CalEntry calOf(const StationConfig& c, uint32_t t_from) {
  CalEntry e = {};
  e.t_from = t_from;
  e.temp = q16(c.temp_c_offset, 100);
  e.rh = q16(c.humidity_offset, 10);
  e.p = q16(c.pressure_hpa_offset, 100);
  e.dir = q16(c.wind_dir_offset, 10);
  e.wind_f = qf(c.wind_factor);
  e.rain_f = qf(c.rain_factor);
  return e;
}

bool calSame(const CalEntry& a, const CalEntry& b) {
  return a.temp == b.temp && a.rh == b.rh && a.p == b.p && a.dir == b.dir && a.wind_f == b.wind_f && a.rain_f == b.rain_f;
}

bool calAppend(CalLog* log, const CalEntry& e) {
  if (log->n > CAL_MAX) log->n = 0;  // a damaged blob: start over
  if (log->n && calSame(log->e[log->n - 1], e)) return false;
  log->appends++;
  if (log->n && e.t_from <= log->e[log->n - 1].t_from) {  // same second (or a clock behind): replaces the newest
    const uint32_t t = log->e[log->n - 1].t_from;
    log->e[log->n - 1] = e;
    log->e[log->n - 1].t_from = t;
    return true;
  }
  if (log->n == CAL_MAX) {
    memmove(log->e, log->e + 1, sizeof(CalEntry) * (CAL_MAX - 1));
    log->n--;
  }
  log->e[log->n++] = e;
  return true;
}

const CalEntry* calFor(const CalLog& log, uint32_t t) {
  if (!log.n || log.n > CAL_MAX) return nullptr;
  int k = 0;
  for (int i = 0; i < log.n; i++)
    if (log.e[i].t_from <= t) k = i;
  return &log.e[k];
}

uint32_t calSig(const CalLog& log) {
  if (!log.n || log.n > CAL_MAX) return 0;
  return (log.n + 256u * log.appends) * 2654435761u ^ log.e[log.n - 1].t_from;
}

uint32_t calDayTime(uint32_t ymd) { return (uint32_t)(ymdToDayNumber(ymd) * 86400); }

static void addT(int16_t* v, int16_t off01) {  // 0.1 °C / 0.1 hPa record + a 0.01 offset
  if (*v == INT16_MIN || !off01) return;
  const int32_t x = *v + (int32_t)std::lround(off01 / 10.0);
  *v = (int16_t)(x > 32767 ? 32767 : x < -32767 ? -32767 : x);
}
static void addRh(uint8_t* v, int16_t off) {  // 1 % record + a 0.1 % offset, clamped 0..100
  if (*v == 0xFF || !off) return;
  const int32_t x = *v + (int32_t)std::lround(off / 10.0);
  *v = (uint8_t)(x < 0 ? 0 : x > 100 ? 100 : x);
}
static void mulU16(uint16_t* v, uint16_t f) {
  if (*v == 0xFFFF || f == 1000) return;
  const uint32_t x = (uint32_t)(((uint64_t)*v * f + 500) / 1000);
  *v = (uint16_t)(x > 0xFFFE ? 0xFFFE : x);
}
static void mulU8(uint8_t* v, uint16_t f) {
  if (*v == 0xFF || f == 1000) return;
  const uint32_t x = ((uint32_t)*v * f + 500) / 1000;
  *v = (uint8_t)(x > 0xFE ? 0xFE : x);
}
static void addDir(uint16_t* w, int16_t off) {
  const uint16_t d = dirOf(*w);
  if (d == DIR_NONE || !off) return;
  const int32_t x = ((d + (int32_t)std::lround(off / 10.0)) % 360 + 360) % 360;
  *w = packDir((uint16_t)x, steadyOf(*w));
}

void calHour(HourRec* r, const CalEntry* c) {
  if (!c) return;
  addT(&r->t_min, c->temp);
  addT(&r->t_avg, c->temp);
  addT(&r->t_max, c->temp);
  addRh(&r->rh_min, c->rh);
  addRh(&r->rh_avg, c->rh);
  addRh(&r->rh_max, c->rh);
  addT(&r->p_avg, c->p);
  addDir(&r->wdir_steady, c->dir);
  mulU16(&r->wind_avg, c->wind_f);
  mulU16(&r->gust_max, c->wind_f);
  mulU16(&r->rain_sum, c->rain_f);
  mulU8(&r->rain_rate_max, c->rain_f);
}

void calDay(DayRec* r, const CalEntry* c) {
  if (!c) return;
  addT(&r->t_min, c->temp);
  addT(&r->t_avg, c->temp);
  addT(&r->t_max, c->temp);
  addRh(&r->rh_min, c->rh);
  addRh(&r->rh_avg, c->rh);
  addRh(&r->rh_max, c->rh);
  addT(&r->p_min, c->p);
  addT(&r->p_avg, c->p);
  addT(&r->p_max, c->p);
  addDir(&r->wdir_steady, c->dir);
  // the rose by whole 45° sectors (raw bins, rotated on read; <= 22.5° granularity)
  const int k = ((int)std::lround(c->dir / 450.0) % 8 + 8) % 8;
  if (k) {
    uint8_t rose[8];
    for (int i = 0; i < 8; i++) rose[(i + k) % 8] = r->rose[i];
    memcpy(r->rose, rose, sizeof rose);
  }
  mulU16(&r->wind_avg, c->wind_f);
  mulU16(&r->gust_max, c->wind_f);
  mulU16(&r->rain_sum, c->rain_f);
  mulU8(&r->rain_rate_max, c->rain_f);
}

}  // namespace fc
