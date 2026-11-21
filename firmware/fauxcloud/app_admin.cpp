// app_admin.cpp -- admin login, sessions and the admin API, STA side only.
//
// Every /api/admin/* request must arrive on the STA interface and carry `X-Fauxcloud: 1` (CSRF: a cross-site form
// cannot set it) and, except challenge + login, a valid `fcs` session cookie presented from the IP it was issued
// to. Anything else is answered exactly like an unknown path (200 "success" + a log line), so the admin surface is
// invisible from the AP and to casual probes -- except a home-network call that carries the header but no valid
// session: 401 {"error":"auth"}, so the page can show its sign-in.
//
// Login is challenge-response: GET /api/admin/challenge -> {nonce, salt, iters}; the browser sends
// proof = HMAC(PBKDF2(password, salt, iters), nonce). The password is never accepted by any endpoint. Failed proofs
// count per IP (5 / 60 s -> 60 s) and account-wide (10 / 10 min -> 5 min, doubling, cap 60 min), RAM only: a power
// cycle clears them (physical access = trust).
#include <WiFi.h>
#include <esp_random.h>
#include <lwip/sockets.h>

#include "app.h"
#include "src/console.h"
#include "src/json.h"

namespace app {

static SessionStore gSessions;     // httpd task only
static Lockout gLockout;           // httpd task only
static NonceStore gNonces;         // httpd task only
static AccountLock gAccount;       // httpd task writes, loop()/diag read under gAuthMux
static portMUX_TYPE gAuthMux = portMUX_INITIALIZER_UNLOCKED;
static uint32_t gLockIps[4];       // distinct IPs of the failures that led to the current account lock
static int gLockIpCount = 0;

static bool csrfOk(httpd_req_t* req) {
  char v[8];
  return httpd_req_get_hdr_value_str(req, "X-Fauxcloud", v, sizeof v) == ESP_OK && strcmp(v, "1") == 0;
}

static bool sessionToken(httpd_req_t* req, char out[33]) {
  char cookie[256];
  if (httpd_req_get_hdr_value_str(req, "Cookie", cookie, sizeof cookie) != ESP_OK) return false;
  return cookieValue(cookie, "fcs", out, 33);
}

static char gCurrentId[9];  // public id of the session of the request being handled (httpd task only)

// The gate of every admin endpoint. false = already answered: as an unknown request, or a JSON 401 to a
// signed-out page on the home network.
static bool gate(httpd_req_t* req, bool needSession) {
  const SrcIf via = srcIf(req);
  const bool csrf = csrfOk(req);
  gCurrentId[0] = 0;
  bool sessionOk = false;
  if (via == SrcIf::Sta && csrf && needSession) {
    char tok[33];
    sessionOk = sessionToken(req, tok) && gSessions.check(tok, peerIp(req), uptimeS(), gCurrentId);
  }
  const AdminGate g = adminGate(via, csrf, needSession, sessionOk);
  if (g == AdminGate::Pass) return true;
  gAdminDenied++;
  if (g == AdminGate::Unauth) replyUnauth(req);
  else onOther(req, HTTPD_404_NOT_FOUND);
  return false;
}

bool adminGateReq(httpd_req_t* req) { return gate(req, true); }  // app_fwd.cpp's routes

static Errors gErr;  // httpd task only
static void errorsArray(Out& o) {
  o.raw("[");
  for (int i = 0; i < gErr.n; i++) {
    if (i) o.raw(",");
    o.jsonStr(gErr.at(i));
  }
  o.raw("]");
}

// ---------------------------------------------------------------- account lock state for diag / loop()
void adminLockInfo(bool* active, uint32_t* remaining_s, int* failures) {
  const uint32_t now = uptimeS();
  uint32_t until = 0;
  portENTER_CRITICAL(&gAuthMux);
  *active = gAccount.locked(now, &until);
  // during a lock the window was reset; report the count that caused it
  *failures = *active ? AccountLock::MAX_FAILS : gAccount.failures(now);
  portEXIT_CRITICAL(&gAuthMux);
  *remaining_s = *active ? until - now : 0;
}

void adminLockTick() {
  portENTER_CRITICAL(&gAuthMux);
  const bool ended = gAccount.expiredSince(uptimeS());
  portEXIT_CRITICAL(&gAuthMux);
  if (ended) logf("AUTH", "account-wide admin lock ended");
}

static void lockIpsStr(char* out, size_t cap) {
  out[0] = 0;
  for (int i = 0; i < gLockIpCount; i++) {
    char ip[16];
    ipToStr(gLockIps[i], ip);
    snprintf(out + strlen(out), cap - strlen(out), "%s%s", i ? ", " : "", ip);
  }
}

// ---------------------------------------------------------------- proof checking (login, reveal, password change)
enum class Proof { Ok, IpLocked, Bad };
static uint32_t gRetry = 0;

// Consumes the nonce, applies both lockouts, logs failures. `what` names the operation for the log.
static Proof checkProof(httpd_req_t* req, const AuthBody& b, const char* what) {
  const uint32_t ip = peerIp(req), now = uptimeS();
  char ipStr[16];
  ipToStr(ip, ipStr);
  if (gLockout.locked(ip, now, &gRetry)) return Proof::IpLocked;
  const bool nonceOk = gNonces.consume(b.nonce, ip, millis());
  xSemaphoreTake(gLock, portMAX_DELAY);
  char stored[HASH_STR_MAX];
  snprintf(stored, sizeof stored, "%s", gSettings.admin_hash);
  xSemaphoreGive(gLock);
  portENTER_CRITICAL(&gAuthMux);
  const bool accountLocked = gAccount.locked(now);
  portEXIT_CRITICAL(&gAuthMux);
  // While the account is locked every proof fails, indistinguishable from a wrong password (no leak to callers).
  const bool ok = nonceOk && !accountLocked && verifyProof(stored, b.nonce, b.proof);
  if (ok) {
    gLockout.success(ip);
    portENTER_CRITICAL(&gAuthMux);
    gAccount.success();
    portEXIT_CRITICAL(&gAuthMux);
    gLockIpCount = 0;
    return Proof::Ok;
  }
  // without a valid nonce of this address there was no password to check -- no lockout counts (an
  // empty POST from anyone on the LAN must not lock the admin out)
  if (!nonceOk) {
    logf("AUTH", "%s refused from %s (unknown or expired nonce, not counted)", what, ipStr);
    return Proof::Bad;
  }
  gLoginFails++;
  const bool ipLock = gLockout.fail(ip, now);
  uint32_t lockLen = 0;
  portENTER_CRITICAL(&gAuthMux);
  const bool accLock = gAccount.fail(now, &lockLen);
  portEXIT_CRITICAL(&gAuthMux);
  bool known = false;
  for (int i = 0; i < gLockIpCount; i++) known |= gLockIps[i] == ip;
  if (!known && gLockIpCount < 4) gLockIps[gLockIpCount++] = ip;
  logf("AUTH", "%s refused from %s (%s)%s", what, ipStr, accountLocked ? "account locked" : "wrong proof",
       ipLock ? " -> this IP locked for 60 s" : "");
  if (accLock) {
    char ips[80];
    lockIpsStr(ips, sizeof ips);
    logf("AUTH", "account-wide admin lock for %lu s after %d failed proofs within 10 min (from %s)",
         (unsigned long)lockLen, AccountLock::MAX_FAILS, ips);
    gLockIpCount = 0;
  }
  return Proof::Bad;
}

static bool readAuth(httpd_req_t* req, AuthBody* b) {
  size_t len = 0;
  return readBody(req, &len) && parseAuthBody(bodyBuf(), len, b);
}

static esp_err_t sendProofFailure(httpd_req_t* req, Proof p) {
  if (p == Proof::IpLocked) {
    char b[80];
    snprintf(b, sizeof b, "{\"ok\":false,\"error\":\"locked\",\"retry_s\":%lu}", (unsigned long)gRetry);
    return sendJson(req, b);
  }
  return sendJson(req, "{\"ok\":false,\"error\":\"wrong password\"}");
}

// ---------------------------------------------------------------- challenge / login / logout
static esp_err_t onChallenge(httpd_req_t* req) {
  if (!gate(req, false)) return ESP_OK;
  uint8_t rnd[16];
  esp_fill_random(rnd, sizeof rnd);
  char nonce[33];
  if (!gNonces.issue(rnd, peerIp(req), millis(), nonce)) return sendJson(req, "{\"error\":\"rate\",\"retry_ms\":1000}");
  xSemaphoreTake(gLock, portMAX_DELAY);
  char stored[HASH_STR_MAX];
  snprintf(stored, sizeof stored, "%s", gSettings.admin_hash);
  xSemaphoreGive(gLock);
  uint8_t salt[SALT_LEN], h[32];
  uint32_t iters = PBKDF2_ITER;
  if (!parseCredential(stored, salt, &iters, h)) esp_fill_random(salt, sizeof salt);  // no credential: no hint
  memset(h, 0, sizeof h);
  char saltHex[2 * SALT_LEN + 1], b[160];
  toHex(salt, SALT_LEN, saltHex);
  snprintf(b, sizeof b, "{\"nonce\":\"%s\",\"salt\":\"%s\",\"iters\":%lu}", nonce, saltHex, (unsigned long)iters);
  return sendJson(req, b);
}

static esp_err_t onLogin(httpd_req_t* req) {
  if (!gate(req, false)) return ESP_OK;
  AuthBody b;
  if (!readAuth(req, &b)) memset(&b, 0, sizeof b);
  const Proof p = checkProof(req, b, "login");
  if (p != Proof::Ok) return sendProofFailure(req, p);
  uint8_t tokRnd[16], idRnd[4];
  esp_fill_random(tokRnd, sizeof tokRnd);
  esp_fill_random(idRnd, sizeof idRnd);
  char ua[SessionStore::UA_MAX + 1] = "";
  httpd_req_get_hdr_value_str(req, "User-Agent", ua, sizeof ua);  // truncated by the call if longer
  char tok[33];
  const uint32_t ip = peerIp(req);
  gSessions.create(tokRnd, idRnd, ip, ua, uptimeS(), tok);
  char cookie[120];
  snprintf(cookie, sizeof cookie, "fcs=%s; HttpOnly; SameSite=Strict; Path=/; Max-Age=%lu", tok,
           (unsigned long)SessionStore::IDLE_S);
  httpd_resp_set_hdr(req, "Set-Cookie", cookie);
  char ipStr[16];
  ipToStr(ip, ipStr);
  logf("AUTH", "login from %s (%d session(s))", ipStr, gSessions.count(uptimeS()));
  // the login body carries the browser's clock; applied only after a valid proof
  const bool applied = b.t_ms && clockSeed(b.t_ms, "admin login");
  char out[80];
  snprintf(out, sizeof out, "{\"ok\":true,\"applied\":%s,\"time_src\":\"%s\"}", applied ? "true" : "false",
           timeSrcName(timeSrc()));
  return sendJson(req, out);
}

static esp_err_t onLogout(httpd_req_t* req) {
  if (!gate(req, true)) return ESP_OK;
  char tok[33];
  if (sessionToken(req, tok)) gSessions.remove(tok);
  httpd_resp_set_hdr(req, "Set-Cookie", "fcs=; HttpOnly; SameSite=Strict; Path=/; Max-Age=0");
  return sendJson(req, "{\"ok\":true}");
}

// ---------------------------------------------------------------- sessions
static void sessionsCb(Out& o, httpd_req_t*) {
  const uint32_t now = uptimeS();
  SessionStore::Info info[SessionStore::MAX];
  const int n = gSessions.list(now, info, SessionStore::MAX);
  o.raw("[");
  for (int i = 0; i < n; i++) {
    char ip[16];
    ipToStr(info[i].ip, ip);
    o.printf("%s{\"id\":\"%s\",\"ip\":\"%s\",\"ua\":", i ? "," : "", info[i].id, ip);
    o.jsonStr(info[i].ua);
    o.printf(",\"created_s\":%lu,\"last_s\":%lu,\"current\":%s}", (unsigned long)(now - info[i].created),
             (unsigned long)(now - info[i].last), strcmp(info[i].id, gCurrentId) == 0 ? "true" : "false");
  }
  o.raw("]");
}

static esp_err_t onSessions(httpd_req_t* req) {
  if (!gate(req, true)) return ESP_OK;
  return sendRendered(req, "application/json", sessionsCb, false);
}

static esp_err_t onRevoke(httpd_req_t* req) {
  if (!gate(req, true)) return ESP_OK;
  size_t len = 0;
  char id[16] = "";
  if (readBody(req, &len)) {
    JsonReader r(bodyBuf(), len);
    char k[8];
    if (r.beginObject())
      while (r.nextKey(k, sizeof k)) {
        if (strcmp(k, "id") == 0 && r.peek() == '"') r.readString(id, sizeof id);
        else r.skipValue();
      }
  }
  const bool ok = gSessions.revoke(id);
  logf("AUTH", "session %s revoked: %s", id, ok ? "yes" : "no such session");
  return sendJson(req, ok ? "{\"ok\":true}" : "{\"ok\":false,\"errors\":[\"id: no such session\"]}");
}

static esp_err_t onRevokeAll(httpd_req_t* req) {
  if (!gate(req, true)) return ESP_OK;
  gSessions.clear();
  httpd_resp_set_hdr(req, "Set-Cookie", "fcs=; HttpOnly; SameSite=Strict; Path=/; Max-Age=0");
  logf("AUTH", "all sessions revoked");
  return sendJson(req, "{\"ok\":true}");
}

// ---------------------------------------------------------------- AP password reveal
// The one readable secret: pairing another console later. Session + CSRF + a fresh proof (both lockouts).
static char gRevealed[64];
static esp_err_t onApPassReveal(httpd_req_t* req) {
  if (!gate(req, true)) return ESP_OK;
  AuthBody b;
  if (!readAuth(req, &b)) memset(&b, 0, sizeof b);
  const Proof p = checkProof(req, b, "AP password reveal");
  if (p != Proof::Ok) return sendProofFailure(req, p);
  xSemaphoreTake(gLock, portMAX_DELAY);
  snprintf(gRevealed, sizeof gRevealed, "%s", gSettings.ap_pass);
  xSemaphoreGive(gLock);
  char ip[16];
  ipToStr(peerIp(req), ip);
  logf("AUTH", "AP password revealed to %s (value not logged)", ip);
  sendRendered(req, "application/json", [](Out& o, httpd_req_t*) {
    o.raw("{\"ok\":true,\"ap_pass\":");
    o.jsonStr(gRevealed);
    o.raw("}");
  }, false);
  memset(gRevealed, 0, sizeof gRevealed);
  return ESP_OK;
}

// ---------------------------------------------------------------- password change
static esp_err_t onPassword(httpd_req_t* req) {
  if (!gate(req, true)) return ESP_OK;
  AuthBody b;
  if (!readAuth(req, &b)) memset(&b, 0, sizeof b);
  char cred[HASH_STR_MAX];
  if (!credentialFromBody(b, cred, sizeof cred))
    return sendJson(req, "{\"ok\":false,\"errors\":[\"salt / h / iters: the new credential is malformed\"]}");
  const Proof p = checkProof(req, b, "password change");
  if (p != Proof::Ok) return sendProofFailure(req, p);
  xSemaphoreTake(gLock, portMAX_DELAY);
  snprintf(gSettings.admin_hash, sizeof gSettings.admin_hash, "%s", cred);
  xSemaphoreGive(gLock);
  const bool saved = saveSettings();
  // Other sessions were opened with the old password: log them out, keep the one that made the change.
  SessionStore::Info info[SessionStore::MAX];
  const int n = gSessions.list(uptimeS(), info, SessionStore::MAX);
  for (int i = 0; i < n; i++)
    if (strcmp(info[i].id, gCurrentId) != 0) gSessions.revoke(info[i].id);
  logf("ADMIN", "admin password changed (%s), %d other session(s) revoked", saved ? "saved" : "NOT saved", n - 1);
  return sendJson(req, saved ? "{\"ok\":true,\"errors\":[]}" : "{\"ok\":false,\"errors\":[\"nvs: not saved\"]}");
}

// ---------------------------------------------------------------- settings
static const char* gNotes[3];
static int gNoteCount = 0;

static void settingsCb(Out& o, httpd_req_t*) {
  o.raw("{\"settings\":");
  settingsToJson(o, gSettings);
  o.raw(",\"wifi_change\":");
  o.jsonStr(wifiChangeStatus());
  o.raw(",\"ap_ssid\":");
  o.jsonStr(gApSsid);
  o.printf(",\"sessions\":%d,\"errors\":", gSessions.count(uptimeS()));
  errorsArray(o);
  o.raw(",\"notes\":[");
  for (int i = 0; i < gNoteCount; i++) {
    if (i) o.raw(",");
    o.jsonStr(gNotes[i]);
  }
  o.raw("]}");
}

static esp_err_t onSettingsGet(httpd_req_t* req) {
  if (!gate(req, true)) return ESP_OK;
  gErr = Errors();
  gNoteCount = 0;
  return sendRendered(req, "application/json", settingsCb);
}

static esp_err_t onSettingsPut(httpd_req_t* req) {
  if (!gate(req, true)) return ESP_OK;
  gErr = Errors();
  gNoteCount = 0;
  size_t len = 0;
  if (!readBody(req, &len)) {
    gErr.add("body", "missing or larger than 3072 bytes");
    return sendRendered(req, "application/json", settingsCb);
  }
  xSemaphoreTake(gLock, portMAX_DELAY);
  Settings s = gSettings;
  xSemaphoreGive(gLock);
  SettingsChange ch;
  settingsFromJson(bodyBuf(), len, &s, &gErr, &ch);
  // Wi-Fi credentials are only stored after the new network worked; everything else applies now.
  char newSsid[33], newPass[64];
  snprintf(newSsid, sizeof newSsid, "%s", s.sta_ssid);
  snprintf(newPass, sizeof newPass, "%s", s.sta_pass);
  xSemaphoreTake(gLock, portMAX_DELAY);
  snprintf(s.sta_ssid, sizeof s.sta_ssid, "%s", gSettings.sta_ssid);
  snprintf(s.sta_pass, sizeof s.sta_pass, "%s", gSettings.sta_pass);
  gSettings = s;
  xSemaphoreGive(gLock);
  if (ch.mqtt || ch.ap || ch.hostname) {
    if (!saveSettings()) gErr.add("nvs", "not saved; applied until reboot");
  }
  if (ch.mqtt) requestMqttRestart();
  if (ch.hostname) {
    requestMdnsRestart();
    WiFi.setHostname(s.hostname);
  }
  if (ch.ap) {
    requestApRestart();
    gNotes[gNoteCount++] = "The fauxcloud Wi-Fi password changed: re-pair the weather console(s) with the new password.";
  }
  if (ch.wifi) {
    wifiChangeStart(newSsid, newPass);
    gNotes[gNoteCount++] =
        "Trying the new home Wi-Fi for up to 60 s; if it fails, the old one is restored. Reload this page "
        "afterwards (the address may change).";
  }
  memset(newPass, 0, sizeof newPass);
  logf("ADMIN", "settings PUT: wifi=%d mqtt=%d ap=%d hostname=%d, %d error(s)%s%s", ch.wifi, ch.mqtt, ch.ap, ch.hostname,
       gErr.n, ch.mqtt_pass_cleared ? "; MQTT password cleared (new broker, none sent)" : "",
       ch.sta_pass_cleared ? "; new Wi-Fi tried without a password (none sent)" : "");
  return sendRendered(req, "application/json", settingsCb);
}

// ---------------------------------------------------------------- station / bridge
// Copies for snapshot-then-stream (httpd task only).
static struct {
  bool found, stored;
  char id[STATION_ID_LEN];
  StationConfig cfg;
  KeyState key;  // bound / mismatches, never the key
} gStCopy;
static BridgeConfig gBrCopy;

// Copies the station from ?station= (or the first one) into gStCopy.
static void copyStation(httpd_req_t* req) {
  char id[STATION_ID_LEN];
  const bool named = queryParam(req, "station", id, sizeof id);
  xSemaphoreTake(gLock, portMAX_DELAY);
  const Station* st = named ? gStations.find(id) : gStations.first();
  gStCopy.found = st != nullptr;
  if (st) {
    gStCopy.stored = st->stored;
    memcpy(gStCopy.id, st->id, sizeof gStCopy.id);
    gStCopy.cfg = st->cfg;
    gStCopy.key = KeyState{st->key_bound, st->key_mismatch, st->has_src_mac, {}, st->fp, st->cal.n, {}};
    memcpy(gStCopy.key.mac, st->src_mac, 6);
    adoptOffer(st->id, gStCopy.key.adopt);
    // the learners in use, from the station's LearnUse snapshot (the Learning tile's "{n} of 4 in use")
    for (uint8_t b = LU_L1; b <= LU_L6; b <<= 1) gStCopy.key.learn_use += (st->use.bits & b) ? 1 : 0;
  }
  xSemaphoreGive(gLock);
}

static void stationCopyCb(Out& o, httpd_req_t*) {
  if (gStCopy.found) renderStationConfig(o, gStCopy.id, gStCopy.stored, gStCopy.cfg, gErr.n ? &gErr : nullptr, &gStCopy.key);
  else o.raw("{\"error\":\"no such station\"}");
}

static esp_err_t onStationGet(httpd_req_t* req) {
  if (!gate(req, true)) return ESP_OK;
  gErr = Errors();
  copyStation(req);
  return sendRendered(req, "application/json", stationCopyCb, false);
}

static bool validId(const char* id) {
  if (!id[0]) return false;
  for (const char* p = id; *p; p++)
    if (!(isalnum((unsigned char)*p) || *p == '_' || *p == '-')) return false;
  return true;
}

static esp_err_t onStationPut(httpd_req_t* req) {
  if (!gate(req, true)) return ESP_OK;
  char id[STATION_ID_LEN];
  if (!queryParam(req, "station", id, sizeof id) || !validId(id))
    return sendJson(req, "{\"errors\":[\"station: ?station=ID with [A-Za-z0-9_-]\"]}");
  size_t len = 0;
  if (!readBody(req, &len)) return sendJson(req, "{\"errors\":[\"body: missing or larger than 3072 bytes\"]}");
  gErr = Errors();
  xSemaphoreTake(gLock, portMAX_DELAY);
  StationConfig initial = defaultStationConfig();
  initial.publish = gBridge.auto_publish_new_stations;
  // a not-yet-seen station can be configured ahead, within max_stations; every refusal is decided before a slot or a
  // calibration log is taken
  static CalLog calStored;  // httpd task only
  static Errors probe;      // the probe only looks at the calibration; the errors come from the apply below
  probe = Errors();
  const Station* known = gStations.find(id);
  const bool haveStored = !(known && known->cal.n) && readCalLog(id, &calStored);
  Station* st = nullptr;
  const PutAdmit adm = stationPutAdmit(gStations, gParked, id, initial, gBridge.max_stations, timeSynced(),
                                       haveStored ? &calStored : nullptr, bodyBuf(), len, &probe, &st);
  int slot = -1;
  static CalLog calSave;  // httpd task only
  bool calNew = false;
  if (st) {
    if (!st->cal.n) loadCalLog(*st);  // a station configured ahead: its log starts with the old calibration
    StationConfig c = st->cfg;
    stationFromJson(bodyBuf(), len, &c, &gErr);
    const bool profileChanged = c.profile != st->cfg.profile;
    st->cfg = c;
    calNew = calAppend(&st->cal, calOf(c, nowS()));  // a changed offset or factor applies from now
    if (calNew) calSave = st->cal;
    if (profileChanged) Stations::profileChanged(*st, nowS());  // the rain derivation starts over
    Stations::repromote(*st);  // a key promoted now shows its last known value at once, with its age
    st->cfg_rev++;  // the live ETag changes: names and promotions show at once
    slot = gStations.indexOf(st);
  }
  xSemaphoreGive(gLock);
  if (adm == PutAdmit::Parked)
    return sendJson(req, "{\"errors\":[\"station: parked with its saved settings; it gets a place after a restart\"]}");
  if (adm == PutAdmit::NoSlot) return sendJson(req, "{\"errors\":[\"station: max_stations reached, no free slot or no memory\"]}");
  // a calibration change needs a real clock (its log entry is stamped with it): 200 + errors like the refusals above,
  // so the page shows the sentence
  if (adm == PutAdmit::CalRefused)
    return sendJson(req, "{\"errors\":[\"calibration: set the clock first (offsets and factors are logged with the time)\"]}");
  if (!saveStation(slot)) gErr.add("nvs", "not saved (too large or NVS error); applied until reboot");
  if (calNew) saveCalLog(id, calSave);
  markStationChanged(slot);
  logf("ADMIN", "station %s PUT: %d error(s)%s", id, gErr.n, calNew ? ", calibration log entry added" : "");
  copyStation(req);
  gStCopy.found = true;  // PUT answers always carry the errors array
  return sendRendered(req, "application/json", [](Out& o, httpd_req_t*) {
    renderStationConfig(o, gStCopy.id, gStCopy.stored, gStCopy.cfg, &gErr);
  }, false);
}

// Forget on a known key (admin Extra sensors): gone until the console sends it again.
// POST /api/admin/station/forget-key?station=<ID>&key=<key> (empty body)
static esp_err_t onForgetKey(httpd_req_t* req) {
  if (!gate(req, true)) return ESP_OK;
  char id[STATION_ID_LEN] = "", key[16] = "";
  if (!queryParam(req, "station", id, sizeof id) || !validId(id) || !queryParam(req, "key", key, sizeof key) || !key[0])
    return sendJson(req, "{\"ok\":false,\"errors\":[\"station, key: ?station=ID&key=KEY\"]}");
  xSemaphoreTake(gLock, portMAX_DELAY);
  Station* st = gStations.find(id);
  const bool ok = st && Stations::forgetKey(*st, key);
  if (ok) st->cfg_rev++;  // the live ETag changes
  xSemaphoreGive(gLock);
  if (!ok) return sendJson(req, "{\"ok\":false,\"errors\":[\"key: not known for this station\"]}");
  logf("ADMIN", "station %s: known key %s forgotten", id, key);
  return sendJson(req, "{\"ok\":true}");
}

// "Restart learning" (the Learning sheet): every learner of the station back to the standard rules until it is
// learned again. POST /api/learn/relearn?station=<ID> (empty body); the loop applies it within a second.
static esp_err_t onLearnRelearn(httpd_req_t* req) {
  if (!gate(req, true)) return ESP_OK;
  char id[STATION_ID_LEN] = "";
  if (!queryParam(req, "station", id, sizeof id) || !validId(id))
    return sendJson(req, "{\"ok\":false,\"errors\":[\"station: ?station=ID\"]}");
  if (!learnRelearn(id)) return sendJson(req, "{\"ok\":false,\"errors\":[\"station: no learning state\"]}");
  logf("ADMIN", "station %s: restart of learning asked", id);
  return sendJson(req, "{\"ok\":true}");
}

// Forget: loop() removes the HA entities, publishes offline, deletes the NVS config and frees the slot.
// POST /api/admin/station/forget?station=<ID> (empty body); a {"station": ...} body is accepted as a fallback.
static esp_err_t onStationForget(httpd_req_t* req) {
  if (!gate(req, true)) return ESP_OK;
  size_t len = 0;
  char id[STATION_ID_LEN] = "";
  if (!queryParam(req, "station", id, sizeof id) && readBody(req, &len)) {
    JsonReader r(bodyBuf(), len);
    char k[12];
    if (r.beginObject())
      while (r.nextKey(k, sizeof k)) {
        if (strcmp(k, "station") == 0 && r.peek() == '"') r.readString(id, sizeof id);
        else r.skipValue();
      }
  }
  xSemaphoreTake(gLock, portMAX_DELAY);
  Station* st = validId(id) ? gStations.find(id) : nullptr;
  const int slot = st ? gStations.indexOf(st) : -1;
  const bool parked = validId(id) && gParked.has(id);
  xSemaphoreGive(gLock);
  // on flash only (no RAM slot): freed by its header id; or settings parked without a slot, or leftover keys of an id
  // with no config (the same path: the NVS entries go, a missing ring is a no-op)
  if (slot < 0 && validId(id) && (storeOwns(id) || parked || stationKeysStored(id))) {
    requestForgetFlash(id);
    logf("ADMIN", "forget %s station %s requested", parked ? "parked" : "flash-only", id);
    return sendJson(req, "{\"ok\":true}");
  }
  if (slot < 0) return sendJson(req, "{\"ok\":false,\"errors\":[\"station: no such station\"]}");
  requestForget(slot);
  logf("ADMIN", "forget station %s requested (slot %d)", id, slot);
  return sendJson(req, "{\"ok\":true}");
}

// POST /api/admin/station/adopt {"from":"OLD1","to":"NEW1"} (adopt.h) -- the offer must still stand for exactly this
// pair (the same device, within the note's hour); the journal goes to NVS and a planned restart moves everything at the
// next boot. {"ok":true,"restart":true} | {"ok":false,"errors":["adopt: ..."]}
static esp_err_t onStationAdopt(httpd_req_t* req) {
  if (!gate(req, true)) return ESP_OK;
  size_t len = 0;
  char from[STATION_ID_LEN] = "", to[STATION_ID_LEN] = "";
  if (readBody(req, &len)) {
    JsonReader r(bodyBuf(), len);
    char k[8];
    if (r.beginObject())
      while (r.nextKey(k, sizeof k)) {
        if (strcmp(k, "from") == 0 && r.peek() == '"') r.readString(from, sizeof from);
        else if (strcmp(k, "to") == 0 && r.peek() == '"') r.readString(to, sizeof to);
        else r.skipValue();
      }
  }
  if (!validId(from) || !validId(to)) return sendJson(req, "{\"ok\":false,\"errors\":[\"adopt: from and to are station ids\"]}");
  // a restart while this image is unconfirmed would roll back to one that ignores the journal
  if (otaPending()) return sendJson(req, "{\"ok\":false,\"errors\":[\"adopt: fauxcloud is still checking its new firmware; try in a minute\"]}");
  uint8_t key[32];
  xSemaphoreTake(gLock, portMAX_DELAY);
  const bool ok = adoptTake(from, to, key);
  xSemaphoreGive(gLock);
  if (!ok) return sendJson(req, "{\"ok\":false,\"errors\":[\"adopt: the offer has expired\"]}");
  const char* why = nullptr;
  if (!adoptWriteJournal(from, to, key, &why)) {
    memset(key, 0, sizeof key);
    static char e[96];
    snprintf(e, sizeof e, "{\"ok\":false,\"errors\":[\"adopt: %s\"]}", why ? why : "not saved");
    return sendJson(req, e);
  }
  memset(key, 0, sizeof key);
  logf("ADMIN", "adopt %s as %s confirmed: the move runs at the restart", to, from);
  sysPlanRestart("adopt", 3000);
  return sendJson(req, "{\"ok\":true,\"restart\":true}");
}

// GET /api/admin/rings -- {"rings":[{"id":"...","hours":N,"days":N,"slot":bool}],"parked":["..."]}: the history
// rings on flash; slot:false = a station that exists on flash only (the admin offers Forget for it); "parked" =
// stored settings that got no slot at boot (Forget too)
static esp_err_t onRingsGet(httpd_req_t* req) {
  if (!gate(req, true)) return ESP_OK;
  static StoreRingRow rows[RING_SLOTS];
  const int n = storeRingList(rows);
  bool slot[RING_SLOTS] = {};
  static ParkedList parked;
  xSemaphoreTake(gLock, portMAX_DELAY);
  for (int i = 0; i < n; i++) slot[i] = gStations.find(rows[i].id) != nullptr;
  parked = parkedListing(gParked, gLeftover, gStations);  // an id that took a slot since is a live station
  xSemaphoreGive(gLock);
  static bool slots[RING_SLOTS];
  static int count;
  memcpy(slots, slot, sizeof slots);
  count = n;
  return sendRendered(req, "application/json", [](Out& o, httpd_req_t*) {
    o.raw("{\"rings\":[");
    for (int i = 0; i < count; i++) {
      o.raw(i ? ",{\"id\":" : "{\"id\":");
      o.jsonStr(rows[i].id);
      o.printf(",\"hours\":%d,\"days\":%d,\"slot\":%s}", rows[i].hours, rows[i].days, slots[i] ? "true" : "false");
    }
    o.raw("],\"parked\":[");
    for (int i = 0; i < parked.n; i++) {
      if (i) o.raw(",");
      o.jsonStr(parked.ids[i]);
    }
    o.raw("]}");
  }, false);
}

static esp_err_t onBridgeGet(httpd_req_t* req) {
  if (!gate(req, true)) return ESP_OK;
  xSemaphoreTake(gLock, portMAX_DELAY);
  gBrCopy = gBridge;
  xSemaphoreGive(gLock);
  return sendRendered(req, "application/json", [](Out& o, httpd_req_t*) { renderBridgeConfig(o, gBrCopy, nullptr); },
                      false);
}

static esp_err_t onBridgePut(httpd_req_t* req) {
  if (!gate(req, true)) return ESP_OK;
  size_t len = 0;
  if (!readBody(req, &len)) return sendJson(req, "{\"errors\":[\"body: missing or larger than 3072 bytes\"]}");
  gErr = Errors();
  xSemaphoreTake(gLock, portMAX_DELAY);
  BridgeConfig b = gBridge;
  bridgeFromJson(bodyBuf(), len, &b, &gErr, gStations.active());  // max_stations below the stations in use: refused
  PosixTz parsed;
  if (parsePosixTz(b.tz, &parsed)) gTz = parsed;  // fromJson validated it already
  gBridge = b;
  // a raised max_stations reserves its slots now; a lowered one keeps the slots until the next restart
  const int before = gStations.allocated(), slots = gStations.reserve(b.max_stations);
  xSemaphoreGive(gLock);
  if (slots != before) logf("HEAP", "max_stations %u: %d station slot(s) reserved now", (unsigned)b.max_stations, slots);
  applyTz();
  if (!saveBridge()) gErr.add("nvs", "not saved; applied until reboot");
  logf("ADMIN", "bridge PUT: TZ %s (%s), max_stations %u, detailed log %s, %d error(s)", b.tz, b.tz_name,
       (unsigned)b.max_stations, b.log_verbose ? "on" : "off", gErr.n);
  gBrCopy = b;
  return sendRendered(req, "application/json", [](Out& o, httpd_req_t*) { renderBridgeConfig(o, gBrCopy, &gErr); },
                      false);
}

// ---------------------------------------------------------------- the console key, block / unblock
// POST /api/admin/station/accept-key?station=ID: forget the bound key; the next upload binds again.
static esp_err_t onAcceptKey(httpd_req_t* req) {
  if (!gate(req, true)) return ESP_OK;
  char id[STATION_ID_LEN] = "";
  queryParam(req, "station", id, sizeof id);
  xSemaphoreTake(gLock, portMAX_DELAY);
  Station* st = validId(id) ? gStations.find(id) : nullptr;
  if (st) {
    st->key_bound = false;
    st->key_mismatch = 0;
  }
  xSemaphoreGive(gLock);
  if (!st) return sendJson(req, "{\"ok\":false,\"errors\":[\"station: no such station\"]}");
  saveStationKey(id, nullptr);
  logf("ADMIN", "%s: key released, the next upload binds its key", id);
  return sendJson(req, "{\"ok\":true}");
}

// POST /api/admin/station/block?station=ID: forget the station (if it is here) and drop its uploads from now on.
// The id need not be a station now (a neighbour's console seen in the log). Unblock removes it from the list.
static esp_err_t onBlock(httpd_req_t* req, bool block) {
  if (!gate(req, true)) return ESP_OK;
  char id[STATION_ID_LEN] = "";
  queryParam(req, "station", id, sizeof id);
  if (!validId(id)) return sendJson(req, "{\"ok\":false,\"errors\":[\"station: ?station=ID with [A-Za-z0-9_-]\"]}");
  if (!block) {
    const bool was = blockedRemove(id);
    logf("ADMIN", "unblock %s%s", id, was ? "" : " (was not blocked)");
    return sendJson(req, "{\"ok\":true}");
  }
  if (!blockedAdd(id)) {  // the list holds 8 ids; a 9th is refused, never evicts the oldest
    logf("ADMIN", "block %s refused: the block list is full", id);
    return sendJson(req, "{\"ok\":false,\"errors\":[\"block: list not saved\"]}");
  }
  xSemaphoreTake(gLock, portMAX_DELAY);
  Station* st = gStations.find(id);
  const int slot = st ? gStations.indexOf(st) : -1;
  xSemaphoreGive(gLock);
  if (slot >= 0) requestForget(slot);
  logf("ADMIN", "block %s%s", id, slot >= 0 ? " (station forgotten)" : "");
  return sendJson(req, "{\"ok\":true}");
}
static esp_err_t onBlockPost(httpd_req_t* req) { return onBlock(req, true); }
static esp_err_t onUnblockPost(httpd_req_t* req) { return onBlock(req, false); }

// GET /api/admin/station/blocked -> {"blocked":["ID",...]}
static esp_err_t onBlockedGet(httpd_req_t* req) {
  if (!gate(req, true)) return ESP_OK;
  return sendRendered(req, "application/json", [](Out& o, httpd_req_t*) {
    char ids[8][STATION_ID_LEN];
    const int n = blockedList(ids, 8);
    o.raw("{\"blocked\":[");
    for (int i = 0; i < n; i++) {
      if (i) o.raw(",");
      o.jsonStr(ids[i]);
    }
    o.raw("]}");
  }, false);
}

// ---------------------------------------------------------------- console, debug roll
// GET /api/admin/station/console?station=<ID>&since=<seq> -> {"first","last","items":[{seq,t,line}]}
static ConsoleRing gConsoleCopy;
static uint32_t gConsoleSince = 0;
static char gConsoleId[STATION_ID_LEN];
static esp_err_t onConsole(httpd_req_t* req) {
  if (!gate(req, true)) return ESP_OK;
  char id[STATION_ID_LEN], since[12];
  gConsoleSince = queryParam(req, "since", since, sizeof since) ? (uint32_t)strtoul(since, nullptr, 10) : 0;
  const bool named = queryParam(req, "station", id, sizeof id);
  xSemaphoreTake(gLock, portMAX_DELAY);
  const Station* st = named ? gStations.find(id) : gStations.first();
  if (st) {
    gConsoleCopy = gConsole;
    memcpy(gConsoleId, st->id, sizeof gConsoleId);
  }
  xSemaphoreGive(gLock);
  if (!st) return sendJson(req, "{\"error\":\"no such station\"}");
  return sendRendered(req, "application/json",
                      [](Out& o, httpd_req_t*) { renderConsole(o, gConsoleCopy, gConsoleId, gConsoleSince); }, false);
}

// ---------------------------------------------------------------- firmware upload, crash data
// POST /api/admin/ota, body = the raw .bin (Content-Length required). The OTA task answers {ok, bytes, version}
// and restarts, or {error} without a restart.
static esp_err_t onOta(httpd_req_t* req) {
  if (!gate(req, true)) return ESP_OK;
  if (captureQuiet()) {  // listening to a new console: the update waits for its stop (a capture start waits for an update)
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_set_status(req, "409 Conflict");
    sendJson(req, "{\"error\":\"capture\"}");
    shutdown(httpd_req_to_sockfd(req), SHUT_RD);
    httpd_sess_trigger_close(req->handle, httpd_req_to_sockfd(req));
    return ESP_OK;
  }
  const char* refused = otaStart(req);
  if (!refused) return ESP_OK;  // the OTA task owns the request now
  // A refusal closes the connection instead of reading the rest of a 1.4 MB body into nowhere (httpd
  // would drain it on the only worker and the console would wait 5 s). The browser may see the reset before the reply.
  httpd_resp_set_hdr(req, "Connection", "close");
  const bool busy = strcmp(refused, "busy") == 0;
  if (busy) httpd_resp_set_status(req, "409 Conflict");
  sendJson(req, busy ? "{\"error\":\"an update is already running\"}" : refused);
  shutdown(httpd_req_to_sockfd(req), SHUT_RD);  // the drain after this handler ends at once
  httpd_sess_trigger_close(req->handle, httpd_req_to_sockfd(req));
  return ESP_OK;
}

static bool putChunk(void* ctx, const uint8_t* data, size_t len) {
  return httpd_resp_send_chunk((httpd_req_t*)ctx, (const char*)data, len) == ESP_OK;
}

// POST /api/admin/coredump {nonce, proof}: the raw dump for tools/decode-coredump.ps1; DELETE clears it.
// The dump is a RAM image with the passwords in it, so the download asks for a fresh proof (the reveal
// pattern, both lockouts) and the crash data is erased once it was sent whole.
static esp_err_t onCoredumpGet(httpd_req_t* req) {
  if (!gate(req, true)) return ESP_OK;
  AuthBody b;
  if (!readAuth(req, &b)) memset(&b, 0, sizeof b);
  const Proof p = checkProof(req, b, "crash data download");
  if (p != Proof::Ok) return sendProofFailure(req, p);
  if (!lastCrash().present) return sendJson(req, "{\"error\":\"no crash data\"}");
  httpd_resp_set_type(req, "application/octet-stream");
  // the raw flash image of the dump (header + ELF + CRC): espcoredump.py info_corefile --core-format raw
  httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=\"fauxcloud-coredump.bin\"");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  size_t total = 0;
  const bool ok = crashStream(putChunk, req, &total) && httpd_resp_send_chunk(req, nullptr, 0) == ESP_OK;
  if (ok) logf("SYS", "coredump sent (%u B) and %s", (unsigned)total, crashErase() ? "erased" : "NOT erased");
  else logf("SYS", "coredump download FAILED (%u B sent), kept", (unsigned)total);
  return ok ? ESP_OK : ESP_FAIL;  // a cut download: close the socket (endChunks)
}

