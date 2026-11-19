// metar.h -- one METAR / SPECI line -> MetarObs, a pure function (no float, no heap, no String),
// and the line splitter the streaming body feeds (MetarLineSink, 192-B buffer). RAM-only reference data: it reaches
// MQTT only through the outlook's T3 codes (+ the HA `near` attribute, the airport's ICAO) and the baro_ref notice (its
// sentence and attributes carry the difference, the airports' QNH and the REL target) -- never as a sensor value or
// entity, and never history, the rings, the learners or forwarding.
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace fc {

// MetarObs.flags
enum : uint8_t {
  MO_VALID = 1u << 0,
  MO_COR = 1u << 1,  // COR, CCA..CCZ (a delayed / Canadian correction) or RTD: replaces a report of the same time
  MO_AUTO = 1u << 2,
  MO_CAVOK = 1u << 3,
  MO_LOW_CONF = 1u << 4,  // NCD / NSC from an automatic station, or CLR: "no cloud detected", not "no cloud"
  MO_CB = 1u << 5,        // a CB layer
  MO_SPECI = 1u << 6,
  MO_VIS_GE = 1u << 7,  // vis_m is a lower bound (9999, P6SM, CAVOK)
};
// MetarObs.wx
enum : uint8_t {
  MW_PRECIP = 1u << 0,  // any precipitation (DZ RA SN SG IC PL GR GS UP)
  MW_FOG = 1u << 1,     // FG (BR is MW_MIST)
  MW_THUNDER = 1u << 2,
  MW_FREEZING = 1u << 3,
  MW_SNOW = 1u << 4,
  MW_MIST = 1u << 5,     // BR
  MW_DRIZZLE = 1u << 6,  // DZ
  MW_RAIN = 1u << 7,     // RA (also in SHRA, TSRA, FZRA)
};
constexpr uint16_t METAR_WDIR_VRB = 0xFFFF;   // variable
constexpr uint16_t METAR_WDIR_NONE = 0xFFFE;  // calm, unknown (/////KT) or no wind group
constexpr uint16_t METAR_U16_UNSET = 0xFFFF;
constexpr int16_t METAR_I16_UNSET = INT16_MIN;
constexpr uint8_t METAR_COVER_UNSET = 0xFF;  // cover: 0 none, 1 FEW, 2 SCT, 3 BKN, 4 OVC (VV counts as OVC)

struct MetarObs {
  char icao[5];
  uint8_t flags;
  uint32_t obs_unix, got_unix;
  int16_t qnh_dhpa, t_dc, td_dc;   // hPa x10, deg C x10 (whole degrees as reported)
  uint16_t wdir;                    // degrees, METAR_WDIR_VRB / METAR_WDIR_NONE
  uint16_t wind_dkmh, gust_dkmh;    // km/h x10
  uint16_t vis_m, ceil_ft;          // metres (capped 10 000), feet of the lowest BKN / OVC / VV
  uint8_t cover, wx;
  int8_t wx_int;  // -1 light, 0 moderate, +1 heavy (the strongest group)
};

enum class MetarWhy : uint8_t {
  Ok,
  Empty,     // a blank line (CRLF, keep-alive): not counted
  Garbage,   // no ICAO + DDHHMMZ, or not a report at all
  Unwanted,  // an ICAO that was not asked for
  Nil,       // NIL report
  Time,      // the time does not reconcile: > 10 min in the future or older than 24 h
};
struct MetarResult {
  MetarWhy why;
  uint8_t field_bad;  // groups read but out of bounds (the field left unset)
};

// `want`: the ICAOs asked for (4 upper-case characters each); `now_unix`: real time (the month the report belongs to).
MetarResult metarParse(const char* line, size_t n, const char* const* want, int nwant, uint32_t now_unix, MetarObs& out);
const char* metarWhyName(MetarWhy w);
// One answer may carry several reports of an airport (hours=2: newest first, SPECIs, corrections): the newer one is
// kept, or a correction of the same time; a new airport is added while there is room (false: no room, dropped).
bool metarKeep(MetarObs* got, uint8_t& ngot, int cap, const MetarObs& o);
// the last line without '\n' is a report only when the body ended cleanly: the answer was read to
// its end (no abort, no TLS error), not cut at the cap and not a short read; otherwise it is dropped.
inline bool metarCleanEnd(bool answered, bool done, bool cut, bool failed) { return answered && done && !cut && !failed; }

// The streaming body's line splitter: bytes in any split, a line out on '\n' (or at finish()); '\r' dropped; a line
// over 191 B keeps its first 191 (every group we read precedes RMK) and counts `trunc`. The callback runs per line.
class MetarLineSink {
 public:
  typedef void (*LineFn)(void* ctx, const char* line, size_t n, bool truncated);
  static constexpr size_t LINE_CAP = 192;
  MetarLineSink(LineFn fn, void* ctx) : fn_(fn), ctx_(ctx), len_(0), over_(false), trunc_(0) {}
  void feed(const char* d, size_t n);
  void finish();  // the end of the body: a last line without '\n'
  void drop();    // the body was cut at the cap: a pending part line is discarded (counted in trunc), never parsed
  uint16_t trunc() const { return trunc_; }
  static void sinkFn(void* self, const char* d, size_t n) { ((MetarLineSink*)self)->feed(d, n); }  // FwdResp's sink

 private:
  void emit();
  LineFn fn_;
  void* ctx_;
  char buf_[LINE_CAP];
  size_t len_;
  bool over_;
  uint16_t trunc_;
};

}  // namespace fc
