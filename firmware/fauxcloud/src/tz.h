// tz.h -- POSIX TZ strings ("CET-1CEST,M3.5.0,M10.5.0/3") evaluated in plain C++.
//
// Day boundaries of the daily aggregates use local time. The device also sets TZ via setenv/tzset, but the logic
// here does not depend on the C library, so the DST rollover is host-testable (MSVC has no POSIX TZ rules) and
// both builds agree. Supported: std/dst names (alphabetic or <+01>), offsets [+-]hh[:mm[:ss]], Mm.w.d rules with
// an optional /time (also negative or > 24 h). Not supported: Jn / n day-of-year rules (parse fails).
#pragma once
#include <cstdint>

namespace fc {

constexpr const char* DEFAULT_TZ = "CET-1CEST,M3.5.0,M10.5.0/3";

struct PosixTz {
  int32_t std_off;   // seconds to ADD to UTC to get local standard time (CET-1 -> +3600)
  int32_t dst_off;   // same for daylight time
  bool has_dst;
  struct Rule {
    uint8_t month;   // 1..12
    uint8_t week;    // 1..5 (5 = last)
    uint8_t wday;    // 0 = Sunday
    int32_t time;    // seconds after local midnight (wall clock of the period being left)
  } start, end;
};

// Returns false (and leaves *out untouched) when the string is not understood.
bool parsePosixTz(const char* s, PosixTz* out);

// Seconds to add to UTC for local wall-clock time at the instant utc.
int32_t localOffset(const PosixTz& tz, int64_t utc);

// Local calendar day of utc as days since 1970-01-01 (local), and as YYYYMMDD.
int32_t localDayNumber(const PosixTz& tz, int64_t utc);
uint32_t dayNumberToYmd(int32_t days);
int32_t ymdToDayNumber(uint32_t ymd);  // YYYYMMDD -> days since 1970-01-01 (the inverse)

}  // namespace fc
