// test_tlsc.inc -- src/tlsc.cpp (our BearSSL client, the vendored engine, the same explicit profile as the
// board) against an in-process BearSSL server, driven inside the client's TlscIo callbacks: a byte pump, no sockets,
// deterministic. The test PKI is generated when the tests run -- an RSA-2048 root A (the RSA chain check the
// presets use), an unrelated EC root B, a P-256 server key and its test.example certificate signed by A -- so no private
// key is ever committed. Plus the error table, the record tracker, the generated anchors against the certificates, and
// the scheduler's TLS gate per class. BearSSL is compiled as C for the host with every OS entropy / time
// source and the 64-bit / unaligned paths off, like on the xtensa target.
#include <functional>

#include "bearssl/bearssl.h"
#include "fwd_roots.h"
#include "tlsc.h"
#include "tlsc_trust.h"

#include "fwd_roots_der.inc"

namespace {

using Bytes = std::vector<unsigned char>;
Bytes der(uint8_t tag, const Bytes& v) {
  Bytes o{tag};
  const size_t n = v.size();
  if (n < 128) {
    o.push_back((unsigned char)n);
  } else if (n < 256) {
    o.push_back(0x81), o.push_back((unsigned char)n);
  } else {
    o.push_back(0x82), o.push_back((unsigned char)(n >> 8)), o.push_back((unsigned char)n);
  }
  o.insert(o.end(), v.begin(), v.end());
  return o;
}
Bytes cat(std::initializer_list<Bytes> l) {
  Bytes o;
  for (const Bytes& b : l) o.insert(o.end(), b.begin(), b.end());
  return o;
}
Bytes str(const char* s) { return Bytes(s, s + strlen(s)); }
Bytes name(const char* cn) {  // SEQ { SET { SEQ { OID commonName, UTF8String } } }
  return der(0x30, der(0x31, der(0x30, cat({der(0x06, {0x55, 0x04, 0x03}), der(0x0C, str(cn))}))));
}
Bytes bits(const Bytes& v) { return der(0x03, cat({{0x00}, v})); }

// The test PKI, generated once per run from a fixed DRBG seed (deterministic, never written anywhere)
struct TestPki {
  Bytes dnA, dnB, nA, eA, qB, cert;
  br_x509_trust_anchor taA{}, taB{};
  unsigned char ec_priv[BR_EC_KBUF_PRIV_MAX_SIZE];
  br_ec_private_key sk{};
  bool ok = false;
  TestPki() {
    br_hmac_drbg_context rng;
    static const char seed[] = "fauxcloud host tests: test PKI (generated at run time, never committed)";
    br_hmac_drbg_init(&rng, &br_sha256_vtable, seed, sizeof seed);
    // root A: RSA-2048
    std::vector<unsigned char> kpriv(BR_RSA_KBUF_PRIV_SIZE(2048)), kpub(BR_RSA_KBUF_PUB_SIZE(2048));
    br_rsa_private_key rsk;
    br_rsa_public_key rpk;
    if (!br_rsa_i31_keygen(&rng.vtable, &rsk, kpriv.data(), &rpk, kpub.data(), 2048, 65537)) return;
    dnA = name("fauxcloud test root A");
    nA.assign(rpk.n, rpk.n + rpk.nlen);
    eA.assign(rpk.e, rpk.e + rpk.elen);
    taA.dn = {dnA.data(), dnA.size()};
    taA.flags = BR_X509_TA_CA;
    taA.pkey.key_type = BR_KEYTYPE_RSA;
    taA.pkey.key.rsa = {nA.data(), nA.size(), eA.data(), eA.size()};
    // root B: an unrelated EC key (only its anchor exists)
    unsigned char bpriv[BR_EC_KBUF_PRIV_MAX_SIZE], bpub[BR_EC_KBUF_PUB_MAX_SIZE];
    br_ec_private_key bsk;
    br_ec_public_key bpk;
    if (!br_ec_keygen(&rng.vtable, &br_ec_all_m31, &bsk, bpriv, BR_EC_secp256r1) ||
        !br_ec_compute_pub(&br_ec_all_m31, &bpk, bpub, &bsk))
      return;
    dnB = name("fauxcloud test root B");
    qB.assign(bpk.q, bpk.q + bpk.qlen);
    taB.dn = {dnB.data(), dnB.size()};
    taB.flags = BR_X509_TA_CA;
    taB.pkey.key_type = BR_KEYTYPE_EC;
    taB.pkey.key.ec = {BR_EC_secp256r1, qB.data(), qB.size()};
    // the server: P-256, test.example, 2026-01-01 .. 2036-01-01, signed by A (sha256WithRSAEncryption)
    unsigned char spub[BR_EC_KBUF_PUB_MAX_SIZE];
    br_ec_public_key spk;
    if (!br_ec_keygen(&rng.vtable, &br_ec_all_m31, &sk, ec_priv, BR_EC_secp256r1) ||
        !br_ec_compute_pub(&br_ec_all_m31, &spk, spub, &sk))
      return;
    const Bytes sigalg = der(0x30, cat({der(0x06, {0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x01, 0x01, 0x0B}), der(0x05, {})}));
    const Bytes spki = der(0x30, cat({der(0x30, cat({der(0x06, {0x2A, 0x86, 0x48, 0xCE, 0x3D, 0x02, 0x01}),
                                                    der(0x06, {0x2A, 0x86, 0x48, 0xCE, 0x3D, 0x03, 0x01, 0x07})})),
                                      bits(Bytes(spk.q, spk.q + spk.qlen))}));
    const Bytes san = der(0x30, cat({der(0x06, {0x55, 0x1D, 0x11}), der(0x04, der(0x30, der(0x82, str("test.example"))))}));
    const Bytes ku = der(0x30, cat({der(0x06, {0x55, 0x1D, 0x0F}), der(0x01, {0xFF}), der(0x04, der(0x03, {0x07, 0x80}))}));
    const Bytes tbs = der(0x30, cat({der(0xA0, der(0x02, {0x02})), der(0x02, {0x01, 0x01}), sigalg, dnA,
                                     der(0x30, cat({der(0x17, str("260101000000Z")), der(0x17, str("360101000000Z"))})),
                                     name("test.example"), spki, der(0xA3, der(0x30, cat({san, ku})))}));
    unsigned char h[32];
    br_sha256_context hc;
    br_sha256_init(&hc);
    br_sha256_update(&hc, tbs.data(), tbs.size());
    br_sha256_out(&hc, h);
    Bytes sig(256);
    if (!br_rsa_i31_pkcs1_sign(BR_HASH_OID_SHA256, h, sizeof h, &rsk, sig.data())) return;
    cert = der(0x30, cat({tbs, sigalg, bits(sig)}));
    ok = true;
  }
};
const TestPki& pki() {
  static TestPki p;
  return p;
}

// unix seconds of a date (proleptic Gregorian), for the validity window of the fixtures (2026-01-01 .. 2036-01-01)
uint32_t unixOf(int y, int m, int d) {
  y -= m <= 2;
  const int era = y / 400, yoe = y - era * 400, doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return (uint32_t)((era * 146097 + doe - 719468) * 86400LL);
}

// A trust anchor built from a DER certificate with BearSSL's own decoder (the fixtures' roots; section 6 for ours)
struct DecodedTa {
  std::vector<unsigned char> dn, n, e, q;
  br_x509_trust_anchor ta{};
  bool ok = false;
  explicit DecodedTa(const unsigned char* der, size_t len) {
    br_x509_decoder_context dc;
    br_x509_decoder_init(&dc, [](void* ctx, const void* buf, size_t l) {
      auto* v = (std::vector<unsigned char>*)ctx;
      v->insert(v->end(), (const unsigned char*)buf, (const unsigned char*)buf + l);
    }, &dn);
    br_x509_decoder_push(&dc, der, len);
    const br_x509_pkey* pk = br_x509_decoder_get_pkey(&dc);
    if (!pk || br_x509_decoder_last_error(&dc)) return;
    ta.dn.data = dn.data();
    ta.dn.len = dn.size();
    ta.flags = BR_X509_TA_CA;
    ta.pkey.key_type = pk->key_type;
    if (pk->key_type == BR_KEYTYPE_RSA) {
      n.assign(pk->key.rsa.n, pk->key.rsa.n + pk->key.rsa.nlen);
      e.assign(pk->key.rsa.e, pk->key.rsa.e + pk->key.rsa.elen);
      ta.pkey.key.rsa = {n.data(), n.size(), e.data(), e.size()};
    } else {
      q.assign(pk->key.ec.q, pk->key.ec.q + pk->key.ec.qlen);
      ta.pkey.key.ec = {pk->key.ec.curve, q.data(), q.size()};
    }
    dn.shrink_to_fit();
    ta.dn.data = dn.data();
    ok = true;
  }
};

// The in-process server: BearSSL's server engine with one EC certificate (root A -> test.example), ECDHE_ECDSA suites.
struct TlsServer {
  br_ssl_server_context sc;
  std::vector<unsigned char> buf = std::vector<unsigned char>(BR_SSL_BUFSIZE_BIDI);
  br_x509_certificate chain[1];
  br_ec_private_key sk;
  std::string req, head, body;
  size_t head_sent = 0, body_sent = 0;
  bool responded = false, close_notify = true, plain_closed = false, big_records = false, close_before_status = false;
  bool stall_reply = false;       // the request arrives, the reply never does (a deterministic read failure)
  bool renegotiate = false;       // ask the client for a new handshake once the request is in
  int cli_hs_records = 0;         // handshake records the client sent (a refused renegotiation adds none)
  uint8_t w_hdr[5] = {};          // the client's record headers
  int w_hn = 0;
  uint32_t w_left = 0;
  size_t max_chunk = SIZE_MAX;   // bytes per client read (splits)
  int stop_after_reads = -1;     // the pump stops (a timeout) after this many client reads
  int reads = 0, writes = 0;
  explicit TlsServer(uint16_t only_suite = 0) {
    static const uint16_t SUITES[] = {BR_TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256, BR_TLS_ECDHE_ECDSA_WITH_AES_256_GCM_SHA384,
                                      BR_TLS_ECDHE_ECDSA_WITH_CHACHA20_POLY1305_SHA256};
    chain[0] = {(unsigned char*)pki().cert.data(), pki().cert.size()};
    sk = pki().sk;
    br_ssl_server_zero(&sc);
    br_ssl_engine_context* e = &sc.eng;
    br_ssl_engine_set_versions(e, BR_TLS12, BR_TLS12);
    if (only_suite) br_ssl_engine_set_suites(e, &only_suite, 1);  // AES-256-GCM / ChaCha20 runs
    else br_ssl_engine_set_suites(e, SUITES, 3);
    br_ssl_engine_set_hash(e, br_sha256_ID, &br_sha256_vtable);
    br_ssl_engine_set_hash(e, br_sha384_ID, &br_sha384_vtable);
    br_ssl_engine_set_prf_sha256(e, &br_tls12_sha256_prf);
    br_ssl_engine_set_prf_sha384(e, &br_tls12_sha384_prf);
    br_ssl_engine_set_ec(e, &br_ec_all_m31);
    br_ssl_engine_set_aes_ctr(e, &br_aes_ct_ctr_vtable);
    br_ssl_engine_set_ghash(e, &br_ghash_ctmul);
    br_ssl_engine_set_gcm(e, &br_sslrec_in_gcm_vtable, &br_sslrec_out_gcm_vtable);
    br_ssl_engine_set_chacha20(e, &br_chacha20_ct_run);
    br_ssl_engine_set_poly1305(e, &br_poly1305_ctmul_run);
    br_ssl_engine_set_chapol(e, &br_sslrec_in_chapol_vtable, &br_sslrec_out_chapol_vtable);
    br_ssl_server_set_single_ec(&sc, chain, 1, &sk, BR_KEYTYPE_KEYX | BR_KEYTYPE_SIGN, BR_KEYTYPE_RSA, &br_ec_all_m31,
                                &br_ecdsa_i31_sign_asn1);
    br_ssl_engine_set_buffer(e, buf.data(), buf.size(), 1);
    static const char seed[] = "fauxcloud host test server seed (fixed, test only)";
    br_ssl_engine_inject_entropy(e, seed, sizeof seed);
    br_ssl_server_reset(&sc);
    head = "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: 8\r\nConnection: close\r\n\r\n";
    body = "success\n";
  }
  bool closed() const { return plain_closed || (br_ssl_engine_current_state(&sc.eng) & BR_SSL_CLOSED); }
  void step() {  // run the server's application side until it has nothing to do
    br_ssl_engine_context* e = &sc.eng;
    for (int guard = 0; guard < 1000; guard++) {
      const unsigned st = br_ssl_engine_current_state(e);
      if (st & BR_SSL_CLOSED) return;
      if (st & BR_SSL_RECVAPP) {
        size_t len = 0;
        unsigned char* b = br_ssl_engine_recvapp_buf(e, &len);
        req.append((const char*)b, len);
        br_ssl_engine_recvapp_ack(e, len);
        if (!responded && req.find("\r\n\r\n") != std::string::npos) {
          responded = true;
          if (close_before_status) {
            br_ssl_engine_close(e);
            return;
          }
          if (renegotiate) br_ssl_engine_renegotiate(e);
        }
        continue;
      }
      if (responded && (st & BR_SSL_SENDAPP) && (head_sent < head.size() || body_sent < body.size())) {
        std::string& s = head_sent < head.size() ? head : body;
        size_t& sent = head_sent < head.size() ? head_sent : body_sent;
        size_t len = 0;
        unsigned char* b = br_ssl_engine_sendapp_buf(e, &len);
        const size_t k = std::min(len, s.size() - sent);
        memcpy(b, s.data() + sent, k);
        br_ssl_engine_sendapp_ack(e, k);
        sent += k;
        if (&s == &head && sent == head.size()) {
          br_ssl_engine_flush(e, 0);  // the head in a record of its own
          if (big_records) e->max_frag_len = 16384;  // a server that ignores the client's MFL (BearSSL's own honours it)
        }
        if (head_sent == head.size() && body_sent == body.size()) {
          br_ssl_engine_flush(e, 0);
          if (close_notify) br_ssl_engine_close(e);
        }
        continue;
      }
      return;
    }
  }
  static int cliRead(void* ctx, uint8_t* out, size_t n) {
    TlsServer* s = (TlsServer*)ctx;
    if (s->stop_after_reads >= 0 && s->reads >= s->stop_after_reads) return -1;
    s->reads++;
    s->step();
    if (s->stall_reply && s->responded) return -1;  // the request is in, the reply never comes: a timeout
    br_ssl_engine_context* e = &s->sc.eng;
    if (br_ssl_engine_current_state(e) & BR_SSL_SENDREC) {
      size_t len = 0;
      unsigned char* b = br_ssl_engine_sendrec_buf(e, &len);
      const size_t k = std::min(std::min(len, n), s->max_chunk);
      memcpy(out, b, k);
      br_ssl_engine_sendrec_ack(e, k);
      return (int)k;
    }
    // nothing to send: after a complete reply without close_notify the transport closes; otherwise it would stall
    if (s->responded && s->head_sent == s->head.size() && s->body_sent == s->body.size()) s->plain_closed = true;
    return -1;
  }
  static int cliWrite(void* ctx, const uint8_t* in, size_t n) {
    TlsServer* s = (TlsServer*)ctx;
    s->writes++;
    // count the client's handshake records (type 22): a renegotiation would add a new ClientHello
    for (size_t i = 0; i < n; i++) {
      if (s->w_left) {
        s->w_left--;
        continue;
      }
      s->w_hdr[s->w_hn++] = in[i];
      if (s->w_hn == 5) {
        if (s->w_hdr[0] == 22) s->cli_hs_records++;
        s->w_left = (uint32_t)((s->w_hdr[3] << 8) | s->w_hdr[4]);
        s->w_hn = 0;
      }
    }
    br_ssl_engine_context* e = &s->sc.eng;
    size_t done = 0;
    while (done < n) {
      if (br_ssl_engine_current_state(e) & BR_SSL_CLOSED) return done ? (int)done : -1;
      size_t len = 0;
      unsigned char* b = br_ssl_engine_recvrec_buf(e, &len);
      if (!b || !len) {
        s->step();
        if (!(br_ssl_engine_current_state(e) & BR_SSL_RECVREC)) break;
        continue;
      }
      const size_t k = std::min(len, n - done);
      memcpy(b, in + done, k);
      br_ssl_engine_recvrec_ack(e, k);
      done += k;
      s->step();
    }
    return done ? (int)done : -1;
  }
};

// Counting / instrumented allocator: fail the k-th allocation; check every block was zeroed before free
struct AllocSpy {
  static int fail_at, count;
  static std::map<void*, size_t> live;
  static bool all_wiped;
  static void* alloc(size_t n) {
    if (++count == fail_at) return nullptr;
    void* p = calloc(1, n);
    live[p] = n;
    return p;
  }
  static void release(void* p) {
    auto it = live.find(p);
    if (it != live.end()) {
      const unsigned char* b = (const unsigned char*)p;
      for (size_t i = 0; i < it->second; i++)
        if (b[i]) {
          all_wiped = false;
          break;
        }
      live.erase(it);
    }
    free(p);
  }
  static void reset(int f = 0) {
    fail_at = f, count = 0, all_wiped = true;
    live.clear();
  }
};
int AllocSpy::fail_at = 0, AllocSpy::count = 0;
std::map<void*, size_t> AllocSpy::live;
bool AllocSpy::all_wiped = true;

void testEntropy(void* b, size_t n) {
  for (size_t i = 0; i < n; i++) ((uint8_t*)b)[i] = (uint8_t)(0x5A ^ i);  // fixed: deterministic runs
}

struct TlsRun {
  TlscResult r;
  TlscDiag d;
  FwdResp resp;
};

TlsRun tlsRun(TlsServer& s, const br_x509_trust_anchor* tas, size_t n_tas, const char* host = "test.example",
              uint32_t now = 0, size_t buf_len = TLSC_BUF_LEN, const std::string& request = "", FwdType type = FwdType::Template,
              const char* success = "", bool entropy = true) {
  TlsRun x;
  TlscReq q = {};
  static TlscTrust trust;  // the run's anchors (host tests only: tlsc_trust.h)
  trust = {tas, n_tas};
  q.host = host;
  q.trust = &trust;
  q.unix_now = now ? now : unixOf(2026, 11, 13);
  q.buf_len = buf_len;
  q.read_cap = FwdResp::CAP_TOTAL;
  q.entropy = entropy ? testEntropy : nullptr;
  q.alloc = AllocSpy::alloc;
  q.release = AllocSpy::release;
  const TlscIo io{&s, TlsServer::cliRead, TlsServer::cliWrite};
  const std::string req = request.empty() ? std::string("GET /x?id=EXAMPLE1&key=examplekey HTTP/1.1\r\nHost: ") + host +
                                                "\r\nConnection: close\r\n\r\n"
                                          : request;
  x.resp.begin(type, success);
  x.r = tlscGet(q, io, req.data(), req.size(), x.resp, x.d);
  return x;
}

}  // namespace

