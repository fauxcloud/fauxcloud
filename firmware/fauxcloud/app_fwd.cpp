// app_fwd.cpp -- forwarding to online weather services: the targets in NVS,
// the loop tick (busy table + scheduler, src/fwd_sched.h), the net-out task (one connection at a time), the
// admin routes. The logic is in src/fwd.* and src/fwd_sched.* (host-tested). FWD_TLS (src/fwd.h) picks the variant:
// 0 = plain HTTP over an lwip socket, no TLS client and no anchors in the image (the HTTP-only fallback); 1 = the release:
// the same socket with BearSSL on top (src/tlsc + src/bearssl, vendored), HTTPS per target, and the METAR reference:
// the airports' reports from aviationweather.gov on the same task, admitted by the one net-out queue (src/netq.h:
// forwarding send > user job > METAR fetch, one job at a time), parsed line by line as they stream, kept in RAM only.
//
// Threads: fwdTick() runs on the loop task once a second (everySecond, after learnTick); it decides, renders the job
// (the key is read from NVS into the job and wiped after the send) and hands it to the net-out task through a one-deep
// slot. The task does DNS -> connect -> (TLS) -> write -> read -> close and posts the result; the loop never waits on
// it, the httpd task never touches it, nothing outbound runs under gLock. The routes run on the httpd task: they edit
// the targets under gLock, ask the scheduler for a test under gFwdMux, and never send anything themselves.
//
// RAM: one 1 KB job buffer holds, in turn, the station snapshot and the request
// (the render writes the request target straight into it); no template text is ever built. The net-out task (5 KB
// stack; 8 KB with FWD_TLS) and its buffers come from the heap at boot when the switch is on and a target is enabled,
// otherwise at the first enable or test, and stay; BearSSL's three blocks (FWD_TLS) only for the length of a TLS send.
#include "app.h"  // Arduino first: lwip's INADDR_NONE macro would break IPAddress.h after it

#include <errno.h>
#include <fcntl.h>
#include <lwip/netdb.h>
#include <lwip/sockets.h>
#include <unistd.h>

#include <atomic>

#include "src/forecast.h"
#include "src/fwd.h"
#include "src/fwd_sched.h"
#include "src/netq.h"
#if FWD_TLS
#include <esp_random.h>

#include <new>

#include "src/airport.h"
#include "src/fwd_roots.h"
#include "src/metar.h"
#include "src/metar_sched.h"
#include "src/metar_store.h"
#include "src/wufc.h"
#include "src/wufc_sched.h"
#include "src/tlsc.h"
#endif
#include "src/ha_discovery.h"
#include "src/json.h"