// GET /api/admin/bootlog -- text/plain: one header line, the head, a separator, the tail (src/bootlog.h).
static bool bootChunk(void* ctx, const char* data, size_t len) {
  return httpd_resp_send_chunk((httpd_req_t*)ctx, data, len) == ESP_OK;
}
static esp_err_t onBootLog(httpd_req_t* req) {
  if (!gate(req, true)) return ESP_OK;
  httpd_resp_set_type(req, "text/plain; charset=utf-8");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  char* buf = httpScratch();  // the httpd task's render buffer
  Out o(bootChunk, req, buf, HTTP_SCRATCH_LEN);
  o.printf("%s boot log, uptime %llu s, restart #%lu, reset reason %s\n", SW_VERSION,
           (unsigned long long)(esp_timer_get_time() / 1000000), (unsigned long)restarts(), lastReset());
  const bool ok = bootlogSend(o) && httpd_resp_send_chunk(req, nullptr, 0) == ESP_OK;
  return ok ? ESP_OK : ESP_FAIL;  // a cut send: close the socket (as the coredump)
}

static esp_err_t onCoredumpDelete(httpd_req_t* req) {
  if (!gate(req, true)) return ESP_OK;
  const bool ok = crashErase();
  logf("SYS", "coredump cleared: %s", ok ? "yes" : "FAILED");
  return sendJson(req, ok ? "{\"ok\":true}" : "{\"ok\":false,\"errors\":[\"crash data: not cleared\"]}");
}

