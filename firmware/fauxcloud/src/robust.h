// robust.h -- the pure parts of the robustness layer: the image marker an OTA upload must carry, the rollback
// health check, the network watchdog timers and the reset-reason names. Plain C++, host-compilable.
#pragma once
#include <cstddef>
#include <cstdint>

namespace fc {

// ---------------------------------------------------------------- image marker (OTA sanity check)
// Every fauxcloud image carries "FAUXCLOUD-IMAGE:<version> <build date>" in rodata (app_core.cpp). On Arduino the
// ESP-IDF app descriptor names "arduino-lib-builder" for every sketch, so it cannot tell our image from another.
// The scanner never stores the marker literally: its pattern is assembled at runtime, so the scanner's own code in
// the running image cannot be what it finds.
class MarkerScan {
 public:
  MarkerScan();
  void feed(const uint8_t* data, size_t len);  // any chunking: the marker may span chunks
  bool found() const { return found_; }
  const char* version() const { return version_; }  // the text after the marker, up to NUL (max 47 chars)

 private:
  char pat_[20];
  size_t patLen_, matched_ = 0;
  bool found_ = false, copying_ = false;
  char version_[48] = {};
  size_t vlen_ = 0;
};

// ---------------------------------------------------------------- rollback health check
// A freshly updated image boots in PENDING_VERIFY; it marks itself valid only after the home Wi-Fi joined, one HTTP
// response was served and 60 s ran without a watchdog reset. Otherwise the next reset boots the previous image.
struct RollbackInputs {
  bool pending;          // running image is ESP_OTA_IMG_PENDING_VERIFY
  bool sta_up;
  bool http_served;
  uint32_t uptime_s;
  bool wdt_reset;        // this boot came from a watchdog reset
};
constexpr uint32_t ROLLBACK_HEALTHY_S = 60;
bool shouldMarkValid(const RollbackInputs& in);
enum class OtaState : uint8_t { Stable, Pending, RolledBack };
const char* otaStateName(OtaState s);

// ---------------------------------------------------------------- network watchdog
// STA without an IP: a Wi-Fi re-init every 15 min; at 60 min one restart (reason "sta_lost") -- once per outage
// (else hours without home Wi-Fi mean a restart every 61 min). The app keeps "rebooted for this outage"
// in NVS across that restart and clears it on the next STA join. Paused (the timer restarts) while an OTA write or
// a Wi-Fi change attempt runs: those take the STA down on purpose.
enum class NetAction : uint8_t { None, Reinit, Reboot };
class NetWatchdog {
 public:
  static constexpr uint32_t REINIT_S = 15 * 60, REBOOT_S = 60 * 60;
  NetAction feed(uint32_t now_s, bool sta_ok, bool paused);
  uint32_t reinits() const { return reinits_; }
  // true = this outage already had its restart (set at boot from NVS, and when feed() returns Reboot)
  void setRebooted(bool r) { rebooted_ = r; }
  bool rebooted() const { return rebooted_; }

 private:
  bool down_ = false, rebooted_ = false;
  uint32_t since_ = 0, lastReinit_ = 0, reinits_ = 0;
};

// ---------------------------------------------------------------- heap-fragmentation watchdog
// Seen on the bench: stalled HTTP clients left the heap in pieces (largest block 1268 B) -- MQTT could not reconnect,
// console uploads stopped, nothing recovered in minutes. Rule: the largest free block below FRAG_LARGEST_MIN for
// FRAG_HOLD_MS without one upload processed in that time -> a planned restart (reason "heap_frag"). An upload that
// still gets through, or an OTA write (it fragments on purpose and restarts anyway), restarts the window.
// 180 s (60 s gave 4 restarts in 10 min of a stall attack on the bench, uploads lost each time), and at
// most one such restart per 30 min -- after a "heap_frag" boot the app sets quiet(FRAG_QUIET_MS).
class FragWatch {
 public:
  static constexpr uint32_t FRAG_LARGEST_MIN = 4096, FRAG_HOLD_MS = 180000, FRAG_QUIET_MS = 30 * 60000;
  // once a second; true = restart now (then it stays armed false until the window restarts)
  bool feed(uint32_t now_ms, uint32_t largest, uint32_t uploads, bool ota);
  void quiet(uint32_t until_ms) { quiet_ = until_ms; }  // no restart before this (ms since boot)

