// station.h -- per-station state: config, filter state, live sample, counters, recent ring,
// hourly and daily history. Plain C++, host-compilable. Not thread-safe: the caller serializes access.
#pragma once
#include <cstdint>

#include "calib.h"
#include "config.h"
#include "console.h"
#include "filter.h"
#include "forecast.h"  // LearnUse
#include "history.h"
#include "profile.h"
#include "rel_check.h"
#include "ring.h"
#include "tz.h"
#include "wu_parse.h"

namespace fc {

// the station count is the build profile's (profile.h): 1 on the WROOM-32 release. It is not the ring count:
// RING_SLOTS is the profile's too (ring.h: WROOM-32 1, PSRAM 2), and every station needs its ring.
constexpr int MAX_STATIONS = FC_MAX_STATIONS;
static_assert(MAX_STATIONS >= 1, "at least one station");
static_assert(MAX_STATIONS <= RING_SLOTS, "every station needs its flash ring slot (ring.h)");
static_assert(MAX_STATIONS <= PROFILE_HEAP_SLOTS_MAX, "2 stations need the PSRAM profile: this board's heap carries 1 (profile.h)");

// Interface an upload arrived on: the ESP's own AP (the console) or the home LAN (LAN mode).
// Which side a request came from. lwIP accepts a packet for any local address on any interface (weak host
// model), so the local address alone does not tell: an AP client can open http://<STA IP>/ and a LAN host with a
// route via the bridge can reach 192.168.77.1. Both addresses must agree: local = the AP address and peer in its /24
// -> Ap; neither -> Sta; one of them -> Mixed, which is neither side (pages and admin answer like an unknown request,
// uploads are answered and dropped as "lan").
enum class SrcIf : uint8_t { Unknown, Ap, Sta, Mixed };
const char* srcIfName(SrcIf i);
// Addresses in network byte order, as in sockaddr_in; local 0 = unknown.
SrcIf classifySrc(uint32_t local, uint32_t peer, uint32_t ap_ip);

struct Station {
  bool used;                 // false = an allocated slot that was forgotten and waits for reuse
  uint32_t gen;              // allocation generation (unique per boot): part of every ETag of this station
  char id[STATION_ID_LEN];
  StationConfig cfg;
  bool stored;               // the config has an NVS entry (survives reboots)
  // every field's last accepted value, merged field by field (an upload without a field keeps its value);
  // `fresh` = the fields of the latest accepted upload (MQTT publishes only these), field_t = when each arrived.
  Sample live;
  uint32_t live_rejected;    // fields the filters rejected in the last upload
  uint32_t fresh;
  uint32_t field_t[F_COUNT];
  uint8_t promoted_mask;     // cfg.promote[i] has a value (carried like the fields)
  uint8_t promo_fresh;       // ... from the latest upload
  float promoted[MAX_PROMOTE];
  uint32_t promo_t[MAX_PROMOTE];
  KnownKey known[MAX_KNOWN]; // unknown keys ever received, oldest-seen evicted first, promoted ones kept
  uint32_t last_seen;        // ESP clock, unix seconds (or uptime seconds before SNTP sync)
  uint32_t last_seen_ms;     // millis() of the last upload: liveness is measured on this, immune to SNTP
  uint32_t src_ip;           // network byte order, hint only
  SrcIf src_if;
  // the console's MAC from the last accepted upload -- AP side from the DHCP lease, LAN side from
  // the ARP table when the console is in the home subnet (else unknown). RAM only; admin only, never public or MQTT.
  bool has_src_mac;
  uint8_t src_mac[6];
  uint32_t last_rx_ms;       // millis() when the last accepted upload was received
  uint32_t last_qhash;       // FNV-1a of that upload's query string (the dedupe window runs on last_rx_ms)
  Fingerprint fp;            // the 7-in-1 fingerprint of the last accepted upload (RAM; admin shows it)
  // a rate profile's derived rain_mm = the daily-counter deltas of the last 60 min (recent ring).
  // Warm once the first valid delta since boot is 60 min old and the ring still spans the hour; RAM only.
  bool rain_started;
  uint32_t rain_first_ms;    // millis() of the first valid delta since boot
  bool rain_warm;
  bool rain_hours;           // while not warm, rain_mm is estimated from the hour records
  uint16_t rain_n;           // readings in the last hour (the HA attribute "n")
  // the live fields computed here, not sent ("derived", with the readings behind each), and the ones
  // still warming up (no value yet); the dashboard notes them, /api/live carries both
  uint32_t derived;
  uint32_t warming;
  uint8_t derived_n[F_COUNT];
  // wind_avg10m_kmh, the mean wind over the last 10 min of the recent ring -- the same number
  // forwarding sends (recentFieldMean). A pseudo-field (no FIELDS row: no snapshot change); RAM only.
  float wind10;
  uint32_t wind10_t;  // when the window last held wind (as field_t): the value's age
  uint8_t wind10_n;
  bool wind10_have, wind10_fresh;  // a value exists / computed on the latest upload
  // the outdoor sensor set (profile sensor_mask) as received, before calibration; same_n uploads in a
  // row unchanged (or absent after it was seen) -> sensor_stale at STALE_SENSOR_N; stale_since = its last change
  bool sensor_init;
  uint8_t same_n;
  bool sensor_stale;
  bool sensor_absent;        // ... and the set is absent now (the console lost it after a cold start)
  uint32_t stale_since;
  uint32_t profile_t;        // when cfg.profile last changed (station clock), 0 = never since boot (RAM)
  // for tuning STALE_SENSOR_N from outdoor nights -- the longest unchanged run since boot (uploads)
  // and how many times the flag went on; RAM, /api/live diag
  uint16_t stale_run, stale_longest_run, stale_on_count;
  uint32_t last_sensor_present;
  float last_sensor[CORE_FIELDS];
  // uploads in a row with the raw pressure at the profile's factory value; baro_uncal at BARO_UNCAL_N
  uint8_t baro_n;
  bool baro_uncal;
  // the last upload's calibrated pressure not at the factory value (baro_uncalibrated's "last good value"; RAM).
  // good_t 0 = none since boot: while baro_uncal is up, ingest fills it once from the newest clean closed hour
  // (lastGoodFromHours, rel_check.h)
  float good_p;
  uint32_t good_t;
  RelState rel;              // the REL hint against the nearby airports (rel_check.h; RAM, empty after a reboot)
  // the console's clock vs ours, from dateutc, only while our clock is valid (SNTP, DHCP, browser);
  // clock_off = beyond CLOCK_SKEW_MAX_S and not the 7-in-1's DST hour (its dateutc carries +1 h in summer)
  bool clock_off;
  int32_t clock_skew_s;      // dateutc - our clock at the last upload that carried dateutc
  uint32_t uploads;          // every upload, accepted or dropped
  uint32_t dropped;          // whole samples dropped (profile_humidity)
  uint32_t rejects[F_COUNT]; // field-level rejections since boot, every rule
  uint32_t rej_rule[R_COUNT];// uploads in which a rule fired, since boot
  uint32_t seen_fields;      // union of accepted fields since boot (drives lazy HA discovery)
  uint8_t seen_promoted;     // union of promoted_mask since boot
  FilterState fstate;
  uint32_t cfg_rev;           // bumped on every config change (part of the /api/live ETag)
  uint32_t hour_closes;       // hourly records closed since boot (part of the history ETags)
  uint8_t flash_state;        // FLASH_* bits: what was restored from flash after this boot
  // the console's WU key, bound on the first accepted upload (trust on first use: a keyed hash, never the key itself;
  // never exposed). An upload with another key is dropped and counted until the admin accepts the new key.
  bool key_bound;
  uint8_t key_hash[32];
  uint32_t key_mismatch;      // uploads dropped for another key since restart
  uint32_t flash_rev;         // bumped when history comes back from flash (part of every ETag of this station)
  // the history's extra channels -- keys (a field key or a promotion key), scales (10^decimals), the
  // map version the records carry (NVS ch:<id> keeps the versions); ch_dirty = a new version waits to be stored
  ChMap chmap;
  CalLog cal;                // the calibration log (NVS cl:<id>), applied to history rows on read
  LearnUse use;              // what the outlook may read of the learners (app_learn copies it under gLock)
  bool ch_dirty;
  Recent recent;
  Hourly hourly;
  Daily daily;

