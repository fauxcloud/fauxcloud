// app_core.cpp -- globals, logging, clock, NVS persistence, dev seed, factory reset, BOOT button (see app.h).
#include <Preferences.h>
#include <WiFi.h>
#include <esp_heap_caps.h>
#include <esp_random.h>
#include <esp_timer.h>
#include <esp_wifi.h>
#include <nvs.h>
#include <sys/time.h>
#include <time.h>

#include "app.h"
#include "src/ha_discovery.h"
#include "src/bootlog.h"
#include "board.h"
#include "src/led.h"
#include "src/lifecycle.h"
#include "src/robust.h"
#include "src/fwd_sched.h"
#include "src/metrics.h"

#if __has_include("secrets.h")
#include "secrets.h"
#define HAVE_SECRETS 1
#else
#define HAVE_SECRETS 0
#endif

namespace app {

const char* const SW_VERSION = "fauxcloud " FC_VERSION;
Mode gMode = Mode::Setup;
Stations gStations;
BridgeConfig gBridge;
ParkedList gParked;  // configs without a slot (under gLock after boot)
ParkedList gLeftover;  // leftover keys of a refused adopt's new id: listed with Forget only (under gLock)
PosixTz gTz;
Settings gSettings;
SemaphoreHandle_t gLock;
QueueHandle_t gQueue;
uint8_t gMac[6];
char gMac4[5];
char gApSsid[40];
std::atomic<uint32_t> gDropLan{0}, gDropBlocked{0}, gDropKey{0};
std::atomic<uint32_t> gTooLong{0}, gNoSlot{0}, gRateLimited{0}, gDuplicate{0};
std::atomic<uint32_t> gUploads{0}, gQueueDrops{0}, gOtherHits{0}, gDnsAnswered{0}, gDnsRejected{0}, gNtpAnswered{0},
    gNtpUnanswered{0}, gLoginFails{0}, gAdminDenied{0};

static const char* NVS_NS = "fauxcloud";
[[maybe_unused]] static const char* NVS_DEV_NS = "fc_dev";  // survives a factory reset (unused without secrets.h)
static Preferences gPrefs;                 // NVS_NS; used by loop() and the httpd task (never both at once in practice)
static SemaphoreHandle_t gNvsLock;         // serializes Preferences access across tasks
static SemaphoreHandle_t gSaveBufLock;     // saveStation's 3.8 KB buffer, used by the loop and the httpd task
static volatile uint32_t gRebootAtMs = 0;
static volatile bool gFactoryResetPending = false;

// ---------------------------------------------------------------- logging, clock
// Inlined into both entry points, so a line costs no extra stack frame. A Chatter
// line with the detailed log off goes nowhere -- not formatted, not on the UART (no TX buffer: a 250-B line holds the
// loop ~10 ms), not in the ring; logKeeps() is the one rule for both sinks (src/log.h, host-tested)
static inline __attribute__((always_inline)) void logImpl(bool keep, bool boot, const char* tag, const char* fmt,
                                                         va_list ap) {
  if (!keep) return;
  char msg[384];
  const int n = vsnprintf(msg, sizeof msg, fmt, ap);
  Serial.printf("%10lu [%s] %s\n", (unsigned long)millis(), tag, msg);
  bootlogCapture(tag, msg, sizeof msg, n >= (int)sizeof msg, boot);  // after Serial (msg is redacted in place)
}
void logf(const char* tag, const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  logImpl(true, false, tag, fmt, ap);
  va_end(ap);
}
void logl(Log lvl, const char* tag, const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  logImpl(logKeeps(lvl == Log::Chatter, gBridge.log_verbose), lvl == Log::Boot, tag, fmt, ap);
  va_end(ap);
}

// A valid clock behind the stored history is held: "no real time yet" for every writer
static bool syncedAt(uint32_t t) { return timeValid(t) && !clockHeld(t, storeClockRef()); }
bool timeSynced() { return syncedAt((uint32_t)time(nullptr)); }
bool clockHeldNow() {
  const uint32_t t = (uint32_t)time(nullptr);
  return clockHeld(t, storeClockRef());
}
uint32_t uptimeS() { return (uint32_t)(esp_timer_get_time() / 1000000LL); }
// One read of the clock, so `now` and `synced` always agree (a sync between two reads would pair an
// uptime-based now with synced = true and open a 1970 hour)
Clock clockNow() {
  const time_t t = time(nullptr);
  const uint32_t ms = (uint32_t)millis();
  const bool synced = syncedAt((uint32_t)t);
  return Clock{synced ? (uint32_t)t : ms / 1000, synced, uptimeS(), ms, timeSrcName(timeSrc())};
}
uint32_t nowS() { return clockNow().now; }
uint32_t apIpNet() { return htonl(0xC0A84D01); }  // 192.168.77.1 (away from 192.168.4.1 and its HSTS memories)

// ---------------------------------------------------------------- 10-minute windows
static portMUX_TYPE gMetMux = portMUX_INITIALIZER_UNLOCKED;
static MinuteWindow gHeapWin(MinuteWindow::Kind::Min);
#if FC_DIAG_BENCH
static MinuteWindow gBusyWin(MinuteWindow::Kind::Max);  // http_busy_ms_max is bench only
#endif
static uint32_t gHttpRequests = 0, gHttp304 = 0, gHttpAborted = 0;

static uint32_t gHeapMinBoot = UINT32_MAX;  // loop() only; the byte-addressable heap
// The since-boot 8-bit minimum with its second's context (written by loop() under gMetMux, read by the diag);
// the marks of events that leave no counter (a socket accepted, a history write, a learner pass), cleared each sample
static HeapDip gDip;
static std::atomic<uint32_t> gHeapCtxAcc{0};
void heapCtxMark(uint16_t bits) { gHeapCtxAcc.fetch_or(bits, std::memory_order_relaxed); }
HeapDip heapDip() {
  portENTER_CRITICAL(&gMetMux);
  const HeapDip d = gDip;
  portEXIT_CRITICAL(&gMetMux);
  return d;
}

// Every heap figure of the sketch is read here or in heapSample (test/tools/test_heap_calls.py checks it)
HeapNow heapNow() {
  HeapNow h = {};
  h.free8 = heap_caps_get_free_size(MALLOC_CAP_8BIT);
  h.largest8 = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
  // The allocator's local minimum since heapSample last restarted its monitor (not restarted while a forwarding
  // send runs: the send's whole connect); the net-out task reads it after a connect
  h.min8 = heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT);
  return h;
}

