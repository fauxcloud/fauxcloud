// web_render.h -- /api/live, /api/recent, /api/hourly, /api/daily, config JSON and the one-page status HTML.
//
// Renders into a small buffered Out that flushes through a callback (httpd_resp_send_chunk on the ESP,
// a std::string in the host tests), so nothing is built in one big heap block. Plain C++, host-compilable.
#pragma once
#include <cstdarg>
#include <cstddef>
#include <cstdint>

#include "forecast.h"
#include "heapinfo.h"
#include "notices.h"
#include "ring.h"
#include "station.h"
#include "wufc.h"  // LiveSnap.wufc

namespace fc {

class Out {
 public:
  using Fn = bool (*)(void* ctx, const char* data, size_t len);
  Out(Fn fn, void* ctx, char* buf, size_t cap) : fn_(fn), ctx_(ctx), buf_(buf), cap_(cap) {}
  void write(const char* s, size_t n);
  void raw(const char* s);
#if defined(__GNUC__)
  void printf(const char* fmt, ...) __attribute__((format(printf, 2, 3)));
#else
  void printf(const char* fmt, ...);
#endif
  void jsonStr(const char* s);  // quoted + escaped JSON string
  void htmlStr(const char* s);  // HTML-escaped text
  bool flush();                 // returns false once any flush failed (client gone)
  bool ok() const { return ok_; }  // no flush has failed yet (a stream stops reading once false)