  bool online(uint32_t now_ms) const { return uploads && now_ms - last_seen_ms < cfg.stale_s * 1000UL; }
  const char* displayName(char* buf, size_t cap) const;  // cfg.name, or "fauxcloud <ID>"
};

// an upload's key against the station's. Bound = the station had none and now has this one.
enum class KeyCheck : uint8_t { Bound, Match, Mismatch };
KeyCheck checkStationKey(Station& st, const uint8_t key[32]);
// why an upload is answered but not used: "lan" (home network, switch off), "blocked" (deny-list), else null.
const char* uploadDrop(SrcIf via, bool lan_uploads, bool blocked);
// the boot loads the stored stations in three passes, so with fewer slots than stored
// stations the live console keeps its slot and the others are parked (stored data untouched, listed with Forget):
// pass 0 the ring owner with the newest hour key (the console that uploaded last; a tie keeps ring 0), pass 1 the other
// ring owner, pass 2 every id without a ring. "" or null = no owner.
struct LoadOrder {
  char first[STATION_ID_LEN];
  char second[STATION_ID_LEN];
};
LoadOrder loadOrder(const char* owner0, uint32_t newest0, const char* owner1, uint32_t newest1);
int loadPass(const char* id, const LoadOrder& o);  // 0, 1 or 2
// the admin gate's verdict. Only a home-network call that carries the CSRF header but no valid
// session learns it is signed out (401); the AP side and a header-less probe keep the catch-all (the admin stays
// invisible to casual probes).
enum class AdminGate : uint8_t { Pass, CatchAll, Unauth };
AdminGate adminGate(SrcIf via, bool csrf, bool needSession, bool sessionOk);
// the AP is a dead end: the AP face answers the catch-all for every public path except the upload path,
// /tools and / -- in run mode only. In setup mode the AP is the only face and serves the setup page and its assets.
bool apCatchAllApplies(SrcIf via, bool setup_mode);
// DNS rebinding guard: may a run-mode request from the home network with this Host header reach a page or the API?
// Yes for no Host at all, an IP literal (IPv4, or IPv6 in brackets), the bridge's hostname, or the hostname with
// .local or a router's private-use suffix (.lan, .home, .internal, .home.arpa), each in any case with an
// optional trailing dot and :port. A page that rebinds its own name to the bridge still sends
// that name, which is none of these. Setup mode, the AP side and the upload path do not ask (app_http.cpp).
bool hostAllowed(const char* host, const char* hostname);
// station configs in NVS that got no slot at boot (more stored than the profile's station count), RAM only, so
// the admin can Forget them. Ids beyond MAX are only counted.
struct ParkedList {
  static constexpr int MAX = 4;
  char ids[MAX][STATION_ID_LEN];
  uint8_t n;
  uint16_t dropped;
  void add(const char* id);
  bool has(const char* id) const;
  bool remove(const char* id);
};
class Stations;
// the parked ids that still have no slot (one that got a slot since -- a freed slot taken by its
// upload, or a station PUT -- is a live station and must never be offered as "settings only")
ParkedList parkedWithoutSlot(const ParkedList& p, Stations& s);
// the admin's "Saved settings without a slot" (each with Forget): the parked configs, then the leftover keys of a refused
// adopt's new id (`leftover`: RAM only, so the admit, restart and ghost rules, which read `parked` alone, stay as they were);
// none that took a slot since
ParkedList parkedListing(const ParkedList& parked, const ParkedList& leftover, Stations& s);
// an announced id is a ghost (its HA device is retracted) only when it is neither a live station nor
// a parked one -- a parked station keeps its HA device, its stored config and its rings.
bool isGhost(const char* id, Stations& s, const ParkedList& parked);
// a stored station never starts from defaults. A parked id gets no slot at runtime (its upload is
// turned away, an admin PUT is refused); only the boot loader admits it with its stored config and key. Returns the
// station (found or added with `initial`), or null.
Station* admitStation(Stations& s, const ParkedList& parked, const char* id, const StationConfig& initial, int limit);
// The admin PUT's admission, decided before anything is taken or written. A parked id waits for the boot; a new id needs
// a free slot; a calibration change while the clock is not synced is refused (its log entry needs the time). The
// probe compares the body applied to a copy of the config against the calibration in force: the slot's log, else
// `stored` (the id's log on flash, read without writing; null = none), else the starting config's. `scratch` takes the
// probe's errors. Only Ok takes a slot (*out); every refusal leaves the stations as they were.
enum class PutAdmit : uint8_t { Ok, Parked, NoSlot, CalRefused };
PutAdmit stationPutAdmit(Stations& s, const ParkedList& parked, const char* id, const StationConfig& initial, int limit,
                         bool synced, const CalLog* stored, const char* body, size_t len, Errors* scratch, Station** out);
// a Forget that frees a slot while something is parked plans a restart ("forget_parked"), so the boot
// loader admits the parked station with its stored config.
inline bool forgetNeedsRestart(const ParkedList& parked) { return parked.n > 0; }
// the last console turned away for lack of a slot, shown for an hour after its last try; cleared
// when a Forget frees a slot. RAM only (the console knocks again).
struct NoSlotNote {
  static constexpr uint32_t SHOW_MS = 3600000;
  char id[STATION_ID_LEN];
  uint32_t ms;  // millis() of the last try, | 1 (0 = none)
  // adopt.h: the turned-away id uploads from the same device as the loaded station `from`; key_hash = the HMAC of
  // its upload's key, bound at the admin's confirmation. RAM only, gone with the note (an hour, a restart).
  bool same;
  char from[STATION_ID_LEN];
  uint8_t key_hash[32];
  void note(const char* sid, uint32_t now_ms);  // a new id clears the offer; the same id keeps it until offer() says
  void offer(bool is_same, const char* loaded_id, const uint8_t key[32]);  // after note(): this upload's verdict
  // the offer stands for exactly this pair (call expire() first)
  bool offers(const char* loaded_id, const char* new_id) const;
  void expire(uint32_t now_ms);  // clears it once SHOW_MS have passed since the last try
  void clear() { id[0] = 0, ms = 0, same = false, from[0] = 0; }
};
// with auto-publish on, a station is stored (NVS) on its first accepted upload, so its key binding
// survives a restart and only real ghosts lose their HA device; with it off it stays pending (RAM only) until saved.
inline bool storeOnUpload(bool stored, bool auto_publish) { return !stored && auto_publish; }
// records the upload's fingerprint; on a new station's first accepted upload (no NVS entry, no upload
// yet) a mismatch switches it to Generic WU. A station stored before keeps its profile. True = the profile changed.
bool applyFingerprint(Station& st, Fingerprint fp);
// as tuned on a real console: a lost link freezes the last values, wind included, so a frozen
// set with wind or gust > 0 (or an absent set) is stale after STALE_SENSOR_N uploads; a frozen set in calm air (wind
// and gust 0: indoors, a still night) is ordinary and needs STALE_CALM_N (indoors the longest run seen was 16)
constexpr uint8_t STALE_SENSOR_N = 10;   // ~11 min at the 7-in-1's 65 s
constexpr uint8_t STALE_CALM_N = 110;    // ~2 h (a measured -6.0 °C night had 55 identical)
// The count that makes the frozen set stale: absent or windy -> STALE_SENSOR_N, calm -> STALE_CALM_N
uint8_t staleThreshold(bool set_absent, float frozen_wind, float frozen_gust);
// a real 1013.2 hPa day reads exactly the factory 29.92 inHg, so the value must hold unchanged
// for ~1 h of uploads before it counts as "not calibrated"; any change clears it (the forecast's tendency pauses on it)
constexpr uint8_t BARO_UNCAL_N = 60;
constexpr float BARO_FACTORY_TOL_HPA = 0.05f;
constexpr int32_t CLOCK_SKEW_MAX_S = 600, CLOCK_DST_HOUR_TOL_S = 300;
// |skew| > 10 min, except +1 h +-5 min while DST is active in our zone (the console's summer hour)
bool consoleClockOff(int32_t skew_s, bool dst_active);
// what ingestQueued does with an upload that passed onUpload's checks (side, block list), in this order:
// no slot -> NoSlot; < RATE_MIN_MS after the last accepted one -> RateLimited; the same query again ->
// Duplicate; another key -> KeyMismatch; else Accept. The rate comes before the key, and the caller checks
// (and binds) the key only for an upload that is neither limited nor a duplicate, so a binding always comes with an
// accepted upload. Each refusal is answered `success`.
enum class UploadVerdict : uint8_t { Accept, NoSlot, KeyMismatch, RateLimited, Duplicate };
struct Station;
UploadVerdict uploadVerdict(const Station* st, KeyCheck kc, uint32_t rx_ms, uint32_t qhash, bool dated = true);

// a console with Weathercloud ticked repeats the identical WU GET 2-4x per cycle. The same station
// with the same query string (FNV-1a 32) < DUP_WINDOW_MS after its last accepted upload is a duplicate. Only an upload
// with a real dateutc (the time inside makes a repeat identical only within its second) gets the 120 s window; one with `dateutc=now` or none gets DUP_UNDATED_MS, well below a 60 s cycle, so a console with
// steady values is never halved.
constexpr uint32_t DUP_WINDOW_MS = 120000, DUP_UNDATED_MS = 30000;
uint32_t fnv1a32(const char* s);

// an upload received < RATE_MIN_MS after the previous accepted one of the same station (wrap-safe
// millis) is answered `success` and dropped: one console cannot flood MQTT/HA or push the 60-reading ring out in a
// minute. 4 s, so a console that really sends every 5 s never loses an upload to jitter. Checked after block and
// slot, before the key (uploadVerdict); the first upload since boot is never limited.
constexpr uint32_t RATE_MIN_MS = 4000;
inline bool rateLimited(const Station& st, uint32_t rx_ms) { return st.uploads && rx_ms - st.last_rx_ms < RATE_MIN_MS; }
inline bool isDuplicate(const Station& st, uint32_t rx_ms, uint32_t qhash, bool dated = true) {
  return st.uploads && st.last_qhash == qhash && rx_ms - st.last_rx_ms < (dated ? DUP_WINDOW_MS : DUP_UNDATED_MS);
}

// FLASH_RINGS_LOADED: the station's flash rings were read into RAM; FLASH_SNAP_DONE: the shared snapshot's
// entry was applied (or there was none); FLASH_LIVE_RESTORED: the last reading came back from flash (no upload yet).
constexpr uint8_t FLASH_RINGS_LOADED = 1, FLASH_SNAP_DONE = 2, FLASH_LIVE_RESTORED = 8;
// a daily-counter reset farther than this from local midnight is the console's (bit console_reset)
constexpr uint32_t RESET_MIDNIGHT_TOL_S = 3600;

struct IngestResult {
  bool dropped;      // the whole sample was dropped
  bool rain_glitch;
  bool hour_closed;
  bool day_closed;
};

struct TickResult {
  bool hour_closed;
  bool day_closed;
};

// Lazy slots: a Station (~14 KB) is allocated on the heap on its first upload or when a stored
// config is loaded, and never freed. Unused slots cost one pointer; a forgotten station keeps its allocation and
// the slot is reused by the next new station.
class Stations {
 public:
  Station* find(const char* id);
  // Finds the station or allocates it with `initial` as its config; nullptr when `limit` stations are already in
  // use (bridge max_stations), all slots are taken or the allocation fails.
  Station* findOrAdd(const char* id, const StationConfig& initial, int limit = MAX_STATIONS);
  Station* first();                 // first allocated slot or nullptr
  Station* at(int i) { return s_[i] && s_[i]->used ? s_[i] : nullptr; }  // nullptr = no station in the slot
  int indexOf(const Station* st) const;
  int allocated() const;  // slots reserved (forgotten ones included)
  // slots are reserved at boot for max_stations, before Wi-Fi, so they sit in one stretch and the heap
  // keeps its large block for an OTA. Reserves up to n slots in total (never frees); returns the slots reserved.
  int reserve(int n);
  int active() const;     // stations in use
  // Resets the station in the slot to the empty state and frees the slot for reuse; false if nothing is there.
  bool forget(int slot);