// The window takes the allocator's own low-water since the previous sample (the
// local-minimum monitor, restarted every second), not the free heap at the sample instant -- a burst that comes and
// goes between two samples would be invisible. While the monitor runs, getMinFreeHeap() reports the local value, so the
// since-boot minimum is kept here (heapMinBoot).
void heapSample() {
  static bool mon = false;
  static uint32_t logged = UINT32_MAX;
  // While a forwarding send ran since the previous sample, the 10-minute windows
  // take the free heap from just before the send (or at this instant once it ended) and the allocator's local minimum
  // (the send's dip) goes to fwd_heap_min, so "Low memory" keeps its meaning (memory the core lacks); the since-boot
  // minima below stay the true ones
  bool fwdRun = false, fwdRan = false;
  uint32_t fwdPre = 0;
  fwdHeapState(&fwdRun, &fwdRan, &fwdPre);
  // The local minimum, read before the monitor restarts; the window and every threshold take the byte-addressable
  // heap (not the INTERNAL figure, which includes ~40 KB of IRAM)
  const uint32_t low8 = heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT), ms = millis();
  // not restarted while a send runs, so the send reads its minimum over the whole connect (heapNow().min8)
  // and the first sample after it still holds the whole dip
  if (!fwdRun) {
    if (mon) heap_caps_monitor_local_minimum_free_size_stop();
    mon = heap_caps_monitor_local_minimum_free_size_start() == ESP_OK;
  }
  const HeapNow hn = heapNow();
  const FwdHeapFeed hf = fwdHeapFeed(fwdRun, fwdRan, low8, hn.free8, fwdPre);
#if FC_DIAG_BENCH
  if (hf.to_fwd) fwdNoteHeapLow(low8);  // fwd_heap_min on the byte-addressable heap, like the send's own samples
#endif
  portENTER_CRITICAL(&gMetMux);
  gHeapWin.feed(ms, hf.window);
  uint32_t busy = 0;
#if FC_DIAG_BENCH
  gBusyWin.value(ms, &busy);
#endif
  const uint32_t reqs = gHttpRequests;
  portEXIT_CRITICAL(&gMetMux);
  // who was busy in this second, from counters and flags that exist already; kept only with a new minimum
  static uint32_t prevReq = 0, prevPub = 0, prevUp = 0;
  const uint32_t pub = mqttPublishedN(), up = gUploads.load();
  uint32_t ctx = gHeapCtxAcc.exchange(0, std::memory_order_relaxed);
  if (reqs != prevReq || httpInFlight() > 0) ctx |= HC_HTTP;
  if (fwdRun || fwdRan) ctx |= HC_FWD;
  if (pub != prevPub) ctx |= HC_MQTT;
  if (storeBusy()) ctx |= HC_STORE;
  if (learnBusy()) ctx |= HC_LEARN;
  if (otaActive()) ctx |= HC_OTA;
  if (up != prevUp) ctx |= HC_UPLOAD;
  prevReq = reqs, prevPub = pub, prevUp = up;
  if (gDip.lower(low8)) {  // read outside gMetMux: only loop() writes gDip
    const int socks = httpSessions();  // only on a new minimum
    const uint32_t upS = uptimeS();
    portENTER_CRITICAL(&gMetMux);
    gDip.note(low8, upS, (uint16_t)ctx, socks);
    portEXIT_CRITICAL(&gMetMux);
  }
  if (low8 < gHeapMinBoot) gHeapMinBoot = low8;
  // Diagnosis (who holds the heap in a wedge): every 10 s while the heap is tight
  static uint32_t lastDiagMs = 0;
  const uint32_t free8 = hn.free8, largest8 = hn.largest8;
#if FC_DIAG_BENCH
  if (otaActive()) otaHeapFeed(low8, largest8);  // the allocator low-water, not a 64-KB sample
#endif
  // the first line of a tight spell and one per 10 min are events, the 10-s lines between chatter
  static bool tightSpell = false;
  static uint32_t tightEventMs = 0;
  const bool tight = heapTight(free8, largest8);
  if (!tight) tightSpell = false;
  if (tight && !hf.to_fwd && ms - lastDiagMs >= 10000) {  // not for a send's dip
    lastDiagMs = ms;
    const bool ev = !tightSpell || ms - tightEventMs >= 600000;
    if (ev) tightEventMs = ms;
    tightSpell = true;
    multi_heap_info_t hi;
    heap_caps_get_info(&hi, MALLOC_CAP_8BIT);
    logl(ev ? Log::Event : Log::Chatter, "HEAP", "tight: free8 %lu largest8 %lu | blocks %u/%u | mqtt %s outbox %d B | httpd sessions %d | "
         "ap_clients %d", (unsigned long)free8, (unsigned long)largest8, (unsigned)hi.free_blocks, (unsigned)hi.allocated_blocks,
         mqttUp() ? "up" : "down", mqttOutbox(), httpSessions(), (int)WiFi.softAPgetStationNum());
  }
  static FragWatch frag;  // a wedged, fragmented heap restarts the bridge (robust.h)
  static bool fragInit = false;
  if (!fragInit) {  // at most one heap_frag restart per 30 min
    fragInit = true;
    if (strcmp(lastReset(), "heap_frag") == 0) frag.quiet(FragWatch::FRAG_QUIET_MS);
  }
  if (frag.feed(ms, largest8, gUploads.load(), otaActive())) {
    logf("HEAP", "largest block < %lu B for %lu s with no upload: planned restart (heap_frag)",
         (unsigned long)FragWatch::FRAG_LARGEST_MIN, (unsigned long)(FragWatch::FRAG_HOLD_MS / 1000));
    sysCountFragRestart();
    sysPlanRestart("heap_frag", 200);  // the reboot path flushes the snapshots first (storeFlushAll)
  }
  if (logged == UINT32_MAX || logged - gHeapMinBoot >= 1024) {  // a new low-water, 1 KB steps: who was busy then
    logged = gHeapMinBoot;
    // the dip's context on the line too (the diag dies with a restart; the ring is zeroed at every boot,
    // but its tail is copied into the previous-boot block first, so this line survives one restart)
    const HeapLowLine hl{gHeapMinBoot, free8, largest8, WiFi.isConnected() ? (int)WiFi.RSSI() : 0,
                         (int)WiFi.softAPgetStationNum(), mqttUp(), busy, gDip};
    char msg[BOOTLOG_LINE_MAX];
    heapLowLine(msg, sizeof msg, hl);
    logf("HEAP", "%s", msg);
  }
}

uint32_t heapMinBoot() { return gHeapMinBoot == UINT32_MAX ? heapNow().free8 : gHeapMinBoot; }

