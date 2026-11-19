// app_sys.cpp -- system health: the image marker, reset reason + restart counter (NVS fc_sys),
// crash data (coredump in flash), the rollback health check, the Task WDT on the loop, the network watchdog and the
// debug hang. See app.h. Pure decisions live in src/robust.{h,cpp}.
#include <Preferences.h>
#include <WiFi.h>
#include <esp_core_dump.h>
#include <esp_flash.h>
#include <esp_ota_ops.h>
#include <esp_task_wdt.h>
#include <esp32/rom/rtc.h>
#include <esp_attr.h>
#include <esp_sntp.h>
#include <sys/time.h>
#include <time.h>

#include "app.h"
#include "src/bootlog.h"
#include "src/robust.h"
#include "src/web_render.h"

// The marker an OTA upload must contain (src/robust.h). Referenced at runtime, so the linker keeps it.
extern "C" const char FC_IMAGE_MARKER[] __attribute__((used)) = "FAUXCLOUD-IMAGE:" FC_VERSION " " __DATE__ " " __TIME__;

// The core marks a new image valid at startup unless this returns true; the health check below does it.
extern "C" bool verifyRollbackLater() { return true; }

namespace app {

static const char* SYS_NS = "fc_sys";  // reset reason, restart counter, planned reason; wiped by a factory reset
static uint32_t gFragRestarts = 0;  // read at boot (sysBoot), +1 before a heap_frag restart
static char gLastReset[24] = "unknown";
static char gResetRaw[16] = "";  // "esp/rtc0/rtc1": what the chip really said (cold plug-in vs EN pulse)
static uint32_t gRestarts = 0;
static bool gWdtBoot = false;
static OtaState gOtaState = OtaState::Stable;
static CrashInfo gCrash = {};
static std::atomic<bool> gHttpServed{false};
static NetWatchdog gNetWd;
static volatile bool gHangReq = false;
// Every RTC_NOINIT word of the sketch in ONE struct, so the linker cannot reorder them across images
// (separate variables are placed by object file and in reverse declaration order: a new word in an earlier TU moves
// the others, and the image after an OTA reads the clock word at a never-written address). The layout is fixed
// (cut 0x200, clk 0x208, magic 0x210) and APPEND-ONLY: a new word goes at the end; tools/rtc_layout.txt
// and the build's RTC check (tools/check_rtc_layout.py) fail the build when an existing word moves.
struct RtcWords {
  uint32_t cut[2];         // 24-h clock acceptances that cut history {magic, count}
  uint32_t clk[2];         // who set the clock {magic, TimeSrc}, carried with the clock across a restart
  uint32_t magic;          // the boot magic (src/robust.h effectiveResetReason)
  OtaHeapRec ota_heap;     // the byte heap the last OTA write had
  uint32_t boot_no[2];     // this boot's restart number {RTC_BOOTNO_MAGIC, n}, written once the NVS counter is read; the
                           // next boot copies it into gPrevTail (the first 8 B of the old zero reserve)
  uint8_t reserved[20];    // zero, for later cross-OTA words (a new word takes its bytes; the struct stays 64 B)
};
static_assert(offsetof(RtcWords, cut) == 0 && offsetof(RtcWords, clk) == 8 && offsetof(RtcWords, magic) == 16 &&
                  offsetof(RtcWords, ota_heap) == 20 && offsetof(RtcWords, boot_no) == 36 &&
                  offsetof(RtcWords, reserved) == 44 && sizeof(RtcWords) == 64,
              "the RTC layout is append-only (tools/rtc_layout.txt)");
constexpr uint32_t RTC_BOOTNO_MAGIC = 0x4F4E5442;  // "BTNO"
// The boot log (src/bootlog.h), 4.1 KB of RTC slow memory instead of DRAM; zeroed at every boot by
// bootlogBegin() (it does not carry across a restart: a fresh head per boot). Listed after gRtc in tools/rtc_layout.txt.
// Declared BEFORE gRtc on purpose: GCC emits a unit's variables in reverse order, so this keeps gRtc at the section
// start (the board's addresses) and the boot log after it. tools/check_rtc_layout.py (build.ps1, CI) guards it; if it
// ever fails with gRtc / gBootLog swapped (a toolchain or flag change), the fix is to swap these two declarations.
// The previous boot's tail, after gBootLog in .rtc_noinit (declared before it: reverse emission, as
// above; listed in tools/rtc_layout.txt). Filled by bootlogBegin() before the ring is reset; read by the download.
RTC_NOINIT_ATTR static fc::BootPrev gPrevTail;
RTC_NOINIT_ATTR static fc::BootLog gBootLog;
// gRtc after gBootLog in the source = first in .rtc_noinit (reverse emission; check_rtc_layout.py; fix = swap the two).
RTC_NOINIT_ATTR static RtcWords gRtc;
static portMUX_TYPE gLogMux = portMUX_INITIALIZER_UNLOCKED;  // put / freeze / thaw; never a logf inside it

void bootlogBegin() {
  // the previous boot's number, if it wrote one (an image before this word left the reserve zero); cleared for this boot
  const uint32_t prevNo = gRtc.boot_no[0] == RTC_BOOTNO_MAGIC ? gRtc.boot_no[1] : fc::BOOTPREV_RESTART_UNKNOWN;
  gRtc.boot_no[0] = gRtc.boot_no[1] = 0;
  memset(gRtc.reserved, 0, sizeof gRtc.reserved);
  // the previous boot's tail survives one restart -- not a power-on (RTC content undefined), and only a sane ring
  const esp_reset_reason_t r = esp_reset_reason();
  const bool keep = r != ESP_RST_POWERON && r != ESP_RST_UNKNOWN;
  fc::bootlogKeepPrev(gBootLog, gPrevTail, keep, keep ? prevNo : fc::BOOTPREV_RESTART_UNKNOWN);
  fc::bootlogReset(gBootLog);
}

// One logf line into the ring: a redacted copy (Serial keeps the original), formatted outside the lock.
// The small tasks that log (netout 4 KB, udp53_123 4 KB, the Wi-Fi event task) pay only this frame on
// top of logf's own: the SSIDs are matched where they live (read up to 32 B; a rewrite in flight can at worst miss one
// line's redaction, never print more), the MAC text is 18 B built without printf, and the line is formatted in msg itself.
void bootlogCapture(const char* tag, char* msg, size_t cap, bool truncated, bool boot) {
  // an SSID of 1-2 characters would match all over the text and names nobody: left as it is
  const char* ssids[2] = {gSettings.sta_ssid, netChangeSsid()};
  for (const char* id : ssids)
    if (id[0] && id[1] && id[2]) fc::bootlogRedact(msg, cap, id, "<home Wi-Fi>", false, 32);
  if (gMac[0] | gMac[1] | gMac[2] | gMac[3] | gMac[4] | gMac[5]) {
    static const char HEXD[] = "0123456789abcdef";  // not HEX: an Arduino macro
    char m[18];
    for (int i = 0; i < 6; i++) m[i * 3] = HEXD[gMac[i] >> 4], m[i * 3 + 1] = HEXD[gMac[i] & 15], m[i * 3 + 2] = ':';
    m[17] = 0;
    fc::bootlogRedact(msg, cap, m, "<mac>", true);  // aa:bb:..., any case
    for (int i = 0; i < 6; i++) m[i * 2] = HEXD[gMac[i] >> 4], m[i * 2 + 1] = HEXD[gMac[i] & 15];
    m[12] = 0;
    fc::bootlogRedact(msg, cap, m, "<mac>", true);  // the topic / unique_id form
  }
  const uint64_t ms = (uint64_t)(esp_timer_get_time() / 1000);
  const size_t n = fc::bootlogFormat(msg, cap, ms, tag, truncated);  // msg becomes the line (Serial already has it)
  portENTER_CRITICAL(&gLogMux);
  fc::bootlogPut(gBootLog, ms, tag, msg, n, boot);
  portEXIT_CRITICAL(&gLogMux);
}

// GET /api/admin/bootlog's body after the header line: frozen while it is sent (no copy), thawed on every path.
bool bootlogSend(Out& o) {
  portENTER_CRITICAL(&gLogMux);
  fc::bootlogFreeze(gBootLog);
  portEXIT_CRITICAL(&gLogMux);
  fc::bootlogRender(o, gBootLog);
  // the previous boot's section (nothing after a power-on); this boot's reset reason is what ended it
  fc::bootlogRenderPrev(o, gPrevTail, gLastReset, gRestarts ? gRestarts - 1 : 0);
  const bool ok = o.flush();
  portENTER_CRITICAL(&gLogMux);
  const uint32_t lost = fc::bootlogThaw(gBootLog);
  portEXIT_CRITICAL(&gLogMux);
  if (lost) logf("LOG", "%lu line(s) not kept while the log was sent", (unsigned long)lost);
  return ok;
}
static uint32_t& gRtcMagic = gRtc.magic;  // survives software resets, lost on a power cut (src/robust.h)
static uint32_t (&gRtcClk)[2] = gRtc.clk;
static uint32_t (&gRtcCut)[2] = gRtc.cut;
OtaHeapRec& rtcOtaHeap() { return gRtc.ota_heap; }

const char* buildText() { return FC_IMAGE_MARKER + 16; }  // "<version> <date> <time>"
const char* lastReset() { return gLastReset; }
const char* resetRaw() { return gResetRaw; }
uint32_t restarts() { return gRestarts; }
const char* otaStateText() { return otaStateName(gOtaState); }
bool otaPending() { return gOtaState == OtaState::Pending; }
const CrashInfo& lastCrash() { return gCrash; }
uint32_t staReinits() { return gNetWd.reinits(); }
void markHttpServed() { gHttpServed = true; }
void requestHang() { gHangReq = true; }

void sysBoot() {
  Preferences p;
  p.begin(SYS_NS, false);
  const int raw = (int)esp_reset_reason();
  const int reason = effectiveResetReason(raw, gRtcMagic == RTC_BOOT_MAGIC);
  gRtcMagic = RTC_BOOT_MAGIC;
  char planned[16] = "";
  p.getString("plan", planned, sizeof planned);
  snprintf(gLastReset, sizeof gLastReset, "%s", resetReasonName(reason, planned));
  gWdtBoot = resetIsWatchdog(reason);
  gRestarts = p.getUInt("restarts", 0) + 1;
  gRtc.boot_no[1] = gRestarts, gRtc.boot_no[0] = RTC_BOOTNO_MAGIC;  // the next boot's "previous boot" header names this number
  gFragRestarts = p.getUInt("fragrst", 0);
  p.putUInt("restarts", gRestarts);
  p.putString("last", gLastReset);
  if (planned[0]) p.remove("plan");
  gNetWd.setRebooted(p.getBool("outage", false));  // this outage already had its restart
  p.end();

  // rollback state of the running image
  const esp_partition_t* run = esp_ota_get_running_partition();
  esp_ota_img_states_t st;
  if (run && esp_ota_get_state_partition(run, &st) == ESP_OK && st == ESP_OTA_IMG_PENDING_VERIFY) gOtaState = OtaState::Pending;
  else if (esp_ota_get_last_invalid_partition()) gOtaState = OtaState::RolledBack;
  snprintf(gResetRaw, sizeof gResetRaw, "%d/%d/%d", raw, (int)rtc_get_reset_reason(0), (int)rtc_get_reset_reason(1));
  logl(Log::Boot, "SYS", "image %s, reset reason %s (esp_reset_reason %d, rtc cpu0 %d cpu1 %d), restart #%lu, OTA state %s",
       buildText(), gLastReset, raw, (int)rtc_get_reset_reason(0), (int)rtc_get_reset_reason(1),
       (unsigned long)gRestarts, otaStateText());

  // crash data from the previous run (kept until downloaded / cleared in admin)
  if (esp_core_dump_image_check() == ESP_OK) {
    gCrash.present = true;
    esp_core_dump_get_panic_reason(gCrash.reason, sizeof gCrash.reason);
    static esp_core_dump_summary_t sum;  // ~0.5 KB, once
    if (esp_core_dump_get_summary(&sum) == ESP_OK) {
      snprintf(gCrash.task, sizeof gCrash.task, "%s", sum.exc_task);
      gCrash.pc = sum.exc_pc;
    }
    logl(Log::Boot, "SYS", "crash data in flash: \"%s\" in task %s at 0x%08lx", gCrash.reason, gCrash.task, (unsigned long)gCrash.pc);
  }

  // the loop task under the Task WDT, 30 s (PBKDF2 and LittleFS writes run on the loop)
  esp_task_wdt_config_t cfg = {};
  cfg.timeout_ms = 30000;
  cfg.idle_core_mask = 1u << 0;  // as the core's sdkconfig: idle task of CPU0 watched
  cfg.trigger_panic = true;
  esp_task_wdt_reconfigure(&cfg);
  enableLoopWDT();  // the core's loopTask feeds it once per loop() pass
}

// A restart the firmware decides itself, with its reason for the next boot (resetReasonName: planned wins).
void sysPlanRestart(const char* why, uint32_t delay_ms) {
  Preferences p;
  p.begin(SYS_NS, false);
  p.putString("plan", why);
  p.end();
  scheduleReboot(delay_ms);
}


void sysCountFragRestart() {
  Preferences p;
  p.begin(SYS_NS, false);
  p.putUInt("fragrst", ++gFragRestarts);
  p.end();
}
uint32_t fragRestarts() { return gFragRestarts; }

// One-shot markers in fc_sys: wiped by a factory reset with the rest of fc_sys.
bool sysFlag(const char* key) {
  Preferences p;
  if (!p.begin(SYS_NS, true)) return false;
  const bool v = p.getBool(key, false);
  p.end();
  return v;
}
void sysSetFlag(const char* key) {
  Preferences p;
  p.begin(SYS_NS, false);
  p.putBool(key, true);
  p.end();
}

void sysWipe() {
  Preferences p;
  p.begin(SYS_NS, false);
  p.clear();
  p.end();
}

bool crashErase() {
  const bool ok = esp_core_dump_image_erase() == ESP_OK;
  if (ok) gCrash = {};
  return ok;
}

// Streams the raw dump (ELF) through `put`; false when there is none.
bool crashStream(bool (*put)(void* ctx, const uint8_t* data, size_t len), void* ctx, size_t* total) {
  size_t addr = 0, size = 0;
  if (esp_core_dump_image_get(&addr, &size) != ESP_OK || !size) return false;
  uint8_t* buf = (uint8_t*)httpScratch();  // the httpd task's 1 KB render buffer, idle during this
  const size_t bufLen = HTTP_SCRATCH_LEN;
  for (size_t off = 0; off < size; off += bufLen) {
    const size_t n = size - off < bufLen ? size - off : bufLen;
    if (esp_flash_read(esp_flash_default_chip, buf, addr + off, n) != ESP_OK || !put(ctx, buf, n)) return false;
  }
  *total = size;
  return true;
}

// loop(), once a second in run mode
void sysTick() {
  if (gOtaState == OtaState::Pending) {
    const RollbackInputs in = {true, staUp(), gHttpServed.load(), uptimeS(), gWdtBoot};
    if (shouldMarkValid(in) && esp_ota_mark_app_valid_cancel_rollback() == ESP_OK) {
      gOtaState = OtaState::Stable;
      logf("OTA", "new image healthy (Wi-Fi up, HTTP served, %lu s): rollback cancelled", (unsigned long)in.uptime_s);
      storeImageValid();  // a store waiting for this formats once now
    }
  }
  const bool paused = otaActive() || strcmp(wifiChangeStatus(), "busy") == 0;
  const bool wasRebooted = gNetWd.rebooted();
  const NetAction a = gNetWd.feed(uptimeS(), staUp(), paused);
  if (wasRebooted && !gNetWd.rebooted()) {  // joined again: the next outage may restart once more
    Preferences p;
    p.begin(SYS_NS, false);
    p.remove("outage");
    p.end();
  }
  switch (a) {
    case NetAction::Reinit:
      logf("NET", "home Wi-Fi lost: re-initialising the STA every 15 min (the AP keeps serving the console)");
      netReinit();
      break;
    case NetAction::Reboot: {
      logf("NET", "home Wi-Fi lost for 60 min: restarting once (then only re-inits until it is back)");
      Preferences p;
      p.begin(SYS_NS, false);
      p.putBool("outage", true);
      p.end();
      sysPlanRestart("sta_lost", 500);
      break;
    }
    default:
      break;
  }
}

// ---------------------------------------------------------------- the clock ladder (src/clock.h)
// gLadder is read by the UDP task (NTP answers) and the httpd task (seed, status), written by loop() and the seed:
// every access under gClkMux, a few words each. The system clock itself is libc's.
static portMUX_TYPE gClkMux = portMUX_INITIALIZER_UNLOCKED;
static ClockLadder gLadder;
static std::atomic<bool> gSntpEvent{false};
static std::atomic<uint32_t> gSntpBad{0};  // the unix time of an SNTP/DHCP answer below the floor (0 = none)
static std::atomic<bool> gPersistReq{false};  // a seed asks loop() to write t_last (one writer)
static TimeSrc gSrcBeforeSync = TimeSrc::None;  // under gClkMux: the source the SNTP callback replaced
static uint32_t gSeedT = 0, gSeedMs = 0;  // the last seed and when (loop(): "time corrected by N s")
// The latest seed accepted = the build date + 10 years (0 = __DATE__ did not parse: the floor + 10 years)
static uint32_t seedCeil() {
  static const uint32_t b = unixFromBuildDate(__DATE__);
  return (b ? b : TIME_FLOOR) + SEED_SPAN_S;
}
// What set the system clock, carried across a software restart with the clock itself (lost on a power cut, as the
// clock is). Valid only while the word next to it holds the magic.
static const uint32_t RTC_CLK_MAGIC = 0x434c4b31;  // "CLK1"
// The 24-h clock acceptances that cut history, carried across the restart that follows
// one (lost on a power cut); diag clock_cut_n
static const uint32_t RTC_CUT_MAGIC = 0x43555431;  // "CUT1"
static ClockHold gHold;  // loop() only (clockTick); diag reads the counters
uint32_t clockHeldCount() { return gHold.held_n; }
uint32_t clockCutCount() { return gRtcCut[0] == RTC_CUT_MAGIC ? gRtcCut[1] : 0; }

static void setSysSrc(TimeSrc s) {
  portENTER_CRITICAL(&gClkMux);
  gLadder.sys_src = s;
  portEXIT_CRITICAL(&gClkMux);
  gRtcClk[0] = RTC_CLK_MAGIC;
  gRtcClk[1] = (uint32_t)s;
}

bool outlookTimeOk() { return timeSynced() && timeSrc() != TimeSrc::Persisted; }

TimeSrc timeSrc() {
  const uint32_t now = (uint32_t)time(nullptr);
  portENTER_CRITICAL(&gClkMux);
  const TimeSrc s = gLadder.best(now);
  portEXIT_CRITICAL(&gClkMux);
  return s;
}

// t_last: the best time now, if it is due (or `force`) and moves forward; loop() only
static void clockPersist(bool force) {
  const uint32_t up = uptimeS(), sys = (uint32_t)time(nullptr);
  if (clockHeld(sys, storeClockRef())) return;  // a held clock is not written as the last known time
  portENTER_CRITICAL(&gClkMux);
  const uint32_t t = gLadder.bestTime(sys, up);
  const bool auth = timeValid(sys) && (gLadder.sys_src == TimeSrc::Sntp || gLadder.sys_src == TimeSrc::Dhcp);
  const bool due = gLadder.persistDue(t, up, force, auth);
  portEXIT_CRITICAL(&gClkMux);
  if (!due) return;
  Preferences p;
  p.begin(SYS_NS, false);
  const bool ok = p.putUInt("t_last", t) == sizeof(uint32_t);
  p.end();
  if (!ok) return;
  portENTER_CRITICAL(&gClkMux);
  gLadder.markWritten(t, up, auth);
  portEXIT_CRITICAL(&gClkMux);
}
void clockPersistNow() { clockPersist(true); }

// The source is marked SNTP in the callback itself, so a seed arriving before clockTick classifies it (Sntp or
// Dhcp) is refused; the clock lwIP just set is never overwritten by a browser
static void onSntpSync(struct timeval* tv) {
  // lwIP takes any reply (SNTP_CHECK_RESPONSE 0); an unsynced router can answer with a time
  // below the floor. Such a time is not a source: nothing is marked, the seed stays open, loop() logs and retries.
  if (!tv || !timeValid((uint32_t)tv->tv_sec)) {
    gSntpBad = tv && tv->tv_sec > 0 ? (uint32_t)tv->tv_sec : 1;
    return;
  }
  portENTER_CRITICAL(&gClkMux);
  if (gLadder.sys_src != TimeSrc::Sntp && gLadder.sys_src != TimeSrc::Dhcp) gSrcBeforeSync = gLadder.sys_src;
  gLadder.sys_src = TimeSrc::Sntp;  // provisional
  portEXIT_CRITICAL(&gClkMux);
  gSntpEvent = true;
}

// A factory reset forgets who set the clock, so the next boot does not carry it (nor write t_last back)
void clockForget() { gRtcClk[0] = 0; }

void clockBoot() {
  Preferences p;
  p.begin(SYS_NS, true);
  const uint32_t last = p.getUInt("t_last", 0);
  p.end();
  const uint32_t now = (uint32_t)time(nullptr), up = uptimeS();
  // Does the system clock survive esp_restart() on this core? If so, the RTC word says who set it.
  const bool carried = timeValid(now) && gRtcClk[0] == RTC_CLK_MAGIC && gRtcClk[1] >= (uint32_t)TimeSrc::Browser &&
                       gRtcClk[1] <= (uint32_t)TimeSrc::Sntp;
  portENTER_CRITICAL(&gClkMux);
  gLadder.persisted = timeValid(last) ? last : 0;
  gLadder.persisted_up = up;
  gLadder.sys_src = carried ? (TimeSrc)gRtcClk[1] : TimeSrc::None;
  portEXIT_CRITICAL(&gClkMux);
  if (timeValid(now) && !carried) {  // a valid clock nobody here set: not trusted (cannot happen after a power cut)
    struct timeval tv = {0, 0};
    settimeofday(&tv, nullptr);
  }
  logl(Log::Boot, "TIME", "clock at boot: %s (unix %lu); persisted t_last %lu%s; source %s",
       carried ? "kept across the restart" : "not set", (unsigned long)now, (unsigned long)last,
       timeValid(last) ? "" : " (none / below the floor)", timeSrcName(timeSrc()));
  sntp_set_time_sync_notification_cb(onSntpSync);
}

// Browser seed: setup page and admin login only. True = the system clock was set.
bool clockSeed(uint64_t ms, const char* who) {
  // A held clock is replaced only by a seed that releases the hold (the
  // admin's escape: a login from a device with the right time); any other seed meets the SNTP-first rule, so it never
  // overwrites SNTP nor restarts the 24-h run
  const uint32_t judge = seedJudgeTime((uint32_t)time(nullptr), storeClockRef(), (uint32_t)(ms / 1000));
  portENTER_CRITICAL(&gClkMux);
  const bool ok = gLadder.seedAccepted(ms, seedCeil(), judge);  // the build-date ceiling, the SNTP-first rule, the below-floor rule
  portEXIT_CRITICAL(&gClkMux);
  if (!ok) return false;
  const uint32_t t = (uint32_t)(ms / 1000);
  const uint32_t before = (uint32_t)time(nullptr);
  struct timeval tv = {(time_t)t, (suseconds_t)((ms % 1000) * 1000)};
  settimeofday(&tv, nullptr);
  setSysSrc(TimeSrc::Browser);
  gSeedT = t;
  gSeedMs = millis();
  logf("TIME", "clock seeded by the %s: unix %lu (was %lu)", who, (unsigned long)t, (unsigned long)before);
  gPersistReq = true;  // written by loop() within a second; a setup -> run restart writes it anyway
  return true;
}

// The public SNTP hosts back after a DHCP lease cleared them (and slot 0 for them when there is no DHCP server)
static void sntpSlotsTick() {
  if (!esp_sntp_enabled()) return;  // setup mode: no SNTP
  SntpSlot cur[SNTP_SLOTS];
  for (int i = 0; i < SNTP_SLOTS; i++) {
    const ip_addr_t* a = esp_sntp_getserver(i);
    cur[i] = SntpSlot{esp_sntp_getservername(i), a && !ip_addr_isany(a)};
  }
  const char* want[SNTP_SLOTS];
  bool moveDhcp = false;
  if (!sntpPlan(cur, want, &moveDhcp)) return;
  if (moveDhcp) {  // the lease's server goes behind the public hosts (SNTP before DHCP)
    const ip_addr_t a = *esp_sntp_getserver(0);
    esp_sntp_setserver(2, &a);
  }
  for (int i = 0; i < SNTP_SLOTS; i++)
    if (want[i]) esp_sntp_setservername(i, want[i]);  // static strings: lwIP keeps the pointer
  const ip_addr_t* d = esp_sntp_getserver(2);
  const bool dhcp = !esp_sntp_getservername(2) && d && !ip_addr_isany(d);
  logf("TIME", "SNTP servers set: %s / %s / %s", esp_sntp_getservername(0) ? esp_sntp_getservername(0) : "-",
       esp_sntp_getservername(1) ? esp_sntp_getservername(1) : "-", dhcp ? "DHCP server" : "-");
}

// loop(), every second (both modes): the SNTP sync source, the correction of a seed, the 15-min t_last
void clockTick() {
  sntpSlotsTick();
  static uint8_t pending = 0;  // ticks since the SNTP callback: lwIP marks the server reachable after it (~2 s)
  static uint32_t badMs = 0;   // when a below-floor answer came; SNTP restarts 5 min later while still invalid
  if (const uint32_t bad = gSntpBad.exchange(0)) {
    static RateLimitedLine line;  // a router that keeps answering 1970
    uint32_t n, min;
    char sx[32];
    if (line.hit(millis(), &n, &min))
      logf("TIME", "SNTP/DHCP answer below the floor ignored (unix %lu): not a time source; retry in 5 min%s", (unsigned long)bad,
           rateSuffix(sx, sizeof sx, n, min));
    badMs = millis() | 1;
  }
  if (badMs && millis() - badMs >= 300000) {
    badMs = 0;
    if (!timeValid((uint32_t)time(nullptr)) && esp_sntp_enabled()) esp_sntp_restart();
  }
  if (gSntpEvent.exchange(false)) pending = 1;
  if (pending && ++pending > 3) {
    pending = 0;
    // The DHCP server sits in slot 2 (an address, no name) once sntpPlan moved it; slot 0 right after a fresh
    // lease, before the move
    int dslot = -1;
    for (int i = SNTP_SLOTS - 1; i >= 0 && dslot < 0; i--)
      if (!esp_sntp_getservername(i) && esp_sntp_getserver(i) && !ip_addr_isany(esp_sntp_getserver(i))) dslot = i;
    const bool dhcpSlot = dslot >= 0;
    uint8_t pubReach = 0;  // the named slots (the public hosts, 0-1)
    for (int i = 0; i < SNTP_SLOTS; i++)
      if (esp_sntp_getservername(i)) pubReach |= esp_sntp_getreachability(i);
    const bool pub = (pubReach & 1) != 0;
    const bool dhcp = dhcpSlot && (esp_sntp_getreachability(dslot) & 1);
    const TimeSrc s = (dhcp && !pub) ? TimeSrc::Dhcp : TimeSrc::Sntp;
    portENTER_CRITICAL(&gClkMux);
    const TimeSrc was = gSrcBeforeSync;  // the provisional Sntp of the callback is not "was"
    gSrcBeforeSync = s;
    portEXIT_CRITICAL(&gClkMux);
    setSysSrc(s);
    const uint32_t now = (uint32_t)time(nullptr);
    if (was == TimeSrc::Browser && gSeedT) {
      const int32_t d = (int32_t)(now - (gSeedT + (millis() - gSeedMs) / 1000));
      logf("TIME", "time corrected by %ld s (browser seed -> %s)", (long)d, timeSrcName(s));
    }
    if (was != s)
      logf("TIME", "clock synced by %s (unix %lu; DHCP server %s, reach slots %02x/%02x/%02x)", timeSrcName(s),
           (unsigned long)now, dhcpSlot ? "given" : "none", esp_sntp_getreachability(0), esp_sntp_getreachability(1),
           esp_sntp_getreachability(2));
  }
  clockPersist(gPersistReq.exchange(false));
  // The hold's bookkeeping; public SNTP for 24 h, steady, accepts a held clock
  {
    const uint32_t sys = (uint32_t)time(nullptr), up = uptimeS(), ref = storeClockRef();
    portENTER_CRITICAL(&gClkMux);
    const bool pub = gLadder.sys_src == TimeSrc::Sntp;
    portEXIT_CRITICAL(&gClkMux);
    const bool wasHeld = gHold.held;
    bool started = false;
    const bool accept = gHold.step(sys, up, ref, pub, &started);
    if (started)
      logf("TIME", "clock %lu held: %lu s behind the history (trusted to %lu); writes, NTP and t_last wait", (unsigned long)sys,
           (unsigned long)(ref - sys), (unsigned long)ref);
    else if (wasHeld && !gHold.held && !accept)
      logf("TIME", "clock released (unix %lu, trusted to %lu)", (unsigned long)sys, (unsigned long)ref);
    if (accept) {
      const int cut = storeClockAccept(sys);
      if (gRtcCut[0] != RTC_CUT_MAGIC) gRtcCut[0] = RTC_CUT_MAGIC, gRtcCut[1] = 0;
      gRtcCut[1]++;
      logf("TIME", "%d record(s) cut; restarting", cut);
      sysPlanRestart("clock_accepted", 500);
    }
  }
}

// loop(): the debug hang (admin route, test builds) holds the loop until the Task WDT fires
void sysLoop() {
#ifdef FAUXCLOUD_TEST  // the hang is a test image's (its route is FAUXCLOUD_TEST only)
  if (!gHangReq) return;
  gHangReq = false;
  logf("SYS", "debug hang: busy loop for 40 s, the Task WDT should reset within 30 s");
  const uint32_t t0 = millis();
  while (millis() - t0 < 40000) {
  }
  logf("SYS", "debug hang ended without a reset (Task WDT not armed?)");
#endif
}

}  // namespace app