#ifdef FAUXCLOUD_TEST
// Test builds only (tools\build.ps1 fauxcloud -Define FAUXCLOUD_TEST); a release image has neither.
// POST /api/admin/debug/hang: the loop busy-waits 40 s, so the Task WDT (30 s) must reset the device.
static esp_err_t onDebugHang(httpd_req_t* req) {
  if (!gate(req, true)) return ESP_OK;
  logf("ADMIN", "debug hang requested");
  requestHang();
  return sendJson(req, "{\"ok\":true}");
}

// POST /api/admin/debug/seed-days?station=<ID>&extra=5: the station's rings filled past wrap -- 37 hourly and 17
// daily segments written whole, then `extra` real appends that rotate; the newest hour is the one before the current
// hour, the newest day yesterday. For an id that has no slot yet: its first upload restores the rings (boot path).
static esp_err_t onDebugSeed(httpd_req_t* req) {
  if (!gate(req, true)) return ESP_OK;
  char id[STATION_ID_LEN], ex[8] = "5";
  if (!queryParam(req, "station", id, sizeof id)) return sendJson(req, "{\"error\":\"station?\"}");
  queryParam(req, "extra", ex, sizeof ex);
  int extra = atoi(ex);
  if (extra < 0 || extra > 100) extra = 5;
  xSemaphoreTake(gLock, portMAX_DELAY);
  const bool taken = gStations.find(id) != nullptr;
  xSemaphoreGive(gLock);
  if (taken) return sendJson(req, "{\"error\":\"that id has a slot: forget it first\"}");
  if (!timeSynced()) return sendJson(req, "{\"error\":\"clock not synced\"}");
  const uint32_t now = nowS();
  const uint32_t lastHour = now - now % 3600 - 3600;
  const uint32_t yesterday = dayNumberToYmd(localDayNumber(gTz, now) - 1);
  char msg[160];
  const bool ok = storeSeed(id, lastHour, yesterday, extra, msg, sizeof msg);
  char b[220];
  snprintf(b, sizeof b, "{\"ok\":%s,\"result\":\"%s\"}", ok ? "true" : "false", msg);
  return sendJson(req, b);
}

