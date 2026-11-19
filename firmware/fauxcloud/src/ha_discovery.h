// ha_discovery.h -- MQTT topics and Home Assistant discovery payloads for one station = one HA device.
//
// Built with snprintf, no JSON library. Inserted strings are either from the FIELDS table, a station id
// sanitized to [A-Za-z0-9_-], a validated promote key, or JSON-escaped (device name, promote unit/classes).
// Plain C++, host-compilable.
//
// Topics (station topics carry the bridge's <mac4>, the SSID suffix, so two bridges on one
// broker never mix stations of the same id; unique_id does not carry it, so HA keeps the entities):
//   homeassistant/sensor/fauxcloud_<mac12>_<id>/<key>/config   discovery (retained)
//   fauxcloud/<mac4>/<id>/<key>                                state, one plain value per field (NOT retained)
//   fauxcloud/<mac4>/<id>/last_seen                            ISO-8601 UTC, at most every 5 min
//   fauxcloud/<mac4>/<id>/rej_<rule>                           per-rule rejection counter (diagnostic)
//   fauxcloud/<mac4>/<id>/availability                         online / offline per station (retained)
//   fauxcloud/<id>/availability                                the topic before <mac4>: cleared once (empty retained)
//   fauxcloud/bridge/<mac12>/status                            online / offline of the ESP itself (LWT, retained)
//   homeassistant/sensor/fauxcloud_bridge_<mac12>/<key>/config  the bridge device's diagnostic sensors
//   fauxcloud/bridge/<mac12>/<key>                             bridge diagnostics (NOT retained)
// Every station entity needs both availability topics online (availability_mode "all"); bridge entities use
// the bridge LWT alone. Station devices are linked to the bridge device (via_device) and carry configuration_url.
#pragma once
#include <cstddef>
#include <cstdint>

#include "config.h"
#include "fields.h"

