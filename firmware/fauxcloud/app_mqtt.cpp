// app_mqtt.cpp -- MQTT client + HA publishing, upload ingest, per-second work, heartbeat, MQTT test (see app.h).
#include <Preferences.h>
#include <WiFi.h>
#include <esp_heap_caps.h>
#include <mqtt_client.h>
#include <time.h>

#include <cmath>

#include "app.h"
#include "src/fields.h"
#include "src/filter.h"
#include "src/ha_discovery.h"
#include "src/derive.h"
#include "src/notices.h"
#include "src/forecast.h"
#include "src/fwd.h"  // FC_DIAG_BENCH
#include "src/adopt.h"

namespace app {

// What loop() needs to publish one station, copied under gLock so MQTT I/O runs without holding it.
struct PubView {
  int slot;
  char id[STATION_ID_LEN];
  char name[64];
  bool publish;
  uint32_t seen_fields;
  uint32_t disabled;
  uint32_t last_seen;
  bool online;
  bool has_live;
  Sample live;
  uint8_t promoted_mask;
  float promoted[MAX_PROMOTE];
  Promote promote[MAX_PROMOTE];
  uint32_t rej_rule[R_COUNT];
  bool rain_rate;      // the profile's rainin is a rate -> rain_mm is derived (own availability + attributes)
  bool rain_warm;
  bool rain_hours;     // rain_mm estimated from the hour records until warm (attribute source "hours")
  uint16_t rain_n;
  uint32_t derived;    // derived wind fields this upload, with their readings
  uint8_t derived_n[F_COUNT];
  bool baro_factory;   // the profile has a factory pressure (its pressure entity carries "calibrated")
  bool baro_uncal;
  bool sensor_stale;   // the outdoor sensor link is not updating
  bool w10_fresh;      // wind_avg10m_kmh computed on this upload
  float w10;
  uint8_t w10_n;
};

static esp_mqtt_client_handle_t gMqtt = nullptr;
static volatile bool gMqttConnected = false;
static volatile bool gNeedAnnounce = false;   // set on (re)connect, consumed by loop()
static volatile bool gRestartReq = false;
static char gBridgeTopic[64];
static constexpr uint32_t STALL_MS = 5000;  // publishing paused after a publish that waited

// Per slot, set by the PUT handlers (under gLock), consumed by loop(): config changed -> re-announce.
static bool gCfgChanged[MAX_STATIONS];

// loop() only
static bool gStaleSent[MAX_STATIONS];
static bool gOffPublished[MAX_STATIONS];
static uint32_t gLastSeenPubMs[MAX_STATIONS];
static uint32_t gRejPub[MAX_STATIONS][R_COUNT];
static bool gRejPubValid[MAX_STATIONS];
// The derived rain_mm's retained availability (-2 = not sent since the announce) and the retained attribute
// payloads (rain, pressure, the derived wind) by their FNV-1a, 0 = not sent since the announce
static int8_t gRainAvail[MAX_STATIONS];
static const uint8_t META_FIELDS[] = {F_RAIN_MM, F_PRESSURE_HPA, F_WIND_AVG2M_KMH, F_WIND_DIR_AVG2M, F_GUST_10M_KMH};
constexpr int META_N = sizeof META_FIELDS;
static uint32_t gAttrSig[MAX_STATIONS][META_N];
static uint32_t gW10Sig[MAX_STATIONS];  // wind_avg10m_kmh's retained attributes (FNV-1a; 0 = not sent)
// The station notices last published (FNV-1a of the state + attributes) and when
static uint32_t gNoticeSig[MAX_STATIONS], gNoticeMs[MAX_STATIONS];
// The "Local outlook" sensor: last sent signature + time, and the upload / hour-close counters it was computed at
static uint32_t gOutSig[MAX_STATIONS], gOutMs[MAX_STATIONS], gOutUp[MAX_STATIONS], gOutClose[MAX_STATIONS];
static bool gFpWarned[MAX_STATIONS];  // logged once per boot
static uint32_t gPublished = 0, gPublishFails = 0;
static std::atomic<uint32_t> gConnects{0};  // MQTT_EVENT_CONNECTED; reconnects = all but the first
static bool gTzChecked = false;
static char gConfigUrl[24];  // "http://<sta-ip>/" for every HA device, "" while the STA has no address
// HA subscribes to a new entity's state topic only after it handled the discovery message, so the
// first (non-retained) state right after discovery is lost. After a discovery the state goes out ~2 s later.
static uint32_t gStateDueMs[MAX_STATIONS];  // 0 = nothing due
// The one PubView scratch (~1 KB) of the loop task. Ingest, forget, the paced announce and the deferred
// states each fill and use it in turn; none of them runs inside another.
static PubView gView;
static uint8_t gAnnStep[MAX_STATIONS];  // the paced discovery's next item + 1 per slot (0 = idle)
static bool gQuietOff[MAX_STATIONS];    // "offline" still owed for a station with no upload since boot

bool mqttUp() { return gMqttConnected; }
int mqttOutbox() { return gMqtt ? esp_mqtt_client_get_outbox_size(gMqtt) : -1; }
uint32_t mqttPublishedN() { return gPublished; }
uint32_t mqttReconnects() { const uint32_t c = gConnects.load(); return c ? c - 1 : 0; }
void requestMqttRestart() { gRestartReq = true; }
// A Wi-Fi (re)join asks for a start only when there is no client (esp-mqtt reconnects by itself); a
// null esp_mqtt_client_init is retried 30 s later from mqttLoop
static volatile bool gStartReq = false;
static uint32_t gInitRetryMs = 0, gInitFails = 0;
void requestMqttStart() { gStartReq = true; }
uint32_t mqttInitFails() { return gInitFails; }

void markStationChanged(int slot) {
  if (slot >= 0 && slot < MAX_STATIONS) gCfgChanged[slot] = true;
}

static volatile bool gForgetReq[MAX_STATIONS];
// The last console turned away for lack of a slot (RAM only, under gLock): named on the dashboard and in the admin
// until 1 h after its last try or a Forget frees a slot; never stored (the console knocks again)
static NoSlotNote gNoSlot1;  // the rules in station.h (host-tested)
std::atomic<uint32_t> gBridgeRev{1};
// A station whose ring (and NVS logs) outlived its RAM slot -- forgotten by id on the loop task
static char gForgetFlash[STATION_ID_LEN];
static volatile bool gForgetFlashReq = false;
void requestForgetFlash(const char* id) {
  if (gForgetFlashReq) return;
  snprintf(gForgetFlash, sizeof gForgetFlash, "%s", id);
  gForgetFlashReq = true;
}

// The offer as the admin sees it (adopt.h); the caller holds gLock. `to` = the turned-away id while it uploads from
// the same device as `from` (the note's hour), else "".
void adoptOffer(const char* from, char to[STATION_ID_LEN]) {
  to[0] = 0;
  gNoSlot1.expire(millis());
  if (gNoSlot1.same && gNoSlot1.id[0] && strncmp(gNoSlot1.from, from, STATION_ID_LEN) == 0)
    memcpy(to, gNoSlot1.id, STATION_ID_LEN);
}
// The route's re-check (gLock held): the offer still stands for exactly this pair; then its key hash
bool adoptTake(const char* from, const char* to, uint8_t key[32]) {
  gNoSlot1.expire(millis());
  if (!gNoSlot1.offers(from, to) || !gStations.find(from)) return false;  // the pure pair check
  memcpy(key, gNoSlot1.key_hash, 32);
  return true;
}

void requestForget(int slot) {
  if (slot >= 0 && slot < MAX_STATIONS) gForgetReq[slot] = true;
}

// ---------------------------------------------------------------- client
// The state lines through MqttStateLine (src/log.h, host-tested): a change of direction prints
// (the log ends on the true state), a retry while down is counted and summed once per 10 min, a flapping broker is held
// after 3 changes and settled by heartbeat(). An outage retry fires ERROR then DISCONNECTED: the ERROR only keeps its
// detail, which the DISCONNECTED line prints. Under gMqttLogMux (the MQTT task and the loop task).
static MqttStateLine gMqttLine;
static portMUX_TYPE gMqttLogMux = portMUX_INITIALIZER_UNLOCKED;
static char gMqttErr[56];  // the last error's detail ("" = none); MQTT task only
static void mqttStateLine(bool up) {
  uint32_t n = 1, min = 0;
  portENTER_CRITICAL(&gMqttLogMux);
  const MqttStateLine::Out out = gMqttLine.event(up, millis(), &n, &min);
  portEXIT_CRITICAL(&gMqttLogMux);
  if (out == MqttStateLine::None) return;
  char sx[32];
  const char* suffix = out == MqttStateLine::Repeat ? rateSuffix(sx, sizeof sx, n, min) : "";
  static bool upOnce = false;  // only the first connect is a boot fact, later ones are Events
  if (up) logl(upOnce ? Log::Event : Log::Boot, "MQTT", "connected%s", suffix), upOnce = true;
  else logf("MQTT", "disconnected%s%s%s%s", gMqttErr[0] ? " (" : "", gMqttErr, gMqttErr[0] ? ")" : "", suffix);
}
static void mqttEvent(void*, esp_event_base_t, int32_t id, void* data) {
  auto* e = (esp_mqtt_event_handle_t)data;
  switch ((esp_mqtt_event_id_t)id) {
    case MQTT_EVENT_CONNECTED:
      gConnects++;
      gMqttConnected = true;
      gNeedAnnounce = true;
      gMqttErr[0] = 0;
      mqttStateLine(true);
      break;
    case MQTT_EVENT_DISCONNECTED:
      gMqttConnected = false;
      mqttStateLine(false);
      break;
    case MQTT_EVENT_ERROR:
      if (!e->error_handle) break;
      if (e->error_handle->error_type == MQTT_ERROR_TYPE_CONNECTION_REFUSED)
        snprintf(gMqttErr, sizeof gMqttErr, "refused, code %d", (int)e->error_handle->connect_return_code);
      else
        snprintf(gMqttErr, sizeof gMqttErr, "error type %d, esp_err 0x%x, errno %d", (int)e->error_handle->error_type,
                 (unsigned)e->error_handle->esp_tls_last_esp_err, e->error_handle->esp_transport_sock_errno);
      break;
    default:
      break;
  }
}

static void mqttStop() {
  if (!gMqtt) return;
  esp_mqtt_client_stop(gMqtt);
  esp_mqtt_client_destroy(gMqtt);
  gMqtt = nullptr;
  gMqttConnected = false;
}

// (Re)starts the client from gSettings; no client at all when no broker is configured (dashboard-only mode).
static void mqttStart() {
  static char host[64], user[64], pass[64], clientId[32];
  xSemaphoreTake(gLock, portMAX_DELAY);
  snprintf(host, sizeof host, "%s", gSettings.mqtt_host);
  snprintf(user, sizeof user, "%s", gSettings.mqtt_user);
  snprintf(pass, sizeof pass, "%s", gSettings.mqtt_pass);
  const uint16_t port = gSettings.mqtt_port;
  xSemaphoreGive(gLock);
  mqttStop();
  if (!host[0]) {
    logf("MQTT", "no broker configured -- MQTT off (dashboard only)");
    return;
  }
  snprintf(clientId, sizeof clientId, "fauxcloud-%02x%02x%02x", gMac[3], gMac[4], gMac[5]);
  bridgeStatusTopic(gBridgeTopic, sizeof gBridgeTopic, gMac);

  esp_mqtt_client_config_t cfg = {};
  cfg.broker.address.hostname = host;
  cfg.broker.address.port = port;
  cfg.broker.address.transport = MQTT_TRANSPORT_OVER_TCP;
  cfg.credentials.client_id = clientId;
  if (user[0]) cfg.credentials.username = user;
  if (pass[0]) cfg.credentials.authentication.password = pass;
  cfg.session.keepalive = 30;
  // Bridge-level LWT: one per connection, so it describes the ESP; stations have their own topic.
  cfg.session.last_will.topic = gBridgeTopic;
  cfg.session.last_will.msg = "offline";
  cfg.session.last_will.msg_len = 7;
  cfg.session.last_will.qos = 1;
  cfg.session.last_will.retain = 1;
  cfg.buffer.size = 1024;
  cfg.outbox.limit = 8 * 1024;
  cfg.network.timeout_ms = 3000;  // the longest a blocked write holds loop() (default 10 s)

  gMqtt = esp_mqtt_client_init(&cfg);
  if (!gMqtt) {  // no memory for its task / buffers: no calls on null, retried in 30 s
    gInitFails++;
    gInitRetryMs = millis() | 1;
    logf("MQTT", "client init FAILED (largest free block %u B): retry in 30 s",
         (unsigned)heapNow().largest8);
    return;
  }
  esp_mqtt_client_register_event(gMqtt, MQTT_EVENT_ANY, mqttEvent, nullptr);
  esp_mqtt_client_start(gMqtt);
  logf("MQTT", "client started, broker port %u, client id %s", (unsigned)port, clientId);
}

// QoS 0, written to the socket in the caller's context (loop task). Never called from the httpd task and never
// under gLock, so a slow broker delays only MQTT, not uploads, pages, DNS or NTP.
// With the default network timeout (10 s) a wedged broker holds loop() that long per message, and a discovery burst can
// trip the 30 s loop watchdog. So the timeout is 3 s, and a publish that waited over 1 s pauses publishing for
// STALL_MS (failures counted), so one pass waits at most once. The paced announce (announceTick) waits out a pause
// instead of skipping items. esp_mqtt_client_enqueue is not used: its task sends one queued QoS 0
// message per loop (~1/s), so a discovery would take 14 s and four stations' states would not keep up.
static uint32_t gStallUntilMs = 0, gStalls = 0;
static uint32_t gPassPubMs = 0;  // publish time in this loop() pass (reset by mqttLoop)
static bool stalled() { return gStallUntilMs && (int32_t)(millis() - gStallUntilMs) < 0; }
static bool publish(const char* topic, const char* payload, bool retain) {
  if (!gMqttConnected) return false;
  if (otaActive()) return false;  // quiet mode: no MQTT buffers while the OTA writes (a good OTA restarts)
  if (stalled()) {
    gPublishFails++;
    return false;
  }
  gStallUntilMs = 0;
  const uint32_t t0 = millis();
  const int id = esp_mqtt_client_publish(gMqtt, topic, payload, 0, 0, retain ? 1 : 0);
  const uint32_t took = millis() - t0;
  gPassPubMs += took;
  // one publish over 1 s, or many slow ones adding up to 1 s in this pass (a half-dead broker)
  if (took > 1000 || gPassPubMs > 1000) {
    gPassPubMs = 0;
    gStallUntilMs = millis() + STALL_MS;
    if (!gStallUntilMs) gStallUntilMs = 1;
    if (++gStalls == 1 || gStalls % 20 == 0)
      logf("MQTT", "broker not taking data (a publish waited %lu ms, over 1 s in this pass): publishing paused 5 s "
           "(%lu since boot)", (unsigned long)took, (unsigned long)gStalls);
  }
  if (id < 0) {
    gPublishFails++;
    return false;
  }
  gPublished++;
  return true;
}
static bool paceOk() { return gMqtt && !stalled(); }

static bool publishAvailability(const char* id, bool online) {
  char t[64];
  return availabilityTopic(t, sizeof t, gMac, id) > 0 && publish(t, online ? "online" : "offline", true);
}
// The station is gone (forgotten, a ghost): its retained availability is cleared, not left "offline" for ever
// (otherwise the broker keeps one per test station)
static bool clearAvailability(const char* id) {
  char t[64];
  return availabilityTopic(t, sizeof t, gMac, id) > 0 && publish(t, "", true);
}

// The derived rain_mm's own availability ("offline" until the hour is warm) and the entities'
// attributes (derived values: source + n [+ window_s]; pressure: calibrated), retained, sent on change; cleared
// ("" retained) for a station that is gone or whose profile no longer has them
static void rainAvail(const char* id, int slot, int8_t avail) {
  char t[80];
  if (avail != gRainAvail[slot] && entitySubTopic(t, sizeof t, gMac, id, FIELDS[F_RAIN_MM].key, "availability") > 0 &&
      publish(t, avail < 0 ? "" : avail ? "online" : "offline", true))
    gRainAvail[slot] = avail;
}
static void attrMeta(const char* id, int slot, int m, const char* json) {
  const uint32_t sig = fnv1a32(json) | 1u;  // never 0 ("not sent")
  char t[80];
  if (sig != gAttrSig[slot][m] && entitySubTopic(t, sizeof t, gMac, id, FIELDS[META_FIELDS[m]].key, "attr") > 0 &&
      publish(t, json, true))
    gAttrSig[slot][m] = sig;
}
static void clearMeta(const char* id) {
  char t[80];
  if (entitySubTopic(t, sizeof t, gMac, id, FIELDS[F_RAIN_MM].key, "availability") > 0) publish(t, "", true);
  for (int m = 0; m < META_N; m++)
    if (entitySubTopic(t, sizeof t, gMac, id, FIELDS[META_FIELDS[m]].key, "attr") > 0) publish(t, "", true);
  if (entitySubTopic(t, sizeof t, gMac, id, fieldSpec(FIELD_WIND_AVG10M).key, "attr") > 0) publish(t, "", true);
}
static void publishMeta(const PubView& v) {
  char json[64];
  for (int m = 0; m < META_N; m++) {
    const int f = META_FIELDS[m];
    if ((v.disabled >> f) & 1u) continue;
    if (f == F_RAIN_MM) {
      if (!v.rain_rate) continue;
      rainAvail(v.id, v.slot, v.rain_warm || v.rain_hours ? 1 : 0);
      if (!v.rain_warm && !v.rain_hours) continue;
      snprintf(json, sizeof json, "{\"source\":\"%s\",\"n\":%u}", v.rain_warm ? "derived" : "hours", (unsigned)v.rain_n);
    } else if (f == F_PRESSURE_HPA) {
      if (!v.baro_factory) continue;
      snprintf(json, sizeof json, "{\"calibrated\":%s}", v.baro_uncal ? "false" : "true");
    } else {
      if (!((v.derived >> f) & 1u)) {
        char t[80];  // once, when a derivation stops (the console sends the field itself)
        if (gAttrSig[v.slot][m] && entitySubTopic(t, sizeof t, gMac, v.id, FIELDS[f].key, "attr") > 0 && publish(t, "", true))
          gAttrSig[v.slot][m] = 0;
        continue;
      }
      uint32_t w = 0;
      for (const DerivedDef& d : WIND_DERIVED)
        if (d.field == f) w = d.window_s;
      snprintf(json, sizeof json, "{\"source\":\"derived\",\"n\":%u,\"window_s\":%lu}", (unsigned)v.derived_n[f],
               (unsigned long)w);
    }
    attrMeta(v.id, v.slot, m, json);
  }
}
// The station's "Outdoor sensor link" and "Notices" entities, removed with the station
static void removeExtras(const char* id) {
  char topic[128];
  const EntitySpec link = fieldSpec(FIELD_SENSOR_LINK);
  if (discoveryTopicKey(topic, sizeof topic, id, gMac, link.key, link.component) > 0) publish(topic, "", true);
  if (discoveryTopicKey(topic, sizeof topic, id, gMac, fieldSpec(FIELD_NOTICES).key) > 0) publish(topic, "", true);
  if (discoveryTopicKey(topic, sizeof topic, id, gMac, fieldSpec(FIELD_OUTLOOK).key) > 0) publish(topic, "", true);
  if (discoveryTopicKey(topic, sizeof topic, id, gMac, fieldSpec(FIELD_WIND_AVG10M).key) > 0) publish(topic, "", true);
  clearMeta(id);
}

// A station's notices, not retained: the sentences, then the attributes
static bool publishNotices(const char* id, const Notices& ns, const PosixTz& tz) {
  char topic[96], state[NOTICE_STATE_MAX + 4], attr[NOTICES_MAX * 64 + 16];
  // the day word only with a real clock; a day change reaches HA with the periodic re-send (mqtt_interval_last_seen_s)
  const Clock c = clockNow();
  if (noticesState(state, sizeof state, ns, tz, c.synced ? c.now : 0) < 0 || noticesAttributes(attr, sizeof attr, ns) < 0)
    return true;
  bool ok = stateTopic(topic, sizeof topic, gMac, id, FIELD_NOTICES) > 0 && publish(topic, state, false);
  ok = ok && entitySubTopic(topic, sizeof topic, gMac, id, "notices", "attr") > 0 && publish(topic, attr, false);
  return ok;
}

// The station's local outlook, not retained: the English sentence, then the code attributes
static bool publishOutlook(const char* id, const Outlook& x) {
  char topic[96], state[256], attr[256];
  if (!x.present || outlookState(state, sizeof state, x) < 0 || outlookAttributes(attr, sizeof attr, x) < 0) return true;
  bool ok = stateTopic(topic, sizeof topic, gMac, id, FIELD_OUTLOOK) > 0 && publish(topic, state, false);
  ok = ok && entitySubTopic(topic, sizeof topic, gMac, id, "outlook", "attr") > 0 && publish(topic, attr, false);
  return ok;
}

static void publishEntity(const PubView& v, const EntitySpec& e) {
  static char topic[128], payload[1152];
  if (discoveryTopicKey(topic, sizeof topic, v.id, gMac, e.key, e.component) < 0) return;
  if (discoveryPayloadSpec(payload, sizeof payload, v.id, v.name, gMac, e, SW_VERSION, gConfigUrl) < 0) {
    logf("MQTT", "discovery payload too long for %s", topic);
    return;
  }
  publish(topic, payload, true);
}

static void removeEntity(const char* id, const char* key) {
  char topic[128];
  if (discoveryTopicKey(topic, sizeof topic, id, gMac, key) > 0) publish(topic, "", true);  // empty = delete
}
static void removeEntity(const PubView& v, const char* key) { removeEntity(v.id, key); }
// A field's entity under its own component (battery_low is a binary_sensor; its older "sensor" twin goes too)
static void removeField(const char* id, int f) {
  const EntitySpec e = fieldSpec(f);
  char topic[128];
  if (e.component && discoveryTopicKey(topic, sizeof topic, id, gMac, e.key, e.component) > 0) publish(topic, "", true);
  removeEntity(id, e.key);
}

// ---------------------------------------------------------------- announced ids (ghost stations in HA)
// NVS fc_ann "ids": every station id that got discovery. Written only when an id is added or forgotten; loop task.
static const char* ANN_NS = "fc_ann";
static AnnList gAnn;
static bool gAnnLoaded = false;

static void annLoad() {
  if (gAnnLoaded) return;
  gAnnLoaded = true;
  Preferences p;
  if (!p.begin(ANN_NS, true)) return;  // read-only: absent until the first announcement
  char s[AnnList::MAX * ANN_ID_LEN + 8] = "";
  p.getString("ids", s, sizeof s);
  p.end();
  gAnn.parse(s);
}

static void annSave() {
  char s[AnnList::MAX * ANN_ID_LEN + 8];
  if (gAnn.format(s, sizeof s) < 0) return;
  Preferences p;
  if (!p.begin(ANN_NS, false)) return;
  p.putString("ids", s);
  p.end();
}

static void annRemember(const char* id) {
  annLoad();
  if (gAnn.add(id)) annSave();
}

static void annForget(const char* id) {
  annLoad();
  if (gAnn.remove(id)) annSave();
  char k[16];
  annPromoKey(id, k);
  Preferences p;
  if (p.begin(ANN_NS, false)) {
    if (p.isKey(k)) p.remove(k);
    p.end();
  }
}

// The promoted entities announced for an id (written only when the set changes); a promotion removed in admin
// loses its HA entity here (the announced set minus the current one)
static void annRememberPromos(const PubView& v) {
  char want[ANN_PROMO_MAX * ANN_PROMO_LEN] = "", have[sizeof want] = "", k[16];
  for (int i = 0, n = 0; i < MAX_PROMOTE; i++)
    if (v.promote[i].used) n += snprintf(want + n, sizeof want - n, "%s%s", n ? "," : "", v.promote[i].key);
  annPromoKey(v.id, k);
  Preferences p;
  if (!p.begin(ANN_NS, false)) return;
  p.getString(k, have, sizeof have);
  if (strcmp(want, have) != 0) {
    if (want[0]) p.putString(k, want);
    else if (p.isKey(k)) p.remove(k);
  }
  p.end();
  char gone[ANN_PROMO_MAX][ANN_PROMO_LEN];
  const int ng = annPromoGone(have, want, gone, ANN_PROMO_MAX);
  for (int i = 0; i < ng; i++) {
    removeEntity(v, gone[i]);
    logf("MQTT", "%s: promotion %s removed: its HA entity was retracted", v.id, gone[i]);
  }
}

void annWipe() {
  Preferences p;
  if (p.begin(ANN_NS, false)) {
    p.clear();
    p.end();
  }
  gAnn = AnnList();
}

// Promotions dropped at config load (their key became a built-in field or a protocol flag): their HA entities
// go on the first connect, like the ghosts below. Loop task only (config load runs there too).
static struct {
  char id[STATION_ID_LEN];
  char key[24];
} gPromoRetract[8];
static int gPromoRetractN = 0;
void queuePromoRetract(const char* station_id, const char* key) {
  if (gPromoRetractN >= 8) return;
  snprintf(gPromoRetract[gPromoRetractN].id, STATION_ID_LEN, "%s", station_id);
  snprintf(gPromoRetract[gPromoRetractN].key, sizeof gPromoRetract[0].key, "%s", key);
  gPromoRetractN++;
}
static void retractStalePromotions() {
  for (int i = 0; i < gPromoRetractN; i++) {
    removeEntity(gPromoRetract[i].id, gPromoRetract[i].key);
    logf("MQTT", "%s: promotion %s retracted from HA (its key is now built in or ignored)", gPromoRetract[i].id,
         gPromoRetract[i].key);
  }
  gPromoRetractN = 0;
}

// Once per boot, on the first MQTT connect: ids announced before but no longer stations (RAM-only stations are gone
// with the reboot; stored ones are loaded before MQTT starts) lose their HA device, as forget does. A RAM-only
// station that uploaded again before the connect is a station again and stays.
// The ghosts are queued here and retracted item by item by announceTick(); a forgotten
// station's retraction goes the same way, so a pause or a disconnect delays it instead of losing it.
static char gGhosts[AnnList::MAX][ANN_ID_LEN];
static int gGhostN = 0, gGhostAt = 0, gGhostStep = 0;
static bool queueRetract(const char* id) {
  if (gGhostAt >= gGhostN) gGhostN = gGhostAt = gGhostStep = 0;  // drained: start over
  for (int i = gGhostAt; i < gGhostN; i++)
    if (strcmp(gGhosts[i], id) == 0) return true;
  if (gGhostN >= AnnList::MAX) return false;
  snprintf(gGhosts[gGhostN++], ANN_ID_LEN, "%s", id);
  return true;
}
static void retractGhosts() {
  annLoad();
  char ids[AnnList::MAX][ANN_ID_LEN];
  int n = 0;
  xSemaphoreTake(gLock, portMAX_DELAY);
  for (int i = 0; i < gAnn.size(); i++)
    if (isGhost(gAnn.at(i), gStations, gParked)) snprintf(ids[n++], ANN_ID_LEN, "%s", gAnn.at(i));  // not a parked one
  xSemaphoreGive(gLock);
  for (int i = 0; i < n; i++) queueRetract(ids[i]);
}
// One item of a ghost's retraction: the fields, last_seen, the rule counters, its promotions, then its availability
// cleared.
static bool ghostItem(const char* id, int i) {
  if (i < F_COUNT) {
    removeField(id, i);
    return true;
  }
  i -= F_COUNT;
  if (i == 0) {
    removeEntity(id, "last_seen");
    removeExtras(id);
    return true;
  }
  i--;
  if (i < R_COUNT) {
    char k[24];
    snprintf(k, sizeof k, "rej_%s", ruleName(i));
    removeEntity(id, k);
    return true;
  }
  i -= R_COUNT;
  if (i == 0) {  // and the promotions it had (their object ids are the promotion keys)
    char pk[16], list[ANN_PROMO_MAX * ANN_PROMO_LEN] = "", keys[ANN_PROMO_MAX][ANN_PROMO_LEN];
    annPromoKey(id, pk);
    Preferences p;
    if (p.begin(ANN_NS, false)) {
      p.getString(pk, list, sizeof list);
      if (p.isKey(pk)) p.remove(pk);
      p.end();
    }
    const int np = annPromoParse(list, keys, ANN_PROMO_MAX);
    for (int k = 0; k < np; k++) removeEntity(id, keys[k]);
    clearAvailability(id);
    gAnn.remove(id);
    annSave();
    logf("MQTT", "station %s is no longer here (not stored): its HA device was retracted (%d promotion(s))", id, np);
    return true;
  }
  return false;
}

// Station topics live under fauxcloud/<mac4>/<id>/...; older firmware used fauxcloud/<id>/. Once per bridge (flag
// "topics2" in fc_sys), on the first connect: an empty retained message on the legacy fauxcloud/<id>/availability of every
// station stored or ever announced, so no retained "online" lingers there. Discovery (re-announced on every connect)
// moves HA to the new topics; unique_id is unchanged, so HA keeps the entities and their history.
static bool gLegacyDone = false;  // RAM copy of the fc_sys flag (read once)
static void retireLegacyTopics() {
  static bool checked = false;
  if (gLegacyDone) return;
  if (!checked) {
    checked = true;
    gLegacyDone = sysFlag("topics2");
    if (gLegacyDone) return;
  }
  annLoad();
  char ids[MAX_STATIONS + AnnList::MAX][STATION_ID_LEN];
  int n = 0;
  xSemaphoreTake(gLock, portMAX_DELAY);
  for (int i = 0; i < MAX_STATIONS; i++)
    if (const Station* st = gStations.at(i)) memcpy(ids[n++], st->id, STATION_ID_LEN);
  xSemaphoreGive(gLock);
  for (int i = 0; i < gAnn.size(); i++) {
    bool dup = false;
    for (int k = 0; k < n && !dup; k++) dup = strcmp(ids[k], gAnn.at(i)) == 0;
    if (!dup) snprintf(ids[n++], STATION_ID_LEN, "%s", gAnn.at(i));
  }
  int ok = 0;
  for (int i = 0; i < n; i++) {
    char t[64];
    if (legacyAvailabilityTopic(t, sizeof t, ids[i]) > 0 && publish(t, "", true)) ok++;
  }
  if (ok == n) {  // a failed publish tries again next second
    sysSetFlag("topics2");
    gLegacyDone = true;
  }
  logf("MQTT", "station topics now under fauxcloud/%s/: old retained availability cleared for %d of %d id(s)", gMac4, ok, n);
}

// Core fields + last_seen + per-rule counters always; extended fields and promotions once seen; disabled fields
// are removed from HA. The device name comes from the station config.
// One numbered item at a time (paced by announceTick): 0 the announced lists in NVS, then the fields,
// last_seen, the retired battery "sensor", the outdoor sensor link, the notices, the outlook, the 10-min mean wind,
// the rule counters, the promotions. false = past the last item.
static bool discoveryItem(const PubView& v, int i) {
  if (i == 0) {
    annRemember(v.id);
    annRememberPromos(v);
    gRainAvail[v.slot] = -2;  // the retained meta goes again with the next state
    memset(gAttrSig[v.slot], 0, sizeof gAttrSig[v.slot]);
    gW10Sig[v.slot] = 0;
    gNoticeSig[v.slot] = 0;
    gOutSig[v.slot] = gOutUp[v.slot] = gOutClose[v.slot] = 0;
    return true;
  }
  i--;
  if (i < F_COUNT) {
    const int f = i;
    if (f >= CORE_FIELDS && !((v.seen_fields >> f) & 1u)) return true;
    EntitySpec e = fieldSpec(f);
    if (f == F_RAIN_MM) {  // derived for a rate profile; a profile change re-announces and clears
      e.own_availability = e.attributes = v.rain_rate;
      if (!v.rain_rate) {
        char t[80];
        if (entitySubTopic(t, sizeof t, gMac, v.id, e.key, "availability") > 0) publish(t, "", true);
        if (entitySubTopic(t, sizeof t, gMac, v.id, e.key, "attr") > 0) publish(t, "", true);
      }
    }
    if (f == F_PRESSURE_HPA) e.attributes = v.baro_factory;  // {"calibrated":...}
    for (const DerivedDef& d : WIND_DERIVED)
      if (d.field == f) e.attributes = true;  // {"source":"derived",...} when derived
    if ((v.disabled >> f) & 1u) removeField(v.id, f);
    else publishEntity(v, e);
    return true;
  }
  i -= F_COUNT;
  if (i == 0) {
    publishEntity(v, fieldSpec(FIELD_LAST_SEEN));
    return true;
  }
  if (i == 1) {
    removeEntity(v, fieldSpec(F_BATTERY_LOW).key);  // the retired battery "sensor" (now a binary_sensor): no ghost
    return true;
  }
  if (i == 2) {
    publishEntity(v, fieldSpec(FIELD_SENSOR_LINK));  // part of the constant set from the first announce
    return true;
  }
  if (i == 3) {
    publishEntity(v, fieldSpec(FIELD_NOTICES));
    return true;
  }
  if (i == 4) {
    publishEntity(v, fieldSpec(FIELD_OUTLOOK));  // part of the constant set from the first announce
    return true;
  }
  if (i == 5) {  // once the station sent wind; retracted while wind_kmh is hidden
    if ((v.disabled >> F_WIND_KMH) & 1u) {
      char t[80];
      removeEntity(v, fieldSpec(FIELD_WIND_AVG10M).key);
      if (entitySubTopic(t, sizeof t, gMac, v.id, fieldSpec(FIELD_WIND_AVG10M).key, "attr") > 0) publish(t, "", true);
      gW10Sig[v.slot] = 0;
    } else if ((v.seen_fields >> F_WIND_KMH) & 1u) publishEntity(v, fieldSpec(FIELD_WIND_AVG10M));
    return true;
  }
  i -= 6;
  if (i < R_COUNT) {
    char k[24], l[40];
    publishEntity(v, ruleSpec(i, k, sizeof k, l, sizeof l));
    return true;
  }
  i -= R_COUNT;
  if (i < MAX_PROMOTE) {
    if (v.promote[i].used) publishEntity(v, promoteSpec(v.promote[i]));  // on save; HA shows unknown until a value
    return true;
  }
  return false;
}

// Measurements are not retained: a rejected field is simply not updated.
static void publishState(const PubView& v) {
  char topic[64], val[32];
  publishMeta(v);
  // the outdoor sensor link with every upload, "1" = not updating
  if (stateTopic(topic, sizeof topic, gMac, v.id, FIELD_SENSOR_LINK) > 0) publish(topic, v.sensor_stale ? "1" : "0", false);
  for (int f = 0; f < F_COUNT; f++) {
    if (!v.live.has(f) || ((v.disabled >> f) & 1u)) continue;  // hidden: recorded, not sent
    if (stateTopic(topic, sizeof topic, gMac, v.id, f) < 0 || formatState(val, sizeof val, f, v.live.v[f]) < 0) continue;
    publish(topic, val, false);
  }
  // the 10-min mean wind with its attributes (retained, on change)
  if (v.w10_fresh && !((v.disabled >> F_WIND_KMH) & 1u) && stateTopic(topic, sizeof topic, gMac, v.id, FIELD_WIND_AVG10M) > 0 &&
      formatValue(val, sizeof val, FIELD_WIND_AVG10M, v.w10) > 0) {
    publish(topic, val, false);
    char json[64], t[80];
    snprintf(json, sizeof json, "{\"source\":\"derived\",\"n\":%u,\"window_s\":600}", (unsigned)v.w10_n);
    const uint32_t sig = fnv1a32(json) | 1u;
    if (sig != gW10Sig[v.slot] && entitySubTopic(t, sizeof t, gMac, v.id, fieldSpec(FIELD_WIND_AVG10M).key, "attr") > 0 &&
        publish(t, json, true))
      gW10Sig[v.slot] = sig;
  }
  for (int i = 0; i < MAX_PROMOTE; i++) {
    if (!v.promote[i].used || !((v.promoted_mask >> i) & 1u) || !std::isfinite(v.promoted[i])) continue;
    snprintf(val, sizeof val, "%.*f", (int)promoType(v.promote[i].type).dec, (double)v.promoted[i]);
    if (stateTopicKey(topic, sizeof topic, gMac, v.id, v.promote[i].key) > 0) publish(topic, val, false);
  }
}

static void publishRejCounters(const PubView& v, bool force) {
  for (int r = 0; r < R_COUNT; r++) {
    if (!force && gRejPubValid[v.slot] && gRejPub[v.slot][r] == v.rej_rule[r]) continue;
    char k[24], topic[64], val[16];
    snprintf(k, sizeof k, "rej_%s", ruleName(r));
    snprintf(val, sizeof val, "%lu", (unsigned long)v.rej_rule[r]);
    if (stateTopicKey(topic, sizeof topic, gMac, v.id, k) > 0 && publish(topic, val, false)) gRejPub[v.slot][r] = v.rej_rule[r];
  }
  gRejPubValid[v.slot] = true;
}

static void publishLastSeen(const PubView& v, bool force, uint32_t interval_s) {
  const uint32_t ms = millis();
  uint32_t& last = gLastSeenPubMs[v.slot];
  if (!force && last && ms - last < interval_s * 1000UL) return;
  if (v.last_seen < EPOCH_MIN) return;  // seconds since boot before SNTP, never "1970-01-01" in HA
  char topic[64], val[32];
  if (stateTopic(topic, sizeof topic, gMac, v.id, FIELD_LAST_SEEN) > 0 && formatIsoUtc(val, sizeof val, v.last_seen) > 0 &&
      publish(topic, val, false))
    last = ms ? ms : 1;
}

// Forget: every entity this station announced gets an empty retained config (HA drops it), the
// station goes offline, its NVS config is deleted and the slot is freed for reuse (the allocation stays).
static void snapshot(int slot, const Station& st, uint32_t now_ms, PubView* v);

static void forgetStation(int slot) {
  PubView& v = gView;
  xSemaphoreTake(gLock, portMAX_DELAY);
  Station* st = gStations.at(slot);
  if (!st) {
    xSemaphoreGive(gLock);
    return;
  }
  snapshot(slot, *st, millis(), &v);
  xSemaphoreGive(gLock);
  // The HA entities go through the paced retraction (fields, last_seen, rules, the announced
  // promotions from NVS, the availability cleared, the announced list), retried after a pause or a reconnect. It reads the announced
  // promotions, so annForget runs only when the queue is full and the retraction happens here and now.
  annLoad();
  const bool queued = queueRetract(v.id);
  if (!queued) {
    for (int f = 0; f < F_COUNT; f++) removeField(v.id, f);
    removeEntity(v, "last_seen");
    removeExtras(v.id);
    for (int r = 0; r < R_COUNT; r++) {
      char k[24];
      snprintf(k, sizeof k, "rej_%s", ruleName(r));
      removeEntity(v, k);
    }
    for (int i = 0; i < MAX_PROMOTE; i++)
      if (v.promote[i].used) removeEntity(v, v.promote[i].key);
    clearAvailability(v.id);
    annForget(v.id);
  }
  deleteStationConfig(v.id);
  storeForget(v.id);
  consoleForget(v.id);
  xSemaphoreTake(gLock, portMAX_DELAY);
  gStations.forget(slot);
  gNoSlot1.clear();  // a slot is free: the turned-away console gets it at its next upload
  const bool restart = forgetNeedsRestart(gParked);
  xSemaphoreGive(gLock);
  if (restart) {  // the parked station is admitted by the boot loader, with its stored config and key
    logf("HIST", "a parked station waits for the free slot: restarting to load its saved settings");
    sysPlanRestart("forget_parked", 3000);
  }
  gStaleSent[slot] = gOffPublished[slot] = gRejPubValid[slot] = false;
  gLastSeenPubMs[slot] = 0;
  gStateDueMs[slot] = 0;
  gAnnStep[slot] = 0;
  gCfgChanged[slot] = false;
  gQuietOff[slot] = false;
  logf("HIST", "station %s forgotten: NVS config deleted, HA entities %s, slot %d free for reuse", v.id,
       queued ? "queued for removal" : "removed now (retraction queue full)", slot);
}

// ---------------------------------------------------------------- the bridge device
// Diagnostics only, never retained: IP, firmware and last restart on connect and on change; RSSI,
// AP clients and heap every 5 minutes. Availability is the bridge LWT.
static void publishBridgeValue(int entity, const char* val) {
  char topic[64];
  if (bridgeStateTopic(topic, sizeof topic, gMac, bridgeSpec(entity).key) > 0) publish(topic, val, false);
}

// One entity per item (paced by announceTick), then the retired entity's removal.
static bool bridgeDiscoveryItem(int i) {
  static char topic[128], payload[768];
  if (i == B_COUNT) {
    if (bridgeDiscoveryTopic(topic, sizeof topic, gMac, BRIDGE_RETIRED_KEY) > 0) publish(topic, "", true);
    return true;
  }
  if (i > B_COUNT) return false;
  const EntitySpec e = bridgeSpec(i);
#if FC_HA_WUFC
  if (i == B_WU_TOMORROW && !wufcOn()) {  // retracted while the forecast switch is off
    if (bridgeDiscoveryTopic(topic, sizeof topic, gMac, e.key) > 0) publish(topic, "", true);
    return true;
  }
#endif
  if (bridgeDiscoveryTopic(topic, sizeof topic, gMac, e.key) < 0 ||
      bridgeDiscoveryPayload(payload, sizeof payload, gMac, e, SW_VERSION, gConfigUrl) < 0) {
    logf("MQTT", "bridge discovery payload too long for %s", e.key);
    return true;
  }
  publish(topic, payload, true);
  return true;
}
static int gBridgeStep = 0;  // 0 idle, else item + 1
// The bridge's notices now (clock, memory, history writes)
static Notices bridgeNoticesNow() {
  BridgeNoticeIn in = {};
  in.time_valid = timeSynced();
  in.time_approx = timeSrc() == TimeSrc::Persisted;
  in.clock_held = clockHeldNow();
  in.heap_low = heapLow(nullptr);
  in.store_fail = storeFails();
  in.frag_restart = strcmp(lastReset(), "heap_frag") == 0 && uptimeS() < 86400;
  in.store_layout = storeLayoutBad();
  return bridgeNotices(in);
}
static uint32_t gBridgeNoticeSig = 0;  // 0 = not sent since the connect
static void publishBridgeNotices() {
  static PosixTz utc;  // the bridge's texts carry no HH:MM
  static bool init = false;
  if (!init) init = parsePosixTz("UTC0", &utc);
  const Notices ns = bridgeNoticesNow();
  char topic[80], state[NOTICE_STATE_MAX + 4], attr[NOTICES_MAX * 64 + 16];
  if (noticesState(state, sizeof state, ns, utc, 0) < 0 || noticesAttributes(attr, sizeof attr, ns) < 0) return;
  if (bridgeStateTopic(topic, sizeof topic, gMac, "notices") <= 0 || !publish(topic, state, false)) return;
  if (bridgeStateTopic(topic, sizeof topic, gMac, "notices/attr") > 0 && publish(topic, attr, false))
    gBridgeNoticeSig = noticesSig(ns) | 1u;
}
static const char* gTimeSrcSent = nullptr;  // the "Time source" value last published (on connect + on change)
#if FC_HA_WUFC
// The WU sensor -- its discovery follows the switch, its state + attributes go on change and with the 5-min cadence
static int8_t gWuOnSent = -1;   // the switch as last announced (-1 = not since the connect)
static uint32_t gWuSigSent = 0;
static void publishWufc(bool force) {
  char state[WUFC_PHRASE_MAX + 8], attr[160], topic[80];
  const uint32_t sig = wufcHa(state, sizeof state, attr, sizeof attr);
  if (!sig || (!force && sig == gWuSigSent)) return;
  if (bridgeStateTopic(topic, sizeof topic, gMac, "wu_tomorrow") <= 0 || !publish(topic, state, false)) return;
  if (bridgeStateTopic(topic, sizeof topic, gMac, "wu_tomorrow/attr") > 0 && publish(topic, attr, false)) gWuSigSent = sig;
}
static void wufcMqttTick(bool connectAnnounce) {
  const bool on = wufcOn();
  if (connectAnnounce) gWuOnSent = (int8_t)on, gWuSigSent = 0;  // the paced discovery covers the entity on a connect
  else if (gWuOnSent != (int8_t)on) {
    gWuOnSent = (int8_t)on;
    bridgeDiscoveryItem(B_WU_TOMORROW);
    gWuSigSent = 0;
  }
  if (on) publishWufc(false);
}
#else
static void publishWufc(bool) {}  // FC_HA_WUFC=0: no HA forecast sensor
static void wufcMqttTick(bool) {}
#endif

static void publishBridgePeriodic() {
  char val[24];
  snprintf(val, sizeof val, "%d", staUp() ? (int)WiFi.RSSI() : 0);
  if (staUp()) publishBridgeValue(B_RSSI, val);
  snprintf(val, sizeof val, "%d", (int)WiFi.softAPgetStationNum());
  publishBridgeValue(B_AP_CLIENTS, val);
  snprintf(val, sizeof val, "%lu", (unsigned long)heapNow().free8);  // the byte-addressable heap
  publishBridgeValue(B_HEAP, val);
  publishBridgeNotices();  // with the 5-min cadence too
  publishWufc(true);       // the forecast sensor too (not retained)
}

// The boot time, once the clock is synced: on connect / re-announce, and once when SNTP first syncs.
static bool publishLastRestart() {
  if (!timeSynced()) return false;
  char val[24];
  if (formatIsoUtc(val, sizeof val, (int64_t)time(nullptr) - (int64_t)uptimeS()) < 0) return false;
  publishBridgeValue(B_LAST_RESTART, val);
  return true;
}

// loop(), every second: config URL changes re-announce everything (station devices carry it too).
static void bridgeTick(bool connectAnnounce) {
  static uint32_t lastPeriodicMs = 0;
  static bool restartSent = false;
  char ip[16] = "";
  if (staUp()) staIpStr(ip);
  char url[sizeof gConfigUrl];
  if (ip[0]) snprintf(url, sizeof url, "http://%s/", ip);
  else url[0] = 0;
  const bool urlChanged = strcmp(url, gConfigUrl) != 0;
  if (urlChanged) {
    memcpy(gConfigUrl, url, sizeof gConfigUrl);
    if (gMqttConnected && !connectAnnounce) gNeedAnnounce = true;  // next second: discovery with the new URL
  }
  if (!gMqttConnected) return;
  const uint32_t ms = millis();
  if (connectAnnounce) {
    gBridgeStep = 1;  // the discovery is paced (announceTick); the values are small and go now
    publishBridgeValue(B_FIRMWARE, SW_VERSION);
    publishBridgeValue(B_LAST_RESET, lastReset());
    char n[12];
    snprintf(n, sizeof n, "%lu", (unsigned long)restarts());
    publishBridgeValue(B_RESTARTS, n);
    restartSent = publishLastRestart();
    gTimeSrcSent = timeSrcName(timeSrc());
    publishBridgeValue(B_TIME_SRC, gTimeSrcSent);
    if (ip[0]) publishBridgeValue(B_STA_IP, ip);
    wufcMqttTick(true);  // before the periodic values (they carry the sensor's state)
    publishBridgePeriodic();
    lastPeriodicMs = ms ? ms : 1;
    return;
  }
  if (!restartSent) restartSent = publishLastRestart();
  if ((noticesSig(bridgeNoticesNow()) | 1u) != gBridgeNoticeSig) publishBridgeNotices();  // on change
  wufcMqttTick(false);
  const char* ts = timeSrcName(timeSrc());  // a literal: compared by address
  if (ts != gTimeSrcSent) {
    gTimeSrcSent = ts;
    publishBridgeValue(B_TIME_SRC, ts);
  }
  if (!lastPeriodicMs || ms - lastPeriodicMs >= 300000UL) {
    publishBridgePeriodic();
    lastPeriodicMs = ms ? ms : 1;
  }
}

// The paced announce. Per station slot the next discovery item (0 = idle, else item + 1); the
// availability and the deferred first state follow its last item. Ghost retractions and the bridge's own
// discovery go the same way. announceTick() runs every loop() pass. (gAnnStep: with the other per-slot state.)
static void startAnnounce(int slot) { gAnnStep[slot] = 1; }

static void announce(const PubView& v) {
  if (!v.publish) {
    // A disabled station is visibly off (availability), not frozen: offline once, no discovery, no state.
    if (!gOffPublished[v.slot] && publishAvailability(v.id, false)) {  // until it went out
      gOffPublished[v.slot] = true;
      logf("MQTT", "%s has publish:false -> availability offline", v.id);
    }
    return;
  }
  gOffPublished[v.slot] = false;
  startAnnounce(v.slot);  // discovery, then availability; state, counters and last_seen follow from everySecond()
}

// Caller holds gLock.
static void snapshot(int slot, const Station& st, uint32_t now_ms, PubView* v) {
  v->slot = slot;
  memcpy(v->id, st.id, sizeof v->id);
  st.displayName(v->name, sizeof v->name);
  v->publish = st.cfg.publish;
  v->seen_fields = st.seen_fields;
  v->disabled = st.cfg.disabled;
  v->last_seen = st.last_seen;
  v->online = st.online(now_ms);
  v->has_live = st.uploads > 0;
  // st.live carries every field's last value; HA gets only what the latest upload sent (its history unpadded)
  v->live = st.live;
  v->live.present &= st.fresh;
  v->promoted_mask = st.promo_fresh;
  memcpy(v->promoted, st.promoted, sizeof v->promoted);
  memcpy(v->promote, st.cfg.promote, sizeof v->promote);
  memcpy(v->rej_rule, st.rej_rule, sizeof v->rej_rule);
  v->rain_rate = profileRules(st.cfg.profile).rainin_is_rate;
  v->rain_warm = st.rain_warm;
  v->rain_hours = st.rain_hours;
  v->rain_n = st.rain_n;
  v->derived = st.derived;
  memcpy(v->derived_n, st.derived_n, sizeof v->derived_n);
  v->baro_factory = !std::isnan(profileRules(st.cfg.profile).baro_factory_hpa);
  v->baro_uncal = st.baro_uncal;
  v->sensor_stale = st.sensor_stale;
  v->w10_fresh = st.wind10_fresh, v->w10 = st.wind10, v->w10_n = st.wind10_n;
}

// ---------------------------------------------------------------- MQTT test (admin "Test" button)
static esp_mqtt_client_handle_t gTest = nullptr;
static volatile int gTestResult = 0;  // 0 running, 1 connected, -1 error
static char gTestDetail[80] = "";
static char gTestState[16] = "idle";
static uint32_t gTestStartMs = 0;
static volatile bool gTestReq = false;
static char gTestHost[64], gTestUser[64], gTestPass[64];
static uint16_t gTestPort = 1883;
static portMUX_TYPE gTestMux = portMUX_INITIALIZER_UNLOCKED;

static void testEvent(void*, esp_event_base_t, int32_t id, void* data) {
  auto* e = (esp_mqtt_event_handle_t)data;
  if (id == MQTT_EVENT_CONNECTED) {
    gTestResult = 1;
  } else if (id == MQTT_EVENT_ERROR && gTestResult == 0) {
    portENTER_CRITICAL(&gTestMux);
    if (e->error_handle && e->error_handle->error_type == MQTT_ERROR_TYPE_CONNECTION_REFUSED)
      snprintf(gTestDetail, sizeof gTestDetail, "refused by the broker (code %d: bad user/password?)",
               (int)e->error_handle->connect_return_code);
    else
      snprintf(gTestDetail, sizeof gTestDetail, "cannot connect (socket errno %d)",
               e->error_handle ? e->error_handle->esp_transport_sock_errno : 0);
    portEXIT_CRITICAL(&gTestMux);
    gTestResult = -1;
  }
}

void mqttTestStart(const char* host, uint16_t port, const char* user, const char* pass) {
  snprintf(gTestHost, sizeof gTestHost, "%s", host);
  snprintf(gTestUser, sizeof gTestUser, "%s", user);
  snprintf(gTestPass, sizeof gTestPass, "%s", pass);
  gTestPort = port;
  portENTER_CRITICAL(&gTestMux);
  snprintf(gTestState, sizeof gTestState, "running");
  gTestDetail[0] = 0;
  portEXIT_CRITICAL(&gTestMux);
  gTestReq = true;
}

void mqttTestJson(Out& o) {
  char st[16], detail[80];
  portENTER_CRITICAL(&gTestMux);
  snprintf(st, sizeof st, "%s", gTestState);
  snprintf(detail, sizeof detail, "%s", gTestDetail);
  portEXIT_CRITICAL(&gTestMux);
  o.raw("{\"state\":");
  o.jsonStr(st);
  o.raw(",\"detail\":");
  o.jsonStr(detail);
  o.raw("}");
}

static void testFinish(const char* state, const char* detail) {
  if (gTest) {
    esp_mqtt_client_stop(gTest);
    esp_mqtt_client_destroy(gTest);
    gTest = nullptr;
  }
  portENTER_CRITICAL(&gTestMux);
  snprintf(gTestState, sizeof gTestState, "%s", state);
  if (detail) snprintf(gTestDetail, sizeof gTestDetail, "%s", detail);
  portEXIT_CRITICAL(&gTestMux);
  memset(gTestPass, 0, sizeof gTestPass);
  logf("MQTT", "test of %s:%u -> %s", gTestHost, (unsigned)gTestPort, state);
}

static void testTick() {
  if (gTestReq) {
    gTestReq = false;
    if (gTest) testFinish("failed", "superseded");
    if (!gTestHost[0]) {
      testFinish("failed", "no broker host");
      return;
    }
    if (!WiFi.isConnected()) {
      testFinish("failed", "fauxcloud is not on the home network");
      return;
    }
    esp_mqtt_client_config_t cfg = {};
    cfg.broker.address.hostname = gTestHost;
    cfg.broker.address.port = gTestPort;
    cfg.broker.address.transport = MQTT_TRANSPORT_OVER_TCP;
    static char cid[32];
    snprintf(cid, sizeof cid, "fauxcloud-test-%02x%02x", gMac[4], gMac[5]);
    cfg.credentials.client_id = cid;
    if (gTestUser[0]) cfg.credentials.username = gTestUser;
    if (gTestPass[0]) cfg.credentials.authentication.password = gTestPass;
    cfg.network.disable_auto_reconnect = true;
    gTestResult = 0;
    gTest = esp_mqtt_client_init(&cfg);
    esp_mqtt_client_register_event(gTest, MQTT_EVENT_ANY, testEvent, nullptr);
    esp_mqtt_client_start(gTest);
    gTestStartMs = millis();
    return;
  }
  if (!gTest) return;
  if (gTestResult == 1) testFinish("ok", "");
  else if (gTestResult == -1) testFinish("failed", nullptr);
  else if (millis() - gTestStartMs > 8000) testFinish("failed", "no answer within 8 s");
}

// Every loop() pass, a few discovery items while publishing is not paused: the bridge first, then the
// ghosts, then the stations. A disconnect leaves the cursors; the reconnect starts them again.
static void announceTick() {
  if (!gMqttConnected || otaActive()) return;
  int budget = 8;  // items per pass (skipped fields count too: a pass stays short)
  while (gBridgeStep && budget > 0 && paceOk()) {
    budget--;
    if (!bridgeDiscoveryItem(gBridgeStep - 1)) gBridgeStep = 0;
    else if (!stalled()) gBridgeStep++;  // a publish that hit a pause is tried again, never skipped
  }
  while (gGhostAt < gGhostN && budget > 0 && paceOk()) {
    budget--;
    if (ghostItem(gGhosts[gGhostAt], gGhostStep)) {
      if (!stalled()) gGhostStep++;
    } else {
      gGhostAt++;
      gGhostStep = 0;
    }
  }
  PubView& v = gView;
  for (int slot = 0; slot < MAX_STATIONS && budget > 0; slot++) {
    if (!gAnnStep[slot]) continue;
    xSemaphoreTake(gLock, portMAX_DELAY);
    Station* st = gStations.at(slot);
    if (st) snapshot(slot, *st, millis(), &v);
    xSemaphoreGive(gLock);
    if (!st) {
      gAnnStep[slot] = 0;
      continue;
    }
    while (budget > 0 && paceOk()) {
      budget--;
      if (discoveryItem(v, gAnnStep[slot] - 1)) {
        if (!stalled()) gAnnStep[slot]++;
        continue;
      }
      publishAvailability(v.id, v.online);  // done: availability now, the state ~2 s later (once HA subscribed)
      if (stalled()) break;                 // this last step again after the pause
      gAnnStep[slot] = 0;
      const uint32_t due = millis() + 2000;
      gStateDueMs[slot] = due ? due : 1;
      logf("MQTT", "discovery published for %s (\"%s\")", v.id, v.name);
      break;
    }
  }
}

void mqttLoop() {
  gPassPubMs = 0;  // a new loop() pass (mqttLoop runs first in it)
  // During a firmware upload the MQTT client is stopped and destroyed (its task stack, buffers and socket are
  // ~8 KB the write needs with 4 stations); it comes back through the normal restart if the upload is aborted (a good
  // upload restarts the board). Loop task only, like every other use of gMqtt.
  static bool otaPaused = false;
  if (otaActive() && gMqtt && !otaPaused) {
    otaPaused = true;
    mqttStop();
    logf("MQTT", "stopped for the firmware update (frees its task, buffers and socket)");
  } else if (!otaActive() && otaPaused) {
    otaPaused = false;
    gRestartReq = true;
  }
  if (gInitRetryMs && millis() - gInitRetryMs >= 30000) {
    gInitRetryMs = 0;
    if (!gMqtt) gRestartReq = true;
  }
  if (gStartReq && WiFi.isConnected() && !otaActive()) {  // a rejoin keeps a client it already has
    gStartReq = false;
    if (!gMqtt && !gInitRetryMs) gRestartReq = true;
  }
  if (gRestartReq && WiFi.isConnected() && !otaActive()) {
    gRestartReq = false;
    mqttStart();
  }
  testTick();
  announceTick();
}

// ---------------------------------------------------------------- per upload, per second, heartbeat
static uint32_t gLastIngestMs = 0;
uint32_t lastIngestMs() { return gLastIngestMs; }
bool mqttBusy() {  // discovery / retraction pending, or the publish stall (forwarding waits)
  if (gBridgeStep || gGhostAt < gGhostN || stalled()) return true;
  for (int i = 0; i < MAX_STATIONS; i++)
    if (gAnnStep[i]) return true;
  return false;
}

void ingestQueued() {
  static Upload u;
  PubView& v = gView;
  // Readings taken before SNTP carry seconds since boot; at the first sync they get their real time
  static bool restamped = false;
  // a held clock is "no real time yet": readings carry uptime again and are re-stamped at the release
  if (restampRearm(restamped, clockHeldNow())) {
    restamped = false;
    logf("HIST", "clock held: readings carry uptime until it is released");
  }
  if (!restamped && timeSynced()) {
    restamped = true;
    const uint32_t offset = (uint32_t)time(nullptr) - (uint32_t)(millis() / 1000);  // the clock nowS() used
    int n = 0;
    xSemaphoreTake(gLock, portMAX_DELAY);
    for (int i = 0; i < MAX_STATIONS; i++)
      if (Station* s = gStations.at(i)) n += Stations::clockSynced(*s, offset);
    xSemaphoreGive(gLock);
    if (n) logf("HIST", "clock synced: %d reading(s) taken before it now carry their real time", n);
    Stations::setBootTime(offset);  // the hour of this boot gets the `restart` quality bit
  }
  Stations::setClockApprox(timeSrc() == TimeSrc::Browser);  // hours stamped on a browser seed
  while (xQueueReceive(gQueue, &u, 0) == pdTRUE) {
    gLastIngestMs = millis() | 1;  // forwarding keeps a 3-s quiet gap after an upload
    const Clock clk = clockNow();  // one read per upload
    const uint32_t now = clk.now, ms = clk.ms;
    xSemaphoreTake(gLock, portMAX_DELAY);
    StationConfig initial = defaultStationConfig();
    initial.publish = gBridge.auto_publish_new_stations;  // new stations only; stored ones keep their config
    // a parked id never gets a slot here (it would start from defaults): turned away until the boot
    Station* st = admitStation(gStations, gParked, u.s.station_id, initial, gBridge.max_stations);
    // The order (slot, key, rate) is uploadVerdict's, host-tested; the key binds on the first
    // accepted upload (checkStationKey), another key drops the sample. Every refusal was answered `success`.
    // No key check (and no binding) when limited or a duplicate
    const bool dated = u.s.has_dateutc;  // a real dateutc, not "now" or none
    const bool limited = st && (rateLimited(*st, u.rx_ms) || isDuplicate(*st, u.rx_ms, u.qhash, dated));
    const KeyCheck kc = st && !limited ? checkStationKey(*st, u.key) : KeyCheck::Match;
    const UploadVerdict uv = uploadVerdict(st, kc, u.rx_ms, u.qhash, dated);
    if (uv == UploadVerdict::NoSlot) {
      const int maxSt = gBridge.max_stations, active = gStations.active();
      gNoSlot1.note(u.s.station_id, ms);
      {  // an upload from the same device as the loaded station -> the admin offers to adopt the new id (adopt.h)
        const Station* ld = nullptr;
        for (int i = 0; i < MAX_STATIONS && !ld; i++) ld = gStations.at(i);
        AdoptIn ai = {};
        ai.blocked = false;  // a blocked id never reaches the queue (onUpload drops it)
        ai.parked = gParked.has(u.s.station_id) || gLeftover.has(u.s.station_id);  // a refused one: no offer until Forget
        ai.max_stations = MAX_STATIONS;
        ai.loaded = ld != nullptr, ai.loaded_stored = ld && ld->stored;
        ai.up_has_mac = u.has_mac, ai.up_mac = u.mac;
        ai.st_has_mac = ld && ld->has_src_mac, ai.st_mac = ld ? ld->src_mac : nullptr;
        ai.persisted_mac_hash = ld && !ld->has_src_mac ? learnMacHash(ld->id) : 0;  // after a bridge reboot
        gNoSlot1.offer(adoptCandidate(ai), ld ? ld->id : "", u.key);
      }
      xSemaphoreGive(gLock);
      gNoSlot++;
      static uint32_t lastLogMs = 0;  // a foreign station uploading every minute must not flood the log
      if (!lastLogMs || ms - lastLogMs > 600000) {
        lastLogMs = ms;
        logf("HIST", "no slot for %s (%d of max_stations %d in use, free heap %u)", u.s.station_id, active, maxSt,
             (unsigned)heapNow().free8);
      }
      continue;
    }
    const int slot = gStations.indexOf(st);
    if (uv != UploadVerdict::Accept) {
      char sid[STATION_ID_LEN];
      memcpy(sid, st->id, sizeof sid);
      const uint32_t km = uv == UploadVerdict::KeyMismatch ? ++st->key_mismatch : 0;
      xSemaphoreGive(gLock);
      if (uv == UploadVerdict::Duplicate) {  // ring, history and MQTT untouched
        const uint32_t n = ++gDuplicate;
        if (n == 1 || n % 60 == 0)
          logf("HIST", "%s: the same upload again within %lu s dropped (%lu since boot; Weathercloud ticked?)", sid,
               (unsigned long)((u.s.has_dateutc ? DUP_WINDOW_MS : DUP_UNDATED_MS) / 1000), (unsigned long)n);
      } else if (uv == UploadVerdict::KeyMismatch) {
        gDropKey++;
        if (km == 1 || km % 60 == 0) logf("HIST", "%s: upload with another key dropped (%lu since restart)", sid, (unsigned long)km);
      } else {  // ring, history and MQTT untouched
        const uint32_t n = ++gRateLimited;
        if (n == 1 || n % 60 == 0)
          logf("HIST", "%s: upload less than %lu s after the previous one dropped (%lu since boot)", sid,
               (unsigned long)(RATE_MIN_MS / 1000), (unsigned long)n);
      }
      continue;
    }
    const bool bindNow = kc == KeyCheck::Bound;
    st->last_rx_ms = u.rx_ms;
    st->last_qhash = u.qhash;
    if (!st->uploads && !st->chmap.ver) {  // a station allocated now (not stored) gets its channel map back
      static ChMap maps[CH_VERSIONS];  // loop task only
      if (const int nm = loadChannelMaps(st->id, maps)) st->chmap = maps[nm - 1];
    }
    if (!st->cal.n) loadCalLog(*st);  // once per slot (NVS, under gLock like the maps above)
    storeRestore(*st);  // a new station's hours and days come back from flash before its first sample
    const bool wasOnline = st->online(ms);
    const bool first = st->uploads == 0;
    const bool toGeneric = applyFingerprint(*st, u.fp);  // before ingest, so the first sample has its meanings
    // A stored station keeps its profile (it may predate the fingerprint); say once when its uploads are not the 7-in-1's
    const bool fpWarn = st->stored && u.fp == Fingerprint::Mismatch && profileRules(st->cfg.profile).fingerprinted &&
                        !gFpWarned[slot];
    if (fpWarn) gFpWarned[slot] = true;
    gStaleSent[slot] = false;
    const uint32_t seenBefore = st->seen_fields;
    const bool staleBefore = st->sensor_stale;
    const uint8_t promoBefore = st->seen_promoted;
    const IngestResult res = Stations::ingest(*st, u.s, now, ms, u.src_ip, u.src_if, clk.synced, gTz);
    st->has_src_mac = u.has_mac;
    if (u.has_mac) memcpy(st->src_mac, u.mac, 6);
    // When the flag goes on, say whether the frozen set held wind (a lost link keeps the last non-zero wind; a
    // live calm sensor reports 0/0) -- data for the threshold, no rule change
    if (!staleBefore && st->sensor_stale) {
      const bool hw = (st->last_sensor_present >> F_WIND_KMH) & 1u, hg = (st->last_sensor_present >> F_GUST_KMH) & 1u;
      const float w = hw ? st->last_sensor[F_WIND_KMH] : 0, g = hg ? st->last_sensor[F_GUST_KMH] : 0;
      logf("HIST", "%s: outdoor sensor not updating (%u unchanged uploads%s; frozen wind %.1f / gust %.1f km/h: %s; stale #%u)",
           st->id, (unsigned)st->same_n, st->sensor_absent ? ", set absent" : "", (double)w, (double)g,
           st->sensor_absent ? "absent" : (w > 0 || g > 0) ? "non-zero, likely a lost link" : "zero, may be a calm sensor",
           (unsigned)st->stale_on_count);
    }
    const bool storeNow = storeOnUpload(st->stored, gBridge.auto_publish_new_stations);
    static ChMap chSave;  // a new channel map version goes to NVS ch:<id> after the lock
    const bool chNew = st->ch_dirty;
    if (chNew) {
      chSave = st->chmap;
      st->ch_dirty = false;
    }
    consoleApply(*st, u.line_tag, now);  // numbered by the upload counter ingest just advanced
    storeMarkDue(slot, res.hour_closed, res.day_closed);
    const bool newEntities = st->seen_fields != seenBefore || st->seen_promoted != promoBefore;
    snapshot(slot, *st, ms, &v);
    const uint32_t interval = gBridge.mqtt_interval_last_seen_s;
    const bool keepKey = bindNow && st->stored;
    uint8_t keyHash[32];
    if (keepKey) memcpy(keyHash, st->key_hash, sizeof keyHash);
    xSemaphoreGive(gLock);

    if (chNew) {
      saveChannelMap(v.id, chSave);
      logf("HIST", "%s: history channel map v%u: %s %s %s %s", v.id, (unsigned)chSave.ver, chSave.key[0], chSave.key[1],
           chSave.key[2], chSave.key[3]);
    }
    if (storeNow) {  // one NVS write; saveStation keeps a bound key too
      // a failed write leaves the station unstored, so the next upload tries again: that line is rate limited
      static RateLimitedLine notSaved;
      uint32_t sn, sm;
      char sx[32];
      if (saveStation(slot)) logf("HIST", "%s: stored on its first upload (auto-publish on)", v.id);
      else if (notSaved.hit(millis(), &sn, &sm))
        logf("HIST", "%s: stored on its first upload (auto-publish on) -- NOT saved, NVS error; retried on the next upload%s", v.id,
             rateSuffix(sx, sizeof sx, sn, sm));
    }
    if (bindNow) logf("HIST", "%s: console key bound%s", v.id, keepKey || storeNow ? " and saved" : " (kept in RAM until the station is saved)");
    if (keepKey) saveStationKey(v.id, keyHash);
    if (first) logf("HIST", "station %s registered (slot %d of %d reserved)", v.id, slot, gStations.allocated());
    if (fpWarn)
      logf("HIST", "%s: its uploads do not look like the 7-in-1's (fingerprint mismatch) but its stored profile is %s: "
                   "rainin is read as a rate; switch it to Generic WU in the station sheet if this is another console", v.id,
           profileRules(Profile::Yt60234).label);
    if (first) logf("HIST", "%s: fingerprint %s%s", v.id, fingerprintName(u.fp)[0] ? fingerprintName(u.fp) : "-",
                    toGeneric ? " -> profile Generic WU (not the 7-in-1's upload)" : "");
    if (res.dropped || res.rain_glitch) {  // rate limited; the rej counters keep the full count
      static RateLimitedLine filtLine[2];   // one per kind, so the count is of that kind
      uint32_t rn, rm;
      char sx[32];
      if (filtLine[res.dropped ? 0 : 1].hit(millis(), &rn, &rm))
        logf("FILT", "%s: %s%s", v.id, res.dropped ? "whole sample dropped (profile humidity rule)" : "daily rain counter glitch ignored",
             rateSuffix(sx, sizeof sx, rn, rm));
    }
    if (res.day_closed) logf("HIST", "day closed for %s", v.id);
    if (!gMqttConnected) continue;  // everything is announced again on (re)connect
    if (!v.publish) {
      announce(v);
      continue;
    }
    if (newEntities || first) {  // no device before its first upload
      gOffPublished[slot] = false;
      startAnnounce(slot);  // paced; availability and the state follow its last item
      continue;
    }
    if (gAnnStep[slot]) continue;  // an announce is under way: its end sends everything the station has
    if (!wasOnline) publishAvailability(v.id, true);
    gOffPublished[slot] = false;
    if (!res.dropped) publishState(v);
    publishRejCounters(v, first);
    publishLastSeen(v, first || !wasOnline, interval);
  }
}

void everySecond() {
  for (int i = 0; i < MAX_STATIONS; i++)
    if (gForgetReq[i]) {
      gForgetReq[i] = false;
      forgetStation(i);
    }
  if (gForgetFlashReq) {  // no slot, so no snapshot of its entities; an announced id is retracted by id
    char id[STATION_ID_LEN];
    memcpy(id, gForgetFlash, sizeof id);
    xSemaphoreTake(gLock, portMAX_DELAY);
    const bool hasSlot = gStations.find(id) != nullptr;
    xSemaphoreGive(gLock);
    if (!hasSlot) {
      annLoad();
      const bool queued = queueRetract(id);
      deleteStationConfig(id);  // config, key, channel maps, calibration log
      storeForget(id);
      consoleForget(id);
      xSemaphoreTake(gLock, portMAX_DELAY);
      const bool parked = gParked.remove(id);  // no longer listed as parked
      gLeftover.remove(id);
      xSemaphoreGive(gLock);
      logf("ADMIN", "%s: %s station forgotten%s", id, parked ? "parked" : "flash-only", queued ? " (HA retraction queued)" : "");
    }
    gForgetFlashReq = false;
  }
  adminLockTick();
  heapSample();
  {  // /api/bridge/status ETag: one counter for every visible change (station states, MQTT, STA, locks, heap)
    static BridgeStatus prev;
    static BridgeStatus cur;
    bridgeStatusNow(&cur);
    if (memcmp(&cur, &prev, sizeof cur) != 0) {
      prev = cur;
      gBridgeRev++;
    }
  }
  bool stale[MAX_STATIONS] = {};
  bool redo[MAX_STATIONS] = {};
  static Notices notes[MAX_STATIONS];  // due to publish (n = 0xFF: nothing due)
  bool noteDue[MAX_STATIONS] = {};
  static Outlook outs[MAX_STATIONS];  // due to publish
  bool outDue[MAX_STATIONS] = {};
  PosixTz tz;
  char ids[MAX_STATIONS][STATION_ID_LEN];
  const Clock clk = clockNow();  // one read per pass
  const uint32_t now = clk.now, ms = clk.ms;
  const bool connectAnnounce = gNeedAnnounce && gMqttConnected && !stalled();  // not into a pause

  xSemaphoreTake(gLock, portMAX_DELAY);
  // LearnUse.metar follows the METAR settings and the location every pass (the learners' snapshot is
  // hourly): the pressure chip switches between baro_ref and baro_check with the settings, never two at once
  const bool metarOk = metarEligibleLocked();
  for (int i = 0; i < MAX_STATIONS; i++) {
    Station* st = gStations.at(i);
    if (!st) continue;
    st->use.metar = metarOk;
    memcpy(ids[i], st->id, sizeof ids[i]);
    if (clk.synced) {
      storeRestore(*st);  // rings and snapshot entry (a store that became ready after the valid mark)
      const TickResult t = Stations::tick(*st, now, gTz);
      storeMarkDue(i, t.hour_closed, t.day_closed);
      if (t.hour_closed) logl(Log::Chatter, "HIST", "hour closed for %s (%d kept)", st->id, st->hourly.size());  // verbose
      if (t.day_closed) logf("HIST", "day closed for %s (%d kept)", st->id, st->daily.size());
      // the clock's reference follows the RAM ring and the open hour (trusted ones only)
      if (const int n = st->hourly.size()) storeNoteHour(st->hourly.at(n - 1).hour, st->hourly.at(n - 1).quality);
      if (st->hourly.acc().n) storeNoteHour(st->hourly.acc().hour, st->hourly.acc().quality);
    }
    // liveness runs on millis(), so an SNTP jump cannot mark everything stale
    if (st->uploads && !st->online(ms) && !gStaleSent[i]) {
      gStaleSent[i] = true;
      stale[i] = st->cfg.publish;
    }
    if ((gCfgChanged[i] || connectAnnounce) && gMqttConnected && st->uploads) redo[i] = true;
    if (gMqttConnected && st->uploads && st->cfg.publish && !gAnnStep[i]) {  // on change and every last_seen interval
      notes[i] = stationNotices(*st);
      const uint32_t sig = noticesSig(notes[i]) | 1u;
      noteDue[i] = sig != gNoticeSig[i] || ms - gNoticeMs[i] >= gBridge.mqtt_interval_last_seen_s * 1000UL;
      if (noteDue[i]) {
        gNoticeSig[i] = sig;
        gNoticeMs[i] = ms;
      }
      // the outlook, computed only when an upload or an hour close changed it, or the last-seen interval is up
      const bool every = ms - gOutMs[i] >= gBridge.mqtt_interval_last_seen_s * 1000UL;
      if (st->uploads != gOutUp[i] || st->hour_closes != gOutClose[i] || every) {
        gOutUp[i] = st->uploads;
        gOutClose[i] = st->hour_closes;
        OutlookIn oin = outlookInput(*st, gBridge.lat, gBridge.lon, outlookTimeOk(), outlookBuffer(), OUT_BUF_H);  // under gLock
        outlookNearInput(st->last_seen, &oin.near);  // the main airport's admitted report, in the same lock
        outs[i] = outlook(oin);
        const uint32_t osig = outlookSig(outs[i]) | 1u;
        outDue[i] = outs[i].present && (osig != gOutSig[i] || every);
        if (outDue[i]) {
          gOutSig[i] = osig;
          gOutMs[i] = ms;
        }
      }
    }
    if (gMqttConnected) gCfgChanged[i] = false;
  }
  const uint32_t interval = gBridge.mqtt_interval_last_seen_s;
  tz = gTz;
  xSemaphoreGive(gLock);
  for (int i = 0; i < MAX_STATIONS; i++)
    if (noteDue[i] && !publishNotices(ids[i], notes[i], tz)) gNoticeSig[i] = 0;  // a failure goes again next second
  for (int i = 0; i < MAX_STATIONS; i++)
    if (outDue[i] && !publishOutlook(ids[i], outs[i])) gOutSig[i] = gOutUp[i] = 0;  // again next second

  for (int i = 0; i < MAX_STATIONS; i++) {
    if (!stale[i]) continue;
    // a failed offline (MQTT paused or down) is tried again next second, so HA never keeps "online"
    if (publishAvailability(ids[i], false)) logf("HIST", "%s stale (no upload for its stale_s): availability offline", ids[i]);
    else gStaleSent[i] = false;
  }
  storeTick();  // history files, outside gLock
  learnTick();  // the learners, outside gLock
  fwdTick();    // forwarding: decides only when the core is idle, never waits on the net-out task
  if (gMode == Mode::Run) sysTick();
  bridgeTick(connectAnnounce);
  if (connectAnnounce) {
    gNeedAnnounce = false;
    publish(gBridgeTopic, "online", true);
    static bool ghostsDone = false;
    if (!ghostsDone) {
      ghostsDone = true;
      retractGhosts();
      retractStalePromotions();
      // a stored station with no upload since the boot is not online, whatever HA retained from before
      // (sent below, kept pending until it went out)
      for (int i = 0; i < MAX_STATIONS; i++) gQuietOff[i] = true;
    }
    for (int i = 0; i < MAX_STATIONS; i++) gOffPublished[i] = false;  // a fresh session: say it again
  }
  if (gMqttConnected && !stalled()) {
    int nq = 0;
    for (int i = 0; i < MAX_STATIONS; i++) {
      if (!gQuietOff[i]) continue;
      char id[STATION_ID_LEN];
      bool quiet = false;
      xSemaphoreTake(gLock, portMAX_DELAY);
      if (const Station* st = gStations.at(i)) {
        quiet = !st->uploads && st->cfg.publish;
        memcpy(id, st->id, sizeof id);
      }
      xSemaphoreGive(gLock);
      if (!quiet || publishAvailability(id, false)) gQuietOff[i] = false;  // uploaded meanwhile: nothing to say
      if (quiet && !gQuietOff[i]) nq++;
    }
    if (nq) logf("MQTT", "%d station(s) with no upload since the restart: availability offline until they upload", nq);
    retireLegacyTopics();  // once per bridge; a failure tries again next second
  }
  int n = 0;
  for (int i = 0; i < MAX_STATIONS; i++) {  // one scratch view, filled per station
    if (!redo[i]) continue;
    xSemaphoreTake(gLock, portMAX_DELAY);
    Station* st = gStations.at(i);
    if (st) snapshot(i, *st, ms, &gView);
    xSemaphoreGive(gLock);
    if (!st) continue;
    announce(gView);
    n++;
  }
  static bool onlineOnce = false;  // only the first announce is a boot fact, later ones are Events
  if (connectAnnounce) logl(onlineOnce ? Log::Event : Log::Boot, "MQTT", "bridge online, %d station(s) announced", n), onlineOnce = true;
  // Deferred first states: everything the station has, once HA has subscribed.
  for (int i = 0; i < MAX_STATIONS; i++) {
    if (!gStateDueMs[i] || (int32_t)(ms - gStateDueMs[i]) < 0) continue;
    gStateDueMs[i] = 0;
    if (!gMqttConnected) continue;  // a reconnect announces again
    PubView& d = gView;
    xSemaphoreTake(gLock, portMAX_DELAY);
    Station* st = gStations.at(i);
    if (st) snapshot(i, *st, ms, &d);
    xSemaphoreGive(gLock);
    if (!st || !d.publish) continue;
    if (d.has_live) publishState(d);
    publishRejCounters(d, true);
    publishLastSeen(d, true, interval);
  }
}

#if FC_DIAG_BENCH
static volatile uint32_t gTaskFree[DIAG_TASKS];  // written by taskSnapshot() (loop), 0 = no such task
#endif
Diag diagNow() {
  Diag d = {};
  d.ap_clients = WiFi.softAPgetStationNum();
  d.sta_up = staUp();
  d.sta_rssi = d.sta_up ? WiFi.RSSI() : 0;
  d.mqtt_up = gMqttConnected;
  const HeapNow hn = heapNow();  // byte-addressable
  d.heap_free = hn.free8;
  d.heap_min = heapMinBoot();
  d.heap_largest = hn.largest8;
  d.heap_dip = heapDip();
#if FC_DIAG_BENCH
  {
    OtaHeapRec r;
    d.ota_heap_have = otaHeapLast(&r);
    d.ota_start8 = r.start8, d.ota_min8 = r.min8, d.ota_largest_min8 = r.largest8_min;
  }
#endif
  d.uploads = gUploads.load();
  d.queue_drops = gQueueDrops.load();
  d.uploads_dropped_lan = gDropLan.load();
  d.uploads_dropped_key = gDropKey.load();
  d.uploads_dropped_blocked = gDropBlocked.load();
  d.uploads_too_long = gTooLong.load();
  d.uploads_no_slot = gNoSlot.load();
  d.uploads_rate_limited = gRateLimited.load();
  d.uploads_duplicate = gDuplicate.load();
  xSemaphoreTake(gLock, portMAX_DELAY);
  for (int i = 0; i < MAX_STATIONS; i++)
    if (const Station* s = gStations.at(i)) {
      if (s->stale_longest_run > d.stale_longest_run) d.stale_longest_run = s->stale_longest_run;
      d.stale_on_count += s->stale_on_count;
      d.hist_back[i] = s->hourly.back();
    }
  xSemaphoreGive(gLock);
  d.store_fail = storeFails();
  d.store_drop_old = storeDropOld();
  d.store_cut = storeCut();
  d.store_fail_append = storeFailAppend();
  d.store_future_dropped = storeFutureDropped();
  d.clock_held = clockHeldNow();
  d.clock_held_n = clockHeldCount();
  d.clock_ref = storeClockRef();
  d.clock_cut_n = clockCutCount();
  d.store_future_kept = storeFutureKept();
  d.snap_from = storeSnapFrom();
  d.store_retry = storeRetrySteps();
  d.http_stream_abort = streamAborts();
  d.mqtt_init_fail = mqttInitFails();
  d.other_hits = gOtherHits.load();
  d.dns_answered = gDnsAnswered.load();
  d.ntp_answered = gNtpAnswered.load();
  d.ntp_unanswered = gNtpUnanswered.load();
  d.mqtt_published = gPublished;  // written by loop(); a torn read of a word cannot happen on Xtensa
  d.mqtt_failed = gPublishFails;
  fwdDiag(&d);
  staIpStr(d.sta_ip);
  d.fw_version = SW_VERSION;
  d.hostname = gSettings.hostname;
  d.stations_allocated = gStations.allocated();
  d.stations_used = gStations.active();
  d.mode = gMode == Mode::Run ? "run" : "setup";
  adminLockInfo(&d.admin_locked, &d.admin_lock_s, &d.admin_failures);
  d.heap_low = heapLow(&d.heap_min_10m);
  httpMetrics(&d.http_requests, &d.http_304, &d.http_busy_ms_max, &d.http_aborted);
  d.http_purged = httpPurged();
  d.http_shed = httpShed();
  d.http_paced = httpPaced();
  d.store_ms_max = storeMounted() ? (int32_t)storeMsMax() : -1;
  d.build = buildText();
  d.ota_state = otaStateText();
  d.last_reset = lastReset();
  d.reset_raw = resetRaw();
  d.restarts = restarts();
  d.sta_reinits = staReinits();
  d.mqtt_reconnects = mqttReconnects();
  d.ota_slot = (uint32_t)otaSlotSize();
  const CrashInfo& c = lastCrash();
  d.crash = c.present;
  d.crash_reason = c.reason;
  d.crash_task = c.task;
  d.crash_pc = c.pc;
#if FC_DIAG_BENCH
  for (int i = 0; i < DIAG_TASKS; i++) d.task_free[i] = gTaskFree[i];  // heartbeat's snapshot
#endif  // the release reports diag.tasks as {} (all 0)
  return d;
}

#if FC_DIAG_BENCH
// diag.tasks: one FreeRTOS snapshot of every task (trace facility), so no handle is kept: a task that ends
// (ota, the MQTT task stopped for an update, netout) is never read after it is gone. Taken by
// heartbeat() only (loop task, once a minute; ~1 KB of heap for the call), never per request; a high-water mark only
// falls, so a minute-old value is good enough. A failed malloc keeps the previous values.
static void taskSnapshot() {
  const UBaseType_t cap = uxTaskGetNumberOfTasks() + 2;
  TaskStatus_t* ts = (TaskStatus_t*)malloc(cap * sizeof(TaskStatus_t));
  if (!ts) return;
  uint32_t f[DIAG_TASKS] = {};
  const UBaseType_t n = uxTaskGetSystemState(ts, cap, nullptr);
  for (UBaseType_t i = 0; i < n; i++) {
    const int k = diagTaskIndex(ts[i].pcTaskName);
    if (k >= 0) f[k] = ts[i].usStackHighWaterMark ? ts[i].usStackHighWaterMark : 1;  // 0 means "no task"
  }
  free(ts);
  for (int i = 0; i < DIAG_TASKS; i++) gTaskFree[i] = f[i];  // word stores; httpd reads them in diagNow()
}
#endif  // FC_DIAG_BENCH (the release takes no snapshot, no heartbeat allocation)

void bridgeStatusNow(BridgeStatus* b) {
  memset(b, 0, sizeof *b);  // memcmp-compared by everySecond(): no padding garbage
  b->mode = gMode == Mode::Run ? "run" : "setup";
  b->sta = staUp();
  b->mqtt = gMqtt ? (gMqttConnected ? 1 : 0) : -1;
  b->time = timeSynced();
  b->time_src = timeSrcName(timeSrc());
  b->notices = bridgeNoticesNow();
  uint32_t lockS = 0;
  int fails = 0;
  adminLockInfo(&b->admin_locked, &lockS, &fails);
  b->heap_low = heapLow(nullptr);
  b->ota = otaActive();
  b->store_fail = storeFails();
  b->too_long = gTooLong.load();
  b->no_slot = gNoSlot.load();
  const uint32_t ms = millis();
  xSemaphoreTake(gLock, portMAX_DELAY);
  for (int i = 0; i < MAX_STATIONS; i++) {
    const Station* st = gStations.at(i);
    if (!st) continue;
    memcpy(b->ids[b->n], st->id, STATION_ID_LEN);
    b->state[b->n++] = stationStateChar(*st, ms);
  }
  gNoSlot1.expire(ms);  // an hour after its last try
  memcpy(b->no_slot_id, gNoSlot1.id, STATION_ID_LEN);
#if FWD_TLS
  b->metar = metarShownLocked();  // a change bumps the status ETag with the rest (everySecond's memcmp)
#endif
  xSemaphoreGive(gLock);
  if (!b->n && gMode == Mode::Run) b->ap = gApSsid;  // the pairing card names the network
}

void heartbeat() {
  {  // a flapping broker's held state changes end in one line with the true state
    uint32_t n = 0;
    bool up = false;
    portENTER_CRITICAL(&gMqttLogMux);
    const bool due = gMqttLine.settle(millis(), &n, &up);
    portEXIT_CRITICAL(&gMqttLogMux);
    if (due) logf("MQTT", "now %s (x%lu changes)", up ? "connected" : "disconnected", (unsigned long)n);
  }
  apLineSettle();  // the same for a flapping AP client
#if FC_DIAG_BENCH
  taskSnapshot();
#endif
  const Diag d = diagNow();
  // Chatter (the "detailed log" switch), and short enough for the boot log's 200-B line at 100+ days of uptime
  // (worst case 192 B with every counter at 100+ days; login fails and denied calls are in diag and have their own AUTH
  // lines)
  char sta[8];
  if (d.sta_up) snprintf(sta, sizeof sta, "%d", d.sta_rssi);
  else strcpy(sta, "down");
  static bool beatOnce = false;  // the boot's BEAT (setup) stays in the head as the startup snapshot
  logl(beatOnce ? Log::Chatter : Log::Boot, "BEAT",
       "up=%lu f8=%lu m8=%lu l8=%lu%s %s ap=%d sta=%s mqtt=%s %lu/%lu st=%d/%d upl=%lu drop=%lu oth=%lu "
       "dns=%lu/%lu ntp=%lu/%lu t=%s",
       (unsigned long)uptimeS(), (unsigned long)d.heap_free, (unsigned long)d.heap_min, (unsigned long)d.heap_largest,
       d.heap_low ? " LOW" : "", d.mode, d.ap_clients, sta,
       d.mqtt_up ? "up" : (gMqtt ? "down" : "off"), (unsigned long)d.mqtt_published, (unsigned long)d.mqtt_failed,
       d.stations_used, d.stations_allocated, (unsigned long)d.uploads, (unsigned long)d.queue_drops, (unsigned long)d.other_hits,
       (unsigned long)d.dns_answered, (unsigned long)gDnsRejected.load(), (unsigned long)d.ntp_answered,
       (unsigned long)d.ntp_unanswered, timeSrcName(timeSrc()));
  beatOnce = true;
  if (gMode == Mode::Run && timeSynced() && !gTzChecked) {
    gTzChecked = true;  // cross-check our POSIX TZ evaluator against libc once (both got the same TZ string)
    const time_t t = time(nullptr);
    struct tm lt;
    localtime_r(&t, &lt);
    const int32_t libc = (int32_t)(unixFromUtc(lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday, lt.tm_hour, lt.tm_min,
                                               lt.tm_sec) - t);
    xSemaphoreTake(gLock, portMAX_DELAY);
    const int32_t ours = localOffset(gTz, t);
    xSemaphoreGive(gLock);
    logf("CFG", "TZ check: ours %+ld s, libc %+ld s -> %s", (long)ours, (long)libc, ours == libc ? "agree" : "DIFFER");
  }
}

}  // namespace app