bool heapLow(uint32_t* min10) {
  const uint32_t now = millis();
  uint32_t v3 = 0, v10 = 0;
  portENTER_CRITICAL(&gMetMux);
  const bool any3 = gHeapWin.value(now, &v3, HEAP_LOW_MINUTES);
  const bool any10 = min10 && gHeapWin.value(now, &v10);
  portEXIT_CRITICAL(&gMetMux);
  if (!any3) v3 = heapNow().free8;  // no sample yet: the live heap (an empty 10-min window has an empty 3-min one)
  if (min10) *min10 = any10 ? v10 : v3;
  return heapLowBy(v3);  // the HEAP_LOW_MINUTES minimum of the byte-addressable heap below HEAP_LOW_FREE8
}

void httpCount(uint32_t busy_ms, bool not_modified, bool aborted) {
  const uint32_t ms = millis();
  portENTER_CRITICAL(&gMetMux);
  gHttpRequests++;
  if (not_modified) gHttp304++;
  if (aborted) gHttpAborted++;
#if FC_DIAG_BENCH
  gBusyWin.feed(ms, busy_ms);
#else
  (void)ms, (void)busy_ms;
#endif
  portEXIT_CRITICAL(&gMetMux);
}

void httpMetrics(uint32_t* requests, uint32_t* n304, uint32_t* busy_max_ms, uint32_t* aborted) {
  uint32_t busy = 0;
  portENTER_CRITICAL(&gMetMux);
  *requests = gHttpRequests;
  *n304 = gHttp304;
  *aborted = gHttpAborted;
#if FC_DIAG_BENCH
  gBusyWin.value(millis(), &busy);
#endif
  portEXIT_CRITICAL(&gMetMux);
  *busy_max_ms = busy;
}

