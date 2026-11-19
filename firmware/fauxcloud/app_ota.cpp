// app_ota.cpp -- firmware upload from the admin face: POST /api/admin/ota with the raw .bin. The admin
// handler (app_admin.cpp) checks the session and hands the socket to its own task (httpd async request), so the
// only httpd worker keeps answering the console and the JSON while the image streams into the inactive slot.
#include <Update.h>
#include <esp_heap_caps.h>
#include <esp_ota_ops.h>

#include "app.h"
#include "src/robust.h"

namespace app {

static std::atomic<bool> gOtaActive{false};
static const uint32_t OTA_SECTOR_PAUSE_MS = 40;  // after every 4 KB: airtime and flash cache for the console
bool otaActive() { return gOtaActive.load(); }

size_t otaSlotSize() {
  const esp_partition_t* p = esp_ota_get_next_update_partition(nullptr);
  return p ? p->size : 0;
}

static void reply(httpd_req_t* r, const char* json) {
  httpd_resp_set_type(r, "application/json");
  httpd_resp_set_hdr(r, "Cache-Control", "no-store");
  httpd_resp_sendstr(r, json);
}

// The start guard's constants live in src/heapinfo.h (OTA_MIN_FREE8 28 KB byte-addressable, OTA_MIN_BLOCK 12 KB)
// The heap the last write had, kept across the restart that follows it (RTC; lost on a power cut)
// The boot after a write turns the record "seen" (shown in diag for this boot); the boot after that clears it. No RAM copy.
// The record lives in app_sys.cpp's one RTC struct (rtcOtaHeap()), at a fixed offset.
// Bench only: the release neither writes nor reads the slot (it stays at gRtc@20, unwritten: the RTC layout is
// append-only; a bench image after a release finds no valid magic and shows no ota_heap)
#if FC_DIAG_BENCH
static constexpr uint32_t OTA_HEAP_SEEN = 0x4f485053;  // "OHPS"
void otaHeapBoot() { rtcOtaHeap().magic = rtcOtaHeap().magic == OTA_HEAP_MAGIC ? OTA_HEAP_SEEN : 0; }
bool otaHeapLast(OtaHeapRec* out) {
  *out = rtcOtaHeap();
  return out->magic == OTA_HEAP_SEEN;
}
// heapSample's allocator low-water of the last second (and the largest block) while a write runs
void otaHeapFeed(uint32_t low8, uint32_t largest8) {
  if (gOtaActive.load() && rtcOtaHeap().magic == OTA_HEAP_MAGIC) otaHeapStep(&rtcOtaHeap(), low8, largest8);
}
#endif
static std::atomic<int> gOtaFd{-1};
static std::atomic<uint32_t> gSkipped{0};
int otaSockFd() { return gOtaFd.load(); }
void otaCountSkipped() { gSkipped++; }

struct OtaJob {
  httpd_req_t* req;  // the async copy: ours until httpd_req_async_handler_complete
  size_t len;
};

static void otaTask(void* arg) {
  OtaJob job = *(OtaJob*)arg;
  delete (OtaJob*)arg;
  httpd_req_t* r = job.req;
  char msg[160] = "";
  size_t got = 0;
  const uint32_t t0 = millis();
  // one TCP segment at a time: a small buffer (heap during a cold load); Update keeps its own 4 KB sector
  static const size_t CHUNK = 1436;
  uint8_t* buf = (uint8_t*)malloc(CHUNK);
  MarkerScan marker;
  const HeapNow h0 = heapNow();
  bool ok = buf && Update.begin(job.len, U_FLASH);
  const HeapNow h1 = heapNow();
#if FC_DIAG_BENCH
  otaHeapStep(&rtcOtaHeap(), h1.free8, h1.largest8);
#endif
  logf("OTA", "heap free8 %lu before Update.begin, %lu after", (unsigned long)h0.free8, (unsigned long)h1.free8);
  if (!buf) snprintf(msg, sizeof msg, "{\"error\":\"low memory\"}");
  else if (!ok) snprintf(msg, sizeof msg, "{\"error\":\"update did not start: %s\"}", Update.errorString());
  int timeouts = 0;
  while (ok && got < job.len) {
    const size_t want = job.len - got < CHUNK ? job.len - got : CHUNK;
    const int n = httpd_req_recv(r, (char*)buf, want);
    if (n == HTTPD_SOCK_ERR_TIMEOUT) {
      if (++timeouts < 2) continue;  // ~6 s without data (recv_wait_timeout 3 s, twice)
      snprintf(msg, sizeof msg, "{\"error\":\"upload stalled\"}");
      ok = false;
      break;
    }
    if (n <= 0) {
      snprintf(msg, sizeof msg, "{\"error\":\"connection lost\"}");
      ok = false;
      break;
    }
    timeouts = 0;
    httpd_sess_update_lru_counter(r->handle, gOtaFd.load());  // the OTA socket is never the LRU purge's pick
    if (got == 0 && buf[0] != 0xE9) {
      snprintf(msg, sizeof msg, "{\"error\":\"not an ESP32 firmware image\"}");
      ok = false;
      break;
    }
    marker.feed(buf, (size_t)n);
    if (Update.write(buf, (size_t)n) != (size_t)n) {
      snprintf(msg, sizeof msg, "{\"error\":\"flash write failed: %s\"}", Update.errorString());
      ok = false;
      break;
    }
    got += (size_t)n;
    if (got / 65536 != (got - (size_t)n) / 65536) {  // the RTC heap record every 64 KB, a log line every 256 KB
      const HeapNow hn = heapNow();
#if FC_DIAG_BENCH
      otaHeapStep(&rtcOtaHeap(), hn.free8, hn.largest8);
#endif
      if (got / 262144 != (got - (size_t)n) / 262144)
        logf("OTA", "%u KB written, heap free8 %lu, largest8 %lu", (unsigned)(got / 1024), (unsigned long)hn.free8,
             (unsigned long)hn.largest8);
    }
    // Each 4 KB sector erase + write stops both CPUs' flash cache for tens of ms. A pause after every sector lets
    // the httpd worker and lwIP answer the console in between (upload replies stay < 200 ms).
    if (got / 4096 != (got - (size_t)n) / 4096) vTaskDelay(pdMS_TO_TICKS(OTA_SECTOR_PAUSE_MS));
  }
#if FC_DIAG_BENCH
  {
    const HeapNow hn = heapNow();
    otaHeapStep(&rtcOtaHeap(), hn.free8, hn.largest8);
  }
#endif
  free(buf);
  if (ok && !marker.found()) {
    snprintf(msg, sizeof msg, "{\"error\":\"not a fauxcloud image\"}");  // it sits in the inactive slot: nothing lost
    ok = false;
  }
  if (ok && !Update.end(true)) {
    snprintf(msg, sizeof msg, "{\"error\":\"image rejected: %s\"}", Update.errorString());
    ok = false;
  }
  if (!ok) Update.abort();
  const uint32_t ms = millis() - t0;
  if (ok) {
    char ver[48];
    snprintf(ver, sizeof ver, "%s", marker.version());
    snprintf(msg, sizeof msg, "{\"ok\":true,\"bytes\":%u,\"version\":\"%.40s\"}", (unsigned)got, ver);
    logf("OTA", "%u B written in %lu ms, image %s: restarting into it (pending verify); %lu console upload(s) "
         "answered, not processed", (unsigned)got, (unsigned long)ms, ver, (unsigned long)gSkipped.load());
  } else {
    logf("OTA", "aborted after %u of %u B (%lu ms): %s; the running image stays; %lu console upload(s) answered, "
         "not processed", (unsigned)got, (unsigned)job.len, (unsigned long)ms, msg, (unsigned long)gSkipped.load());
  }
  reply(r, msg);
  httpd_req_async_handler_complete(r);
  gOtaFd = -1;
  gOtaActive = false;  // a retry is possible at once
  if (ok) sysPlanRestart("ota", 1000);
  vTaskDelete(nullptr);
}

// Called by the admin handler after its gate. Returns the reply for an immediate refusal, or nullptr when the task
// owns the request now.
const char* otaStart(httpd_req_t* req) {
  if (gOtaActive.exchange(true)) return "busy";
  fwdAbort("ota");  // a forwarding send stops at its next step, before the heap guard below
  for (int i = 0; i < 60 && !fwdIdle(); i++) vTaskDelay(pdMS_TO_TICKS(50));  // <= 3 s for its TLS heap
  if (!fwdIdle()) {
    gOtaActive = false;
    return "{\"error\":\"busy sending to an online service, try again in a few seconds\"}";
  }
  const size_t slot = otaSlotSize();
  // Start guard: what the write needs now, the current free heap and largest block (heap_low, a minutes-long
  // minimum, stays a warning: one page load or a discovery burst must not block an update for minutes). Measured: the
  // write needs the 8 KB task stack in one piece (Update's 4 KB sector and the 1.4 KB buffer are separate blocks)
  // and ~25 KB in all.
  // Both on the byte-addressable heap (an INTERNAL figure counts ~40 KB of IRAM and protects nothing)
  const HeapNow hn = heapNow();
  if (!otaHeapOk(hn.free8, hn.largest8)) {
    gOtaActive = false;
    logf("OTA", "refused: heap free8 %lu (needs %lu), largest8 %lu (needs %lu)", (unsigned long)hn.free8,
         (unsigned long)OTA_MIN_FREE8, (unsigned long)hn.largest8, (unsigned long)OTA_MIN_BLOCK);
    return "{\"error\":\"low memory — restart fauxcloud first\"}";
  }
  if (req->content_len == 0 || req->content_len > slot) {
    gOtaActive = false;
    return "{\"error\":\"the file must be a firmware image no larger than the app slot\"}";
  }
  // The record starts with the figures the guard judged (the size is valid), before the write
  // allocates anything; the task, heapSample and the progress steps only lower its minima: need = start8 - min8
#if FC_DIAG_BENCH
  rtcOtaHeap() = OtaHeapRec{OTA_HEAP_MAGIC, hn.free8, hn.free8, hn.largest8};
#endif
  // Idle page connections close now (the console's sockets are never kept open), so the socket table
  // has room and the LRU purge never reaches the OTA's socket
  const int fd = httpd_req_to_sockfd(req);
  const int closed = httpCloseIdlePages(fd);
  gOtaFd = fd;
  gSkipped = 0;
  if (closed) logf("OTA", "%d idle page connection(s) closed for the update", closed);
  httpd_req_t* copy = nullptr;
  if (httpd_req_async_handler_begin(req, &copy) != ESP_OK) {
    gOtaFd = -1;
    gOtaActive = false;
    return "{\"error\":\"busy, try again\"}";
  }
  OtaJob* job = new OtaJob{copy, req->content_len};
  logf("OTA", "upload of %u B started (slot %u B)", (unsigned)req->content_len, (unsigned)slot);
  // priority 1: below httpd (5) and lwIP, so the console is always served first
  if (xTaskCreate(otaTask, "ota", 8192, job, 1, nullptr) != pdPASS) {
    delete job;
    reply(copy, "{\"error\":\"low memory\"}");
    httpd_req_async_handler_complete(copy);
    gOtaActive = false;
  }
  return nullptr;
}

}  // namespace app
