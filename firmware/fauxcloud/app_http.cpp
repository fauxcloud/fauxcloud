// app_http.cpp -- esp_http_server, shared helpers, public (run mode) and setup-mode handlers (see app.h).
//
// One server per boot, its handler set chosen by the mode (setup pages and admin never exist at the same time).
//   Run mode:   upload, status page, read-only /api/*, hashed assets /a/<hash8>/<name>, /admin page (STA only) +
//               /api/admin/* (app_admin.cpp); OS connectivity probes on the AP get their expected success answer.
//   Setup mode: the setup page for any host on the AP (captive), /api/setup/{scan,apply,state}; nothing else.
//
// Cache contract: assets immutable for a year; shells no-cache + ETag (build hash);
// face JSON no-store + ETag from counters, If-None-Match checked before anything is copied or rendered; admin JSON
// no-store without ETag. Every JSON handler copies what it needs under gLock and renders after releasing it.
#include <WiFi.h>
#include <esp_random.h>
#include <lwip/sockets.h>

#include <new>
#include <type_traits>

#include "app.h"
#include "src/auth.h"
#include "src/console.h"
#include "src/fwd.h"  // FWD_TLS (the METAR routes)
#include "src/metrics.h"
#include "src/robust.h"
#include "src/web_assets.h"