namespace app {

static FwdTarget gFwd[FWD_SLOTS];  // under gLock (the loop copies one at a send, the routes edit)
static FwdSched gSched;            // under gFwdMux (loop: tick / result; httpd: test requests)
static portMUX_TYPE gFwdMux = portMUX_INITIALIZER_UNLOCKED;

// A connection's diagnosis, "diag" in the admin JSON: the byte heap around it (heapNow(), 8-bit),
// the timings and, for TLS (FWD_TLS=1), BearSSL's stage, error, the negotiated suite and the largest record the server
// sent (the board's answer to "does this host honour MFL").
struct FwdDiag {
  uint32_t pre8, largest8_pre;   // free / largest block right before the connect
  uint32_t min8, largest8_min;   // the lowest over the job (the allocator's minimum; the smallest largest block sampled)
  uint16_t ms_dns, ms_tcp, ms_hs, ms_total;
  int16_t sock_err;              // getaddrinfo / errno of a failed DNS or connect
  int16_t br_err;                // BearSSL's last error (FWD_TLS)
  uint16_t version, suite, rec_in_max;  // FWD_TLS
  uint16_t rec_app_max;          // FWD_TLS: the largest application-data record (the buffer's real test)
  uint8_t curve;                 // FWD_TLS: the ECDHE curve (BR_EC_*)
  uint16_t stack_free;           // the net-out task's high-water after the job
  uint8_t stage;                 // TlscStage (FWD_TLS)
};

// What the sheet shows per target (the loop writes under gFwdMux; the GET reads words, a torn row is harmless)
struct FwdLast {
  uint32_t t;        // the last attempt's end (unix, 0 = none)
  uint32_t ok_t;     // the last accepted send
  int16_t status;    // HTTP status, -1 = none
  FwdResult result;
  uint16_t ms;
  char detail[20];   // abort / render reason
  FwdDiag d;
};
static FwdLast gLast[FWD_SLOTS];

// ---------------------------------------------------------------- the net-out job (one deep)
enum JobSt : uint8_t { J_IDLE, J_POSTED, J_RUNNING, J_DONE };
static std::atomic<uint8_t> gJobSt{J_IDLE};
static std::atomic<bool> gAbort{false};
static const char* volatile gAbortWhy = nullptr;
constexpr size_t REQ_LEN = 1024;
struct Job {
  int slot;
  FwdKind kind;
  FwdType type;
  bool tls;
  bool sent;                   // the request left whole (plain: written; TLS: flushed)
  uint16_t roots;              // FWD_TLS: the anchors for this host (16 bits, GlobalSign R3 is bit 8)
  uint16_t port;
  uint32_t seen;               // the record's last_seen
  char host[FWD_HOST_MAX + 7];
  char success[FWD_SUCCESS_MAX + 1];
  char id[FWD_ID_MAX + 1];
  char key[FWD_KEY_MAX + 1];   // read from NVS at render time, wiped after the send (masking an echoed body)
  union {                      // the loop's scratch while idle, the request while a job is posted / running
    char req[REQ_LEN];         // request line + headers (the key inside); wiped after the send
    FwdLiveIn live;            // the station snapshot (startJob, before the render)
  };
  size_t req_len;
  uint32_t t0;
  // result
  FwdResult result;
  int status;
  uint32_t retry_after, ms;
  char body[81];               // wu_compat / template: the first 80 B of an unexpected body, printable, id/key masked
  const char* why;             // "dns", "connect", "handshake", "tls_untrusted", "too_large", "read", "aborted_heap", ...
  FwdDiag d;
  uint32_t unix_now;           // real time for the certificate check (0 = none: no TLS job starts)
#if FWD_TLS
  TlscWhy tls_why;
#endif
};
static_assert(sizeof(FwdLiveIn) <= REQ_LEN, "the job buffer is shared");
// The response parser (net-out task, while a job runs) and the render scratch (loop, while no job is posted) are never
// in use at the same time.
union Scratch {
  FwdResp resp;
  struct {
    FwdTarget t;    // the one target, copied out of gFwd
    FwdRecord rec;  // its clean record
  } r;
};
static TaskHandle_t gNetTask = nullptr;
static uint32_t gNetStackFree = 0;
// The send low-waters are bench diagnostics (FC_DIAG_BENCH); last.diag keeps the failing send's own heap figures
#if FC_DIAG_BENCH
static std::atomic<uint32_t> gSendHeapMin{UINT32_MAX}, gSendLargestMin{UINT32_MAX}, gSendMsMax{0};
#endif
static std::atomic<bool> gSending{false}, gSentSinceSample{false};
static std::atomic<uint32_t> gPreSendFree{0};  // free heap read by the loop just before the job was rendered
static uint32_t gTickUsMax = 0;  // the longest tick (the one number that says "the loop was held")
// Everything below about the tick's parts is bench only; BENCH(...) keeps a statement only at FC_DIAG_BENCH=1
#if FC_DIAG_BENCH
#define BENCH(...) __VA_ARGS__
#else
#define BENCH(...)
#endif
// The tick in parts, in
// tick order, each part's own time (its gLock waits taken out); the maxima since boot, the parts of the slowest tick
// (with the net-out job's state at its start: a send on the same core shares the CPU with the loop task, equal
// priority) and the count of ticks over 5 ms of own work. startJob in its own parts.
enum TickPart : uint8_t { TP_FINISH, TP_ABORT, TP_VIEW, TP_BUSY, TP_HOUR, TP_HEAP, TP_GATES, TP_SCHED, TP_START, TP_COUNT };
enum StartPart : uint8_t { SP_HEAP, SP_SNAP, SP_RECORD, SP_KEY, SP_RENDER, SP_COUNT };
#if FC_DIAG_BENCH
static uint32_t gTickLockUsMax = 0, gTickOwnUsMax = 0;  // the gLock waits apart
static const char* const TICK_PART_NAME[TP_COUNT] = {"finish", "abort", "view",  "busy", "hour",
                                                     "heap",   "gates", "sched", "start"};
static const char* const START_PART_NAME[SP_COUNT] = {"heap", "snap", "record", "key", "render"};
static const char* const JOB_ST_NAME[4] = {"idle", "posted", "running", "done"};
static uint32_t gTickPartMax[TP_COUNT], gTickWorst[TP_COUNT], gStartPart[SP_COUNT], gStartPartMax[SP_COUNT],
    gStartWorst[SP_COUNT];
static uint32_t gTickSlow = 0;  // ticks with more than TICK_SLOW_US of own work
static uint8_t gTickWorstJob = 0;
static uint32_t gTickWorstT = 0;  // when the worst tick ran (real time; 0 = none, or no real time then)
// startJob's parts, written into gStartPart (zeroed per tick); a no-op struct in the release
struct StartClock {
  uint32_t t = micros();
  void cut(int p) {
    const uint32_t n = micros();
    gStartPart[p] += n - t;
    t = n;
  }
  void lockWait();  // the gLock wait into gStartLockUs (defined after it)
};
#else
struct StartClock {
  void cut(int) {}
  void lockWait() {}
};
#endif
[[maybe_unused]] constexpr uint32_t TICK_SLOW_US = 5000;
// Why attempts were not delivered, per target: FwdCause, scheduled sends only; under gFwdMux
static uint16_t gCause[FWD_SLOTS][(int)FwdCause::COUNT];
// The net-out stack: 5 KB plain (1 304 B of 4 KB were left after plain sends); 8 KB with BearSSL (1 620 B of 7 KB
// free after RSA-2048 chains; an ECDSA P-384 chain runs ~1 KB deeper)
constexpr uint32_t NETOUT_STACK = FWD_TLS ? 8192 : 5120;

// "Send one now" (one at a time, polled by the sheet); under gFwdMux. Its body excerpt and redacted target live in gB.
struct FwdTest {
  uint8_t state;  // 0 idle, 1 running, 2 done
  int8_t slot;
  int16_t status;
  uint32_t ms;
  char result[14];
  char why[20];
  FwdDiag d;
  uint16_t gen;   // gVerGen[slot] when the test was asked; a later edit makes its result verify nothing
};
static FwdTest gTest;
static uint16_t gVerGen[FWD_SLOTS];  // under gFwdMux: bumped by every PUT that changes a target, and by its removal
static std::atomic<uint8_t> gVerSave{0};  // slots verified in RAM by the loop, not yet in NVS
#if FWD_TLS
// The METAR reference. The settings and the readings under gLock (RAM only:
// no reading is ever written to flash); the source, the user jobs and the last job's diag under gFwdMux.
static MetarCfg gMetarCfg;      // gLock; NVS blob "metar" (v1)
static std::atomic<bool> gMetarSave{false};  // a lookup refreshed a name in RAM; saved by the httpd task
static MetarStore gMetarStore;  // gLock
static MetarSched gMetar;       // gFwdMux
static MetarLookup gLook;       // gFwdMux: the last airport lookup
struct MetarRun {               // gFwdMux: the last "run once" for the sheet
  uint8_t state;                // 0 idle, 1 waiting, 2 running, 3 done
  char result[14];
};
static MetarRun gRun;
struct MetarLastJob {           // gFwdMux: the last METAR job (R&D in the admin JSON, never a notice)
  uint32_t t;
  FwdKind kind;
  MetarOutcome out;
  int16_t status;
  char why[20];
  FwdDiag d;
  uint32_t http_idle_ms;        // since the last page request when it started (>= 3 000 wanted)
  uint16_t http_during;         // ticks that saw a page request while it ran
  uint8_t lines, garbage, unwanted, field_bad, trunc;
  uint8_t nil, old;  // NIL reports and reports outside the time window, not garbage
  bool taf;          // the hour's TAF cycle
};
static MetarLastJob gMetarLast;
static uint32_t gMetarPage0 = 0;  // loop: the page clock at the job's start (the yield rule: functional, keep)
// The WU forecast line. The settings and the store under gLock (RAM only),
// the source and the user's "Fetch now" under gFwdMux; the key only in NVS `wk` (read by the job, wiped after).
static WufcCfg gWufcCfg;       // gLock; NVS blob "wufc" (v1)
static WufcStore gWufcStore;   // gLock
static bool gWufcKeySet = false;  // gLock: `wk` exists (NVS read at boot and at the POST, never on the loop tick)
static std::atomic<bool> gWufcSave{false};  // an accepted Fetch now verified the key in RAM; the httpd task saves it
static WufcSched gWufc;        // gFwdMux
static MetarRun gWuRun;        // gFwdMux: the last "Fetch now" for the sheet (0 idle, 1 waiting, 2 running, 3 done)
static uint32_t gWuRunT = 0;   // gFwdMux: when it ended (real time; "Tried {t}" / "Fetched {t}")
struct WufcLastJob {           // gFwdMux
  uint32_t t;
  WufcOutcome out;
  int16_t status;
  char why[20];
  FwdDiag d;
};
static WufcLastJob gWufcLast;
static bool gWufcRan = false;  // gFwdMux: a WU job ran since boot
// Every accepted settings change bumps the epoch (gLock); a pull started under an older one neither stores
// its line nor verifies a key the user has since changed
static uint32_t gWufcEpoch = 0, gWufcJobEpoch = 0;
static bool gWuHeld = false;  // gFwdMux: a queued Fetch now is held by its start gate (the tick's busy table and heap)
#if FC_DIAG_BENCH  // the page-idle statistics are bench only
static uint16_t gHttpIdleHist[6];  // per job, idle ms < 3 000 | < 5 000 | < 10 000 | < 30 000 | < 60 000 | more
static uint32_t gMetarIdle0 = 0;
static uint16_t gMetarDuring = 0;
#endif
static bool metarKind(FwdKind k) { return k == FwdKind::Fetch || k == FwdKind::RunOnce || k == FwdKind::Lookup; }
static bool wufcKind(FwdKind k) { return k == FwdKind::WuFetch || k == FwdKind::WuRun; }
// The net-out line is taken (a job running or its result unread, or a forwarding / METAR job). gFwdMux held.
static bool wufcLineBusy() { return !(gNetTask && gJobSt.load() == J_IDLE) || gSched.running() || gMetar.running(); }
// The streaming sinks' state lives in the job block (gB), never on the net-out stack (1.6 KB free at the peak)
struct MetarFetchBuf {
  MetarLineSink sink;
  char want[METAR_AIRPORTS_MAX][5];
  const char* wantp[METAR_AIRPORTS_MAX];
  int nwant;
  uint32_t now;
  MetarObs got[METAR_AIRPORTS_MAX];
  uint8_t ngot, garbage, unwanted, field_bad, nil, old;
  char dropped[65];
  TafParser tp;  // the hour's TAF cycle reads its body here (the line sink routes to it)
  bool taf;
};
constexpr size_t METAR_BUF0 = sizeof(MetarFetchBuf) > sizeof(AirportJsonSink) ? sizeof(MetarFetchBuf) : sizeof(AirportJsonSink);
// The WU scanner shares the union (one job at a time); it adds nothing while it is the smaller
constexpr size_t METAR_BUF = METAR_BUF0 > sizeof(WufcSink) ? METAR_BUF0 : sizeof(WufcSink);
// The lookup body may be 32 KB (it streams: reading time only, the 10-s cap holds); a cut
// is reported ("partial": the nearest five of what was read)
constexpr size_t METAR_BODY_FETCH = 2048, METAR_BODY_LOOKUP = 32768;
// The TAF cycle's body cap (stream mode: nothing of it is held, the 192-B line sink is the only buffer); three
// TAFs are <= 3 x 608 B typical, the largest seen 3 061 B -- a body over the cap is too_large, never cut
constexpr size_t METAR_BODY_TAF = 3072;
#endif

// The big buffers (≈ 3 KB) come from the heap with the net-out task (at boot or at the first enable / test) and are
// never freed (with forwarding off only the targets and the scheduler cost RAM).
struct FwdBufs {
  Job job;
  Scratch s;
  char test_body[81];
  char test_target[FWD_TARGET_MAX + 1];  // the redacted target (as tools/fwd_reference.py's dry run)
#if FWD_TLS
  alignas(8) unsigned char metar[METAR_BUF];  // a METAR job's sink (MetarFetchBuf or AirportJsonSink), placement new
#endif
};
static FwdBufs* gB = nullptr;  // set once by the loop (fwdTick); every user runs only after it exists

// ---------------------------------------------------------------- NVS (fw<n> = target JSON without the key, fk<n> = key)
static void nvsName(char out[8], char kind, int slot) { snprintf(out, 8, "f%c%d", kind, slot & 7); }

static bool loadKey(int slot, char* out, size_t cap) {
  char k[8];
  nvsName(k, 'k', slot);
  const size_t n = nvsGetBlob(k, out, cap - 1);
  if (!n || n >= cap) {
    out[0] = 0;
    return false;
  }
  out[n] = 0;
  return true;
}
static bool hasKey(int slot) {
  char k[8], b[FWD_KEY_MAX + 1];
  nvsName(k, 'k', slot);
  const size_t n = nvsGetBlob(k, b, sizeof b);
  memset(b, 0, sizeof b);
  return n > 0;
}

struct StrSink {
  char* p;
  size_t cap, n;
};
static bool toSink(void* ctx, const char* d, size_t n) {
  StrSink* s = (StrSink*)ctx;
  if (s->n + n >= s->cap) return false;
  memcpy(s->p + s->n, d, n);
  s->n += n;
  s->p[s->n] = 0;
  return true;
}

static bool saveTarget(int slot, const FwdTarget& t) {  // httpd task (its 8 KB stack)
  char buf[FWD_NVS_JSON_MAX + 64];
  StrSink s{buf, sizeof buf, 0};
  buf[0] = 0;
  char tmp[64];
  Out o(toSink, &s, tmp, sizeof tmp);
  fwdToJson(o, t);
  if (!o.flush()) return false;
  char k[8];
  nvsName(k, 'w', slot);
  return nvsPutBlob(k, buf, s.n);
}

static bool startNetOut(const char* when);

// After fwdBegin (adopt.h): the targets that sent `from`'s readings now send `to`'s (re-saved), then the journal goes:
// the last step of the move (a cut before it replays the whole idempotent move at the next boot)
void adoptBootFwd(bool rest_ok) {
  {  // a journal given up this boot (three boots did not finish it): one line with where the move left things (settings,
     // rings, the forwarding slots still on the old id), all left as they are for the owner to finish by hand
    char from[STATION_ID_LEN], to[STATION_ID_LEN];
    if (adoptGaveUp(from, to)) {
      uint32_t slots = 0;
      for (int i = 0; i < FWD_SLOTS; i++)
        if (gFwd[i].used && strncmp(gFwd[i].station, from, sizeof gFwd[i].station) == 0) slots |= 1u << i;
      char t[160];
      adoptGiveUpText(t, sizeof t, from, to, stationKeysStored(from), stationKeysStored(to), storeOwns(from), storeOwns(to),
                      slots);
      logl(Log::Boot, "ADMIN", "adopt %s -> %s %s", from, to, t);
    }
  }
  AdoptJournal j;
  if (!adoptJournalRead(&j)) return;
  if (!rest_ok) {  // the earlier steps did not finish: nothing renamed here, the journal stays
    logf("ADMIN", "adopt %s -> %s: an earlier step failed; forwarding targets untouched (the move repeats at the next boot)",
         j.from, j.to);
    memset(j.key, 0, sizeof j.key);
    return;
  }
  uint32_t mask = 0;
  const int n = fwdRenameStation(gFwd, FWD_SLOTS, j.from, j.to, &mask);
  bool ok = true;
  for (int i = 0; i < FWD_SLOTS; i++)
    if ((mask >> i) & 1u) ok = saveTarget(i, gFwd[i]) && ok;
  if (ok && rest_ok) adoptJournalRemove();
  logf("ADMIN", "adopt %s -> %s: %d forwarding target(s) renamed; %s", j.from, j.to, n,
       ok && rest_ok ? "done" : "a step failed (the move repeats at the next boot)");
  memset(j.key, 0, sizeof j.key);
}

void fwdBegin() {  // setup(), the loop task's stack
  char buf[FWD_NVS_JSON_MAX + 64];
  int n = 0;
  for (int i = 0; i < FWD_SLOTS; i++) {
    char k[8];
    nvsName(k, 'w', i);
    const size_t len = nvsGetBlob(k, buf, sizeof buf - 1);
    memset(&gFwd[i], 0, sizeof gFwd[i]);
    if (!len || len >= sizeof buf) continue;
    buf[len] = 0;
    Errors e;
    fwdLoadStored(buf, len, &gFwd[i], &e);  // a record written before "https" existed gets FWD_LEGACY_HTTPS
    if (gFwd[i].used) n++;
    if (e.n) logf("FWD", "target %d loaded with %d error(s): %s", i, e.n, e.at(0));
  }
  uint32_t used = 0, free_e = 0;
  nvsStats(&used, &free_e);
  logl(Log::Boot, "FWD", "%d forwarding target(s) from NVS, switch %s; NVS %lu used, %lu free entries", n,
       gBridge.forwarding ? "on" : "off", (unsigned long)used, (unsigned long)free_e);
  bool any = false;
  for (const FwdTarget& t : gFwd) any |= t.used && t.enabled;
#if FWD_TLS
  // The METAR settings (a v1 blob; a later version migrates it, never resets it); the readings start empty (RAM only)
  metarCfgDefault(&gMetarCfg);
  MetarCfg mc;
  const size_t ml = nvsGetBlob("metar", &mc, sizeof mc);
  if (ml == sizeof mc && metarCfgValid(mc)) {
    metarCfgShortNames(&mc);  // names stored before the short-name rule are shortened in RAM only (no NVS write)
    gMetarCfg = mc;
  }
  else if (ml) logf("METAR", "settings blob not v1 (%u B): defaults", (unsigned)ml);
  memset(&gMetarStore, 0, sizeof gMetarStore);
  gMetar = MetarSched(esp_random() % 60001u);  // a per-boot phase: no fleet at :00
  logl(Log::Boot, "METAR", "airport weather %s, %u airport(s)", gMetarCfg.on ? "on" : "off", (unsigned)gMetarCfg.n);
  // The WU forecast settings (blob v1) and whether a key is stored (the key itself stays in NVS)
  {
    uint8_t wb[8] = {};
    const size_t wl = nvsGetBlob("wufc", wb, sizeof wb);
    if (wl && !wufcCfgLoad(wb, wl, &gWufcCfg)) logf("WUFC", "settings blob not v1 (%u B): defaults", (unsigned)wl);
    if (!wl) wufcCfgDefault(&gWufcCfg);
    char kb[WUFC_KEY_MAX + 1];
    gWufcKeySet = nvsGetBlob("wk", kb, sizeof kb) > 0;
    memset(kb, 0, sizeof kb);
    memset(&gWufcStore, 0, sizeof gWufcStore);
    gWufc = WufcSched(esp_random() % (WUFC_PHASE_MAX_MS + 1));
    logf("WUFC", "forecast %s, key %s", gWufcCfg.on ? "on" : "off", gWufcKeySet ? "stored" : "none");  // not one of the boot head's facts
  }
  const bool metarAny = (gMetarCfg.on && gMetarCfg.n) || (gWufcCfg.on && gWufcKeySet);
#else
  const bool metarAny = false;
#endif
  // before Wi-Fi and MQTT take the heap apart; only for an enabled target: a test on a disabled one
  // starts the task lazily in fwdTick
  if ((gBridge.forwarding && any) || metarAny) startNetOut("at boot");
}

// ---------------------------------------------------------------- the net-out task
#if FWD_TLS
static uint16_t rootsFor(FwdType t) {
  switch (t) {
    // the root + the cross-signed certificate the server sends, as a second anchor: the chain may end one step earlier
    case FwdType::Wu:
    case FwdType::PwsWeather: return FWD_ROOTBIT_STARFIELD_G2 | FWD_ROOTBIT_AMAZON_CA1_X;
    case FwdType::Windy: return FWD_ROOTBIT_GLOBALSIGN | FWD_ROOTBIT_GTS_R1_X;
    case FwdType::WowBe: return FWD_ROOTBIT_HARICA_2015;
    default: return FWD_ROOTS_ALL;  // a custom HTTPS host must chain to one of our anchors
  }
}
#endif

static void sampleHeap(Job& j) {  // the send's low-water on the byte-addressable heap, after each step
  const HeapNow h = heapNow();  // the one heap reader
  BENCH(if (h.free8 < gSendHeapMin.load()) gSendHeapMin = h.free8;)
  BENCH(if (h.largest8 < gSendLargestMin.load()) gSendLargestMin = h.largest8;)
  if (h.largest8 < j.d.largest8_min) j.d.largest8_min = h.largest8;  // keep: last.diag.largest8_min
}

static bool stepOk(Job& j) {
  sampleHeap(j);
  if (gAbort.load()) {
    j.why = gAbortWhy ? gAbortWhy : "aborted";
    return false;
  }
  if (millis() - j.t0 >= FWD_SEND_MAX_MS) {
    j.why = "deadline";
    return false;
  }
  return true;
}

static void maskBody(Job& j, const FwdResp& r) {
  j.body[0] = 0;
  if (j.type != FwdType::WuCompat && j.type != FwdType::Template) return;  // never a body for presets
  if (j.result == FwdResult::Ok || !r.bodyLen()) return;
  char b[FwdResp::CAP_BODY + 1];
  size_t n = 0;
  for (size_t i = 0; i < r.bodyLen() && n < sizeof b - 1; i++) {
    const char c = r.body()[i];
    b[n++] = (c >= 0x20 && c < 0x7F) ? c : ' ';
  }
  b[n] = 0;
  for (const char* secret : {(const char*)j.key, (const char*)j.id}) {  // id / key echoed back: masked
    const size_t sl = strlen(secret);
    if (sl < 3) continue;
    for (char* p = strstr(b, secret); p; p = strstr(p + 3, secret)) {
      memcpy(p, "***", 3);
      memmove(p + 3, p + sl, strlen(p + sl) + 1);
    }
  }
  snprintf(j.body, sizeof j.body, "%.80s", b);
}

// The lowest byte-addressable free heap since heapSample last restarted the allocator's local monitor; it does not
// restart it while a send runs, so after the job this spans all of it.
static uint32_t min8Now() { return heapNow().min8; }
static uint16_t ms16(uint32_t ms) { return (uint16_t)(ms > 65535 ? 65535 : ms); }

// Connect within what is left of the 10-s send, 1.5 s kept for write + read (plain 3 s). The handshake (FWD_TLS) runs
// after it under the same 10 s, checked in its I/O callbacks.
static uint32_t connectCap(const Job& j) {
  const uint32_t left = FWD_SEND_MAX_MS - (millis() - j.t0), room = left > 1500 ? left - 1500 : 0, cap = 3000;
  return room < cap ? room : cap;
}

// The socket layer, one for both variants: DNS, a non-blocking connect within connectCap, send / receive
// timeouts. Plain HTTP runs on it as it is; TLS (FWD_TLS=1) runs BearSSL on top through TlscIo.
struct Conn {
  int fd = -1;
};

static bool connOpen(Job& j, Conn& c) {
  const uint32_t cap = connectCap(j), d0 = millis();
  struct addrinfo hints = {};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  struct addrinfo* res = nullptr;
  const int gr = getaddrinfo(j.host, nullptr, &hints, &res);
  j.d.ms_dns = ms16(millis() - d0);
  if (gr != 0 || !res) {
    if (res) freeaddrinfo(res);
    j.d.sock_err = (int16_t)gr;
    j.why = "dns";
    return false;
  }
  struct sockaddr_in a;
  memcpy(&a, res->ai_addr, sizeof a);
  freeaddrinfo(res);
  a.sin_port = htons(j.port);
  const uint32_t t0 = millis();
  int err = -1;  // -1: stopped by stepOk (its why stands)
  c.fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (c.fd < 0) {
    err = errno ? errno : ENOMEM;
  } else if (stepOk(j)) {
    const int fl = fcntl(c.fd, F_GETFL, 0);
    fcntl(c.fd, F_SETFL, fl | O_NONBLOCK);
    err = connect(c.fd, (struct sockaddr*)&a, sizeof a) == 0 ? 0 : errno;
    if (err == EINPROGRESS) {  // wait for it within the cap
      fd_set w;
      FD_ZERO(&w);
      FD_SET(c.fd, &w);
      struct timeval tv = {(time_t)(cap / 1000), (suseconds_t)((cap % 1000) * 1000)};
      const int s = select(c.fd + 1, nullptr, &w, nullptr, &tv);
      socklen_t len = sizeof err;
      err = ETIMEDOUT;
      if (s == 1 && getsockopt(c.fd, SOL_SOCKET, SO_ERROR, &err, &len) != 0) err = errno;
    }
    fcntl(c.fd, F_SETFL, fl);
    if (!err) {
      struct timeval tv = {(time_t)(cap / 1000), (suseconds_t)((cap % 1000) * 1000)};
      setsockopt(c.fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    }
  }
  j.d.ms_tcp = ms16(millis() - t0);
  if (err == 0) return true;
  if (err != -1) {
    j.d.sock_err = (int16_t)err;
    j.why = "connect";
  }
  return false;
}
static ssize_t connWrite(Conn& c, const char* b, size_t n) { return send(c.fd, b, n, 0); }
// > 0 bytes, 0 closed, -1 an error or the timeout
static ssize_t connRead(Conn& c, char* b, size_t n, uint32_t to_ms) {
  struct timeval tv = {(time_t)(to_ms / 1000), (suseconds_t)((to_ms % 1000) * 1000)};
  setsockopt(c.fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
  const ssize_t r = recv(c.fd, b, n, 0);
  return r < 0 ? -1 : r;
}
static void connClose(Conn& c) {
  if (c.fd >= 0) close(c.fd);
  c.fd = -1;
}

// Plain HTTP: the request, then the response through FwdResp (each read <= 5 s, all <= 10 s)
__attribute__((noinline)) static void plainExchange(Job& j, Conn& c) {  // its 512-B buffer stays out of the TLS frame
  {
    size_t off = 0;
    while (off < j.req_len) {
      const ssize_t w = connWrite(c, j.req + off, j.req_len - off);
      if (w <= 0) {
        j.why = "write";
        return;
      }
      off += (size_t)w;
      if (!stepOk(j)) return;
    }
  }
  memset(j.req, 0, sizeof j.req);  // the key leaves RAM as soon as it is on the wire
  j.sent = true;
  gB->s.resp.begin(j.type, j.success);
  char buf[512];
  while (!gB->s.resp.done()) {
    if (!stepOk(j)) break;
    const uint32_t left = FWD_SEND_MAX_MS - (millis() - j.t0);
    const ssize_t r = connRead(c, buf, sizeof buf, left < 5000 ? left : 5000);
    if (r <= 0) {  // closed (0) or an error / timeout
      if (r < 0 && gB->s.resp.status() < 0) j.why = "read";
      break;
    }
    gB->s.resp.feed(buf, (size_t)r);
  }
  memset(buf, 0, sizeof buf);  // a WOW-BE body (coordinates) never stays on the stack
  if (!gB->s.resp.done() && !j.why) gB->s.resp.end();
}

#if FWD_TLS
// TLS over the socket: BearSSL in src/tlsc, these callbacks keep the 10-s send and the abort flag
struct TlsIoCtx {
  Job* j;
  Conn* c;
  bool aborted;
};
static int tlsRead(void* ctx, uint8_t* b, size_t n) {
  TlsIoCtx* x = (TlsIoCtx*)ctx;
  if (!stepOk(*x->j)) {
    x->aborted = true;
    return -1;
  }
  const uint32_t left = FWD_SEND_MAX_MS - (millis() - x->j->t0);
  const ssize_t r = connRead(*x->c, (char*)b, n, left < 5000 ? left : 5000);
  return r <= 0 ? -1 : (int)r;
}
static int tlsWrite(void* ctx, const uint8_t* b, size_t n) {
  TlsIoCtx* x = (TlsIoCtx*)ctx;
  if (!stepOk(*x->j)) {
    x->aborted = true;
    return -1;
  }
  const ssize_t w = connWrite(*x->c, (const char*)b, n);
  return w <= 0 ? -1 : (int)w;
}
static void tlsEntropy(void* b, size_t n) { esp_fill_random(b, n); }  // Wi-Fi is up: the hardware RNG is a true one
static uint32_t tlsNowMs() { return millis(); }

// One METAR line from the stream (net-out task): parsed at once into the job block (the store is the loop's)
static void metarLine(void* ctx, const char* line, size_t n, bool) {
  MetarFetchBuf* f = (MetarFetchBuf*)ctx;
  MetarObs o;
  const MetarResult r = metarParse(line, n, f->wantp, f->nwant, f->now, o);
  f->field_bad = (uint8_t)(f->field_bad + r.field_bad > 255 ? 255 : f->field_bad + r.field_bad);
  if (r.why == MetarWhy::Ok) {
    metarKeep(f->got, f->ngot, METAR_AIRPORTS_MAX, o);  // the same airport twice: the newer report (or a COR) wins
  } else if (r.why == MetarWhy::Unwanted) {
    if (f->unwanted < 255) f->unwanted++;
  } else if (r.why == MetarWhy::Nil) {  // a station's NIL is no fault of the line
    if (f->nil < 255) f->nil++;
  } else if (r.why == MetarWhy::Time) {  // an old report of a dead station, or a clock issue: counted, not sampled
    if (f->old < 255) f->old++;
  } else if (r.why != MetarWhy::Empty) {
    if (f->garbage < 255) f->garbage++;
    size_t k = 0;
    for (; k < n && k < sizeof f->dropped - 1; k++) f->dropped[k] = line[k] >= 0x20 && line[k] < 0x7F ? line[k] : ' ';
    f->dropped[k] = 0;
  }
}

__attribute__((noinline)) static void tlsExchange(Job& j, Conn& c) {
  TlscReq q = {};
  q.host = j.host;
  q.roots = j.roots;  // tlsc resolves the anchors (no BearSSL type out here)
  q.unix_now = j.unix_now;
  q.buf_len = TLSC_BUF_LEN;
  q.read_cap = FwdResp::CAP_TOTAL;
  q.entropy = tlsEntropy;
  q.now_ms = tlsNowMs;
  TlsIoCtx x{&j, &c, false};
  const TlscIo io{&x, tlsRead, tlsWrite};
  gB->s.resp.begin(j.type, j.success);
  if (j.kind == FwdKind::Lookup) {  // the stationinfo JSON into the airport sink, the reports into the line sink
    gB->s.resp.stream(AirportJsonSink::sinkFn, gB->metar, METAR_BODY_LOOKUP);
    q.read_cap = FwdResp::CAP_LINE + 2 + FwdResp::CAP_HEAD + METAR_BODY_LOOKUP;
  } else if (wufcKind(j.kind)) {  // the 5-day JSON into the scanner, nothing of it held
    gB->s.resp.stream(WufcSink::sinkFn, gB->metar, WUFC_BODY_CAP);
    q.read_cap = FwdResp::CAP_LINE + 2 + FwdResp::CAP_HEAD + WUFC_BODY_CAP;
  } else if (metarKind(j.kind)) {
    const size_t cap = ((MetarFetchBuf*)gB->metar)->taf ? METAR_BODY_TAF : METAR_BODY_FETCH;
    gB->s.resp.stream(MetarLineSink::sinkFn, &((MetarFetchBuf*)gB->metar)->sink, cap);
    q.read_cap = FwdResp::CAP_LINE + 2 + FwdResp::CAP_HEAD + cap;
  }
  TlscDiag td;
  const TlscResult r = tlscGet(q, io, j.req, j.req_len, gB->s.resp, td);
  if (j.kind == FwdKind::Fetch || j.kind == FwdKind::RunOnce) {  // the last line (AWC ends the body without '\n')
    // only after a clean end: a body cut at the cap, a reset, an abort or a short read leaves a part
    // line that would parse as a report with fewer clouds and no ceiling -- dropped, never a half report
    MetarLineSink& k = ((MetarFetchBuf*)gB->metar)->sink;
    const FwdResp& rs = gB->s.resp;
    if (metarCleanEnd(r.response && !x.aborted, rs.done(), rs.cut(), rs.result() == FwdResult::Fail)) {
      k.finish();
      if (((MetarFetchBuf*)gB->metar)->taf) ((MetarFetchBuf*)gB->metar)->tp.finish();  // the last TAF block
    } else {
      k.drop();  // an open TAF block is never committed (the forecasts already stored stay)
    }
  }
  memset(j.req, 0, sizeof j.req);  // the request (with the key) is gone; tlsc wiped its own copy in the record buffer
  j.d.stage = (uint8_t)td.stage;
  j.d.br_err = (int16_t)td.br_err;
  j.d.version = td.version;
  j.d.curve = td.curve;
  j.d.suite = td.suite;
  j.d.rec_in_max = td.rec_in_max;
  j.d.rec_app_max = td.rec_app_max;  // the data records apart from the handshake's
  j.d.ms_hs = ms16(td.ms_hs);
  j.sent = td.flushed;
  if (r.response) return;  // the response decides (below, from FwdResp)
  const TlscWhy w = x.aborted ? TlscWhy::Aborted : r.why;
  j.tls_why = w;
  if (!j.why) j.why = w == TlscWhy::Untrusted ? "tls_untrusted" : tlscWhyName(w);
}
#endif

static void runJob(Job& j) {
  j.result = FwdResult::Fail;
  j.status = -1;
  j.retry_after = 0;
  j.why = nullptr;
  j.sent = false;
  j.body[0] = 0;
  memset(&j.d, 0, sizeof j.d);
  j.d.largest8_min = UINT32_MAX;
#if FWD_TLS
  j.tls_why = TlscWhy::Ok;
#endif
  Conn c;
  bool started = false;
  if (!stepOk(j)) goto done;
  {
    const HeapNow h = heapNow();
    j.d.pre8 = h.free8;
    j.d.largest8_pre = h.largest8;
  }
#if FWD_TLS
  if (j.tls && !j.unix_now) {  // `clock` without touching the network
    j.why = "clock";
    goto done;
  }
#endif
  if (!connOpen(j, c) || !stepOk(j)) goto done;
  started = true;
#if FWD_TLS
  if (j.tls) tlsExchange(j, c);
  else
#endif
    plainExchange(j, c);
  if (gB->s.resp.done()) {
    j.result = gB->s.resp.result();
    j.status = gB->s.resp.status();
    j.retry_after = gB->s.resp.retryAfter();
  } else {
#if FWD_TLS
    if (j.tls && j.tls_why != TlscWhy::Ok) j.result = fwdTlsResult(j.tls_why);
#endif
    if (gB->s.resp.status() >= 0) j.status = gB->s.resp.status();  // aborted or timed out with a status in hand
  }
  maskBody(j, gB->s.resp);
done:
  (void)started;
  connClose(c);
  sampleHeap(j);
  j.d.min8 = min8Now();
  if (j.d.largest8_min == UINT32_MAX) j.d.largest8_min = 0;
  memset(j.req, 0, sizeof j.req);
  memset(j.key, 0, sizeof j.key);
  j.ms = millis() - j.t0;
  j.d.ms_total = ms16(j.ms);
}

// The send's log line, written here on the net-out task before J_DONE. On the loop tick (finishResult) the
// ~250-B line would block the loop task for ~11 ms (UART at 115 200 Bd, no TX buffer: the write waits for the FIFO);
// here it delays only the job's end.
static void logJob(const Job& j) {
  const bool m = j.kind == FwdKind::Fetch || j.kind == FwdKind::RunOnce || j.kind == FwdKind::Lookup;
  const bool w = j.kind == FwdKind::WuFetch || j.kind == FwdKind::WuRun;  // no wufcKind in FWD_TLS=0
  // A test, a run once, a lookup or a Fetch now is an event. A scheduled job (a send, its retry, the METAR / TAF
  // fetch, the WU pull) that worked is chatter, unless it ends a spell of failures; its failures go through the
  // source's FailSpell: the first an event, the repeats chatter with an hourly count
  const bool scheduled = j.kind == FwdKind::Send || j.kind == FwdKind::Retry || j.kind == FwdKind::Fetch || j.kind == FwdKind::WuFetch;
  // the targets, METAR, the WU pull. Written by the net-out task, or by the loop tick for a send that could not render;
  // never both at once (one job on the line)
  static FailSpell spell[FWD_SLOTS + 2];
  Log lvl = Log::Event;
  char sx[32];
  const char* rep = "";
  const int src = m ? FWD_SLOTS : w ? FWD_SLOTS + 1 : j.slot;
  if (scheduled && src >= 0 && src < FWD_SLOTS + 2) {
    FailSpell& f = spell[src];
    const uint32_t now = millis();
    uint32_t n = 1, min = 0;
    if (j.result == FwdResult::Ok || j.result == FwdResult::Dup) lvl = f.ok(now) ? Log::Event : Log::Chatter;
    else if (f.fail(now, &n, &min)) rep = rateSuffix(sx, sizeof sx, n, min);
    else lvl = Log::Chatter;
  }
  logl(lvl, "FWD", "slot %d %s %s%s: status %d, %lu ms, %s%s%s%s; heap8 %lu/%lu before, low %lu/%lu; dns %u tcp %u hs %u ms; br %d "
       "rec %u; stack %u", j.slot, m ? "metar" : w ? "wufc" : fwdTypeName(j.type), j.tls ? "https" : "http",
       j.kind == FwdKind::Test      ? " (test)"
       : j.kind == FwdKind::Retry   ? " (retry)"
       : j.kind == FwdKind::RunOnce ? " (run once)"
       : j.kind == FwdKind::Lookup  ? " (lookup)"
       : j.kind == FwdKind::WuRun   ? " (fetch now)"
                                    : "",
       j.status, (unsigned long)j.ms, fwdResultName(j.result), j.why ? " " : "", j.why ? j.why : "", rep,
       (unsigned long)j.d.pre8, (unsigned long)j.d.largest8_pre, (unsigned long)j.d.min8, (unsigned long)j.d.largest8_min,
       (unsigned)j.d.ms_dns, (unsigned)j.d.ms_tcp, (unsigned)j.d.ms_hs, (int)j.d.br_err, (unsigned)j.d.rec_in_max,
       (unsigned)j.d.stack_free);
}

static void netTask(void*) {
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    uint8_t want = J_POSTED;
    if (!gJobSt.compare_exchange_strong(want, J_RUNNING)) continue;
    gSending = true;
    gSentSinceSample = true;
    runJob(gB->job);
    gSending = false;
    gSentSinceSample = true;  // the first sample after the end still holds the dip in its local minimum
    gNetStackFree = uxTaskGetStackHighWaterMark(nullptr);
    gB->job.d.stack_free = (uint16_t)(gNetStackFree > 65535 ? 65535 : gNetStackFree);  // the send's own depth
    logJob(gB->job);
    gNetStackFree = uxTaskGetStackHighWaterMark(nullptr);  // the task's, the log line included
    gJobSt = J_DONE;
  }
}

// ---------------------------------------------------------------- heap_low during a send
void fwdHeapState(bool* running, bool* ran_since, uint32_t* pre_send) {
  *running = gSending.load();
  *ran_since = gSentSinceSample.exchange(false) || *running;
  *pre_send = gPreSendFree.load();
}
bool fwdIdle() {
  const uint8_t s = gJobSt.load();
  return s != J_POSTED && s != J_RUNNING;
}

// The buffers and the task: only with the plain-HTTP floor; a failure logs once and is tried
// again once a minute. Kept until restart.
static bool startNetOut(const char* when) {
  static uint32_t lastTryMs = 0;
  static bool failed = false;
  const uint32_t ms = millis();
  if (gNetTask) return true;
  if (lastTryMs && ms - lastTryMs < 60000) return false;
  lastTryMs = ms | 1;
  const HeapNow h = heapNow();
  const uint32_t fr = h.free8, lg = h.largest8;
  if (fwdResourcesOk(false, FwdTlsClass::Custom, fr, lg)) {
    if (!gB) gB = (FwdBufs*)calloc(1, sizeof *gB);
    if (gB && xTaskCreate(netTask, "netout", NETOUT_STACK, nullptr, 1, &gNetTask) != pdPASS) gNetTask = nullptr;
  }
  if (!gNetTask) {
    if (!failed) logf("FWD", "net-out not started (heap8 %lu, largest %lu); tried again once a minute", (unsigned long)fr,
                      (unsigned long)lg);
    failed = true;
    return false;
  }
  logl(Log::Boot, "FWD", "net-out up %s (%lu B stack + %u B buffers)%s, heap8 %lu", when, (unsigned long)NETOUT_STACK,
       (unsigned)sizeof *gB, failed ? " after a failed start" : "",
       (unsigned long)heapNow().free8);
  failed = false;
  lastTryMs = 0;
  return true;
}
#if FC_DIAG_BENCH
void fwdNoteHeapLow(uint32_t low) {
  if (low < gSendHeapMin.load()) gSendHeapMin = low;
}
#endif

// ---------------------------------------------------------------- the loop tick
static uint32_t gShedSeen = 0, gShedMs = 0;
static FwdHourGuard gHour;
#if FWD_TLS
// ---------------------------------------------------------------- the REL hint
// In the METAR job after the readings land, under gLock: per station, the 3-h silence, then one step when the reference
// has a report not stepped yet. Never per upload; no heap, no NVS.
static void relResetAll() {
  for (int i = 0; i < MAX_STATIONS; i++)
    if (Station* st = gStations.at(i)) relReset(&st->rel);
}

static void relAfterReports(uint32_t now) {
  if (!now) return;
  // a moved location or no airport within 60 km any more ends the run (the airport set's change is
  // reset in the admin PUT)
  static float sLat = NAN, sLon = NAN;
  const bool moved = !(gBridge.lat == sLat && gBridge.lon == sLon);
  sLat = gBridge.lat, sLon = gBridge.lon;
  if (moved || !metarEligible(gMetarCfg, gBridge.lat, gBridge.lon)) relResetAll();
  int16_t qnh = 0;
  uint32_t ref_t = 0;
  const bool ref = relReference(gMetarCfg, gMetarStore, gBridge.lat, gBridge.lon, now, REL_SAMPLE_S, &qnh, &ref_t);
  for (int i = 0; i < MAX_STATIONS; i++) {
    Station* st = gStations.at(i);
    if (!st) continue;
    if (st->baro_uncal) {
      relReset(&st->rel);
      continue;
    }
    relExpire(&st->rel, now);
    if (!ref || ref_t <= st->rel.seen_t) continue;
    float p;
    if (relStationValue(*st, gBridge.lat, gBridge.lon, ref_t, &p)) relStep(&st->rel, (int16_t)lroundf(p * 10 - qnh), qnh, ref_t);
    else relSeen(&st->rel, ref_t);
  }
}
#endif

#if FWD_TLS
// How a WU job ended -- the network's verdict first, then the scanner's on a 2xx
static WufcOutcome wufcOutcomeOf(const Job& j, const WufcSink& k, WufcData* d) {
  const FwdResp& rs = gB->s.resp;
  if (j.why && !strcmp(j.why, "aborted_yield")) return WufcOutcome::Yield;
  if (j.result == FwdResult::TlsUntrusted) return WufcOutcome::Untrusted;
  if (j.result == FwdResult::NoMemory) return WufcOutcome::NoMemory;
  if (j.why && !strcmp(j.why, "too_large")) return WufcOutcome::TooLarge;
  if (j.status == 429) return WufcOutcome::Limited;
  if (j.status == 401 || j.status == 403) return WufcOutcome::Refused;
  if (j.status == 406) return WufcOutcome::Encoding;
  if (j.status == 204) return WufcOutcome::NoData;
  if (j.status >= 200 && j.status <= 299 && rs.done()) {
    switch (k.finish(rs.cut(), j.unix_now, d)) {
      case WufcParse::Ok: return WufcOutcome::Ok;
      case WufcParse::NoData: return WufcOutcome::NoData;
      case WufcParse::Encoding: return WufcOutcome::Encoding;
      case WufcParse::TooLarge: return WufcOutcome::TooLarge;
      default: return WufcOutcome::Bad;
    }
  }
  if (j.status < 0 || j.status >= 500 || !rs.done()) return WufcOutcome::Fail;
  return WufcOutcome::Bad;  // another 4xx
}

static void finishWufc(Job& j, uint32_t now) {
  WufcSink* k = (WufcSink*)gB->metar;
  WufcData d = {};
  const WufcOutcome out = wufcOutcomeOf(j, *k, &d);
  // one gLock hold from the epoch check to the last write (gFwdMux inside it): a save bumps the epoch under gLock before
  // its keyChanged(), so either this answer sees the new epoch, or the save's keyChanged() lifts what this answer set
  xSemaphoreTake(gLock, portMAX_DELAY);
  const bool same = gWufcEpoch == gWufcJobEpoch;  // the settings did not change while it ran
  portENTER_CRITICAL(&gFwdMux);
  gWufc.result(j.kind, same ? out : WufcOutcome::Off, millis(), j.retry_after);  // an answer to replaced settings never pauses
  const bool keyPause = same && gWufc.pause() == WufcPause::Key;
  WufcLastJob& x = gWufcLast;  // the last job as it went (what WU said), fenced or not
  x.t = now;
  x.out = out;
  x.status = (int16_t)j.status;
  snprintf(x.why, sizeof x.why, "%s", j.why ? j.why : "");
  x.d = j.d;
  gWufcRan = true;
  if (j.kind == FwdKind::WuRun) gWuRun.state = 3, gWuRunT = now, snprintf(gWuRun.result, sizeof gWuRun.result, "%s", wufcOutcomeName(out));
  portEXIT_CRITICAL(&gFwdMux);
  if (same && out == WufcOutcome::Ok) wufcPut(&gWufcStore, d, j.unix_now);
  else if (same && out == WufcOutcome::NoData) wufcDrop(&gWufcStore);  // WU has no line for tomorrow: none shown
  if (keyPause) wufcDrop(&gWufcStore);  // a refused-key pause takes the line away at once
  // an accepted "Fetch now" (WU answered with this key) verifies it; the httpd task saves it
  if (same && j.kind == FwdKind::WuRun && (out == WufcOutcome::Ok || out == WufcOutcome::NoData) && !gWufcCfg.verified) {
    gWufcCfg.verified = 1;
    gWufcSave = true;
  }
  xSemaphoreGive(gLock);
  k->~WufcSink();
  gAbort = false;
  gAbortWhy = nullptr;
  gJobSt = J_IDLE;
}
#endif

static void finishResult() {
  Job& j = gB->job;
  const uint32_t now = nowS();
  BENCH(if (j.ms > gSendMsMax.load()) gSendMsMax = j.ms;)
#if FWD_TLS
  if (wufcKind(j.kind)) {
    finishWufc(j, now);
    return;
  }
  if (metarKind(j.kind)) {  // the readings into the store (RAM), the lookup's list, the source's outcome
    MetarOutcome out;
    const bool lookup = j.kind == FwdKind::Lookup;
    MetarFetchBuf* f = (MetarFetchBuf*)gB->metar;
    AirportJsonSink* l = (AirportJsonSink*)gB->metar;
    if (j.why && !strcmp(j.why, "aborted_yield")) out = MetarOutcome::Yield;
    else if (j.result == FwdResult::TlsUntrusted) out = MetarOutcome::Untrusted;
    else if (j.result == FwdResult::NoMemory) out = MetarOutcome::NoMemory;
    else if (j.why && !strcmp(j.why, "too_large")) out = MetarOutcome::TooLarge;
    else if (j.status == 429) out = MetarOutcome::Limited;
    else if (j.status >= 200 && j.status <= 299 && gB->s.resp.done())
      out = lookup ? MetarOutcome::Ok : (f->taf ? f->tp.count() : f->ngot) ? MetarOutcome::Ok : MetarOutcome::NoData;
    else if (j.status < 0 || j.status >= 500 || !gB->s.resp.done()) out = MetarOutcome::Fail;
    else out = MetarOutcome::Reject;
    if (!lookup) {
      xSemaphoreTake(gLock, portMAX_DELAY);
      if (f->taf) {  // the hour's forecasts: absent markers, the time only on ok / nodata
        metarStoreTafCycle(&gMetarStore, gMetarCfg, out, f->tp.all(), f->tp.count(), now, j.unix_now - j.unix_now % 3600u,
                           f->wantp, f->nwant);
      } else {
        if (out == MetarOutcome::Ok) {  // a failed job stores nothing (its lines may end in a part line)
          for (int i = 0; i < f->ngot; i++) metarStorePut(&gMetarStore, gMetarCfg, f->got[i]);
          relAfterReports(j.unix_now);  // one REL step per station per new reference report
        }
        gMetarStore.last_t = now;
        gMetarStore.last = out;
      }
      if (f->dropped[0]) memcpy(gMetarStore.dropped, f->dropped, sizeof gMetarStore.dropped);
      xSemaphoreGive(gLock);
    }
    portENTER_CRITICAL(&gFwdMux);
    gMetar.result(j.kind, out, millis(), j.retry_after);
    MetarLastJob& x = gMetarLast;
    x.t = now;
    x.kind = j.kind;
    x.out = out;
    x.status = (int16_t)j.status;
    snprintf(x.why, sizeof x.why, "%s", j.why ? j.why : "");
    x.d = j.d;
    BENCH(x.http_idle_ms = gMetarIdle0; x.http_during = gMetarDuring;)
    x.taf = !lookup && f->taf;
    x.lines = lookup ? (uint8_t)(l->seen() > 255 ? 255 : l->seen()) : x.taf ? (uint8_t)f->tp.count() : f->ngot;
    x.garbage = lookup ? (uint8_t)(l->rejected() > 255 ? 255 : l->rejected())
                : x.taf    ? (uint8_t)(f->tp.garbage() > 255 ? 255 : f->tp.garbage())
                           : f->garbage;
    x.unwanted = lookup ? (uint8_t)(l->dropped() > 255 ? 255 : l->dropped())
                 : x.taf    ? (uint8_t)(f->tp.unwanted() > 255 ? 255 : f->tp.unwanted())
                            : f->unwanted;
    x.field_bad = lookup ? 0 : f->field_bad;
    x.trunc = lookup ? (uint8_t)(gB->s.resp.cut() ? 1 : 0) : (uint8_t)(f->sink.trunc() > 255 ? 255 : f->sink.trunc());
    x.nil = lookup ? 0 : f->nil;
    x.old = lookup ? 0 : f->old;
#if FC_DIAG_BENCH
    const uint32_t idle = gMetarIdle0;
    const int b = idle < 3000 ? 0 : idle < 5000 ? 1 : idle < 10000 ? 2 : idle < 30000 ? 3 : idle < 60000 ? 4 : 5;
    if (gHttpIdleHist[b] < UINT16_MAX) gHttpIdleHist[b]++;
#endif
    if (lookup) {
      gLook.state = 3;
      gLook.n = 0;
      if (out == MetarOutcome::Ok) {
        gLook.taf_mask = 0;
        for (int i = 0; i < l->count(); i++) {
          if (l->hasTaf(i)) gLook.taf_mask |= (uint8_t)(1u << gLook.n);
          gLook.a[gLook.n++] = l->at(i);
        }
        snprintf(gLook.result, sizeof gLook.result, "%s", gB->s.resp.cut() ? "partial" : gLook.n ? "ok" : "none");
      } else {
        snprintf(gLook.result, sizeof gLook.result, "%s", metarOutcomeName(out));
      }
    } else if (j.kind == FwdKind::RunOnce) {
      gRun.state = 3;
      snprintf(gRun.result, sizeof gRun.result, "%s", metarOutcomeName(out));
    }
    portEXIT_CRITICAL(&gFwdMux);
    if (lookup && out == MetarOutcome::Ok) {  // AWC's names for the airports the user did not name
      MetarLookup lk;  // RAM here; the httpd task saves it on the sheet's next poll (no NVS write on the loop tick)
      portENTER_CRITICAL(&gFwdMux);
      lk = gLook;
      portEXIT_CRITICAL(&gFwdMux);
      xSemaphoreTake(gLock, portMAX_DELAY);
      if (metarLookupNames(&gMetarCfg, lk)) gMetarSave = true;
      xSemaphoreGive(gLock);
    }
    if (lookup) l->~AirportJsonSink();
    else f->~MetarFetchBuf();
    gAbort = false;
    gAbortWhy = nullptr;
    gJobSt = J_IDLE;
    return;
  }
#endif
  const FwdCause cause = j.kind == FwdKind::Test ? FwdCause::None : fwdCauseOf(j.result, j.status, j.why);
  // The unconfirmed rule: the request went out whole and no answer came back -- plain HTTP: fully written, then
  // no status line (a read time-out or a close); TLS: br_sslio_flush returned 0 (stage Data
  // only means "buffered in BearSSL"), then no status line
  const bool written = fwdWritten(j.sent, j.status, j.result);
  portENTER_CRITICAL(&gFwdMux);
  gSched.result(j.slot, j.result, millis(), j.retry_after, j.seen, written);
  if (cause != FwdCause::None && gCause[j.slot][(int)cause] < UINT16_MAX) gCause[j.slot][(int)cause]++;
  FwdLast& l = gLast[j.slot];
  l.t = now;
  l.status = (int16_t)j.status;
  l.result = j.result;
  l.ms = (uint16_t)(j.ms > 65535 ? 65535 : j.ms);
  snprintf(l.detail, sizeof l.detail, "%s", j.why ? j.why : "");
  l.d = j.d;
  if (j.result == FwdResult::Ok || j.result == FwdResult::Dup) l.ok_t = now;
  const uint16_t gen = gTest.gen;  // rechecked under gLock below
  const bool verify = j.kind == FwdKind::Test && gTest.slot == j.slot && gen == gVerGen[j.slot] &&
                      (j.result == FwdResult::Ok || j.result == FwdResult::Dup);
  if (j.kind == FwdKind::Test && gTest.slot == j.slot) {
    snprintf(gTest.result, sizeof gTest.result, "%s", fwdResultName(j.result));
    snprintf(gTest.why, sizeof gTest.why, "%s", j.why ? j.why : "");
    gTest.d = j.d;
    gTest.status = (int16_t)j.status;
    gTest.ms = j.ms;
    memcpy(gB->test_body, j.body, sizeof gB->test_body);
    gTest.state = 2;
  }
  portEXIT_CRITICAL(&gFwdMux);
  if (verify) {  // an accepted "Send one now" with the current settings: the switch may go on. RAM only here (the
    // loop tick neither logs nor writes flash); the httpd task saves it on the sheet's next poll (saveVerified)
    xSemaphoreTake(gLock, portMAX_DELAY);
    // A PUT bumps the generation before it writes gFwd, so a change landing between the compare above
    // and this section is seen here and verifies nothing
    portENTER_CRITICAL(&gFwdMux);
    const bool same = gen == gVerGen[j.slot];
    portEXIT_CRITICAL(&gFwdMux);
    if (same && gFwd[j.slot].used && !gFwd[j.slot].verified) gFwd[j.slot].verified = true, gVerSave |= (uint8_t)(1u << j.slot);
    xSemaphoreGive(gLock);
  }
  gAbort = false;
  gAbortWhy = nullptr;
  gJobSt = J_IDLE;
}

// Real time for a certificate check: any rung the clock ladder accepts as real -- SNTP, DHCP, the browser (never
// a persisted or a held clock) -- read once; a value under EPOCH_MIN (a hold engaging between the two reads hands out
// uptime seconds, i.e. 1970) counts as none, so the check never runs against 1970.
static uint32_t realNow() {
  if (!outlookTimeOk()) return 0;
  const uint32_t t = nowS();
  return t >= EPOCH_MIN ? t : 0;
}
#if FC_DIAG_BENCH
static uint32_t gStartLockUs = 0;  // startJob's gLock wait, counted into the tick's lock time
void StartClock::lockWait() {
  const uint32_t n = micros();
  gStartLockUs += n - t;
  t = n;
}
#endif

// The data gate's input for one target (caller holds gLock); with `live`, also the record's input (snapshot only).
static bool snapStation(const char* id, uint32_t min_s, uint32_t ok_seen, bool time_ok, FwdGateIn* g, FwdLiveIn* live) {
  memset(g, 0, sizeof *g);
  g->time_ok = time_ok;
  g->ok_seen = ok_seen;
  Station* st = id[0] ? gStations.find(id) : gStations.first();
  if (!st) return false;
  g->exists = true;
  g->online = st->online(millis());
  g->sensor_stale = st->sensor_stale;
  g->restored = (st->flash_state & FLASH_LIVE_RESTORED) && !st->uploads;
  g->last_seen = st->last_seen;
  const uint32_t drop = st->cfg.disabled | st->warming;
  auto kept = [&](int f) {
    return st->live.has(f) && !((drop >> f) & 1u) && st->field_t[f] && st->field_t[f] <= st->last_seen &&
           st->last_seen - st->field_t[f] <= min_s;
  };
  g->has_temp = kept(F_TEMP_C);
  g->has_wind = kept(F_WIND_KMH);
  if (live) {
    memcpy(live->v, st->live.v, sizeof live->v);
    live->present = st->live.present;
    live->hidden = st->cfg.disabled;
    live->warming = st->warming;
    memcpy(live->field_t, st->field_t, sizeof live->field_t);
    live->last_seen = st->last_seen;
    live->wind10 = recentFieldMean(st->recent, F_WIND_KMH, st->last_seen, 600);
  }
  return true;
}

#if FWD_TLS
static void centi(char* out, size_t cap, int32_t v) {  // -7378 -> "-73.78"
  snprintf(out, cap, "%s%ld.%02ld", v < 0 ? "-" : "", (long)((v < 0 ? -v : v) / 100), (long)((v < 0 ? -v : v) % 100));
}
static bool metarLocation(int32_t* lat, int32_t* lon) {  // the bridge's configured location (gLock held)
  const float a = gBridge.lat, b = gBridge.lon;
  if (!(a == a) || !(b == b)) return false;  // NaN: not set
  *lat = (int32_t)(a * 100.0f + (a < 0 ? -0.5f : 0.5f));
  *lon = (int32_t)(b * 100.0f + (b < 0 ? -0.5f : 0.5f));
  return true;
}
// A METAR job's request (aviationweather.gov, anchors DigiCert Global Root G2; no key, no station): the batched
// reports of the chosen airports (fetch, run once), or the stationinfo list (lookup: near the location, or one code)
static void startMetar(FwdKind kind) {
  Job& j = gB->job;
  gPreSendFree = heapNow().free8;
  memset(j.key, 0, sizeof j.key);
  j.slot = FWD_SLOTS;  // not a target
  j.kind = kind;
  j.type = FwdType::Template;  // a 2xx is the answer; the body goes to the sink
  j.seen = 0;
  j.t0 = millis();
  j.why = nullptr;
  j.body[0] = 0;
  j.unix_now = realNow();
  j.tls = true;
  j.port = 443;
  j.roots = FWD_ROOTBIT_DIGICERT_G2;
  j.success[0] = 0;
  j.id[0] = 0;
  snprintf(j.host, sizeof j.host, "%s", "aviationweather.gov");
  char path[120];
  if (kind == FwdKind::Lookup) {
    portENTER_CRITICAL(&gFwdMux);
    const bool nearMe = gLook.near;
    char ids[5];
    memcpy(ids, gLook.ids, sizeof ids);
    gLook.state = 2;
    portEXIT_CRITICAL(&gFwdMux);
    int32_t lat = 0, lon = 0;
    xSemaphoreTake(gLock, portMAX_DELAY);
    const bool loc = metarLocation(&lat, &lon);
    xSemaphoreGive(gLock);
    if (nearMe && !loc) {  // the location was cleared after the request -- nothing is sent
      portENTER_CRITICAL(&gFwdMux);
      gMetar.result(FwdKind::Lookup, MetarOutcome::Reject, millis(), 0);  // frees the scheduler's running slot
      gLook.state = 3;
      gLook.n = 0;
      snprintf(gLook.result, sizeof gLook.result, "%s", "no_location");
      portEXIT_CRITICAL(&gFwdMux);
      return;
    }
    new (gB->metar) AirportJsonSink(loc, lat, lon);
    if (nearMe && loc) {
      int32_t box[4];
      airportBox(lat, lon, box);
      char v[4][16];
      for (int i = 0; i < 4; i++) centi(v[i], sizeof v[i], box[i]);
      snprintf(path, sizeof path, "/api/data/stationinfo?bbox=%s,%s,%s,%s&format=json", v[0], v[1], v[2], v[3]);
    } else {
      snprintf(path, sizeof path, "/api/data/stationinfo?ids=%.4s&format=json", ids);
    }
  } else {
    MetarFetchBuf* f = new (gB->metar)
        MetarFetchBuf{MetarLineSink(metarLine, nullptr), {}, {}, 0, 0, {}, 0, 0, 0, 0, 0, 0, {}, TafParser(nullptr, 0, 0), false};
    f->sink = MetarLineSink(metarLine, f);
    f->now = j.unix_now;
    xSemaphoreTake(gLock, portMAX_DELAY);
    f->nwant = gMetarCfg.n < METAR_AIRPORTS_MAX ? gMetarCfg.n : METAR_AIRPORTS_MAX;
    for (int i = 0; i < f->nwant; i++) memcpy(f->want[i], gMetarCfg.a[i].icao, 5);
    xSemaphoreGive(gLock);
    char ids[3 * 5 + 1] = "";
    for (int i = 0; i < f->nwant; i++) {
      f->wantp[i] = f->want[i];
      strcat(ids, i ? "," : "");
      strcat(ids, f->want[i]);
    }
    // Without hours= AWC answers a dead station's last report however old (a month-old line whose day
    // matches today's passes the time rule); with hours=2 it answers 204 -> nodata. Several lines per airport come back
    // newest first; metarLine keeps the newest (and a correction of it).
    snprintf(path, sizeof path, "/api/data/metar?ids=%s&format=raw&hours=2", ids);
    if (kind == FwdKind::Fetch && gMetar.runTaf()) {
      // the hour's :50 cycle reads the TAFs (TAF only: a METAR line in this answer has no hours window, and the
      // other three cycles bring the reports); the body cap is METAR_BODY_TAF
      f->taf = true;
      f->tp = TafParser(f->wantp, f->nwant, j.unix_now);
      f->sink = MetarLineSink(TafParser::lineFn, &f->tp);
      snprintf(path, sizeof path, "/api/data/taf?ids=%s&format=raw", ids);
    }
    if (kind == FwdKind::RunOnce) {
      portENTER_CRITICAL(&gFwdMux);
      gRun.state = 2;
      portEXIT_CRITICAL(&gFwdMux);
    }
  }
  const int n = snprintf(j.req, sizeof j.req,
                         "GET %s HTTP/1.1\r\nHost: aviationweather.gov\r\nUser-Agent: fauxcloud/" FC_VERSION
                         "\r\nConnection: close\r\n\r\n",
                         path);
  j.req_len = n > 0 ? (size_t)n : 0;
  // the yield test reads the parser's status: no status of an earlier job may stand until the net-out task begins its own
  gB->s.resp.begin(FwdType::Template, "");
  const uint32_t lp = httpLastPageMs();
  [[maybe_unused]] const uint32_t ms = millis();  // the page-idle statistic (bench) reads it
  gMetarPage0 = lp;
  BENCH(gMetarIdle0 = lp ? ms - lp : UINT32_MAX; gMetarDuring = 0;)  // the gap to the last page request at the start
  gAbort = false;
  gAbortWhy = nullptr;
  gJobSt = J_POSTED;
  xTaskNotifyGive(gNetTask);
}
#endif

#if FWD_TLS
// The WU forecast pull (api.weather.com, anchor GlobalSign Root CA - R3). The key comes from NVS into the job
// block here and is wiped with the request after the exchange (runJob), as a target's key.
static void startWufc(FwdKind kind) {
  Job& j = gB->job;
  gPreSendFree = heapNow().free8;
  j.slot = FWD_SLOTS;  // not a target
  j.kind = kind;
  j.type = FwdType::Template;  // a 2xx is the answer; the body goes to the scanner
  j.seen = 0;
  j.t0 = millis();
  j.why = nullptr;
  j.body[0] = 0;
  j.unix_now = realNow();
  j.tls = true;
  j.port = 443;
  j.roots = FWD_ROOTBIT_GLOBALSIGN_R3;
  j.success[0] = 0;
  j.id[0] = 0;
  snprintf(j.host, sizeof j.host, "%s", "api.weather.com");
  new (gB->metar) WufcSink();
  const size_t kn = nvsGetBlob("wk", j.key, sizeof j.key - 1);
  j.key[kn < sizeof j.key ? kn : 0] = 0;
  xSemaphoreTake(gLock, portMAX_DELAY);
  const float lat = gBridge.lat, lon = gBridge.lon;
  gWufcJobEpoch = gWufcEpoch;  // a settings change while it runs voids its result (gWufcEpoch)
  xSemaphoreGive(gLock);
  char path[200];
  const int pn = j.key[0] && lat == lat && lon == lon ? wufcPath(path, sizeof path, lat, lon, j.key) : -1;
  int n = pn < 0 ? -1
                 : snprintf(j.req, sizeof j.req,
                            "GET %s HTTP/1.1\r\nHost: api.weather.com\r\nUser-Agent: fauxcloud/" FC_VERSION
                            "\r\nAccept-Encoding: identity\r\nAccept: application/json\r\nConnection: close\r\n\r\n",
                            path);
  memset(path, 0, sizeof path);  // the key was in it
  if (n <= 0 || (size_t)n >= sizeof j.req) {  // no key or no location since the offer: nothing is sent
    memset(j.req, 0, sizeof j.req);
    memset(j.key, 0, sizeof j.key);
    portENTER_CRITICAL(&gFwdMux);
    gWufc.result(kind, WufcOutcome::Off, millis(), 0);  // never sent: no answer to count
    if (kind == FwdKind::WuRun) gWuRun.state = 3, gWuRunT = realNow(), snprintf(gWuRun.result, sizeof gWuRun.result, "%s", "off");
    portEXIT_CRITICAL(&gFwdMux);
    ((WufcSink*)gB->metar)->~WufcSink();
    return;
  }
  j.req_len = (size_t)n;
  if (kind == FwdKind::WuRun) {
    portENTER_CRITICAL(&gFwdMux);
    gWuRun.state = 2;
    portEXIT_CRITICAL(&gFwdMux);
  }
  gB->s.resp.begin(FwdType::Template, "");
  gMetarPage0 = httpLastPageMs();  // the page-yield rule, as a METAR job
  gAbort = false;
  gAbortWhy = nullptr;
  gJobSt = J_POSTED;
  xTaskNotifyGive(gNetTask);
}
#endif

// Render the job into gB->job.req and post it (loop task). A render that cannot send is judged at once.
static void startJob(int slot, FwdKind kind) {
  FwdTarget& t = gB->s.r.t;  // the one target, copied (loop only, no job posted)
  StartClock sc;  // the parts (gStartPart, read by fwdTick); bench only
  gPreSendFree = heapNow().free8;  // what the 10-minute window sees while the send runs (the byte heap)
  FwdGateIn g;
  Job& j = gB->job;
  sc.cut(SP_HEAP);
  xSemaphoreTake(gLock, portMAX_DELAY);
  sc.lockWait();
  t = gFwd[slot];
  const bool have = snapStation(t.station, t.min_s, 0, true, &g, &j.live);
  const PosixTz tz = gTz;
  xSemaphoreGive(gLock);
  j.slot = slot;
  j.kind = kind;
  j.type = t.type;
  j.seen = j.live.last_seen;
  j.t0 = millis();
  j.why = nullptr;
  j.body[0] = 0;
  j.unix_now = realNow();  // real time only: the certificate check needs it
  FwdErr e = FwdErr::BadUrl;
  FwdUrl url;
  sc.cut(SP_SNAP);
  if (have) fwdRecordBuild(j.live, t.min_s, &gB->s.r.rec);  // the snapshot is dead after this: the buffer takes the request
  sc.cut(SP_RECORD);
  if (have) {
    if (kind == FwdKind::Test) {  // the redacted target for the sheet, before the key is read
      // straight into the test record: the GET reads it only once the state says done (written while running)
      const FwdCtx rc{&gB->s.r.rec, t.id, nullptr, "fauxcloud-" FC_VERSION, &tz, true};
      if (fwdRender(t, rc, &url, gB->test_target, sizeof gB->test_target) != FwdErr::None) gB->test_target[0] = 0;
    }
    sc.cut(SP_RENDER);  // the test's redacted render, if any
    loadKey(slot, j.key, sizeof j.key);
    sc.cut(SP_KEY);
    const FwdCtx c{&gB->s.r.rec, t.id, j.key, "fauxcloud-" FC_VERSION, &tz, false};
    memcpy(j.req, "GET ", 4);
    e = fwdRender(t, c, &url, j.req + 4, sizeof j.req - 4);
    if (e == FwdErr::None) {
      const size_t at = strlen(j.req);
      const int n = snprintf(j.req + at, sizeof j.req - at,
                             " HTTP/1.1\r\nHost: %s\r\nUser-Agent: fauxcloud/" FC_VERSION "\r\nConnection: close\r\n\r\n",
                             url.host);
      if (n <= 0 || (size_t)n >= sizeof j.req - at) e = FwdErr::TooLong;
      else j.req_len = at + (size_t)n;
    }
    sc.cut(SP_RENDER);
  }
  if (e != FwdErr::None) {  // nothing to send (no value for a path variable, too long, no station)
    memset(j.key, 0, sizeof j.key);
    memset(j.req, 0, sizeof j.req);
    j.result = FwdResult::Reject;
    j.status = -1;
    j.retry_after = 0;
    j.ms = 0;
    j.tls = t.https;
    j.why = have ? fwdErrName(e) : "no_station";
    logJob(j);  // rare (a render that cannot send): the one send log left on the loop tick
    finishResult();
    return;
  }
  // host[:port] -> name + port
  memcpy(j.host, url.host, sizeof j.host);
  char* colon = strchr(j.host, ':');
  j.tls = FWD_TLS && url.https;  // FWD_TLS=0: a target never carries https (fwdFromJson), this is the last guard
  j.port = j.tls ? 443 : 80;
  if (colon) {
    *colon = 0;
    j.port = (uint16_t)atoi(colon + 1);
  }
#if FWD_TLS
  j.roots = rootsFor(t.type);
#else
  j.roots = 0;
#endif
  memcpy(j.success, t.success, sizeof j.success);
  memcpy(j.id, t.id, sizeof j.id);
  gAbort = false;
  gAbortWhy = nullptr;
  gJobSt = J_POSTED;
  xTaskNotifyGive(gNetTask);
}

void fwdAbort(const char* why) {  // otaStart (before its own heap guard)
  if (gJobSt.load() == J_IDLE) return;
  gAbortWhy = why;
  gAbort = true;
}

struct SlotView {  // what the tick needs of a target (copied under gLock each second)
  bool used, enabled, https;
  FwdType type;
  uint16_t min_s, max_s;
  char station[STATION_ID_LEN];
};

void fwdTick() {
  if (gMode != Mode::Run) return;
  const uint32_t t0 = micros(), ms = millis();
  struct {  // the parts of this tick (bench only; the release measures the whole tick)
    uint32_t last, lock = 0, part[TP_COUNT] = {};
    void mark(int p) {
#if FC_DIAG_BENCH
      const uint32_t n = micros();
      part[p] += n - last - lock;
      last = n, lock = 0;
#else
      (void)p;
#endif
    }
  } tc;
  tc.last = t0;
  const uint8_t jobAt = gJobSt.load();
  if (jobAt == J_DONE) finishResult();  // a finished job first
  tc.mark(TP_FINISH);
  if (gJobSt.load() == J_RUNNING && !gAbort.load()) {  // abort a running send: heap, OTA, restart, the 10-s deadline
    const HeapNow h = heapNow();
    const char* why = fwdAbortWhy(h.free8, h.largest8, otaActive(),
                                  rebootPending() || factoryResetPending(), ms - gB->job.t0);
#if FWD_TLS
    // A page wins over a scheduled METAR / TAF fetch or WU pull -- a page request before the answer's status line
    // aborts it (`yield`, retried by the source); after it the answer is short and is read to the end. A user's job
    // (run once, lookup, Fetch now) never yields: the sheet polls while the user waits for it, and nothing retries it
    if (metarKind(gB->job.kind) || gB->job.kind == FwdKind::WuFetch) {
      const uint32_t lp = httpLastPageMs();
      const bool page = httpInFlight() > 0 || lp != gMetarPage0;
      BENCH(if (page && gMetarDuring < UINT16_MAX) gMetarDuring++;)
      gMetarPage0 = lp;
      const bool scheduled = gB->job.kind == FwdKind::Fetch || gB->job.kind == FwdKind::WuFetch;
      if (!why && scheduled && page && gB->s.resp.status() < 0) why = "yield";
    }
#endif
    if (why) {
      static char w[24];
      snprintf(w, sizeof w, "aborted_%s", why);
      gAbortWhy = w;
      gAbort = true;
    }
  }
  tc.mark(TP_ABORT);
  SlotView t[FWD_SLOTS];
  [[maybe_unused]] uint32_t lockUs = 0, l0 = 0;  // the tick split into the gLock waits and its own work (bench)
  BENCH(l0 = micros();)
  xSemaphoreTake(gLock, portMAX_DELAY);
  BENCH(tc.lock = micros() - l0; lockUs += tc.lock;)
  const bool master = gBridge.forwarding;
  const PosixTz tz = gTz;
  for (int i = 0; i < FWD_SLOTS; i++) {
    const FwdTarget& x = gFwd[i];
    t[i] = SlotView{x.used, x.enabled, x.https, x.type, x.min_s, fwdMaxS(x), {}};
    memcpy(t[i].station, x.station, sizeof t[i].station);
  }
  xSemaphoreGive(gLock);
  bool any = false, testWaiting = false, metarWants = false;
  for (const SlotView& x : t) any |= x.used && x.enabled;
  portENTER_CRITICAL(&gFwdMux);
  for (const FwdSlot& s : gSched.s) testWaiting |= s.test_pending;
#if FWD_TLS
  metarWants = gMetar.lookupPending() || gMetar.runPending();
#endif
  portEXIT_CRITICAL(&gFwdMux);
#if FWD_TLS
  xSemaphoreTake(gLock, portMAX_DELAY);
  const bool metarOn = gMetarCfg.on, metarHave = gMetarCfg.n > 0;
  xSemaphoreGive(gLock);
  metarWants |= metarOn && metarHave;
  xSemaphoreTake(gLock, portMAX_DELAY);
  const bool wuOn = gWufcCfg.on && gWufcKeySet, wuKey = gWufcKeySet;
  const bool wuLoc = gBridge.lat == gBridge.lat && gBridge.lon == gBridge.lon;
  xSemaphoreGive(gLock);
  portENTER_CRITICAL(&gFwdMux);
  metarWants |= wuOn || gWufc.runPending();
  portEXIT_CRITICAL(&gFwdMux);
#endif
  if (!gNetTask && ((master && (any || testWaiting)) || metarWants)) startNetOut("at the first enable");
  tc.mark(TP_VIEW);
  // the busy table
  uint16_t busy = 0;
  const uint32_t lastIn = lastIngestMs();
  if (uxQueueMessagesWaiting(gQueue) > 0 || (lastIn && ms - lastIn < FWD_QUIET_MS)) busy |= FB_UPLOAD;
  const uint32_t shed = httpShed();
  if (shed != gShedSeen) gShedSeen = shed, gShedMs = ms | 1;
  const uint32_t lastPage = httpLastPageMs();
  if (fwdHttpBusy(ms, httpInFlight(), lastPage, gShedMs)) busy |= FB_HTTP;
  if (storeBusy()) busy |= FB_STORE;
  if (mqttBusy()) busy |= FB_MQTT;
  if (rebootPending() || factoryResetPending() || otaActive()) busy |= FB_RESTART;
  const bool timeOk = outlookTimeOk();
  if (!staUp() || !timeOk) busy |= FB_LINK;
  tc.mark(TP_BUSY);
  const bool clockOk = timeSynced();
  const uint32_t nowU = nowS();
  const int32_t off = clockOk ? localOffset(tz, nowU) : 0;  // hours close on UTC boundaries, days at local midnight
  // learners: the hour guard needs them right after a boundary, the table whenever a send could start
  const bool nearHour = clockOk && (FwdHourGuard::near(nowU, off) || gHour.boundary);
  if (learnBusy()) busy |= FB_LEARN;  // app_learn.cpp's own flag
  if (clockOk && gHour.tick(nowU, off, storeBusy(), nearHour && learnBusy())) busy |= FB_HOUR;
  tc.mark(TP_HOUR);
  // the per-target input: the data gates under gLock
  FwdTickIn in = {};
  in.now_ms = ms;
  in.busy = busy;
  tc.mark(TP_GATES);
  {
    const HeapNow h = heapNow();  // the floors are on the byte heap
    in.free = h.free8;
    in.largest = h.largest8;
  }
  tc.mark(TP_HEAP);
  in.netout_idle = gNetTask && gJobSt.load() == J_IDLE;
  uint32_t okSeen[FWD_SLOTS];
  portENTER_CRITICAL(&gFwdMux);
  for (int i = 0; i < FWD_SLOTS; i++) okSeen[i] = gSched.s[i].ok_seen;
  portEXIT_CRITICAL(&gFwdMux);
  BENCH(l0 = micros();)
  xSemaphoreTake(gLock, portMAX_DELAY);
  BENCH(tc.lock = micros() - l0; lockUs += tc.lock;)
  for (int i = 0; i < FWD_SLOTS; i++) {
    FwdSlotIn& s = in.s[i];
    s.on = master && t[i].used && t[i].enabled;
    s.test_ok = master && t[i].used;  // a test may run on a disabled target while the switch is on
    s.tls = FWD_TLS && t[i].https;
    s.tls_class = fwdTlsClassOf(t[i].type);
    s.min_s = t[i].min_s;
    s.max_s = t[i].max_s;
    s.floor_s = fwdMinFloor(t[i].type);
    if (!t[i].used) continue;
    FwdGateIn g;
    snapStation(t[i].station, t[i].min_s, okSeen[i], timeOk, &g, nullptr);
    s.gate = fwdGate(g, false);
    s.gate_test = fwdGate(g, true);
  }
  xSemaphoreGive(gLock);
  tc.mark(TP_GATES);
  // the net-out queue (src/netq.h): forwarding's candidates and METAR's, one start at most
  portENTER_CRITICAL(&gFwdMux);
  NqCand cand[NQ_MAX];
  int nc = gSched.offer(in, cand);
  bool mRun = false;
#if FWD_TLS
  const MetarTickIn mi{ms, metarOn && timeOk, metarHave, timeOk ? realNow() : 0};  // the grid on UTC
  nc += gMetar.offer(mi, cand + nc);
  mRun = gMetar.running();
  // the WU pull (the lowest priority) and "Fetch now" (user); the switch may be off for Fetch now
  const WufcTickIn wi{ms, wuOn && wuLoc && timeOk, wuKey && wuLoc && timeOk, timeOk ? realNow() : 0};
  nc += gWufc.offer(wi, cand + nc);
  mRun = mRun || gWufc.running();
  gWuHeld = wufcRunHeld(busy, in.free, in.largest);  // the estimate's input (etaS)
#endif
  const NqIn nq{ms, busy, in.free, in.largest, in.netout_idle && !gSched.running() && !mRun, gSched.sendDueS(ms)};
  const NqPick pick = nqPick(nq, cand, nc);
  const FwdStart st = gSched.picked(in, cand, nc, pick);
  FwdKind mKind = FwdKind::None;
#if FWD_TLS
  const bool mStart = gMetar.picked(mi, cand, nc, pick, &mKind);
  FwdKind wKind = FwdKind::None;
  const bool wStart = gWufc.picked(wi, cand, nc, pick, &wKind);
  const MetarUserOut wo = gWufc.takeRunOutcome();
  if (wo != MetarUserOut::None && gWuRun.state == 1)
    gWuRun.state = 3, gWuRunT = timeOk ? realNow() : 0, snprintf(gWuRun.result, sizeof gWuRun.result, "%s", wo == MetarUserOut::Off ? "off" : "busy");
  const MetarUserOut ro = gMetar.takeRunOutcome(), lo = gMetar.takeLookupOutcome();
  if (ro != MetarUserOut::None && gRun.state == 1)
    gRun.state = 3, snprintf(gRun.result, sizeof gRun.result, "%s", ro == MetarUserOut::Off ? "off" : "busy");
  if (lo != MetarUserOut::None && gLook.state == 1)
    gLook.state = 3, snprintf(gLook.result, sizeof gLook.result, "%s", lo == MetarUserOut::Off ? "off" : "busy");
#else
  const bool mStart = false;
#endif
  int outSlot = -1;
  const FwdTestOutcome out = gSched.takeTestOutcome(&outSlot);
  if (out != FwdTestOutcome::None && gTest.state == 1 && gTest.slot == outSlot) {
    snprintf(gTest.result, sizeof gTest.result, "%s",
             out == FwdTestOutcome::NoData ? "nodata" : out == FwdTestOutcome::Off ? "off" : "busy");
    gTest.state = 2;
  }
  portEXIT_CRITICAL(&gFwdMux);
  tc.mark(TP_SCHED);
  BENCH(gStartLockUs = 0; memset(gStartPart, 0, sizeof gStartPart);)
  if (st.slot >= 0) startJob(st.slot, st.kind);
#if FWD_TLS
  else if (mStart) startMetar(mKind);
  else if (wStart) startWufc(wKind);
#else
  (void)mStart, (void)mKind;
#endif
  BENCH(tc.lock = gStartLockUs;)
  tc.mark(TP_START);
  BENCH(lockUs += gStartLockUs;)
  const uint32_t us = (FC_DIAG_BENCH ? tc.last : micros()) - t0;
  if (us > gTickUsMax) gTickUsMax = us;
#if FC_DIAG_BENCH
  if (lockUs > gTickLockUsMax) gTickLockUsMax = lockUs;
  for (int p = 0; p < TP_COUNT; p++)
    if (tc.part[p] > gTickPartMax[p]) gTickPartMax[p] = tc.part[p];
  for (int p = 0; p < SP_COUNT; p++)
    if (gStartPart[p] > gStartPartMax[p]) gStartPartMax[p] = gStartPart[p];
  if (us - lockUs > TICK_SLOW_US) gTickSlow++;
  if (us - lockUs > gTickOwnUsMax) {
    gTickOwnUsMax = us - lockUs;
    memcpy(gTickWorst, tc.part, sizeof gTickWorst);
    memcpy(gStartWorst, gStartPart, sizeof gStartWorst);
    gTickWorstJob = jobAt;
    gTickWorstT = realNow();
  }
#endif
}

// diag (LAN): sums over the targets
void fwdDiag(Diag* d) {
  uint32_t ok = 0, fail = 0, att = 0, skip = 0, drop = 0;
  portENTER_CRITICAL(&gFwdMux);
  for (const FwdSlot& s : gSched.s) {
    ok += s.n.ok + s.n.dup;
    // readings not delivered after an attempt (a failed attempt retried with success is not one); the failed
    // attempts themselves on their own
    fail += s.n.drop + s.n.reject + s.n.refused + s.n.limited;
    att += s.n.fail + s.n.unconfirmed;  // an unconfirmed send is a failed attempt, never a lost reading
    skip += s.n.skip + s.n.nodata + s.n.heap;
    drop += s.n.drop;
  }
  portEXIT_CRITICAL(&gFwdMux);
  d->fwd_ok = ok, d->fwd_fail = fail, d->fwd_attempt_fail = att, d->fwd_skip = skip, d->fwd_drop = drop;
#if FC_DIAG_BENCH
  const uint32_t hm = gSendHeapMin.load(), lm = gSendLargestMin.load();
  d->fwd_heap_min = hm == UINT32_MAX ? 0 : hm;
  d->fwd_largest_min = lm == UINT32_MAX ? 0 : lm;
  d->fwd_send_ms_max = gSendMsMax.load();
#endif
}

// ---------------------------------------------------------------- routes (STA, session + X-Fauxcloud: adminGateReq)
// GET is rendered under gLock (the targets), the scheduler's words read as they are (display only).
static bool gKeySet[FWD_SLOTS];
static const Errors* gFwdErrP = nullptr;  // the handler's Errors (its stack) while it renders

static void diagJson(Out& o, const FwdDiag& d, const char* key = "diag") {
  o.printf(",\"%s\":{\"pre8\":%lu,\"largest8_pre\":%lu,\"min8\":%lu,\"largest8_min\":%lu,\"ms_dns\":%u,\"ms_tcp\":%u,"
           "\"ms_total\":%u,\"sock_err\":%d",
           key, (unsigned long)d.pre8, (unsigned long)d.largest8_pre, (unsigned long)d.min8, (unsigned long)d.largest8_min,
           (unsigned)d.ms_dns, (unsigned)d.ms_tcp, (unsigned)d.ms_total, (int)d.sock_err);
  BENCH(o.printf(",\"stack_free\":%u", (unsigned)d.stack_free);)  // bench only
#if FWD_TLS
  o.printf(",\"stage\":\"%s\",\"br_err\":%d,\"br_err_name\":\"%s\",\"ms_hs\":%u,\"version\":\"0x%04x\",\"suite\":\"0x%04x\"",
           tlscStageName((TlscStage)d.stage), (int)d.br_err, tlscErrName(d.br_err), (unsigned)d.ms_hs, (unsigned)d.version,
           (unsigned)d.suite);
  BENCH(o.printf(",\"curve\":%u,\"rec_in_max\":%u", (unsigned)d.curve, (unsigned)d.rec_in_max);)  // bench only
#endif
  o.raw("}");
}

static void isoOrNull(Out& o, uint32_t t) {
  char b[24];
  if (t >= EPOCH_MIN && formatIsoUtc(b, sizeof b, t) > 0) o.printf("\"%s\"", b);
  else o.raw("null");
}

#if FWD_TLS
static void renderWufc(Out& o);  // below
#endif
static void renderFwd(Out& o, httpd_req_t*) {  // caller holds gLock (sendRendered)
  const uint32_t nowMs = millis();
  o.printf("{\"enabled\":%s,\"tls\":%s,\"https_default\":%s,\"stations\":[", gBridge.forwarding ? "true" : "false",
           FWD_TLS ? "true" : "false", FWD_TLS && FWD_HTTPS_DEFAULT ? "true" : "false");  // the sheet follows it
  for (int i = 0, n = 0; i < MAX_STATIONS; i++)
    if (Station* s = gStations.at(i)) {
      if (n++) o.raw(",");
      o.jsonStr(s->id);
    }
  o.raw("],\"targets\":[");
  bool first = true;
  for (int i = 0; i < FWD_SLOTS; i++) {
    const FwdTarget& t = gFwd[i];
    if (!t.used) continue;
    const FwdSlot& s = gSched.s[i];
    const FwdLast& l = gLast[i];
    o.printf("%s{\"slot\":%d,\"type\":\"%s\",\"station\":", first ? "" : ",", i, fwdTypeName(t.type));
    first = false;
    o.jsonStr(t.station);
    if (t.type == FwdType::WuCompat) {
      o.raw(",\"host\":");
      o.jsonStr(t.wc.host);
      o.raw(",\"path\":");
      o.jsonStr(t.wc.path);
    }
    if (t.type == FwdType::Template) {
      o.raw(",\"url\":");
      o.jsonStr(t.url);
    }
    if (!fwdIsPreset(t.type)) {
      o.raw(",\"success\":");
      o.jsonStr(t.success);
    }
    o.raw(",\"id\":");
    o.jsonStr(t.id);
    o.printf(",\"key\":\"%s\",\"min_s\":%u,\"max_s\":%u,\"floor_s\":%u,\"https\":%s,\"enabled\":%s",
             gKeySet[i] ? SECRET_SET : "", (unsigned)t.min_s, (unsigned)fwdMaxS(t), (unsigned)fwdMinFloor(t.type),
             t.https ? "true" : "false", t.enabled ? "true" : "false");
    o.printf(",\"verified\":%s", t.verified ? "true" : "false");
    o.printf(",\"paused\":%s", !s.paused ? "null" : s.pause_why == FP_MEMORY ? "\"memory\"" : "\"key\"");
    const long next = gSched.nextS(i, nowMs);
    const char* state = s.st == FwdSt::Running ? "running" : s.st == FwdSt::Retry ? "retry"
                        : s.st == FwdSt::Off   ? "off"
                        : next == 0            ? "due"
                                               : "idle";
    o.printf(",\"state\":\"%s\",\"next_s\":%ld,\"last\":{\"t\":", state, next);
    isoOrNull(o, l.t);
    o.printf(",\"status\":%d,\"result\":\"%s\",\"ms\":%u", l.t ? l.status : -1, fwdResultName(l.result), (unsigned)l.ms);
    if (l.detail[0]) o.printf(",\"why\":\"%s\"", l.detail);
    if (l.t) diagJson(o, l.d);
    o.raw("},\"last_ok\":");
    isoOrNull(o, l.ok_t);
    o.printf(",\"n\":{\"ok\":%lu,\"dup\":%lu,\"skip\":%lu,\"nodata\":%lu,\"reject\":%lu,", (unsigned long)s.n.ok,
             (unsigned long)s.n.dup, (unsigned long)s.n.skip, (unsigned long)s.n.nodata, (unsigned long)s.n.reject);
    o.printf("\"refused\":%lu,\"limited\":%lu,\"drop\":%lu,\"heap\":%lu,\"fail\":%lu,\"unconfirmed\":%lu}",
             (unsigned long)s.n.refused, (unsigned long)s.n.limited, (unsigned long)s.n.drop, (unsigned long)s.n.heap,
             (unsigned long)s.n.fail, (unsigned long)s.n.unconfirmed);
    o.raw(",\"causes\":{");
    for (int c = 1; c < (int)FwdCause::COUNT; c++)
      o.printf("%s\"%s\":%u", c > 1 ? "," : "", fwdCauseName((FwdCause)c), (unsigned)gCause[i][c]);
    o.raw("},\"skip_why\":");
    if (s.last_why) o.printf("\"%s\"", fwdBusyName(s.last_why));
    else o.raw("null");
    o.raw("}");
  }
#if FC_DIAG_BENCH
  const uint32_t hm = gSendHeapMin.load(), lm = gSendLargestMin.load();
  o.printf("],\"heap\":{\"send_min\":%lu,\"largest_min\":%lu,\"send_ms_max\":%lu,\"tick_us_max\":%lu,"
           "\"tick_lock_us_max\":%lu,\"tick_own_us_max\":%lu}",
           (unsigned long)(hm == UINT32_MAX ? 0 : hm), (unsigned long)(lm == UINT32_MAX ? 0 : lm),
           (unsigned long)gSendMsMax.load(), (unsigned long)gTickUsMax, (unsigned long)gTickLockUsMax,
           (unsigned long)gTickOwnUsMax);
  // the tick in parts (microseconds of own work; "worst" = the parts of the tick with the largest own work)
  o.printf(",\"tick_parts\":{\"slow\":%lu,\"worst_job\":\"%s\",\"worst_t\":", (unsigned long)gTickSlow,
           JOB_ST_NAME[gTickWorstJob & 3]);
  isoOrNull(o, gTickWorstT);  // lines the worst tick up with an hour or day close
  const uint32_t* const rows[4] = {gTickPartMax, gTickWorst, gStartPartMax, gStartWorst};
  static const char* const rowName[4] = {"max", "worst", "start_max", "start_worst"};
  for (int r = 0; r < 4; r++) {
    const bool tick = r < 2;
    o.printf(",\"%s\":{", rowName[r]);
    for (int p = 0; p < (tick ? (int)TP_COUNT : (int)SP_COUNT); p++)
      o.printf("%s\"%s\":%lu", p ? "," : "", tick ? TICK_PART_NAME[p] : START_PART_NAME[p], (unsigned long)rows[r][p]);
    o.raw("}");
  }
  o.raw("}");
#else  // the release keeps heap.tick_us_max only; no tick_parts
  o.printf("],\"heap\":{\"tick_us_max\":%lu}", (unsigned long)gTickUsMax);
#endif
  o.printf(",\"task\":{\"up\":%s,\"stack_free\":%lu}", gNetTask ? "true" : "false", (unsigned long)gNetStackFree);
#if FWD_TLS
  renderWufc(o);  // the forecast card; gLock is held here
#endif
  if (gFwdErrP && gFwdErrP->n) {
    o.raw(",\"errors\":[");
    for (int i = 0; i < gFwdErrP->n; i++) {
      if (i) o.raw(",");
      o.jsonStr(gFwdErrP->at(i));
    }
    o.raw("]");
  }
  o.raw("}");
}

#if FWD_TLS
// "wufc":{"on","key","verified","loc","state","next_t","last","run","tomorrow"} -- the key as a
// bool only; loc = the geocode as sent (1 decimal, WUFC_GEO_DECIMALS) or null; tomorrow = the stored line while shown. Caller holds gLock.
static void renderWufc(Out& o) {
  const uint32_t now = realNow();
  o.printf(",\"wufc\":{\"on\":%s,\"key\":%s,\"verified\":%s,\"loc\":", gWufcCfg.on ? "true" : "false",
           gWufcKeySet ? "true" : "false", gWufcCfg.verified ? "true" : "false");
  if (gBridge.lat == gBridge.lat && gBridge.lon == gBridge.lon)
    o.printf("{\"lat\":%.*f,\"lon\":%.*f}", WUFC_GEO_DECIMALS, (double)gBridge.lat, WUFC_GEO_DECIMALS, (double)gBridge.lon);
  else o.raw("null");
  portENTER_CRITICAL(&gFwdMux);
  const char* st = gWufc.stateName();
  const WufcPause pz = gWufc.pause();
  const long next = gWufc.nextS(millis());
  const WufcLastJob x = gWufcLast;
  const bool ran = gWufcRan;
  const MetarRun run = gWuRun;
  const uint32_t runT = gWuRunT, eta = gWufc.etaS(millis(), wufcLineBusy(), gWuHeld);
  const WufcCounters n = gWufc.n;
  portEXIT_CRITICAL(&gFwdMux);
  static const char* const RUN_NAME[] = {"idle", "waiting", "running", "done"};
  o.printf(",\"state\":\"%s\",\"pause\":\"%s\",\"next_t\":", st, wufcPauseName(pz));
  isoOrNull(o, next >= 0 && now ? now + (uint32_t)next : 0);
  o.raw(",\"last\":");
  if (ran) {
    o.raw("{\"t\":");
    isoOrNull(o, x.t);
    o.printf(",\"result\":\"%s\",\"status\":%d}", wufcOutcomeName(x.out), (int)x.status);
  } else {
    o.raw("null");
  }
  o.printf(",\"run\":{\"state\":\"%s\",\"result\":", RUN_NAME[run.state & 3]);
  if (run.state == 3) o.jsonStr(run.result);
  else o.raw("null");
  if (run.state == 1) o.printf(",\"eta_s\":%lu", (unsigned long)eta);  // queued
  if (run.state == 3) {
    o.raw(",\"t\":");
    isoOrNull(o, runT);
  }
  o.printf("},\"n\":{\"ok\":%lu,\"nodata\":%lu,\"fail\":%lu,\"limited\":%lu,\"refused\":%lu,\"skip\":%lu,", (unsigned long)n.ok,
           (unsigned long)n.nodata, (unsigned long)n.fail, (unsigned long)n.limited, (unsigned long)n.refused, (unsigned long)n.skip);
  o.printf("\"heap\":%lu,\"missed\":%lu,\"bad\":%lu,\"paused\":%lu}", (unsigned long)n.heap, (unsigned long)n.missed,
           (unsigned long)n.bad, (unsigned long)n.paused);
  BENCH(o.printf(",\"diag\":{\"rec_in_max\":%u,\"rec_app_max\":%u,\"min8\":%lu,\"ms\":%u}", (unsigned)x.d.rec_in_max,
                 (unsigned)x.d.rec_app_max, (unsigned long)x.d.min8, (unsigned)x.d.ms_total);)
  o.raw(",\"tomorrow\":");
  const char* day = wufcDay(gWufcStore, now, gTz);
  if (day) {
    o.printf("{\"day\":\"%s\",\"phrase\":", day);
    o.jsonStr(gWufcStore.d.phrase);
    o.printf(",\"hi\":%d,\"lo\":%d,\"pop\":", (int)gWufcStore.d.hi, (int)gWufcStore.d.lo);
    if (gWufcStore.d.pop == WUFC_UNSET) o.raw("null");
    else o.printf("%d", (int)gWufcStore.d.pop);
    o.raw(",\"t\":");
    isoOrNull(o, gWufcStore.fetched_t);
    o.raw("}");
  } else {
    o.raw("null");
  }
  o.raw("}");
}
#endif

static esp_err_t sendFwd(httpd_req_t* req, const Errors* err) {
  for (int i = 0; i < FWD_SLOTS; i++) gKeySet[i] = hasKey(i);  // NVS, outside gLock
  gFwdErrP = err;
  const esp_err_t r = sendRendered(req, "application/json", renderFwd);  // under gLock
  gFwdErrP = nullptr;
  return r;
}

// The httpd task writes a verification the loop marked (its NVS write and log line never run on the loop tick); a
// restart before the next poll loses it, and the user sends one again
static void saveVerified() {
  const uint8_t m = gVerSave.exchange(0);
  for (int i = 0; i < FWD_SLOTS; i++)
    if (m & (1u << i)) {
      xSemaphoreTake(gLock, portMAX_DELAY);
      const FwdTarget t = gFwd[i];
      xSemaphoreGive(gLock);
      if (t.used && t.verified) logf("FWD", "target %d verified%s", i, saveTarget(i, t) ? "" : " (not saved; until reboot)");
    }
}

static void saveWufc();
static esp_err_t onFwdGet(httpd_req_t* req) {
  if (!adminGateReq(req)) return ESP_OK;
  saveVerified();
  saveWufc();  // a key a Fetch now verified (the loop marked it); a no-op in FWD_TLS=0
  return sendFwd(req, nullptr);
}

static int slotParam(httpd_req_t* req) {
  char s[4];
  if (!queryParam(req, "slot", s, sizeof s) || s[0] < '0' || s[0] > '2' || s[1]) return -1;
  return s[0] - '0';
}

// PUT /api/admin/forwarding?slot=n -- a partial target; no slot or an unused one = a new target (the first free slot).
// {"remove":true} removes it (both NVS entries). A changed type/host/path/url clears the key.
static esp_err_t onFwdPut(httpd_req_t* req) {
  if (!adminGateReq(req)) return ESP_OK;
  Errors err;
  size_t len = 0;
  if (!readBody(req, &len)) {
    err.add("body", "missing or larger than 3072 bytes");
    return sendFwd(req, &err);
  }
  int slot = slotParam(req);
  xSemaphoreTake(gLock, portMAX_DELAY);
  if (slot < 0 || !gFwd[slot].used) {
    slot = -1;
    for (int i = 0; i < FWD_SLOTS && slot < 0; i++)
      if (!gFwd[i].used) slot = i;
  }
  FwdTarget before, t;  // the httpd task's stack (8 KB)
  if (slot >= 0) before = gFwd[slot];
  else memset(&before, 0, sizeof before);
  char firstId[STATION_ID_LEN] = "";
  if (Station* first = gStations.first()) memcpy(firstId, first->id, sizeof firstId);
  xSemaphoreGive(gLock);
  if (slot < 0) {
    err.add("slot", "three services already: remove one first");
    return sendFwd(req, &err);
  }
  t = before;
  FwdChange ch;
  fwdFromJson(bodyBuf(), len, &t, &err, &ch);
  memset(bodyBuf(), 0, len);  // the key was in the body
  char k[8];
  if (ch.remove && before.used) {
    nvsName(k, 'w', slot);
    nvsDelKey(k);
    nvsName(k, 'k', slot);
    nvsDelKey(k);
    xSemaphoreTake(gLock, portMAX_DELAY);
    memset(&gFwd[slot], 0, sizeof gFwd[slot]);
    xSemaphoreGive(gLock);
    portENTER_CRITICAL(&gFwdMux);
    gSched.reset(slot);
    memset(&gSched.s[slot].n, 0, sizeof gSched.s[slot].n);  // a new target in this slot starts at zero
    gSched.s[slot].ok_seen = 0, gSched.s[slot].sent_ever = false, gSched.s[slot].test_pending = false;
    memset(&gLast[slot], 0, sizeof gLast[slot]);
    memset(gCause[slot], 0, sizeof gCause[slot]);
    gVerGen[slot]++;
    if (gTest.state == 1 && gTest.slot == slot) gTest.state = 2, strcpy(gTest.result, "off");  // a test still running on this slot ends as "off"
    portEXIT_CRITICAL(&gFwdMux);
    logf("FWD", "target %d removed", slot);
  } else if (t.used) {
    if (!t.station[0]) memcpy(t.station, firstId, sizeof t.station);
    if (t.type == FwdType::WuCompat && !t.wc.host[0]) err.add("host", "required for a WU-compatible server");
    if (t.type == FwdType::Template && !t.url[0]) err.add("url", "required for a custom service");
    if (!before.used && err.n) {  // a new target is saved whole or not at all: no half target, no duplicate
      memset(ch.key, 0, sizeof ch.key);
      return sendFwd(req, &err);
    }
    if (ch.key_given && before.used) {  // the same key typed again is no change (nothing to re-check)
      char cur[FWD_KEY_MAX + 1];
      if (loadKey(slot, cur, sizeof cur) && strcmp(cur, ch.key) == 0) ch.key_given = false, memset(ch.key, 0, sizeof ch.key);
      memset(cur, 0, sizeof cur);
    }
    const bool keyDrops = before.used && !fwdKeyFollows(before, t);
    const bool unverified = fwdVerifyApply(before, &t, ch, &err);  // after the key rule's inputs are known
    const bool fresh = ch.any || ch.key_given || ch.key_clear || !before.used;
    if (fresh) {  // the generation moves before the new settings are written, never after
      portENTER_CRITICAL(&gFwdMux);
      gVerGen[slot]++;  // a test asked before this edit verifies nothing
      portEXIT_CRITICAL(&gFwdMux);
    }
    nvsName(k, 'k', slot);
    if (ch.key_given) nvsPutBlob(k, ch.key, strlen(ch.key));
    else if (ch.key_clear || keyDrops) nvsDelKey(k);
    memset(ch.key, 0, sizeof ch.key);
    if (!saveTarget(slot, t)) err.add("nvs", "not saved; applied until reboot");
    xSemaphoreTake(gLock, portMAX_DELAY);
    gFwd[slot] = t;
    xSemaphoreGive(gLock);
    if (fresh) {  // a fresh window; a pause is lifted
      portENTER_CRITICAL(&gFwdMux);
      gSched.reset(slot);
      portEXIT_CRITICAL(&gFwdMux);
    }
    uint32_t used = 0, free_e = 0;
    nvsStats(&used, &free_e);
    logf("FWD", "target %d %s: %s, %s, every %u s%s%s; NVS %lu used, %lu free", slot, before.used ? "changed" : "added",
         fwdTypeName(t.type), t.https ? "https" : "http", (unsigned)t.min_s, t.enabled ? ", on" : ", off",
         keyDrops && !ch.key_given ? ", key cleared" : "", (unsigned long)used, (unsigned long)free_e);
    if (unverified) logf("FWD", "target %d: settings changed, off until a send is accepted", slot);
  }
  return sendFwd(req, &err);
}

// POST /api/admin/forwarding/test?slot=n -> 202 {"state":"running"} (poll GET), 409 busy / off
static esp_err_t onFwdTestPost(httpd_req_t* req) {
  if (!adminGateReq(req)) return ESP_OK;
  const int slot = slotParam(req);
  xSemaphoreTake(gLock, portMAX_DELAY);
  const bool on = gBridge.forwarding && slot >= 0 && gFwd[slot].used;
  const uint16_t floor = slot >= 0 ? fwdMinFloor(gFwd[slot].type) : 60;
  xSemaphoreGive(gLock);
  portENTER_CRITICAL(&gFwdMux);
  const bool running = gTest.state == 1;
  const FwdTestReq r = running ? FwdTestReq::Busy : gSched.requestTest(slot, millis(), floor, on);
  if (r == FwdTestReq::Accepted || r == FwdTestReq::TooSoon) {
    memset(&gTest, 0, sizeof gTest);
    gTest.slot = (int8_t)slot;
    gTest.gen = gVerGen[slot];
    gTest.status = -1;
    gTest.state = r == FwdTestReq::Accepted ? 1 : 2;
    if (r == FwdTestReq::TooSoon) strcpy(gTest.result, "too_soon");
    if (gB) gB->test_target[0] = gB->test_body[0] = 0;  // no text of an earlier test (the loop writes them once it starts)
  }
  portEXIT_CRITICAL(&gFwdMux);
  if (r == FwdTestReq::Busy || r == FwdTestReq::Off) {
    httpd_resp_set_status(req, "409 Conflict");
    return sendJson(req, r == FwdTestReq::Busy ? "{\"error\":\"busy\"}" : "{\"error\":\"off\"}");
  }
  httpd_resp_set_status(req, "202 Accepted");
  return sendJson(req, r == FwdTestReq::Accepted ? "{\"state\":\"running\"}" : "{\"state\":\"done\"}");
}

static esp_err_t onFwdTestGet(httpd_req_t* req) {
  if (!adminGateReq(req)) return ESP_OK;
  saveVerified();
  return sendRendered(req, "application/json", [](Out& o, httpd_req_t*) {
    FwdTest x;  // the small fields under the mux; the target / body are stable once the state says done
    portENTER_CRITICAL(&gFwdMux);
    x.state = gTest.state, x.slot = gTest.slot, x.status = gTest.status, x.ms = gTest.ms, x.d = gTest.d;
    memcpy(x.result, gTest.result, sizeof x.result);
    memcpy(x.why, gTest.why, sizeof x.why);
    portEXIT_CRITICAL(&gFwdMux);
    o.printf("{\"state\":\"%s\",\"slot\":%d", x.state == 1 ? "running" : x.state == 2 ? "done" : "idle", x.slot);
    if (x.state == 2) {
      o.printf(",\"result\":\"%s\",\"status\":%d,\"ms\":%lu,\"target\":", x.result, x.status, (unsigned long)x.ms);
      o.jsonStr(gB ? gB->test_target : "");
      o.raw(",\"body\":");
      o.jsonStr(gB ? gB->test_body : "");
      if (x.why[0]) o.printf(",\"why\":\"%s\"", x.why);
      diagJson(o, x.d);
    }
    o.raw("}");
  }, false);
}

// POST /api/admin/forwarding/enable {"keep":"<id>"} -- forwarding on => one station, LAN uploads off.
// With two stations the other one is forgotten (config and history, as Forget) -- the sheet asks first, naming it.
static esp_err_t onFwdEnable(httpd_req_t* req) {
  if (!adminGateReq(req)) return ESP_OK;
  size_t len = 0;
  char keep[STATION_ID_LEN] = "";
  if (readBody(req, &len)) {
    JsonReader r(bodyBuf(), len);
    char k[8];
    if (r.beginObject())
      while (r.nextKey(k, sizeof k)) {
        if (strcmp(k, "keep") == 0 && r.peek() == '"') r.readString(keep, sizeof keep);
        else r.skipValue();
      }
  }
  char ids[MAX_STATIONS][STATION_ID_LEN] = {};
  const char* idp[MAX_STATIONS];
  int slots[MAX_STATIONS], n = 0;
  xSemaphoreTake(gLock, portMAX_DELAY);
  for (int i = 0; i < MAX_STATIONS; i++)
    if (Station* s = gStations.at(i)) {
      memcpy(ids[n], s->id, sizeof ids[n]);
      idp[n] = ids[n];
      slots[n++] = i;
    }
  xSemaphoreGive(gLock);
  const FwdEnablePlan p = fwdEnablePlan(idp, n, keep);
  if (!p.ok) {
    char b[96];
    snprintf(b, sizeof b, "{\"ok\":false,\"errors\":[\"%s\"]}", p.why);
    return sendJson(req, b);
  }
  // At one station (the WROOM-32 profile) there is nobody to forget: this branch folds away and the call only switches on
  if (MAX_STATIONS > 1 && p.forget >= 0) {
    requestForget(slots[p.forget]);
    logf("ADMIN", "forwarding on: station %s forgotten (kept %s)", ids[p.forget], keep);
    // targets of the forgotten station follow the kept one
    for (int i = 0; i < FWD_SLOTS; i++) {
      FwdTarget t;
      xSemaphoreTake(gLock, portMAX_DELAY);
      t = gFwd[i];
      const bool moves = t.used && strcmp(t.station, ids[p.forget]) == 0;
      if (moves) memcpy(gFwd[i].station, keep, sizeof gFwd[i].station);
      t = gFwd[i];
      xSemaphoreGive(gLock);
      if (!moves) continue;
      saveTarget(i, t);
      logf("FWD", "target %d now sends station %s", i, keep);
    }
  }
  Errors e;
  static const char ON[] = "{\"forwarding\":true}";
  xSemaphoreTake(gLock, portMAX_DELAY);
  BridgeConfig b = gBridge;
  bridgeFromJson(ON, sizeof ON - 1, &b, &e, p.forget >= 0 ? n - 1 : n);  // sets max_stations 1, lan_uploads off
  gBridge = b;
  xSemaphoreGive(gLock);
  const bool saved = saveBridge();
  // The forgotten station's slot stays reserved until a restart, so the memory this frees needs one (the sheet says so)
  const bool restart = p.forget >= 0 && !e.n && saved;
  logf("ADMIN", "forwarding on (max_stations 1, LAN uploads off)%s%s", saved ? "" : " -- NOT saved",
       restart ? "; restarting to free the second station's memory" : "");
  if (restart) sysPlanRestart("forwarding", 3000);
  return sendJson(req, e.n       ? "{\"ok\":false,\"errors\":[\"forwarding: not switched on\"]}"
                       : restart ? "{\"ok\":true,\"restart\":true}"
                       : saved   ? "{\"ok\":true}"
                                 : "{\"ok\":true,\"errors\":[\"nvs: not saved; applied until reboot\"]}");
}

// The main airport's admitted report for the outlook at `now` (the station's last upload); the caller holds
// gLock. False (nothing admitted) in a FWD_TLS=0 build (no metar / airport symbol there: CI's nm gate).
bool outlookNearInput(uint32_t now, OutNear* out) {
#if FWD_TLS
  *out = nearFromStore(gMetarCfg, gMetarStore, gBridge.lat, gBridge.lon, now);
#else
  (void)now;
  *out = OutNear{};
#endif
  return out->ok;
}

// "METAR eligible" -- the module on, the location set, at least one chosen airport within 60 km
// (metarEligible); the switch between baro_ref and baro_check, LearnUse.metar. The caller holds gLock; false
// in a FWD_TLS=0 build (no METAR symbol there)
#if FWD_TLS
bool metarEligibleLocked() { return metarEligible(gMetarCfg, gBridge.lat, gBridge.lon); }  // FWD_TLS=0 has the inline (app.h)
#endif

#if FWD_TLS
// The dashboard's plane tab (/api/bridge/status `metar`); the caller holds gLock. FWD_TLS builds only (a
// FWD_TLS=0 build carries no METAR / airport symbol, CI's nm gate)
bool metarShownLocked() { return gMetarCfg.on && gMetarCfg.n > 0; }

// The parts of /api/metar's ETag -- the store's change counter and a signature of the settings
// (on, the airports and their order); app_http.cpp adds a 15-min clock bucket (stale flags and `current` move with time)
void metarEtagParts(uint32_t* gen, uint32_t* cfg, uint32_t* now) {
  *now = realNow() ? realNow() : nowS();  // the clock /api/metar renders with
  xSemaphoreTake(gLock, portMAX_DELAY);
  *gen = gMetarStore.gen;
  uint32_t h = 2166136261u;  // FNV-1a over the settings blob
  const uint8_t* p = (const uint8_t*)&gMetarCfg;
  for (size_t i = 0; i < sizeof gMetarCfg; i++) h = (h ^ p[i]) * 16777619u;
  xSemaphoreGive(gLock);
  *cfg = h;
}

// ---------------------------------------------------------------- METAR routes
// GET /api/metar (public, LAN face; the AP face gets the catch-all in app_http.cpp): the readings + the reference.
esp_err_t metarPublicGet(httpd_req_t* req) {
  return sendRendered(req, "application/json", [](Out& o, httpd_req_t*) {
    MetarCfg c;  // copied under gLock, rendered after it (the httpd task's 8-KB stack, not globals)
    MetarStore s;
    MetarStationView sv{};
    const uint32_t ms = millis();
    xSemaphoreTake(gLock, portMAX_DELAY);
    c = gMetarCfg;
    s = gMetarStore;
    sv.lat = gBridge.lat, sv.lon = gBridge.lon;
    // the first station with a pressure reading -- the one console of the 1-station profile;
    // on the 2-station PSRAM profile the sheet compares that first station only (the tile notices stay per station)
    for (int i = 0; i < MAX_STATIONS && !sv.ok; i++) {
      const Station* st = gStations.at(i);
      // an offline console's last reading is not compared with the current QNH
      if (!st || st->baro_uncal || !st->online(ms) || !((st->live.present >> F_PRESSURE_HPA) & 1u)) continue;
      sv.ok = true;
      sv.p_hpa = st->live.v[F_PRESSURE_HPA];
      sv.offset_hpa = st->cfg.pressure_hpa_offset;
      sv.active = relShown(st->rel, st->last_seen);
    }
    xSemaphoreGive(gLock);
    metarJson(o, c, s, c.on, realNow() ? realNow() : nowS(), &sv);
  }, false);
}

static const char* const RUN_ST[] = {"idle", "waiting", "running", "done"};
// GET /api/admin/metar: the settings, the source's state and counters, the last job's diag (R&D), the lookup and run
static esp_err_t onMetarAdminGet(httpd_req_t* req) {
  if (!adminGateReq(req)) return ESP_OK;
  if (gMetarSave.exchange(false)) {  // names a lookup refreshed (the loop changed RAM only)
    xSemaphoreTake(gLock, portMAX_DELAY);
    const MetarCfg c = gMetarCfg;
    xSemaphoreGive(gLock);
    logf("ADMIN", "airport names from the lookup%s", nvsPutBlob("metar", &c, sizeof c) ? "" : " -- NOT saved");
  }
  return sendRendered(req, "application/json", [](Out& o, httpd_req_t*) {
    MetarCfg c;  // copies on the httpd task's stack (≈ 0.5 KB), not globals
    MetarLookup lk;
    MetarLastJob x;
    MetarCounters n;
    [[maybe_unused]] uint16_t hist[6];  // bench only
    xSemaphoreTake(gLock, portMAX_DELAY);
    c = gMetarCfg;
    int32_t lat, lon;
    const bool loc = metarLocation(&lat, &lon);
    char dropped[65];
    memcpy(dropped, gMetarStore.dropped, sizeof dropped);
    xSemaphoreGive(gLock);
    portENTER_CRITICAL(&gFwdMux);
    lk = gLook;
    x = gMetarLast;
    n = gMetar.n;
    BENCH(memcpy(hist, gHttpIdleHist, sizeof hist);)
    const MetarRun run = gRun;
    const long next = gMetar.nextS(millis());
    const char* state = gMetar.stateName();
    const uint16_t lastWhy = gMetar.lastWhy();
    portEXIT_CRITICAL(&gFwdMux);
    o.printf("{\"available\":true,\"on\":%s,\"location\":%s,\"time\":%s,\"airports\":[", c.on ? "true" : "false",
             loc ? "true" : "false", outlookTimeOk() ? "true" : "false");
    for (int i = 0; i < c.n; i++) {
      o.raw(i ? ",{\"icao\":" : "{\"icao\":");
      o.jsonStr(c.a[i].icao);
      o.raw(",\"name\":");
      o.jsonStr(c.a[i].name);
      if (metarNameUser(c, i)) o.raw(",\"own\":true");  // the user typed this name
      char cc[3];  // the row's flag
      if (airportCountry(c.a[i].icao, cc, nullptr)) o.printf(",\"cc\":\"%s\"", cc);
      if (c.a[i].km == AIRPORT_KM_UNKNOWN) o.raw(",\"km\":null}");
      else o.printf(",\"km\":%u}", (unsigned)c.a[i].km);
    }
    o.printf("],\"state\":\"%s\",\"next_s\":%ld,\"wait_why\":", state, next);
    if (lastWhy) o.printf("\"%s\"", lastWhy == FW_WHY_SEND_SOON ? "send_soon" : fwdBusyName(lastWhy));
    else o.raw("null");
    metarCountersJson(o, n);  // split under Out's 191-char line
    BENCH(o.printf(",\"http_idle_hist\":[%u,%u,%u,%u,%u,%u]", hist[0], hist[1], hist[2], hist[3], hist[4], hist[5]);)
    o.printf(",\"run\":{\"state\":\"%s\",\"result\":", RUN_ST[run.state & 3]);
    if (run.state == 3) o.jsonStr(run.result);
    else o.raw("null");
    o.printf("},\"lookup\":{\"state\":\"%s\",\"near\":%s,\"result\":", RUN_ST[lk.state & 3], lk.near ? "true" : "false");
    if (lk.state == 3) o.jsonStr(lk.result);
    else o.raw("null");
    o.raw(",\"list\":[");
    for (int i = 0; lk.state == 3 && i < lk.n; i++) {
      o.raw(i ? ",{\"icao\":" : "{\"icao\":");
      o.jsonStr(lk.a[i].icao);
      o.raw(",\"name\":");
      o.jsonStr(lk.a[i].name);
      char cc[3];
      if (airportCountry(lk.a[i].icao, cc, nullptr)) o.printf(",\"cc\":\"%s\"", cc);
      if (lk.a[i].km == AIRPORT_KM_UNKNOWN) o.raw(",\"km\":null");
      else o.printf(",\"km\":%u", (unsigned)lk.a[i].km);
      o.printf(",\"taf\":%s}", (lk.taf_mask >> i) & 1 ? "true" : "false");
    }
    o.raw("]},\"last\":");
    if (!x.t) {
      o.raw("null");
    } else {
      o.raw("{\"t\":");
      isoOrNull(o, x.t);
      o.printf(",\"kind\":\"%s\",\"result\":\"%s\",\"status\":%d,\"why\":", x.kind == FwdKind::Lookup ? "lookup"
                                                                              : x.kind == FwdKind::RunOnce ? "run_once"
                                                                              : x.taf                      ? "taf"
                                                                                                           : "fetch",
               metarOutcomeName(x.out), (int)x.status);
      o.jsonStr(x.why);
#if FC_DIAG_BENCH
      if (x.http_idle_ms == UINT32_MAX) o.raw(",\"http_idle_ms\":null");
      else o.printf(",\"http_idle_ms\":%lu", (unsigned long)x.http_idle_ms);
      o.printf(",\"http_during\":%u", x.http_during);
#endif
      o.printf(",\"lines\":%u,\"garbage\":%u,\"unwanted\":%u,\"nil\":%u,\"old\":%u,\"field_bad\":%u,"
               "\"trunc\":%u,\"dropped\":", x.lines, x.garbage, x.unwanted, x.nil, x.old, x.field_bad, x.trunc);
      o.jsonStr(dropped);
      diagJson(o, x.d);
      o.raw("}");
    }
    o.raw("}");
  }, false);
}

// POST /api/admin/metar {"on","airports":[...]}: the airports from the chosen list or the last lookup (their names,
// positions and distances come from there); saved as the v1 blob; a change of airports restarts the schedule
static esp_err_t onMetarAdminPost(httpd_req_t* req) {
  if (!adminGateReq(req)) return ESP_OK;
  size_t len = 0;
  if (!readBody(req, &len)) {  // too big (drained, answered, then closed) or cut: the same error as every admin PUT
    httpd_resp_set_status(req, "400 Bad Request");
    return sendJson(req, "{\"errors\":[\"body: missing or larger than 3072 bytes\"]}");
  }
  MetarLookup lk;
  portENTER_CRITICAL(&gFwdMux);
  lk = gLook;
  portEXIT_CRITICAL(&gFwdMux);
  if (lk.state != 3) lk.n = 0;
  xSemaphoreTake(gLock, portMAX_DELAY);
  const MetarCfg before = gMetarCfg;
  xSemaphoreGive(gLock);
  MetarCfg next;
  const char* err = nullptr;
  if (!metarCfgFromJson(bodyBuf(), len, before, lk, &next, &err)) {
    httpd_resp_set_status(req, "400 Bad Request");
    char b[96];
    snprintf(b, sizeof b, "{\"errors\":[\"%s\"]}", err ? err : "bad request");
    return sendJson(req, b);
  }
  bool moved = next.n != before.n;
  for (int i = 0; !moved && i < next.n; i++) moved = strcmp(next.a[i].icao, before.a[i].icao) != 0;
  xSemaphoreTake(gLock, portMAX_DELAY);
  gMetarCfg = next;
  if (moved) metarStoreRemap(&gMetarStore, before, next);
  if (!next.on || moved) relResetAll();  // module off ends the REL hint now; so does a changed airport list
  xSemaphoreGive(gLock);
  // A pure reorder (the same airports) only remaps the readings; the schedule restarts, and a pause lifts, only
  // when the set of airports changed
  if (moved && !metarSameSet(before, next)) {
    portENTER_CRITICAL(&gFwdMux);
    gMetar.airportsChanged();
    portEXIT_CRITICAL(&gFwdMux);
  }
  const bool saved = nvsPutBlob("metar", &next, sizeof next);
  logf("ADMIN", "airport weather %s, %u airport(s)%s", next.on ? "on" : "off", (unsigned)next.n,
       saved ? "" : " -- NOT saved");
  return sendJson(req, saved ? "{\"ok\":true}" : "{\"ok\":true,\"errors\":[\"nvs: not saved; applied until reboot\"]}");
}

static esp_err_t metarUserReply(httpd_req_t* req, MetarReq r) {
  if (r == MetarReq::Accepted) {
    httpd_resp_set_status(req, "202 Accepted");
    return sendJson(req, "{\"state\":\"waiting\"}");
  }
  httpd_resp_set_status(req, "409 Conflict");
  char b[48];
  snprintf(b, sizeof b, "{\"error\":\"%s\"}", metarReqName(r));
  return sendJson(req, b);
}

// POST /api/admin/metar/run: one fetch now (user priority; >= 60 s after the previous METAR job; lifts a pause)
static esp_err_t onMetarRun(httpd_req_t* req) {
  if (!adminGateReq(req)) return ESP_OK;
  xSemaphoreTake(gLock, portMAX_DELAY);
  const bool usable = gMetarCfg.on && gMetarCfg.n > 0 && outlookTimeOk();
  xSemaphoreGive(gLock);
  portENTER_CRITICAL(&gFwdMux);
  const MetarReq r = gMetar.requestRun(millis(), usable);
  if (r == MetarReq::Accepted) gRun.state = 1, gRun.result[0] = 0;
  portEXIT_CRITICAL(&gFwdMux);
  return metarUserReply(req, r);
}

// POST /api/admin/metar/lookup?near=1 | ?ids=XXXX: the airport list (near the configured location, the nearest
// five within about 120 km; a typed code works anywhere). Without a location "near" answers 409 no_location.
static esp_err_t onMetarLookup(httpd_req_t* req) {
  if (!adminGateReq(req)) return ESP_OK;
  char ids[8] = "", nearQ[4] = "";
  const bool nearMe = queryParam(req, "near", nearQ, sizeof nearQ) && nearQ[0] == '1';
  if (!nearMe) {
    bool ok = queryParam(req, "ids", ids, sizeof ids) && strlen(ids) == 4;
    for (int i = 0; ok && i < 4; i++) {
      if (ids[i] >= 'a' && ids[i] <= 'z') ids[i] = (char)(ids[i] - 32);
      ok = (ids[i] >= 'A' && ids[i] <= 'Z') || (ids[i] >= '0' && ids[i] <= '9');
    }
    if (!ok) {
      httpd_resp_set_status(req, "400 Bad Request");
      return sendJson(req, "{\"error\":\"ids: 4 characters A-Z 0-9\"}");
    }
  }
  int32_t lat, lon;
  xSemaphoreTake(gLock, portMAX_DELAY);
  const bool loc = metarLocation(&lat, &lon);
  const bool usable = gMetarCfg.on && outlookTimeOk();
  xSemaphoreGive(gLock);
  if (nearMe && !loc) {
    httpd_resp_set_status(req, "409 Conflict");
    return sendJson(req, "{\"error\":\"no_location\"}");
  }
  portENTER_CRITICAL(&gFwdMux);
  const MetarReq r = gMetar.requestLookup(millis(), usable);
  if (r == MetarReq::Accepted) {
    memset(&gLook, 0, sizeof gLook);
    gLook.state = 1;
    gLook.near = nearMe;
    memcpy(gLook.ids, ids, 4);
  }
  portEXIT_CRITICAL(&gFwdMux);
  return metarUserReply(req, r);
}
#endif

#if FWD_TLS
// The httpd task saves the settings a Fetch now changed in RAM (never an NVS write on the loop tick)
static void saveWufc() {
  if (!gWufcSave.exchange(false)) return;
  xSemaphoreTake(gLock, portMAX_DELAY);
  const WufcCfg c = gWufcCfg;
  xSemaphoreGive(gLock);
  logf("WUFC", "key verified%s", nvsPutBlob("wufc", &c, sizeof c) ? "" : " (not saved; until reboot)");
}

// POST /api/admin/wufc {"on":bool, "key":"<32 hex>"|""|null}: an empty or missing key = unchanged, null clears it. A new
// key is stored write-only in NVS `wk` (never in a GET, a log, MQTT or an export) and clears `verified`; "on" needs a
// verified key (Fetch now first). Any change of the key or the switch restarts the schedule, lifts the
// pauses and (switching off, a key change) drops the shown line.
static esp_err_t onWufcPost(httpd_req_t* req) {
  if (!adminGateReq(req)) return ESP_OK;
  size_t len = 0;
  if (!readBody(req, &len)) {  // too big (drained, answered, then closed) or cut: the same error as every admin PUT
    httpd_resp_set_status(req, "400 Bad Request");
    return sendJson(req, "{\"errors\":[\"body: missing or larger than 3072 bytes\"]}");
  }
  JsonReader r(bodyBuf(), len);
  char k[8], key[WUFC_KEY_MAX + 2] = "";
  int on = -1;
  bool keyGiven = false, keyClear = false, ok = r.beginObject();
  while (ok && r.nextKey(k, sizeof k)) {
    if (!strcmp(k, "on")) {
      bool v = false;
      ok = r.readBool(&v);
      on = v ? 1 : 0;
    } else if (!strcmp(k, "key")) {
      if (r.peek() == 'n') r.skipValue(), keyClear = true;
      else ok = r.readString(key, sizeof key), keyGiven = key[0] != 0;
    } else {
      r.skipValue();
    }
  }
  memset(bodyBuf(), 0, len);  // the key was in the body
  const char* err = !ok ? "not a JSON object" : keyGiven && !wufcKeyOk(key) ? "key_format" : nullptr;  // codes; the sheet words them
  xSemaphoreTake(gLock, portMAX_DELAY);
  const WufcCfg before = gWufcCfg;
  const bool verifiedAfter = !(keyGiven || keyClear) && before.verified;
  if (!err && on == 1 && !before.on && !verifiedAfter) err = "not_verified";
  xSemaphoreGive(gLock);
  if (err) {
    memset(key, 0, sizeof key);
    httpd_resp_set_status(req, "400 Bad Request");
    char b[64];
    snprintf(b, sizeof b, "{\"errors\":[\"%s\"]}", err);
    return sendJson(req, b);
  }
  bool saved = true;
  if (keyGiven) saved = nvsPutBlob("wk", key, strlen(key));
  else if (keyClear) nvsDelKey("wk");
  memset(key, 0, sizeof key);
  WufcCfg next = before;
  if (on >= 0) next.on = (uint8_t)on;
  if (keyGiven || keyClear) next.verified = 0;  // a new key is checked again (Fetch now) before the switch opens
  if (!next.verified) next.on = 0;
  saved = nvsPutBlob("wufc", &next, sizeof next) && saved;
  const bool changed = keyGiven || keyClear || next.on != before.on;
  portENTER_CRITICAL(&gFwdMux);
  const bool paused = gWufc.pause() != WufcPause::None;  // "Save to try again" -- any accepted save lifts a pause
  portEXIT_CRITICAL(&gFwdMux);
  xSemaphoreTake(gLock, portMAX_DELAY);
  gWufcCfg = next;
  if (changed || paused) gWufcEpoch++;
  if (keyGiven && saved) gWufcKeySet = true;  // only a key the next boot will have
  if (keyClear) gWufcKeySet = false;
  if (changed && (!next.on || keyGiven || keyClear)) wufcDrop(&gWufcStore);
  xSemaphoreGive(gLock);
  if (changed || paused) {
    portENTER_CRITICAL(&gFwdMux);
    gWufc.keyChanged();
    portEXIT_CRITICAL(&gFwdMux);
  }
  logf("ADMIN", "forecast %s, key %s%s", next.on ? "on" : "off", keyGiven ? "changed" : keyClear ? "cleared" : "unchanged",
       saved ? "" : " -- NOT saved");
  return sendJson(req, saved ? "{\"ok\":true}" : "{\"ok\":true,\"errors\":[\"nvs: not saved; applied until reboot\"]}");
}

// POST /api/admin/wufc/run: "Fetch now" (user priority; >= 60 s after the previous pull; the switch may be off)
// Never refused for a busy line -- queued (one pending flag; a second press joins it) and answered at
// once with an estimate; refused only without key / location / time (409 off) or below the heap gate on an idle line
// (503 low_memory, a status the sheet tells apart without the body; while another job runs its buffers
// hold the heap, so the press is queued and nqPick's start gate protects the job). The result comes with the forwarding GET.
static esp_err_t onWufcRun(httpd_req_t* req) {
  if (!adminGateReq(req)) return ESP_OK;
  xSemaphoreTake(gLock, portMAX_DELAY);
  const bool usable = gWufcKeySet && gBridge.lat == gBridge.lat && gBridge.lon == gBridge.lon && outlookTimeOk();
  xSemaphoreGive(gLock);
  NqCand gate{};
  nqHeapGate(true, FwdTlsClass::Wufc, gate);
  const HeapNow h = heapNow();
  portENTER_CRITICAL(&gFwdMux);
  const bool lineBusy = wufcLineBusy();
  const bool heapOk = lineBusy || (h.free8 >= gate.need8 && h.largest8 >= gate.largest8);
  const bool joined = gWufc.runPending() || gWufc.runningKind() == FwdKind::WuRun;
  const MetarReq r = gWufc.requestRun(millis(), usable, heapOk);
  if (r == MetarReq::Accepted && !joined) gWuRun.state = 1, gWuRun.result[0] = 0;
  const uint32_t eta = gWufc.etaS(millis(), lineBusy, gWuHeld);
  portEXIT_CRITICAL(&gFwdMux);
  char b[56];
  if (r == MetarReq::Accepted) {
    httpd_resp_set_status(req, "202 Accepted");
    snprintf(b, sizeof b, "{\"ok\":true,\"queued\":true,\"eta_s\":%lu}", (unsigned long)eta);
    return sendJson(req, b);
  }
  httpd_resp_set_status(req, r == MetarReq::LowMemory ? "503 Service Unavailable" : "409 Conflict");
  snprintf(b, sizeof b, "{\"error\":\"%s\"}", metarReqName(r));
  return sendJson(req, b);
}

// The /api/live object and the HA sensor read the store through these (gLock held by the caller for the first)
bool wufcSnap(WufcStore* out) {
  if (!gWufcCfg.on) return false;
  *out = gWufcStore;
  return true;
}
uint32_t wufcEtagWord() {  // gLock held: the store's generation and what is shown now (stale, label)
  const uint32_t now = realNow();
  if (!gWufcCfg.on) return 0;
  const char* d = wufcDay(gWufcStore, now, gTz);
  return (gWufcStore.gen * 2654435761u) ^ (d ? (uint32_t)d[2] : 0u) ^ 0x5a17u;  // 'm' / 'd' tell the two labels apart
}
#if FC_HA_WUFC  // the HA sensor is compiled out (FC_HA_WUFC=0); the code is kept for a later release
bool wufcOn() {
  xSemaphoreTake(gLock, portMAX_DELAY);
  const bool on = gWufcCfg.on;
  xSemaphoreGive(gLock);
  return on;
}
// HA: the state (the phrase, "None" while nothing is shown) and the attributes; a signature of both (0 = off)
uint32_t wufcHa(char* state, size_t sc, char* attr, size_t ac) {
  xSemaphoreTake(gLock, portMAX_DELAY);
  const bool on = gWufcCfg.on;
  const WufcStore s = gWufcStore;
  const PosixTz tz = gTz;
  xSemaphoreGive(gLock);
  if (!on) return 0;
  const uint32_t now = realNow();
  const char* d = wufcDay(s, now, tz);
  snprintf(state, sc, "%s", d ? s.d.phrase : "None");
  if (wufcAttributes(attr, ac, s, now, tz) < 0) snprintf(attr, ac, "{}");
  return (s.gen * 2654435761u) ^ (d ? (uint32_t)d[2] : 7u) ^ 1u;
}
#endif
#else
bool wufcSnap(WufcStore*) { return false; }
uint32_t wufcEtagWord() { return 0; }
bool wufcOn() { return false; }
uint32_t wufcHa(char*, size_t, char*, size_t) { return 0; }
static void saveWufc() {}
#endif

void registerFwd(httpd_handle_t srv) {
  const httpd_uri_t uris[] = {
      {"/api/admin/forwarding", HTTP_GET, onFwdGet, nullptr},
      {"/api/admin/forwarding", HTTP_PUT, onFwdPut, nullptr},
      {"/api/admin/forwarding/test", HTTP_POST, onFwdTestPost, nullptr},
      {"/api/admin/forwarding/test", HTTP_GET, onFwdTestGet, nullptr},
      {"/api/admin/forwarding/enable", HTTP_POST, onFwdEnable, nullptr},
#if FWD_TLS
      {"/api/admin/metar", HTTP_GET, onMetarAdminGet, nullptr},
      {"/api/admin/metar", HTTP_POST, onMetarAdminPost, nullptr},
      {"/api/admin/metar/run", HTTP_POST, onMetarRun, nullptr},
      {"/api/admin/metar/lookup", HTTP_POST, onMetarLookup, nullptr},
      {"/api/admin/wufc", HTTP_POST, onWufcPost, nullptr},
      {"/api/admin/wufc/run", HTTP_POST, onWufcRun, nullptr},
#endif
  };
  registerUris(srv, uris, sizeof uris / sizeof uris[0]);
}

}  // namespace app
