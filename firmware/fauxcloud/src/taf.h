// taf.h -- the airports' forecasts from
// aviationweather.gov's raw TAF answer, streamed line by line, folded into one small grid per airport. Display-only,
// RAM-only reference data like the METAR: nothing here reaches MQTT, history, the learners or forwarding.
//
// The model (hourly cells so that fog fits in the same 44 B): 13 one-hour cells from the hour of the fetch, one
// 16-bit mask per cell -- each phenomenon "expected" (the base state, FM, a BECMG after its period) or "possible" (TEMPO,
// PROBnn, INTER, the BECMG transition). Phenomena (significant groups only): thunder (TS), snow (SN SG PL), rain (RA DZ UP
// GR GS IC, with FZ / SH), fog (FG or BR with the group's visibility < 1 000 m), gusts (G >= 22 kt / 12 m/s / 40 km/h).
// Not kept: temperatures, cloud, the visibility as a number, wind direction, PROB30 vs PROB40, exact FM minutes.
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace fc {

constexpr int TAF_HOURS = 13;           // 12 h from now, whatever minute the fetch ran
constexpr uint32_t TAF_MAX_AGE_S = 144000;  // 40 h: older issue times are rejected (a TAF is valid <= 36 h)

enum : uint16_t {
  TB_E_TS = 1u << 0, TB_P_TS = 1u << 1,
  TB_E_SN = 1u << 2, TB_P_SN = 1u << 3,
  TB_E_RA = 1u << 4, TB_P_RA = 1u << 5,
  TB_E_FG = 1u << 6, TB_P_FG = 1u << 7,
  TB_E_GU = 1u << 8, TB_P_GU = 1u << 9,
};
// the phenomena in display priority (thunder > snow > rain > fog > gusts); bit pair = 1u << (2 * what)
enum class TafWhat : uint8_t { Thunder, Snow, Rain, Fog, Gusts, COUNT };
const char* tafWhatName(TafWhat w);  // thunder snow rain fog gusts

// Absent: the airport was asked for in an ok / nodata TAF cycle and the answer had no TAF for it
enum class TafStatus : uint8_t { None, Ok, Nil, Cnl, Absent };
const char* tafStatusName(TafStatus s);  // none ok nil cnl absent
// a forecast is shown for 3 h from its fetch hour (the TAF cycle is hourly: one missed cycle is
// tolerated), so the line always covers >= 10 of its 12 hours; `tafHorizon` is where its cells (and validity) end.
constexpr uint32_t TAF_SHOW_S = 3 * 3600;

struct TafObs {
  char icao[5];
  uint8_t status;      // TafStatus
  uint32_t issue_unix;
  uint32_t valid_to;   // the forecast's own end (unix)
  uint32_t grid_unix;  // the start of cell 0 = the fetch time floored to the hour
  uint16_t cell[TAF_HOURS];
};

// One window of the next 12 h: a run of cells with the phenomenon, `possible` where it is not also expected.
struct TafWindow {
  TafWhat what;
  bool possible;
  uint32_t from, to;  // unix, clipped to [now, now + 12 h)
  bool cut;            // `to` is where the data stops (now + 12 h, or the grid's end when shown late), not a forecast end
  bool started;        // the run's first hour began at or before now: `from` is now, not a forecast start
};
// A forecast to show: Ok, inside its validity and its grid. Windows in priority order, then by time; at most `max`.
bool tafCurrent(const TafObs& t, uint32_t now_unix);
uint32_t tafHorizon(const TafObs& t);  // min(grid + 13 h, valid_to)
int tafWindows(const TafObs& t, uint32_t now_unix, TafWindow* out, int max);

// The streaming parser: one body line at a time (no '\n', any length the line sink keeps). Blocks start with "TAF" at the
// line start and continue on the following lines that start with a space (AWC's layout: one group per line, PROBnn
// alone before its TEMPO, trailing spaces). A METAR / SPECI line is not ours (counted in `other`). Unknown groups, TX/TN,
// QNH...INS, WS..., 5xxxxx are skipped; RMK ends the block's reading.
class TafParser {
 public:
  TafParser(const char* const* want, int nwant, uint32_t now_unix);
  void line(const char* l, size_t n, bool truncated = false);
  void finish();  // the end of a clean body: the open block is committed
  int count() const { return n_; }
  const TafObs& at(int i) const { return got_[i]; }
  const TafObs* all() const { return got_; }
  uint16_t garbage() const { return garbage_; }  // lines / blocks that are no TAF of ours (header, time)
  uint16_t other() const { return other_; }      // METAR / SPECI lines
  uint16_t unwanted() const { return unwanted_; }
  static void lineFn(void* ctx, const char* l, size_t n, bool cut) { ((TafParser*)ctx)->line(l, n, cut); }

  struct State {
    uint8_t wx;      // 1 ts, 2 sn, 4 ra, 8 fog weather (FG / BR)
    bool gust;       // the wind's gust is significant
    uint16_t vis_m;  // 10 000 = 10 km or more / not stated
  };
  struct Group {
    uint8_t kind;    // 0 base, 1 FM, 2 BECMG, 3 TEMPO / INTER, 4 PROB
    bool prob;       // a PROBnn before this TEMPO
    uint32_t a, b;   // the period (FM: a = the time, b = 0)
    bool has_wind, gust, has_vis, has_wx, nsw, cavok;
    uint16_t vis_m;
    uint8_t wx;
    int whole;       // a 1-2 digit token before an SM fraction
  };

 private:
  void startBlock(const char* l, size_t n);
  void token(const char* p, size_t n);
  void closeGroup();
  void endBlock();
  void paint(uint32_t a, uint32_t b, uint16_t bits);
  uint32_t ddhh(int dd, int hh, int mm) const;

  const char* const* want_;
  int nwant_;
  uint32_t now_;
  bool open_, stop_, pending_prob_;
  int stage_;  // 0 expect the validity, 1 groups
  TafObs cur_;
  State st_;
  uint32_t from_;  // the expected state holds from here
  Group g_;
  bool have_g_;
  uint32_t vfrom_;
  TafObs got_[3];
  int n_;
  uint16_t garbage_, other_, unwanted_;
};

}  // namespace fc
