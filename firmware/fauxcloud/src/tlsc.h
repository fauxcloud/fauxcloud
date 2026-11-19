// tlsc.h -- the TLS interface -- tlscGet (connect is the
// caller's socket; verify, get and close happen inside it) and its error class TlscWhy. No BearSSL type here:
// the stack behind it can be swapped in tlsc.cpp + tlsc_trust.h alone. Now: one HTTPS GET over BearSSL (vendored, src/bearssl/),
// TLS 1.2 only, over an injected byte I/O. Pure C++ (no IDF): the host tests drive it against an in-process BearSSL
// server, the board's net-out task over a socket (app_fwd.cpp). BearSSL never allocates: tlscGet allocates three fixed
// blocks per connection (client context, X.509 context, the half-duplex record buffer), wipes and frees them before it
// returns. Linked only by FWD_TLS=1 builds (nothing calls it otherwise).
#pragma once
#include <cstddef>
#include <cstdint>

#include "fwd.h"

namespace fc {

// The half-duplex record buffer: 4 KB of payload + BearSSL's record overhead (BR_SSL_BUFSIZE_INPUT - 16384). The engine asks
// the server for MFL 4096; hosts that ignore it (WU, PWSweather, Windy) reply in 296-453 B records, CDN-fronted bodies in
// ~1.4 KB ones (measured). A record above it ends in `too_large`, never a crash.
constexpr size_t TLSC_BUF_LEN = 4096 + 325;
static_assert(TLSC_BUF_LEN >= 837 && TLSC_BUF_LEN <= 16709, "BearSSL's buffer bounds (512 B .. 16 KB payload)");
// An upper bound of the two contexts (client 3 408 + X.509 3 048 B on xtensa; 3 720 + 3 176 on a 64-bit host),
// static_asserted in tlsc.cpp: the heap gate keeps the shed line's block whole against all three blocks.
constexpr size_t TLSC_CTX_BYTES = 7 * 1024;
constexpr size_t TLSC_BLOCKS_MAX = TLSC_CTX_BYTES + TLSC_BUF_LEN;
struct TlscTrust;  // tlsc_trust.h (BearSSL types): an explicit anchor set, host tests only

// The byte I/O under the engine: >= 1 bytes moved, -1 closed / error / timeout / abort (0 is never returned to BearSSL).
// The device glue checks the 10-s send deadline and the abort flag inside them.
struct TlscIo {
  void* ctx;
  int (*read)(void* ctx, uint8_t* buf, size_t n);
  int (*write)(void* ctx, const uint8_t* buf, size_t n);
};

struct TlscReq {
  const char* host;                         // SNI and the name checked in the certificate (never empty)
  uint16_t roots;                           // the anchors this host may chain to: a FWD_ROOTBIT_* mask (fwd_roots.h; 16 bits)
  const TlscTrust* trust;                   // nullptr = FWD_TAS by `roots`; host tests pass their own set
  uint32_t unix_now;                        // real time only (no held clock); 0 = none -> `clock`, nothing sent
  size_t buf_len;                           // TLSC_BUF_LEN unless a test says otherwise
  size_t read_cap;                          // stop reading after this many bytes of plaintext
  void (*entropy)(void* buf, size_t n);     // 32 B seed: esp_fill_random on the board (Wi-Fi up: a true RNG)
  void* (*alloc)(size_t n);                 // the three blocks (calloc-like); nullptr = calloc
  void (*release)(void* p);                 // nullptr = free
  uint32_t (*now_ms)();                     // handshake timing; nullptr = none
};

// Where a connection ended and why. The device glue sets Dns / Connect / Aborted around the engine.
enum class TlscWhy : uint8_t { Ok, Dns, Connect, Handshake, Untrusted, TooLarge, Read, NoMemory, Clock, Aborted, COUNT };
const char* tlscWhyName(TlscWhy w);  // ok dns connect handshake untrusted too_large read no_memory clock aborted
enum class TlscStage : uint8_t { None, Alloc, Handshake, Data, Done };
const char* tlscStageName(TlscStage s);  // none alloc handshake data done

// The verdict on a BearSSL error, pure: alloc_failed decides before the engine ran (BearSSL never allocates, so no engine
// code is ever no_memory and no allocation failure is ever untrusted); the X.509 trust / validity family is untrusted;
// BR_ERR_TOO_LARGE is too_large; BR_ERR_IO is handshake before the handshake completed, read after; anything else is
// handshake. Ok for BR_ERR_OK.
TlscWhy tlscClassify(TlscStage stage, int br_err, bool alloc_failed);
const char* tlscErrName(int br_err);  // "BR_ERR_IO", "BR_ERR_X509_NOT_TRUSTED", ... "" for an unknown code

// The largest TLS record header seen in a byte stream (the board's answer to "does this host honour MFL"): a 5-byte header
// tracker over arbitrary splits.
struct TlscRecTracker {
  uint8_t hdr[5];
  uint8_t hn = 0;
  uint32_t left = 0;     // body bytes of the current record still to skip
  uint16_t max = 0;      // largest record length seen
  uint16_t app_max = 0;  // the largest application-data record (type 23) -- the one that meets the
                         // buffer once the handshake is done; a large Certificate record (type 22) says nothing of it
  uint16_t count = 0;    // records seen
  void feed(const uint8_t* d, size_t n);
};

struct TlscDiag {
  TlscStage stage;
  int br_err;
  uint16_t version;        // the negotiated protocol (0x0303 = TLS 1.2)
  uint16_t suite;          // the negotiated cipher suite (0 before ServerHello)
  uint8_t curve;           // the ECDHE curve (BR_EC_*: 23 P-256, 24 P-384, 29 X25519)
  uint16_t rec_in_max;     // the largest record the server sent
  uint16_t rec_app_max;    // the largest application-data record of it
  uint32_t ms_hs;          // handshake (reset -> the request flushed)
  bool flushed;            // br_sslio_flush returned 0 -- the request left BearSSL whole
};

struct TlscResult {
  TlscWhy why;
  bool response;           // the response parser said done (a status line and what the target needs)
};

// One GET: allocate -> profile -> entropy -> time -> reset(host) -> write req + flush -> read into `resp` until it is done or
// read_cap -> close_notify (best effort, no wait) -> wipe + free. A server that closes without close_notify after a complete
// response is a success.
TlscResult tlscGet(const TlscReq& q, const TlscIo& io, const char* req, size_t req_len, FwdResp& resp, TlscDiag& d);

// sizeof of the three blocks for this buffer (for the heap model and the logs)
size_t tlscBlockBytes(size_t buf_len);

}  // namespace fc
