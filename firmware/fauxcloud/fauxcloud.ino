// fauxcloud.ino -- the sketch entry: setup() and loop().
//
// Two modes, nothing in between:
//   Setup mode  first boot / factory reset / "setup again": AP "fauxcloud-setup-XXXX" with the shared, documented
//               password "fauxcloud" (no screen or label to show a unique one), captive setup page (home Wi-Fi, time
//               zone, admin password, AP password), DNS only.
//               After a successful join: setup_done, reboot. Time-boxed: 30 min without a request -> reboot.
//   Run mode    AP "fauxcloud-XXXX" for the consoles only (DNS, NTP, uploads, read-only status); STA on the home
//               LAN with mDNS <hostname>.local, MQTT (optional), and the admin face behind a login (STA only).
// BOOT (board.h, GPIO0 on the DevKit) held 5 s in either mode = factory reset. Settings live in NVS; secrets.h is an optional dev seed.
//
// The modules beside this file are listed in app.h; all logic is in src/ (host-tested). Serial 115200.

#include <esp_mac.h>

#include "app.h"
#include "board.h"
#include "src/lifecycle.h"

using namespace app;

#ifdef FAUXCLOUD_TEST
static const uint32_t SETUP_IDLE_MS = 60UL * 1000UL;  // test image: the setup time box in a minute
#else
static const uint32_t SETUP_IDLE_MS = 30UL * 60UL * 1000UL;
#endif

void setup() {
  bootlogBegin();  // before the first logf
  Serial.begin(115200);
  delay(300);
  Serial.println();
  esp_read_mac(gMac, ESP_MAC_WIFI_STA);
  mac4(gMac, gMac4);
  // The boot facts are Log::Boot (the boot log's head keeps only these, ~20 lines)
  logl(Log::Boot, "BOOT", "%s, core %s, MAC %02x:%02x:%02x:%02x:%02x:%02x", SW_VERSION, ESP_ARDUINO_VERSION_STR, gMac[0], gMac[1],
       gMac[2], gMac[3], gMac[4], gMac[5]);
  pinMode(BOARD.button_pin, BOARD.button_active_low ? INPUT_PULLUP : INPUT_PULLDOWN);  // hold 5 s = factory reset

  gLock = xSemaphoreCreateMutex();
  // 8 deep: the loop drains it every 10 ms and httpd has one worker, so 1-2 wait at most; each
  // Upload is ~330 B, and the 2.6 KB that 16 would cost stay in the large block a 4-station OTA needs
  gQueue = xQueueCreate(8, sizeof(Upload));
  nvsOpen();
  sysBoot();     // reset reason, restart counter, OTA state, crash data, Task WDT on the loop
#if FC_DIAG_BENCH
  otaHeapBoot();  // the last OTA write's heap record (RTC) into diag.ota_heap, then cleared (bench only)
#endif
  clockBoot();   // persisted last-known time, the source of a clock kept across a restart
  storeBegin();  // LittleFS: history and the today snapshot; mounted in both modes (factory reset formats it)
  loadSettings();
  devSeed();
  loadSettings();
  uint32_t pbkdfMs = 0;
  const bool cryptoOk = cryptoSelfTest(&pbkdfMs);
  if (!cryptoOk) logf("BOOT", "PBKDF2-SHA256 self-test FAILED (%lu iterations, %lu ms)", (unsigned long)PBKDF2_ITER,
                     (unsigned long)pbkdfMs);  // an error: the tail too, at once

  gMode = (gSettings.setup_done && settingsComplete(gSettings)) ? Mode::Run : Mode::Setup;
  if (gMode == Mode::Run) {
    blockedLoad();
    adoptBootBegin();                            // a journal counts this boot first (given up after three)
    const bool adoptNvsOk = adoptBootNvs();      // a confirmed adopt moves the old id's settings, key, learners ...
    // Strictly in order -- no header, snapshot or target rename unless the settings moved
    const bool adoptStoreOk = adoptNvsOk && adoptBootStore();  // ... its ring headers and snapshot entry
    loadDataConfig();
    fwdBegin();  // the forwarding targets; the net-out task only with the switch on and a target enabled
    adoptBootFwd(adoptNvsOk && adoptStoreOk);  // ... its forwarding targets; the journal goes last
    netStartRun();
  } else {
    loadBridge();  // setup again keeps the bridge settings; the setup apply changes only its own fields
    netStartSetup(SETUP_AP_PASS);
  }
  httpStart();
  logl(Log::Boot, "BOOT", "%s mode, sizeof Station=%u, free8 %u; PBKDF2 self-test %s in %lu ms",
       gMode == Mode::Run ? "run" : "setup", (unsigned)sizeof(Station), (unsigned)heapNow().free8, cryptoOk ? "ok" : "FAILED",
       (unsigned long)pbkdfMs);
  heartbeat();
}

void loop() {
#ifdef FAUXCLOUD_BAD_IMAGE
  // Rollback test only: a build that crashes 2 s into loop(), after the normal boot log. Built with an extra
  // -DFAUXCLOUD_BAD_IMAGE by hand; never set in sketch.yaml.
  static uint32_t firstLoopMs = millis();
  if (millis() - firstLoopMs > 2000) abort();
#endif
  coreLoop();
  netLoop();
  sysLoop();
  static uint32_t lastSec = 0, lastBeat = 0;
  const uint32_t ms = millis();
  if (gMode == Mode::Run) {
    captureTick();  // listening to a new console: its start / stop order runs ahead of mqttLoop (which follows its flags)
    mqttLoop();
    ingestQueued();
    if (ms - lastSec >= 1000) {
      lastSec = ms;
      everySecond();
    }
  } else {
    static bool idleActed = false;  // once -- the reboot is planned (or done), never re-armed per pass
    const SetupIdle a =
        idleActed ? SetupIdle::Wait : setupIdleAction(setupIdleMs(), SETUP_IDLE_MS, settingsComplete(gSettings));
    if (a != SetupIdle::Wait) idleActed = true;
    if (a == SetupIdle::BackToRun) {  // an accidental BOOT press must not cut the consoles off for good
      gSettings.setup_done = true;
      const bool saved = saveSettings();
      logf("SETUP", "no setup activity for %lu min, settings complete -> back to run mode (%s)",
           (unsigned long)(SETUP_IDLE_MS / 60000), saved ? "saved" : "NOT saved");
      sysPlanRestart("setup_idle", 100);
    } else if (a == SetupIdle::Restart) {
      logf("SETUP", "no setup activity for %lu min -> reboot (setup opens again)", (unsigned long)(SETUP_IDLE_MS / 60000));
      clockPersistNow();
      delay(100);
      ESP.restart();
    }
  }
  static uint32_t lastClk = 0;
  if (ms - lastClk >= 1000) {
    lastClk = ms;
    clockTick();
  }
  if (ms - lastBeat >= 60000) {
    lastBeat = ms;
    heartbeat();
  }
  delay(10);
}
