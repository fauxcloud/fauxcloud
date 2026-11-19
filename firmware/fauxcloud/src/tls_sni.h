// tls_sni.h -- the server name a TLS ClientHello asks for (the setup mode's port-443 logger shows which
// host an Android phone probes over HTTPS). Read-only parsing of the first record; nothing is answered. Plain C++.
#pragma once
#include <cstddef>
#include <cstdint>

namespace fc {

// true and the host name in `out` (cut to cap - 1) when `d` starts with a TLS handshake record holding a ClientHello
// with a server_name extension; false for anything else or a record cut short.
bool tlsSni(const uint8_t* d, size_t n, char* out, size_t cap);

}  // namespace fc
