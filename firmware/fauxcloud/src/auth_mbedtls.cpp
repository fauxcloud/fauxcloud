// auth_mbedtls.cpp -- PBKDF2-HMAC-SHA256 on the device, via mbedTLS from the core (hardware SHA where present).
// The host build uses auth_soft.cpp; both are checked against the same vector (host test, boot self-test).
#ifdef ARDUINO
#include <mbedtls/md.h>
#include <mbedtls/pkcs5.h>

#include "auth.h"

namespace fc {

bool pbkdf2Sha256(const uint8_t* pw, size_t pw_len, const uint8_t* salt, size_t salt_len, uint32_t iter,
                  uint8_t out[32]) {
  return mbedtls_pkcs5_pbkdf2_hmac_ext(MBEDTLS_MD_SHA256, pw, pw_len, salt, salt_len, iter, 32, out) == 0;
}

bool hmacSha256(const uint8_t* key, size_t key_len, const uint8_t* msg, size_t msg_len, uint8_t out[32]) {
  return mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), key, key_len, msg, msg_len, out) == 0;
}

}  // namespace fc
#endif  // ARDUINO
