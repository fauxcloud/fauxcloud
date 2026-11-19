// config.h -- per-station and bridge configuration. Plain C++, host-compilable.
//
// Merge order: compiled defaults <- model profile <- unit overrides (JSON). The JSON stored in NVS and exchanged
// over the API holds only the overrides: toJson() writes the keys that differ from defaults+profile, fromJson()
// applies a (partial) document on top of an effective config and never fails as a whole -- a bad key keeps the
// previous value and is reported in Errors.
#pragma once
#include <cstddef>
#include <cstdint>

#include "fields.h"
#include "profile.h"
#include "wu_parse.h"

namespace fc {

class Out;

// ---------------------------------------------------------------- profiles (data, not code)
enum class Profile : uint8_t { Generic, Yt60234 };

// a row per console family, backed by a captured upload log; a new console = a new row, no code.
struct StationProfile {
  const char* name;         // id, stored in the station's NVS config (never renamed)
  const char* label;        // shown in admin
  float wind_max_kmh;       // > 0: wind or gust above -> drop wind, gust, direction (rule profile_wind)
  float humidity_drop_pct;  // > 0: humidity below -> drop the whole sample (rule profile_humidity)
  bool rain_monotonic;      // daily rain: drop <= 0.5 mm = midnight reset, any other drop = glitch
  bool rainin_is_rate;      // `rainin` is a rain rate in/h (-> rain_rate_mmh), not WU's past-hour sum (-> rain_mm)
  uint32_t sensor_mask;     // fields from the outdoor RF sensor (the stale-sensor rule)
  float baro_factory_hpa;   // the console's uncalibrated pressure; NaN = none
  bool fingerprinted;       // the 7-in-1 fingerprint (stationFingerprint) is this row's
  // Direction is wrapped to 0..359 for every profile; dateutc is informational for every profile.
};
using ProfileRules = StationProfile;
const StationProfile& profileRules(Profile p);
bool profileFromName(const char* s, Profile* out);

// The 7-in-1 fingerprint, on a new station's first upload: User-Agent "lwIP/...", the query's parameter
// names an order-preserving subset of the console's 14 (any case) and nothing else, and -- when the MAC is known --
// a Bouffalo Lab OUI. UA or parameters off -> Mismatch (Generic WU suggested); both fine but no MAC or another OUI
// -> Unknown (an unknown MAC never vetoes); all three -> Match.
enum class Fingerprint : uint8_t { NotChecked, Match, Unknown, Mismatch };
const char* fingerprintName(Fingerprint f);  // "" | match | unknown | mismatch
Fingerprint stationFingerprint(const char* user_agent, const char* query, const uint8_t* mac /* null = unknown */);

// ---------------------------------------------------------------- station config
enum class PromoConv : uint8_t { None, FtoC, InHgToHpa, MphToKmh, InToMm, Scale };
constexpr int MAX_PROMOTE = 4;
constexpr int HIST_KEYS = 4, HIST_KEY_LEN = 24;  // = HIST_CH, CH_KEY_LEN (records.h)
constexpr size_t NAME_MAX_BYTES = 47;  // name[48], UTF-8, cut on a character boundary
constexpr size_t PROMO_NAME_MAX_BYTES = 31;

// Promotion types: the type fills every technical field, the user only names it.
enum class PromoType : uint8_t {
  Temperature, SoilTemperature, Humidity, SoilMoisture, LeafWetness, Pressure, WindSpeed, Rain, Pm25, Pm10, Co2,
  Uv, Illuminance, Solar, Battery, Voltage, Other, COUNT
};
struct PromoTypeDef {
  const char* id;            // JSON value of "type"
  const char* label;         // default display name
  const char* unit;          // "" = none (Other: free)
  PromoConv conv;
  const char* device_class;  // nullptr = none
  const char* state_class;
  const char* group;         // dashboard tile group: air | garden | other
  const char* icon;          // Lucide icon name
  float span;                // minimum chart span
  float max;                 // hard axis ceiling, NaN = none
  uint8_t dec;               // decimals
};
const PromoTypeDef& promoType(PromoType t);
bool promoTypeFromName(const char* s, PromoType* out);
// "Garden-PM2.5" -> "garden_pm2_5": ASCII-folded (accented Latin letters, the Polish ones included), [a-z0-9_]{1,23}; "" if nothing left.
void promoSlug(const char* name, char out[24]);

struct Promote {
  bool used;
  char param[16];         // upload parameter as received (an extra); may not have been sent yet
  char name[32];          // display name (UTF-8), the HA entity name
  PromoType type;
  char key[24];           // [a-z0-9_]: MQTT topic leaf + HA object id; from the name unless set under Advanced
  char unit[12];          // from the type unless overridden (Advanced)
  PromoConv conv;
  float scale;            // for PromoConv::Scale
  char device_class[24];  // "" = none
  char state_class[20];   // "" = none
};

struct StationConfig {
  char name[48];
  Profile profile;
  bool publish;
  uint32_t stale_s;
  float lim_min[CORE_FIELDS];       // NaN = no limit (metric, before calibration)
  float lim_max[CORE_FIELDS];
  float jump_per_min[CORE_FIELDS];  // 0 = off; max change per minute vs the last accepted value
  float temp_c_offset, humidity_offset, pressure_hpa_offset, wind_dir_offset;
  float rain_factor, wind_factor;
  uint32_t disabled;                // "Hidden" field mask: still recorded, not in /api/live, not sent to HA
  Promote promote[MAX_PROMOTE];
  // the history's extra channels chosen by hand ("hist": up to 4 keys, a field key such as
  // "indoor_temp_c" or a promotion key); empty = automatic (the first candidates that arrive)
  char hist[HIST_KEYS][HIST_KEY_LEN];
};

// Compiled defaults: profile yt60234 (the target console), publish true, no limits/jumps/calibration.
// A new station without stored config gets publish = BridgeConfig::auto_publish_new_stations instead (the
// sketch applies that at registration), so flipping the bridge flag never changes existing stations.
StationConfig defaultStationConfig();
// Defaults with a given profile: the base toJson() diffs against.
StationConfig profileDefaults(Profile p);

// Per-key errors of one apply. The texts are packed into one buffer so a long one arrives whole: an error that does
// not fit is counted in `dropped`, never cut.
struct Errors {
  static constexpr int MAX = 8;
  static constexpr int TEXT = 512;
  char text[TEXT] = {};
  uint16_t off[MAX] = {};
  uint16_t used = 0;
  int n = 0;
  int dropped = 0;  // errors beyond MAX or beyond the buffer
  const char* at(int i) const { return text + off[i]; }
  // promotions dropped because their key is now a built-in field or a WU protocol flag; their HA object ids
  // (the caller retracts the discovery)
  char stale_keys[4][24];
  int n_stale = 0;
  void add(const char* key, const char* why);
};

// Applies a JSON overrides document on top of cfg ("profile" first, so its defaults apply before the rest).
// Returns false only when the document is not a JSON object at all (cfg unchanged then).
bool stationFromJson(const char* json, size_t len, StationConfig* cfg, Errors* err);
// Writes the overrides: keys whose value differs from profileDefaults(cfg.profile). "{}" when none.
void stationToJson(Out& o, const StationConfig& cfg);
// Writes the full effective config (every key).
void stationEffectiveJson(Out& o, const StationConfig& cfg);
bool stationConfigured(const StationConfig& cfg);  // has any override

// Merge: defaults <- profile <- overrides, as one pure function.
StationConfig mergeStation(const char* overrides_json, Errors* err);

// ---------------------------------------------------------------- bridge config
struct BridgeConfig {
  char tz[64];                // POSIX rule: drives the day boundaries (validated)
  char tz_name[48];           // IANA name chosen by city in setup ("Europe/London"), stored for the UI only
  float lat, lon;             // optional location of that city, NaN = unset (dashboard sun/sky)
  bool auto_publish_new_stations;
  uint32_t mqtt_interval_last_seen_s;
  uint8_t max_stations;       // 1..MAX_STATIONS (the build profile's), default = MAX_STATIONS; read-only at 1
  bool lan_uploads;           // accept console uploads from the home network (LAN mode); default off
  bool forwarding;            // the online forwarding switch; default off. On => max_stations 1 and
                              // lan_uploads off (turning it on sets both; either back is refused while it is on)
  bool log_verbose;           // chatter lines (uploads, page requests, sends, BEAT) in the boot log; default off
};
constexpr uint8_t DEFAULT_MAX_STATIONS = FC_MAX_STATIONS;  // the build profile's station count
BridgeConfig defaultBridgeConfig();
// `in_use` = stations currently registered: max_stations below it is refused (forget one first); forwarding on is
// refused while in_use > 1 (the admin's enable action makes the user choose the station to keep).
bool bridgeFromJson(const char* json, size_t len, BridgeConfig* cfg, Errors* err, int in_use = 0);
void bridgeToJson(Out& o, const BridgeConfig& cfg);          // overrides only
void bridgeEffectiveJson(Out& o, const BridgeConfig& cfg);

// Applies a promotion's conversion to a raw value.
double promoteConvert(const Promote& p, double raw);
// a promoted value is taken only when it is finite as a float and |y| <= 1e9 (1e300 would print "inf")
inline bool promotedOk(double y) { return y == y && y <= 1e9 && y >= -1e9; }

}  // namespace fc