 private:
  bool on_ = false;
  uint32_t since_ = 0, uploads_ = 0, quiet_ = 0;
};

// load shedding: while the largest byte-addressable block is below SHED_LARGEST_MIN, every request but a
// console upload gets an immediate 503 with Connection: close (no body, nothing queued), so stalled clients cannot
// grow the TCP send queues further; uploads are never shed.
constexpr uint32_t SHED_LARGEST_MIN = 8192;
inline bool shedRequest(bool upload, uint32_t largest) { return !upload && largest < SHED_LARGEST_MIN; }

// page pacing: lwIP copies every answer into the socket's send
// queue and keeps it until the client acknowledges it (TCP_SND_BUF 5 744 B per socket), so a burst of page answers to
// slow clients (a phone waking, several tabs) stacked ~50 KB of the byte heap within a second. A page answer starts only
// while free8 >= PACE_FREE8; below it the one httpd worker waits PACE_STEP_MS at a time, at most PACE_MAX_MS, for earlier
// answers to be acknowledged, then sheds the request through the load-shedding 503 path (http_shed; the page asks once more,
// core.js). Uploads and the catch-all are never paced; the largest-block rule stays first. 200 ms at most: the wait
// holds the only httpd worker, and a console upload queued behind a paced page waits with it (uploads first).
constexpr uint32_t PACE_FREE8 = 24 * 1024, PACE_STEP_MS = 20, PACE_MAX_MS = 200;
enum class Pace : uint8_t { Go, Wait, Shed };
inline Pace pagePace(uint32_t free8, uint32_t largest8, uint32_t waited_ms) {
  if (largest8 < SHED_LARGEST_MIN) return Pace::Shed;
  if (free8 >= PACE_FREE8) return Pace::Go;
  return waited_ms + PACE_STEP_MS <= PACE_MAX_MS ? Pace::Wait : Pace::Shed;
}

// ---------------------------------------------------------------- page-session cap
// Every httpd session that served something other than a console upload ("page"), with its last request time. More
// than CAP open at once -> the idlest other one is closed (stalled and keep-alive browser sessions hold lwIP buffers
// and the heap). Uploads are exempt: a console uses a fresh connection per upload, closed after
// the reply. httpd task only.
class PageSessions {
 public:
  static constexpr int CAP = 6, SLOTS = 16;
  // a page request on fd at now_ms; returns the fd to close (the idlest other open page session) or -1
  int touch(int fd, uint32_t now_ms);
  void drop(int fd);  // the session closed (httpd's close callback)
  int count() const { return n_; }
  int fdAt(int i) const { return fd_[i]; }

 private:
  int fd_[SLOTS] = {};
  uint32_t at_[SLOTS] = {};
  bool closing_[SLOTS] = {};
  int n_ = 0;
};

// ---------------------------------------------------------------- request body on the only httpd worker
// A client that announces a body and never sends it (or drips it) must not hold the worker that answers the
// console: give up after two receive timeouts in the whole body (~6 s with recv_wait_timeout 3 s) or after
// BODY_DEADLINE_MS in all (a drip; at most one more receive timeout past it). The caller then closes the connection,
// so httpd does not drain the rest either. On the board: 10 of 3000 bytes, then nothing -> the worker is free again
// after ~6 s.
// Drained: over the cap, but announced <= drain_max and read to the end and thrown away under the
// same rules, so the caller can answer with an error the browser reads, then close. TooBig: over drain_max, nothing
// read -- the caller cuts the connection at once.
enum class BodyRead : uint8_t { Ok, TooBig, Stalled, Lost, Drained };
constexpr int BODY_RECV_TIMEOUT = -3;  // HTTPD_SOCK_ERR_TIMEOUT (static_assert in app_http.cpp)
constexpr uint32_t BODY_DEADLINE_MS = 6000;
using BodyRecv = int (*)(void* ctx, char* buf, size_t len);  // httpd_req_recv: bytes, BODY_RECV_TIMEOUT or < 0
using BodyClock = uint32_t (*)(void* ctx);                    // milliseconds, wraps
// Reads exactly `len` bytes into buf (cap + 1 bytes, NUL-terminated on Ok); *got = bytes read in any case.
BodyRead readAll(BodyRecv recv, BodyClock nowMs, void* ctx, char* buf, size_t cap, size_t len, size_t* got,
                 size_t drain_max = 0);
const char* bodyReadName(BodyRead r);

// ---------------------------------------------------------------- reset reason
// esp_reset_reason_t value -> a short name for HA / admin. A software restart the firmware planned itself carries
// its own reason (saved in NVS just before: "ota", "sta_lost", "admin", "factory_reset", ...), which wins.
const char* resetReasonName(int esp_reset_reason, const char* planned);
bool resetIsWatchdog(int esp_reset_reason);

// Board behaviour (seen on a WROOM-32 DevKit; a known, benign DevKit quirk). Every plug-in and every
// EN / RTS pulse boots twice inside the ROM:
//   rst:0x1 (POWERON_RESET),boot:0x13 (SPI_FAST_FLASH_BOOT)
//   flash read err, 1000
//   ets_main.c 371
//   rst:0x10 (RTCWDT_RTC_RESET),boot:0x13 (SPI_FAST_FLASH_BOOT)
// The ROM's first flash read fails, its RTC WDT resets the chip, and the second boot is the only one the app runs:
// esp_reset_reason() says 7 (other WDT), rtc cpu0 16 / cpu1 14. The app keeps a magic word in RTC_NOINIT memory. A power cut or an EN pulse loses it, while
// software resets, panics, watchdogs and brownouts keep it. With the magic absent, a raw reason that is not one of
// those counts as a power-on (1). The raw codes stay in diag (reset_raw).
constexpr uint32_t RTC_BOOT_MAGIC = 0x0FC0B007u;
int effectiveResetReason(int esp_reset_reason, bool rtc_magic_present);

}  // namespace fc