void ipToStr(uint32_t ip, char out[16]) {
  const uint8_t* b = (const uint8_t*)&ip;
  snprintf(out, 16, "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
}

void scheduleReboot(uint32_t delay_ms) {
  uint32_t at = millis() + delay_ms;
  gRebootAtMs = at ? at : 1;
}

void requestFactoryReset() { gFactoryResetPending = true; }
bool rebootPending() { return gRebootAtMs != 0; }
bool factoryResetPending() { return gFactoryResetPending; }

// ---------------------------------------------------------------- NVS: settings
// One key per setting (NVS keys <= 15 chars).
static const char *K_SSID = "s_ssid", *K_SPASS = "s_spass", *K_MHOST = "s_mhost", *K_MPORT = "s_mport",
                  *K_MUSER = "s_muser", *K_MPASS = "s_mpass", *K_APPASS = "s_appass", *K_ADMIN = "s_admin",
                  *K_HOST = "s_host", *K_DONE = "s_done";

void nvsOpen() {
  gNvsLock = xSemaphoreCreateMutex();
  gSaveBufLock = xSemaphoreCreateMutex();  // at boot, before any task can save a station
  if (!gPrefs.begin(NVS_NS, false)) logf("NVS", "namespace %s cannot be opened -- running on defaults", NVS_NS);
}

static void getStr(const char* key, char* out, size_t cap) {
  if (!gPrefs.isKey(key)) return;
  String v = gPrefs.getString(key, "");
  snprintf(out, cap, "%s", v.c_str());
}

void loadSettings() {
  Settings s = defaultSettings();
  xSemaphoreTake(gNvsLock, portMAX_DELAY);
  getStr(K_SSID, s.sta_ssid, sizeof s.sta_ssid);
  getStr(K_SPASS, s.sta_pass, sizeof s.sta_pass);
  getStr(K_MHOST, s.mqtt_host, sizeof s.mqtt_host);
  if (gPrefs.isKey(K_MPORT)) s.mqtt_port = gPrefs.getUShort(K_MPORT, 1883);
  getStr(K_MUSER, s.mqtt_user, sizeof s.mqtt_user);
  getStr(K_MPASS, s.mqtt_pass, sizeof s.mqtt_pass);
  getStr(K_APPASS, s.ap_pass, sizeof s.ap_pass);
  getStr(K_ADMIN, s.admin_hash, sizeof s.admin_hash);
  getStr(K_HOST, s.hostname, sizeof s.hostname);
  s.setup_done = gPrefs.getBool(K_DONE, false);
  xSemaphoreGive(gNvsLock);
  gSettings = s;
}

bool saveSettings() {
  xSemaphoreTake(gLock, portMAX_DELAY);
  const Settings s = gSettings;
  xSemaphoreGive(gLock);
  xSemaphoreTake(gNvsLock, portMAX_DELAY);
  bool ok = true;
  ok &= gPrefs.putString(K_SSID, s.sta_ssid) == strlen(s.sta_ssid);
  ok &= gPrefs.putString(K_SPASS, s.sta_pass) == strlen(s.sta_pass);
  ok &= gPrefs.putString(K_MHOST, s.mqtt_host) == strlen(s.mqtt_host);
  ok &= gPrefs.putUShort(K_MPORT, s.mqtt_port) > 0;
  ok &= gPrefs.putString(K_MUSER, s.mqtt_user) == strlen(s.mqtt_user);
  ok &= gPrefs.putString(K_MPASS, s.mqtt_pass) == strlen(s.mqtt_pass);
  ok &= gPrefs.putString(K_APPASS, s.ap_pass) == strlen(s.ap_pass);
  ok &= gPrefs.putString(K_ADMIN, s.admin_hash) == strlen(s.admin_hash);
  ok &= gPrefs.putString(K_HOST, s.hostname) == strlen(s.hostname);
  ok &= gPrefs.putBool(K_DONE, s.setup_done) > 0;
  xSemaphoreGive(gNvsLock);
  if (!ok) logf("NVS", "settings NOT fully saved");
  return ok;
}

// ---------------------------------------------------------------- dev seed
// Once per device (flag in NVS_DEV_NS, which a factory reset keeps): if secrets.h has the seed defines and NVS has
// no settings, seed them, so a developer's board comes up in run mode without the phone walkthrough. Values are
// never logged. The admin password comes from ADMIN_PASS, or from the legacy ADMIN_TOKEN.
void devSeed() {
#if HAVE_SECRETS
  Preferences dev;
  if (!dev.begin(NVS_DEV_NS, false)) return;
  const bool seeded = dev.getBool("seeded", false);
  xSemaphoreTake(gNvsLock, portMAX_DELAY);
  const bool haveSettings = gPrefs.isKey(K_SSID) || gPrefs.isKey(K_DONE);
  xSemaphoreGive(gNvsLock);
  if (seeded || haveSettings) {
    dev.end();
    return;
  }
  Settings s = defaultSettings();
  int n = 0;
#ifdef STA_SSID
  snprintf(s.sta_ssid, sizeof s.sta_ssid, "%s", STA_SSID);
  n++;
#endif
#ifdef STA_PASS
  snprintf(s.sta_pass, sizeof s.sta_pass, "%s", STA_PASS);
#endif
#ifdef MQTT_HOST
  snprintf(s.mqtt_host, sizeof s.mqtt_host, "%s", MQTT_HOST);
  n++;
#endif
#ifdef MQTT_PORT
  s.mqtt_port = MQTT_PORT;
#endif
#ifdef MQTT_USER
  snprintf(s.mqtt_user, sizeof s.mqtt_user, "%s", MQTT_USER);
#endif
#ifdef MQTT_PASS
  snprintf(s.mqtt_pass, sizeof s.mqtt_pass, "%s", MQTT_PASS);
#endif
#ifdef AP_PASS
  snprintf(s.ap_pass, sizeof s.ap_pass, "%s", AP_PASS);
  n++;
#endif
  const char* admin = nullptr;
#if defined(ADMIN_PASS)
  admin = ADMIN_PASS;
#elif defined(ADMIN_TOKEN)
  admin = ADMIN_TOKEN;
#endif
  if (admin && validAdminPass(admin)) {
    uint8_t salt[SALT_LEN];
    esp_fill_random(salt, sizeof salt);
    if (hashPassword(admin, salt, PBKDF2_ITER, s.admin_hash, sizeof s.admin_hash)) n++;
  }
  s.setup_done = settingsComplete(s);
  gSettings = s;
  saveSettings();
  dev.putBool("seeded", true);
  dev.end();
  logf("SEED", "dev settings seeded once from secrets.h (%d groups; values not logged) -> %s", n,
       s.setup_done ? "run mode" : "setup mode (incomplete)");
#endif
}

// ---------------------------------------------------------------- NVS: bridge + stations
static void nvsKey(const char* id, char out[16]) {
  if (strlen(id) <= 12) {
    snprintf(out, 16, "st:%.12s", id);
    return;
  }
  uint32_t h = 2166136261u;
  for (const char* p = id; *p; p++) h = (h ^ (uint8_t)*p) * 16777619u;
  snprintf(out, 16, "st#%08lx", (unsigned long)h);
}

struct StrBuf {
  char* buf;
  size_t cap, len;
};
static bool toStrBuf(void* ctx, const char* d, size_t n) {
  StrBuf* b = (StrBuf*)ctx;
  if (b->len + n >= b->cap) return false;
  memcpy(b->buf + b->len, d, n);
  b->len += n;
  b->buf[b->len] = 0;
  return true;
}

void applyTz() {
  // Our own evaluator decides day boundaries; libc gets the same string so anything using localtime() agrees.
  setenv("TZ", gBridge.tz, 1);
  tzset();
}

// The stored bridge settings, in both modes: setup mode starts from them too, so a setup again (data kept) does
// not reset lan_uploads, max_stations, auto_publish and the last-seen interval. After a factory reset NVS is empty:
// defaults.
void loadBridge() {
  gBridge = defaultBridgeConfig();
  xSemaphoreTake(gNvsLock, portMAX_DELAY);
  String b = gPrefs.getString("bridge", "");
  xSemaphoreGive(gNvsLock);
  if (b.length()) {
    Errors e;
    bridgeFromJson(b.c_str(), b.length(), &gBridge, &e);
    logf("NVS", "bridge config loaded (%u B, %d errors)", (unsigned)b.length(), e.n);
    for (int i = 0; i < e.n; i++)  // a stored value above the profile's is refused, the default stays
      if (strncmp(e.at(i), "max_stations", 12) == 0)
        logf("CFG", "stored max_stations above this board's %d: %u used", MAX_STATIONS, (unsigned)gBridge.max_stations);
  }
  if (!parsePosixTz(gBridge.tz, &gTz)) {
    logf("CFG", "TZ \"%s\" not understood, using %s", gBridge.tz, DEFAULT_TZ);
    strcpy(gBridge.tz, DEFAULT_TZ);
    parsePosixTz(DEFAULT_TZ, &gTz);
  }
  applyTz();
}

static int blockedCount();  // defined with gBlocked below
void loadDataConfig() {
  loadBridge();

  // every slot max_stations allows, now, before Wi-Fi and MQTT allocate (one stretch, no fragmentation)
  const int slots = gStations.reserve(gBridge.max_stations);
  const HeapNow hn = heapNow();
  logl(Log::Boot, "HEAP", "%d of max_stations %u station slot(s) reserved (%u B each), free8 %u, largest8 %u", slots,
       (unsigned)gBridge.max_stations, (unsigned)sizeof(Station), (unsigned)hn.free8, (unsigned)hn.largest8);
  int loaded = 0;
  int staleSaves[MAX_STATIONS], nStaleSaves = 0;  // stations whose stored config lost a stale promotion
  // The ring owner with the newest hour key first, the other ring owner next, ringless
  // ids last, so a board with more stored stations than slots keeps the console that uploaded last
  char own[RING_SLOTS][STATION_ID_LEN];
  uint32_t newest[RING_SLOTS] = {}, ymd;
  for (int k = 0; k < RING_SLOTS; k++) {
    storeRingOwner(k, own[k]);
    if (own[k][0]) storeNewestKeys(own[k], &newest[k], &ymd);
  }
  constexpr int K1 = RING_SLOTS > 1 ? 1 : 0;  // one ring on the WROOM-32 profile -> no second owner ("")
  const LoadOrder order = loadOrder(own[0], newest[0], K1 ? own[K1] : "", K1 ? newest[K1] : 0);
  for (int pass = 0; pass < 3; pass++) {
    nvs_iterator_t it = nullptr;
    esp_err_t rc = nvs_entry_find(NVS_DEFAULT_PART_NAME, NVS_NS, NVS_TYPE_STR, &it);
    while (rc == ESP_OK) {
      nvs_entry_info_t info;
      nvs_entry_info(it, &info);
      if (strncmp(info.key, "st:", 3) == 0 || strncmp(info.key, "st#", 3) == 0) {
        xSemaphoreTake(gNvsLock, portMAX_DELAY);
        String v = gPrefs.getString(info.key, "");
        xSemaphoreGive(gNvsLock);
        int nl = v.indexOf('\n');
        if (nl > 0 && nl < STATION_ID_LEN) {
          char id[STATION_ID_LEN];
          memcpy(id, v.c_str(), nl);
          id[nl] = 0;
          if (loadPass(id, order) != pass) {
            rc = nvs_entry_next(&it);
            continue;
          }
          Errors e;
          StationConfig cfg = mergeStation(v.c_str() + nl + 1, &e);
          Station* st = gStations.findOrAdd(id, cfg);  // allocates the slot (slots reserved at boot)
          if (st) {
            st->stored = true;
            st->key_bound = loadStationKey(id, st->key_hash);  // the console's key, bound before
            static ChMap maps[CH_VERSIONS];  // the current channel map (the newest version); loop task, not its stack
            if (const int nm = loadChannelMaps(id, maps)) st->chmap = maps[nm - 1];
            loadCalLog(*st);  // its calibration log
            xSemaphoreTake(gLock, portMAX_DELAY);  // storeRestore expects it (it releases it around flash I/O)
            storeRestore(*st);  // its days come back from flash before the first upload
            xSemaphoreGive(gLock);
            loaded++;
            // the errors by name, not just a count
            char why[200] = "";
            for (int i = 0, k = 0; i < e.n && k < (int)sizeof why - 1; i++)
              k += snprintf(why + k, sizeof why - k, "%s%s", i ? "; " : ": ", e.at(i));
            logf("NVS", "station %s config loaded (%u B, %d error(s)%s)", id, (unsigned)v.length(), e.n, why);
            if (e.n_stale) {  // stale promotions: out of NVS now, out of HA on the first MQTT connect
              for (int i = 0; i < e.n_stale; i++) queuePromoRetract(id, e.stale_keys[i]);
              staleSaves[nStaleSaves++ % MAX_STATIONS] = gStations.indexOf(st);
            }
          } else {
            // A board that stored more stations than this profile keeps: the others wait, nothing is deleted
            logf("NVS", "station %s parked: this board keeps %d station(s); its stored data stays (Forget in Settings)", id,
                 MAX_STATIONS);
            gParked.add(id);  // listed in admin (rings "parked") so it can be forgotten
          }
        }
      }
      rc = nvs_entry_next(&it);
    }
    nvs_release_iterator(it);
  }
  if (gParked.dropped)  // listed after the first ones are forgotten and the bridge restarts
    logf("NVS", "%u more station config(s) without a slot not listed in admin (max %d)", (unsigned)gParked.dropped, ParkedList::MAX);
  for (int i = 0; i < nStaleSaves && i < MAX_STATIONS; i++) saveStation(staleSaves[i]);  // cleaned configs
  // The one NVS + CFG summary of the boot (the per-item load lines are tail events)
  logl(Log::Boot, "CFG", "TZ %s (%s), auto_publish %s, last_seen %lu s; %d station config(s) from NVS, %u parked, %d blocked",
       gBridge.tz, gBridge.tz_name[0] ? gBridge.tz_name : "no name", gBridge.auto_publish_new_stations ? "on" : "off",
       (unsigned long)gBridge.mqtt_interval_last_seen_s, loaded, (unsigned)(gParked.n + gParked.dropped), blockedCount());
}

bool saveBridge() {
  static char val[384];
  xSemaphoreTake(gLock, portMAX_DELAY);
  StrBuf sb{val, sizeof val, 0};
  val[0] = 0;
  char tmp[64];
  Out o(toStrBuf, &sb, tmp, sizeof tmp);
  bridgeToJson(o, gBridge);
  const bool fits = o.flush();
  xSemaphoreGive(gLock);
  if (!fits) return false;
  xSemaphoreTake(gNvsLock, portMAX_DELAY);
  const bool ok = gPrefs.putString("bridge", val) > 0;
  xSemaphoreGive(gNvsLock);
  return ok;
}

bool saveStation(int slot) {
  static char val[3800];
  struct Hold {
    Hold() { xSemaphoreTake(gSaveBufLock, portMAX_DELAY); }
    ~Hold() { xSemaphoreGive(gSaveBufLock); }
  } hold;
  char key[16];
  xSemaphoreTake(gLock, portMAX_DELAY);
  Station* st = gStations.at(slot);
  bool fits = false;
  if (st) {
    nvsKey(st->id, key);
    int n = snprintf(val, sizeof val, "%s\n", st->id);
    StrBuf b{val, sizeof val, (size_t)n};
    char tmp[128];
    Out o(toStrBuf, &b, tmp, sizeof tmp);
    stationToJson(o, st->cfg);
    fits = o.flush();
  }
  uint8_t keyHash[32];
  char sid[STATION_ID_LEN] = "";
  if (st) memcpy(sid, st->id, sizeof sid);
  const bool keyBound = st && st->key_bound;
  if (keyBound) memcpy(keyHash, st->key_hash, sizeof keyHash);
  xSemaphoreGive(gLock);
  if (!fits) return false;
  if (keyBound) saveStationKey(sid, keyHash);  // a station stored now keeps its key across restarts
  xSemaphoreTake(gNvsLock, portMAX_DELAY);
  const bool ok = gPrefs.putString(key, val) > 0;
  xSemaphoreGive(gNvsLock);
  if (!ok) return false;
  // "stored" only after the write worked, on the same station: the slot may have been forgotten meanwhile
  xSemaphoreTake(gLock, portMAX_DELAY);
  Station* now = gStations.at(slot);
  if (now && strcmp(now->id, sid) == 0) now->stored = true;
  xSemaphoreGive(gLock);
  return true;
}

void deleteStationConfig(const char* id) {
  char key[16];
  nvsKey(id, key);
  xSemaphoreTake(gNvsLock, portMAX_DELAY);
  if (gPrefs.isKey(key)) gPrefs.remove(key);
  xSemaphoreGive(gNvsLock);
  saveStationKey(id, nullptr);
  deleteChannelMaps(id);
  deleteCalLog(id);  // the calibration log
  learnDelete(id);   // the learners' blobs
}

// "ch:<id>" / "ch#<hash>": the channel map versions as one blob, oldest first, <= CH_VERSIONS
static void nvsChKey(const char* id, char out[16]) {
  nvsKey(id, out);
  out[0] = 'c';
  out[1] = 'h';
}

int loadChannelMaps(const char* id, ChMap out[CH_VERSIONS]) {
  char k[16];
  nvsChKey(id, k);
  xSemaphoreTake(gNvsLock, portMAX_DELAY);
  const size_t len = gPrefs.isKey(k) ? gPrefs.getBytesLength(k) : 0;
  const bool ok = len && len % sizeof(ChMap) == 0 && len <= CH_VERSIONS * sizeof(ChMap) &&
                  gPrefs.getBytes(k, out, len) == len;
  xSemaphoreGive(gNvsLock);
  if (!ok) return 0;
  const int n = (int)(len / sizeof(ChMap));
  for (int i = 0; i < n; i++)
    for (auto& key : out[i].key) key[CH_KEY_LEN - 1] = 0;
  return n;
}

void saveChannelMap(const char* id, const ChMap& m) {
  static ChMap maps[CH_VERSIONS + 1];  // loop task only
  int n = loadChannelMaps(id, maps);
  if (n && maps[n - 1].ver == m.ver) n--;  // the same version again: replaced
  maps[n++] = m;
  const int from = n > CH_VERSIONS ? n - CH_VERSIONS : 0;  // the oldest drops
  char k[16];
  nvsChKey(id, k);
  xSemaphoreTake(gNvsLock, portMAX_DELAY);
  gPrefs.putBytes(k, maps + from, (size_t)(n - from) * sizeof(ChMap));
  xSemaphoreGive(gNvsLock);
}

void deleteChannelMaps(const char* id) {
  char k[16];
  nvsChKey(id, k);
  xSemaphoreTake(gNvsLock, portMAX_DELAY);
  if (gPrefs.isKey(k)) gPrefs.remove(k);
  xSemaphoreGive(gNvsLock);
}

// "cl:<id>" / "cl#<hash>" -- the calibration log, the entries only (oldest first, n = length / 16)
static void nvsClKey(const char* id, char out[16]) {
  nvsKey(id, out);
  out[0] = 'c';
  out[1] = 'l';
}

void saveCalLog(const char* id, const CalLog& log) {
  char k[16];
  nvsClKey(id, k);
  xSemaphoreTake(gNvsLock, portMAX_DELAY);
  gPrefs.putBytes(k, log.e, (size_t)log.n * sizeof(CalEntry));
  xSemaphoreGive(gNvsLock);
}

void deleteCalLog(const char* id) {
  char k[16];
  nvsClKey(id, k);
  xSemaphoreTake(gNvsLock, portMAX_DELAY);
  if (gPrefs.isKey(k)) gPrefs.remove(k);
  xSemaphoreGive(gNvsLock);
}

// Learners: "lr:<id>" / "lm:<id>" (the nvsKey rule, another prefix), raw blobs under the NVS lock
void nvsLearnKey(const char* id, char kind, char out[16]) {
  nvsKey(id, out);
  out[0] = 'l';
  out[1] = kind;
}
size_t nvsGetBlob(const char* k, void* p, size_t cap) {
  xSemaphoreTake(gNvsLock, portMAX_DELAY);
  size_t len = gPrefs.isKey(k) ? gPrefs.getBytesLength(k) : 0;
  if (len && len <= cap && gPrefs.getBytes(k, p, len) != len) len = 0;
  xSemaphoreGive(gNvsLock);
  return len;  // > cap: not read (the caller judges the length)
}
bool nvsPutBlob(const char* k, const void* p, size_t len) {
  xSemaphoreTake(gNvsLock, portMAX_DELAY);
  const bool ok = gPrefs.putBytes(k, p, len) == len;
  xSemaphoreGive(gNvsLock);
  return ok;
}
void nvsDelKey(const char* k) {
  xSemaphoreTake(gNvsLock, portMAX_DELAY);
  if (gPrefs.isKey(k)) gPrefs.remove(k);
  xSemaphoreGive(gNvsLock);
}
bool nvsStats(uint32_t* used, uint32_t* free_entries) {
  nvs_stats_t st;
  if (nvs_get_stats(nullptr, &st) != ESP_OK) return false;
  *used = st.used_entries, *free_entries = st.free_entries;
  return true;
}

bool readCalLog(const char* id, CalLog* log) {
  char k[16];
  nvsClKey(id, k);
  *log = CalLog{};
  xSemaphoreTake(gNvsLock, portMAX_DELAY);
  const size_t len = gPrefs.isKey(k) ? gPrefs.getBytesLength(k) : 0;
  const bool ok = len && len % sizeof(CalEntry) == 0 && len <= sizeof log->e && gPrefs.getBytes(k, log->e, len) == len;
  xSemaphoreGive(gNvsLock);
  log->n = ok ? (uint8_t)(len / sizeof(CalEntry)) : 0;
  return log->n > 0;
}

// The station's log from NVS; none yet (a new station, or none stored so far) -> {0, its current calibration}, saved
void loadCalLog(Station& st) {
  CalLog log;
  if (!readCalLog(st.id, &log)) {
    calAppend(&log, calOf(st.cfg, 0));
    saveCalLog(st.id, log);
    logf("HIST", "%s: calibration log started", st.id);
  }
  st.cal = log;
}

// "sk:<id>" / "sk#<hash>" next to the config's "st:" key: the bound key's hash, 32 bytes
static void nvsKeyKey(const char* id, char out[16]) {
  nvsKey(id, out);
  out[1] = 'k';
}

void saveStationKey(const char* id, const uint8_t* key) {
  char k[16];
  nvsKeyKey(id, k);
  xSemaphoreTake(gNvsLock, portMAX_DELAY);
  if (key) gPrefs.putBytes(k, key, 32);
  else if (gPrefs.isKey(k)) gPrefs.remove(k);
  xSemaphoreGive(gNvsLock);
}

bool loadStationKey(const char* id, uint8_t key[32]) {
  char k[16];
  nvsKeyKey(id, k);
  xSemaphoreTake(gNvsLock, portMAX_DELAY);
  const bool ok = gPrefs.isKey(k) && gPrefs.getBytes(k, key, 32) == 32;
  xSemaphoreGive(gNvsLock);
  return ok;
}

// ---------------------------------------------------------------- adopt: the journal and the NVS move (adopt.h)
namespace {
class PrefsKv : public AdoptKv {  // the app's NVS namespace through Preferences, under the NVS lock (boot: one task)
 public:
  Type type(const char* k) override {
    xSemaphoreTake(gNvsLock, portMAX_DELAY);
    const PreferenceType t = gPrefs.isKey(k) ? gPrefs.getType(k) : PT_INVALID;
    xSemaphoreGive(gNvsLock);
    return t == PT_STR ? Type::Str : t == PT_BLOB ? Type::Blob : Type::None;
  }
  long size(const char* k) override {
    xSemaphoreTake(gNvsLock, portMAX_DELAY);
    long n = -1;
    if (gPrefs.isKey(k)) {
      if (gPrefs.getType(k) == PT_STR) {  // the length from NVS itself: a 3.8-KB value is never copied onto the heap here
        nvs_handle_t h;
        size_t len = 0;
        if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
          if (nvs_get_str(h, k, nullptr, &len) == ESP_OK && len) n = (long)len - 1;  // len counts the NUL
          nvs_close(h);
        }
      } else n = (long)gPrefs.getBytesLength(k);
    }
    xSemaphoreGive(gNvsLock);
    return n;
  }
  long get(const char* k, uint8_t* buf, size_t cap) override {
    xSemaphoreTake(gNvsLock, portMAX_DELAY);
    long n = -1;
    if (gPrefs.isKey(k)) {
      if (gPrefs.getType(k) == PT_STR) {
        const size_t r = gPrefs.getString(k, (char*)buf, cap);  // with its NUL
        n = r ? (long)strnlen((const char*)buf, cap) : -1;
      } else {
        const size_t len = gPrefs.getBytesLength(k);
        n = len <= cap && gPrefs.getBytes(k, buf, len) == len ? (long)len : -1;
      }
    }
    xSemaphoreGive(gNvsLock);
    return n;
  }
  bool put(const char* k, Type t, const uint8_t* d, size_t n) override {
    xSemaphoreTake(gNvsLock, portMAX_DELAY);
    bool ok;
    if (t == Type::Str) {
      String v;
      v.concat((const char*)d, n);
      ok = gPrefs.putString(k, v) == n;
    } else {
      ok = gPrefs.putBytes(k, d, n) == n;
    }
    xSemaphoreGive(gNvsLock);
    return ok;
  }
  void del(const char* k) override { nvsDelKey(k); }
};
}  // namespace