  // Runs the filter chain and applies one upload. `clock_ok` = SNTP synced; before that the sample goes to live
  // and recent only, so no bogus 1970 hour or day is ever closed.
  static IngestResult ingest(Station& st, const Sample& s, uint32_t now, uint32_t now_ms, uint32_t src_ip,
                             SrcIf src_if, bool clock_ok, const PosixTz& tz);
  // at the first SNTP sync, everything stamped with seconds since boot gets unix time (offset = unix time at
  // boot): the recent ring, last_seen and the jump filter's times. Returns the readings re-stamped.
  static int clockSynced(Station& st, uint32_t offset);
  // the promotions' values from the known keys (after a config change: a key promoted now shows its last value
  // at once, with its age); a promotion whose key is not known has no value.
  static void repromote(Station& st);
  // the profile was switched by hand. The rain derivation starts over: no warm-up carried, no derived
  // or warming flag, and the ring's rain_mm slots (per-upload deltas or past-hour sums, by the old meaning) cleared.
  static void profileChanged(Station& st, uint32_t now);
  // Forget on a known key: gone until the console sends it again. False if it is not known.
  static bool forgetKey(Station& st, const char* key);
  // Closes the hour / local day when the clock has moved on. Call every second (only with a synced clock).
  static TickResult tick(Station& st, uint32_t now, const PosixTz& tz);
  // Test hooks: close the open hour (feeding the day), and the open hour + day.
  static bool closeHourNow(Station& st, const PosixTz& tz);
  static bool closeDayNow(Station& st, const PosixTz& tz);
  // quality bits the app knows: the unix time of this boot (0 = not known yet) for `restart`, and whether the
  // clock is a browser seed for `clock_approx`. Global: one bridge, one clock.
  static void setBootTime(uint32_t unix_s);
  static void setClockApprox(bool approx);
  // new channel candidates that arrived (indoor temperature, indoor humidity, then promotions with a
  // value) take free channels, in that order; a change bumps ch_ver and sets ch_dirty. True when the map changed.
  static bool updateChannels(Station& st);

 private:
  Station* s_[MAX_STATIONS] = {};
  uint32_t gen_ = 0;
};

}  // namespace fc