namespace app {

static const char* WU_PATH = "/weatherstation/updateweatherstation.php";
static const char* SETUP_URL = "http://fauxcloud.setup/";  // any name resolves on the AP in setup mode (DNS)
static volatile uint32_t gSetupActivityMs = 0;
static uint32_t gBootId = 0;  // random per boot: an ETag from the previous boot never matches

// ---------------------------------------------------------------- request accounting (httpd task only)
static uint32_t gReqT0 = 0;
static uint32_t gReqBytes = 0;
static int gReqStatus = 200;
static bool gReqQuiet = false;  // the handler logged its own line (upload, catch-all)

static void reqBegin() {
  gReqT0 = millis();
  gReqBytes = 0;
  gReqStatus = 200;
  gReqQuiet = false;
}

// aborted = the handler returned an error, so httpd closes the socket (a chunk that could not be sent: the client
// sees a closed connection at once, never a truncated body it waits on)
static void reqEnd(httpd_req_t* req, bool aborted) {
  const uint32_t ms = millis() - gReqT0;
  httpCount(ms, gReqStatus == 304, aborted);
  markHttpServed();  // rollback health check
  if (gReqQuiet) return;
  // Path plus the station parameter only: other query strings may carry secrets.
  const char* q = strchr(req->uri, '?');
  const int pathLen = q ? (int)(q - req->uri) : (int)strlen(req->uri);
  char st[STATION_ID_LEN] = "";
  if (q) queryParam(req, "station", st, sizeof st);
  // A page request is chatter; an error answer or a broken-off one is an event -- but not a 503 of the load
  // shedding: its own "HTTP low memory" line is the event, once per spell
  logl((gReqStatus >= 400 && gReqStatus != 503) || aborted ? Log::Event : Log::Chatter, "REQ", "%s %.*s%s%s -> %d %luB %lums%s", http_method_str((enum http_method)req->method),
       pathLen > 60 ? 60 : pathLen, req->uri, st[0] ? "?station=" : "", st, gReqStatus, (unsigned long)gReqBytes,
       (unsigned long)ms, aborted ? " ABORTED" : "");
}

typedef esp_err_t (*Handler)(httpd_req_t*);

// Page connections (anything but a console upload) by socket, so an OTA start can close the idle ones.
// httpd task only: dispatch, the close callback and otaStart all run on it.
static httpd_handle_t gSrv = nullptr;
int httpSessions() {  // open httpd sessions (of max_open_sockets 10); -1 = no server
  if (!gSrv) return -1;
  size_t n = 16;
  int fds[16];
  return httpd_get_client_list(gSrv, &n, fds) == ESP_OK ? (int)n : -1;
}
static PageSessions gPages;  // page sessions with their last request, capped at 6 (robust.h)
static std::atomic<uint32_t> gHttpPurged{0}, gHttpShed{0}, gHttpPaced{0};
uint32_t httpShed() { return gHttpShed.load(); }
uint32_t httpPaced() { return gHttpPaced.load(); }  // page answers that waited for the heap (pagePace)
static std::atomic<int> gInFlight{0};  // forwarding's idle check
static std::atomic<uint32_t> gLastPageMs{0};
int httpInFlight() { return gInFlight.load(); }
uint32_t httpLastPageMs() { return gLastPageMs.load(); }
// The setup page and its probes log per request in setup mode, rate limited; httpd task only.
// One limiter each, so a phone's probe storm never hides that the page itself was opened
static RateLimitedLine gSetupPageLine, gSetupProbeLine;

static esp_err_t onSockOpen(httpd_handle_t, int) {  // a client connecting, for the heap minimum's context
  heapCtxMark(HC_ACCEPT);
  return ESP_OK;
}
static void onSockClose(httpd_handle_t, int fd) {
  gPages.drop(fd);
  close(fd);
}
uint32_t httpPurged() { return gHttpPurged.load(); }

int httpCloseIdlePages(int keep_fd) {
  int n = 0;
  for (int i = 0; i < gPages.count(); i++)
    if (gPages.fdAt(i) != keep_fd && httpd_sess_trigger_close(gSrv, gPages.fdAt(i)) == ESP_OK) n++;
  return n;
}

static esp_err_t onUpload(httpd_req_t* req);
static esp_err_t dispatchInner(httpd_req_t* req);
static bool sideAndHostOk(httpd_req_t* req);
static esp_err_t dispatch(httpd_req_t* req) {
  gInFlight++;
  const esp_err_t r = dispatchInner(req);
  if ((Handler)req->user_ctx != onUpload) gLastPageMs = millis() | 1;
  gInFlight--;
  return r;
}
static esp_err_t dispatchInner(httpd_req_t* req) {
  reqBegin();
  const Handler h = (Handler)req->user_ctx;
  const int fd = httpd_req_to_sockfd(req);
  // a request whose local and peer addresses name different sides gets no page and no admin call; nor does one from
  // the home network that names another host (DNS rebinding). Decided first, so a refused request costs what an unknown
  // path costs: no pacing wait, no page-session slot
  const bool refused = h != onUpload && !sideAndHostOk(req);
  if (h != onUpload && !refused) {
    // Pacing (robust.h pagePace): while the byte heap is low a page answer waits for earlier answers to be
    // acknowledged, then sheds; no block for a page -> 503 at once, the session closes. Uploads never wait.
    uint32_t waited = 0;
    Pace pace;
    for (;;) {
      const HeapNow hn = heapNow();
      pace = pagePace(hn.free8, hn.largest8, waited);
      if (pace != Pace::Wait) break;
      vTaskDelay(pdMS_TO_TICKS(PACE_STEP_MS));
      waited += PACE_STEP_MS;
    }
    if (waited) gHttpPaced++;
    if (pace == Pace::Shed) {
      static const char R503[] = "HTTP/1.1 503 Service Unavailable\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
      httpd_send(req, R503, sizeof R503 - 1);
      stopReading(req);
      gHttpShed++;
      gReqStatus = 503;
      gReqQuiet = true;  // one log line per 10 s at most, below
      static uint32_t lastLog = 0, lastShed = 0, lastEvent = 0;
      const uint32_t now = millis() | 1;
      // The first line of a shedding spell (no shed for 60 s before) and one per 10 min are
      // events, the 10-s repeats chatter
      const bool spell = !lastShed || now - lastShed > 60000;
      lastShed = now;
      if (!lastLog || now - lastLog > 10000) {
        lastLog = now;
        const bool ev = spell || !lastEvent || now - lastEvent >= 600000;
        if (ev) lastEvent = now;
        logl(ev ? Log::Event : Log::Chatter, "HTTP", "low memory: page requests answered 503 (%lu shed since boot)",
             (unsigned long)gHttpShed.load());
      }
      reqEnd(req, false);
      return ESP_OK;
    }
    const int purge = gPages.touch(fd, millis());
    if (purge >= 0 && httpd_sess_trigger_close(gSrv, purge) == ESP_OK) gHttpPurged++;
  }
  const esp_err_t r = refused ? onOther(req, HTTPD_404_NOT_FOUND) : h(req);
  reqEnd(req, r != ESP_OK);
  // During an OTA no connection but the OTA's own stays open after its answer: sockets stay free for the console
  // and the LRU purge has nothing to pick but idle pages.
  if (otaActive() && fd != otaSockFd()) httpd_sess_trigger_close(gSrv, fd);
  return r;
}

void registerUris(httpd_handle_t srv, const httpd_uri_t* uris, size_t n) {
  for (size_t i = 0; i < n; i++) {
    httpd_uri_t u = uris[i];
    u.user_ctx = (void*)u.handler;
    u.handler = dispatch;
    if (httpd_register_uri_handler(srv, &u) != ESP_OK) logf("HTTP", "register failed: %s", u.uri);
  }
}

// ---------------------------------------------------------------- helpers
static uint32_t sockAddr(httpd_req_t* req, bool local) {
  struct sockaddr_in6 addr;
  socklen_t len = sizeof addr;
  const int fd = httpd_req_to_sockfd(req);
  const int rc = local ? getsockname(fd, (struct sockaddr*)&addr, &len) : getpeername(fd, (struct sockaddr*)&addr, &len);
  if (rc != 0) return 0;
  if (addr.sin6_family == AF_INET) return ((struct sockaddr_in*)&addr)->sin_addr.s_addr;
  if (addr.sin6_family == AF_INET6) return addr.sin6_addr.un.u32_addr[3];  // IPv4-mapped
  return 0;
}

uint32_t peerIp(httpd_req_t* req) { return sockAddr(req, false); }

SrcIf srcIf(httpd_req_t* req) { return classifySrc(sockAddr(req, true), sockAddr(req, false), apIpNet()); }

// dispatch's gate for every route but the upload: not Mixed, and in run mode a home-network request must name the
// bridge (hostAllowed: an IP literal, the hostname, <hostname>.local or a router suffix). A web page that rebinds its own name to the
// bridge's address sends that name and gets the catch-all, as an unknown path does. Setup mode and the AP side
// are exempt: the setup face answers any name on purpose, and the AP's DNS gives every name the bridge.
static RateLimitedLine gHostLine;  // httpd task only
static bool gOtherSilent = false;   // the gate logged the refusal itself: onOther answers without its own line
static bool sideAndHostOk(httpd_req_t* req) {
  const SrcIf via = srcIf(req);
  if (via == SrcIf::Mixed) return false;
  if (gMode == Mode::Setup || via != SrcIf::Sta) return true;
  char host[72], name[sizeof gSettings.hostname];
  const esp_err_t e = httpd_req_get_hdr_value_str(req, "Host", host, sizeof host);
  if (e == ESP_ERR_NOT_FOUND) return true;
  xSemaphoreTake(gLock, portMAX_DELAY);
  memcpy(name, gSettings.hostname, sizeof name);
  xSemaphoreGive(gLock);
  name[sizeof name - 1] = 0;
  if (e == ESP_OK && hostAllowed(host, name)) return true;
  if (e != ESP_OK) snprintf(host, sizeof host, "<over %u B>", (unsigned)(sizeof host - 1));
  logSafe(host);
  char ip[16], sx[32];
  ipToStr(peerIp(req), ip);
  uint32_t n, min;
  if (gHostLine.hit(millis(), &n, &min))
    logf("HTTP", "host %s from %s is not this bridge's name: catch-all (DNS rebinding guard)%s", host, ip,
         rateSuffix(sx, sizeof sx, n, min));
  gOtherSilent = true;
  return false;
}

// The status face served on the AP hides LAN details: the page also reads this header.
static bool apFace(httpd_req_t* req) {
  const bool ap = srcIf(req) == SrcIf::Ap;
  httpd_resp_set_hdr(req, "X-Fauxcloud-Face", ap ? "ap" : "lan");
  return ap;
}

static bool sendChunk(void* ctx, const char* data, size_t len) {
  gReqBytes += len;
  return httpd_resp_send_chunk((httpd_req_t*)ctx, data, len) == ESP_OK;
}

static char gChunk[HTTP_SCRATCH_LEN];  // one httpd task -> one shared render buffer (also the coredump stream's)
char* httpScratch() { return gChunk; }

// The last chunk + the terminator. A chunk that failed (send_wait_timeout on a stalled link) -> ESP_FAIL, so httpd
// closes the socket; ESP_OK there would leave a keep-alive socket with a cut chunked body the client waits on.
static esp_err_t endChunks(httpd_req_t* req, Out& o) {
  return o.flush() && httpd_resp_send_chunk(req, nullptr, 0) == ESP_OK ? ESP_OK : ESP_FAIL;
}

// Streams what `render` writes as chunks. lock = render under gLock (only for state too big to copy).
esp_err_t sendRendered(httpd_req_t* req, const char* type, RenderCb render, bool lock) {
  httpd_resp_set_type(req, type);
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  Out o(sendChunk, req, gChunk, sizeof gChunk);
  if (lock) xSemaphoreTake(gLock, portMAX_DELAY);
  render(o, req);
  if (lock) xSemaphoreGive(gLock);
  return endChunks(req, o);
}

esp_err_t sendJson(httpd_req_t* req, const char* json) {
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  gReqBytes += strlen(json);
  return httpd_resp_sendstr(req, json);
}

// 304 with no body when If-None-Match matches; otherwise sets ETag + Cache-Control for the full response.
static char gEtag[48];  // httpd keeps the header pointer until the response is sent
static bool notModified(httpd_req_t* req, const char* cache) {
  httpd_resp_set_hdr(req, "Cache-Control", cache);
  httpd_resp_set_hdr(req, "ETag", gEtag);
  char inm[96];
  if (httpd_req_get_hdr_value_str(req, "If-None-Match", inm, sizeof inm) != ESP_OK || !etagMatches(inm, gEtag))
    return false;
  gReqStatus = 304;
  httpd_resp_set_status(req, "304 Not Modified");
  httpd_resp_send(req, nullptr, 0);
  return true;
}

static esp_err_t sendGzFlash(httpd_req_t* req, const uint8_t* data, size_t len) {
  httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
  for (size_t off = 0; off < len; off += 4096) {
    const size_t n = len - off < 4096 ? len - off : 4096;
    if (httpd_resp_send_chunk(req, (const char*)data + off, n) != ESP_OK) return ESP_FAIL;
    gReqBytes += n;
  }
  return httpd_resp_send_chunk(req, nullptr, 0);
}

// Shells: no-cache + ETag "<build hash>" -> 304 while the firmware (page) is unchanged.
esp_err_t serveGz(httpd_req_t* req, const uint8_t* data, size_t len, const char* version) {
  snprintf(gEtag, sizeof gEtag, "\"%s\"", version);
  httpd_resp_set_type(req, "text/html; charset=utf-8");
  if (notModified(req, "no-cache")) return ESP_OK;
  return sendGzFlash(req, data, len);
}

bool queryParam(httpd_req_t* req, const char* key, char* out, size_t cap) {
  char q[128];
  if (httpd_req_get_url_query_str(req, q, sizeof q) != ESP_OK) return false;
  return httpd_query_key_value(q, key, out, cap) == ESP_OK;
}

static char gBody[PUT_MAX_BODY + 1];  // one httpd task -> one shared body buffer
char* bodyBuf() { return gBody; }

// The reply still goes out; the unread rest of the body is never drained on the only worker.
// Set the "Connection: close" header before the reply; call this after it (or before: the send side stays open).
void stopReading(httpd_req_t* req) {
  const int fd = httpd_req_to_sockfd(req);
  shutdown(fd, SHUT_RD);
  httpd_sess_trigger_close(req->handle, fd);
}

// A console-facing 200 in ONE send -- status line, headers and body in one TCP
// segment (the 7-in-1 reads only the first one) -- then the session closes. Used by the upload and the catch-all.
static void replyOneShot(httpd_req_t* req, const char* body) {
  char buf[128];
  const size_t blen = strlen(body);
  const int n = snprintf(buf, sizeof buf,
                         "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: %u\r\nConnection: close\r\n\r\n%s",
                         (unsigned)blen, body);
  if (n > 0 && n < (int)sizeof buf) httpd_send(req, buf, n);
  gReqBytes += blen;
  stopReading(req);
}

static_assert(HTTPD_SOCK_ERR_TIMEOUT == BODY_RECV_TIMEOUT, "robust.h mirrors the httpd timeout code");
static int recvReq(void* ctx, char* buf, size_t len) { return httpd_req_recv((httpd_req_t*)ctx, buf, len); }
static uint32_t nowMs(void*) { return millis(); }

bool readBody(httpd_req_t* req, size_t* len) {
  size_t got = 0;
  const BodyRead r = readAll(recvReq, nowMs, req, gBody, PUT_MAX_BODY, req->content_len, &got, BODY_DRAIN_MAX);
  if (r == BodyRead::Ok) {
    *len = got;
    return true;
  }
  char ip[16], uri[80];
  ipToStr(peerIp(req), ip);
  logUri(req->uri, uri, sizeof uri);
  logf("HTTP", "%s: body %s (%u of %u B) from %s, connection closed", uri, bodyReadName(r), (unsigned)got,
       (unsigned)req->content_len, ip);
  httpd_resp_set_hdr(req, "Connection", "close");
  // A body that is merely too big (<= BODY_DRAIN_MAX announced) has been read to its end and thrown
  // away by readAll under the same 2-timeout / 6 s rules, so nothing is left to read: the caller's JSON error goes
  // out, then the connection closes. A stall during that drain, a larger body or a lost one is cut at once.
  if (r == BodyRead::Drained) httpd_sess_trigger_close(req->handle, httpd_req_to_sockfd(req));
  else stopReading(req);
  return false;
}

// Station from ?station=, else the first one. Caller holds gLock.
static Station* pickStation(httpd_req_t* req) {
  char id[STATION_ID_LEN];
  if (queryParam(req, "station", id, sizeof id)) return gStations.find(id);
  return gStations.first();
}

// An unknown station is a 404 (same body); the admin API keeps its own answers
static esp_err_t noStation(httpd_req_t* req) {
  gReqStatus = 404;
  httpd_resp_set_status(req, "404 Not Found");
  return sendJson(req, "{\"error\":\"no such station\"}");
}

// ---------------------------------------------------------------- catch-all, connectivity probes
// Run mode on the AP: a phone joined for debugging gets the success answer its OS expects, so it is not nagged
// by a captive-portal sheet (setup mode redirects the same probes to the setup page instead).
struct Probe {
  const char* path;
  int status;  // 200 or 204
  const char* type;
  const char* body;
};
static const Probe PROBES[] = {
    {"/hotspot-detect.html", 200, "text/html", "<HTML><HEAD><TITLE>Success</TITLE></HEAD><BODY>Success</BODY></HTML>"},
    {"/library/test/success.html", 200, "text/html",
     "<HTML><HEAD><TITLE>Success</TITLE></HEAD><BODY>Success</BODY></HTML>"},
    {"/generate_204", 204, nullptr, nullptr},
    {"/gen_204", 204, nullptr, nullptr},
    {"/connecttest.txt", 200, "text/plain", "Microsoft Connect Test"},
    {"/ncsi.txt", 200, "text/plain", "Microsoft NCSI"},
    {"/success.txt", 200, "text/plain", "success\n"},
    {"/canonical.html", 200, "text/html",
     "<meta http-equiv=\"refresh\" content=\"0;url=https://support.mozilla.org/kb/captive-portal\"/>"},
};

static const Probe* findProbe(httpd_req_t* req) {
  const char* q = strchr(req->uri, '?');
  const size_t n = q ? (size_t)(q - req->uri) : strlen(req->uri);
  for (const Probe& p : PROBES)
    if (strlen(p.path) == n && strncmp(p.path, req->uri, n) == 0) return &p;
  return nullptr;
}

// Anything else (other path or method, oversized request, a denied admin call): always 200, like the console
// expects. Weathercloud (Host header names it) gets "200", everything else "success".
esp_err_t onOther(httpd_req_t* req, httpd_err_code_t err) {
  gReqQuiet = true;
  if (gOtherSilent) {  // a Host the gate refused: its rate-limited line is the log, the answer is the plain catch-all
    gOtherSilent = false;
    replyOneShot(req, "success");
    return ESP_OK;
  }
  char ip[16];
  ipToStr(peerIp(req), ip);
  const SrcIf via = srcIf(req);
  const bool parsed = err != HTTPD_414_URI_TOO_LONG && err != HTTPD_431_REQ_HDR_FIELDS_TOO_LARGE;
  if (parsed && via == SrcIf::Ap && req->method == HTTP_GET) {
    if (const Probe* p = findProbe(req)) {
      logl(Log::Chatter, "HTTP", "probe %s from %s -> %d", p->path, ip, p->status);  // a phone repeats these
      if (p->status == 204) {
        httpd_resp_set_status(req, "204 No Content");
        gReqStatus = 204;
        return httpd_resp_send(req, nullptr, 0);
      }
      httpd_resp_set_type(req, p->type);
      httpd_resp_set_hdr(req, "Cache-Control", "no-store");
      gReqBytes += strlen(p->body);
      return httpd_resp_sendstr(req, p->body);
    }
  }
  const uint32_t nOther = ++gOtherHits;  // the first three since the restart are events, the rest chatter
  const char* body = "success";
  // A body nobody reads (a denied admin call, an unknown POST) is not drained on the only worker; every
  // answer below closes the session (replyOneShot)
  // Path only: the query may carry the console's PASSWORD. On 414/431 the request was never fully parsed,
  // so method, URI and headers are not trustworthy -- log the error code alone.
  if (!parsed) {
    // a console upload over the URI limit is answered and lost: counted, logged once per 10 min
    static uint32_t lastLogMs = 0;
    const uint32_t ms = millis();
    if (err == HTTPD_414_URI_TOO_LONG) gTooLong++;
    if (err != HTTPD_414_URI_TOO_LONG || !lastLogMs || ms - lastLogMs > 600000) {
      if (err == HTTPD_414_URI_TOO_LONG) lastLogMs = ms;
      logf("HTTP", "other via %s: httpd error %d (%s over %d B) from %s -> 200 success (%lu too long since boot)",
           srcIfName(via), (int)err, err == HTTPD_414_URI_TOO_LONG ? "URI" : "headers", CONFIG_HTTPD_MAX_URI_LEN, ip,
           (unsigned long)gTooLong.load());
    }
  } else {
    char host[64] = "";
    if (httpd_req_get_hdr_value_str(req, "Host", host, sizeof host) == ESP_OK) {
      for (char* p = host; *p; p++) *p = (char)tolower((unsigned char)*p);
      if (strstr(host, "weathercloud")) body = "200";
    }
    logSafe(host);
    // the first two path segments only -- Weathercloud's /v01/set/wid/<WID>/key/<KEY>/... carries both
    // secrets in the path; the rest of the path and the query are never logged
    char path[84];
    const int full = (int)(strchr(req->uri, '?') ? strchr(req->uri, '?') - req->uri : (int)strlen(req->uri));
    const int keep = pathPrefixLen(req->uri, 2);
    snprintf(path, sizeof path, "%.*s", keep > 80 ? 80 : keep, req->uri);
    logSafe(path);
    logl(nOther <= 3 ? Log::Event : Log::Chatter, "HTTP", "other via %s: %s %s%s%s host=%s from %s -> 200 \"%s\"", srcIfName(via),
         http_method_str((enum http_method)req->method), path, keep < full ? "/<more>" : "",
         strchr(req->uri, '?') ? "?<query>" : "", host[0] ? host : "-", ip, body);
  }
  replyOneShot(req, body);
  return ESP_OK;
}

// A signed-out admin call from the home network with the CSRF header -> 401 JSON, so the page
// shows its sign-in instead of a parse error. The body (a PUT or an OTA image) is never read; the session closes.
void replyUnauth(httpd_req_t* req) {
  gReqQuiet = true;
  gReqStatus = 401;
  char ip[16], path[84];
  ipToStr(peerIp(req), ip);
  const int keep = pathPrefixLen(req->uri, 2), full = (int)strcspn(req->uri, "?");
  snprintf(path, sizeof path, "%.*s", keep > 80 ? 80 : keep, req->uri);
  logSafe(path);
  static RateLimitedLine line;  // a signed-out page polls; httpd task only
  uint32_t n, min;
  char sx[32];
  if (line.hit(millis(), &n, &min))
    logf("HTTP", "admin via sta: %s %s%s from %s -> 401 (not signed in)%s", http_method_str((enum http_method)req->method), path,
         keep < full ? "/<more>" : "", ip, rateSuffix(sx, sizeof sx, n, min));
  httpd_resp_set_status(req, "401 Unauthorized");
  httpd_resp_set_hdr(req, "Connection", "close");
  sendJson(req, "{\"error\":\"auth\"}");
  stopReading(req);
}

static esp_err_t onOtherErr(httpd_req_t* req, httpd_err_code_t err) {
  reqBegin();
  const esp_err_t r = onOther(req, err);
  reqEnd(req, r != ESP_OK);
  return r;
}

// In run mode the AP face serves the upload path, /tools and / (= the /tools page) only; every
// other public path answers the catch-all `success`, like an unknown path. The LAN face is unchanged.
// Never in setup mode (the setup page's /a/<hash>/tz.json comes from the AP and must not get `success`).
static bool apCatchAll(httpd_req_t* req) {
  if (!apCatchAllApplies(srcIf(req), gMode == Mode::Setup)) return false;
  onOther(req, HTTPD_404_NOT_FOUND);
  return true;
}

// ---------------------------------------------------------------- console line stash
struct StashLine {
  uint16_t tag;
  char text[CONSOLE_STORE_MAX];  // already redacted, then cut
};
static StashLine gStash[4];  // written by the httpd task, read by loop(); both under gLock
static uint16_t gStashNext = 0;

uint16_t consoleStash(const char* redacted) {
  if (++gStashNext == 0) gStashNext = 1;
  const uint16_t tag = gStashNext;
  xSemaphoreTake(gLock, portMAX_DELAY);
  StashLine& l = gStash[tag % 4];
  l.tag = tag;
  snprintf(l.text, sizeof l.text, "%s", redacted);
  xSemaphoreGive(gLock);
  return tag;
}

ConsoleRing gConsole;  // one ring for all stations, under gLock

void consoleApply(Station& st, uint16_t tag, uint32_t t) {
  if (!tag) return;
  StashLine& l = gStash[tag % 4];
  if (l.tag != tag) return;  // overwritten by a later upload before loop() got here
  gConsole.push(st.id, l.text, t, st.uploads);
  l.tag = 0;
}

void consoleForget(const char* id) {
  xSemaphoreTake(gLock, portMAX_DELAY);
  gConsole.forget(id);
  xSemaphoreGive(gLock);
}

// ---------------------------------------------------------------- run mode: public handlers
static esp_err_t onUpload(httpd_req_t* req) {
  gReqQuiet = true;  // logged below with the parse result
  // The User-Agent is copied before the reply -- httpd_resp_send() purges the request headers (IDF docs), so
  // read afterwards it is always empty and every console would fingerprint "mismatch". A
  // header copy from the parsed request: no socket I/O, so the reply still goes out first in practice.
  static char ua[24];  // "lwIP/2.1.2" on the 7-in-1; only the prefix matters (httpd task only)
  ua[0] = 0;
  httpd_req_get_hdr_value_str(req, "User-Agent", ua, sizeof ua);  // cut by the call if longer
  // Reply first: nothing after this can delay or fail the console's upload.
  // The whole reply in ONE send, as `discovery` does. httpd_resp_send() writes the headers
  // and the body in separate TCP segments ("success" arrives ~16 ms after the headers); the 7-in-1 seems to read
  // only the first segment, finds no "success", waits ~6 s and sends the same GET again (3x per cycle, 78 s
  // instead of 65 s). Then the session closes at once.
  replyOneShot(req, "success");
  // Quiet mode: during an OTA the console still gets "success", the sample is not
  // processed (no history, no MQTT): the heap stays in one piece for the write; a good OTA restarts anyway.
  if (otaActive()) {
    otaCountSkipped();
    return ESP_OK;
  }

  static Upload u;  // httpd task only
  static char query[CONFIG_HTTPD_MAX_URI_LEN + 1];
  static char line[CONSOLE_LINE_MAX];
  size_t qlen = httpd_req_get_url_query_len(req);
  if (qlen == 0 || httpd_req_get_url_query_str(req, query, sizeof query) != ESP_OK) query[0] = 0;
  u.src_if = srcIf(req);
  u.qhash = fnv1a32(query);
  {
    // a keyed hash of the PASSWORD as sent (URL-encoded, case kept); the key itself is wiped at once
    char pw[96];
    queryValueCi(query, "PASSWORD", pw, sizeof pw);  // any case; an over-long key is cut, never emptied
    static const char SALT[] = "fauxcloud station key v1";
    hmacSha256((const uint8_t*)SALT, sizeof SALT - 1, (const uint8_t*)pw, strlen(pw), u.key);
    memset(pw, 0, sizeof pw);
  }
  parseQuery(query, &u.s);
  // the home network only with the switch on; never a blocked id (the reply above stays `success`)
  const char* drop = uploadDrop(u.src_if, gBridge.lan_uploads, blockedHas(u.s.station_id));
  u.src_ip = peerIp(req);
  u.rx_ms = millis();
  u.has_mac = !drop && peerMac(u.src_ip, u.src_if, u.mac);
  u.fp = stationFingerprint(ua, query, u.has_mac ? u.mac : nullptr);
  const uint32_t nUp = ++gUploads;
  char ip[16];
  ipToStr(u.src_ip, ip);
  // the first upload since the restart is an event (the console reaches fauxcloud), the rest chatter
  logl(nUp == 1 ? Log::Boot : Log::Chatter, "HTTP", "upload via %s from %s id=%s fields=%d rejected=0x%05lx extras=%u query=%uB%s%s", srcIfName(u.src_if), ip,
       u.s.station_id, popcount32(u.s.present), (unsigned long)u.s.rejected(), (unsigned)u.s.n_extras, (unsigned)qlen,
       drop ? " dropped " : "", drop ? drop : "");
  if (drop) {
    (drop[0] == 'l' ? gDropLan : gDropBlocked)++;
    return ESP_OK;
  }
  redactRequestLine(http_method_str((enum http_method)req->method), req->uri, line, sizeof line);
  u.line_tag = consoleStash(line);  // a dropped upload takes no console slot
  if (xQueueSend(gQueue, &u, 0) != pdTRUE) {
    gQueueDrops++;
    logf("HTTP", "queue full, sample dropped");
  }
  return ESP_OK;
}

// The tools page at /tools (the status face is the static shell at /), no ETag. Rendering under gLock would let a
// slow client starve the loop task into the task watchdog, so each
// station is copied under the lock into a heap buffer, the card is rendered and sent from the copy with the lock free.
static esp_err_t onTools(httpd_req_t* req) {
  const bool ap = apFace(req);
  Station* copy = (Station*)malloc(sizeof(Station));
  if (!copy) {  // no heap for one station: 503 like the load shedding (robust.h)
    gReqStatus = 503;
    httpd_resp_set_status(req, "503 Service Unavailable");
    httpd_resp_set_hdr(req, "Connection", "close");
    return httpd_resp_send(req, nullptr, 0);
  }
  httpd_resp_set_type(req, "text/html; charset=utf-8");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  const Diag d = ap ? Diag{} : diagNow();
  Out o(sendChunk, req, gChunk, sizeof gChunk);
  renderPageHead(o, clockNow(), SW_VERSION, ap ? nullptr : &d);
  bool any = false;
  for (int i = 0; i < MAX_STATIONS; i++) {
    xSemaphoreTake(gLock, portMAX_DELAY);
    const Station* sp = gStations.at(i);
    if (sp) memcpy((void*)copy, (const void*)sp, sizeof(Station));
    xSemaphoreGive(gLock);
    if (!sp) continue;
    renderStationCard(o, *copy, clockNow());
    any = true;
  }
  free(copy);
  renderPageTail(o, any, !ap);
  return endChunks(req, o);
}

// During a firmware upload no new page loads (heap): open pages keep their live/status polls.
static bool refusedWhileUpdating(httpd_req_t* req) {
  if (!otaActive()) return false;
  gReqStatus = 503;
  httpd_resp_set_status(req, "503 Service Unavailable");
  httpd_resp_set_type(req, "text/plain");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  httpd_resp_set_hdr(req, "Retry-After", "60");
  httpd_resp_set_hdr(req, "Connection", "close");
  const char* body = "fauxcloud is updating its firmware, reload in a minute";
  gReqBytes += strlen(body);
  httpd_resp_sendstr(req, body);
  return true;
}

// The status face: a static shell on both sides; the page hides LAN details on the AP (header).
static esp_err_t onRoot(httpd_req_t* req) {
  if (srcIf(req) == SrcIf::Ap) return onTools(req);  // the AP's / is the plain status page (no JavaScript)
  if (refusedWhileUpdating(req)) return ESP_OK;
  apFace(req);
  return serveGz(req, web::INDEX_HTML_GZ, web::INDEX_HTML_GZ_LEN, web::INDEX_HTML_VERSION);
}

// /api/live?station=<id>: one station, ETag = generation + upload counter + config revision + online (+ face)
// + the outlook's own inputs (hour closes, calibration log, location, real clock).
// /api/live: every station (the tools form), no ETag, copied one station at a time.
static LiveSnap gSnap;  // httpd task only
#if FWD_TLS
// GET /api/metar -- the METAR readings and the reference reading (app_fwd.cpp); the LAN face only (not in the
// mirror's allow-list: an airport is a rough location)
static esp_err_t onMetar(httpd_req_t* req) {
  if (apCatchAll(req)) return ESP_OK;
  // ETag = the store's change counter + the settings + a 15-min clock bucket, so the dashboard's
  // 5-min poll is a 304 most of the time and the ages / stale flags never lag more than 15 min behind
  uint32_t gen = 0, cfg = 0, now = 0;
  metarEtagParts(&gen, &cfg, &now);
  makeEtag(gEtag, gBootId, gen, cfg, now / 900);
  if (notModified(req, "no-store")) return ESP_OK;
  return metarPublicGet(req);
}
#endif

static esp_err_t onLive(httpd_req_t* req) {
  if (apCatchAll(req)) return ESP_OK;
  const bool ap = apFace(req);
  httpd_resp_set_type(req, "application/json");
  char id[STATION_ID_LEN];
  // The diag (a heap read, counters, the tasks) only for an answer with a body -- never for a 304 poll; one Diag
  // in the frame for both answers
  Diag d = {};
  if (queryParam(req, "station", id, sizeof id)) {
    xSemaphoreTake(gLock, portMAX_DELAY);
    const Station* st = gStations.find(id);
    OutNear near;  // in the ETag (a METAR fetch between uploads refreshes the body) and the outlook
    if (st) outlookNearInput(st->last_seen, &near);
    if (st) liveEtag(gEtag, gBootId, *st, millis(), ap, timeSynced(),
                     outlookEtagIn(*st, gBridge.lat, gBridge.lon, outlookTimeOk(), &near) ^ wufcEtagWord());
    xSemaphoreGive(gLock);
    if (!st) return noStation(req);
    if (notModified(req, "no-store")) return ESP_OK;
    xSemaphoreTake(gLock, portMAX_DELAY);
    st = gStations.find(id);
    if (st) {
      snapLive(*st, millis(), &gSnap, &gTz);  // the ETag is re-made from the same moment as the copy
      outlookNearInput(st->last_seen, &near);
      snapOutlook(*st, &gSnap, gBridge.lat, gBridge.lon, outlookTimeOk(), &near);
      gSnap.wufc_on = !ap && wufcSnap(&gSnap.wufc);  // LAN face only: AP pages never see it
      liveEtag(gEtag, gBootId, *st, millis(), ap, timeSynced(),
               outlookEtagIn(*st, gBridge.lat, gBridge.lon, outlookTimeOk(), &near) ^ wufcEtagWord());
    }
    xSemaphoreGive(gLock);
    if (!st) return noStation(req);
    if (!ap) d = diagNow();
    d.page = web::ADMIN_HTML_VERSION;  // the admin's home poll reads its own shell version here
    Out o(sendChunk, req, gChunk, sizeof gChunk);
    renderLiveOne(o, gSnap, clockNow(), ap ? nullptr : &d, web::INDEX_HTML_VERSION);
    return endChunks(req, o);
  }
  if (!ap) d = diagNow();
  d.page = web::ADMIN_HTML_VERSION;
  const Diag* dp = ap ? nullptr : &d;
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  const Clock c = clockNow();
  Out o(sendChunk, req, gChunk, sizeof gChunk);
  renderLiveHead(o, c, dp, web::INDEX_HTML_VERSION);
  o.raw(",\"stations\":[");
  bool first = true;
  for (int i = 0; i < MAX_STATIONS; i++) {
    xSemaphoreTake(gLock, portMAX_DELAY);
    const Station* st = gStations.at(i);
    if (st) {
      snapLive(*st, c.ms, &gSnap, &gTz);
      OutNear near;
      outlookNearInput(st->last_seen, &near);
      snapOutlook(*st, &gSnap, gBridge.lat, gBridge.lon, outlookTimeOk(), &near);
      gSnap.wufc_on = !ap && wufcSnap(&gSnap.wufc);
    }
    xSemaphoreGive(gLock);
    if (!st) continue;
    if (!first) o.raw(",");
    first = false;
    renderLiveStation(o, gSnap, c, dp != nullptr);
  }
  o.raw("]}");
  return endChunks(req, o);
}

// /api/bridge/status (<= 300 B): ETag = the bridge change counter loop() advances (+ face).
static BridgeStatus gStatus;
static esp_err_t onBridgeStatus(httpd_req_t* req) {
  if (apCatchAll(req)) return ESP_OK;
  const bool ap = apFace(req);
  httpd_resp_set_type(req, "application/json");
  makeEtag(gEtag, gBootId, gBridgeRev.load(), 0, ap ? 1 : 0);
  if (notModified(req, "no-store")) return ESP_OK;
  bridgeStatusNow(&gStatus);
  Out o(sendChunk, req, gChunk, sizeof gChunk);
  renderBridgeStatus(o, gStatus);
  return endChunks(req, o);
}

// History: RAM views (no from/to) copy the ring under the lock and render without it; flash views
// (from/to, days beyond the RAM ring, all=1) copy the ring index and stream the records one at a time.
using Hist = HistView;
static_assert(std::is_trivially_copyable<Recent>::value && std::is_trivially_copyable<Hourly>::value &&
                  std::is_trivially_copyable<Daily>::value,
              "history rings are copied with memcpy");

static bool queryU32(httpd_req_t* req, const char* key, uint32_t* out) {
  char b[16];
  if (!queryParam(req, key, b, sizeof b)) return false;
  *out = (uint32_t)strtoul(b, nullptr, 10);
  return true;
}

// The row view of a request: hidden fields and every channel map version (NVS ch:<id> + the current one)
static ChMap gMaps[CH_VERSIONS + 1];
static CalLog gCal;  // httpd task only: the station's calibration log, copied under the lock
static RowView viewOf(const char* id, uint32_t hidden, const ChMap& current, bool raw = false) {
  int n = loadChannelMaps(id, gMaps);
  bool have = !current.ver;
  for (int i = 0; i < n; i++) have = have || gMaps[i].ver == current.ver;
  if (!have) gMaps[n++] = current;  // a map not stored yet (pending station)
  return RowView{hidden, gMaps, n, raw ? nullptr : &gCal};  // raw=1: the rows as stored (the learners' input)
}
// raw=1: the ETag gains the flag (a raw and a calibrated body never share a tag)
static void etagRaw() {
  const size_t n = strlen(gEtag);
  if (n >= 2 && n + 2 < sizeof gEtag) gEtag[n - 1] = 0, strcat(gEtag, "-r\"");
}

static esp_err_t onHistory(httpd_req_t* req, Hist h) {
  httpd_resp_set_type(req, "application/json");
  if (otaActive()) {  // no heap copies while a firmware upload is written (heap > 60 KB)
    gReqStatus = 503;
    httpd_resp_set_status(req, "503 Service Unavailable");
    httpd_resp_set_hdr(req, "Connection", "close");
    return sendJson(req, "{\"error\":\"updating\"}");
  }
  uint32_t from = 0, to = UINT32_MAX, days = 0;
  const bool hasFrom = queryU32(req, "from", &from), hasTo = queryU32(req, "to", &to);
  const bool ranged = hasFrom || hasTo;
  char all[4];
  const bool exportAll = h == Hist::Daily && queryParam(req, "all", all, sizeof all) && all[0] == '1';
  if (h == Hist::Daily) queryU32(req, "days", &days);
  if (days > DAY_SEGS * DAY_PER_SEG) days = DAY_SEGS * DAY_PER_SEG;
  const bool flashView = h != Hist::Recent && (ranged || exportAll || (h == Hist::Daily && days > DAILY_LEN));
  char rawq[4];
  const bool raw = h != Hist::Recent && queryParam(req, "raw", rawq, sizeof rawq) && rawq[0] == '1';  // hourly + daily (L7 replays days)
  char id[STATION_ID_LEN];
  static ChMap cur;
  static FlashRing ring;  // httpd task only
  static HourRec openH;
  static DayRec openD;
  uint32_t hidden = 0;
  bool haveOpen = false, onFlash = false;
  xSemaphoreTake(gLock, portMAX_DELAY);
  const Station* st = pickStation(req);
  if (st) {
    memcpy(id, st->id, sizeof id);
    hidden = st->cfg.disabled;
    cur = st->chmap;
    gCal = st->cal;
    histEtag(gEtag, gBootId, *st, h);
    if (flashView) {
      haveOpen = h == Hist::Hourly ? st->hourly.openRecord(&openH) : st->daily.openRecord(st->hourly, gTz, &openD);
      onFlash = storeRingCopy(id, h == Hist::Hourly ? 'h' : 'd', &ring);
      // the flash view's ETag: + the ring's newest key and count, and the uploads (the open record)
      if (onFlash)  // + calSig, as the RAM views
        makeEtag(gEtag, gBootId, st->gen ^ calSig(st->cal), ring.newestKey(), (uint32_t)ring.count() + st->uploads * 65536u);
    }
    if (raw) etagRaw();
  }
  xSemaphoreGive(gLock);
  if (!st) return noStation(req);
  if (notModified(req, "no-store")) return ESP_OK;
  if (onFlash) {
    const RowView v = viewOf(id, hidden, cur, raw);
    Out o(sendChunk, req, gChunk, sizeof gChunk);
    if (h == Hist::Hourly) {
      renderHourlyRing(o, id, ring, from, to, haveOpen ? &openH : nullptr, v);
    } else {
      // daily: the days whose local midnight is in [from, to); days=N / all=1: the newest N / every day
      const uint32_t fromYmd = hasFrom ? dayNumberToYmd(localDayNumber(gTz, (int64_t)from - 1) + 1) : 0;
      const uint32_t toYmd = hasTo ? dayNumberToYmd(localDayNumber(gTz, (int64_t)to - 1) + 1) : UINT32_MAX;
      const int newest = ranged ? 0 : exportAll ? ring.count() : (int)days;
      renderDailyRing(o, id, ring, fromYmd, toYmd, newest, haveOpen ? &openD : nullptr, v);
    }
    return endChunks(req, o);
  }
  const size_t size = h == Hist::Recent ? sizeof(Recent) : h == Hist::Hourly ? sizeof(Hourly) : sizeof(Daily);
  void* copy = malloc(size);
  if (!copy) {
    httpd_resp_set_status(req, "503 Service Unavailable");
    gReqStatus = 503;
    logf("HTTP", "history copy: no memory for %u B (free8 %u)", (unsigned)size, (unsigned)heapNow().free8);
    return sendJson(req, "{\"error\":\"low memory, try again\"}");
  }
  xSemaphoreTake(gLock, portMAX_DELAY);
  st = pickStation(req);
  if (st) {
    histEtag(gEtag, gBootId, *st, h);
    if (raw) etagRaw();
    memcpy(id, st->id, sizeof id);
    hidden = st->cfg.disabled;
    cur = st->chmap;
    gCal = st->cal;
    memcpy(copy, h == Hist::Recent ? (const void*)&st->recent
                 : h == Hist::Hourly ? (const void*)&st->hourly
                                     : (const void*)&st->daily,
           size);
    if (h == Hist::Daily) haveOpen = st->daily.openRecord(st->hourly, gTz, &openD);
  }
  xSemaphoreGive(gLock);
  if (!st) {
    free(copy);
    return noStation(req);
  }
  Out o(sendChunk, req, gChunk, sizeof gChunk);
  if (h == Hist::Recent) {
    renderRecent(o, id, *(const Recent*)copy, hidden);
  } else {
    const RowView v = viewOf(id, hidden, cur, raw);
    if (h == Hist::Hourly) renderHourly(o, id, *(const Hourly*)copy, v);
    else renderDaily(o, id, *(const Daily*)copy, haveOpen ? &openD : nullptr, days ? (int)days : DAILY_LEN, v);
  }
  free(copy);
  return endChunks(req, o);
}

// GET /api/learn?station= -- the learners' shadow state (LAN, no-store, no ETag; not in the public contract)
static LearnView gLearnView;  // httpd task only
static esp_err_t onLearn(httpd_req_t* req) {
  if (apCatchAll(req)) return ESP_OK;
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  char id[STATION_ID_LEN];
  xSemaphoreTake(gLock, portMAX_DELAY);
  const Station* st = pickStation(req);
  if (st) memcpy(id, st->id, sizeof id);
  xSemaphoreGive(gLock);
  LearnMeta m = {};
  if (!st || !learnCopy(id, &gLearnView, &m)) return noStation(req);
  Out o(sendChunk, req, gChunk, sizeof gChunk);
  learnJson(o, gLearnView, m);
  return endChunks(req, o);
}

static esp_err_t onRecent(httpd_req_t* req) { return apCatchAll(req) ? ESP_OK : onHistory(req, Hist::Recent); }
static esp_err_t onHourly(httpd_req_t* req) { return apCatchAll(req) ? ESP_OK : onHistory(req, Hist::Hourly); }
static esp_err_t onDaily(httpd_req_t* req) { return apCatchAll(req) ? ESP_OK : onHistory(req, Hist::Daily); }

// Read-only config views (tools): copied, then rendered.
static StationConfig gCfgCopy;
static esp_err_t onStationGet(httpd_req_t* req) {
  if (apCatchAll(req)) return ESP_OK;
  char id[STATION_ID_LEN];
  bool stored = false;
  xSemaphoreTake(gLock, portMAX_DELAY);
  const Station* st = pickStation(req);
  if (st) {
    memcpy(id, st->id, sizeof id);
    stored = st->stored;
    gCfgCopy = st->cfg;
  }
  xSemaphoreGive(gLock);
  if (!st) return noStation(req);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  Out o(sendChunk, req, gChunk, sizeof gChunk);
  renderStationConfig(o, id, stored, gCfgCopy, nullptr);
  return endChunks(req, o);
}

static esp_err_t onBridgeGet(httpd_req_t* req) {
  if (apCatchAll(req)) return ESP_OK;
  xSemaphoreTake(gLock, portMAX_DELAY);
  const BridgeConfig b = gBridge;
  xSemaphoreGive(gLock);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  Out o(sendChunk, req, gChunk, sizeof gChunk);
  renderBridgeConfig(o, b, nullptr);
  return endChunks(req, o);
}

// /a/<hash8>/<name>: immutable assets from flash. An unknown hash is a real 404 (never a console path).
static esp_err_t onAsset(httpd_req_t* req) {
  if (apCatchAll(req)) return ESP_OK;
  if (refusedWhileUpdating(req)) return ESP_OK;
  const char* q = strchr(req->uri, '?');
  const size_t n = q ? (size_t)(q - req->uri) : strlen(req->uri);
  for (size_t i = 0; i < web::ASSET_COUNT; i++) {
    const web::Asset& a = web::ASSETS[i];
    if (strlen(a.path) != n || strncmp(a.path, req->uri, n) != 0) continue;
    httpd_resp_set_type(req, a.type);
    httpd_resp_set_hdr(req, "Cache-Control", "public, max-age=31536000, immutable");
    return sendGzFlash(req, a.gz, a.len);
  }
  gReqStatus = 404;
  httpd_resp_set_status(req, "404 Not Found");
  httpd_resp_set_type(req, "text/plain");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  gReqBytes += 9;
  return httpd_resp_sendstr(req, "not found");
}

// ---------------------------------------------------------------- setup mode (AP only, captive)
static bool setupAllowed(httpd_req_t* req) {
  if (srcIf(req) != SrcIf::Ap) {
    onOther(req, HTTPD_404_NOT_FOUND);  // the STA side (during a join attempt) never sees the setup face
    return false;
  }
  gSetupActivityMs = millis();
  return true;
}

static esp_err_t onSetupPage(httpd_req_t* req) {
  if (!setupAllowed(req)) return ESP_OK;
  // A GET / about a second after a phone joins may be its own connectivity check, not the browser
  char host[64] = "-", ua[72] = "-", ip[16];
  httpd_req_get_hdr_value_str(req, "Host", host, sizeof host);
  httpd_req_get_hdr_value_str(req, "User-Agent", ua, sizeof ua);
  ipToStr(peerIp(req), ip);
  char uri[80];
  logUri(req->uri, uri, sizeof uri);  // a console upload's query or a Weathercloud key never reaches the log
  logSafe(host);
  logSafe(ua, true);  // the UA keeps its spaces
  uint32_t rn, rm;
  char sx[32];
  if (gSetupPageLine.hit(millis(), &rn, &rm))  // per request in setup mode, rate limited
    logf("SETUP", "page %s host=%s ua=\"%s\" from %s%s", uri, host, ua, ip, rateSuffix(sx, sizeof sx, rn, rm));
  return serveGz(req, web::SETUP_HTML_GZ, web::SETUP_HTML_GZ_LEN, web::SETUP_HTML_VERSION);
}

// Any other path in setup mode (the OS connectivity probes /hotspot-detect.html, /generate_204, /connecttest.txt,
// /ncsi.txt, /success.txt, /canonical.html, ...) gets a 302 to the setup name: the OS opens its captive sheet there.
static esp_err_t onSetupCaptive(httpd_req_t* req, httpd_err_code_t) {
  reqBegin();
  if (setupAllowed(req)) {
    // every probe with the host and client it came from (phones differ in their captive checks)
    char host[64] = "-", ua[72] = "-", ip[16];
    httpd_req_get_hdr_value_str(req, "Host", host, sizeof host);
    httpd_req_get_hdr_value_str(req, "User-Agent", ua, sizeof ua);
    ipToStr(peerIp(req), ip);
    gReqQuiet = true;
    char uri[80];
    logUri(req->uri, uri, sizeof uri);  // as onSetupPage: no query, no deep path
    logSafe(host);
    logSafe(ua, true);  // the UA keeps its spaces
    uint32_t rn, rm;
    char sx[32];
    if (gSetupProbeLine.hit(millis(), &rn, &rm))
      logf("SETUP", "probe %s %s host=%s ua=\"%s\" from %s -> 302 %s%s", http_method_str((enum http_method)req->method),
           uri, host, ua, ip, SETUP_URL, rateSuffix(sx, sizeof sx, rn, rm));
    gReqStatus = 302;
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", SETUP_URL);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_send(req, nullptr, 0);
  }
  reqEnd(req, false);
  return ESP_OK;
}

static void scanCb(Out& o, httpd_req_t* req) {
  char again[4];
  scanJson(o, queryParam(req, "again", again, sizeof again) && again[0] == '1');
}

static esp_err_t onSetupScan(httpd_req_t* req) {
  if (!setupAllowed(req)) return ESP_OK;
  return sendRendered(req, "application/json", scanCb, false);
}

static void setupStateCb(Out& o, httpd_req_t*) {
  char st[64];
  setupState(st, sizeof st);
  o.raw("{\"state\":");
  o.jsonStr(st);
  o.raw(",\"hostname\":");
  o.jsonStr(gSettings.hostname);
  o.raw(",\"ap_ssid\":");
  char ap[40];
  snprintf(ap, sizeof ap, "fauxcloud-%s", gMac4);  // the run-mode SSID the console will be paired with
  o.jsonStr(ap);
  o.raw(",\"sta_ssid\":");
  o.jsonStr(gSettings.sta_ssid);
  // Setup again offers "Keep the current password" for these two (never the values, only that they exist)
  o.printf(",\"keep_ap\":%s,\"keep_admin\":%s", gSettings.ap_pass[0] ? "true" : "false",
           gSettings.admin_hash[0] ? "true" : "false");
  o.raw(",\"tz\":");
  o.jsonStr(gBridge.tz);
  o.raw(",\"tz_name\":");
  o.jsonStr(gBridge.tz_name);
  char ip[16] = "", mask[16] = "";
  if (staUp()) staIpStr(ip), staMaskStr(mask);
  o.raw(",\"sta_ip\":");  // the success screen shows the address with numbers too
  o.jsonStr(ip);
  o.raw(",\"sta_mask\":");  // with the address: does the home network overlap fauxcloud's own 192.168.77.0/24
  o.jsonStr(mask);
  o.raw("}");
}

static esp_err_t onSetupState(httpd_req_t* req) {
  if (!setupAllowed(req)) return ESP_OK;
  return sendRendered(req, "application/json", setupStateCb);
}

static Errors gSetupErr;
static void setupErrCb(Out& o, httpd_req_t*) {
  o.raw("{\"errors\":[");
  for (int i = 0; i < gSetupErr.n; i++) {
    if (i) o.raw(",");
    o.jsonStr(gSetupErr.at(i));
  }
  o.raw("]}");
}

// The setup page sends the phone's clock once on load, {"t":<ms>}; applied only while no SNTP or
// DHCP time, persisted at once so the setup -> run restart keeps it.
static esp_err_t onSetupTime(httpd_req_t* req) {
  if (!setupAllowed(req)) return ESP_OK;
  size_t len = 0;
  AuthBody b;
  if (!readBody(req, &len) || !parseAuthBody(bodyBuf(), len, &b)) memset(&b, 0, sizeof b);
  const bool applied = b.t_ms && clockSeed(b.t_ms, "setup page");
  char out[64];
  snprintf(out, sizeof out, "{\"applied\":%s,\"time_src\":\"%s\"}", applied ? "true" : "false", timeSrcName(timeSrc()));
  return sendJson(req, out);
}

static esp_err_t onSetupApply(httpd_req_t* req) {
  if (!setupAllowed(req)) return ESP_OK;
  gSetupErr = Errors();
  size_t len = 0;
  if (!readBody(req, &len)) {
    gSetupErr.add("body", "missing or larger than 3072 bytes");
    return sendRendered(req, "application/json", setupErrCb, false);
  }
  char state[64];
  setupState(state, sizeof state);
  if (strcmp(state, "joining") == 0 || strcmp(state, "joined") == 0) {
    gSetupErr.add("state", "already joining, wait for the result");
    return sendRendered(req, "application/json", setupErrCb, false);
  }
  xSemaphoreTake(gLock, portMAX_DELAY);
  Settings s = gSettings;
  BridgeConfig b = gBridge;
  xSemaphoreGive(gLock);
  // The page sends admin_salt + admin_h: the admin password never reaches the ESP.
  bool mqttCleared = false;
  if (!setupFromJson(bodyBuf(), len, &s, &b, &gSetupErr, &mqttCleared))
    return sendRendered(req, "application/json", setupErrCb, false);
  if (mqttCleared) logf("SETUP", "MQTT password cleared: new Wi-Fi network in setup again");
  xSemaphoreTake(gLock, portMAX_DELAY);
  s.setup_done = false;  // set only after the join succeeded
  gSettings = s;
  gBridge = b;
  parsePosixTz(gBridge.tz, &gTz);
  xSemaphoreGive(gLock);
  logf("SETUP", "apply: Wi-Fi \"%s\", time zone %s, hostname %s (passwords not logged)", s.sta_ssid,
       b.tz_name[0] ? b.tz_name : b.tz, s.hostname);
  setupJoinStart();
  return sendRendered(req, "application/json", setupErrCb, false);  // {"errors":[]}
}

uint32_t setupIdleMs() { return millis() - gSetupActivityMs; }

// The admin page itself is static; it is still served on the STA side only (the AP never shows an admin face).
esp_err_t onAdminPage(httpd_req_t* req) {
  if (srcIf(req) != SrcIf::Sta) return onOther(req, HTTPD_404_NOT_FOUND);
  if (refusedWhileUpdating(req)) return ESP_OK;
  return serveGz(req, web::ADMIN_HTML_GZ, web::ADMIN_HTML_GZ_LEN, web::ADMIN_HTML_VERSION);
}

// ---------------------------------------------------------------- server
void httpStart() {
  gBootId = esp_random();
  httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
  cfg.server_port = 80;
  cfg.stack_size = 8192;        // page rendering with float formatting; PBKDF2 (mbedTLS) is shallow
  cfg.lru_purge_enable = true;  // a fresh TCP connection per upload: recycle the oldest socket when full
  // Ten sockets, so a page burst during an OTA cannot purge a console socket (at 7 one was lost). lwIP has 16
  // sockets (httpd_start requires max_open_sockets <= LWIP_MAX_SOCKETS - 3): httpd 10 + its listen and ctrl
  // sockets 2 + DNS and NTP 2 + MQTT 1 = 15.
  cfg.max_open_sockets = 10;
  cfg.close_fn = onSockClose;  // forgets the socket's page mark, then closes it
  cfg.open_fn = onSockOpen;    // marks the accept for the heap minimum's context
  // every module's routes (run mode); FWD_TLS adds its own
  cfg.max_uri_handlers = 59 + (FWD_TLS ? 7 : 0);  // FWD_TLS: /api/metar, four /api/admin/metar*, /api/admin/wufc and /wufc/run
  cfg.send_wait_timeout = 2;    // a stalled browser must not hold the only worker for long
  // 2 s: a partial write makes IDF's httpd_send_all wait twice (6.8 s measured at 3 s), so a
  // client that stops reading holds the worker <= ~4.5 s.
  // 3 s: a client that announces a body and stops holds the worker for two of these (readAll,
  // the OTA's stall rule): <= 6 s ahead of a console upload.
  cfg.recv_wait_timeout = 3;
  cfg.uri_match_fn = httpd_uri_match_wildcard;  // /a/* ; every other URI is literal
  httpd_handle_t srv = nullptr;
  if (httpd_start(&srv, &cfg) != ESP_OK) {
    logf("HTTP", "httpd_start failed");
    return;
  }
  gSrv = srv;
  // The console must never see anything but 200 (a non-200 makes it retry): over-long URI or headers too.
  httpd_register_err_handler(srv, HTTPD_414_URI_TOO_LONG, onOtherErr);
  httpd_register_err_handler(srv, HTTPD_431_REQ_HDR_FIELDS_TOO_LARGE, onOtherErr);

  if (gMode == Mode::Setup) {
    gSetupActivityMs = millis();
    const httpd_uri_t uris[] = {
        {"/", HTTP_GET, onSetupPage, nullptr},
        {"/setup", HTTP_GET, onSetupPage, nullptr},
        {"/api/setup/scan", HTTP_GET, onSetupScan, nullptr},
        {"/api/setup/state", HTTP_GET, onSetupState, nullptr},
        {"/api/setup/apply", HTTP_POST, onSetupApply, nullptr},
        {"/api/setup/time", HTTP_POST, onSetupTime, nullptr},
        {"/a/*", HTTP_GET, onAsset, nullptr},  // the location picker's data packs
    };
    registerUris(srv, uris, sizeof uris / sizeof uris[0]);
    httpd_register_err_handler(srv, HTTPD_404_NOT_FOUND, onSetupCaptive);
    httpd_register_err_handler(srv, HTTPD_405_METHOD_NOT_ALLOWED, onSetupCaptive);
    logf("HTTP", "setup mode: captive setup page (%s) + /api/setup/* on the AP only (setup.html %u B gz, v%s)",
         SETUP_URL, (unsigned)web::SETUP_HTML_GZ_LEN, web::SETUP_HTML_VERSION);
    return;
  }

  const httpd_uri_t uris[] = {
      {WU_PATH, HTTP_GET, onUpload, nullptr},
      {"/", HTTP_GET, onRoot, nullptr},
      {"/tools", HTTP_GET, onTools, nullptr},
      {"/api/live", HTTP_GET, onLive, nullptr},
      {"/api/bridge/status", HTTP_GET, onBridgeStatus, nullptr},
      {"/api/recent", HTTP_GET, onRecent, nullptr},
      {"/api/hourly", HTTP_GET, onHourly, nullptr},
      {"/api/learn", HTTP_GET, onLearn, nullptr},
      {"/api/daily", HTTP_GET, onDaily, nullptr},
      {"/api/station", HTTP_GET, onStationGet, nullptr},
      {"/api/bridge", HTTP_GET, onBridgeGet, nullptr},
#if FWD_TLS
      {"/api/metar", HTTP_GET, onMetar, nullptr},
#endif
      {"/a/*", HTTP_GET, onAsset, nullptr},
      {"/admin", HTTP_GET, onAdminPage, nullptr},
  };
  registerUris(srv, uris, sizeof uris / sizeof uris[0]);
  registerAdmin(srv);
  httpd_register_err_handler(srv, HTTPD_404_NOT_FOUND, onOtherErr);
  httpd_register_err_handler(srv, HTTPD_405_METHOD_NOT_ALLOWED, onOtherErr);
  logf("HTTP", "run mode: esp_http_server on 0.0.0.0:80 (AP + STA), admin on the STA side only, uri max %d B",
       CONFIG_HTTPD_MAX_URI_LEN);
  for (size_t i = 0; i < web::ASSET_COUNT; i++)
    logf("HTTP", "asset %s (%u B gz)", web::ASSETS[i].path, (unsigned)web::ASSETS[i].len);
}

}  // namespace app