// This boot's decision on the journal (adoptBootBegin): the move's steps read the journal only when it said Run; a
// given-up journal's ids stay for the forwarding step's line (the targets it leaves)
static bool gAdoptRun = false, gAdoptGaveUp = false;
static char gAdoptGaveFrom[STATION_ID_LEN], gAdoptGaveTo[STATION_ID_LEN];

// Boot, before the first step of the move (adopt.h adoptJournalBoot): this boot counted in the journal, or the journal
// given up after ADOPT_MAX_BOOTS boots that did not finish it, or no move this boot when the count cannot be saved
void adoptBootBegin() {
  PrefsKv kv;
  AdoptJournal j;
  const AdoptBoot b = adoptJournalBoot(kv, &j);
  gAdoptRun = b == AdoptBoot::Run;
  if (b == AdoptBoot::GiveUp) {
    gAdoptGaveUp = true;
    snprintf(gAdoptGaveFrom, sizeof gAdoptGaveFrom, "%s", j.from);
    snprintf(gAdoptGaveTo, sizeof gAdoptGaveTo, "%s", j.to);
  } else if (b == AdoptBoot::Skip) {
    logf("ADMIN", "adopt %s -> %s: the boot count could not be saved; the move waits for the next boot", j.from, j.to);
  } else if (b == AdoptBoot::Run && j.boots > 1) {
    logf("ADMIN", "adopt %s -> %s: repeated, boot %u of %u", j.from, j.to, (unsigned)j.boots, (unsigned)ADOPT_MAX_BOOTS);
  }
  memset(&j, 0, sizeof j);
}
bool adoptGaveUp(char from[STATION_ID_LEN], char to[STATION_ID_LEN]) {
  if (!gAdoptGaveUp) return false;
  memcpy(from, gAdoptGaveFrom, STATION_ID_LEN);
  memcpy(to, gAdoptGaveTo, STATION_ID_LEN);
  return true;
}