// POST /api/admin/debug/roll?station=<ID>&what=hour|day: closes the open hour (or hour + day) now. Test aid (test
// builds only).
static esp_err_t onDebugRoll(httpd_req_t* req) {
  if (!gate(req, true)) return ESP_OK;
  char id[STATION_ID_LEN], what[8] = "hour";
  const bool named = queryParam(req, "station", id, sizeof id);
  queryParam(req, "what", what, sizeof what);
  const bool day = strcmp(what, "day") == 0;
  xSemaphoreTake(gLock, portMAX_DELAY);
  Station* st = named ? gStations.find(id) : gStations.first();
  const bool closed = st && (day ? Stations::closeDayNow(*st, gTz) : Stations::closeHourNow(*st, gTz));
  if (st) memcpy(id, st->id, sizeof id);
  if (closed) storeMarkDue(gStations.indexOf(st), true, day);  // same flash writes as a real close
  xSemaphoreGive(gLock);
  if (!st) return sendJson(req, "{\"error\":\"no such station\"}");
  logf("HIST", "debug roll (%s) for %s: %s", day ? "hour + day" : "hour", id, closed ? "closed" : "nothing open");
  return sendJson(req, closed ? "{\"ok\":true,\"closed\":true}" : "{\"ok\":true,\"closed\":false}");
}
#endif

