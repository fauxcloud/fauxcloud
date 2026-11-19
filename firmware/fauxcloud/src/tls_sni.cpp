// tls_sni.cpp -- see tls_sni.h.
#include "tls_sni.h"

namespace fc {

static uint32_t be(const uint8_t* p, int k) {
  uint32_t v = 0;
  for (int i = 0; i < k; i++) v = v << 8 | p[i];
  return v;
}

bool tlsSni(const uint8_t* d, size_t n, char* out, size_t cap) {
  if (!cap) return false;
  out[0] = 0;
  // record header (5) + handshake header (4) + client_version (2) + random (32)
  if (n < 43 || d[0] != 0x16 || d[5] != 0x01) return false;
  size_t end = 5 + be(d + 3, 2);  // the record; the ClientHello may be longer than what arrived
  if (end > n) end = n;
  size_t i = 43;
  if (i + 1 > end) return false;
  i += 1 + d[i];  // session id
  if (i + 2 > end) return false;
  i += 2 + be(d + i, 2);  // cipher suites
  if (i + 1 > end) return false;
  i += 1 + d[i];  // compression methods
  if (i + 2 > end) return false;
  size_t extEnd = i + 2 + be(d + i, 2);
  i += 2;
  if (extEnd > end) extEnd = end;
  while (i + 4 <= extEnd) {
    const uint32_t type = be(d + i, 2), len = be(d + i + 2, 2);
    i += 4;
    if (i + len > extEnd) return false;
    if (type == 0) {  // server_name: list length (2), then entries of type (1) + length (2) + name
      size_t j = i + 2;
      while (j + 3 <= i + len) {
        const uint32_t nameType = d[j], nameLen = be(d + j + 1, 2);
        j += 3;
        if (j + nameLen > i + len) return false;
        if (nameType == 0) {
          size_t k = 0;
          for (; k < nameLen && k + 1 < cap; k++) {
            const char c = (char)d[j + k];
            out[k] = (c > 32 && c < 127) ? c : '?';  // a log line, never raw bytes
          }
          out[k] = 0;
          return k > 0;
        }
        j += nameLen;
      }
      return false;
    }
    i += len;
  }
  return false;
}

}  // namespace fc
