// tls_sni.h -- what a TLS ClientHello asks for. tlsSni: the server name (the setup mode's port-443 logger shows
// which host an Android phone probes over HTTPS). tlsHello: every field a later TLS server needs to know whether it
// could answer that client (the capture mode's `tls` record). Read-only parsing of the first record; nothing is
// answered. Plain C++.
#pragma once
#include <cstddef>
#include <cstdint>

namespace fc {

// true and the host name in `out` (cut to cap - 1) when `d` starts with a TLS handshake record holding a ClientHello
// with a server_name extension; false for anything else or a record cut short.
bool tlsSni(const uint8_t* d, size_t n, char* out, size_t cap);

// A field of the hello: not sent, sent, or not known because the bytes ended before it could appear.
enum TlsField : uint8_t { TF_ABSENT = 0, TF_PRESENT = 1, TF_UNKNOWN = 2 };
constexpr int TLS_HELLO_SUITES = 12;  // the suite ids kept for the record (all of them count for the verdict)

struct TlsHello {
  bool hello;              // a handshake record with a ClientHello
  bool cut;                // the bytes ended before the hello did
  uint16_t rec_len;        // the record's declared length
  uint16_t client_version;  // the legacy version field (0x0303 = TLS 1.2)
  uint8_t sv_st;           // supported_versions
  uint8_t nver;
  uint16_t ver[6];
  uint16_t nsuites;        // every suite offered (GREASE and the renegotiation SCSV not counted)
  uint8_t nkept;
  uint16_t suites[TLS_HELLO_SUITES];
  bool suites_cut;         // the suite list itself ran past the bytes
  uint8_t sni_st;
  char sni[64];
  uint8_t groups_st;
  uint8_t ngroups;
  uint16_t groups[6];
  uint8_t sigalg_st;
  uint8_t mfl_st;
  uint8_t mfl;             // max_fragment_length code (1 = 512 B ... 4 = 4096 B)
  uint8_t alpn_st;
  char alpn[24];           // the first protocol offered
  // the suite families seen anywhere in the list (the verdict's input)
  bool ecdsa_gcm, ecdsa_chacha, rsa_ecdhe, ecdhe_other, legacy, tls13;
};

// Parses `d` (the first bytes of a connection, at most what the peek kept). false = not a ClientHello.
bool tlsHello(const uint8_t* d, size_t n, TlsHello* h);
// Whether the vendored BearSSL server (TLS 1.2; ECDHE with ECDSA or RSA certificates; AES-GCM, ChaCha20-Poly1305;
// P-256 or X25519) could answer this client: ecdhe-ecdsa-gcm, chacha, ecdhe-rsa-gcm, no-common-curve, cbc-only,
// rsa-kx-only, tls13-only, tls-old, unknown.
const char* tlsVerdict(const TlsHello& h);
// The capture record's text, the verdict first so a cut at cap never loses it:
// "verdict=ecdhe-rsa-gcm v=1.3,1.2 sni=api.example.net suites=c02b,...,c030 +18 groups=x25519,p256 sigalg=yes mfl=- alpn=h2"
// (an unknown field "?", an absent one "-", " cut" when the bytes ended inside the hello). Returns the length.
size_t tlsHelloText(const TlsHello& h, char* out, size_t cap);

}  // namespace fc