bool adoptJournalRead(AdoptJournal* j) {
  if (!gAdoptRun) return false;  // none, given up, or not counted this boot
  uint8_t raw[sizeof(AdoptJournal)];
  const size_t n = nvsGetBlob(ADOPT_JOURNAL_KEY, raw, sizeof raw);
  const bool ok = n <= sizeof raw && adoptJournalDecode(raw, n, j);
  memset(raw, 0, sizeof raw);
  return ok;
}
void adoptJournalRemove() { nvsDelKey(ADOPT_JOURNAL_KEY); }

bool stationKeysStored(const char* id) {
  PrefsKv kv;
  return adoptKeysExist(kv, id);
}

// The admin's confirmation (httpd task): the room the move needs, then the journal; the planned restart does the rest
bool adoptWriteJournal(const char* from, const char* to, const uint8_t key[32], const char** why) {
  PrefsKv kv;
  switch (adoptPrecheck(kv, from, to)) {  // copy-if-absent is exact only on a clean target
    case AdoptBlock::ToExists: {
      *why = "the new id already has saved settings; forget it first";
      // the way out: listed under "Saved settings without a slot" with Forget (gLeftover, RAM only: not parked, so admit,
      // restart and ghost rules are untouched) -- nothing erased here
      xSemaphoreTake(gLock, portMAX_DELAY);
      if (!gStations.find(to)) gLeftover.add(to);
      xSemaphoreGive(gLock);
      return false;
    }
    case AdoptBlock::KeyCollision:
      *why = "the two ids share a settings key";
      return false;
    case AdoptBlock::None:
      break;
  }
  nvs_stats_t st;
  const uint32_t need = adoptNeedEntries(kv, from);  // from the old keys' real lengths
  if (nvs_get_stats(nullptr, &st) != ESP_OK || st.available_entries < need) {
    *why = "not enough room in settings";
    logf("ADMIN", "adopt %s -> %s refused: %lu free settings entries, the move needs %lu", from, to,
         (unsigned long)st.available_entries, (unsigned long)need);
    return false;
  }
  AdoptJournal j = {};
  snprintf(j.from, sizeof j.from, "%s", from);
  snprintf(j.to, sizeof j.to, "%s", to);
  memcpy(j.key, key, sizeof j.key);
  j.ver = ADOPT_JOURNAL_VER;  // boots 0: the next boot counts itself before the move
  const bool ok = adoptJournalValid(j) && nvsPutBlob(ADOPT_JOURNAL_KEY, &j, sizeof j);
  memset(&j, 0, sizeof j);
  if (!ok) *why = "the settings could not be written";
  return ok;
}

