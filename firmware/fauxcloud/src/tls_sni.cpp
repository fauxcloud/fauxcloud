// tls_sni.cpp -- see tls_sni.h.
#include "tls_sni.h"

#include <cstdio>
#include <cstring>

namespace fc {

static uint32_t be(const uint8_t* p, int k) {
  uint32_t v = 0;
  for (int i = 0; i < k; i++) v = v << 8 | p[i];
  return v;
}

// a name from the wire made printable (a record, never raw bytes)
static void copyName(const uint8_t* s, size_t n, char* out, size_t cap) {
  size_t k = 0;
  for (; k < n && k + 1 < cap; k++) {
    const char c = (char)s[k];
    out[k] = (c > 32 && c < 127) ? c : '?';
  }
  out[k] = 0;
}

// GREASE values (RFC 8701: 0x?a?a with both bytes equal) and the renegotiation SCSV are not suites a client wants
static bool noiseSuite(uint16_t s) { return s == 0x00ff || ((s & 0x0f0f) == 0x0a0a && (s >> 8) == (s & 0xff)); }

static void classify(TlsHello* h, uint16_t s) {
  if (s == 0xc02b || s == 0xc02c) h->ecdsa_gcm = true;
  else if (s == 0xcca9) h->ecdsa_chacha = true;
  else if (s == 0xc02f || s == 0xc030 || s == 0xcca8) h->rsa_ecdhe = true;
  else if ((s >> 8) == 0xc0 || (s >> 8) == 0xcc) h->ecdhe_other = true;
  else if ((s >> 8) == 0x13) h->tls13 = true;
  else if ((s >> 8) == 0x00) h->legacy = true;
}

bool tlsHello(const uint8_t* d, size_t n, TlsHello* h) {
  memset(h, 0, sizeof *h);
  h->sv_st = h->sni_st = h->groups_st = h->sigalg_st = h->mfl_st = h->alpn_st = TF_UNKNOWN;
  // record header (5) + handshake type (1): a handshake record holding a ClientHello
  if (n < 6 || d[0] != 0x16 || d[1] != 0x03 || d[5] != 0x01) return false;
  h->hello = true;
  h->rec_len = (uint16_t)be(d + 3, 2);
  size_t end = 5 + (size_t)h->rec_len;  // the hello may continue in a later record: these bytes are the evidence
  if (end > n) end = n, h->cut = true;
  // handshake length (3) + client_version (2) + random (32)
  if (end < 43) {
    h->cut = true;
    if (end >= 11) h->client_version = (uint16_t)be(d + 9, 2);
    return true;
  }
  h->client_version = (uint16_t)be(d + 9, 2);
  size_t i = 43;
  if (i + 1 > end) return h->cut = true, true;
  i += 1 + d[i];  // session id
  if (i + 2 > end) return h->cut = true, true;
  const size_t slen = be(d + i, 2);
  i += 2;
  const size_t send = i + slen;
  for (size_t j = i; j + 2 <= send; j += 2) {
    if (j + 2 > end) {
      h->suites_cut = h->cut = true;
      break;
    }
    const uint16_t s = (uint16_t)be(d + j, 2);
    if (noiseSuite(s)) continue;
    h->nsuites++;
    if (h->nkept < TLS_HELLO_SUITES) h->suites[h->nkept++] = s;
    classify(h, s);
  }
  if (send > end) return h->suites_cut = h->cut = true, true;
  i = send;
  if (i + 1 > end) return h->cut = true, true;
  i += 1 + d[i];  // compression methods
  // no extensions at all: every extension field is known to be absent
  if (i + 2 > end) {
    if (i == end && end == 5 + (size_t)h->rec_len) {
      h->sv_st = h->sni_st = h->groups_st = h->sigalg_st = h->mfl_st = h->alpn_st = TF_ABSENT;
      return true;
    }
    return h->cut = true, true;
  }
  size_t extEnd = i + 2 + be(d + i, 2);
  i += 2;
  bool whole = true;
  if (extEnd > end) extEnd = end, whole = false;
  while (i + 4 <= extEnd) {
    const uint32_t type = be(d + i, 2), len = be(d + i + 2, 2);
    i += 4;
    if (i + len > extEnd) {
      whole = false;
      break;
    }
    const uint8_t* e = d + i;
    switch (type) {
      case 0:  // server_name: list length (2), then entries of type (1) + length (2) + name
        h->sni_st = TF_ABSENT;
        for (size_t j = 2; j + 3 <= len;) {
          const uint32_t nameType = e[j], nameLen = be(e + j + 1, 2);
          j += 3;
          if (j + nameLen > len) break;
          if (nameType == 0 && nameLen) {
            copyName(e + j, nameLen, h->sni, sizeof h->sni);
            h->sni_st = TF_PRESENT;
            break;
          }
          j += nameLen;
        }
        break;
      case 1:  // max_fragment_length: one code
        h->mfl_st = len >= 1 ? TF_PRESENT : TF_ABSENT;
        if (len >= 1) h->mfl = e[0];
        break;
      case 10:  // supported_groups: list length (2), then u16s
        h->groups_st = TF_PRESENT;
        for (size_t j = 2; j + 2 <= len && j + 2 <= 2 + be(e, 2); j += 2) {
          const uint16_t g = (uint16_t)be(e + j, 2);
          if ((g & 0x0f0f) == 0x0a0a) continue;  // GREASE
          if (h->ngroups < sizeof h->groups / sizeof h->groups[0]) h->groups[h->ngroups++] = g;
        }
        break;
      case 13:
        h->sigalg_st = TF_PRESENT;
        break;
      case 16:  // ALPN: list length (2), then length (1) + name
        h->alpn_st = TF_PRESENT;
        if (len >= 3 && e[2] && 3 + (size_t)e[2] <= len) copyName(e + 3, e[2], h->alpn, sizeof h->alpn);
        break;
      case 43:  // supported_versions: list length (1), then u16s
        h->sv_st = TF_PRESENT;
        for (size_t j = 1; j + 2 <= len && j + 2 <= 1 + (size_t)e[0]; j += 2) {
          const uint16_t v = (uint16_t)be(e + j, 2);
          if ((v & 0x0f0f) == 0x0a0a) continue;
          if (h->nver < sizeof h->ver / sizeof h->ver[0]) h->ver[h->nver++] = v;
        }
        break;
      default:
        break;
    }
    i += len;
  }
  if (whole && extEnd == i) {  // every extension read: what was not seen was not sent
    uint8_t* st[] = {&h->sv_st, &h->sni_st, &h->groups_st, &h->sigalg_st, &h->mfl_st, &h->alpn_st};
    for (uint8_t* s : st)
      if (*s == TF_UNKNOWN) *s = TF_ABSENT;
  } else {
    h->cut = true;
  }
  return true;
}

static bool hasU16(const uint16_t* a, int n, uint16_t v) {
  for (int i = 0; i < n; i++)
    if (a[i] == v) return true;
  return false;
}

const char* tlsVerdict(const TlsHello& h) {
  if (!h.hello) return "unknown";
  // TLS 1.2 offered: the version list when sent, else the legacy field
  const bool v12 = h.sv_st == TF_PRESENT ? hasU16(h.ver, h.nver, 0x0303) : h.client_version >= 0x0303;
  const bool v13 = h.sv_st == TF_PRESENT && hasU16(h.ver, h.nver, 0x0304);
  if (!v12 && v13) return "tls13-only";
  if (!v12 && h.client_version) return "tls-old";
  // ECDHE needs P-256 or X25519 (both vendored); without the extension a client means the NIST curves (RFC 4492)
  const bool curve = h.groups_st != TF_PRESENT || hasU16(h.groups, h.ngroups, 0x0017) || hasU16(h.groups, h.ngroups, 0x001d);
  const bool ecdhe = h.ecdsa_gcm || h.ecdsa_chacha || h.rsa_ecdhe;
  if (ecdhe && !curve) return "no-common-curve";
  if (h.ecdsa_gcm) return "ecdhe-ecdsa-gcm";
  if (h.ecdsa_chacha) return "chacha";
  if (h.rsa_ecdhe) return "ecdhe-rsa-gcm";
  if (h.ecdhe_other) return "cbc-only";
  if (h.legacy) return "rsa-kx-only";
  if (h.tls13) return "tls13-only";
  return "unknown";
}

static const char* groupName(uint16_t g, char* buf, size_t cap) {
  switch (g) {
    case 0x001d: return "x25519";
    case 0x0017: return "p256";
    case 0x0018: return "p384";
    case 0x0019: return "p521";
    case 0x001e: return "x448";
    default: snprintf(buf, cap, "%04x", g); return buf;
  }
}

static const char* verName(uint16_t v, char* buf, size_t cap) {
  switch (v) {
    case 0x0304: return "1.3";
    case 0x0303: return "1.2";
    case 0x0302: return "1.1";
    case 0x0301: return "1.0";
    default: snprintf(buf, cap, "%04x", v); return buf;
  }
}

size_t tlsHelloText(const TlsHello& h, char* out, size_t cap) {
  if (!cap) return 0;
  size_t n = 0;
  auto put = [&](const char* fmt, const char* a) {
    if (n + 1 >= cap) return;
    const int w = snprintf(out + n, cap - n, fmt, a);
    if (w > 0) n = n + (size_t)w < cap ? n + (size_t)w : cap - 1;
  };
  char b[8];
  put("verdict=%s", tlsVerdict(h));
  if (h.sv_st == TF_PRESENT && h.nver) {
    for (int i = 0; i < h.nver; i++) put(i ? ",%s" : " v=%s", verName(h.ver[i], b, sizeof b));
  } else {
    put(" v=%s", h.client_version ? verName(h.client_version, b, sizeof b) : "?");
  }
  put(" sni=%s", h.sni_st == TF_PRESENT ? h.sni : h.sni_st == TF_ABSENT ? "-" : "?");
  if (!h.nkept) put(" suites=%s", h.suites_cut || h.cut ? "?" : "-");
  for (int i = 0; i < h.nkept; i++) {
    snprintf(b, sizeof b, "%04x", h.suites[i]);
    put(i ? ",%s" : " suites=%s", b);
  }
  if (h.nsuites > h.nkept) {
    snprintf(b, sizeof b, "%u", (unsigned)(h.nsuites - h.nkept));
    put(" +%s", b);
  }
  if (h.groups_st == TF_PRESENT && h.ngroups) {
    for (int i = 0; i < h.ngroups && i < 4; i++) put(i ? ",%s" : " groups=%s", groupName(h.groups[i], b, sizeof b));
  } else {
    put(" groups=%s", h.groups_st == TF_UNKNOWN ? "?" : "-");
  }
  put(" sigalg=%s", h.sigalg_st == TF_PRESENT ? "yes" : h.sigalg_st == TF_ABSENT ? "no" : "?");
  if (h.mfl_st == TF_PRESENT) {
    snprintf(b, sizeof b, "%u", 1u << (8 + (h.mfl > 4 ? 0 : h.mfl)));
    put(" mfl=%s", b);
  } else {
    put(" mfl=%s", h.mfl_st == TF_ABSENT ? "-" : "?");
  }
  if (h.cut) put("%s", " cut");
  put(" alpn=%s", h.alpn_st == TF_PRESENT && h.alpn[0] ? h.alpn : h.alpn_st == TF_UNKNOWN ? "?" : "-");
  out[n] = 0;
  return n;
}

bool tlsSni(const uint8_t* d, size_t n, char* out, size_t cap) {
  if (!cap) return false;
  out[0] = 0;
  TlsHello h;
  if (!tlsHello(d, n, &h) || h.sni_st != TF_PRESENT) return false;
  snprintf(out, cap, "%s", h.sni);
  return out[0] != 0;
}

}  // namespace fc
