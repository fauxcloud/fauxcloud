// settings.h -- device settings: Wi-Fi, MQTT, AP password, admin hash, hostname, setup state.
//
// Pure model + validation + JSON. Secrets are write-only: settingsToJson() shows "<set>" or "" for them, and
// settingsFromJson() treats "<set>" as "unchanged" so a GET can be edited and PUT back. The admin hash is never in
// JSON at all (own endpoint). Persistence (NVS, one key per field) lives in the sketch. Host-compilable.
#pragma once
#include <cstddef>
#include <cstdint>

#include "auth.h"
#include "config.h"

namespace fc {

constexpr const char* SECRET_SET = "<set>";
constexpr const char* DEFAULT_HOSTNAME = "fauxcloud";

struct Settings {
  char sta_ssid[33];
  char sta_pass[64];   // "" = open network, else 8..63
  char mqtt_host[64];  // "" = MQTT off
  uint16_t mqtt_port;
  char mqtt_user[64];
  char mqtt_pass[64];
  char ap_pass[64];    // 8..63
  char admin_hash[HASH_STR_MAX];
  char hostname[33];   // [a-z0-9-], 1..32, not starting/ending with '-'
  bool setup_done;
};

Settings defaultSettings();

bool validSsid(const char* s);
bool validWifiPass(const char* s);  // "" or 8..63 printable
bool validApPass(const char* s);    // 8..63 printable
bool validHostname(const char* s);
bool validAdminPass(const char* s); // 8..63 bytes, no control characters
// Ready for run mode: everything run mode needs is present and valid.
bool settingsComplete(const Settings& s);

// What a PUT changed, so the sketch knows what to restart. *_pass_cleared: a stored secret dropped because its
// endpoint changed without a new one (for the log).
struct SettingsChange {
  bool wifi, mqtt, ap, hostname;
  bool mqtt_pass_cleared, sta_pass_cleared;
  bool any() const { return wifi || mqtt || ap || hostname; }
};

void settingsToJson(Out& o, const Settings& s);
// Partial update. Keys: sta_ssid, sta_pass, mqtt_host, mqtt_port, mqtt_user, mqtt_pass, ap_pass, hostname.
// Only the keys present apply (partial documents). A secret sent as "" or "<set>" is unchanged; null
// clears mqtt_user / mqtt_pass / sta_pass. Invalid values are rejected per key (Errors), the rest applies.
// A stored secret is bound to its endpoint: a changed mqtt_host or mqtt_port without a new mqtt_pass in the same
// body clears the MQTT password, and a changed sta_ssid without a sta_pass means an open network (sta_pass ""), so a
// stored password never goes to a host or network it was not given for. The MQTT test runs this on a copy, so it tests
// what a save would store.
bool settingsFromJson(const char* json, size_t len, Settings* s, Errors* err, SettingsChange* ch);

// Setup apply body: sta_ssid, sta_pass, ap_pass, admin_salt (32 hex), admin_h (64 hex) and
// tz (POSIX, validated) are required; admin_iters (default PBKDF2_ITER, 10000..200000), tz_name, lat, lon and
// hostname are optional. The admin password itself is never part of it: the page sends H = PBKDF2(password, salt).
// On success fills `s` (Wi-Fi, AP password, hostname, admin credential) and `b` (time zone fields); false + errors
// when anything required is missing or invalid (nothing is applied then).
// in setup again (a stored sta_ssid), a changed SSID clears the stored MQTT password (user and host kept):
// whoever is on the setup AP cannot send it to a broker of their choosing; *mqtt_pass_cleared says so (for the log)
bool setupFromJson(const char* json, size_t len, Settings* s, BridgeConfig* b, Errors* err, bool* mqtt_pass_cleared = nullptr);

// Body of the challenge-response calls: nonce + proof, and for a password change also the new
// salt / H / iterations. Unknown keys are ignored; absent strings stay empty, iters defaults to PBKDF2_ITER.
struct AuthBody {
  char nonce[33];
  char proof[65];
  char salt[33];
  char h[65];
  uint32_t iters;
  uint64_t t_ms;  // the browser's Date.now() (setup time POST, admin login), 0 = not sent
};
bool parseAuthBody(const char* json, size_t len, AuthBody* out);
// New credential from an AuthBody (salt + h + iters); false when malformed or iters out of 10000..200000.
bool credentialFromBody(const AuthBody& a, char* out, size_t cap);

// Setup-mode AP password: shared and documented (the board has no screen or label, and
// a per-device password on serial only is unusable for most people). The setup window is time-boxed (30 min).
constexpr const char* SETUP_AP_PASS = "fauxcloud";
// Last two MAC bytes as 4 upper-case hex characters ("A1B2"), used in the SSIDs.
void mac4(const uint8_t mac[6], char out[5]);

}  // namespace fc