// Boot, before the stations load (setup): a journal found -> the per-station NVS entries move. The buffer (the config
// text is the largest value, <= 3 800 B) comes from the heap for this one call.
bool adoptBootNvs() {
  AdoptJournal j;
  if (!adoptJournalRead(&j)) return true;
  constexpr size_t CAP = 4096;
  uint8_t* buf = (uint8_t*)malloc(CAP);
  PrefsKv kv;
  const bool ok = buf && adoptNvs(kv, j, buf, CAP);
  if (buf) memset(buf, 0, CAP), free(buf);
  logf("ADMIN", "adopt %s -> %s: settings, key, channel maps, calibration and learners %s", j.from, j.to,
       ok ? "moved" : "NOT moved (kept for the next boot)");
  memset(j.key, 0, sizeof j.key);
  return ok;
}

// ---------------------------------------------------------------- block list (NVS fc_blk)
static AnnList gBlocked;  // same shape as the announced-id list: <= 8 ids; a 9th is refused, never evicts one
static int blockedCount() { return gBlocked.size(); }  // the boot summary line (declared above loadDataConfig)
static void blockedSave() {
  char s[AnnList::MAX * ANN_ID_LEN + 8];
  if (gBlocked.format(s, sizeof s) < 0) return;
  Preferences p;
  if (!p.begin("fc_blk", false)) return;
  p.putString("ids", s);
  p.end();
}
void blockedLoad() {
  Preferences p;
  if (!p.begin("fc_blk", true)) return;
  char s[AnnList::MAX * ANN_ID_LEN + 8] = "";
  p.getString("ids", s, sizeof s);
  p.end();
  gBlocked.parse(s);
  // the blocked count is on the boot's CFG summary line
}
void blockedWipe() {
  Preferences p;
  if (p.begin("fc_blk", false)) {
    p.clear();
    p.end();
  }
  gBlocked = AnnList();
}
bool blockedHas(const char* id) { return gBlocked.has(id); }
bool blockedAdd(const char* id) {
  if (!gBlocked.addIfRoom(id)) return gBlocked.has(id);  // full: refused (the caller answers "not saved")
  blockedSave();
  return true;
}
bool blockedRemove(const char* id) {
  if (!gBlocked.remove(id)) return false;
  blockedSave();
  return true;
}
int blockedList(char ids[][STATION_ID_LEN], int max) {
  int n = 0;
  for (; n < gBlocked.size() && n < max; n++) snprintf(ids[n], STATION_ID_LEN, "%s", gBlocked.at(n));
  return n;
}

