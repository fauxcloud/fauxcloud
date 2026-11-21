// app_capture.cpp -- "Listen to a new console" (see src/capture.h): the start / stop order executed in the loop task,
// the buffers that exist only while the mode runs, the peek listeners on the lwIP raw API, the admin routes.
//
// Who touches what:
//   loop task     the machine's actions (captureTick), peek classification into the ring, the heap sample
//   httpd task    the start / stop / poll presses, port-80 records (captureRecord), the GET's snapshot
//   udp53_123     DNS names and NTP (captureDns / captureNtp)
//   Wi-Fi events  joins, leases, leaves (captureAp*)
//   tcpip thread  the peek callbacks: they copy at most CAPTURE_PEEK_MAX bytes into the peek buffer, fill the handoff
//                 slot and close; nothing else (no printf, no lock, no heap: the thread has a 4 KB stack and every
//                 packet of the board waits behind it)
// The ring, the tables and the counters are guarded by gDataMux (short critical sections, no printf inside); the
// machine by gMachMux. The buffers are freed only after the listeners and a live peek are gone (closed under the
// lwIP core lock, so no callback runs after), and never while a GET renders from the snapshot or a port-80 request
// uses the httpd scratch (both on the one httpd task: one flag covers both blocks).
#include <WiFi.h>
#include <esp_wifi.h>
#include <lwip/priv/tcp_priv.h>  // tcp_active_pcbs / tcp_tw_pcbs for the stop line's count (pinned core 3.3.12)
#include <lwip/tcp.h>
#include <lwip/tcpip.h>
#include <lwip/timeouts.h>

#include "app.h"
#include "src/capture.h"
#include "src/json.h"

