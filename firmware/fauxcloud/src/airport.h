// airport.h -- airports for the METAR reference, looked up at pick time from aviationweather.gov's
// stationinfo JSON (no shipped table). Integer maths only: positions in centi-degrees, distances in km.
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace fc {

struct Airport {
  char icao[5];
  char name[25];      // the display name: airportShortName() of the `site` text, cut on a UTF-8 boundary
  int16_t lat, lon;   // centi-degrees
  uint16_t km;        // from the configured location, AIRPORT_KM_UNKNOWN without one
};
constexpr uint16_t AIRPORT_KM_UNKNOWN = 0xFFFF;
constexpr int AIRPORT_NEAR_MAX = 5;
constexpr int32_t AIRPORT_BOX_LAT = 110;   // +-1.1 deg (about 120 km)
constexpr int32_t AIRPORT_BOX_LON_MAX = 300;  // +-3 deg at most, near the poles
constexpr int32_t AIRPORT_BOX_SNAP = 10;      // the centre on a 0.1-deg grid (about 10 km), the box 0.1 deg wider

// Equirectangular distance at the mid latitude (a Q15 cosine table, linear per centi-degree): <= 1 % or 1 km under 300 km.
uint32_t geoKm(int32_t lat1, int32_t lon1, int32_t lat2, int32_t lon2);
// "-73.7789" -> -7378 (two decimals, rounded half away from zero); false for anything but [-]digits[.digits]
bool centiFromText(const char* s, int32_t* out);
// The lookup box around a location: lat0, lon0, lat1, lon1 (centi-degrees, clipped to the globe)
void airportBox(int32_t lat, int32_t lon, int32_t box[4]);
// One stationinfo object -> Airport (km unset); false = not a METAR site with a 4-character ICAO, or not readable.
// has_taf (optional): its siteType lists "TAF" (the lookup list shows it; never stored with the chosen airports)
bool airportFromJson(const char* obj, size_t n, Airport* out, bool* has_taf = nullptr);
// an airport's display name from AWC's `site` text, in place: the part after the last '/', without the whole
// words Intl, Arpt, Airport, International; the text unchanged when nothing would be left. Idempotent (also run at load
// over names stored before it existed, in RAM: no blob change).
void airportShortName(char* s);
// for the round flag in the dashboard's airport sheet: the country of an ICAO code from its prefix (three letters,
// else two, else one; one table for both pages) into cc ("" and false when unknown).
// pack = the flags-<pack>.json holding its flag (tz.json's region of the country's first zone; test_table_sync keeps both
// equal), AIRPORT_PACK_NONE when unknown.
constexpr uint8_t AIRPORT_PACK_NONE = 0xFF;
bool airportCountry(const char* icao, char cc[3], uint8_t* pack);

// The streaming body of a stationinfo answer (a JSON array of objects): objects split by brace depth outside strings
// (escape-aware) into a 384-B buffer; a longer object is dropped and counted. With a reference location the nearest
// AIRPORT_NEAR_MAX are kept (ties: the first seen), without one the first AIRPORT_NEAR_MAX in order.
class AirportJsonSink {
 public:
  static constexpr size_t OBJ_MAX = 384;
  AirportJsonSink(bool have_ref, int32_t lat, int32_t lon);
  void feed(const char* d, size_t n);
  static void sinkFn(void* self, const char* d, size_t n) { ((AirportJsonSink*)self)->feed(d, n); }  // FwdResp's sink
  int count() const { return n_; }
  const Airport& at(int i) const { return best_[i]; }
  bool hasTaf(int i) const { return taf_[i]; }
  uint16_t seen() const { return seen_; }          // objects read
  uint16_t dropped() const { return dropped_; }    // over OBJ_MAX
  uint16_t rejected() const { return rejected_; }  // not a METAR site, or garbage

 private:
  void object();
  bool have_ref_;
  int32_t lat_, lon_;
  char buf_[OBJ_MAX];
  size_t len_;
  int depth_;
  bool array_, in_str_, esc_, over_;
  uint16_t seen_, dropped_, rejected_;
  Airport best_[AIRPORT_NEAR_MAX];
  bool taf_[AIRPORT_NEAR_MAX];
  int n_;
};

}  // namespace fc