static void testTlsc() {
  CHECK(pki().ok);
  if (!pki().ok) return;
  struct {
    br_x509_trust_anchor ta;
  } ra{pki().taA}, rb{pki().taB};
  // 1. engine glue: a GET through the three buffer sizes; the request reaches the server whole; the reply parsed
  for (size_t bl : {(size_t)837, (size_t)2373, TLSC_BUF_LEN}) {
    TlsServer s;
    AllocSpy::reset();
    TlsRun x = tlsRun(s, &ra.ta, 1, "test.example", 0, bl);
    CHECK(x.r.why == TlscWhy::Ok && x.r.response && x.resp.result() == FwdResult::Ok && x.resp.status() == 200);
    CHECK(s.req.find("GET /x?id=EXAMPLE1&key=examplekey HTTP/1.1\r\n") == 0);
    CHECK(x.d.stage == TlscStage::Done && x.d.suite == BR_TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256 && x.d.rec_in_max > 0);
    CHECK(x.d.flushed);  // the request left BearSSL
    CHECK(x.d.version == BR_TLS12 && x.d.curve == BR_EC_curve25519);  // both ends prefer X25519 (BearSSL's order)
    CHECK(AllocSpy::count == 3 && AllocSpy::live.empty() && AllocSpy::all_wiped);  // three blocks, wiped, freed
    if (bl == 837) CHECK(x.d.rec_in_max <= 512 + 64);  // MFL 512 asked and honoured by BearSSL's server
  }
  {  // a request larger than one record at 837 B (512 B of payload per record)
    TlsServer s;
    std::string big = "GET /" + std::string(1500, 'a') + " HTTP/1.1\r\nHost: test.example\r\n\r\n";
    TlsRun x = tlsRun(s, &ra.ta, 1, "test.example", 0, 837, big);
    CHECK(x.r.why == TlscWhy::Ok && s.req == big);
  }
  {  // the reply split across many reads (7 bytes at a time)
    TlsServer s;
    s.max_chunk = 7;
    TlsRun x = tlsRun(s, &ra.ta, 1);
    CHECK(x.r.why == TlscWhy::Ok && x.resp.status() == 200);
  }
  {  // a server that closes after the full reply without close_notify: still a success
    TlsServer s;
    s.close_notify = false;
    TlsRun x = tlsRun(s, &ra.ta, 1);
    CHECK(x.r.why == TlscWhy::Ok && x.r.response && x.resp.result() == FwdResult::Ok);
  }
  {  // closed before the status line: read
    TlsServer s;
    s.close_before_status = true;
    TlsRun x = tlsRun(s, &ra.ta, 1);
    CHECK(x.r.why == TlscWhy::Read && !x.r.response && x.d.stage == TlscStage::Data && x.d.flushed);
  }
  // 2. trust
  {  // wrong root: untrusted, BR_ERR_X509_NOT_TRUSTED
    TlsServer s;
    TlsRun x = tlsRun(s, &rb.ta, 1);
    CHECK(x.r.why == TlscWhy::Untrusted && x.d.br_err == BR_ERR_X509_NOT_TRUSTED && s.req.empty());
  }
  {  // a name the certificate does not carry (SNI / name check): untrusted
    TlsServer s;
    TlsRun x = tlsRun(s, &ra.ta, 1, "other.example");
    CHECK(x.r.why == TlscWhy::Untrusted && x.d.br_err == BR_ERR_X509_BAD_SERVER_NAME && s.req.empty());
  }
  {  // the clock before / after the validity window: untrusted (expired family)
    TlsServer s1, s2;
    TlsRun a = tlsRun(s1, &ra.ta, 1, "test.example", unixOf(2025, 6, 1));
    TlsRun b = tlsRun(s2, &ra.ta, 1, "test.example", unixOf(2037, 6, 1));
    CHECK(a.r.why == TlscWhy::Untrusted && a.d.br_err == BR_ERR_X509_EXPIRED);
    CHECK(b.r.why == TlscWhy::Untrusted && b.d.br_err == BR_ERR_X509_EXPIRED);
  }
  {  // no real time: `clock`, nothing allocated, not a byte written
    TlsServer s;
    AllocSpy::reset();
    TlscReq q = {};
    TlscTrust tr{&ra.ta, 1};
    q.host = "test.example", q.trust = &tr, q.unix_now = 0, q.buf_len = TLSC_BUF_LEN;
    q.read_cap = 1536, q.entropy = testEntropy, q.alloc = AllocSpy::alloc, q.release = AllocSpy::release;
    FwdResp resp;
    resp.begin(FwdType::Template, "");
    TlscDiag d;
    const TlscIo io{&s, TlsServer::cliRead, TlsServer::cliWrite};
    const TlscResult r = tlscGet(q, io, "GET / HTTP/1.1\r\n\r\n", 18, resp, d);
    CHECK(r.why == TlscWhy::Clock && AllocSpy::count == 0 && s.writes == 0 && s.reads == 0);
  }
  {  // an empty host name never runs (it would switch the name check off)
    TlsServer s;
    AllocSpy::reset();
    TlsRun x = tlsRun(s, &ra.ta, 1, "");
    CHECK(x.r.why == TlscWhy::Untrusted && AllocSpy::count == 0 && s.writes == 0);
  }
  {  // an anchor mask without the right bit (no anchor at all, or only the wrong one): untrusted
    TlsServer s;
    TlsRun x = tlsRun(s, nullptr, 0);
    CHECK(x.r.why == TlscWhy::Untrusted);
  }
  // 3. too_large: application records above our buffer from a server that ignores the MFL we asked for
  {
    TlsServer s;
    s.big_records = true;
    s.body = std::string(9000, 'x');
    s.head = "HTTP/1.1 200 OK\r\nContent-Length: 9000\r\nConnection: close\r\n\r\n";
    TlsRun x = tlsRun(s, &ra.ta, 1, "test.example", 0, TLSC_BUF_LEN, "", FwdType::Template, "zzz");
    CHECK(x.r.why == TlscWhy::TooLarge && x.d.br_err == BR_ERR_TOO_LARGE && x.d.rec_in_max > 4096);
    CHECK(fwdTlsResult(x.r.why) == FwdResult::Reject && fwdCauseOf(FwdResult::Reject, 200, "too_large") == FwdCause::Other);
  }
  // 4. failure injection
  for (int k = 1; k <= 3; k++) {  // allocation k fails: no_memory, nothing sent, the others freed
    TlsServer s;
    AllocSpy::reset(k);
    TlsRun x = tlsRun(s, &ra.ta, 1);
    CHECK(x.r.why == TlscWhy::NoMemory && s.writes == 0 && AllocSpy::live.empty());
    CHECK(fwdTlsResult(x.r.why) == FwdResult::NoMemory);
  }
  AllocSpy::reset();
  {  // the pump stops mid-handshake (a timeout): handshake
    TlsServer s;
    s.stop_after_reads = 1;
    TlsRun x = tlsRun(s, &ra.ta, 1);
    CHECK(x.r.why == TlscWhy::Handshake && x.d.br_err == BR_ERR_IO && x.d.stage == TlscStage::Handshake && !x.d.flushed);
  }
  {  // ... after the handshake, before the reply: read (deterministic: the request arrives, the reply never does)
    TlsServer s;
    s.stall_reply = true;
    TlsRun x = tlsRun(s, &ra.ta, 1);
    CHECK(x.r.why == TlscWhy::Read && x.d.stage == TlscStage::Data && x.d.br_err == BR_ERR_IO && !s.req.empty() && x.d.flushed);
  }
  {  // a server asking for a renegotiation gets none (BR_OPT_NO_RENEGOTIATION): no second ClientHello
    TlsServer base;
    TlsRun a = tlsRun(base, &ra.ta, 1);
    TlsServer s;
    s.renegotiate = true;
    TlsRun x = tlsRun(s, &ra.ta, 1);
    CHECK(a.r.why == TlscWhy::Ok && base.cli_hs_records >= 2);
    CHECK(s.cli_hs_records == base.cli_hs_records);
    printf("  info: renegotiation refused: client handshake records %d (as without it), result %s\n", s.cli_hs_records,
           tlscWhyName(x.r.why));
  }
  // the AES-256-GCM / SHA-384 and ChaCha20-Poly1305 suites end to end
  for (uint16_t suite : {(uint16_t)BR_TLS_ECDHE_ECDSA_WITH_AES_256_GCM_SHA384, (uint16_t)BR_TLS_ECDHE_ECDSA_WITH_CHACHA20_POLY1305_SHA256}) {
    TlsServer s(suite);
    TlsRun x = tlsRun(s, &ra.ta, 1);
    CHECK(x.r.why == TlscWhy::Ok && x.d.suite == suite && x.resp.status() == 200 && x.d.version == BR_TLS12);
  }
  {  // no entropy injected: the reset fails (BR_ERR_NO_RANDOM, no system RNG here as on xtensa), never a crash
    TlsServer s;
    TlsRun x = tlsRun(s, &ra.ta, 1, "test.example", 0, TLSC_BUF_LEN, "", FwdType::Template, "", false);
    CHECK(x.r.why == TlscWhy::Handshake && x.d.br_err == BR_ERR_NO_RANDOM && s.writes == 0);
  }
  CHECK(fwdTlsResult(TlscWhy::Untrusted) == FwdResult::TlsUntrusted && fwdTlsResult(TlscWhy::Handshake) == FwdResult::Fail &&
        fwdTlsResult(TlscWhy::Read) == FwdResult::Fail && fwdTlsResult(TlscWhy::Clock) == FwdResult::Fail &&
        fwdTlsResult(TlscWhy::Aborted) == FwdResult::Fail);
  CHECK(fwdCauseOf(FwdResult::Fail, -1, "handshake") == FwdCause::Connect);

  // 5. the classification over every BearSSL error code and both alert ranges: no engine code is ever no_memory
  {
    int named = 0;
    for (int e = 0; e < 1024; e++) {
      for (TlscStage st : {TlscStage::Handshake, TlscStage::Data}) {
        const TlscWhy w = tlscClassify(st, e, false);
        CHECK(w != TlscWhy::NoMemory);
        if (e == BR_ERR_OK) CHECK(w == TlscWhy::Ok);
        if (e == BR_ERR_IO) CHECK(w == (st == TlscStage::Data ? TlscWhy::Read : TlscWhy::Handshake));
        if (e == BR_ERR_TOO_LARGE) CHECK(w == TlscWhy::TooLarge);
      }
      if (tlscErrName(e)[0] && e < 256) named++;
    }
    CHECK(named == 60);  // BR_ERR_* 0..31 (30 names: 11 and 30 unused) + BR_ERR_X509_* 32..62 (30 names: 61 unused)
    for (int e : {BR_ERR_X509_NOT_TRUSTED, BR_ERR_X509_BAD_SERVER_NAME, BR_ERR_X509_EXPIRED, BR_ERR_X509_NOT_CA,
                  BR_ERR_X509_FORBIDDEN_KEY_USAGE, BR_ERR_X509_WEAK_PUBLIC_KEY, BR_ERR_X509_DN_MISMATCH})
      CHECK(tlscClassify(TlscStage::Handshake, e, false) == TlscWhy::Untrusted);
    for (int e : {BR_ERR_X509_BAD_SIGNATURE, BR_ERR_BAD_MAC, BR_ERR_UNSUPPORTED_VERSION, BR_ERR_BAD_CIPHER_SUITE,
                  BR_ERR_RECV_FATAL_ALERT + 40, BR_ERR_SEND_FATAL_ALERT + 80})
      CHECK(tlscClassify(TlscStage::Handshake, e, false) == TlscWhy::Handshake);
    CHECK(tlscClassify(TlscStage::Handshake, BR_ERR_X509_NOT_TRUSTED, true) == TlscWhy::NoMemory);  // decided before
    CHECK(strcmp(tlscErrName(BR_ERR_RECV_FATAL_ALERT + 40), "BR_ERR_RECV_FATAL_ALERT") == 0 &&
          strcmp(tlscErrName(BR_ERR_X509_NOT_TRUSTED), "BR_ERR_X509_NOT_TRUSTED") == 0 && !tlscErrName(999)[0]);
    for (int w = 0; w < (int)TlscWhy::COUNT; w++) CHECK(tlscWhyName((TlscWhy)w)[0]);
  }
  // 6. our anchors: BearSSL's decoder of each pinned certificate gives the generated DN and key, byte for byte
  {
    CHECK((int)(sizeof FWD_ROOT_DERS / sizeof FWD_ROOT_DERS[0]) == FWD_ROOT_COUNT);
    for (int i = 0; i < FWD_ROOT_COUNT; i++) {
      DecodedTa t(FWD_ROOT_DERS[i].der, FWD_ROOT_DERS[i].len);
      const br_x509_trust_anchor& g = FWD_TAS[i];
      bool same = t.ok && t.ta.dn.len == g.dn.len && memcmp(t.ta.dn.data, g.dn.data, g.dn.len) == 0 &&
                  t.ta.pkey.key_type == g.pkey.key_type && (g.flags & BR_X509_TA_CA);
      if (same && g.pkey.key_type == BR_KEYTYPE_RSA)
        same = t.ta.pkey.key.rsa.nlen == g.pkey.key.rsa.nlen &&
               memcmp(t.ta.pkey.key.rsa.n, g.pkey.key.rsa.n, g.pkey.key.rsa.nlen) == 0 &&
               t.ta.pkey.key.rsa.elen == g.pkey.key.rsa.elen &&
               memcmp(t.ta.pkey.key.rsa.e, g.pkey.key.rsa.e, g.pkey.key.rsa.elen) == 0;
      if (same && g.pkey.key_type == BR_KEYTYPE_EC)
        same = t.ta.pkey.key.ec.curve == g.pkey.key.ec.curve && t.ta.pkey.key.ec.qlen == g.pkey.key.ec.qlen &&
               memcmp(t.ta.pkey.key.ec.q, g.pkey.key.ec.q, g.pkey.key.ec.qlen) == 0;
      CHECK(same);
      if (!same) printf("  anchor %d differs from its certificate\n", i);
    }
  }
  // 7. the record tracker over arbitrary splits; the TLS gate per class
  {
    // three records: 5-byte headers + bodies of 300, 4100 and 2 bytes
    std::vector<uint8_t> st;
    for (uint16_t len : {(uint16_t)300, (uint16_t)4100, (uint16_t)2}) {
      const uint8_t h[5] = {0x17, 0x03, 0x03, (uint8_t)(len >> 8), (uint8_t)len};
      st.insert(st.end(), h, h + 5);
      st.insert(st.end(), len, 0xAB);
    }
    for (size_t chunk : {(size_t)1, (size_t)3, (size_t)5, (size_t)7, st.size()}) {
      TlscRecTracker t;
      for (size_t i = 0; i < st.size(); i += chunk) t.feed(st.data() + i, std::min(chunk, st.size() - i));
      CHECK(t.max == 4100 && t.count == 3);
    }
    // the gate: P[class] + 12 KB free and the shed line's block + our record buffer
    // the Metar class at 17.5 KB (a measured quiet median), the presets at 17 KB
    CHECK(fwdResourcesOk(true, FwdTlsClass::Metar, 17920 + 12 * 1024, FWD_TLS_LARGEST_MIN) &&
          !fwdResourcesOk(true, FwdTlsClass::Metar, 17920 + 12 * 1024 - 1, FWD_TLS_LARGEST_MIN) &&
          !fwdResourcesOk(true, FwdTlsClass::Metar, 17920 + 12 * 1024, FWD_TLS_LARGEST_MIN - 1) &&
          fwdResourcesOk(true, FwdTlsClass::PresetNoMfl, 29 * 1024, FWD_TLS_LARGEST_MIN) &&
          !fwdResourcesOk(true, FwdTlsClass::PresetNoMfl, 29 * 1024 - 1, FWD_TLS_LARGEST_MIN));
    CHECK(FWD_TLS_LARGEST_MIN == SHED_LARGEST_MIN + TLSC_CTX_BYTES + TLSC_BUF_LEN);  // all three blocks
    CHECK(tlscBlockBytes(TLSC_BUF_LEN) <= TLSC_BLOCKS_MAX);
    // the WROOM-32's byte heap at rest (50 240 free, 45 044 largest) passes the TLS gate of every class
    for (int c = 0; c < (int)FwdTlsClass::COUNT; c++) CHECK(fwdResourcesOk(true, (FwdTlsClass)c, 50240, 45044));
    CHECK(fwdTlsClassOf(FwdType::Wu) == FwdTlsClass::PresetNoMfl && fwdTlsClassOf(FwdType::Windy) == FwdTlsClass::PresetNoMfl &&
          fwdTlsClassOf(FwdType::PwsWeather) == FwdTlsClass::PresetNoMfl && fwdTlsClassOf(FwdType::WowBe) == FwdTlsClass::WowBe &&
          fwdTlsClassOf(FwdType::Template) == FwdTlsClass::Custom && strcmp(fwdTlsClassName(FwdTlsClass::Metar), "metar") == 0);
  }
  // the METAR source (run once, fetch, lookup) is tested in test_metar_sched.inc
  printf("  info: tlsc blocks %u B (client %u + X.509 %u + buffer %u)\n", (unsigned)tlscBlockBytes(TLSC_BUF_LEN),
         (unsigned)sizeof(br_ssl_client_context), (unsigned)sizeof(br_x509_minimal_context), (unsigned)TLSC_BUF_LEN);
}