 private:
  Fn fn_;
  void* ctx_;
  char* buf_;
  size_t cap_;
  size_t len_ = 0;
  bool ok_ = true;
};

// Bridge-level diagnostics for /api/live. Filled by the sketch.
// diag.tasks: the tasks whose stack margin is watched -- the key in diag.tasks and the FreeRTOS name
constexpr int DIAG_TASKS = 6;
// the bench build only (FC_DIAG_BENCH, src/diag_bench.h); the release has no "tasks" key
extern const char* const DIAG_TASK_KEY[DIAG_TASKS];   // loop, httpd, mqtt, udp53_123, ota, netout
extern const char* const DIAG_TASK_RTOS[DIAG_TASKS];  // loopTask, httpd, mqtt_task, udp53_123, ota, netout
int diagTaskIndex(const char* rtos_name);              // -1 = not one of them

struct Diag {
  int ap_clients;
  int sta_rssi;  // dBm, 0 when not connected
  bool sta_up;
  bool mqtt_up;
  uint32_t heap_free, heap_min, heap_largest;
  uint32_t uploads, queue_drops, other_hits;
  uint32_t uploads_dropped_lan, uploads_dropped_key, uploads_dropped_blocked;  // upload trust rules: answered, not used
  uint32_t uploads_too_long, uploads_no_slot, uploads_rate_limited;          // answered, not used
  uint32_t uploads_duplicate;                                                // the same query within 120 s
  uint32_t stale_longest_run, stale_on_count;  // max / sum over the stations (stale-sensor tuning)
  uint32_t store_fail;  // history writes that failed
  // closes refused as older, torn segments cut, failed appends (retried), future records cut
  uint32_t store_drop_old, store_cut, store_fail_append, store_future_dropped;
  // the clock hold, trusted records kept past the clock, append back-off, streams stopped on a dead client, MQTT init
  // failures, the snapshot version read at boot
  bool clock_held;
  uint32_t clock_held_n, clock_ref, clock_cut_n, store_future_kept, store_retry, http_stream_abort, mqtt_init_fail;
  uint8_t snap_from;
  uint32_t hist_back[MAX_STATIONS];  // late readings per station slot (joined the open hour or kept out)
  uint32_t dns_answered, ntp_answered, ntp_unanswered;
  uint32_t mqtt_published, mqtt_failed;
  // forwarding (LAN diag only): sums over the targets; the send low-water, its largest block, the longest send.
  // fwd_fail = readings not delivered after an attempt, fwd_attempt_fail = failed attempts (retried or
  // not); fwd_heap_min / fwd_largest_min on the byte-addressable heap (MALLOC_CAP_8BIT). The bench keys
  // (fwd_heap_min/largest_min/send_ms_max, ota_*, http_busy_ms_max, task_free) stay in the struct (stack only) and are
  // filled and rendered only at FC_DIAG_BENCH=1
  uint32_t fwd_ok, fwd_fail, fwd_attempt_fail, fwd_skip, fwd_drop, fwd_heap_min, fwd_largest_min, fwd_send_ms_max;
  char sta_ip[16];
  const char* fw_version;
  const char* hostname;
  int stations_allocated;  // slots reserved (max_stations at boot)
  int stations_used;
  const char* mode;  // "run" (the only mode that serves /api/live)
  // account-wide admin lock (RAM only)
  bool admin_locked;
  uint32_t admin_lock_s;  // seconds until it ends
  int admin_failures;     // failed proofs in the current 10-minute window
  // heap_low = minimum free heap over the last HEAP_LOW_MINUTES below HEAP_LOW_FREE8, heap_min_10m = the 10-minute
  // minimum (every heap_* figure is the byte-addressable heap, MALLOC_CAP_8BIT); HTTP load counters
  bool heap_low;
  uint32_t heap_min_10m;
  // the last OTA write's heap (RTC record)
  HeapDip heap_dip;  // the since-boot 8-bit minimum's second: uptime, context mask, open httpd sockets
  bool ota_heap_have;
  uint32_t ota_start8, ota_min8, ota_largest_min8;
  uint32_t http_requests, http_304, http_busy_ms_max;
  uint32_t http_aborted;  // responses cut by a send error (socket closed), since boot
  uint32_t http_purged;   // idle page sessions closed by the session cap, since boot
  uint32_t http_shed;     // page requests answered 503 under low memory, since boot
  uint32_t http_paced;    // page answers that waited for earlier answers to be acknowledged (pagePace)
  // flash history -- -1 = LittleFS not mounted
  int32_t store_ms_max;
  // image, rollback, reset, watchdogs, crash data
  const char* build;
  const char* page;  // the admin shell's version: an open admin page an OTA left behind reloads once (core.js)
  const char* ota_state;
  const char* last_reset;
  const char* reset_raw;  // "esp/rtc0/rtc1" codes of this boot
  uint32_t restarts, sta_reinits, mqtt_reconnects, ota_slot;
  bool crash;
  const char* crash_reason;
  const char* crash_task;
  uint32_t crash_pc;
  uint32_t task_free[DIAG_TASKS];  // stack never used since the task started, bytes; 0 = no such task now
};

struct Clock {
  uint32_t now;      // unix seconds (uptime seconds when not synced)
  bool synced;       // SNTP has set the clock
  uint32_t uptime_s;
  uint32_t ms;       // millis(): liveness (online / stale) is measured on this
  const char* src = "none";  // time_src, none | persisted | browser | dhcp | sntp
};

// Everything /api/live shows of one station, copied under the state lock so the JSON renders without it.
struct LiveSnap {
  char id[STATION_ID_LEN];
  char name[64];
  bool configured, stored, publish, online;
  Profile profile;
  uint32_t last_seen, src_ip, uploads, dropped;
  bool restored;  // the last reading came back from flash and no upload has arrived since the restart
  SrcIf src_if;
  Sample live;
  uint32_t live_rejected;
  uint32_t rejects[F_COUNT];
  uint32_t rej_rule[R_COUNT];
  uint32_t hidden;  // the station's hidden fields (cfg.disabled); the page drops them from history and CSV too
  uint32_t field_t[F_COUNT];      // when each field / promotion / known key last arrived
  uint32_t promo_t[MAX_PROMOTE];
  KnownKey known[MAX_KNOWN];
  uint8_t promoted_mask;
  float promoted[MAX_PROMOTE];
  Promote promote[MAX_PROMOTE];
  uint32_t derived, warming;  // see Station
  bool rain_hours;            // rain_mm estimated from the hour records ("sources":{"rain_mm":"hours"})
  uint8_t derived_n[F_COUNT];
  bool w10;  // wind_avg10m_kmh shown (a value, wind_kmh not hidden)
  float w10v;
  uint32_t w10t;  // when its window last held wind
  uint8_t w10n;
  bool sensor_stale, baro_uncal;
  uint32_t stale_since;
  Notices notices;                // the dashboard chips' texts
  PosixTz tz;                     // their HH:MM
  Outlook outlook;                // the local outlook (not present until snapOutlook)
  bool wufc_on;                   // the WU forecast switch (the store below is shown by its own rules)
  WufcStore wufc;
};
// tz: the bridge's zone for the notices' HH:MM (null = UTC)
void snapLive(const Station& st, uint32_t now_ms, LiveSnap* out, const PosixTz* tz = nullptr);
// the station's local outlook into the copy (the bridge's location for tide and sun; time_ok = a real clock, not
// the persisted one; near = the admitted airport source). Uses the shared outlookBuffer(): call with gLock held.
void snapOutlook(const Station& st, LiveSnap* out, float lat, float lon, bool time_ok, const OutNear* near = nullptr);

// ETag of /api/live?station=: boot id, slot generation, upload counter, config revision,
// plus one bit for online (a station going stale or coming back changes the body) and one for the AP face, plus
// one for the clock sync and the flash restore revision (history restored after SNTP, no upload yet), plus
// `outlook_in`: what the outlook block reads beyond the upload (outlookEtagIn).
void liveEtag(char out[48], uint32_t boot, const Station& st, uint32_t now_ms, bool ap, bool synced, uint32_t outlook_in = 0);
// The outlook's inputs outside the upload: hour closes, the calibration log, the bridge location, a real clock, the
// admitted airport source, the REL hint and the learners' snapshot (a learner tick between uploads changes the body).
uint32_t outlookEtagIn(const Station& st, float lat, float lon, bool time_ok, const OutNear* near = nullptr);
uint32_t outlookNearWord(const OutNear* near);  // the admitted T3 source's word (0 = none)

// ETag of a history view: the slot generation, its uploads / hour closes and flash restores, the calibration log
// (hourly, daily: every row re-renders) and the config revision (a hidden field leaves the body at once).
enum class HistView : uint8_t { Recent, Hourly, Daily };
void histEtag(char out[48], uint32_t boot, const Station& st, HistView h);

// All stations (the tools form, caller holds the lock). d may be null (tests; the AP face also passes null).
// page: the dashboard shell's version, on both faces (an open dashboard an OTA left behind reloads once); null = none.
void renderLive(Out& o, Stations& all, const Clock& c, const Diag* d, const char* page = nullptr);
// One station (the face form): {"now",...,"page"?,"diag"?,"station":{...}}.
void renderLiveOne(Out& o, const LiveSnap& s, const Clock& c, const Diag* d, const char* page = nullptr);
// The pieces, for a caller that snapshots station by station: head + ,"stations":[ station, ... ]} .
void renderLiveHead(Out& o, const Clock& c, const Diag* d, const char* page = nullptr);
// lan = the home-network face: src_ip / src_if only there (the AP face omits diag too).
void renderLiveStation(Out& o, const LiveSnap& s, const Clock& c, bool lan);

// /api/bridge/status (<= 300 B): flags + one state char per station: o online, s stale, x publish off,
// u unconfigured (auto-registered, no overrides).
struct BridgeStatus {
  const char* mode;
  const char* time_src;  // a timeSrcName() literal
  Notices notices;       // the bridge's notices (their texts have no HH:MM)
  bool sta, time, admin_locked, heap_low, ota;  // ota: a firmware upload is being written
  bool metar;  // airport weather on with an airport chosen (the dashboard's plane tab)
  const char* ap;  // fauxcloud's own Wi-Fi name, only while no station is there (empty-state card); else null
  int8_t mqtt;  // -1 off (no broker), 0 down, 1 up
  // counts since boot, rendered only when > 0 (the dashboard's problems chips)
  uint32_t store_fail, too_long, no_slot;
  char no_slot_id[STATION_ID_LEN];  // the last console turned away for lack of a slot ("" = none in the last hour)
  int n;
  char ids[MAX_STATIONS][STATION_ID_LEN];
  char state[MAX_STATIONS];
};
char stationStateChar(const Station& st, uint32_t now_ms);
void renderBridgeStatus(Out& o, const BridgeStatus& b);
// One row shape for RAM rows, flash rows and the open hour / day. The view: hidden fields (cfg.disabled)
// are omitted, keys and all; channel keys come from the map version the record carries.
struct RowView {
  uint32_t hidden;
  const ChMap* maps;
  int n_maps;
  const CalLog* cal;  // applied on read (nullptr = raw)
};
RowView rowView(const Station& st);  // the station's current map only
void hourRow(Out& o, const HourRec& r, const RowView& v);
void dayRow(Out& o, const DayRec& r, const RowView& v);
void renderRecent(Out& o, const Station& st);
void renderHourly(Out& o, const Station& st);
void renderDaily(Out& o, const Station& st, int days, const PosixTz& tz);
// The same from copies of the RAM rings taken under the lock.
void renderRecent(Out& o, const char* id, const Recent& r, uint32_t hidden);
void renderHourly(Out& o, const char* id, const Hourly& h, const RowView& v);
void renderDaily(Out& o, const char* id, const Daily& d, const DayRec* open, int days, const RowView& v);
// From a station's flash ring (a copy of its index, the records read one at a time): hours with from <= key < to;
// days with from_ymd <= ymd < to_ymd, or the newest `newest` days when newest > 0 (days=N, all=1). The open record
// joins when it is in range (always for newest).
class FlashRing;
uint32_t streamAborts();  // ring streams stopped because the client was gone (diag http_stream_abort)
void renderHourlyRing(Out& o, const char* id, const FlashRing& ring, uint32_t from, uint32_t to, const HourRec* open,
                      const RowView& v);
void renderDailyRing(Out& o, const char* id, const FlashRing& ring, uint32_t from_ymd, uint32_t to_ymd, int newest,
                     const DayRec* open, const RowView& v);
void renderStationConfig(Out& o, const Station& st, const Errors* err);  // err: PUT result, else null
// key: the upload-key binding state for the admin ("key":"bound"|"none","key_mismatch":N); null leaves it out. Never
// the key or hash.
struct KeyState {
  bool bound;
  uint32_t mismatch;
  bool has_mac;  // the console's MAC, admin only
  uint8_t mac[6];
  Fingerprint fp = Fingerprint::NotChecked;  // "fp", the 7-in-1 fingerprint of the last upload
  uint8_t cal_n = 0;  // "cal_n", the calibration log's entries (NVS cl:<id>; 0 = not loaded yet)
  char adopt[STATION_ID_LEN] = {};  // the new id the admin may adopt this station as ("" = no offer)
  uint8_t learn_use = 0;  // "learn_use", the learners in use (L1, L2, L5, L6: 0..4), the Learning tile's status
};
void renderStationConfig(Out& o, const char* id, bool stored, const StationConfig& cfg, const Errors* err,
                         const KeyState* key = nullptr);
// with `key`, "adopt":"<new id>" while the admin may adopt a turned-away console from the same device (adopt.h)
void renderBridgeConfig(Out& o, const BridgeConfig& b, const Errors* err);
void renderPage(Out& o, Stations& all, const Clock& c, const char* sw_version, const Diag* d = nullptr);
// The same page in parts (the httpd renders each card from a copy of one station, never under gLock)
void renderPageHead(Out& o, const Clock& c, const char* sw_version, const Diag* d);
void renderStationCard(Out& o, const Station& st, const Clock& c);
void renderPageTail(Out& o, bool any, bool links = true);  // links: the JSON links (not on the AP side)

}  // namespace fc
