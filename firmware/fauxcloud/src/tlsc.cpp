// tlsc.cpp -- see tlsc.h.
#include "tlsc.h"

#include <cstdlib>
#include <cstring>

#include "bearssl/bearssl.h"
#include "fwd_roots.h"
#include "tlsc_trust.h"

namespace fc {

static_assert(sizeof(br_ssl_client_context) + sizeof(br_x509_minimal_context) <= TLSC_CTX_BYTES,
              "TLSC_CTX_BYTES bounds the two contexts (the heap gate counts on it)");

static const char* const WHY_NAMES[(int)TlscWhy::COUNT] = {"ok",        "dns",  "connect",   "handshake", "untrusted",
                                                           "too_large", "read", "no_memory", "clock",     "aborted"};
const char* tlscWhyName(TlscWhy w) { return (int)w < (int)TlscWhy::COUNT ? WHY_NAMES[(int)w] : ""; }
static const char* const STAGE_NAMES[] = {"none", "alloc", "handshake", "data", "done"};
const char* tlscStageName(TlscStage s) { return (int)s <= (int)TlscStage::Done ? STAGE_NAMES[(int)s] : ""; }

TlscWhy tlscClassify(TlscStage stage, int e, bool alloc_failed) {
  if (alloc_failed) return TlscWhy::NoMemory;  // before the engine ran: BearSSL itself never allocates
  switch (e) {
    case BR_ERR_OK: return TlscWhy::Ok;
    // the X.509 trust / validity family: the chain does not end in our anchors, or not for this name, or not now
    case BR_ERR_X509_NOT_TRUSTED:
    case BR_ERR_X509_BAD_SERVER_NAME:
    case BR_ERR_X509_EXPIRED:
    case BR_ERR_X509_NOT_CA:
    case BR_ERR_X509_FORBIDDEN_KEY_USAGE:
    case BR_ERR_X509_WEAK_PUBLIC_KEY:
    case BR_ERR_X509_DN_MISMATCH: return TlscWhy::Untrusted;
    case BR_ERR_TOO_LARGE: return TlscWhy::TooLarge;
    case BR_ERR_IO: return stage == TlscStage::Data ? TlscWhy::Read : TlscWhy::Handshake;
    default: return TlscWhy::Handshake;  // alerts, unsupported version / suite, bad MAC, the other X.509 codes...
  }
}

#define E(x) {x, #x}
static const struct {
  int code;
  const char* name;
} ERR_NAMES[] = {
    E(BR_ERR_OK), E(BR_ERR_BAD_PARAM), E(BR_ERR_BAD_STATE), E(BR_ERR_UNSUPPORTED_VERSION), E(BR_ERR_BAD_VERSION),
    E(BR_ERR_BAD_LENGTH), E(BR_ERR_TOO_LARGE), E(BR_ERR_BAD_MAC), E(BR_ERR_NO_RANDOM), E(BR_ERR_UNKNOWN_TYPE),
    E(BR_ERR_UNEXPECTED), E(BR_ERR_BAD_CCS), E(BR_ERR_BAD_ALERT), E(BR_ERR_BAD_HANDSHAKE), E(BR_ERR_OVERSIZED_ID),
    E(BR_ERR_BAD_CIPHER_SUITE), E(BR_ERR_BAD_COMPRESSION), E(BR_ERR_BAD_FRAGLEN), E(BR_ERR_BAD_SECRENEG),
    E(BR_ERR_EXTRA_EXTENSION), E(BR_ERR_BAD_SNI), E(BR_ERR_BAD_HELLO_DONE), E(BR_ERR_LIMIT_EXCEEDED),
    E(BR_ERR_BAD_FINISHED), E(BR_ERR_RESUME_MISMATCH), E(BR_ERR_INVALID_ALGORITHM), E(BR_ERR_BAD_SIGNATURE),
    E(BR_ERR_WRONG_KEY_USAGE), E(BR_ERR_NO_CLIENT_AUTH), E(BR_ERR_IO), E(BR_ERR_X509_OK), E(BR_ERR_X509_INVALID_VALUE),
    E(BR_ERR_X509_TRUNCATED), E(BR_ERR_X509_EMPTY_CHAIN), E(BR_ERR_X509_INNER_TRUNC), E(BR_ERR_X509_BAD_TAG_CLASS),
    E(BR_ERR_X509_BAD_TAG_VALUE), E(BR_ERR_X509_INDEFINITE_LENGTH), E(BR_ERR_X509_EXTRA_ELEMENT),
    E(BR_ERR_X509_UNEXPECTED), E(BR_ERR_X509_NOT_CONSTRUCTED), E(BR_ERR_X509_NOT_PRIMITIVE),
    E(BR_ERR_X509_PARTIAL_BYTE), E(BR_ERR_X509_BAD_BOOLEAN), E(BR_ERR_X509_OVERFLOW), E(BR_ERR_X509_BAD_DN),
    E(BR_ERR_X509_BAD_TIME), E(BR_ERR_X509_UNSUPPORTED), E(BR_ERR_X509_LIMIT_EXCEEDED), E(BR_ERR_X509_WRONG_KEY_TYPE),
    E(BR_ERR_X509_BAD_SIGNATURE), E(BR_ERR_X509_TIME_UNKNOWN), E(BR_ERR_X509_EXPIRED), E(BR_ERR_X509_DN_MISMATCH),
    E(BR_ERR_X509_BAD_SERVER_NAME), E(BR_ERR_X509_CRITICAL_EXTENSION), E(BR_ERR_X509_NOT_CA),
    E(BR_ERR_X509_FORBIDDEN_KEY_USAGE), E(BR_ERR_X509_WEAK_PUBLIC_KEY), E(BR_ERR_X509_NOT_TRUSTED),
};
#undef E
const char* tlscErrName(int e) {
  if (e >= BR_ERR_SEND_FATAL_ALERT && e < BR_ERR_SEND_FATAL_ALERT + 256) return "BR_ERR_SEND_FATAL_ALERT";
  if (e >= BR_ERR_RECV_FATAL_ALERT && e < BR_ERR_RECV_FATAL_ALERT + 256) return "BR_ERR_RECV_FATAL_ALERT";
  for (const auto& x : ERR_NAMES)
    if (x.code == e) return x.name;
  return "";
}

void TlscRecTracker::feed(const uint8_t* d, size_t n) {
  while (n) {
    if (left) {
      const size_t k = left < n ? left : n;
      left -= (uint32_t)k, d += k, n -= k;
      continue;
    }
    hdr[hn++] = *d++;
    n--;
    if (hn == 5) {
      const uint16_t len = (uint16_t)((hdr[3] << 8) | hdr[4]);
      if (len > max) max = len;
      if (hdr[0] == 23 && len > app_max) app_max = len;  // application data
      count++;
      left = len;
      hn = 0;
    }
  }
}

size_t tlscBlockBytes(size_t buf_len) {
  return sizeof(br_ssl_client_context) + sizeof(br_x509_minimal_context) + buf_len;
}

namespace {
struct Pump {
  const TlscIo* io;
  TlscRecTracker tr;
};
int lowRead(void* c, unsigned char* buf, size_t n) {
  Pump* p = (Pump*)c;
  const int r = p->io->read(p->io->ctx, buf, n);
  if (r <= 0) return -1;  // BearSSL takes -1 for a closed stream too
  p->tr.feed(buf, (size_t)r);
  return r;
}
int lowWrite(void* c, const unsigned char* buf, size_t n) {
  Pump* p = (Pump*)c;
  const int r = p->io->write(p->io->ctx, buf, n);
  return r <= 0 ? -1 : r;
}
void wipe(void* p, size_t n) {  // through volatile: the session keys and the request (with the key) never stay in RAM
  volatile unsigned char* v = (volatile unsigned char*)p;
  while (n--) *v++ = 0;
}

// TLS 1.2, ECDHE only (forward secrecy), ECDSA or RSA server keys; AES-GCM and ChaCha20-Poly1305.
const uint16_t SUITES[] = {
    BR_TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256,       BR_TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256,
    BR_TLS_ECDHE_ECDSA_WITH_AES_256_GCM_SHA384,       BR_TLS_ECDHE_RSA_WITH_AES_256_GCM_SHA384,
    BR_TLS_ECDHE_ECDSA_WITH_CHACHA20_POLY1305_SHA256, BR_TLS_ECDHE_RSA_WITH_CHACHA20_POLY1305_SHA256,
};

// The profile, named explicitly (no *_default helpers): the host tests run the same primitives as the board.
void profile(br_ssl_client_context* cc, br_x509_minimal_context* xc, const TlscReq& q, const br_x509_trust_anchor* tas,
             size_t n_tas) {
  br_ssl_engine_context* eng = &cc->eng;
  br_ssl_client_zero(cc);
  br_ssl_engine_set_versions(eng, BR_TLS12, BR_TLS12);
  br_ssl_engine_add_flags(eng, BR_OPT_NO_RENEGOTIATION);  // a server's HelloRequest is refused
  br_ssl_engine_set_suites(eng, SUITES, sizeof SUITES / sizeof SUITES[0]);
  br_ssl_engine_set_hash(eng, br_sha256_ID, &br_sha256_vtable);
  br_ssl_engine_set_hash(eng, br_sha384_ID, &br_sha384_vtable);
  br_ssl_engine_set_prf_sha256(eng, &br_tls12_sha256_prf);
  br_ssl_engine_set_prf_sha384(eng, &br_tls12_sha384_prf);
  br_ssl_engine_set_ec(eng, &br_ec_all_m31);  // P-256 / P-384 / P-521, X25519
  br_ssl_engine_set_rsavrfy(eng, &br_rsa_i31_pkcs1_vrfy);
  br_ssl_engine_set_ecdsa(eng, &br_ecdsa_i31_vrfy_asn1);
  br_ssl_engine_set_aes_ctr(eng, &br_aes_ct_ctr_vtable);
  br_ssl_engine_set_ghash(eng, &br_ghash_ctmul);
  br_ssl_engine_set_gcm(eng, &br_sslrec_in_gcm_vtable, &br_sslrec_out_gcm_vtable);
  br_ssl_engine_set_chacha20(eng, &br_chacha20_ct_run);
  br_ssl_engine_set_poly1305(eng, &br_poly1305_ctmul_run);
  br_ssl_engine_set_chapol(eng, &br_sslrec_in_chapol_vtable, &br_sslrec_out_chapol_vtable);
  br_x509_minimal_init(xc, &br_sha256_vtable, tas, n_tas);
  br_x509_minimal_set_hash(xc, br_sha256_ID, &br_sha256_vtable);
  br_x509_minimal_set_hash(xc, br_sha384_ID, &br_sha384_vtable);
  br_x509_minimal_set_rsa(xc, &br_rsa_i31_pkcs1_vrfy);
  br_x509_minimal_set_ecdsa(xc, &br_ec_all_m31, &br_ecdsa_i31_vrfy_asn1);
  // days since 0000-01-01 (proleptic Gregorian): 719528 = 1970-01-01
  br_x509_minimal_set_time(xc, q.unix_now / 86400 + 719528u, q.unix_now % 86400);
  br_ssl_engine_set_x509(eng, &xc->vtable);
}
}  // namespace

TlscResult tlscGet(const TlscReq& q, const TlscIo& io, const char* req, size_t req_len, FwdResp& resp, TlscDiag& d) {
  memset(&d, 0, sizeof d);
  if (!q.unix_now) return {TlscWhy::Clock, false};  // no real time: validity cannot be checked, nothing is sent
  // an empty name would switch the name check off (x509_minimal: "" -> no server name)
  if (!q.host || !q.host[0]) return {TlscWhy::Untrusted, false};
  // the anchors: an explicit set (host tests) or FWD_TAS by the mask (copies of ~30-B structs; they live as long as the call)
  br_x509_trust_anchor masked[FWD_ROOT_COUNT];
  const br_x509_trust_anchor* tas = masked;
  size_t n_tas = 0;
  if (q.trust) {
    tas = q.trust->tas, n_tas = q.trust->n;
  } else {
    for (int i = 0; i < FWD_ROOT_COUNT; i++)
      if ((q.roots >> i) & 1u) masked[n_tas++] = FWD_TAS[i];
  }
  void* (*alloc)(size_t) = q.alloc ? q.alloc : [](size_t n) { return calloc(1, n); };
  void (*release)(void*) = q.release ? q.release : free;
  d.stage = TlscStage::Alloc;
  const size_t bl = q.buf_len ? q.buf_len : TLSC_BUF_LEN;
  // the largest block first: the record buffer, then the two contexts
  auto* buf = (unsigned char*)alloc(bl);
  auto* cc = buf ? (br_ssl_client_context*)alloc(sizeof(br_ssl_client_context)) : nullptr;
  auto* xc = cc ? (br_x509_minimal_context*)alloc(sizeof(br_x509_minimal_context)) : nullptr;
  if (!xc) {  // decided before the engine runs (never inferred from an engine code)
    if (cc) release(cc);
    if (buf) release(buf);
    return {TlscWhy::NoMemory, false};
  }
  profile(cc, xc, q, tas, n_tas);
  br_ssl_engine_context* eng = &cc->eng;
  br_ssl_engine_set_buffer(eng, buf, bl, 0);  // half-duplex: one buffer for both directions
  if (q.entropy) {  // no system RNG on xtensa: without it the reset fails with BR_ERR_NO_RANDOM
    uint8_t seed[32];
    q.entropy(seed, sizeof seed);
    br_ssl_engine_inject_entropy(eng, seed, sizeof seed);
    wipe(seed, sizeof seed);
  }
  Pump p{&io, {}};
  br_sslio_context ioc;
  TlscWhy why = TlscWhy::Ok;
  bool done = false;
  d.stage = TlscStage::Handshake;
  const uint32_t t0 = q.now_ms ? q.now_ms() : 0;
  if (!br_ssl_client_reset(cc, q.host, 0)) {  // SNI = host, no resumption
    d.br_err = br_ssl_engine_last_error(eng);
    why = tlscClassify(d.stage, d.br_err, false);
  } else {
    br_sslio_init(&ioc, eng, lowRead, &p, lowWrite, &p);
    // the request can only go once the handshake is over: write_all returning 0 means it is
    if (br_sslio_write_all(&ioc, req, req_len) == 0) {
      d.stage = TlscStage::Data;
      d.ms_hs = q.now_ms ? q.now_ms() - t0 : 0;
      // never br_ssl_engine_get_session_parameters -- that copies the master secret onto this stack
      d.version = (uint16_t)br_ssl_engine_get_version(eng);
      d.suite = eng->session.cipher_suite;
      d.curve = (uint8_t)br_ssl_engine_get_ecdhe_curve(eng);
      if (br_sslio_flush(&ioc) == 0) {
        d.flushed = true;  // stage Data alone means "buffered in BearSSL"; this is "sent"
        uint8_t tmp[256];
        size_t total = 0;
        while (!resp.done() && total < q.read_cap) {
          const size_t want = q.read_cap - total < sizeof tmp ? q.read_cap - total : sizeof tmp;
          const int r = br_sslio_read(&ioc, tmp, want);
          if (r < 0) break;  // closed (close_notify or a plain close) or an error: decided below
          total += (size_t)r;
          resp.feed((const char*)tmp, (size_t)r);
        }
        wipe(tmp, sizeof tmp);
      }
    }
    d.br_err = br_ssl_engine_last_error(eng);
    // the end of the stream: a server that closes after a complete reply without close_notify (BR_ERR_IO) is fine
    if (!resp.done() && d.stage == TlscStage::Data && (d.br_err == BR_ERR_OK || d.br_err == BR_ERR_IO) &&
        resp.status() >= 0)
      resp.end();
    done = resp.done();
    why = done ? TlscWhy::Ok : tlscClassify(d.stage, d.br_err == BR_ERR_OK ? BR_ERR_IO : d.br_err, false);
    // close_notify, best effort: send what the engine has, never wait for the peer's
    br_ssl_engine_close(eng);
    for (int k = 0; k < 4 && (br_ssl_engine_current_state(eng) & BR_SSL_SENDREC); k++) {
      size_t len = 0;
      unsigned char* b = br_ssl_engine_sendrec_buf(eng, &len);
      const int w = lowWrite(&p, b, len);
      if (w <= 0) break;
      br_ssl_engine_sendrec_ack(eng, (size_t)w);
    }
  }
  if (done) d.stage = TlscStage::Done;
  d.rec_in_max = p.tr.max;
  d.rec_app_max = p.tr.app_max;
  wipe(buf, bl);
  wipe(xc, sizeof *xc);
  wipe(cc, sizeof *cc);
  release(buf);
  release(xc);
  release(cc);
  return {why, done};
}

}  // namespace fc