// ---------------------------------------------------------------- Wi-Fi scan, MQTT test
static esp_err_t onScan(httpd_req_t* req) {
  if (!gate(req, true)) return ESP_OK;
  return sendRendered(req, "application/json", [](Out& o, httpd_req_t* r) {
    char again[4];
    scanJson(o, queryParam(r, "again", again, sizeof again) && again[0] == '1');
  }, false);
}

static esp_err_t onMqttTestPost(httpd_req_t* req) {
  if (!gate(req, true)) return ESP_OK;
  size_t len = 0;
  xSemaphoreTake(gLock, portMAX_DELAY);
  Settings s = gSettings;  // an empty / "<set>" password means "the stored one"
  xSemaphoreGive(gLock);
  if (readBody(req, &len)) {
    Errors e;
    SettingsChange ch;
    settingsFromJson(bodyBuf(), len, &s, &e, &ch);
  }
  mqttTestStart(s.mqtt_host, s.mqtt_port, s.mqtt_user, s.mqtt_pass);
  memset(&s, 0, sizeof s);
  return sendJson(req, "{\"state\":\"running\",\"detail\":\"\"}");
}

static esp_err_t onMqttTestGet(httpd_req_t* req) {
  if (!gate(req, true)) return ESP_OK;
  return sendRendered(req, "application/json", [](Out& o, httpd_req_t*) { mqttTestJson(o); }, false);
}

