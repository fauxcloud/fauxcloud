// ha_discovery.cpp -- see ha_discovery.h.
#include "ha_discovery.h"

#include <cmath>
#include <cstdio>
#include <cstring>

#include "filter.h"
#include "json.h"
#include "settings.h"

namespace fc {

static int checked(int n, size_t cap) { return (n < 0 || (size_t)n >= cap) ? -1 : n; }

static const char* fieldKey(int field) { return field >= F_COUNT ? fieldSpec(field).key : FIELDS[field].key; }

static void macHex(char out[13], const uint8_t mac[6]) {
  snprintf(out, 13, "%02x%02x%02x%02x%02x%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

EntitySpec fieldSpec(int field) {
  if (field == FIELD_LAST_SEEN) return EntitySpec{"last_seen", "Last seen", "", "timestamp", nullptr, "diagnostic", -1};
  // on = the outdoor sensor is not updating (a problem), payload "1" / "0"
  if (field == FIELD_SENSOR_LINK)
    return EntitySpec{"sensor_stale", "Outdoor sensor link", "", "problem", nullptr, nullptr, -1, nullptr, "binary_sensor"};
  if (field == FIELD_NOTICES) {  // plain sentences ("OK" when none) + {"items":[{id, since}]}
    EntitySpec e{"notices", "Notices", "", nullptr, nullptr, "diagnostic", -1, "mdi:message-alert"};
    e.attributes = true;
    return e;
  }
  if (field == FIELD_OUTLOOK) {  // English sentence state + code attributes; no device / state class
    EntitySpec e{"outlook", "Local outlook", "", nullptr, nullptr, nullptr, -1, "mdi:text-box-outline"};  // neutral: no sky claim
    e.attributes = true;
    return e;
  }
  if (field == FIELD_WIND_AVG10M) {  // derived from the recent ring, {"source":"derived","n":N,"window_s":600}
    EntitySpec e{"wind_avg10m_kmh", "Wind speed, 10-min average", "km/h", "wind_speed", "measurement", nullptr, 1};
    e.attributes = true;
    return e;
  }
  // the console's low-battery flag is a battery binary_sensor (on = low), payload "1" / "0"
  if (field == F_BATTERY_LOW)
    return EntitySpec{FIELDS[field].key, FIELDS[field].label, "", "battery", nullptr, nullptr, -1, nullptr, "binary_sensor"};
  const FieldDef& d = FIELDS[field];
  return EntitySpec{d.key, d.label, d.unit, d.device_class, d.state_class, nullptr, d.decimals};
}

EntitySpec ruleSpec(int rule, char* key_buf, size_t key_cap, char* label_buf, size_t label_cap) {
  snprintf(key_buf, key_cap, "rej_%s", ruleName(rule));
  snprintf(label_buf, label_cap, "Rejected %s", ruleName(rule));
  for (char* p = label_buf; *p; p++)
    if (*p == '_') *p = ' ';
  return EntitySpec{key_buf, label_buf, "", nullptr, "total_increasing", "diagnostic", 0};
}

EntitySpec promoteSpec(const Promote& p) {
  return EntitySpec{p.key, p.name[0] ? p.name : p.key, p.unit, p.device_class[0] ? p.device_class : nullptr,
                    p.state_class[0] ? p.state_class : nullptr, nullptr, (int)promoType(p.type).dec};
}

EntitySpec bridgeSpec(int entity) {
  switch (entity) {
    case B_STA_IP: return EntitySpec{"sta_ip", "IP address", "", nullptr, nullptr, "diagnostic", -1};
    case B_RSSI: return EntitySpec{"rssi", "Wi-Fi signal", "dBm", "signal_strength", "measurement", "diagnostic", 0};
    case B_AP_CLIENTS: return EntitySpec{"ap_clients", "AP clients", "", nullptr, "measurement", "diagnostic", 0};
    case B_HEAP: return EntitySpec{"heap_free", "Free heap", "B", "data_size", "measurement", "diagnostic", 0};
    case B_LAST_RESTART: return EntitySpec{"last_restart", "Last restart", "", "timestamp", nullptr, "diagnostic", -1};
    case B_LAST_RESET: return EntitySpec{"last_reset", "Last reset reason", "", nullptr, nullptr, "diagnostic", -1, "mdi:restart-alert"};
    case B_RESTARTS: return EntitySpec{"restarts", "Restarts", "", nullptr, "total_increasing", "diagnostic", 0};
    case B_TIME_SRC: return EntitySpec{"time_src", "Time source", "", nullptr, nullptr, "diagnostic", -1, "mdi:clock-check-outline"};
    case B_NOTICES: {
      EntitySpec e{"notices", "Notices", "", nullptr, nullptr, "diagnostic", -1, "mdi:message-alert"};
      e.attributes = true;
      return e;
    }
#if FC_HA_WUFC
    case B_WU_TOMORROW: {  // not diagnostic -- a reading the user acts on
      EntitySpec e{"wu_tomorrow", "Forecast tomorrow (Weather Underground)", "", nullptr, nullptr, nullptr, -1,
                   "mdi:weather-partly-cloudy"};
      e.attributes = true;
      return e;
    }
#endif
    default: return EntitySpec{"firmware", "Firmware", "", nullptr, nullptr, "diagnostic", -1};
  }
}

int bridgeDiscoveryTopic(char* buf, size_t cap, const uint8_t mac[6], const char* key) {
  char m[13];
  macHex(m, mac);
  return checked(snprintf(buf, cap, "%s/sensor/fauxcloud_bridge_%s/%s/config", DISCOVERY_PREFIX, m, key), cap);
}

int bridgeStateTopic(char* buf, size_t cap, const uint8_t mac[6], const char* key) {
  char m[13];
  macHex(m, mac);
  return checked(snprintf(buf, cap, "%s/bridge/%s/%s", TOPIC_ROOT, m, key), cap);
}

int discoveryTopicKey(char* buf, size_t cap, const char* station_id, const uint8_t mac[6], const char* key,
                      const char* component) {
  char m[13];
  macHex(m, mac);
  return checked(snprintf(buf, cap, "%s/%s/fauxcloud_%s_%s/%s/config", DISCOVERY_PREFIX, component ? component : "sensor",
                          m, station_id, key),
                 cap);
}

int stateTopicKey(char* buf, size_t cap, const uint8_t mac[6], const char* station_id, const char* key) {
  char m4[5];
  mac4(mac, m4);  // the SSID rule (fauxcloud-<mac4>): one helper
  return checked(snprintf(buf, cap, "%s/%s/%s/%s", TOPIC_ROOT, m4, station_id, key), cap);
}

int entitySubTopic(char* buf, size_t cap, const uint8_t mac[6], const char* station_id, const char* key,
                   const char* suffix) {
  char m4[5];
  mac4(mac, m4);
  return checked(snprintf(buf, cap, "%s/%s/%s/%s/%s", TOPIC_ROOT, m4, station_id, key, suffix), cap);
}

int discoveryTopic(char* buf, size_t cap, const char* station_id, const uint8_t mac[6], int field) {
  return discoveryTopicKey(buf, cap, station_id, mac, fieldKey(field));
}

int stateTopic(char* buf, size_t cap, const uint8_t mac[6], const char* station_id, int field) {
  return stateTopicKey(buf, cap, mac, station_id, fieldKey(field));
}

int availabilityTopic(char* buf, size_t cap, const uint8_t mac[6], const char* station_id) {
  return stateTopicKey(buf, cap, mac, station_id, "availability");
}

int legacyAvailabilityTopic(char* buf, size_t cap, const char* station_id) {
  return checked(snprintf(buf, cap, "%s/%s/availability", TOPIC_ROOT, station_id), cap);
}

int bridgeStatusTopic(char* buf, size_t cap, const uint8_t mac[6]) {
  char m[13];
  macHex(m, mac);
  return checked(snprintf(buf, cap, "%s/bridge/%s/status", TOPIC_ROOT, m), cap);
}

int haSlug(char* out, size_t cap, const char* s) {
  if (!cap) return 0;
  size_t n = 0;
  bool us = false;
  for (const char* p = s ? s : ""; *p; p++) {
    char c = *p;
    if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
    if (!ok) {
      us = n > 0;  // a run becomes one '_', never a leading one
      continue;
    }
    if (us && n + 1 < cap) out[n++] = '_';
    us = false;
    if (n + 1 < cap) out[n++] = c;
  }
  out[n] = 0;  // a trailing run was never written
  return (int)n;
}

// ,"default_entity_id":"<component>.fauxcloud_<a>_<b>" or nothing (an empty slug)
static void defaultEntityId(char out[160], const char* component, const char* a, const char* b) {
  char sa[48], sb[48];
  out[0] = 0;
  if (!haSlug(sa, sizeof sa, a) || !haSlug(sb, sizeof sb, b)) return;
  snprintf(out, 160, ",\"default_entity_id\":\"%s.fauxcloud_%s_%s\"", component ? component : "sensor", sa, sb);
}

// Appends ,"name":<quoted value> to buf at *n; false when it does not fit.
static bool member(char* buf, size_t cap, size_t* n, const char* name, const char* value) {
  if (*n >= cap) return false;
  int k = snprintf(buf + *n, cap - *n, ",\"%s\":", name);
  if (k < 0 || *n + (size_t)k >= cap) return false;
  *n += (size_t)k;
  size_t q = jsonQuote(buf + *n, cap - *n, value);
  if (*n + q >= cap) return false;
  *n += q;
  return true;
}

// {"name":...,"unit_of_measurement",...,"suggested_display_precision" -- the entity part both devices share.
static int entityHead(char* buf, size_t cap, const EntitySpec& e) {
  if (cap < 2) return -1;
  buf[0] = '{';
  size_t n = 1;
  // name first, without the leading comma
  size_t q = jsonQuote(buf + n + 7, cap > n + 7 ? cap - n - 7 : 0, e.label);
  if (n + 7 + q >= cap) return -1;
  memcpy(buf + n, "\"name\":", 7);
  n += 7 + q;
  bool ok = true;
  if (e.unit && e.unit[0]) ok &= member(buf, cap, &n, "unit_of_measurement", e.unit);
  if (ok && e.device_class) ok &= member(buf, cap, &n, "device_class", e.device_class);
  if (ok && e.state_class) ok &= member(buf, cap, &n, "state_class", e.state_class);
  if (ok && e.entity_category) ok &= member(buf, cap, &n, "entity_category", e.entity_category);
  if (ok && e.icon) ok &= member(buf, cap, &n, "icon", e.icon);
  if (ok && e.component && strcmp(e.component, "binary_sensor") == 0) {  // our state payloads: formatValue, 0 decimals
    ok &= member(buf, cap, &n, "payload_on", "1");
    ok &= ok && member(buf, cap, &n, "payload_off", "0");
  }
  if (!ok) return -1;
  if (e.precision >= 0) {
    int k = snprintf(buf + n, cap - n, ",\"suggested_display_precision\":%d", e.precision);
    if (k < 0 || n + (size_t)k >= cap) return -1;
    n += (size_t)k;
  }
  return (int)n;
}

// ,"configuration_url":"..." or nothing. The URL is built from an IPv4 address: nothing to escape.
static void configUrl(char out[48], const char* url) {
  if (url && url[0]) snprintf(out, 48, ",\"configuration_url\":\"%s\"", url);
  else out[0] = 0;
}

int discoveryPayloadSpec(char* buf, size_t cap, const char* station_id, const char* device_name,
                         const uint8_t mac[6], const EntitySpec& e, const char* sw_version, const char* config_url) {
  char m[13];
  macHex(m, mac);
  char state[64], avail[64], bridge[64], own[112] = "", attr[112] = "";
  if (stateTopicKey(state, sizeof state, mac, station_id, e.key) < 0) return -1;
  if (availabilityTopic(avail, sizeof avail, mac, station_id) < 0) return -1;
  char t[96];
  if (e.own_availability && (entitySubTopic(t, sizeof t, mac, station_id, e.key, "availability") < 0 ||
                             snprintf(own, sizeof own, ",{\"topic\":\"%s\"}", t) >= (int)sizeof own))
    return -1;
  if (e.attributes && (entitySubTopic(t, sizeof t, mac, station_id, e.key, "attr") < 0 ||
                       snprintf(attr, sizeof attr, ",\"json_attributes_topic\":\"%s\"", t) >= (int)sizeof attr))
    return -1;
  if (bridgeStatusTopic(bridge, sizeof bridge, mac) < 0) return -1;
  const int head = entityHead(buf, cap, e);
  if (head < 0) return -1;
  size_t n = (size_t)head;
  char cu[48];
  configUrl(cu, config_url);
  char devName[128];
  jsonQuote(devName, sizeof devName, device_name);  // <= 47 bytes of name, escapes at most double it
  char dei[160];
  defaultEntityId(dei, e.component, station_id, e.key);  // sensor.fauxcloud_<id>_<key>
  int k = snprintf(buf + n, cap - n,
                   ",\"unique_id\":\"fauxcloud_%s_%s_%s\"%s"
                   ",\"state_topic\":\"%s\""
                   ",\"availability\":[{\"topic\":\"%s\"},{\"topic\":\"%s\"}%s]"
                   ",\"availability_mode\":\"all\"%s"
                   ",\"device\":{\"identifiers\":[\"fauxcloud_%s_%s\"],\"name\":%s"
                   ",\"manufacturer\":\"fauxcloud\",\"model\":\"WU-protocol weather station\""
                   ",\"sw_version\":\"%s\",\"via_device\":\"fauxcloud_bridge_%s\"%s}}",
                   m, station_id, e.key, dei, state, bridge, avail, own, attr, m, station_id, devName, sw_version, m, cu);
  if (k < 0 || n + (size_t)k >= cap) return -1;
  return (int)(n + (size_t)k);
}

int bridgeDiscoveryPayload(char* buf, size_t cap, const uint8_t mac[6], const EntitySpec& e, const char* sw_version,
                           const char* config_url) {
  char m[13];
  macHex(m, mac);
  char state[64], bridge[64], attr[96] = "";
  if (bridgeStateTopic(state, sizeof state, mac, e.key) < 0) return -1;
  if (e.attributes && snprintf(attr, sizeof attr, ",\"json_attributes_topic\":\"%s/attr\"", state) >= (int)sizeof attr) return -1;
  if (bridgeStatusTopic(bridge, sizeof bridge, mac) < 0) return -1;
  const int head = entityHead(buf, cap, e);
  if (head < 0) return -1;
  const size_t n = (size_t)head;
  char cu[48];
  configUrl(cu, config_url);
  // mac4 = the last two bytes, as in the AP name fauxcloud-<mac4>
  char mac4[16], dei[160];
  snprintf(mac4, sizeof mac4, "bridge_%02x%02x", mac[4], mac[5]);
  defaultEntityId(dei, e.component, mac4, e.key);  // sensor.fauxcloud_bridge_<mac4>_<key>
  int k = snprintf(buf + n, cap - n,
                   ",\"unique_id\":\"fauxcloud_bridge_%s_%s\"%s"
                   ",\"state_topic\":\"%s\""
                   ",\"availability_topic\":\"%s\"%s"
                   ",\"device\":{\"identifiers\":[\"fauxcloud_bridge_%s\"],\"name\":\"fauxcloud bridge %02X%02X\""
                   ",\"manufacturer\":\"fauxcloud\",\"model\":\"fauxcloud bridge (ESP32)\""
                   ",\"sw_version\":\"%s\"%s}}",
                   m, e.key, dei, state, bridge, attr, m, mac[4], mac[5], sw_version, cu);
  if (k < 0 || n + (size_t)k >= cap) return -1;
  return (int)(n + (size_t)k);
}

int discoveryPayload(char* buf, size_t cap, const char* station_id, const char* device_name, const uint8_t mac[6],
                     int field, const char* sw_version, const char* config_url) {
  return discoveryPayloadSpec(buf, cap, station_id, device_name, mac, fieldSpec(field), sw_version, config_url);
}

int formatValue(char* buf, size_t cap, int field, float value) {
  if (!std::isfinite(value)) return -1;  // "inf" / "nan" never reach MQTT
  const int dec = field < F_COUNT ? (int)FIELDS[field].decimals : field == FIELD_WIND_AVG10M ? 1 : 0;  // never FIELDS[] past F_COUNT
  return checked(snprintf(buf, cap, "%.*f", dec, (double)value), cap);
}

int formatState(char* buf, size_t cap, int field, float value) {
  const int n = formatValue(buf, cap, field, value);
  if (n < 0 || field != F_BATTERY_LOW || strcmp(buf, "0") == 0) return n;
  return checked(snprintf(buf, cap, "1"), cap);
}

int formatIsoUtc(char* buf, size_t cap, int64_t t) {
  int64_t days = t >= 0 ? t / 86400 : (t - 86399) / 86400;
  int64_t sod = t - days * 86400;
  // H. Hinnant's civil_from_days.
  days += 719468;
  const int64_t era = (days >= 0 ? days : days - 146096) / 146097;
  const int64_t doe = days - era * 146097;
  const int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const int64_t mp = (5 * doy + 2) / 153;
  const int d = (int)(doy - (153 * mp + 2) / 5 + 1);
  const int mo = (int)(mp < 10 ? mp + 3 : mp - 9);
  const int y = (int)(yoe + era * 400 + (mo <= 2));
  return checked(snprintf(buf, cap, "%04d-%02d-%02dT%02d:%02d:%02dZ", y, mo, d, (int)(sod / 3600),
                          (int)(sod % 3600 / 60), (int)(sod % 60)),
                 cap);
}

void AnnList::parse(const char* s) {
  n_ = 0;
  while (s && *s && n_ < MAX) {
    const char* e = strchr(s, ',');
    const size_t len = e ? (size_t)(e - s) : strlen(s);
    if (len > 0 && len < ANN_ID_LEN) {
      memcpy(ids_[n_], s, len);
      ids_[n_][len] = 0;
      if (!has(ids_[n_])) n_++;  // has() looks at the first n_ only: duplicates are skipped
    }
    s = e ? e + 1 : nullptr;
  }
}

int AnnList::format(char* out, size_t cap) const {
  size_t k = 0;
  if (cap) out[0] = 0;
  for (int i = 0; i < n_; i++) {
    const size_t len = strlen(ids_[i]);
    if (k + len + (i ? 1 : 0) + 1 > cap) return -1;
    if (i) out[k++] = ',';
    memcpy(out + k, ids_[i], len + 1);
    k += len;
  }
  return (int)k;
}

bool AnnList::has(const char* id) const {
  for (int i = 0; i < n_; i++)
    if (strcmp(ids_[i], id) == 0) return true;
  return false;
}

bool AnnList::add(const char* id) {
  if (!id || !id[0] || strlen(id) >= ANN_ID_LEN || has(id)) return false;
  if (n_ == MAX) {
    memmove(ids_[0], ids_[1], sizeof ids_[0] * (MAX - 1));
    n_--;
  }
  snprintf(ids_[n_++], ANN_ID_LEN, "%s", id);
  return true;
}

bool AnnList::addIfRoom(const char* id) { return n_ < MAX && add(id); }

bool AnnList::remove(const char* id) {
  for (int i = 0; i < n_; i++)
    if (strcmp(ids_[i], id) == 0) {
      memmove(ids_[i], ids_[i + 1], sizeof ids_[0] * (size_t)(n_ - 1 - i));
      n_--;
      return true;
    }
  return false;
}

void annPromoKey(const char* id, char out[16]) {
  uint32_t h = 2166136261u;
  for (const char* p = id; *p; p++) h = (h ^ (uint8_t)*p) * 16777619u;
  snprintf(out, 16, "p%08lx", (unsigned long)h);
}

int annPromoGone(const char* have, const char* want, char gone[][ANN_PROMO_LEN], int max) {
  char h[ANN_PROMO_MAX][ANN_PROMO_LEN], w[ANN_PROMO_MAX][ANN_PROMO_LEN];
  const int nh = annPromoParse(have, h, ANN_PROMO_MAX), nw = annPromoParse(want, w, ANN_PROMO_MAX);
  int n = 0;
  for (int i = 0; i < nh && n < max; i++) {
    bool kept = false;
    for (int j = 0; j < nw && !kept; j++) kept = strcmp(h[i], w[j]) == 0;
    // h[i] holds at most ANN_PROMO_LEN - 1 characters, so this never truncates; GCC 15 sizes the source by the whole
    // 2-D array and warns (format-truncation), an error under the host tests' -Werror. Silenced for this call only.
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-truncation"
#endif
    if (!kept) snprintf(gone[n++], ANN_PROMO_LEN, "%s", h[i]);
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
  }
  return n;
}

int annPromoParse(const char* s, char keys[][ANN_PROMO_LEN], int max) {
  int n = 0;
  while (s && *s && n < max) {
    const char* e = strchr(s, ',');
    const size_t len = e ? (size_t)(e - s) : strlen(s);
    if (len && len < ANN_PROMO_LEN) {
      memcpy(keys[n], s, len);
      keys[n++][len] = 0;
    }
    s = e ? e + 1 : nullptr;
  }
  return n;
}

}  // namespace fc
