// settings.cpp -- see settings.h.
#include "settings.h"

#include <cmath>
#include <cstdio>
#include <cstring>

#include "json.h"
#include "tz.h"
#include "web_render.h"

namespace fc {

Settings defaultSettings() {
  Settings s;
  memset(&s, 0, sizeof s);
  s.mqtt_port = 1883;
  strcpy(s.hostname, DEFAULT_HOSTNAME);
  return s;
}

static bool printable(const char* s) {
  for (; *s; s++)
    if ((unsigned char)*s < 0x20 || *s == 0x7F) return false;
  return true;
}

bool validSsid(const char* s) {
  size_t n = strlen(s);
  return n >= 1 && n <= 32 && printable(s);
}

bool validWifiPass(const char* s) {
  size_t n = strlen(s);
  return (n == 0 || (n >= 8 && n <= 63)) && printable(s);
}

bool validApPass(const char* s) {
  size_t n = strlen(s);
  return n >= 8 && n <= 63 && printable(s);
}

bool validAdminPass(const char* s) {
  size_t n = strlen(s);
  return n >= 8 && n <= 63 && printable(s);
}

bool validHostname(const char* s) {
  size_t n = strlen(s);
  if (n < 1 || n > 32 || s[0] == '-' || s[n - 1] == '-') return false;
  for (; *s; s++)
    if (!((*s >= 'a' && *s <= 'z') || (*s >= '0' && *s <= '9') || *s == '-')) return false;
  return true;
}

bool settingsComplete(const Settings& s) {
  return validSsid(s.sta_ssid) && validWifiPass(s.sta_pass) && validApPass(s.ap_pass) && s.admin_hash[0] &&
         validHostname(s.hostname);
}

static void str(Out& o, bool& first, const char* k, const char* v) {
  o.printf("%s\"%s\":", first ? "" : ",", k);
  first = false;
  o.jsonStr(v);
}

static void secret(Out& o, bool& first, const char* k, const char* v) { str(o, first, k, v[0] ? SECRET_SET : ""); }

void settingsToJson(Out& o, const Settings& s) {
  bool first = true;
  o.raw("{");
  str(o, first, "sta_ssid", s.sta_ssid);
  secret(o, first, "sta_pass", s.sta_pass);
  str(o, first, "mqtt_host", s.mqtt_host);
  o.printf(",\"mqtt_port\":%u", (unsigned)s.mqtt_port);
  str(o, first, "mqtt_user", s.mqtt_user);
  secret(o, first, "mqtt_pass", s.mqtt_pass);
  secret(o, first, "ap_pass", s.ap_pass);
  secret(o, first, "admin_password", s.admin_hash);
  str(o, first, "hostname", s.hostname);
  o.printf(",\"setup_done\":%s}", s.setup_done ? "true" : "false");
}

struct StrKey {
  const char* key;
  size_t off, cap;
  bool secret;
  bool (*valid)(const char*);
  int group;  // 0 wifi, 1 mqtt, 2 ap, 3 hostname
};

static bool anyStr(const char* s) { return printable(s); }

#define FIELD(name) offsetof(Settings, name), sizeof(((Settings*)0)->name)
static const StrKey KEYS[] = {
    {"sta_ssid", FIELD(sta_ssid), false, validSsid, 0},
    {"sta_pass", FIELD(sta_pass), true, validWifiPass, 0},
    {"mqtt_host", FIELD(mqtt_host), false, anyStr, 1},
    {"mqtt_user", FIELD(mqtt_user), false, anyStr, 1},
    {"mqtt_pass", FIELD(mqtt_pass), true, anyStr, 1},
    {"ap_pass", FIELD(ap_pass), true, validApPass, 2},
    {"hostname", FIELD(hostname), false, validHostname, 3},
};
#undef FIELD

bool settingsFromJson(const char* json, size_t len, Settings* s, Errors* err, SettingsChange* ch) {
  Errors dummy;
  if (!err) err = &dummy;
  SettingsChange local = {};
  if (!ch) ch = &local;
  *ch = SettingsChange{};  // the caller's struct may be uninitialized
  JsonReader r(json, len);
  if (!r.beginObject()) {
    err->add("$", "not a JSON object");
    return false;
  }
  Settings c = *s;
  bool staPassSent = false, mqttPassSent = false;  // a value (or null) for the secret came in this body
  char k[24];
  while (r.nextKey(k, sizeof k)) {
    const StrKey* sk = nullptr;
    for (const auto& x : KEYS)
      if (strcmp(x.key, k) == 0) sk = &x;
    if (sk) {
      bool* sent = strcmp(k, "sta_pass") == 0 ? &staPassSent : strcmp(k, "mqtt_pass") == 0 ? &mqttPassSent : nullptr;
      char v[96];
      bool trunc = false;
      if (r.peek() == 'n') {
        // null clears: the MQTT login, or the Wi-Fi password of an open network. The AP password
        // and the identity fields cannot be cleared.
        r.skipValue();
        if (strcmp(k, "mqtt_user") != 0 && strcmp(k, "mqtt_pass") != 0 && strcmp(k, "sta_pass") != 0) {
          err->add(k, "cannot be cleared");
          continue;
        }
        v[0] = 0;
      } else if (r.peek() != '"') {
        err->add(k, "not a string");
        r.skipValue();
        continue;
      } else {
        if (!r.readString(v, sizeof v, &trunc)) break;
        // A secret sent empty or as the "<set>" marker of a GET is unchanged (absent = "" = unchanged).
        if (sk->secret && (v[0] == 0 || strcmp(v, SECRET_SET) == 0)) continue;
      }
      char* dst = (char*)&c + sk->off;
      if (trunc || strlen(v) >= sk->cap || !sk->valid(v)) {
        err->add(k, "invalid");
        continue;
      }
      if (sent) *sent = true;  // also when equal to the stored one: re-typed for the new endpoint
      if (strcmp(dst, v) == 0) continue;
      strcpy(dst, v);
      switch (sk->group) {
        case 0: ch->wifi = true; break;
        case 1: ch->mqtt = true; break;
        case 2: ch->ap = true; break;
        default: ch->hostname = true; break;
      }
    } else if (strcmp(k, "mqtt_port") == 0) {
      double v;
      const char p = r.peek();
      if (!((p >= '0' && p <= '9') || p == '-')) {
        err->add(k, "not a number");
        r.skipValue();
        continue;
      }
      if (!r.readNumber(&v)) break;
      if (v < 1 || v > 65535 || v != std::floor(v)) {
        err->add(k, "must be 1..65535");
        continue;
      }
      if (c.mqtt_port != (uint16_t)v) {
        c.mqtt_port = (uint16_t)v;
        ch->mqtt = true;
      }
    } else {
      err->add(k, "unknown or read-only key, ignored");
      r.skipValue();
    }
    if (r.failed()) break;
  }
  if (r.failed()) err->add("$", "malformed JSON, the rest of the document was ignored");
  // A stored secret is bound to its endpoint: a new broker or a new network without its own secret in this body
  // gets none, never the stored one.
  const bool mqttMoved = strcmp(c.mqtt_host, s->mqtt_host) != 0 || c.mqtt_port != s->mqtt_port;
  if (mqttMoved && !mqttPassSent && c.mqtt_pass[0]) {
    memset(c.mqtt_pass, 0, sizeof c.mqtt_pass);
    ch->mqtt = ch->mqtt_pass_cleared = true;
  }
  if (strcmp(c.sta_ssid, s->sta_ssid) != 0 && !staPassSent && c.sta_pass[0]) {
    memset(c.sta_pass, 0, sizeof c.sta_pass);
    ch->wifi = ch->sta_pass_cleared = true;
  }
  *s = c;
  return true;
}

bool parseAuthBody(const char* json, size_t len, AuthBody* out) {
  memset(out, 0, sizeof *out);
  out->iters = PBKDF2_ITER;
  JsonReader r(json, len);
  if (!r.beginObject()) return false;
  char k[16];
  while (r.nextKey(k, sizeof k)) {
    char* dst = nullptr;
    size_t cap = 0;
    if (strcmp(k, "nonce") == 0) { dst = out->nonce; cap = sizeof out->nonce; }
    else if (strcmp(k, "proof") == 0) { dst = out->proof; cap = sizeof out->proof; }
    else if (strcmp(k, "salt") == 0 || strcmp(k, "admin_salt") == 0) { dst = out->salt; cap = sizeof out->salt; }
    else if (strcmp(k, "h") == 0 || strcmp(k, "admin_h") == 0) { dst = out->h; cap = sizeof out->h; }
    if (dst && r.peek() == '"') {
      bool trunc = false;
      if (!r.readString(dst, cap, &trunc)) return false;
      if (trunc) dst[0] = 0;
    } else if ((strcmp(k, "iters") == 0 || strcmp(k, "admin_iters") == 0) && r.peek() >= '0' && r.peek() <= '9') {
      double v;
      if (!r.readNumber(&v)) return false;
      out->iters = v >= 0 && v <= 4e9 ? (uint32_t)v : 0;
    } else if (strcmp(k, "t") == 0 && r.peek() >= '0' && r.peek() <= '9') {
      double v;
      if (!r.readNumber(&v)) return false;
      out->t_ms = v >= 0 && v < 1e14 ? (uint64_t)v : 0;  // ms; ~5138 AD at most
    } else {
      r.skipValue();
    }
    if (r.failed()) return false;
  }
  return !r.failed();
}

bool credentialFromBody(const AuthBody& a, char* out, size_t cap) {
  uint8_t salt[SALT_LEN], h[32];
  if (a.iters < 10000 || a.iters > 200000) return false;
  if (!fromHex(a.salt, salt, SALT_LEN) || !fromHex(a.h, h, 32)) return false;
  return formatCredential(salt, a.iters, h, out, cap);
}

bool setupFromJson(const char* json, size_t len, Settings* s, BridgeConfig* b, Errors* err, bool* mqtt_pass_cleared) {
  Settings c = *s;
  BridgeConfig bc = *b;
  bool haveSsid = false, havePass = false, haveAp = false, apSent = false, haveTz = false;
  AuthBody cred;
  if (!parseAuthBody(json, len, &cred)) {
    err->add("$", "not a JSON object");
    return false;
  }
  JsonReader r(json, len);
  r.beginObject();
  char k[24];
  while (r.nextKey(k, sizeof k)) {
    if (strcmp(k, "lat") == 0 || strcmp(k, "lon") == 0) {
      const bool isLat = k[1] == 'a';
      float& dst = isLat ? bc.lat : bc.lon;
      if (r.peek() == 'n') {
        r.skipValue();
        dst = NAN;
        continue;
      }
      double v;
      if (!r.readNumber(&v)) break;
      if (isLat ? (v < -90 || v > 90) : (v < -180 || v > 180)) err->add(k, "out of range");
      else dst = (float)v;
      continue;
    }
    if (strcmp(k, "admin_salt") == 0 || strcmp(k, "admin_h") == 0 || strcmp(k, "admin_iters") == 0) {
      r.skipValue();  // read by parseAuthBody
      continue;
    }
    if (r.peek() != '"') {
      err->add(k, "not a string");
      r.skipValue();
      continue;
    }
    char v[96];
    bool trunc = false;
    if (!r.readString(v, sizeof v, &trunc)) break;
    if (strcmp(k, "sta_ssid") == 0) {
      haveSsid = !trunc && validSsid(v);
      if (haveSsid) strcpy(c.sta_ssid, v);
      else err->add(k, "1..32 characters");
    } else if (strcmp(k, "sta_pass") == 0) {
      havePass = !trunc && validWifiPass(v);
      if (havePass) strcpy(c.sta_pass, v);
      else err->add(k, "empty (open network) or 8..63 characters");
    } else if (strcmp(k, "ap_pass") == 0) {
      apSent = true;
      haveAp = !trunc && validApPass(v);
      if (haveAp) strcpy(c.ap_pass, v);
      else err->add(k, "8..63 characters");
    } else if (strcmp(k, "hostname") == 0) {
      if (!trunc && validHostname(v)) strcpy(c.hostname, v);
      else err->add(k, "a-z, 0-9 and '-', 1..32");
    } else if (strcmp(k, "tz") == 0) {
      PosixTz t;
      haveTz = !trunc && strlen(v) < sizeof bc.tz && parsePosixTz(v, &t);
      if (haveTz) strcpy(bc.tz, v);
      else err->add(k, "not a supported POSIX TZ string");
    } else if (strcmp(k, "tz_name") == 0) {
      if (!trunc && strlen(v) < sizeof bc.tz_name) strcpy(bc.tz_name, v);
      else err->add(k, "too long");
    } else if (strcmp(k, "admin_password") == 0) {
      err->add(k, "never accepted: send admin_salt + admin_h");  // the password never reaches the device
    } else {
      err->add(k, "unknown key, ignored");
    }
    if (r.failed()) break;
  }
  if (r.failed()) err->add("$", "malformed JSON");
  // setup again keeps everything: an absent ap_pass / admin credential keeps the stored one; a re-typed AP
  // password would un-pair the console. Without a stored value (first run, after a factory reset) both are required.
  const bool credSent = cred.salt[0] || cred.h[0];  // iters has a default: not a sign of a credential
  const bool haveCred = credSent ? credentialFromBody(cred, c.admin_hash, sizeof c.admin_hash) : s->admin_hash[0] != 0;
  if (!haveAp && !apSent && s->ap_pass[0]) haveAp = true;
  if (!haveSsid && !err->n) err->add("sta_ssid", "required");
  if (!havePass && !err->n) err->add("sta_pass", "required (empty for an open network)");
  if (!haveCred && !err->n) err->add("admin_h", "required: admin_salt (32 hex) + admin_h (64 hex), iters 10000..200000");
  if (!haveAp && !err->n) err->add("ap_pass", "required");
  if (!haveTz && !err->n) err->add("tz", "required");
  if (err->n || err->dropped) return false;
  const bool clear = s->sta_ssid[0] && strcmp(s->sta_ssid, c.sta_ssid) != 0 && c.mqtt_pass[0];
  if (clear) memset(c.mqtt_pass, 0, sizeof c.mqtt_pass);
  if (mqtt_pass_cleared) *mqtt_pass_cleared = clear;
  *s = c;
  *b = bc;
  return true;
}

void mac4(const uint8_t mac[6], char out[5]) { snprintf(out, 5, "%02X%02X", mac[4], mac[5]); }

}  // namespace fc
