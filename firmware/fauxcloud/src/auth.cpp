// auth.cpp -- see auth.h (everything except the PBKDF2 / HMAC primitives).
#include "auth.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace fc {

bool ctEqual(const uint8_t* a, const uint8_t* b, size_t n) {
  uint8_t d = 0;
  for (size_t i = 0; i < n; i++) d |= a[i] ^ b[i];
  return d == 0;
}

void toHex(const uint8_t* in, size_t n, char* out) {
  static const char H[] = "0123456789abcdef";
  for (size_t i = 0; i < n; i++) {
    out[2 * i] = H[in[i] >> 4];
    out[2 * i + 1] = H[in[i] & 15];
  }
  out[2 * n] = 0;
}

static int hv(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

bool fromHex(const char* in, uint8_t* out, size_t n) {
  if (!in || strlen(in) != 2 * n) return false;
  for (size_t i = 0; i < n; i++) {
    int a = hv(in[2 * i]), b = hv(in[2 * i + 1]);
    if (a < 0 || b < 0) return false;
    out[i] = (uint8_t)(a << 4 | b);
  }
  return true;
}

// ---------------------------------------------------------------- credential

static const char PREFIX[] = "pbkdf2-sha256$";

bool formatCredential(const uint8_t salt[SALT_LEN], uint32_t iter, const uint8_t h[32], char* out, size_t cap) {
  char s[2 * SALT_LEN + 1], hh[65];
  toHex(salt, SALT_LEN, s);
  toHex(h, 32, hh);
  int n = snprintf(out, cap, "%s%lu$%s$%s", PREFIX, (unsigned long)iter, s, hh);
  return n > 0 && (size_t)n < cap;
}

bool parseCredential(const char* stored, uint8_t salt[SALT_LEN], uint32_t* iter, uint8_t h[32]) {
  if (!stored || strncmp(stored, PREFIX, sizeof PREFIX - 1) != 0) return false;
  const char* p = stored + sizeof PREFIX - 1;
  char* end = nullptr;
  unsigned long it = strtoul(p, &end, 10);
  if (!end || *end != '$' || it < 1000 || it > 1000000) return false;
  p = end + 1;
  const char* dollar = strchr(p, '$');
  if (!dollar || dollar - p != (long)(2 * SALT_LEN)) return false;
  char saltHex[2 * SALT_LEN + 1];
  memcpy(saltHex, p, 2 * SALT_LEN);
  saltHex[2 * SALT_LEN] = 0;
  if (!fromHex(saltHex, salt, SALT_LEN) || !fromHex(dollar + 1, h, 32)) return false;
  *iter = (uint32_t)it;
  return true;
}

bool hashPassword(const char* pw, const uint8_t salt[SALT_LEN], uint32_t iter, char* out, size_t cap) {
  uint8_t h[32];
  if (!pbkdf2Sha256((const uint8_t*)pw, strlen(pw), salt, SALT_LEN, iter, h)) return false;
  return formatCredential(salt, iter, h, out, cap);
}

bool computeProof(const uint8_t h[32], const char* nonce_hex, char proof_hex[65]) {
  uint8_t mac[32];
  if (!hmacSha256(h, 32, (const uint8_t*)nonce_hex, strlen(nonce_hex), mac)) return false;
  toHex(mac, 32, proof_hex);
  return true;
}

bool verifyProof(const char* stored, const char* nonce_hex, const char* proof_hex) {
  uint8_t salt[SALT_LEN], h[32], got[32], want[32];
  uint32_t iter;
  if (!parseCredential(stored, salt, &iter, h)) return false;
  if (!nonce_hex || strlen(nonce_hex) != 32 || !fromHex(proof_hex, got, 32)) return false;
  if (!hmacSha256(h, 32, (const uint8_t*)nonce_hex, 32, want)) return false;
  return ctEqual(got, want, 32);
}

// ---------------------------------------------------------------- nonces

bool NonceStore::issue(const uint8_t rnd[16], uint32_t ip, uint32_t now_ms, char out[33]) {
  Slot* slot = nullptr;
  for (auto& s : s_)  // this address's own slot first: rate-limited, then replaced
    if (s.used && s.ip == ip) slot = &s;
  if (slot && now_ms - slot->at < MIN_GAP_MS) return false;
  if (!slot)
    for (auto& s : s_)
      if (!s.used || now_ms - s.at >= TTL_MS) { slot = &s; break; }
  if (!slot) return false;  // every slot held by another address: never evicted
  slot->used = true;
  memcpy(slot->n, rnd, 16);
  slot->ip = ip;
  slot->at = now_ms;
  toHex(rnd, 16, out);
  return true;
}

bool NonceStore::consume(const char* hex, uint32_t ip, uint32_t now_ms) {
  uint8_t n[16];
  if (!fromHex(hex, n, 16)) return false;
  bool ok = false;
  for (auto& s : s_) {
    if (s.used && s.ip == ip && ctEqual(s.n, n, 16)) {
      ok = now_ms - s.at < TTL_MS;
      s.used = false;  // single use, also when expired
    }
  }
  return ok;
}

int NonceStore::outstanding(uint32_t now_ms) {
  int k = 0;
  for (auto& s : s_) k += s.used && now_ms - s.at < TTL_MS;
  return k;
}

// ---------------------------------------------------------------- sessions

void SessionStore::expire(uint32_t now_s) {
  for (auto& s : s_)
    if (s.used && now_s - s.info.last >= IDLE_S) s.used = false;
}

void SessionStore::create(const uint8_t token_rnd[16], const uint8_t id_rnd[4], uint32_t ip, const char* ua,
                          uint32_t now_s, char out[33]) {
  expire(now_s);
  Slot* slot = nullptr;
  for (auto& s : s_)
    if (!s.used) { slot = &s; break; }
  if (!slot) {
    slot = &s_[0];
    for (auto& s : s_)
      if (s.info.last < slot->info.last) slot = &s;  // least recently used
  }
  slot->used = true;
  memcpy(slot->token, token_rnd, 16);
  toHex(id_rnd, 4, slot->info.id);
  slot->info.ip = ip;
  slot->info.created = slot->info.last = now_s;
  snprintf(slot->info.ua, sizeof slot->info.ua, "%s", ua ? ua : "");
  toHex(token_rnd, 16, out);
}

bool SessionStore::check(const char* hex, uint32_t ip, uint32_t now_s, char id_out[9]) {
  uint8_t t[16];
  if (!fromHex(hex, t, 16)) return false;
  expire(now_s);
  bool ok = false;
  for (auto& s : s_) {
    // compare against every slot, constant time per slot; a cookie from another IP is ignored (IP-bound)
    if (s.used && ctEqual(s.token, t, 16) && s.info.ip == ip) {
      s.info.last = now_s;
      if (id_out) memcpy(id_out, s.info.id, 9);
      ok = true;
    }
  }
  return ok;
}

bool SessionStore::remove(const char* hex) {
  uint8_t t[16];
  if (!fromHex(hex, t, 16)) return false;
  for (auto& s : s_)
    if (s.used && ctEqual(s.token, t, 16)) {
      s.used = false;
      return true;
    }
  return false;
}

bool SessionStore::revoke(const char* id) {
  for (auto& s : s_)
    if (s.used && strcmp(s.info.id, id) == 0) {
      s.used = false;
      return true;
    }
  return false;
}

void SessionStore::clear() {
  for (auto& s : s_) s.used = false;
}

int SessionStore::count(uint32_t now_s) {
  expire(now_s);
  int n = 0;
  for (auto& s : s_) n += s.used;
  return n;
}

int SessionStore::list(uint32_t now_s, Info* out, int cap) {
  expire(now_s);
  int n = 0;
  for (auto& s : s_)
    if (s.used && n < cap) out[n++] = s.info;
  return n;
}

// ---------------------------------------------------------------- per-IP lockout

Lockout::Slot* Lockout::find(uint32_t ip, bool create, uint32_t now_s) {
  Slot* oldest = &s_[0];
  for (auto& s : s_) {
    if (s.used && s.ip == ip) return &s;
    if (!s.used) oldest = &s;
    else if (oldest->used && s.last < oldest->last) oldest = &s;
  }
  if (!create) return nullptr;
  *oldest = Slot{true, ip, 0, now_s, 0, now_s};
  return oldest;
}

bool Lockout::locked(uint32_t ip, uint32_t now_s, uint32_t* retry_s) {
  Slot* s = find(ip, false, now_s);
  if (!s || now_s >= s->until) return false;
  if (retry_s) *retry_s = s->until - now_s;
  return true;
}

bool Lockout::fail(uint32_t ip, uint32_t now_s) {
  Slot* s = find(ip, true, now_s);
  s->last = now_s;
  if (s->fails == 0 || now_s - s->first >= WINDOW_S) {  // a new window
    s->fails = 0;
    s->first = now_s;
  }
  if (++s->fails >= MAX_FAILS) {
    s->fails = 0;
    s->until = now_s + LOCK_S;
    return true;
  }
  return false;
}

void Lockout::success(uint32_t ip) {
  for (auto& s : s_)
    if (s.used && s.ip == ip) s.used = false;
}

void Lockout::clear() {
  for (auto& s : s_) s.used = false;
}

// ---------------------------------------------------------------- account-wide lock

bool AccountLock::locked(uint32_t now_s, uint32_t* until_s) const {
  if (until_ == 0 || now_s >= until_) return false;
  if (until_s) *until_s = until_;
  return true;
}

int AccountLock::failures(uint32_t now_s) const {
  int k = 0;
  for (int i = 0; i < n_; i++) k += now_s - times_[i] < WINDOW_S;
  return k;
}

bool AccountLock::fail(uint32_t now_s, uint32_t* lock_s) {
  if (locked(now_s)) return false;  // a proof during the lock fails anyway; it does not extend it
  times_[head_] = now_s;
  head_ = (head_ + 1) % MAX_FAILS;
  if (n_ < MAX_FAILS) n_++;
  if (failures(now_s) < MAX_FAILS) return false;
  const uint32_t len = next_lock_s_;
  until_ = now_s + len;
  if (until_ == 0) until_ = 1;
  next_lock_s_ = next_lock_s_ * 2 > MAX_LOCK_S ? MAX_LOCK_S : next_lock_s_ * 2;
  n_ = head_ = 0;  // a fresh burst is needed for the next lock
  was_locked_ = true;
  if (lock_s) *lock_s = len;
  return true;
}

void AccountLock::success() {
  n_ = head_ = 0;
  until_ = 0;
  next_lock_s_ = BASE_LOCK_S;
  was_locked_ = false;
}

bool AccountLock::expiredSince(uint32_t now_s) {
  if (was_locked_ && !locked(now_s)) {
    was_locked_ = false;
    return true;
  }
  return false;
}

// ---------------------------------------------------------------- cookies

bool cookieValue(const char* header, const char* name, char* out, size_t cap) {
  const size_t nl = strlen(name);
  const char* p = header;
  while (p && *p) {
    while (*p == ' ' || *p == ';') p++;
    const char* semi = strchr(p, ';');
    const size_t len = semi ? (size_t)(semi - p) : strlen(p);
    if (len > nl && strncmp(p, name, nl) == 0 && p[nl] == '=') {
      const size_t vl = len - nl - 1;
      if (vl + 1 > cap) return false;
      memcpy(out, p + nl + 1, vl);
      out[vl] = 0;
      return true;
    }
    p = semi;
  }
  return false;
}

}  // namespace fc
