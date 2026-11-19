// ntp_wire.h -- the SNTP responder's packet logic (ported from discovery.ino), plain C++.
//
// The console syncs its clock from a hostname (default time.windows.com) that our DNS steers to the ESP. We answer
// as a stratum-2 server, but only while the ESP's own clock is synced from the LAN side; the sketch decides that
// and owns the socket. Host-compilable.
#pragma once
#include <cstddef>
#include <cstdint>

namespace fc {

constexpr size_t NTP_PACKET = 48;
constexpr uint32_t NTP_UNIX_OFFSET = 2208988800UL;  // 1900 -> 1970

struct NtpRequest {
  int vn;    // version number from the request
  int mode;  // 3 = client, 1 = symmetric active
};

// Accepts a request of at least 48 bytes in client (3) or symmetric-active (1) mode; anything else (a server
// reply, broadcast, a short packet) is not answered, so two responders can never ping-pong.
bool ntpParseRequest(const uint8_t* pkt, size_t len, NtpRequest* out);

// Builds the 48-byte reply: LI 0, the request's VN (4 if 0), mode 4, stratum 2, reference id "LOCL",
// originate = the request's transmit timestamp, reference/receive/transmit = now.
void ntpBuildReply(const uint8_t* req, const NtpRequest& r, uint32_t unix_s, uint32_t usec, uint8_t out[NTP_PACKET]);

}  // namespace fc
