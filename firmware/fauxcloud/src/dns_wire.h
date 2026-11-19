// dns_wire.h -- the wildcard DNS responder's packet logic (ported from discovery.ino), plain C++.
//
// The console resolves rtupdate.wunderground.com, its NTP host, weathercloud.* ... through the DNS server it got
// from DHCP (the ESP). Every A query is answered with the AP address; any other type gets an empty NOERROR answer.
// Socket I/O stays in the sketch; this file only parses and builds packets. Host-compilable.
#pragma once
#include <cstddef>
#include <cstdint>

namespace fc {

constexpr size_t DNS_NAME_MAX = 128;

struct DnsQuestion {
  uint16_t id;
  bool rd;                   // recursion desired, echoed back
  char name[DNS_NAME_MAX];   // dotted, as received (no case folding)
  uint16_t qtype;            // 1 = A, 28 = AAAA, ...
  uint16_t qclass;           // 1 = IN
  size_t qend;               // offset just past the question (header + question are echoed)
};

// Parses a standard query with exactly one question. Rejects (false): shorter than a header, a response
// (QR set), an opcode other than QUERY, QDCOUNT != 1, a compression pointer or a label running past the end,
// a name longer than DNS_NAME_MAX - 1, a question without its 4 bytes of type/class.
bool dnsParseQuery(const uint8_t* pkt, size_t len, DnsQuestion* q);

// Builds the answer into out: header + the echoed question + (for A/IN) one A record with ip, TTL 60 s.
// Returns the length, or 0 if cap is too small. Additional records of the query (EDNS OPT) are dropped.
size_t dnsBuildAnswer(const uint8_t* pkt, const DnsQuestion& q, const uint8_t ip[4], uint8_t* out, size_t cap);

// Logs each distinct name at most once per window (the console repeats its lookups; Windows floods).
// Fixed table, the oldest entry is evicted. Not thread-safe. *first (optional) is true when the name was not
// in the table (its first line since the restart, or since it was evicted): the boot log keeps that one always.
class NameLogLimiter {
 public:
  static constexpr int SLOTS = 32;
  static constexpr uint32_t WINDOW_S = 600;
  bool shouldLog(const char* name, uint32_t now_s, bool* first = nullptr);

 private:
  uint32_t hash_[SLOTS] = {};
  uint32_t last_[SLOTS] = {};
  bool used_[SLOTS] = {};
};

}  // namespace fc
