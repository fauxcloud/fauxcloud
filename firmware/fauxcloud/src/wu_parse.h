// wu_parse.h -- Weather Underground PWS upload (query string) -> canonical Sample, metric.
//
// Tolerant by design (consoles send odd values): URL-decoding, unpadded `dateutc`, every numeric field parsed
// and validated on its own -- a garbage value drops that field only and sets its reject bit; the rest of the
// sample survives. Unknown parameters are kept as raw strings in `extras`. Plain C++, host-compilable.
#pragma once
#include <cstddef>
#include <cstdint>

#include "fields.h"

namespace fc {

constexpr int MAX_EXTRAS = 4;  // small Sample, deep upload queue
constexpr int STATION_ID_LEN = 16;  // including the terminator
constexpr const char* DEFAULT_STATION_ID = "ST1";

struct Extra {
  char key[16];
  char val[16];
};

struct Sample {
  char station_id[STATION_ID_LEN];  // sanitized: [A-Za-z0-9_-] only, safe for MQTT topics and JSON
  bool id_sanitized;                // the raw ID contained characters that were replaced or cut
  bool has_dateutc;                 // the console's own timestamp parsed (informational only)
  int64_t dateutc;                  // unix seconds, valid if has_dateutc
  float v[F_COUNT];                 // metric values, valid where `present` has the bit
  uint32_t present;                 // bit per Field: value accepted
  uint32_t rejected_parse;          // bit per Field: parameter present but not a clean number
  uint32_t rejected_bounds;         // bit per Field: number outside the sanity bounds
  uint8_t n_extras;
  uint8_t extras_dropped;           // unknown parameters beyond MAX_EXTRAS
  Extra extras[MAX_EXTRAS];

  bool has(int f) const { return (present >> f) & 1u; }
  uint32_t rejected() const { return rejected_parse | rejected_bounds; }
};
// with the extended fields (F_COUNT 26) a Sample is <= 288 B, so the upload queue is 8 deep (fauxcloud.ino)
static_assert(sizeof(Sample) <= 288, "Sample is copied through the upload queue (8 deep)");

// In-place URL decoding: '+' -> ' ', %XX -> byte. Malformed escapes are kept literally.
void urlDecode(char* s);

// Strict number: the whole string must be a finite decimal number (leading/trailing spaces allowed).
// "-117.-5", "", "1e", "nan" are rejected.
bool parseNumber(const char* s, double* out);

// Tolerant WU `dateutc`: "YYYY-M-D H:M:S" with any non-digit separators, 1-2 digit fields
// (already URL-decoded). "now" or anything unparseable returns false.
bool parseDateUtc(const char* s, int64_t* unix_out);

// Days-from-civil helper (proleptic Gregorian), exposed for tests and for formatting.
int64_t unixFromUtc(int y, int mo, int d, int h, int mi, int s);

// Parses a raw query string (the part after '?', still URL-encoded). Never fails: whatever cannot be
// understood is dropped and flagged. Station id defaults to DEFAULT_STATION_ID when absent or empty.
void parseQuery(const char* query, Sample* out);
// the field's parse rule as a predicate (finite as a float, inside its Bound); a restored snapshot value
// is checked with it too
bool fieldInBounds(int field, double x);

// the raw value (URL-encoded, as sent) of the first `key` in the query, matched in any case,
// cut to cap - 1 bytes; "" when missing. The station's upload key (bound on first use) is hashed from this, so an
// over-long key hashes its truncated value instead of the empty one.
void queryValueCi(const char* query, const char* key, char* out, size_t cap);

}  // namespace fc
