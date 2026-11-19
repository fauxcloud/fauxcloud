// auth.h -- admin credential, challenge-response login, sessions, lockouts, cookie parsing (the protocol
// is in docs/api.md §4).
//
// The admin password never reaches the ESP. The browser computes H = PBKDF2-SHA256(password, salt, iters) and
// proves knowledge of it with proof = HMAC-SHA256(H, nonce), where `nonce` is a one-time 32-hex string from
// GET /api/admin/challenge and is used as the HMAC message in its ASCII hex form. The ESP stores salt + H only.
// PBKDF2 / HMAC run on mbedTLS on the device (auth_mbedtls.cpp) and on a small software SHA-256 on the host
// (auth_soft.cpp); the same vectors are checked by the host test, a boot self-test and the JS in the pages.
// Everything else here is plain C++, host-compilable, RAM-only.
#pragma once
#include <cstddef>
#include <cstdint>

namespace fc {

constexpr uint32_t PBKDF2_ITER = 10000;
constexpr size_t SALT_LEN = 16;
constexpr size_t HASH_STR_MAX = 128;  // "pbkdf2-sha256$<iter>$<salt hex 32>$<H hex 64>"

// Platform implementation (mbedTLS on the device, software on the host).
bool pbkdf2Sha256(const uint8_t* pw, size_t pw_len, const uint8_t* salt, size_t salt_len, uint32_t iter,
                  uint8_t out[32]);
bool hmacSha256(const uint8_t* key, size_t key_len, const uint8_t* msg, size_t msg_len, uint8_t out[32]);

bool ctEqual(const uint8_t* a, const uint8_t* b, size_t n);  // constant time for equal lengths
void toHex(const uint8_t* in, size_t n, char* out);          // out: 2n + 1 bytes
bool fromHex(const char* in, uint8_t* out, size_t n);         // exactly 2n hex chars

// ---------------------------------------------------------------- stored credential
// Formats salt + iterations + H; false if cap is too small.
bool formatCredential(const uint8_t salt[SALT_LEN], uint32_t iter, const uint8_t h[32], char* out, size_t cap);
// Parses a stored credential; false for an empty / malformed one (no password set = no login).
bool parseCredential(const char* stored, uint8_t salt[SALT_LEN], uint32_t* iter, uint8_t h[32]);
// Host-side convenience (dev seed, tests): derives H from a plain password and formats the credential.
bool hashPassword(const char* pw, const uint8_t salt[SALT_LEN], uint32_t iter, char* out, size_t cap);
// proof_hex == HMAC(stored H, nonce_hex as ASCII), constant time. False for malformed input.
bool verifyProof(const char* stored, const char* nonce_hex, const char* proof_hex);
// What the browser computes; used by tests and the boot self-test.
bool computeProof(const uint8_t h[32], const char* nonce_hex, char proof_hex[65]);

// ---------------------------------------------------------------- one-time nonces
// a nonce belongs to the address that asked for it. One outstanding per address (a new challenge
// replaces that address's previous one), one new nonce per second per address, accepted only from that address.
// Another address can neither use nor evict it: with every slot held by other addresses the challenge is refused.
class NonceStore {
 public:
  static constexpr int MAX = 4;               // outstanding, one per address
  static constexpr uint32_t TTL_MS = 60000;
  static constexpr uint32_t MIN_GAP_MS = 1000;  // per address: one new nonce per second
  // false = refused (rate, or every slot held by other addresses). out = 32 hex chars + terminator.
  bool issue(const uint8_t rnd[16], uint32_t ip, uint32_t now_ms, char out[33]);
  bool consume(const char* hex, uint32_t ip, uint32_t now_ms);  // its address, unexpired, unused -> true, then gone
  int outstanding(uint32_t now_ms);

 private:
  struct Slot {
    bool used;
    uint8_t n[16];
    uint32_t ip, at;
  };
  Slot s_[MAX] = {};
};

// ---------------------------------------------------------------- sessions (RAM only, bound to the client IP)
class SessionStore {
 public:
  static constexpr int MAX = 4;
  static constexpr uint32_t IDLE_S = 30u * 86400u;
  static constexpr size_t UA_MAX = 96;
  struct Info {
    char id[9];  // public handle for the list / revoke (not the token)
    uint32_t ip, created, last;
    char ua[UA_MAX + 1];
  };
  // token_rnd: 16 random bytes (the cookie), id_rnd: 4 random bytes (the public id). Evicts the least recently
  // used session when full. out = 32 hex chars + terminator.
  void create(const uint8_t token_rnd[16], const uint8_t id_rnd[4], uint32_t ip, const char* ua, uint32_t now_s,
              char out[33]);
  // Valid, not idle-expired and presented from the IP it was created on -> refreshes last use. id_out (optional)
  // gets the session's public id.
  bool check(const char* hex, uint32_t ip, uint32_t now_s, char id_out[9] = nullptr);
  bool remove(const char* hex);
  bool revoke(const char* id);
  void clear();
  int count(uint32_t now_s);
  int list(uint32_t now_s, Info* out, int cap);

 private:
  struct Slot {
    bool used;
    uint8_t token[16];
    Info info;
  };
  void expire(uint32_t now_s);
  Slot s_[MAX] = {};
};

// ---------------------------------------------------------------- lockouts (RAM only)
// Per source IP: 5 failed proofs within 60 s -> that IP refused for 60 s.
class Lockout {
 public:
  static constexpr int SLOTS = 8;
  static constexpr int MAX_FAILS = 5;
  static constexpr uint32_t WINDOW_S = 60;
  static constexpr uint32_t LOCK_S = 60;
  bool locked(uint32_t ip, uint32_t now_s, uint32_t* retry_s);
  bool fail(uint32_t ip, uint32_t now_s);  // true when this failure started a lock
  void success(uint32_t ip);
  void clear();

 private:
  struct Slot {
    bool used;
    uint32_t ip;
    uint8_t fails;
    uint32_t first;  // first failure of the current window
    uint32_t until;  // locked while now < until
    uint32_t last;
  };
  Slot* find(uint32_t ip, bool create, uint32_t now_s);
  Slot s_[SLOTS] = {};
};

// Account-wide: 10 failed proofs from any IPs within 10 min -> the admin is locked for 5 min, doubling with each
// further burst (5, 10, 20, 40, 60 cap). A successful login clears everything.
class AccountLock {
 public:
  static constexpr int MAX_FAILS = 10;
  static constexpr uint32_t WINDOW_S = 600;
  static constexpr uint32_t BASE_LOCK_S = 300;
  static constexpr uint32_t MAX_LOCK_S = 3600;
  bool locked(uint32_t now_s, uint32_t* until_s = nullptr) const;
  // Counts a failure; true when it started a lock (*lock_s = its length).
  bool fail(uint32_t now_s, uint32_t* lock_s = nullptr);
  void success();
  // Reports (once) that a lock has run out since the last call -- for the "lock ended" log line.
  bool expiredSince(uint32_t now_s);
  int failures(uint32_t now_s) const;  // in the current window

 private:
  uint32_t times_[MAX_FAILS] = {};  // ring of the last failure times
  int n_ = 0, head_ = 0;
  uint32_t until_ = 0;
  uint32_t next_lock_s_ = BASE_LOCK_S;
  bool was_locked_ = false;
};

// Value of cookie `name` in a Cookie header ("a=1; fcs=abc"); false if absent or longer than cap - 1.
bool cookieValue(const char* header, const char* name, char* out, size_t cap);

}  // namespace fc
