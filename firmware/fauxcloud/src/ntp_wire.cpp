// ntp_wire.cpp -- see ntp_wire.h.
#include "ntp_wire.h"

#include <cstring>

namespace fc {

bool ntpParseRequest(const uint8_t* pkt, size_t len, NtpRequest* out) {
  if (len < NTP_PACKET) return false;
  out->vn = (pkt[0] >> 3) & 7;
  out->mode = pkt[0] & 7;
  return out->mode == 3 || out->mode == 1;
}

static void putTs(uint8_t* p, uint32_t secs, uint32_t frac) {
  p[0] = (uint8_t)(secs >> 24); p[1] = (uint8_t)(secs >> 16); p[2] = (uint8_t)(secs >> 8); p[3] = (uint8_t)secs;
  p[4] = (uint8_t)(frac >> 24); p[5] = (uint8_t)(frac >> 16); p[6] = (uint8_t)(frac >> 8); p[7] = (uint8_t)frac;
}

void ntpBuildReply(const uint8_t* req, const NtpRequest& r, uint32_t unix_s, uint32_t usec, uint8_t out[NTP_PACKET]) {
  memset(out, 0, NTP_PACKET);
  const uint32_t secs = unix_s + NTP_UNIX_OFFSET;
  const uint32_t frac = (uint32_t)((uint64_t)usec * 4294967296ULL / 1000000ULL);
  out[0] = (uint8_t)((r.vn ? r.vn : 4) << 3 | 4);  // LI 0, VN, mode 4 (server)
  out[1] = 2;                                       // stratum 2
  out[2] = req[2];                                  // poll, echoed
  out[3] = (uint8_t)-20;                            // precision ~1 us
  memcpy(out + 12, "LOCL", 4);                      // reference id
  putTs(out + 16, secs, frac);                      // reference
  memcpy(out + 24, req + 40, 8);                    // originate = client's transmit
  putTs(out + 32, secs, frac);                      // receive
  putTs(out + 40, secs, frac);                      // transmit
}

}  // namespace fc
