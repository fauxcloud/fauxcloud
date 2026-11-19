// metar_store.h -- the chosen airports (settings, NVS blob "metar" v1), the readings
// (RAM only: nothing about a reading is ever written to flash), the one automatic reference reading and the
// JSON of /api/metar. Pure, host-tested. A reading is current while its age (now - the report's own time) <= 90 min.
#pragma once
#include <cstddef>
#include <cstdint>

#include "airport.h"
#include "forecast.h"  // OutNear
#include "metar.h"
#include "metar_sched.h"
#include "taf.h"

namespace fc {

class Out;

constexpr int METAR_AIRPORTS_MAX = 3;
constexpr uint32_t METAR_STALE_S = 5400;     // 90 min: "shown" (values, the amber dot after METAR_CURRENT_S)
// "current" -- the green dot, the T3 admission, the REL target number. Hourly airports (US :51) on the
// 15-min grid reach 60-74 min before the next report lands; 60 would blank them ~14 min an hour.
constexpr uint32_t METAR_CURRENT_S = 4500;    // 75 min
// the one "nearby" radius of the station's card (T3, REL, page 2), from the bridge's location
constexpr uint32_t METAR_NEAR_KM = 60;
constexpr int16_t METAR_SPREAD_DHPA = 15;    // > 1.5 hPa between current airports -> the first current, `inconsistent`
constexpr uint8_t METAR_CFG_V1 = 1;

// NVS blob "metar" (versioned; a later version migrates this one, never resets it)
struct MetarCfg {
  uint8_t ver;
  uint8_t on;  // the module's own switch, off by default
  uint8_t n;   // airports chosen, 0..3
  uint8_t reserved;  // bit i = a[i]'s name was typed by the user (in a former reserve: no blob version change)
  Airport a[METAR_AIRPORTS_MAX];
};
inline bool metarNameUser(const MetarCfg& c, int i) { return (c.reserved >> i) & 1; }
void metarCfgDefault(MetarCfg* c);
bool metarCfgValid(const MetarCfg& c);  // a v1 blob with sane airports
int metarCfgFind(const MetarCfg& c, const char* icao);
// the same airports, in any order
bool metarSameSet(const MetarCfg& a, const MetarCfg& b);
// the display names of settings stored before the short name existed, in place (RAM; idempotent);
// a name the user typed is left as typed
void metarCfgShortNames(MetarCfg* c);

// The readings, index-aligned with MetarCfg.a (RAM only)
struct MetarStore {
  MetarObs obs[METAR_AIRPORTS_MAX];
  TafObs taf[METAR_AIRPORTS_MAX];  // the forecasts, index-aligned like obs (status 0 = none yet)
  uint32_t last_taf_t;             // the last ok / nodata TAF cycle's end (unix, 0 = none)
  MetarOutcome last_taf;           // the last TAF cycle's outcome, failures included
  bool last_taf_any;               // a TAF cycle has ended since the boot (its outcome is in last_taf)
  uint32_t gen;      // bumped on every change (an ETag for /api/metar)
  uint32_t last_t;   // the last fetch's end (unix, 0 = none)
  MetarOutcome last;
  char dropped[65];  // the last line dropped as garbage, first 64 B (admin diag, R&D; never a log)
};
void metarStoreClear(MetarStore* s);
// A new line for one of the chosen airports: a newer report replaces, a COR of the same time replaces, an older one never.
// Returns the index, -1 = not ours / not newer.
int metarStorePut(MetarStore* s, const MetarCfg& c, const MetarObs& o);
// one forecast from a TAF fetch: replaces the airport's previous one unless it is an older issue. -1 = not ours.
int metarStorePutTaf(MetarStore* s, const MetarCfg& c, const TafObs& t);
// the end of an hourly TAF cycle. Ok: its forecasts are stored; ok / nodata: every chosen airport the job
// asked for (`asked`, the want-list taken at its start) and the answer had no TAF for is marked Absent ("no forecast"),
// and last_taf_t = now; an airport added while the job ran was not asked and keeps what it has. Any other outcome
// changes no forecast and keeps last_taf_t (the failure shows in last_taf). `grid` = the cycle's hour (the parser's grid).
void metarStoreTafCycle(MetarStore* s, const MetarCfg& c, MetarOutcome out, const TafObs* got, int n, uint32_t now,
                        uint32_t grid, const char* const* asked, int nasked);
// The airports changed: readings (and forecasts) of airports still chosen move with them, the rest go.
void metarStoreRemap(MetarStore* s, const MetarCfg& before, const MetarCfg& after);
bool metarCurrent(const MetarObs& o, uint32_t now_unix);

// one reference reading from up to three airports in the user's order: QNH = the current reports
// weighted 3 / 2 / 1 by position, the first current alone with `inconsistent` when they spread over 1.5 hPa; clouds,
// visibility, weather, T/Td from the first current airport only; wind raw only (never in the reference).
struct MetarRef {
  int8_t local;          // index of the first current airport in the user's order, -1 = none
  int16_t qnh_dhpa;      // METAR_I16_UNSET = no current QNH
  uint8_t qnh_mask;      // the airports that fed the QNH
  bool inconsistent;
};
MetarRef metarReference(const MetarCfg& c, const MetarStore& s, uint32_t now_unix);

// T3: the main airport's report for the outlook, admitted when all hold at `now` (the station's last
// upload): module on, the main airport (a[0], never a fall-back to #2), its report 0 .. METAR_CURRENT_S old (a future
// report is not), the bridge's location set and the airport <= METAR_NEAR_KM away by geoKm (never the pick-time km).
// `wx` maps the report's groups: snow (SN SG BLSN DRSN, RASN), rain (RA SHRA TSRA FZRA RADZ), drizzle (DZ, FZDZ),
// thunder (TS), fog (FG MIFG BCFG PRFG FZFG), mist (BR). PL / GR / GS / IC / UP alone map to nothing.
OutNear nearFromStore(const MetarCfg& c, const MetarStore& s, float lat, float lon, uint32_t now_unix);
// "METAR eligible" -- the module on, the location set, at least one chosen airport <= METAR_NEAR_KM away
// (the caller adds FWD_TLS). Decides whether baro_ref (eligible) or baro_check (L7, not eligible) may speak.
bool metarEligible(const MetarCfg& c, float lat, float lon);
// the reference side of a REL sample at `now`: module on, the reference QNH present and not
// `inconsistent`, the location set and every qnh_from airport <= METAR_NEAR_KM away, the newest of those reports
// 0 .. max_age_s old. Out: the QNH (0.1 hPa) and that newest report's time.
bool relReference(const MetarCfg& c, const MetarStore& s, float lat, float lon, uint32_t now_unix, uint32_t max_age_s,
                  int16_t* qnh_dhpa, uint32_t* ref_t);
// the station side of /api/metar's reference.station (the Airport weather sheet's live comparison)
struct MetarStationView {
  bool ok;           // a station with a pressure reading (not at the factory value)
  float p_hpa;       // as shown (calibrated)
  float offset_hpa;  // the bridge's pressure offset (target = QNH - offset)
  bool active;       // the baro_ref notice shows
  float lat, lon;    // the bridge's location (the 60-km rule)
};

// GET /api/metar: {"on","airports":[{..., "taf":null|{...}}],"reference":{...}|null,"last":{"t","result"},
// "last_taf":{"t","result"}}; each airport's "taf" carries its windows for the next 12 h;
// each airport also has "cur" (its report <= METAR_CURRENT_S old: the green dot); the reference also has
// "near_qnh_hpa" (the QNH of the current airports within 60 km, the REL basis) and
// "station":{"p_hpa","qnh_hpa","diff_hpa","target_hpa"|null,"active"}|null (null without a station view or a near QNH)
void metarJson(Out& o, const MetarCfg& c, const MetarStore& s, bool on, uint32_t now_unix,
               const MetarStationView* sv = nullptr);
// the admin GET's counters as `,"n":{...}` -- two printf calls, each under Out's 191-char line with
// all ten at 10 characters (the widest-value test)
void metarCountersJson(Out& o, const MetarCounters& n);

// The airport lookup's answer (POST /api/admin/metar/lookup), shown by the sheet; under the module's lock
struct MetarLookup {
  uint8_t state;  // 0 idle, 1 waiting, 2 running, 3 done
  bool near;      // "near me" (else a typed code)
  char ids[5];
  char result[14];  // ok none no_location busy off fail ...
  uint8_t n;
  Airport a[AIRPORT_NEAR_MAX];
  uint8_t taf_mask;  // bit i = a[i] issues a TAF (its siteType)
};
// POST /api/admin/metar body {"on":bool,"airports":["EGLL",...],"names":{"EGLL":"text"}} -> the new settings: every ICAO
// either chosen already or in the last lookup's list (its name, position and distance come from there); at most three, no
// duplicates. "names" sets a chosen airport's display name: trimmed, no control characters, cut
// to 24 bytes on a UTF-8 boundary, and marked as the user's (a lookup never replaces it); "" gives the name back to AWC:
// the last lookup's short name when it has the airport, else the last known name until a lookup has it.
// false + `err` (a short English reason) on a bad body.
bool metarCfgFromJson(const char* body, size_t len, const MetarCfg& cur, const MetarLookup& look, MetarCfg* out,
                      const char** err);
// a finished lookup's short names for the chosen airports whose name is not the user's; true =
// a name changed (the caller saves the settings off the loop tick)
bool metarLookupNames(MetarCfg* c, const MetarLookup& look);

}  // namespace fc
