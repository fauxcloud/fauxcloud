// app.h -- shared declarations of the sketch modules (Arduino/IDF side). The logic lives in src/ (host-tested);
// these files wire it to Wi-Fi, NVS, httpd, MQTT and FreeRTOS.
//
//   fauxcloud.ino   setup() / loop(): mode decision, job ticks
//   app_core.cpp    globals, logging, clock, NVS (settings, station/bridge configs), dev seed, factory reset, BOOT
//   app_net.cpp     AP/STA per mode, Wi-Fi events, scan, setup join, Wi-Fi change with revert, mDNS, DNS/NTP task
//   app_mqtt.cpp    MQTT client + HA publishing, upload ingest, per-second work, heartbeat, MQTT test
//   app_http.cpp    esp_http_server, shared helpers, public + setup handlers
//   app_admin.cpp   admin login, sessions, admin API (STA side only)
//   app_fwd.cpp     forwarding: targets in NVS, the loop tick, the net-out task (BearSSL), its routes; METAR, WU forecast
#pragma once
#include <Arduino.h>
#include <atomic>
#include <esp_http_server.h>

#include "src/adopt.h"
#include "src/auth.h"
#include "src/clock.h"
#include "src/heapinfo.h"
#include "src/log.h"
#include "src/config.h"
#include "src/learn.h"
#include "src/settings.h"
#include "src/station.h"
#include "src/store.h"
#include "src/tz.h"
#include "src/web_render.h"
#include "src/wu_parse.h"
#include "src/fwd.h"   // FWD_TLS (the declarations below depend on it)
#include "src/wufc.h"

#include "src/diag_bench.h"

// the image marker and SW_VERSION are built from it (app_sys.cpp, app_core.cpp). The bench image says
// "+diag" everywhere -- the marker, fw_version / build, the admin page, HA sw_version, and the forwarding / METAR
// User-Agent and softwaretype (honest towards the services; a bug report names the image)
#define FC_VERSION_BASE "1.1.0-dev.1"
#if FC_DIAG_BENCH
#define FC_VERSION FC_VERSION_BASE "+diag"
#else
#define FC_VERSION FC_VERSION_BASE
#endif

