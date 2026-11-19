// dns_wire.cpp -- see dns_wire.h.
#include "dns_wire.h"

#include <cstring>

namespace fc {

static uint16_t be16(const uint8_t* p) { return (uint16_t)(p[0] << 8 | p[1]); }

bool dnsParseQuery(const uint8_t* pkt, size_t len, DnsQuestion* q) {
  if (len < 12) return false;
  const uint8_t flags = pkt[2];
  if (flags & 0x80) return false;           // QR: this is a response
  if ((flags >> 3) & 0x0F) return false;    // opcode != QUERY
  if (be16(pkt + 4) != 1) return false;     // exactly one question

  q->id = be16(pkt);
  q->rd = flags & 0x01;
  size_t p = 12, n = 0;
  for (;;) {
    if (p >= len) return false;
    const uint8_t l = pkt[p++];
    if (l == 0) break;
    if (l & 0xC0) return false;  // compression pointer (or reserved label type) in a question
    if (p + l > len) return false;
    if (n + (n ? 1 : 0) + l >= DNS_NAME_MAX) return false;
    if (n) q->name[n++] = '.';
    memcpy(q->name + n, pkt + p, l);
    n += l;
    p += l;
  }
  q->name[n] = 0;
  if (p + 4 > len) return false;
  q->qtype = be16(pkt + p);
  q->qclass = be16(pkt + p + 2);
  q->qend = p + 4;
  return true;
}

size_t dnsBuildAnswer(const uint8_t* pkt, const DnsQuestion& q, const uint8_t ip[4], uint8_t* out, size_t cap) {
  const bool answerA = q.qtype == 1 && q.qclass == 1;
  const size_t need = q.qend + (answerA ? 16 : 0);
  if (cap < need) return 0;
  memcpy(out, pkt, q.qend);
  out[2] = (uint8_t)(0x80 | 0x04 | (q.rd ? 0x01 : 0));  // QR, AA, RD echoed
  out[3] = 0x80;                                        // RA, RCODE NOERROR
  out[4] = 0; out[5] = 1;                               // QDCOUNT
  out[6] = 0; out[7] = answerA ? 1 : 0;                 // ANCOUNT
  out[8] = out[9] = out[10] = out[11] = 0;              // NSCOUNT, ARCOUNT (EDNS dropped)
  size_t o = q.qend;
  if (answerA) {
    // name = pointer to the question name at offset 12, type A, class IN, TTL 60, RDLENGTH 4
    const uint8_t rr[12] = {0xC0, 0x0C, 0, 1, 0, 1, 0, 0, 0, 60, 0, 4};
    memcpy(out + o, rr, sizeof rr);
    o += sizeof rr;
    memcpy(out + o, ip, 4);
    o += 4;
  }
  return o;
}

static uint32_t nameHash(const char* s) {
  uint32_t h = 2166136261u;  // FNV-1a over the lower-cased name (resolvers may randomize case)
  for (; *s; s++) {
    char c = *s;
    if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    h = (h ^ (uint8_t)c) * 16777619u;
  }
  return h;
}

bool NameLogLimiter::shouldLog(const char* name, uint32_t now_s, bool* first) {
  const uint32_t h = nameHash(name);
  if (first) *first = false;
  int oldest = 0;
  for (int i = 0; i < SLOTS; i++) {
    if (used_[i] && hash_[i] == h) {
      if (now_s - last_[i] < WINDOW_S) return false;
      last_[i] = now_s;
      return true;
    }
    // Slots fill in order and are never freed, so past the first free slot there is nothing to find.
    if (!used_[i]) { oldest = i; break; }
    if (last_[i] < last_[oldest]) oldest = i;
  }
  // Not found: take the free slot, or evict the least recently logged name.
  if (first) *first = true;
  used_[oldest] = true;
  hash_[oldest] = h;
  last_[oldest] = now_s;
  return true;
}

}  // namespace fc