namespace fc {

constexpr int FIELD_LAST_SEEN = F_COUNT;  // pseudo-field index for the last_seen timestamp sensor
// the station's "Outdoor sensor link" problem binary_sensor (sensor_stale) and its "Notices" text sensor
constexpr int FIELD_SENSOR_LINK = F_COUNT + 1;
constexpr int FIELD_NOTICES = F_COUNT + 2;
constexpr int FIELD_OUTLOOK = F_COUNT + 3;  // the "Local outlook" text sensor
constexpr int FIELD_WIND_AVG10M = F_COUNT + 4;  // "Wind speed, 10-min average" (derived; no FIELDS row)
constexpr const char* DISCOVERY_PREFIX = "homeassistant";
constexpr const char* TOPIC_ROOT = "fauxcloud";

// Everything a discovery payload needs about one entity. nullptr / "" / -1 = omitted.
struct EntitySpec {
  const char* key;              // topic leaf + object id
  const char* label;            // entity name
  const char* unit;
  const char* device_class;
  const char* state_class;
  const char* entity_category;  // "diagnostic" or nullptr
  int precision;                // suggested_display_precision, -1 = none
  const char* icon = nullptr;   // "mdi:…" or nullptr
  const char* component = nullptr;  // HA component; nullptr = "sensor" (battery_low is a "binary_sensor")
  // a derived entity -- its own retained availability "<state topic>/availability" next to the bridge's
  // and the station's (warm-up = unavailable), and HA attributes from "<state topic>/attr"
  bool own_availability = false;
  bool attributes = false;
};

EntitySpec fieldSpec(int field);  // a FIELDS row or FIELD_LAST_SEEN
// the entity id HA gives an entity it creates from discovery ("default_entity_id", HA >= 2025.10;
// an entity HA already knows by its unique_id keeps its id). haSlug: lowercase, every char outside [a-z0-9] -> '_', runs
// collapsed, edge '_' trimmed (HA rejects "__" and edge "_"); returns the length, 0 = nothing usable (the key is omitted).
int haSlug(char* out, size_t cap, const char* s);
// Per-rule counter: key "rej_<rule>", label "Rejected <rule>"; buffers must outlive the spec.
EntitySpec ruleSpec(int rule, char* key_buf, size_t key_cap, char* label_buf, size_t label_cap);
EntitySpec promoteSpec(const Promote& p);  // label = the name set in admin, precision from the type

// The bridge device "fauxcloud bridge <mac4>": diagnostics only.
// B_LAST_RESTART (timestamp of the boot), not an "uptime" duration sensor (its history is a sawtooth; the old key's
// retained discovery is cleared: BRIDGE_RETIRED_KEY).
// B_TIME_SRC: none | persisted | browser | dhcp | sntp.
// B_NOTICES: the bridge's notices, sentences + attributes (fauxcloud/bridge/<mac12>/notices[/attr]).
// B_WU_TOMORROW: "Forecast tomorrow (Weather Underground)", state = WU's phrase ("None" = unknown while
// nothing is shown), attributes {"day","t_max_c","t_min_c","precip_pct","fetched","source"}; discovered only while the
// forecast switch is on (retracted when off); never on a station device, never in "Local outlook".
// Built only with FC_HA_WUFC 1 (default 0: not in the release build)
#ifndef FC_HA_WUFC
#define FC_HA_WUFC 0
#endif
enum BridgeEntity { B_STA_IP, B_RSSI, B_AP_CLIENTS, B_HEAP, B_LAST_RESTART, B_FIRMWARE, B_LAST_RESET, B_RESTARTS, B_TIME_SRC,
                    B_NOTICES,
#if FC_HA_WUFC
                    B_WU_TOMORROW,
#endif
                    B_COUNT };
constexpr const char* BRIDGE_RETIRED_KEY = "uptime";  // its retained discovery is cleared on every announce
EntitySpec bridgeSpec(int entity);
int bridgeDiscoveryTopic(char* buf, size_t cap, const uint8_t mac[6], const char* key);
int bridgeStateTopic(char* buf, size_t cap, const uint8_t mac[6], const char* key);
// config_url: "http://<sta-ip>/" or nullptr (omitted while the STA has no address).
int bridgeDiscoveryPayload(char* buf, size_t cap, const uint8_t mac[6], const EntitySpec& e, const char* sw_version,
                           const char* config_url);

// All functions return the snprintf length, or -1 when the buffer was too small (nothing usable written).
int discoveryTopicKey(char* buf, size_t cap, const char* station_id, const uint8_t mac[6], const char* key,
                      const char* component = "sensor");
int discoveryPayloadSpec(char* buf, size_t cap, const char* station_id, const char* device_name,
                         const uint8_t mac[6], const EntitySpec& e, const char* sw_version,
                         const char* config_url = nullptr);
int stateTopicKey(char* buf, size_t cap, const uint8_t mac[6], const char* station_id, const char* key);
// "<state topic>/<suffix>" (availability, attr)
int entitySubTopic(char* buf, size_t cap, const uint8_t mac[6], const char* station_id, const char* key,
                   const char* suffix);

// Field-index conveniences (FIELDS rows and FIELD_LAST_SEEN).
int discoveryTopic(char* buf, size_t cap, const char* station_id, const uint8_t mac[6], int field);
int discoveryPayload(char* buf, size_t cap, const char* station_id, const char* device_name, const uint8_t mac[6],
                     int field, const char* sw_version, const char* config_url = nullptr);
int stateTopic(char* buf, size_t cap, const uint8_t mac[6], const char* station_id, int field);

int availabilityTopic(char* buf, size_t cap, const uint8_t mac[6], const char* station_id);
int legacyAvailabilityTopic(char* buf, size_t cap, const char* station_id);  // fauxcloud/<id>/availability (before <mac4>)
int bridgeStatusTopic(char* buf, size_t cap, const uint8_t mac[6]);

// Value as published / rendered: FIELDS[field].decimals digits after the point.
int formatValue(char* buf, size_t cap, int field, float value);
// The MQTT state: formatValue, except the low-battery binary_sensor: "1" whenever its value text is not "0", else "0",
// so HA never shows a value outside its payloads as unknown (the dashboard reads any non-zero value as low too).
int formatState(char* buf, size_t cap, int field, float value);
// "2026-11-01T12:05:03Z"
int formatIsoUtc(char* buf, size_t cap, int64_t unix_s);

// Station ids announced to HA (for ghost stations), kept in NVS as "ID1,ID2" (ids are [A-Za-z0-9_-] only). At boot
// an id on the list that is no longer a station (RAM-only, gone with the reboot) gets its discovery retracted.
constexpr int ANN_ID_LEN = 16;  // = STATION_ID_LEN
class AnnList {
 public:
  static constexpr int MAX = 8;
  void parse(const char* s);
  int format(char* out, size_t cap) const;  // -1 if it does not fit
  bool add(const char* id);     // true when the list changed; when full the oldest id drops
  bool addIfRoom(const char* id);  // the same without the eviction: false when full (the block list)
  bool remove(const char* id);  // true when it was there
  bool has(const char* id) const;
  int size() const { return n_; }
  const char* at(int i) const { return ids_[i]; }

 private:
  char ids_[MAX][ANN_ID_LEN] = {};
  int n_ = 0;
};

// each announced id's promoted entities, kept next to the list (NVS fc_ann, key annPromoKey(id), value
// "key1,key2"), so the boot retraction of a ghost covers its promotions too. Keys are [a-z0-9_]{1,23}.
constexpr int ANN_PROMO_MAX = 4, ANN_PROMO_LEN = 24;
void annPromoKey(const char* id, char out[16]);  // "p" + 8 hex of the id's FNV-1a hash (NVS keys are <= 15 chars)
int annPromoParse(const char* s, char keys[][ANN_PROMO_LEN], int max);  // the keys, at most max; returns how many
// The keys in `have` that are not in `want` (promotions removed in admin: their HA entities are retracted).
int annPromoGone(const char* have, const char* want, char gone[][ANN_PROMO_LEN], int max);

}  // namespace fc