// ---------------------------------------------------------------- device actions
static esp_err_t onReboot(httpd_req_t* req) {
  if (!gate(req, true)) return ESP_OK;
  logf("ADMIN", "reboot requested");
  sysPlanRestart("admin", 1500);
  return sendJson(req, "{\"ok\":true}");
}

static esp_err_t onFactoryReset(httpd_req_t* req) {
  if (!gate(req, true)) return ESP_OK;
  logf("ADMIN", "factory reset requested");
  sendJson(req, "{\"ok\":true}");
  requestFactoryReset();
  return ESP_OK;
}

static esp_err_t onSetupAgain(httpd_req_t* req) {
  if (!gate(req, true)) return ESP_OK;
  xSemaphoreTake(gLock, portMAX_DELAY);
  gSettings.setup_done = false;  // station configs and the rest stay
  xSemaphoreGive(gLock);
  const bool saved = saveSettings();
  logf("ADMIN", "setup again requested (%s) -> reboot into setup mode", saved ? "saved" : "NOT saved");
  sysPlanRestart("setup_again", 1500);
  return sendJson(req, saved ? "{\"ok\":true}" : "{\"ok\":false,\"errors\":[\"nvs: not saved\"]}");
}

void registerAdmin(httpd_handle_t srv) {
  const httpd_uri_t uris[] = {
      {"/api/admin/challenge", HTTP_GET, onChallenge, nullptr},
      {"/api/admin/login", HTTP_POST, onLogin, nullptr},
      {"/api/admin/logout", HTTP_POST, onLogout, nullptr},
      {"/api/admin/sessions", HTTP_GET, onSessions, nullptr},
      {"/api/admin/sessions/revoke", HTTP_POST, onRevoke, nullptr},
      {"/api/admin/sessions/revoke-all", HTTP_POST, onRevokeAll, nullptr},
      {"/api/admin/settings", HTTP_GET, onSettingsGet, nullptr},
      {"/api/admin/settings", HTTP_PUT, onSettingsPut, nullptr},
      {"/api/admin/password", HTTP_PUT, onPassword, nullptr},
      {"/api/admin/ap-pass/reveal", HTTP_POST, onApPassReveal, nullptr},
      {"/api/admin/station", HTTP_GET, onStationGet, nullptr},
      {"/api/admin/station", HTTP_PUT, onStationPut, nullptr},
      {"/api/admin/station/forget", HTTP_POST, onStationForget, nullptr},
      {"/api/admin/station/adopt", HTTP_POST, onStationAdopt, nullptr},
      {"/api/admin/station/accept-key", HTTP_POST, onAcceptKey, nullptr},
      {"/api/admin/station/forget-key", HTTP_POST, onForgetKey, nullptr},
      {"/api/learn/relearn", HTTP_POST, onLearnRelearn, nullptr},
      {"/api/admin/station/block", HTTP_POST, onBlockPost, nullptr},
      {"/api/admin/station/unblock", HTTP_POST, onUnblockPost, nullptr},
      {"/api/admin/station/blocked", HTTP_GET, onBlockedGet, nullptr},
      {"/api/admin/station/console", HTTP_GET, onConsole, nullptr},
      {"/api/admin/rings", HTTP_GET, onRingsGet, nullptr},
#ifdef FAUXCLOUD_TEST
      {"/api/admin/debug/roll", HTTP_POST, onDebugRoll, nullptr},
      {"/api/admin/debug/hang", HTTP_POST, onDebugHang, nullptr},
      {"/api/admin/debug/seed-days", HTTP_POST, onDebugSeed, nullptr},
#endif
      {"/api/admin/ota", HTTP_POST, onOta, nullptr},
      {"/api/admin/coredump", HTTP_POST, onCoredumpGet, nullptr},
      {"/api/admin/coredump", HTTP_DELETE, onCoredumpDelete, nullptr},
      {"/api/admin/bootlog", HTTP_GET, onBootLog, nullptr},
      {"/api/admin/bridge", HTTP_GET, onBridgeGet, nullptr},
      {"/api/admin/bridge", HTTP_PUT, onBridgePut, nullptr},
      {"/api/admin/wifi/scan", HTTP_GET, onScan, nullptr},
      {"/api/admin/mqtt/test", HTTP_POST, onMqttTestPost, nullptr},
      {"/api/admin/mqtt/test", HTTP_GET, onMqttTestGet, nullptr},
      {"/api/admin/reboot", HTTP_POST, onReboot, nullptr},
      {"/api/admin/factory-reset", HTTP_POST, onFactoryReset, nullptr},
      {"/api/admin/setup-again", HTTP_POST, onSetupAgain, nullptr},
  };
  registerUris(srv, uris, sizeof uris / sizeof uris[0]);
  registerFwd(srv);  // /api/admin/forwarding* (app_fwd.cpp)
  registerCapture(srv);  // /api/admin/capture* (app_capture.cpp)
}

}  // namespace app
