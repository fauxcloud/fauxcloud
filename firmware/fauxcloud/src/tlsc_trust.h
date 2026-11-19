// tlsc_trust.h -- the BearSSL side of the anchors. Only src/tlsc.cpp and the host tests include it, so no
// BearSSL type reaches tlsc.h, the scheduler, the glue or the sheet: swapping the TLS stack touches tlsc.cpp and this file.
#pragma once
#include <cstddef>

#include "bearssl/bearssl_x509.h"

extern "C" const br_x509_trust_anchor FWD_TAS[];  // src/fwd_roots.c (generated); bit i of a root mask = FWD_TAS[i]

namespace fc {

// An explicit anchor set (host tests: their own test roots); tlscGet uses FWD_TAS by TlscReq.roots when none is given.
struct TlscTrust {
  const br_x509_trust_anchor* tas;
  size_t n;
};

}  // namespace fc