namespace app {

// ---------------------------------------------------------------- state
static CaptureMachine gM;
static portMUX_TYPE gMachMux = portMUX_INITIALIZER_UNLOCKED;
static std::atomic<bool> gQuiet{false}, gMqttOffF{false}, gOnF{false};

// The buffers (allocated at the start, after the MQTT client stopped; freed at the stop)
struct CapAux {
  CaptureDns dns;
  CaptureClients clients;
  uint32_t port_hits[CAPTURE_PORT_N];
};
struct CapPeek {
  uint8_t buf[CAPTURE_PEEK_MAX];
  CapUnit u;  // the loop task's records of one peek (not on its 8 KB stack)
};
struct CapSnap {  // what a GET renders, copied under gDataMux
  CaptureRec recs[CAPTURE_GET_MAX];
  CaptureDns dns;
  CaptureClients clients;
};
// the start's heap gate: the four blocks + the margin every heavy job keeps; the ring is the largest block
// (five blocks: the httpd scratch is the fifth, with the snapshot the httpd task's; one "in use" flag covers both)
static constexpr uint32_t CAPTURE_NEED8 = (uint32_t)(sizeof(CaptureRing) + sizeof(CapAux) + sizeof(CapPeek) + sizeof(CapSnap) +
                                                     sizeof(CapHttpScratch)) + CAPTURE_MARGIN8;
static_assert(sizeof(CaptureRing) <= CAPTURE_LARGEST_MIN && sizeof(CapAux) <= CAPTURE_LARGEST_MIN &&
                  sizeof(CapPeek) <= CAPTURE_LARGEST_MIN && sizeof(CapSnap) <= CAPTURE_LARGEST_MIN &&
                  sizeof(CapHttpScratch) <= CAPTURE_LARGEST_MIN,
              "the gate's largest block holds every block");
static CaptureRing* gRing = nullptr;
static CapAux* gAux = nullptr;
static CapPeek* gPeek = nullptr;
static CapSnap* gSnap = nullptr;
static CapHttpScratch* gHttp = nullptr;  // with gSnap the httpd task's (gSnapUse covers both)
static bool gSnapUse = false, gSnapFreeLater = false;
static portMUX_TYPE gDataMux = portMUX_INITIALIZER_UNLOCKED;

// Counters that outlive the buffers (the GET after the stop, the stop line); under gDataMux
static struct {
  uint32_t ntp_asked, ntp_answered, skipped, seq, lost, written, started_utc, elapsed_s, joined;
  uint32_t port_hits[CAPTURE_PORT_N];  // peeks read per extra port
  bool ntp_recorded;
  char vendor[9];
} gC;
static std::atomic<uint32_t> gSkipped{0};

// The start's heap and the capture window's minima (loop task)
static uint32_t gFree0 = 0, gLargest0 = 0, gMin8 = UINT32_MAX, gMinLargest = UINT32_MAX;
static bool gOfflineDone = false;
static uint32_t gOnMs = 0;
// the lowest free stack (bytes, since boot: FreeRTOS keeps no other) of the two tasks that classify, for the stop line
static std::atomic<uint32_t> gStackHttpd{UINT32_MAX}, gStackLoop{UINT32_MAX};

bool captureQuiet() { return gQuiet.load(); }
bool captureMqttOff() { return gMqttOffF.load(); }
bool captureOn() { return gOnF.load(); }
void captureCountSkipped() { gSkipped++; }

static void syncFlags() {  // gMachMux held
  gQuiet = gM.quiet();
  gMqttOffF = gM.mqttOff();
  gOnF = gM.on();
}

// read under gDataMux by the record writers: once gRing is published, gOnMs is the run's (set before allocAll publishes)
static uint32_t sinceOn() { return capSince(millis(), gOnMs); }

// ---------------------------------------------------------------- the peek listeners (lwIP raw API)
// One live peek at a time (one 512-B buffer): a connection while one is read is closed and counted per port.
enum : uint8_t { PK_FREE = 0, PK_READING = 1, PK_READY = 2 };
static std::atomic<uint8_t> gSlotSt{PK_FREE};
static struct {
  struct tcp_pcb* pcb;
  int port_i;
  uint32_t ip;
  size_t len;
  bool cut;
} gSlot;
static std::atomic<uint32_t> gPeekBusy[CAPTURE_PORT_N];
static struct tcp_pcb* gListen[CAPTURE_PORT_N];

static void peekTimeout(void*);
// tcpip thread: the slot is complete. Returns true when the pcb had to be aborted (the caller returns ERR_ABRT).
static bool peekDone() {
  struct tcp_pcb* p = gSlot.pcb;
  gSlot.pcb = nullptr;
  sys_untimeout(peekTimeout, nullptr);
  bool aborted = false;
  if (p) {
    tcp_arg(p, nullptr);
    tcp_recv(p, nullptr);
    tcp_err(p, nullptr);
    if (tcp_close(p) != ERR_OK) {
      tcp_abort(p);
      aborted = true;
    }
  }
  gSlotSt = PK_READY;
  return aborted;
}
static void peekTimeout(void*) {
  if (gSlotSt.load() == PK_READING && gSlot.pcb) peekDone();
}
static void peekErr(void*, err_t) {  // the pcb is already freed by lwIP
  gSlot.pcb = nullptr;
  sys_untimeout(peekTimeout, nullptr);
  if (gSlotSt.load() == PK_READING) gSlotSt = PK_READY;
}
static err_t peekRecv(void*, struct tcp_pcb* p, struct pbuf* pb, err_t) {
  if (!pb) return peekDone() ? ERR_ABRT : ERR_OK;  // the peer closed
  if (gPeek && gSlot.pcb == p) {
    const size_t room = CAPTURE_PEEK_MAX - gSlot.len;
    const size_t k = pb->tot_len < room ? pb->tot_len : room;
    if (k) pbuf_copy_partial(pb, gPeek->buf + gSlot.len, (u16_t)k, 0);
    gSlot.len += k;
    if (pb->tot_len > room) gSlot.cut = true;
  }
  tcp_recved(p, pb->tot_len);
  pbuf_free(pb);
  if (gSlot.len >= CAPTURE_PEEK_MAX) return peekDone() ? ERR_ABRT : ERR_OK;
  return ERR_OK;
}
static err_t peekAccept(void* arg, struct tcp_pcb* np, err_t err) {
  const int i = (int)(intptr_t)arg;
  if (err != ERR_OK || !np) return ERR_VAL;
  uint8_t expect = PK_FREE;
  if (!gPeek || !gSlotSt.compare_exchange_strong(expect, PK_READING)) {
    if (i >= 0 && i < CAPTURE_PORT_N) gPeekBusy[i]++;
    tcp_abort(np);
    return ERR_ABRT;
  }
  gSlot.pcb = np;
  gSlot.port_i = i;
  gSlot.ip = ip_2_ip4(&np->remote_ip)->addr;
  gSlot.len = 0;
  gSlot.cut = false;
  tcp_arg(np, nullptr);
  tcp_recv(np, peekRecv);
  tcp_err(np, peekErr);
  sys_timeout(CAPTURE_PEEK_MS, peekTimeout, nullptr);
  return ERR_OK;
}

// Returns how many listen; `failed` names the ports that did not (" :443 bind -8"), for the start line
static int openListeners(char* failed, size_t cap) {
  int n = 0;
  size_t fn = 0;
  failed[0] = 0;
  ip_addr_t a;
  ip_addr_set_ip4_u32(&a, apIpNet());
  LOCK_TCPIP_CORE();
  for (int i = 0; i < CAPTURE_PORT_N; i++) {
    gPeekBusy[i] = 0;
    struct tcp_pcb* p = tcp_new_ip_type(IPADDR_TYPE_V4);
    if (!p) {
      if (fn < cap) fn += (size_t)snprintf(failed + fn, cap - fn, " :%u pcb", CAPTURE_PORTS[i]);
      continue;
    }
    // the last capture's peeks closed from this side sit in TIME_WAIT on these ports for 2 x MSL (120 s): without
    // the reuse flag a Start within two minutes cannot bind them (bench 2026-11-21: 443, 8080, 5000 refused)
    ip_set_option(p, SOF_REUSEADDR);
    const err_t be = tcp_bind(p, &a, CAPTURE_PORTS[i]);
    if (be != ERR_OK) {
      if (fn < cap) fn += (size_t)snprintf(failed + fn, cap - fn, " :%u bind %d", CAPTURE_PORTS[i], (int)be);
      tcp_close(p);
      continue;
    }
    struct tcp_pcb* l = tcp_listen_with_backlog(p, 1);
    if (!l) {
      if (fn < cap) fn += (size_t)snprintf(failed + fn, cap - fn, " :%u listen", CAPTURE_PORTS[i]);
      tcp_close(p);
      continue;
    }
    tcp_arg(l, (void*)(intptr_t)i);
    tcp_accept(l, peekAccept);
    gListen[i] = l;
    n++;
  }
  UNLOCK_TCPIP_CORE();
  return n;
}

// Listeners first, then a live peek (aborted), then the timer: with the core lock held no callback can run, so the
// buffers may go after this returns. Returns the active + TIME_WAIT pcbs left (the stop line's count).
static int closeListeners() {
  int pcbs = 0;
  LOCK_TCPIP_CORE();
  for (int i = 0; i < CAPTURE_PORT_N; i++) {
    if (!gListen[i]) continue;
    tcp_arg(gListen[i], nullptr);
    tcp_accept(gListen[i], nullptr);
    tcp_close(gListen[i]);
    gListen[i] = nullptr;
  }
  if (gSlot.pcb) {
    tcp_arg(gSlot.pcb, nullptr);
    tcp_recv(gSlot.pcb, nullptr);
    tcp_err(gSlot.pcb, nullptr);
    tcp_abort(gSlot.pcb);
    gSlot.pcb = nullptr;
  }
  sys_untimeout(peekTimeout, nullptr);
  gSlotSt = PK_FREE;
  for (struct tcp_pcb* p = tcp_active_pcbs; p; p = p->next) pcbs++;
  for (struct tcp_pcb* p = tcp_tw_pcbs; p; p = p->next) pcbs++;
  UNLOCK_TCPIP_CORE();
  return pcbs;
}

// ---------------------------------------------------------------- records from every task
void captureRecord(const CapUnit& u, uint32_t ip, bool active) {
  if (!gOnF.load()) return;
  portENTER_CRITICAL(&gDataMux);
  if (gRing) {
    gRing->put(u, capOctet(ip), sinceOn());
    gAux->clients.count(ip, active);
  }
  portEXIT_CRITICAL(&gDataMux);
}

void captureDns(const char* name, uint16_t qtype, uint32_t ip) {
  if (!gOnF.load()) return;
  portENTER_CRITICAL(&gDataMux);
  if (gAux) {
    gAux->dns.add(name, qtype, capOctet(ip));
    gAux->clients.count(ip, false);
  }
  portEXIT_CRITICAL(&gDataMux);
}

void captureNtp(bool answered, uint32_t ip) {
  if (!gOnF.load()) return;
  portENTER_CRITICAL(&gDataMux);
  gC.ntp_asked++;
  if (answered) gC.ntp_answered++;
  if (gRing && !gC.ntp_recorded) {  // the first request is a record; the rest are the counters
    gC.ntp_recorded = true;
    gRing->put1(CapKind::Ntp, answered ? "asked, answered" : "asked, not answered: fauxcloud has no time", capOctet(ip), sinceOn());
  }
  portEXIT_CRITICAL(&gDataMux);
}

// "joined" / "left" carry the device's maker prefix (or "random"), never the whole address
static void apRecord(CapKind k, const uint8_t mac[6], uint32_t ip) {
  if (!gOnF.load()) return;
  char ven[9], text[32];
  capVendor(mac, ven);
  if (k == CapKind::Lease) snprintf(text, sizeof text, "lease vendor:%s", ven);
  else snprintf(text, sizeof text, "%s vendor:%s", k == CapKind::Join ? "joined" : "left", ven);
  portENTER_CRITICAL(&gDataMux);
  if (gAux) {
    const uint32_t t = sinceOn();  // under the lock: the run's start stamp is set once the buffers exist
    if (k == CapKind::Join) gAux->clients.join(mac, t), gC.joined++;
    else if (k == CapKind::Lease) gAux->clients.lease(ip, mac, t);
    else gAux->clients.leave(mac, t);
    if (!ip) {  // a join or a leave: the address the device had, when a lease was seen
      for (int i = 0; i < gAux->clients.n; i++)
        if (!memcmp(gAux->clients.c[i].mac, mac, 6)) ip = gAux->clients.c[i].ip;
    }
    gRing->put1(k, text, capOctet(ip), t);
  }
  portEXIT_CRITICAL(&gDataMux);
}
void captureApJoin(const uint8_t mac[6]) { apRecord(CapKind::Join, mac, 0); }
void captureApLeave(const uint8_t mac[6]) { apRecord(CapKind::Leave, mac, 0); }
void captureApLease(uint32_t ip, const uint8_t mac[6]) { apRecord(CapKind::Lease, mac, ip); }

// ---------------------------------------------------------------- the buffers
static bool allocAll() {
  CaptureRing* r = (CaptureRing*)malloc(sizeof(CaptureRing));
  CapAux* a = (CapAux*)malloc(sizeof(CapAux));
  CapPeek* p = (CapPeek*)malloc(sizeof(CapPeek));
  CapSnap* s = (CapSnap*)malloc(sizeof(CapSnap));
  CapHttpScratch* h = (CapHttpScratch*)malloc(sizeof(CapHttpScratch));
  if (!r || !a || !p || !s || !h) {
    free(r), free(a), free(p), free(s), free(h);
    return false;
  }
  r->reset();
  memset(a, 0, sizeof *a);
  a->dns.reset();
  a->clients.reset();
  // the devices already on the fauxcloud Wi-Fi: listed as "before" (their lease when one was seen)
  wifi_sta_list_t list = {};
  if (esp_wifi_ap_get_sta_list(&list) == ESP_OK)
    for (int i = 0; i < list.num && i < CAPTURE_CLIENTS; i++) {
      a->clients.join(list.sta[i].mac, 0, true);
      uint32_t ip = 0;
      if (apLeaseOf(list.sta[i].mac, &ip)) a->clients.lease(ip, list.sta[i].mac, 0);
    }
  portENTER_CRITICAL(&gDataMux);
  memset(&gC, 0, sizeof gC);
  gRing = r, gAux = a, gPeek = p, gSnap = s, gHttp = h;
  gSnapUse = gSnapFreeLater = false;
  portEXIT_CRITICAL(&gDataMux);
  gSkipped = 0;
  return true;
}

static void freeAll() {
  CaptureRing* r;
  CapAux* a;
  CapPeek* p;
  CapSnap* s = nullptr;
  CapHttpScratch* h = nullptr;
  portENTER_CRITICAL(&gDataMux);
  if (gRing) {  // the last counters stay for the GET after the stop
    gC.seq = gRing->seq();
    gC.lost = gRing->lost();
    gC.written = gRing->written();
    gAux->clients.vendor(gC.vendor);
    memcpy(gC.port_hits, gAux->port_hits, sizeof gC.port_hits);
  }
  r = gRing, a = gAux, p = gPeek;
  gRing = nullptr, gAux = nullptr, gPeek = nullptr;
  if (gSnapUse) gSnapFreeLater = true;  // a GET renders, or a request uses the scratch: the httpd task frees both
  else s = gSnap, h = gHttp, gSnap = nullptr, gHttp = nullptr;
  portEXIT_CRITICAL(&gDataMux);
  free(r), free(a), free(p), free(s), free(h);
}

// ---------------------------------------------------------------- the loop task
static void execute(const CapActs& acts);

static void runAct(CapAct a, bool* stopRest) {
  switch (a) {
    case CapAct::FwdAbort:
      fwdAbort("capture");
      break;
    case CapAct::PublishOffline:
      gOfflineDone = !mqttCaptureOffline();  // no broker or not connected: nothing to wait for
      break;
    case CapAct::MqttOff:
      break;  // the flag (syncFlags) is the action: mqttLoop stops the client
    case CapAct::MqttOn:
      // a stopped client restarts through the flag (mqttLoop); one still up (the stop came between the retained
      // "offline" and the client's stop) restarts here, so it says "online" again
      if (!mqttClientGone()) requestMqttRestart();
      break;
    case CapAct::Alloc: {
      const HeapNow h = heapNow();
      gFree0 = h.free8, gLargest0 = h.largest8;
      gOnMs = millis();  // before the buffers are published: every record's time counts from here
      if (!allocAll()) {
        logf("CAPTURE", "not started: the buffers did not fit (free8 %lu, largest8 %lu)", (unsigned long)h.free8,
             (unsigned long)h.largest8);
        portENTER_CRITICAL(&gMachMux);
        const CapActs back = gM.allocFailed();
        syncFlags();
        portEXIT_CRITICAL(&gMachMux);
        *stopRest = true;
        execute(back);
      }
      break;
    }
    case CapAct::Open: {
      gMin8 = gMinLargest = UINT32_MAX;
      char failed[96];
      const int n = openListeners(failed, sizeof failed);
      if (n < CAPTURE_PORT_N) logf("CAPTURE", "%d of %d extra ports listening; not:%s", n, CAPTURE_PORT_N, failed);
      portENTER_CRITICAL(&gDataMux);
      gC.started_utc = timeSynced() ? nowS() : 0;
      portEXIT_CRITICAL(&gDataMux);
      break;
    }
    case CapAct::LogStart: {
      const HeapNow h = heapNow();
      logf("CAPTURE", "listening on the fauxcloud Wi-Fi: MQTT stopped, online jobs held, uploads not stored (free8 / largest8 "
           "%lu / %lu after the MQTT stop, %lu / %lu with the buffers)", (unsigned long)gFree0, (unsigned long)gLargest0,
           (unsigned long)h.free8, (unsigned long)h.largest8);
      break;
    }
    case CapAct::Close: {
      const int pcbs = closeListeners();
      portENTER_CRITICAL(&gDataMux);
      gC.elapsed_s = sinceOn() / 1000;
      portEXIT_CRITICAL(&gDataMux);
      logf("CAPTURE", "listeners closed (%d tcp pcbs active or in TIME_WAIT)", pcbs);
      break;
    }
    case CapAct::Free:
      freeAll();
      break;
    case CapAct::LogStop: {
      portENTER_CRITICAL(&gDataMux);
      const auto c = gC;
      portEXIT_CRITICAL(&gDataMux);
      char ports[96] = "";
      size_t pn = 0;
      for (int i = 0; i < CAPTURE_PORT_N; i++)
        if (c.port_hits[i] && pn < sizeof ports)
          pn += (size_t)snprintf(ports + pn, sizeof ports - pn, " :%u x%lu", CAPTURE_PORTS[i], (unsigned long)c.port_hits[i]);
      logf("CAPTURE", "stopped (%s) after %lu s: %lu records, %lu not kept, %lu joins, NTP %lu/%lu answered, %lu uploads not "
           "stored", capEndName(gM.ended()), (unsigned long)c.elapsed_s, (unsigned long)c.written, (unsigned long)c.lost,
           (unsigned long)c.joined, (unsigned long)c.ntp_answered, (unsigned long)c.ntp_asked, (unsigned long)gSkipped.load());
      logf("CAPTURE", "window: free8 min %lu, largest8 min %lu; peeks per port:%s", (unsigned long)(gMin8 == UINT32_MAX ? 0 : gMin8),
           (unsigned long)(gMinLargest == UINT32_MAX ? 0 : gMinLargest), pn ? ports : " none");
      const uint32_t sh = gStackHttpd.load(), sl = gStackLoop.load();
      logf("CAPTURE", "stack left, lowest since boot: httpd %ld B, loop %ld B (-1 = no request classified)",
           sh == UINT32_MAX ? -1L : (long)sh, sl == UINT32_MAX ? -1L : (long)sl);
      break;
    }
    case CapAct::LogRefused: {
      const HeapNow h = heapNow();
      logf("CAPTURE", "not started: not enough memory (free8 %lu needs %lu, largest8 %lu needs %lu)", (unsigned long)h.free8,
           (unsigned long)(CAPTURE_NEED8), (unsigned long)h.largest8, (unsigned long)CAPTURE_LARGEST_MIN);
      break;
    }
  }
}

static void execute(const CapActs& acts) {
  bool stopRest = false;
  for (int i = 0; i < acts.n && !stopRest; i++) runAct(acts.a[i], &stopRest);
}

// The peek slot and the refusals into records (loop task: the classifier's stack lives here)
static void drainPeek() {
  if (gSlotSt.load() == PK_READY) {
    const int pi = gSlot.port_i;
    if (!gPeek) {  // the buffers are gone (the loop task frees them itself: not reached while on)
      gSlotSt = PK_FREE;
      return;
    }
    CapUnit& u = gPeek->u;  // in the peek block: the loop task's stack keeps its room
    CapPeekIn in = {CAPTURE_PORTS[pi], gPeek->buf, gSlot.len, gSlot.cut, false};
    capPeekUnit(in, &u);
    const uint32_t ip = gSlot.ip;
    gSlotSt = PK_FREE;  // the next connection may be read from here on (it writes the buffer, never `u`)
    captureRecord(u, ip, true);
    const uint32_t left = (uint32_t)uxTaskGetStackHighWaterMark(nullptr);
    if (left < gStackLoop.load()) gStackLoop = left;
    portENTER_CRITICAL(&gDataMux);
    if (gAux) gAux->port_hits[pi]++;
    portEXIT_CRITICAL(&gDataMux);
  }
  for (int i = 0; i < CAPTURE_PORT_N; i++) {
    uint32_t n = gPeekBusy[i].exchange(0);
    char t[24];
    snprintf(t, sizeof t, ":%u busy, closed", CAPTURE_PORTS[i]);
    for (; n > 0; n--) {
      portENTER_CRITICAL(&gDataMux);
      if (gRing) gRing->put1(CapKind::Tcp, t, 0, sinceOn());
      portEXIT_CRITICAL(&gDataMux);
    }
  }
}

void captureTick() {
  if (!gQuiet.load()) return;
  const uint32_t ms = millis();
  CapTickIn in = {};
  in.now_ms = ms;
  in.net_idle = fwdIdle();
  in.offline_done = gOfflineDone || mqttOfflineAcked();
  in.mqtt_gone = mqttClientGone();
  in.restart = rebootPending() || factoryResetPending();
  portENTER_CRITICAL(&gMachMux);
  const CapSt st = gM.state();
  portEXIT_CRITICAL(&gMachMux);
  if (st == CapSt::Gate) {  // the heap the buffers will meet, read after the client stopped
    const HeapNow h = heapNow();
    in.heap_ok = h.free8 >= CAPTURE_NEED8 && h.largest8 >= CAPTURE_LARGEST_MIN;
  }
  portENTER_CRITICAL(&gMachMux);
  const CapActs acts = gM.tick(in);
  syncFlags();
  portEXIT_CRITICAL(&gMachMux);
  if (st == CapSt::Drain && acts.n && acts.a[0] == CapAct::FwdAbort) gOfflineDone = false;
  execute(acts);
  if (!gOnF.load()) return;
  drainPeek();
}

// The capture window's minima from heapSample's own reading (the allocator's local minimum since the previous second,
// read before the monitor restarts): a sampler of its own read it after a restart now and then and missed a dip (bench
// 2026-11-21: the window said 50 004 while the heap line said 8 252). Loop task, like captureTick.
void captureHeapFeed(uint32_t low8, uint32_t largest8) {
  if (!gOnF.load()) return;
  if (low8 < gMin8) gMin8 = low8;
  if (largest8 < gMinLargest) gMinLargest = largest8;
}

// ---------------------------------------------------------------- the admin routes (LAN face, session + CSRF)
static uint32_t gSince = 0;  // httpd task only
static CapSnap* gRender = nullptr;
static CaptureView gView;

static void renderCb(Out& o, httpd_req_t*) { captureRenderJson(o, gView); }

// The httpd block in use ends (httpd task): a stop that came meanwhile left its free to here
static void snapRelease() {
  CapSnap* late = nullptr;
  CapHttpScratch* lateH = nullptr;
  portENTER_CRITICAL(&gDataMux);
  gSnapUse = false;
  if (gSnapFreeLater) late = gSnap, lateH = gHttp, gSnap = nullptr, gHttp = nullptr, gSnapFreeLater = false;
  portEXIT_CRITICAL(&gDataMux);
  free(late), free(lateH);
}

CapHttpScratch* captureScratchTake() {
  CapHttpScratch* s = nullptr;
  portENTER_CRITICAL(&gDataMux);
  if (gHttp && gRing && !gSnapUse) {
    gSnapUse = true;
    s = gHttp;
  }
  portEXIT_CRITICAL(&gDataMux);
  return s;
}

void captureScratchGive(uint32_t stack_left) {
  if (stack_left < gStackHttpd.load()) gStackHttpd = stack_left;
  snapRelease();
}

// GET /api/admin/capture?since=<seq>: the state, the counters, the devices, the DNS names and up to 8 records newer
// than `since`. Every GET is a poll: it keeps an unwatched mode from stopping.
static esp_err_t onCaptureGet(httpd_req_t* req) {
  if (!adminGateReq(req)) return ESP_OK;
  char sb[12];
  gSince = queryParam(req, "since", sb, sizeof sb) ? (uint32_t)strtoul(sb, nullptr, 10) : 0;
  const uint32_t ms = millis();
  portENTER_CRITICAL(&gMachMux);
  gM.poll(ms);
  const CapSt st = gM.state();
  const CapEnd end = gM.ended();
  const uint32_t left = gM.idleLeft(ms);
  portEXIT_CRITICAL(&gMachMux);
  CaptureView& v = gView;
  memset(&v, 0, sizeof v);
  v.state = st == CapSt::Off ? "off" : st == CapSt::On ? "on" : "starting";
  v.ended = st == CapSt::Off ? capEndName(end) : "";
  v.blocked = otaActive() ? "ota" : rebootPending() || factoryResetPending() ? "restart" : "";
  v.idle_left_s = left / 1000;
  v.clock = timeSynced();
  v.uploads_skipped = gSkipped.load();
  portENTER_CRITICAL(&gDataMux);
  gRender = nullptr;
  if (gSnap && gRing) {
    gRing->served(gSince);
    v.nrecs = gRing->snapshot(gSince, gSnap->recs, CAPTURE_GET_MAX, &v.more);
    memcpy(&gSnap->dns, &gAux->dns, sizeof gSnap->dns);
    memcpy(&gSnap->clients, &gAux->clients, sizeof gSnap->clients);
    v.seq = gRing->seq();
    v.lost = gRing->lost();
    gAux->clients.vendor(v.vendor);
    gSnapUse = true;
    gRender = gSnap;
  } else {
    v.seq = gC.seq;
    v.lost = gC.lost;
    memcpy(v.vendor, gC.vendor, sizeof v.vendor);
  }
  v.ntp_asked = gC.ntp_asked;
  v.ntp_answered = gC.ntp_answered;
  v.started_utc = gC.started_utc;
  v.elapsed_s = gRender ? sinceOn() / 1000 : gC.elapsed_s;
  portEXIT_CRITICAL(&gDataMux);
  if (gRender) {
    v.recs = gRender->recs;
    v.dns = &gRender->dns;
    v.clients = &gRender->clients;
  }
  const esp_err_t r = sendRendered(req, "application/json", renderCb, false);
  if (gRender) snapRelease();
  gRender = nullptr;
  return r;
}

// POST /api/admin/capture/start [{"idle_s":N}]: 200 {"ok":true} (the start runs in the loop: the GET says "starting",
// then "on", or "off" with ended "heap"), or 409 {"error":"ota"|"restart"|"on"}.
static esp_err_t onCaptureStart(httpd_req_t* req) {
  if (!adminGateReq(req)) return ESP_OK;
  uint32_t idle = CAPTURE_IDLE_MS;
  size_t len = 0;
  if (req->content_len > 0 && readBody(req, &len)) {  // the bench's shorter idle limit (clamped to 60..3600 s)
    JsonReader r(bodyBuf(), len);
    char k[16];
    double v;
    if (r.beginObject())
      while (r.nextKey(k, sizeof k)) {
        if (strcmp(k, "idle_s") == 0 && r.readNumber(&v) && v > 0) idle = (uint32_t)(v * 1000);
        else r.skipValue();
      }
  }
  portENTER_CRITICAL(&gMachMux);
  const CapPress p = gM.start(millis(), otaActive(), rebootPending() || factoryResetPending(), idle);
  syncFlags();
  portEXIT_CRITICAL(&gMachMux);
  if (p == CapPress::Ok) {  // the last run's counters go now, not at the allocation seconds later
    portENTER_CRITICAL(&gDataMux);
    memset(&gC, 0, sizeof gC);
    portEXIT_CRITICAL(&gDataMux);
    gSkipped = 0;
  }
  if (p != CapPress::Ok) {
    const char* code = p == CapPress::Ota ? "ota" : p == CapPress::Restart ? "restart" : "on";
    logf("CAPTURE", "start refused: %s", code);
    httpd_resp_set_status(req, "409 Conflict");
    char body[32];
    snprintf(body, sizeof body, "{\"error\":\"%s\"}", code);
    return sendJson(req, body);
  }
  char ip[16];
  ipToStr(peerIp(req), ip);
  logf("CAPTURE", "start requested from %s", ip);
  return sendJson(req, "{\"ok\":true}");
}

// POST /api/admin/capture/stop: 200 {"ok":true} (also when nothing runs)
static esp_err_t onCaptureStop(httpd_req_t* req) {
  if (!adminGateReq(req)) return ESP_OK;
  portENTER_CRITICAL(&gMachMux);
  gM.stop();
  portEXIT_CRITICAL(&gMachMux);
  return sendJson(req, "{\"ok\":true}");
}

void registerCapture(httpd_handle_t srv) {
  const httpd_uri_t uris[] = {
      {"/api/admin/capture", HTTP_GET, onCaptureGet, nullptr},
      {"/api/admin/capture/start", HTTP_POST, onCaptureStart, nullptr},
      {"/api/admin/capture/stop", HTTP_POST, onCaptureStop, nullptr},
  };
  registerUris(srv, uris, sizeof uris / sizeof uris[0]);
}

}  // namespace app
