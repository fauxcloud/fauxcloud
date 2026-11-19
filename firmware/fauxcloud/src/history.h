// history.h -- per-station RAM history: the Recent ring of packed samples, the Hourly
// aggregator that closes one HourRec per clock hour (48 kept in RAM), and Daily, the closed local days (30 in RAM)
// as pure folds over the hours. Longer history is on flash (ring.h, store.h). Plain C++, host-compilable. Not
// thread-safe: the caller serializes access.
#pragma once
#include <cmath>
#include <cstdint>

#include "fields.h"
#include "records.h"
#include "tz.h"
#include "wu_parse.h"

namespace fc {

constexpr int RECENT_LEN = 60;
constexpr int HOURLY_LEN = 48;
constexpr int DAILY_LEN = 30;
constexpr int16_t NO_VALUE = INT16_MIN;
constexpr uint32_t GAP_S = 600;  // quality bit `gap`: this long without a reading (hour edges included)

// Fixed-point packing with the per-field scale from FIELDS (the recent ring); saturates at the int16 range.
int16_t pack(int field, float value);
float unpack(int field, int16_t raw);
inline bool hasValue(int16_t raw) { return raw != NO_VALUE; }

struct PackedSample {
  uint32_t t;                // receive time, unix seconds (ESP clock)
  int16_t v[CORE_FIELDS];    // NO_VALUE where the field was absent or rejected
};
static_assert(sizeof(PackedSample) <= 28, "budget: 28 B per recent sample");

// Any time below this is seconds since boot, not unix time (2001-09-09): stamped before SNTP synced.
constexpr uint32_t EPOCH_MIN = 1000000000u;

// "known parameters": an unknown key the console ever sent, with its last raw value and when (t 0 = empty slot).
constexpr int MAX_KNOWN = 8;
struct KnownKey {
  char key[16];
  char val[16];
  uint32_t t;  // ESP clock of the last upload that carried it (unix, or uptime before SNTP; re-stamped at the sync)
};

class Recent {
 public:
  void push(const Sample& s, uint32_t t);
  int size() const { return count_; }
  const PackedSample& at(int i) const;  // 0 = oldest
  // readings stamped before SNTP carry seconds since boot (t < EPOCH_MIN); at the first sync each gets
  // t + offset (offset = unix time at boot). Returns the readings re-stamped.
  int restamp(uint32_t offset);
  void clearField(int field);  // every reading's value of one core field -> NO_VALUE

 private:
  PackedSample buf_[RECENT_LEN];
  uint16_t head_ = 0;   // next write slot
  uint16_t count_ = 0;
};

// One accepted reading for the hour. Values are raw: after the profile/limit/jump rules, before the whole
// calibration step -- the wind factor and the offsets of temperature, humidity, pressure and direction (the
// calibration log corrects on read). Rain likewise: the deltas and the rate before the rain factor.
struct HourIn {
  uint32_t t;                 // receive time, unix
  uint32_t present;           // core fields in v (stale sensor fields already removed)
  float v[CORE_FIELDS];
  // the calm rule is a decision, not storage: it is judged on the calibrated (factored) speed and 2-min mean, so a
  // factor > 1 cannot make the parked vane "windy"
  float wind_cal;             // the factored speed of this reading, NAN = take v[F_WIND_KMH]
  float wind2m;               // the factored 2-min mean wind (sent or derived), NAN = none
  float rain_delta;           // rain since the previous reading from the daily counter, NAN = unknown
  bool rain_counter;          // the reading carried the daily counter (an unknown delta is then `rain_partial`)
  float rain_rate;            // mm/h, NAN = none
  uint16_t flags;             // QBit bits of this reading (stale, baro_uncal, clock_approx, restart, rejected, ...)
  uint8_t ch_ver;             // the channel map version (NVS ch:<id>)
  float ch[HIST_CH];          // channel values in their unit, NAN = none
  float ch_scale[HIST_CH];    // 10^decimals per channel
};

// The open hour's accumulator: everything the hour needs to continue after a restart (the snapshot carries it raw).
struct HourAcc {
  uint32_t hour;    // unix start, 0 = nothing open
  uint32_t last_t;  // the last reading (gap tracking)
  uint16_t n, quality;
  uint8_t calm_n, ch_ver;
  bool rain_seen;
  uint8_t pad;
  float t_sum, t_min, t_max, td_sum, p_sum, rh_sum, rh_min, rh_max, w_sum, gust_max, rain_sum, rate_max, solar_sum,
      solar_max, uv_max;
  uint16_t t_n, td_n, p_n, rh_n, w_n, solar_n;
  double vx, vy, vs;  // speed-weighted unit vectors of the non-calm readings, and the speed sum
  float ch_sum[HIST_CH], ch_scale[HIST_CH];
  uint16_t ch_n[HIST_CH];
};

class Hourly {
 public:
  // Adds a reading. A reading in another hour than the open one closes the open hour first: returns true when a
  // record was closed (at(size() - 1), or the replaced newest when lastCloseReplaced()).
  bool add(const HourIn& in);
  // A whole sample dropped (profile_humidity): no reading, the hour's `rejected` bit. Same return.
  bool addRejected(uint32_t t);
  // Closes the open hour if t is in a later hour. Call periodically. Returns true when a record was closed.
  bool tick(uint32_t t);
  // Closes the open hour now regardless of the clock (test hook). Returns true when a record was closed.
  bool closeNow();
  // The still-open hour as a record (the end edge not judged yet); false if nothing is open.
  bool openRecord(HourRec* out) const;
  // closing an hour whose start equals the newest record's (debug/roll closed the same clock hour again)
  // replaces that record instead of adding a second one; the caller must not feed the day again then.
  bool lastCloseReplaced() const { return replaced_; }
  // Restore (flash ring, oldest first) and tests: appended as is; the same hour as the newest replaces it.
  void pushRaw(const HourRec& r);
  void clear() { head_ = count_ = 0; }
  // the open hour's accumulator, for the snapshot
  const HourAcc& acc() const { return acc_; }
  void restoreAcc(const HourAcc& a) { acc_ = a; }