void factoryReset() {
  logf("RESET", "factory reset: wiping NVS namespace %s, restarting into setup mode", NVS_NS);
  xSemaphoreTake(gNvsLock, portMAX_DELAY);
  gPrefs.clear();
  gPrefs.end();
  xSemaphoreGive(gNvsLock);
  // out of the box means the data too: history files and snapshots
  logf("RESET", "factory reset: LittleFS format %s", storeFormat() ? "done" : "FAILED");
  sysWipe();  // reset reason, restart counter
  clockForget();  // and t_last is not written back from a carried clock
  annWipe();  // the ids announced to HA
  blockedWipe();  // the block list
  // The crash data is a RAM image (passwords included), and the Wi-Fi driver keeps its own copy of the
  // last STA/AP configuration in NVS (nvs.net80211): both go, or the next owner could read them back
  logf("RESET", "factory reset: crash data %s", crashErase() ? "erased" : "NOT erased");
  logf("RESET", "factory reset: Wi-Fi driver config %s", esp_wifi_restore() == ESP_OK ? "restored to defaults" : "NOT restored");
  delay(200);
  ESP.restart();
}

// ---------------------------------------------------------------- crypto self-test (same vector as the host test)
bool cryptoSelfTest(uint32_t* ms) {
  uint8_t salt[16], out[32];
  for (int i = 0; i < 16; i++) salt[i] = (uint8_t)i;
  const char* pw = "correct horse battery staple \xc5\xbc";  // neutral, with a non-ASCII byte (UTF-8)
  const uint32_t t0 = millis();
  const bool ok = pbkdf2Sha256((const uint8_t*)pw, strlen(pw), salt, 16, PBKDF2_ITER, out);
  *ms = millis() - t0;
  char hex[65], proof[65], cred[HASH_STR_MAX];
  toHex(out, 32, hex);
  // and the login proof the browser sends for that H (same vector as the host test and the JS check)
  const bool proofOk = computeProof(out, "00112233445566778899aabbccddeeff", proof) &&
                       strcmp(proof, "6358e538940f03d3d087a6ab8dceb417a0e3a26746d6bd589bd457ba703926e5") == 0 &&
                       formatCredential(salt, PBKDF2_ITER, out, cred, sizeof cred) &&
                       verifyProof(cred, "00112233445566778899aabbccddeeff", proof);
  return ok && proofOk && strcmp(hex, "460b333275bd03de2073208552f336d871c8cf4ea173a9b5e61c0ef17e98998f") == 0;
}

// ---------------------------------------------------------------- BOOT button, pending actions
// BOOT button, status LED, pending reboot / factory reset. Called every loop() in both modes. Pins: board.h.

void coreLoop() {
  static HoldButton button;
  static uint32_t lastPoll = 0, resetAtMs = 0;
  static bool armed = false;  // only after the button was seen released: a hold that caused a reset and continues
                              // through the reboot must not start a second countdown
  static bool held = false, ledInit = false;
  static uint32_t heldMs = 0;
  static int8_t ledPrev = -1;
  const uint32_t ms = millis();
  if (!ledInit) {
    pinMode(BOARD.led_pin, OUTPUT);
    ledInit = true;
  }
  if (ms - lastPoll >= 20) {
    lastPoll = ms;
    const bool pressed = (digitalRead(BOARD.button_pin) == LOW) == BOARD.button_active_low;
    if (!armed) {
      armed = !pressed;
    } else if (!resetAtMs) {
      const int r = button.feed(ms, pressed);
      held = pressed;
      heldMs = button.heldMs(ms);
      if (r > 0)
        logf("RESET", "BOOT held %d s -- %s", r,
             r < 3 ? "release now for setup mode (keeps your data), or hold 5 s to erase everything" : "factory reset at 5 s");
      if (r == HoldButton::FIRED) resetAtMs = ms + 600;  // strobe for a moment: "triggered"
      if (r == HoldButton::SHORT && gMode == Mode::Run) {  // back into setup, settings and stations kept
        xSemaphoreTake(gLock, portMAX_DELAY);
        gSettings.setup_done = false;
        xSemaphoreGive(gLock);
        const bool saved = saveSettings();
        logf("RESET", "BOOT short press -> setup mode (%s; stations, settings and history kept)", saved ? "saved" : "NOT saved");
        sysPlanRestart("setup_again", 500);
      }
    }
    // Status LED: one pattern at a time (src/led.h).
    bool lockActive = false;
    uint32_t lockS = 0;
    int lockFails = 0;
    if (gMode == Mode::Run) adminLockInfo(&lockActive, &lockS, &lockFails);
    LedInputs in;
    in.reset_commit = resetAtMs != 0 || gFactoryResetPending;
    in.button_held = held;
    in.setup_mode = gMode == Mode::Setup;
    in.sta_up = staUp();
    in.attention = (gSettings.mqtt_host[0] && !mqttUp()) || lockActive || heapLow(nullptr);
    in.uptime_ms = ms;
    in.ota = otaActive();
    in.held_ms = heldMs;
    const int8_t want = ledOn(ledState(in), ms, in.sta_up, heldMs) ? 1 : 0;
    if (want != ledPrev) {  // write on edges only
      ledPrev = want;
      digitalWrite(BOARD.led_pin, (want != 0) != BOARD.led_active_low ? HIGH : LOW);
    }
  }
  if (resetAtMs && (int32_t)(ms - resetAtMs) >= 0) factoryReset();
  if (gFactoryResetPending) {
    if (!resetAtMs) resetAtMs = ms + 600;  // the admin-requested reset strobes too
  }
  if (gRebootAtMs && (int32_t)(ms - gRebootAtMs) >= 0) {
    if (gMode == Mode::Run) storeFlushAll(), learnFlushAll();  // the open hour's last minutes are not lost; the learner blobs too
    clockPersistNow();                        // the next boot starts from this time
    logf("BOOT", "restarting");
    delay(100);
    ESP.restart();
  }
}

}  // namespace app