namespace app {
using namespace fc;

enum class Mode : uint8_t { Setup, Run };

extern const char* const SW_VERSION;
extern Mode gMode;

// gLock guards gStations, gBridge, gTz and writes to gSettings after boot. MQTT/NVS I/O never runs under it.
extern Stations gStations;
extern ParkedList gParked;  // stored station configs that got no slot at boot; under gLock
extern ParkedList gLeftover;  // a refused adopt's new id with leftover keys (listed with Forget, nothing else); under gLock
extern BridgeConfig gBridge;
extern PosixTz gTz;
extern Settings gSettings;
extern SemaphoreHandle_t gLock;
extern QueueHandle_t gQueue;  // Upload items from the httpd task to loop()

extern uint8_t gMac[6];
extern char gMac4[5];
extern char gApSsid[40];

struct Upload {
  Sample s;
  uint32_t src_ip;
  SrcIf src_if;
  uint16_t line_tag;  // console line stash (consoleStash), 0 = none
  uint8_t key[32];    // keyed hash of the upload's PASSWORD (the key itself never leaves onUpload)
  uint32_t rx_ms;     // millis() when httpd received it (the rate limit measures this, not the queue)
  uint32_t qhash;     // FNV-1a 32 of the full query string (dedupe)
  Fingerprint fp;     // the 7-in-1 fingerprint (User-Agent, parameter order, MAC OUI)
  bool has_mac;       // the console's MAC (DHCP lease on the AP, ARP entry in the home subnet)
  uint8_t mac[6];
};
// The MAC behind a peer address -- the AP's DHCP leases, or the ARP entry when the peer is in the STA subnet
bool peerMac(uint32_t ip, SrcIf via, uint8_t mac[6]);

// counters written by the httpd / UDP tasks
extern std::atomic<uint32_t> gUploads, gQueueDrops, gOtherHits, gDnsAnswered, gDnsRejected, gNtpAnswered,
    gNtpUnanswered, gLoginFails, gAdminDenied;
// Uploads answered `success` but dropped: from the home network with lan_uploads off, a blocked id, another key
extern std::atomic<uint32_t> gDropLan, gDropBlocked, gDropKey;
// Also answered `success` and lost -- a URI over the httpd limit, no free station slot, and
// an upload < RATE_MIN_MS (4 s) after the previous accepted one of the same id
extern std::atomic<uint32_t> gTooLong, gNoSlot, gRateLimited;
extern std::atomic<uint32_t> gDuplicate;  // the same query again within 120 s (Weathercloud ticked)
// Block list (NVS fc_blk, <= 8 ids): loaded at boot, then read and changed by the httpd task only
bool blockedHas(const char* id);
bool blockedAdd(const char* id);
bool blockedRemove(const char* id);
int blockedList(char ids[][STATION_ID_LEN], int max);
void blockedLoad();
void blockedWipe();
// The station's bound key in NVS (its own entry next to the config; nullptr removes it)
void saveStationKey(const char* id, const uint8_t* key);
bool loadStationKey(const char* id, uint8_t key[32]);

// ---------------------------------------------------------------- app_core.cpp
void logf(const char* tag, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
// Log levels: an Event line (errors, state changes) reaches the boot log always; a Chatter
// line (per upload, per page request, per send, BEAT) only with the admin's "detailed log" switch (gBridge.log_verbose).
// Serial gets both. logf = logl(Log::Event, ...).
// Boot = a boot fact (the head, while it is open, and the tail; after the head closed it is an Event).
enum class Log : uint8_t { Event, Chatter, Boot };
void logl(Log lvl, const char* tag, const char* fmt, ...) __attribute__((format(printf, 3, 4)));
// The boot log in RTC memory (src/bootlog.h, app_sys.cpp). bootlogBegin() is the first statement of setup();
// logf hands every line to bootlogCapture (it redacts msg in place: the STA / pending SSID -> <home Wi-Fi>, the MAC ->
// <mac>); bootlogSend writes the body of GET /api/admin/bootlog.
void bootlogBegin();
void bootlogCapture(const char* tag, char* msg, size_t cap, bool truncated, bool boot = false);
bool bootlogSend(Out& o);
const char* netChangeSsid();  // app_net.cpp: the SSID of a pending Wi-Fi change ("" = none), for the redaction only
bool timeSynced();   // the system clock is valid (>= TIME_FLOOR): SNTP, DHCP-NTP or a browser seed set it -- and
                     // not held (clockHeld against storeClockRef: a clock behind the stored history)
bool clockHeldNow(); // the system clock is valid but held
uint32_t clockHeldCount();  // app_sys.cpp: holds since boot (diag clock_held_n)
uint32_t clockCutCount();   // 24-h acceptances that cut history (diag clock_cut_n; survives the restart they cause)
bool outlookTimeOk();  // the outlook's clock gate: a valid clock that is not the persisted one
uint32_t nowS();     // unix seconds when synced, uptime seconds otherwise
uint32_t uptimeS();  // 64-bit timer based, never wraps in practice
Clock clockNow();
uint32_t apIpNet();  // 192.168.77.1, network byte order
// The only reader of the IDF heap API besides heapSample -- byte-addressable figures (8BIT) and
// min8 is the monitor's local minimum (heapSample owns the monitor)
HeapNow heapNow();
#if FC_DIAG_BENCH  // bench only (diag.ota_heap)
void otaHeapBoot();                 // app_ota.cpp: the RTC record of the last OTA write -> otaHeapLast (setup)
bool otaHeapLast(OtaHeapRec* out);  // false: no record (no OTA wrote one, or a power cut)
void otaHeapFeed(uint32_t low8, uint32_t largest8);  // heapSample's low-water while a write runs
#endif
OtaHeapRec& rtcOtaHeap();           // app_sys.cpp: the record's slot in the one RTC struct
void heapSample();                  // loop(), once a second: feeds the 10-minute minimum, logs new low-waters
uint32_t heapMinBoot();             // the 8-bit free heap's minimum since boot
// The since-boot 8-bit minimum with its second's context (diag heap_min8_t / _ctx / _socks); heapCtxMark records
// an event that leaves no counter (HeapCtx bits, src/heapinfo.h), from any task
HeapDip heapDip();
void heapCtxMark(uint16_t bits);
uint32_t mqttPublishedN();           // app_mqtt.cpp: publishes since boot (loop task)
int mqttOutbox();                   // esp-mqtt outbox bytes (-1 = no client)
int httpSessions();                  // open httpd sessions (-1 = no server)
bool heapLow(uint32_t* min10);      // the 3-min minimum of the byte heap < HEAP_LOW_FREE8 (18 KB, 8BIT); min10 (the 10-min
                                    // minimum) may be null
void httpCount(uint32_t busy_ms, bool not_modified, bool aborted);  // one call per HTTP response (httpd task)
void httpMetrics(uint32_t* requests, uint32_t* n304, uint32_t* busy_max_ms, uint32_t* aborted);
void ipToStr(uint32_t ip, char out[16]);
void scheduleReboot(uint32_t delay_ms);
void requestFactoryReset();
bool rebootPending();        // a reboot is planned (gRebootAtMs set)
bool factoryResetPending();
void coreLoop();     // pending reboot / factory reset, BOOT button

void nvsOpen();
void devSeed();
void loadSettings();
bool saveSettings();          // gSettings -> NVS; caller must not hold gLock
void loadBridge();            // the stored bridge settings (both modes; defaults when none)
void loadDataConfig();        // loadBridge() + stored station configs (run mode)
bool saveBridge();            // gBridge -> NVS
bool saveStation(int slot);   // the station's overrides -> NVS
void deleteStationConfig(const char* id);  // removes its NVS entry (forget)
void applyTz();
void factoryReset();          // wipe the namespace, restart (into setup mode)
bool cryptoSelfTest(uint32_t* ms);

// ---------------------------------------------------------------- app_store.cpp (LittleFS rings, history on flash)
void storeBegin();                    // mount and the format plan (Pending + no layout: RAM only), rings scanned
void storeImageValid();               // the image was marked valid -- a waiting store formats once now
bool storeMounted();                  // the rings are in use
void storeRingOwner(int k, char out[STATION_ID_LEN]);  // ring k's owner id from its header ("" = free / not mounted)
bool storeLayoutBad();                // another partition table or ring layout: history RAM only (store_layout)
bool storeFormat();                   // factory reset: format + the empty ring layout
FileIo& storeIo();
uint32_t storeMsMax();                // longest history write pass (records + snapshot), ms
uint32_t storeFails();                // history writes that failed, or a foreign layout left untouched
void storeRestore(Station& st);       // caller holds gLock: the station's rings into RAM, then its snapshot entry
void storeMarkDue(int slot, bool hour_closed, bool day_closed);
void storeTick();                     // loop(), outside gLock: new records to the rings, then the shared snapshot
bool storeBusy();                     // history writes due (any due bit of a slot)
void storeFlushAll();                 // the snapshot now (before a planned restart)
void storeForget(const char* id);     // the station's rings emptied and freed
bool storeRingCopy(const char* id, char kind, FlashRing* out);  // httpd: a ring index copy ('h' / 'd')
struct StoreRingRow {
  char id[STATION_ID_LEN];
  int hours, days;
};
int storeRingList(StoreRingRow out[RING_SLOTS]);  // the owned rings by header id
bool storeOwns(const char* id);                   // a ring belongs to `id`
// Learners: the oldest valid record of the station's flash ring with a key > `after` (false: none, or no ring).
// The cursor keeps the walk's place: one binary search, then record by record; a ring that rotated
// or was rewritten since (count or oldest key changed) is searched again. {-1, 0, 0} = start.
struct RingCursor {
  int i, count;
  uint32_t oldest;
};
bool storeNextHour(const char* id, uint32_t after, RingCursor* cur, HourRec* out);
bool storeNextDay(const char* id, uint32_t after, RingCursor* cur, DayRec* out);
void requestForgetFlash(const char* id);          // forget a station that exists on flash only (loop task)
uint32_t storeDropOld();              // closes refused as older than the ring's newest
uint32_t storeCut();                  // torn segments cut
uint32_t storeFailAppend();           // appends that failed (retried)
uint32_t storeFutureDropped();        // records with a future key cut
// The end of the newest trusted (non-approximate) hour in the rings, raised by every
// close / append (any task may read it); a held clock (clock.h clockHeld) is "no real time yet" everywhere
uint32_t storeClockRef();
void storeNoteHour(uint32_t hour, uint16_t quality);  // a closed or open hour: raises the reference when trusted
void storeNewestKeys(const char* id, uint32_t* hour, uint32_t* ymd);  // a station's newest flash keys
void storeRunRelease();               // the learner cursor's run buffer back (an abandoned walk)
uint32_t storeFutureKept();           // cuts stopped at a trusted record past the limit
uint32_t storeRetrySteps();           // append back-off steps since boot (diag store_retry)
uint8_t storeSnapFrom();             // the snapshot version read at the first restore (7, 8; 0 = none)
// The 24-h acceptance of a held clock (loop task): records newer than `now` cut from every ring (approximate or
// not), future open hours dropped from the snapshot, the reference recomputed, the store frozen until the restart
// the caller plans. Returns the records cut.
int storeClockAccept(uint32_t now);
bool storeSeed(const char* id, uint32_t last_hour, uint32_t last_ymd, int extra, char* msg, size_t cap);  // test build
bool otaPending();                    // app_sys.cpp: this image is not confirmed yet
// NVS ch:<id>: the channel map versions, oldest first; save appends (the oldest drops past CH_VERSIONS)
int loadChannelMaps(const char* id, ChMap out[CH_VERSIONS]);
void saveChannelMap(const char* id, const ChMap& m);
void deleteChannelMaps(const char* id);
void loadCalLog(Station& st);  // NVS cl:<id>; none -> {0, the current calibration} (saved)
bool readCalLog(const char* id, CalLog* log);  // NVS cl:<id> as stored, never written; false = none
void saveCalLog(const char* id, const CalLog& log);
void deleteCalLog(const char* id);
// Learners (app_core.cpp: NVS; app_learn.cpp: the per-slot state)
void nvsLearnKey(const char* id, char kind, char out[16]);  // kind 'r' -> "lr:<id>", 'm' -> "lm:<id>"
size_t nvsGetBlob(const char* key, void* p, size_t cap);    // stored length (0 = none; > cap = not read)
bool nvsPutBlob(const char* key, const void* p, size_t len);
void nvsDelKey(const char* key);
bool nvsStats(uint32_t* used, uint32_t* free_entries);
void learnTick();                       // loop(), after storeTick(), outside gLock: backfill, catch-up, hour / day closes
void learnFlushAll();                   // the blobs now (before a planned restart)
bool learnBusy();                       // a migration / backfill pass runs (loop task; forwarding waits)
void learnDelete(const char* id);       // Forget: both blobs gone, the RAM state back to priors
uint32_t learnMacHash(const char* id);  // the persisted core's console MAC hash (0 = unknown)
// Station adoption (adopt.h): the offer, the route's re-check (both gLock held), the journal + restart, the boot side
void adoptOffer(const char* from, char to[STATION_ID_LEN]);
bool adoptTake(const char* from, const char* to, uint8_t key[32]);
bool adoptWriteJournal(const char* from, const char* to, const uint8_t key[32], const char** why);
bool stationKeysStored(const char* id);  // any per-station NVS key of `id` (Forget's way out for leftover keys)
void adoptBootBegin();   // boot, before the move: counts this boot in the journal (or gives it up after ADOPT_MAX_BOOTS)
bool adoptGaveUp(char from[STATION_ID_LEN], char to[STATION_ID_LEN]);  // this boot gave a journal up (its ids)
bool adoptJournalRead(AdoptJournal* j);  // the journal this boot runs (a valid one, counted: adoptBootBegin said Run)
void adoptJournalRemove();
bool adoptBootNvs();     // before the stations load: the NVS move of a journal found (true: done or no journal)
bool adoptBootStore();   // after storeBegin: the ring headers and the snapshot entry (true: done or no journal)
void adoptBootFwd(bool rest_ok);  // after the forwarding targets load: renamed; the journal goes only when all went well
bool learnCopy(const char* id, LearnView* v, LearnMeta* m);  // httpd: the station's view, false = no such station
bool learnRelearn(const char* id);  // "Restart learning" asked (httpd); the loop applies it (false = no state)

// ---------------------------------------------------------------- app_sys.cpp (health, crash data, watchdogs)
struct CrashInfo {
  bool present;
  char reason[64];
  char task[16];
  uint32_t pc;
};
void sysBoot();                       // reset reason + restarts (NVS fc_sys), OTA state, crash data, Task WDT
void sysTick();                       // loop(), every second in run mode: rollback health check, network watchdog
void sysLoop();                       // loop(): the debug hang
void sysPlanRestart(const char* why, uint32_t delay_ms);  // restart with a reason for the next boot
void sysCountFragRestart();          // +1 "fragrst" in fc_sys (wiped by a factory reset)
uint32_t fragRestarts();
void sysWipe();                       // factory reset: fc_sys too
const char* buildText();              // "<version> <date> <time>" of the running image
const char* lastReset();
const char* resetRaw();            // "esp_reset_reason/rtc cpu0/rtc cpu1" of this boot
uint32_t restarts();
const char* otaStateText();           // stable | pending | rolled_back
const CrashInfo& lastCrash();
uint32_t staReinits();
void markHttpServed();
void requestHang();
bool crashErase();
// The httpd task's render buffer, lent to a handler that streams without sendRendered (crashStream)
constexpr size_t HTTP_SCRATCH_LEN = 1024;
char* httpScratch();
bool sysFlag(const char* key);     // one-shot markers in fc_sys (wiped by a factory reset)
void sysSetFlag(const char* key);
bool crashStream(bool (*put)(void* ctx, const uint8_t* data, size_t len), void* ctx, size_t* total);
// The clock ladder (src/clock.h): fc_sys "t_last", the SNTP source, browser seeds
void clockBoot();                  // after sysBoot(): restore t_last, the source of a clock kept across a restart
void clockTick();                  // loop(), every second (both modes)
void clockPersistNow();            // before a planned restart
bool clockSeed(uint64_t ms, const char* who);  // setup page / admin login; true = the system clock was set
TimeSrc timeSrc();                 // the best rung now (any task)
void clockForget();                // factory reset: the clock is not carried into the next boot

// ---------------------------------------------------------------- app_ota.cpp (firmware upload)
bool otaActive();
size_t otaSlotSize();
const char* otaStart(httpd_req_t* req);  // nullptr = the OTA task owns the request; "busy" = one is running

// ---------------------------------------------------------------- app_net.cpp
void netStartRun();
void netStartSetup(const char* setup_pass);
void netLoop();                       // mDNS, AP restart, Wi-Fi change / setup join ticks
bool staUp();
void staIpStr(char out[16]);
void staMaskStr(char out[16]);  // the home network's subnet mask, "" while not joined
void apLineSettle();  // the heartbeat: a flapping AP client's held changes end in one line with the true state
void scanStart();                     // async scan (setup mode starts one at boot)
void scanJson(Out& o, bool again);    // the cached result; `again` rescans, at most once per 15 s
void setupJoinStart();                // gSettings holds the credentials to try
const char* setupState(char* buf, size_t cap);  // idle | busy | joining | joined | failed:<auth|no_ap|dhcp|timeout>
void wifiChangeStart(const char* ssid, const char* pass);
const char* wifiChangeStatus();       // "" | busy | ok | failed:<code> (failed = the old network is back)
void requestApRestart();
void netReinit();                     // network watchdog: drop and rejoin the home Wi-Fi once
void requestMdnsRestart();
void udpStart(bool ntp);

// ---------------------------------------------------------------- app_mqtt.cpp
void mqttLoop();                      // client start/restart requests, MQTT test ticks
void requestMqttRestart();
void requestMqttStart();             // a client only when there is none (Wi-Fi join)
uint32_t mqttInitFails();            // esp_mqtt_client_init returned null (diag mqtt_init_fail)
bool mqttUp();
uint32_t mqttReconnects();
void markStationChanged(int slot);
extern std::atomic<uint32_t> gBridgeRev;  // /api/bridge/status ETag: bumped by loop() when the status changes
void bridgeStatusNow(BridgeStatus* b);    // takes gLock
void requestForget(int slot);
void annWipe();  // factory reset: the list of station ids announced to HA (NVS fc_ann)
// A stale promotion's HA discovery, retracted on the first MQTT connect (queued at config load)
void queuePromoRetract(const char* station_id, const char* key);         // loop(): HA cleanup, NVS delete, slot reuse
void ingestQueued();
uint32_t lastIngestMs();              // millis() of the last upload taken off the queue (0 = none)
bool mqttBusy();                      // HA discovery / retraction pending, or the publish stall
void everySecond();
void heartbeat();
Diag diagNow();
void mqttTestStart(const char* host, uint16_t port, const char* user, const char* pass);
void mqttTestJson(Out& o);

// ---------------------------------------------------------------- app_http.cpp
void httpStart();
uint32_t setupIdleMs();  // since the last setup-page request (setup mode time box)
SrcIf srcIf(httpd_req_t* req);
uint32_t peerIp(httpd_req_t* req);
bool queryParam(httpd_req_t* req, const char* key, char* out, size_t cap);
constexpr size_t PUT_MAX_BODY = 3072;
constexpr size_t BODY_DRAIN_MAX = 16384;  // a too-big body up to this is read and discarded, then answered
bool readBody(httpd_req_t* req, size_t* len);  // into bodyBuf(); false = already closed after the reply
void stopReading(httpd_req_t* req);            // shutdown(SHUT_RD) + close after this request
char* bodyBuf();
typedef void (*RenderCb)(Out& o, httpd_req_t* req);
esp_err_t sendRendered(httpd_req_t* req, const char* type, RenderCb render, bool lock = true);
esp_err_t sendJson(httpd_req_t* req, const char* json);
esp_err_t onOther(httpd_req_t* req, httpd_err_code_t err);
void replyUnauth(httpd_req_t* req);  // 401 {"error":"auth"} for a signed-out admin call (STA + CSRF header)
esp_err_t serveGz(httpd_req_t* req, const uint8_t* data, size_t len, const char* version);

esp_err_t onAdminPage(httpd_req_t* req);  // app_http.cpp (the only includer of web_assets.h)
// Registers handlers through the request accounting wrapper (diag counters + one request-log line each).
void registerUris(httpd_handle_t srv, const httpd_uri_t* uris, size_t n);
// At an OTA start the idle page connections close (keep = the OTA's own socket); returns how many.
int httpCloseIdlePages(int keep_fd);
uint32_t httpPurged();  // idle page sessions closed by the session cap, since boot
uint32_t httpShed();    // page requests answered 503 under low memory, since boot
uint32_t httpPaced();  // page answers that waited for the heap (robust.h pagePace)
int httpInFlight();                  // requests being handled now (dispatch)
uint32_t httpLastPageMs();           // millis() when the last page (non-upload) request ended (0 = none)
int otaSockFd();  // the OTA upload's socket while one runs, else -1
void otaCountSkipped();  // a console upload answered but not processed during the OTA (quiet mode)
// Console lines: the upload handler stashes the redacted line, loop() moves it into the station under
// gLock while ingesting (the station may not exist before that). A line overwritten before loop() got to it is lost.
uint16_t consoleStash(const char* redacted);
void consoleApply(Station& st, uint16_t tag, uint32_t t);  // caller holds gLock
extern ConsoleRing gConsole;  // the shared console ring, under gLock
void consoleForget(const char* id);  // takes gLock

// ---------------------------------------------------------------- app_admin.cpp
void registerAdmin(httpd_handle_t srv);
bool adminGateReq(httpd_req_t* req);  // the admin gate (STA + CSRF + session) for routes in other modules
void adminLockInfo(bool* active, uint32_t* remaining_s, int* failures);
void adminLockTick();                 // loop(): logs the end of an account-wide lock

// ---------------------------------------------------------------- app_fwd.cpp (forwarding, METAR, WU forecast)
void fwdBegin();                      // run mode, after loadDataConfig(): the targets from NVS
void fwdTick();                       // loop(), every second (everySecond, after learnTick): decide, render, hand over
void fwdAbort(const char* why);       // a running send stops at its next step (otaStart)
void fwdDiag(Diag* d);                // the fwd_* diag sums
void registerFwd(httpd_handle_t srv); // /api/admin/forwarding*, /api/admin/metar* (FWD_TLS)
esp_err_t metarPublicGet(httpd_req_t* req);  // GET /api/metar (FWD_TLS builds; app_http.cpp adds the face check)
// The WU forecast line (app_fwd.cpp). wufcSnap / wufcEtagWord with gLock held; wufcOn / wufcHa take it themselves.
bool wufcSnap(WufcStore* out);           // false: the switch off (or FWD_TLS=0): no `wufc` in /api/live
uint32_t wufcEtagWord();                // the live ETag's word (the store's generation, what is shown now)
bool wufcOn();
uint32_t wufcHa(char* state, size_t sc, char* attr, size_t ac);  // the HA sensor's state + attributes; 0 = off
bool outlookNearInput(uint32_t now, OutNear* out);  // the admitted main-airport report (gLock held; false without FWD_TLS)
// METAR eligible (on, location, an airport <= 60 km); gLock held. Without FWD_TLS an inline false, so
// the HTTP-only image carries no "metar" symbol (CI's gate)
#if FWD_TLS
bool metarEligibleLocked();
#else
inline bool metarEligibleLocked() { return false; }
#endif
bool metarShownLocked();  // airport weather on with an airport chosen (gLock held); FWD_TLS builds only
void metarEtagParts(uint32_t* gen, uint32_t* cfg, uint32_t* now);  // /api/metar's ETag inputs (FWD_TLS builds)
// heapSample(): a send runs now / ran since the previous sample, and the free heap read before it started
void fwdHeapState(bool* running, bool* ran_since, uint32_t* pre_send);
#if FC_DIAG_BENCH
void fwdNoteHeapLow(uint32_t low);    // ... the sample's local minimum goes to fwd_heap_min
#endif
bool fwdIdle();                       // no send posted or running (otaStart waits for it)

}  // namespace app