  int size() const { return count_; }
  const HourRec& at(int i) const;  // 0 = oldest
  // readings whose hour was earlier than the open hour (joined it) or than the newest closed hour with
  // none open (kept out of history), since boot -- a small backward clock step (SNTP correcting a fast seed)
  uint32_t back() const { return back_; }

 private:
  void start(uint32_t hour);
  void finalize(HourRec* r, bool closing) const;
  // the hour a reading at `hour` goes to; false = none (an hour already closed and none open)
  bool place(uint32_t* hour);
  uint32_t back_ = 0;

  HourRec ring_[HOURLY_LEN];
  uint16_t head_ = 0, count_ = 0;
  HourAcc acc_ = {};
  bool replaced_ = false;
};

// Sector 0..7 (N, NE, ... NW) of a direction in degrees; sector 0 covers 337.5..22.5.
int roseSector(float deg);

// a local day is a pure fold over its closed hours. False when `hours` has no hour of local day `day`.
bool foldDay(const Hourly& hours, int32_t day, const PosixTz& tz, DayRec* out);

// The closed local days (30 in RAM). The open day is not stored: it is the fold of today's closed hours (the hourly
// ring holds 48 h, restored from flash at boot).
class Daily {
 public:
  // After an hour closed (it is the newest of `hours`): an hour of a later local day closes the open day first.
  // Returns true when a day was closed (at(size() - 1)).
  bool addHour(const HourRec& h, const Hourly& hours, const PosixTz& tz);
  // Closes the open day when `now` is in a later local day. Returns true when a day was closed.
  bool tick(uint32_t now, const Hourly& hours, const PosixTz& tz);
  // Closes the open day now (test hook). A date already newest in the ring is replaced.
  bool closeNow(const Hourly& hours, const PosixTz& tz);
  bool openRecord(const Hourly& hours, const PosixTz& tz, DayRec* out) const;
  // The open day after a restore: the local day of the newest hour, unless the ring already has that date closed.
  void reopen(const Hourly& hours, const PosixTz& tz);
  bool isOpen() const { return open_; }
  int32_t openDay() const { return day_; }

  int size() const { return count_; }
  const DayRec& at(int i) const;  // 0 = oldest
  // Restore (flash ring, oldest first) and the closes: a day with the same ymd as the newest one replaces it.
  void pushClosed(const DayRec& r);
  void clear() { head_ = count_ = 0; open_ = false; }

 private:
  DayRec ring_[DAILY_LEN];
  uint16_t head_ = 0, count_ = 0;
  bool open_ = false;
  int32_t day_ = 0;  // local day number of the open day
};

int popcount32(uint32_t x);

}  // namespace fc
