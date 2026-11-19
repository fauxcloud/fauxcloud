// history.cpp -- see history.h.
#include "history.h"

#include <cstring>

namespace fc {

static constexpr double PI = 3.14159265358979323846;

int popcount32(uint32_t x) {
  int n = 0;
  for (; x; x &= x - 1) n++;
  return n;
}

int16_t pack(int field, float value) {
  double x = std::round((double)value * FIELDS[field].scale);
  if (x > 32767) x = 32767;
  if (x < -32767) x = -32767;  // -32768 is NO_VALUE
  return (int16_t)x;
}

float unpack(int field, int16_t raw) { return (float)raw / FIELDS[field].scale; }

// ---------------------------------------------------------------- Recent

void Recent::push(const Sample& s, uint32_t t) {
  PackedSample& p = buf_[head_];
  p.t = t;
  for (int f = 0; f < CORE_FIELDS; f++) p.v[f] = s.has(f) ? pack(f, s.v[f]) : NO_VALUE;
  head_ = (head_ + 1) % RECENT_LEN;
  if (count_ < RECENT_LEN) count_++;
}

int Recent::restamp(uint32_t offset) {
  int n = 0;
  for (int i = 0; i < RECENT_LEN; i++)
    if (buf_[i].t && buf_[i].t < EPOCH_MIN) {
      buf_[i].t += offset;
      n++;
    }
  return n;
}

void Recent::clearField(int field) {
  for (PackedSample& p : buf_) p.v[field] = NO_VALUE;
}

const PackedSample& Recent::at(int i) const {
  int oldest = (head_ + RECENT_LEN - count_) % RECENT_LEN;
  return buf_[(oldest + i) % RECENT_LEN];
}

// ---------------------------------------------------------------- Hourly

void Hourly::start(uint32_t hour) {
  memset(&acc_, 0, sizeof acc_);  // padding included: the snapshot CRC covers the bytes
  acc_.hour = hour;
  acc_.t_min = acc_.t_max = acc_.rh_min = acc_.rh_max = acc_.gust_max = acc_.rate_max = acc_.solar_max = acc_.uv_max = NAN;
}

static void minMax(float x, float* mn, float* mx) {
  if (std::isnan(*mn) || x < *mn) *mn = x;
  if (std::isnan(*mx) || x > *mx) *mx = x;
}
static void maxOf(float x, float* mx) {
  if (std::isnan(*mx) || x > *mx) *mx = x;
}

// a reading never reopens an earlier hour: it joins the open hour when that is later, and with no hour
// open it stays out of history when the newest closed hour is later (keys keep rising in the RAM ring and on flash)
bool Hourly::place(uint32_t* hour) {
  if (acc_.hour && *hour < acc_.hour) {
    *hour = acc_.hour;
    back_++;
    return true;
  }
  if (!acc_.hour && count_ && *hour < at(count_ - 1).hour) {
    back_++;
    return false;
  }
  return true;
}

bool Hourly::addRejected(uint32_t t) {
  uint32_t hour = t - t % 3600;
  if (!place(&hour)) return false;
  bool closed = false;
  if (acc_.hour && hour != acc_.hour) closed = closeNow();
  if (!acc_.hour) start(hour);
  acc_.quality |= 1u << Q_REJECTED;
  return closed;
}

bool Hourly::add(const HourIn& in) {
  uint32_t hour = in.t - in.t % 3600;
  if (!place(&hour)) return false;
  bool closed = false;
  if (acc_.hour && hour != acc_.hour) closed = closeNow();
  if (!acc_.hour) start(hour);
  HourAcc& a = acc_;
  // gap: from the hour's start to the first reading, or between two readings
  const uint32_t prev = a.n ? a.last_t : a.hour;
  if (in.t >= prev && in.t - prev >= GAP_S) a.quality |= 1u << Q_GAP;
  a.last_t = in.t;
  if (a.n < 65535) a.n++;
  a.quality |= in.flags & Q_HOUR_MASK;
  auto has = [&](int f) { return ((in.present >> f) & 1u) != 0; };
  if (has(F_TEMP_C)) {
    a.t_sum += in.v[F_TEMP_C];
    a.t_n++;
    minMax(in.v[F_TEMP_C], &a.t_min, &a.t_max);
  }
  if (has(F_DEWPT_C)) {
    a.td_sum += in.v[F_DEWPT_C];
    a.td_n++;
  }
  if (has(F_PRESSURE_HPA)) {
    a.p_sum += in.v[F_PRESSURE_HPA];
    a.p_n++;
  }
  if (has(F_HUMIDITY)) {
    a.rh_sum += in.v[F_HUMIDITY];
    a.rh_n++;
    minMax(in.v[F_HUMIDITY], &a.rh_min, &a.rh_max);
  }
  if (has(F_GUST_KMH)) maxOf(in.v[F_GUST_KMH], &a.gust_max);
  if (has(F_WIND_KMH)) {
    const float s = in.v[F_WIND_KMH];
    a.w_sum += s;
    a.w_n++;
    // calm when the (factored) speed and its 2-min mean (sent or derived) are below CALM_KMH; a calm
    // reading adds no direction, so the parked vane (~335° / 0° in still air) never becomes the hour's wind
    const float sc = std::isnan(in.wind_cal) ? s : in.wind_cal;
    const bool calm = sc < CALM_KMH && (std::isnan(in.wind2m) || in.wind2m < CALM_KMH);
    if (calm) {
      if (a.calm_n < 255) a.calm_n++;
    } else if (has(F_WIND_DIR)) {
      const double d = in.v[F_WIND_DIR] * PI / 180.0;
      a.vx += s * std::sin(d);
      a.vy += s * std::cos(d);
      a.vs += s;
    }
  }
  if (!std::isnan(in.rain_delta)) {
    a.rain_sum += in.rain_delta;
    a.rain_seen = true;
  } else if (in.rain_counter) {
    a.rain_seen = true;  // the counter is known, this reading's rain is not (first value, glitch)
    a.quality |= 1u << Q_RAIN_PARTIAL;
  }
  if (!std::isnan(in.rain_rate)) maxOf(in.rain_rate, &a.rate_max);
  if (has(F_SOLAR_WM2)) {
    a.solar_sum += in.v[F_SOLAR_WM2];
    a.solar_n++;
    maxOf(in.v[F_SOLAR_WM2], &a.solar_max);
  }
  if (has(F_UV)) maxOf(in.v[F_UV], &a.uv_max);
  if (a.n > 1 && in.ch_ver != a.ch_ver) {  // a map change inside the hour: the channels start over
    memset(a.ch_sum, 0, sizeof a.ch_sum);
    memset(a.ch_n, 0, sizeof a.ch_n);
  }
  a.ch_ver = in.ch_ver;
  for (int c = 0; c < HIST_CH; c++) {
    if (std::isnan(in.ch[c])) continue;
    a.ch_sum[c] += in.ch[c];
    a.ch_n[c]++;
    a.ch_scale[c] = in.ch_scale[c];
  }
  return closed;
}

bool Hourly::tick(uint32_t t) {
  const uint32_t hour = t - t % 3600;
  if (acc_.hour && hour > acc_.hour) return closeNow();
  return false;
}

void Hourly::finalize(HourRec* r, bool closing) const {
  const HourAcc& a = acc_;
  clearHour(r, a.hour);
  r->n = (uint8_t)(a.n > 255 ? 255 : a.n);
  r->quality = a.quality;
  if (closing && a.n && a.hour + 3600 > a.last_t && a.hour + 3600 - a.last_t >= GAP_S) r->quality |= 1u << Q_GAP;
  if (a.t_n) {
    r->t_avg = toI16(a.t_sum / a.t_n, 10);
    r->t_min = toI16(a.t_min, 10);
    r->t_max = toI16(a.t_max, 10);
  }
  if (a.td_n) r->td_avg = toI16(a.td_sum / a.td_n, 10);
  if (a.p_n) r->p_avg = toI16(a.p_sum / a.p_n, 10);
  if (a.rh_n) {
    r->rh_avg = toU8(a.rh_sum / a.rh_n, 1);
    r->rh_min = toU8(a.rh_min, 1);
    r->rh_max = toU8(a.rh_max, 1);
  }
  if (a.w_n) r->wind_avg = toU16(a.w_sum / a.w_n, 10);
  r->gust_max = toU16(a.gust_max, 10);
  if (a.vs > 0) {
    double deg = std::atan2(a.vx, a.vy) * 180.0 / PI;
    if (deg < 0) deg += 360.0;
    const double steady = std::sqrt(a.vx * a.vx + a.vy * a.vy) / a.vs;
    r->wdir_steady = packDir((uint16_t)(std::lround(deg) % 360), (uint8_t)std::lround(steady * 127.0));
  }
  r->calm_n = a.calm_n;
  if (a.rain_seen) r->rain_sum = toU16(a.rain_sum, 100);
  r->rain_rate_max = toU8(a.rate_max, 2);
  if (a.solar_n) r->solar_avg = toU16(a.solar_sum / a.solar_n, 1);
  r->solar_max = toU16(a.solar_max, 1);
  r->uv_max = toU8(a.uv_max, 10);
  r->ch_ver = a.ch_ver;
  for (int c = 0; c < HIST_CH; c++)
    if (a.ch_n[c]) r->ch_avg[c] = toI16(a.ch_sum[c] / a.ch_n[c], a.ch_scale[c] > 0 ? a.ch_scale[c] : 1);
  sealHour(r);
}

bool Hourly::closeNow() {
  replaced_ = false;
  if (!acc_.hour) return false;
  const bool any = acc_.n != 0;
  if (any && count_ && at(count_ - 1).hour == acc_.hour) {  // the same clock hour again: one record per hour
    finalize(&ring_[(head_ + HOURLY_LEN - 1) % HOURLY_LEN], true);
    replaced_ = true;
  } else if (any) {
    finalize(&ring_[head_], true);
    head_ = (head_ + 1) % HOURLY_LEN;
    if (count_ < HOURLY_LEN) count_++;
  }
  acc_.hour = 0;
  return any;  // an hour of dropped samples only leaves no record
}

void Hourly::pushRaw(const HourRec& r) {
  if (count_ && at(count_ - 1).hour == r.hour) {
    ring_[(head_ + HOURLY_LEN - 1) % HOURLY_LEN] = r;
    return;
  }
  ring_[head_] = r;
  head_ = (head_ + 1) % HOURLY_LEN;
  if (count_ < HOURLY_LEN) count_++;
}

bool Hourly::openRecord(HourRec* out) const {
  if (!acc_.hour || acc_.n == 0) return false;
  finalize(out, false);
  return true;
}

const HourRec& Hourly::at(int i) const {
  int oldest = (head_ + HOURLY_LEN - count_) % HOURLY_LEN;
  return ring_[(oldest + i) % HOURLY_LEN];
}

// ---------------------------------------------------------------- Daily

int roseSector(float deg) {
  double d = std::fmod((double)deg + 22.5, 360.0);
  if (d < 0) d += 360.0;
  return (int)(d / 45.0) % 8;
}

namespace {
// mean of hourly means weighted by the readings, min / max of extremes
struct Fold {
  double sum = 0, w = 0;
  float mn = NAN, mx = NAN;
  void add(float v, double weight) {
    sum += v * weight;
    w += weight;
  }
  float mean() const { return w > 0 ? (float)(sum / w) : NAN; }
};
float i16v(int16_t x, float scale) { return hasI16(x) ? x / scale : NAN; }
float u16v(uint16_t x, float scale) { return hasU16(x) ? x / scale : NAN; }
float u8v(uint8_t x, float scale) { return hasU8(x) ? x / scale : NAN; }
}  // namespace

bool foldDay(const Hourly& hours, int32_t day, const PosixTz& tz, DayRec* out) {
  clearDay(out, dayNumberToYmd(day));
  Fold t, td, p, rh, wind, solar, ch[HIST_CH];
  float gust = NAN, uv = NAN, rate = NAN;
  double vx = 0, vy = 0, vw = 0, rain = 0;
  bool rainSeen = false, any = false;
  uint32_t n = 0;
  int last = -1;  // the newest hour of the day: its channel map is the day's
  for (int i = 0; i < hours.size(); i++)
    if (localDayNumber(tz, hours.at(i).hour) == day) last = i;
  if (last < 0) return false;
  const uint8_t chVer = hours.at(last).ch_ver;
  for (int i = 0; i <= last; i++) {
    const HourRec& h = hours.at(i);
    if (localDayNumber(tz, h.hour) != day) continue;
    any = true;
    const double w = h.n ? h.n : 1;
    n += h.n;
    out->hours++;
    if (!(h.quality & Q_UNCLEAN)) out->clean_hours++;
    out->quality |= h.quality & Q_HOUR_MASK;
    if (hasI16(h.t_avg)) {
      t.add(i16v(h.t_avg, 10), w);
      minMax(i16v(h.t_min, 10), &t.mn, &t.mx);
      minMax(i16v(h.t_max, 10), &t.mn, &t.mx);
    }
    if (hasI16(h.td_avg)) {
      td.add(i16v(h.td_avg, 10), w);
      minMax(i16v(h.td_avg, 10), &td.mn, &td.mx);
    }
    if (hasI16(h.p_avg)) {
      p.add(i16v(h.p_avg, 10), w);
      minMax(i16v(h.p_avg, 10), &p.mn, &p.mx);
    }
    if (hasU8(h.rh_avg)) {
      rh.add(u8v(h.rh_avg, 1), w);
      minMax(u8v(h.rh_min, 1), &rh.mn, &rh.mx);
      minMax(u8v(h.rh_max, 1), &rh.mn, &rh.mx);
    }
    if (hasU16(h.wind_avg)) wind.add(u16v(h.wind_avg, 10), w);
    if (hasU16(h.gust_max)) maxOf(u16v(h.gust_max, 10), &gust);
    if (hasU8(h.uv_max)) maxOf(u8v(h.uv_max, 10), &uv);
    if (hasU8(h.rain_rate_max)) maxOf(u8v(h.rain_rate_max, 2), &rate);
    if (hasU16(h.solar_avg)) solar.add(u16v(h.solar_avg, 1), w);
    if (hasU16(h.solar_max)) maxOf(u16v(h.solar_max, 1), &solar.mx);
    if (hasU16(h.rain_sum)) {
      rain += h.rain_sum;
      rainSeen = true;
      if (h.rain_sum > 0 && out->rain_hours < 255) out->rain_hours++;
    }
    // direction: the hours' vectors weighted by steadiness x mean speed; the rose counts the non-calm hours
    // by their raw direction (calibration rotates on read)
    const uint16_t dir = dirOf(h.wdir_steady);
    if (dir == DIR_NONE && !hasU16(h.wind_avg)) {
      // no wind sent this hour: neither calm nor a direction
    } else if (calmHour(h)) {
      if (out->calm_hours < 255) out->calm_hours++;
    } else {
      uint8_t& r = out->rose[roseSector((float)dir)];
      if (r < 255) r++;
    }
    if (dir != DIR_NONE && hasU16(h.wind_avg)) {
      const double k = steadyOf(h.wdir_steady) / 127.0 * (h.wind_avg / 10.0), a = dir * PI / 180.0;
      vx += k * std::sin(a);
      vy += k * std::cos(a);
      vw += h.wind_avg / 10.0;
    }
    if (h.ch_ver == chVer)
      for (int c = 0; c < HIST_CH; c++)
        if (hasI16(h.ch_avg[c])) {
          const float v = (float)h.ch_avg[c];  // the channel's own scale; min/avg/max stay in it
          ch[c].add(v, w);
          minMax(v, &ch[c].mn, &ch[c].mx);
        }
  }
  if (!any) return false;
  out->n = (uint16_t)(n > 65535 ? 65535 : n);
  out->t_min = toI16(t.mn, 10);
  out->t_avg = toI16(t.mean(), 10);
  out->t_max = toI16(t.mx, 10);
  out->td_min = toI16(td.mn, 10);
  out->td_avg = toI16(td.mean(), 10);
  out->td_max = toI16(td.mx, 10);
  out->p_min = toI16(p.mn, 10);
  out->p_avg = toI16(p.mean(), 10);
  out->p_max = toI16(p.mx, 10);
  out->rh_min = toU8(rh.mn, 1);
  out->rh_avg = toU8(rh.mean(), 1);
  out->rh_max = toU8(rh.mx, 1);
  out->uv_max = toU8(uv, 10);
  out->wind_avg = toU16(wind.mean(), 10);
  out->gust_max = toU16(gust, 10);
  if (vw > 0 && (vx != 0 || vy != 0)) {
    double deg = std::atan2(vx, vy) * 180.0 / PI;
    if (deg < 0) deg += 360.0;
    const double steady = std::sqrt(vx * vx + vy * vy) / vw;
    out->wdir_steady = packDir((uint16_t)(std::lround(deg) % 360), (uint8_t)std::lround(steady * 127.0));
  }
  if (rainSeen) out->rain_sum = (uint16_t)(rain > 65534 ? 65534 : rain);
  out->rain_rate_max = toU8(rate, 2);
  out->solar_avg = toU16(solar.mean(), 1);
  out->solar_max = toU16(solar.mx, 1);
  out->ch_ver = chVer;
  for (int c = 0; c < HIST_CH; c++)
    if (ch[c].w > 0) {
      out->ch[c][0] = toI16(ch[c].mn, 1);
      out->ch[c][1] = toI16(ch[c].mean(), 1);
      out->ch[c][2] = toI16(ch[c].mx, 1);
    }
  sealDay(out);
  return true;
}

bool Daily::addHour(const HourRec& h, const Hourly& hours, const PosixTz& tz) {
  const int32_t day = localDayNumber(tz, h.hour);
  // an earlier day leaves the open day as it is, and never reopens a closed one
  if ((open_ && day < day_) || (!open_ && count_ && dayNumberToYmd(day) < at(count_ - 1).ymd)) return false;
  bool closed = false;
  if (open_ && day != day_) closed = closeNow(hours, tz);
  open_ = true;
  day_ = day;
  return closed;
}

bool Daily::tick(uint32_t now, const Hourly& hours, const PosixTz& tz) {
  if (open_ && localDayNumber(tz, now) > day_) return closeNow(hours, tz);
  return false;
}

bool Daily::closeNow(const Hourly& hours, const PosixTz& tz) {
  if (!open_) return false;
  open_ = false;
  static DayRec r;  // callers are serialized
  if (!foldDay(hours, day_, tz, &r)) return false;
  pushClosed(r);
  return true;
}

bool Daily::openRecord(const Hourly& hours, const PosixTz& tz, DayRec* out) const {
  return open_ && foldDay(hours, day_, tz, out);
}

void Daily::reopen(const Hourly& hours, const PosixTz& tz) {
  open_ = false;
  if (!hours.size()) return;
  const int32_t d = localDayNumber(tz, hours.at(hours.size() - 1).hour);
  if (count_ && at(count_ - 1).ymd >= dayNumberToYmd(d)) return;  // that day is closed already
  open_ = true;
  day_ = d;
}

void Daily::pushClosed(const DayRec& r) {
  if (count_ && at(count_ - 1).ymd == r.ymd) {
    ring_[(head_ + DAILY_LEN - 1) % DAILY_LEN] = r;
    return;
  }
  ring_[head_] = r;
  head_ = (head_ + 1) % DAILY_LEN;
  if (count_ < DAILY_LEN) count_++;
}

const DayRec& Daily::at(int i) const {
  int oldest = (head_ + DAILY_LEN - count_) % DAILY_LEN;
  return ring_[(oldest + i) % DAILY_LEN];
}

}  // namespace fc
