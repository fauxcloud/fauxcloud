// capture.cpp -- see capture.h.
#include "capture.h"

#include <cctype>
#include <cstdio>
#include <cstring>

#include "tls_sni.h"
#include "web_render.h"

namespace fc {

const uint16_t CAPTURE_PORTS[CAPTURE_PORT_N] = {443, 8080, 8443, 8000, 8880, 1883, 8883, 5000};

static const char* const KIND_NAMES[] = {"join", "lease", "leave", "ntp", "tcp", "tls", "http", "hdr", "body", "mqtt", "raw"};
static_assert(sizeof KIND_NAMES / sizeof KIND_NAMES[0] == (size_t)CapKind::COUNT, "a name per kind");
const char* capKindName(CapKind k) { return k < CapKind::COUNT ? KIND_NAMES[(int)k] : ""; }

void CapUnit::add(CapKind k, const char* text) {
  if (n >= CAPTURE_UNIT_MAX) return;
  it[n].kind = k;
  snprintf(it[n].text, sizeof it[n].text, "%s", text);
  n++;
}

// ---------------------------------------------------------------- small text helpers
// A bounded appender: never writes past cap - 1, always terminated.
struct Buf {
  char* p;
  size_t cap, n;
  Buf(char* out, size_t c) : p(out), cap(c), n(0) {
    if (cap) p[0] = 0;
  }
  bool room(size_t k) const { return cap && n + k < cap; }
  void put(const char* s, size_t k) {
    if (!cap) return;
    if (n + k >= cap) k = cap - 1 - n;
    memcpy(p + n, s, k);
    n += k;
    p[n] = 0;
  }
  void put(const char* s) { put(s, strlen(s)); }
  void ch(char c) { put(&c, 1); }
};

// ASCII case-insensitive equality of n bytes / of two strings
static bool ncaseEq(const char* a, const char* b, size_t n) {
  for (size_t i = 0; i < n; i++)
    if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i])) return false;
  return true;
}
static bool caseEq(const char* a, const char* b) { return strlen(a) == strlen(b) && ncaseEq(a, b, strlen(a)); }

static int hexv(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

void capPrintable(char* s) {
  for (; *s; s++)
    if ((unsigned char)*s < 0x20 || (unsigned char)*s >= 0x7f) *s = '?';
}

// ---------------------------------------------------------------- redaction
bool capSecretKey(const char* k, size_t n) {
  char low[48];
  size_t m = 0;
  for (size_t i = 0; i < n && m + 1 < sizeof low; i++) {  // %-decoded, lower case (a longer key: its head decides)
    char c = k[i];
    if (c == '%' && i + 2 < n && hexv(k[i + 1]) >= 0 && hexv(k[i + 2]) >= 0) {
      c = (char)(hexv(k[i + 1]) * 16 + hexv(k[i + 2]));
      i += 2;
    } else if (c == '+') {
      c = ' ';
    }
    low[m++] = (char)tolower((unsigned char)c);
  }
  low[m] = 0;
  if (!m) return false;
  static const char* const PART[] = {"key", "pass", "pwd", "token", "secret", "auth", "sign", "cred"};
  for (const char* p : PART)
    if (strstr(low, p)) return true;
  static const char* const WHOLE[] = {"id",     "uid",       "did", "devid", "deviceid", "device_id", "mac",
                                      "sn",     "serial",    "wid", "user",  "username", "login",     "imei"};
  for (const char* w : WHOLE)
    if (strcmp(low, w) == 0) return true;
  static const char* const WHOLE2[] = {"sid", "siteid", "stationid", "clientid"};
  for (const char* w : WHOLE2)
    if (strcmp(low, w) == 0) return true;
  // "<anything>_id" / "<anything>-id" (never a bare ending "id": humid)
  if (m > 3 && low[m - 2] == 'i' && low[m - 1] == 'd' && (low[m - 3] == '_' || low[m - 3] == '-')) return true;
  return false;
}

size_t capRedactQuery(const char* in, size_t n, char* out, size_t cap) {
  Buf b(out, cap);
  size_t i = 0;
  while (i < n) {
    size_t j = i;
    while (j < n && in[j] != '&') j++;  // one pair [i, j)
    size_t eq = i;
    while (eq < j && in[eq] != '=') eq++;
    if (eq < j && capSecretKey(in + i, eq - i)) {
      b.put(in + i, eq - i + 1);
      b.put("***");
    } else {
      b.put(in + i, j - i);
    }
    if (j < n) b.ch('&');
    i = j + 1;
  }
  return b.n;
}

static bool pathKeySegment(const char* s, size_t n) {
  static const char* const SEG[] = {"wid", "key", "id", "token", "user", "pass", "station", "device", "dev", "sn", "serial", "mac"};
  for (const char* w : SEG)
    if (strlen(w) == n && ncaseEq(s, w, n)) return true;
  return false;
}

static bool tokenSegment(const char* s, size_t n) {
  if (n < 16) return false;
  bool digit = false, alpha = false;
  for (size_t i = 0; i < n; i++) {
    const unsigned char c = (unsigned char)s[i];
    if (c >= '0' && c <= '9') digit = true;
    else if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) alpha = true;
    else if (c != '-' && c != '_') return false;
  }
  return digit && alpha;
}

size_t capRedactPath(const char* in, size_t n, char* out, size_t cap) {
  Buf b(out, cap);
  size_t i = 0;
  bool hide = false;
  while (i < n) {
    if (in[i] == '/') {
      b.ch('/');
      i++;
      continue;
    }
    size_t j = i;
    while (j < n && in[j] != '/') j++;
    if (hide || tokenSegment(in + i, j - i)) b.put("***");
    else b.put(in + i, j - i);
    hide = pathKeySegment(in + i, j - i);
    i = j;
  }
  return b.n;
}

// the end of a JSON string starting at the quote at `i` (the index past the closing quote), or n when it is cut
static size_t jsonStrEnd(const char* in, size_t n, size_t i) {
  for (size_t j = i + 1; j < n; j++) {
    if (in[j] == '\\') {
      j++;
      continue;
    }
    if (in[j] == '"') return j + 1;
  }
  return n;
}

// the end of a JSON value starting at `i` (strings, nested objects and arrays skipped whole), or n when it is cut
static size_t jsonValueEnd(const char* in, size_t n, size_t i) {
  if (i >= n) return n;
  if (in[i] == '"') return jsonStrEnd(in, n, i);
  if (in[i] == '{' || in[i] == '[') {
    int depth = 0;
    for (size_t j = i; j < n; j++) {
      if (in[j] == '"') {
        j = jsonStrEnd(in, n, j) - 1;
        continue;
      }
      if (in[j] == '{' || in[j] == '[') depth++;
      if (in[j] == '}' || in[j] == ']') {
        if (--depth == 0) return j + 1;
      }
    }
    return n;
  }
  size_t j = i;
  while (j < n && in[j] != ',' && in[j] != '}' && in[j] != ']' && !isspace((unsigned char)in[j])) j++;
  return j;
}

size_t capRedactJson(const char* in, size_t n, char* out, size_t cap) {
  Buf b(out, cap);
  size_t i = 0;
  while (i < n) {
    if (in[i] != '"') {
      b.ch(in[i++]);
      continue;
    }
    const size_t e = jsonStrEnd(in, n, i);
    b.put(in + i, e - i);
    if (e >= n || in[e - 1] != '"' || e - i < 2) {  // a string cut by the end: nothing after it
      i = e;
      continue;
    }
    size_t k = e;
    while (k < n && isspace((unsigned char)in[k])) k++;
    if (k < n && in[k] == ':' && capSecretKey(in + i + 1, e - i - 2)) {
      b.ch(':');
      k++;
      while (k < n && isspace((unsigned char)in[k])) k++;
      b.put("***");
      i = jsonValueEnd(in, n, k);  // the value, whole or cut by the end, is never written
      continue;
    }
    i = e;
  }
  return b.n;
}

size_t capRedactUri(const char* uri, char* out, size_t cap) {
  const char* q = strchr(uri, '?');
  const size_t pn = q ? (size_t)(q - uri) : strlen(uri);
  size_t n = capRedactPath(uri, pn, out, cap);
  if (q && n + 1 < cap) {
    out[n++] = '?';
    out[n] = 0;
    n += capRedactQuery(q + 1, strlen(q + 1), out + n, cap - n);
  }
  return n;
}

// ---------------------------------------------------------------- the vendor prefix
void capVendor(const uint8_t mac[6], char out[9]) {
  if (mac[0] & 0x02) snprintf(out, 9, "random");  // a locally administered (randomised) address names no maker
  else snprintf(out, 9, "%02x:%02x:%02x", mac[0], mac[1], mac[2]);
}

// ---------------------------------------------------------------- HTTP
const CapHdrName CAPTURE_HDRS[CAPTURE_HDR_N] = {
    {"Host", "host"},          {"User-Agent", "ua"},          {"Content-Type", "ct"},      {"Content-Length", "cl"},
    {"Accept", "accept"},      {"Accept-Encoding", "enc"},    {"Connection", "conn"},      {"Transfer-Encoding", "te"},
    {"Expect", "expect"},      {"Cache-Control", "cache"},    {"X-Requested-With", "xrw"}, {"Authorization", "auth"},
    {"Cookie", "cookie"},      {"X-Api-Key", "x-api-key"},    {"X-Auth-Token", "x-auth-token"},
};
static bool hiddenHeader(const char* name, size_t n) {
  return capSecretKey(name, n) || (n == 6 && ncaseEq(name, "cookie", 6));
}

// "k=v" (or k="v w" when the value has a space), printable, into b unless it does not fit whole (then false)
static bool putPair(Buf& b, const char* k, const char* v, size_t vn, bool hide, size_t limit) {
  char val[160];
  size_t m = 0;
  if (hide) m = (size_t)snprintf(val, sizeof val, "***");
  else {
    for (size_t i = 0; i < vn && m + 1 < sizeof val; i++) val[m++] = v[i];
    val[m] = 0;
    capPrintable(val);
  }
  const bool q = !hide && (strchr(val, ' ') || !m);
  const size_t need = (b.n ? 1 : 0) + strlen(k) + 1 + m + (q ? 2 : 0);
  if (b.n + need > limit) return false;
  if (b.n) b.ch(' ');
  b.put(k);
  b.ch('=');
  if (q) b.ch('"');
  b.put(val, m);
  if (q) b.ch('"');
  return true;
}

// The request line as `http` records: one, or parts joined by "..." when longer than a record.
static void httpLine(const char* line, CapUnit* u) {
  const size_t len = strlen(line), room = CAPTURE_TEXT - 1;
  if (len <= room) {
    u->add(CapKind::Http, line);
    return;
  }
  char part[CAPTURE_TEXT];
  size_t at = 0;
  for (int k = 0; k < 3 && at < len; k++) {
    const bool first = k == 0, last = k == 2;
    const size_t body = room - (first ? 0 : 3) - 3;  // "..." before (not the first) and after (unless it fits)
    const size_t take = len - at <= room - (first ? 0 : 3) ? len - at : body;
    Buf b(part, sizeof part);
    if (!first) b.put("...");
    b.put(line + at, take);
    at += take;
    if (at < len) b.put("...");
    u->add(CapKind::Http, part);
    if (last) break;
  }
}

void capBodyText(const uint8_t* d, size_t n, size_t total, char* out, size_t cap) {
  if (!total) total = n;
  bool text = n > 0;
  for (size_t i = 0; i < n && text; i++)
    if ((d[i] < 0x20 && d[i] != '\r' && d[i] != '\n' && d[i] != '\t') || d[i] == 0x7f) text = false;  // UTF-8 is text
  Buf b(out, cap);
  char head[64];
  if (!text) {
    const size_t k = n < CAPTURE_BODY_HEX ? n : CAPTURE_BODY_HEX;
    snprintf(head, sizeof head, k < total ? "hex %u of %uB:" : "hex %uB:", (unsigned)k, (unsigned)total);
    b.put(head);
    for (size_t i = 0; i < k; i++) {
      char h[4];
      snprintf(h, sizeof h, " %02x", d[i]);
      b.put(h);
    }
    return;
  }
  char raw[CAPTURE_BODY_HEAD + 1], red[CAPTURE_BODY_HEAD * 2];
  const size_t k = n < CAPTURE_BODY_HEAD ? n : CAPTURE_BODY_HEAD;
  for (size_t i = 0; i < k; i++) raw[i] = (d[i] == '\r' || d[i] == '\n' || d[i] == '\t') ? ' ' : (char)d[i];
  raw[k] = 0;
  size_t s = 0;
  while (s < k && raw[s] == ' ') s++;
  const bool json = s < k && (raw[s] == '{' || raw[s] == '[');
  const bool form = !json && memchr(raw, '=', k) && !memchr(raw, ' ', k);
  if (json) capRedactJson(raw, k, red, sizeof red);
  else capRedactQuery(raw, k, red, sizeof red);  // a form, or free text: a "key=value" in it is hidden all the same
  capPrintable(red);
  snprintf(head, sizeof head, "%s %uB: ", json ? "json" : form ? "form" : "text", (unsigned)total);
  b.put(head);
  b.put(red);
}

void capHttpUnit(const CapHttpIn& in, CapUnit* u) {
  u->n = 0;
  if (in.over_limits) {
    u->add(CapKind::Http, in.headers_over ? "request over the parser's limits (headers)" : "request over the parser's limits (URI 512 B)");
    return;
  }
  char line[720];  // the method, then the URI redacted in place (one buffer: the httpd task's stack)
  const int k = snprintf(line, sizeof line, "%s ", in.method ? in.method : "?");
  capRedactUri(in.uri ? in.uri : "", line + k, sizeof line - (size_t)k);
  capPrintable(line);
  httpLine(line, u);
  char h[CAPTURE_TEXT];
  Buf b(h, sizeof h);
  const char* word = in.closed_before_reply ? "closed-before-reply" : nullptr;
  const size_t limit = CAPTURE_TEXT - 1 - (word ? strlen(word) + 1 : 0) - 4;  // room for the word and " ..."
  bool cut = false;
  for (int i = 0; i < CAPTURE_HDR_N && !cut; i++) {
    const char* v = in.hdr[i];
    if (!v) continue;
    cut = !putPair(b, CAPTURE_HDRS[i].key, v, strlen(v), i >= 11, limit);
  }
  if (cut) b.put(" ...");
  if (word) {
    if (b.n) b.ch(' ');
    b.put(word);
  }
  if (b.n) u->add(CapKind::Hdr, h);
  if (in.chunked) u->add(CapKind::Body, "chunked, not read");
  else if (in.content_len && !in.body_len) {
    char t[64];
    snprintf(t, sizeof t, "%uB announced, none arrived in 3 s", (unsigned)in.content_len);
    u->add(CapKind::Body, t);
  } else if (in.body_len) {
    char t[CAPTURE_TEXT];
    capBodyText(in.body, in.body_len, in.content_len, t, sizeof t);
    u->add(CapKind::Body, t);
  }
}

// ---------------------------------------------------------------- the peek ports
bool capMqttConnect(const uint8_t* d, size_t n, char* out, size_t cap) {
  if (n < 2 || d[0] != 0x10) return false;
  size_t i = 1;
  for (int k = 0; k < 4 && i < n; k++)  // remaining length: a varint
    if (!(d[i++] & 0x80)) break;
  if (i + 2 > n) return false;
  const size_t pl = (size_t)d[i] << 8 | d[i + 1];
  i += 2;
  if (i + pl > n || !((pl == 4 && !memcmp(d + i, "MQTT", 4)) || (pl == 6 && !memcmp(d + i, "MQIsdp", 6)))) return false;
  i += pl;
  if (i + 4 > n) {
    snprintf(out, cap, "MQTT (cut)");
    return true;
  }
  const uint8_t level = d[i], flags = d[i + 1];
  const unsigned keep = (unsigned)d[i + 2] << 8 | d[i + 3];
  i += 4;
  if (level >= 5 && i < n) {  // MQTT 5: properties (a varint length, then that many bytes) before the client id
    size_t plen = 0, mul = 1, k = 0;
    for (; k < 4 && i < n; k++, mul *= 128) {
      plen += (d[i] & 0x7f) * mul;
      if (!(d[i++] & 0x80)) break;
    }
    i += plen;
  }
  char cid[16];
  if (i + 2 <= n) snprintf(cid, sizeof cid, "%uB", (unsigned)d[i] << 8 | d[i + 1]);
  else snprintf(cid, sizeof cid, "?");
  char fl[40];
  Buf f(fl, sizeof fl);
  if (flags & 0x80) f.put("user");
  if (flags & 0x40) f.put(f.n ? ",pass" : "pass");
  if (flags & 0x04) f.put(f.n ? ",will" : "will");
  if (flags & 0x02) f.put(f.n ? ",clean" : "clean");
  if (!f.n) f.put("-");
  const char* ver = level == 3 ? "3.1" : level == 4 ? "3.1.1" : level == 5 ? "5" : "?";
  snprintf(out, cap, "MQTT %s flags=%s keepalive=%u clientid=%s", ver, fl, keep, cid);
  return true;
}

// An HTTP request in raw bytes (a peek port): the request line, every header, the body's head.
static bool httpFromBytes(uint16_t port, const uint8_t* d, size_t n, CapUnit* u) {
  size_t m = 0;
  while (m < n && m < 8 && d[m] >= 'A' && d[m] <= 'Z') m++;
  if (m < 3 || m >= n || d[m] != ' ') return false;
  size_t eol = 0;
  while (eol < n && d[eol] != '\r' && d[eol] != '\n') eol++;
  char raw[600];
  const size_t ln = eol < sizeof raw - 1 ? eol : sizeof raw - 1;
  memcpy(raw, d, ln);
  raw[ln] = 0;
  char* sp = strchr(raw, ' ');
  char* uri = sp + 1;
  char* sp2 = strchr(uri, ' ');  // " HTTP/1.1"
  if (sp2) *sp2 = 0;
  *sp = 0;
  char line[720];
  const int k = snprintf(line, sizeof line, ":%u %s ", port, raw);
  capRedactUri(uri, line + k, sizeof line - (size_t)k);
  capPrintable(line);
  httpLine(line, u);
  // headers: "Name: value" lines until the empty line
  size_t i = eol;
  if (i < n && d[i] == '\r') i++;
  if (i < n && d[i] == '\n') i++;
  char h[CAPTURE_TEXT];
  Buf b(h, sizeof h);
  bool cut = false, ended = false;
  size_t clen = 0;
  while (i < n) {
    size_t e = i;
    while (e < n && d[e] != '\r' && d[e] != '\n') e++;
    if (e == i) {  // the empty line: the body follows
      ended = true;
      if (i < n && d[i] == '\r') i++;
      if (i < n && d[i] == '\n') i++;
      break;
    }
    if (e >= n) {  // a header cut by the end of the peek
      cut = true;
      break;
    }
    const char* l = (const char*)d + i;
    const char* c = (const char*)memchr(l, ':', e - i);
    if (c && !cut) {
      char key[40];
      size_t kn = (size_t)(c - l) < sizeof key - 1 ? (size_t)(c - l) : sizeof key - 1;
      for (size_t x = 0; x < kn; x++) key[x] = (char)tolower((unsigned char)l[x]);
      key[kn] = 0;
      const char* v = c + 1;
      while (v < (const char*)d + e && *v == ' ') v++;
      const size_t vn = (size_t)((const char*)d + e - v);
      if (strcmp(key, "content-length") == 0) {
        clen = 0;
        for (size_t x = 0; x < vn && v[x] >= '0' && v[x] <= '9' && clen < 1000000; x++) clen = clen * 10 + (size_t)(v[x] - '0');
      }
      cut = !putPair(b, key, v, vn, hiddenHeader(key, kn), CAPTURE_TEXT - 1 - 4);
    }
    i = e;
    if (i < n && d[i] == '\r') i++;
    if (i < n && d[i] == '\n') i++;
  }
  if (cut) b.put(" ...");
  if (b.n) u->add(CapKind::Hdr, h);
  if (ended && i < n) {
    char t[CAPTURE_TEXT];
    capBodyText(d + i, n - i, clen, t, sizeof t);
    u->add(CapKind::Body, t);
  }
  return true;
}

void capPeekUnit(const CapPeekIn& in, CapUnit* u) {
  u->n = 0;
  char t[CAPTURE_TEXT];
  snprintf(t, sizeof t, ":%u%s", in.port, in.busy ? " busy, closed" : in.n ? "" : " no data");
  u->add(CapKind::Tcp, t);
  if (in.busy || !in.n) return;
  if (in.d[0] == 0x16) {
    TlsHello hh;
    if (tlsHello(in.d, in.n, &hh)) {
      const int k = snprintf(t, sizeof t, ":%u ", in.port);
      tlsHelloText(hh, t + k, sizeof t - (size_t)k);
      u->add(CapKind::Tls, t);
      return;
    }
  }
  if (httpFromBytes(in.port, in.d, in.n, u)) return;
  char m[96];
  if (capMqttConnect(in.d, in.n, m, sizeof m)) {
    snprintf(t, sizeof t, ":%u %s", in.port, m);
    u->add(CapKind::Mqtt, t);
    return;
  }
  Buf b(t, sizeof t);
  char head[32];
  const size_t k = in.n < CAPTURE_RAW_HEX ? in.n : CAPTURE_RAW_HEX;
  snprintf(head, sizeof head, ":%u %u%sB:", in.port, (unsigned)in.n, in.cut ? "+" : "");
  b.put(head);
  for (size_t i = 0; i < k; i++) {
    char h[4];
    snprintf(h, sizeof h, " %02x", in.d[i]);
    b.put(h);
  }
  u->add(CapKind::Raw, t);
}

// ---------------------------------------------------------------- the ring
void CaptureRing::reset() {
  memset(r_, 0, sizeof r_);
  seq_ = served_ = lost_ = written_ = 0;
}

int CaptureRing::find(uint32_t id) const {
  if (!id) return -1;
  for (int i = 0; i < CAPTURE_SLOTS; i++)
    if (r_[i].id == id) return i;
  return -1;
}

int CaptureRing::count() const {
  int c = 0;
  for (const CaptureRec& r : r_) c += r.id != 0;
  return c;
}

void CaptureRing::put(const CapUnit& u, uint8_t client, uint32_t t) {
  CapKind kinds[CAPTURE_UNIT_MAX];
  const char* texts[CAPTURE_UNIT_MAX];
  const int n = u.n < CAPTURE_UNIT_MAX ? u.n : CAPTURE_UNIT_MAX;
  for (int i = 0; i < n; i++) kinds[i] = u.it[i].kind, texts[i] = u.it[i].text;
  putItems(n, kinds, texts, client, t);
}

// the text into a record: bounded, terminated (no printf: callers may hold a spinlock)
static void recText(char* dst, const char* src) {
  size_t k = 0;
  for (; k + 1 < CAPTURE_TEXT && src[k]; k++) dst[k] = src[k];
  dst[k] = 0;
}
static bool recSame(const char* rec, const char* src) {  // as recText would store it
  size_t k = 0;
  for (; k + 1 < CAPTURE_TEXT && src[k]; k++)
    if (rec[k] != src[k]) return false;
  return rec[k] == 0;
}

void CaptureRing::putItems(int n, const CapKind* kinds, const char* const* texts, uint8_t client, uint32_t t) {
  if (n <= 0) return;
  // a resident unit of the same size with the same kinds, texts and client: counted again
  for (int h = 0; h < CAPTURE_SLOTS; h++) {
    const CaptureRec& head = r_[h];
    if (!head.id || head.group != head.id || head.client != client) continue;
    bool same = true;
    for (int i = 0; i < n && same; i++) {
      const int k = find(head.id + (uint32_t)i);
      same = k >= 0 && r_[k].group == head.id && r_[k].kind == (uint8_t)kinds[i] && recSame(r_[k].text, texts[i]);
    }
    if (!same) continue;
    const int past = find(head.id + (uint32_t)n);
    if (past >= 0 && r_[past].group == head.id) continue;  // the resident unit is longer
    for (int i = 0; i < n; i++) {
      CaptureRec& r = r_[find(head.id + (uint32_t)i)];
      r.seq = ++seq_;
      if (r.n < UINT16_MAX) r.n++;
      r.t_last = t;
    }
    return;
  }
  uint32_t group = 0;
  for (int i = 0; i < n; i++) {
    int at = -1;
    for (int k = 0; k < CAPTURE_SLOTS && at < 0; k++)
      if (!r_[k].id) at = k;
    if (at < 0) {  // full: the record changed longest ago goes
      at = 0;
      for (int k = 1; k < CAPTURE_SLOTS; k++)
        if (r_[k].seq < r_[at].seq) at = k;
      if (r_[at].id > served_) lost_++;  // no GET returned it
    }
    CaptureRec& r = r_[at];
    r.id = r.seq = ++seq_;
    if (!group) group = r.id;
    r.group = group;
    r.t_first = r.t_last = t;
    r.n = 1;
    r.kind = (uint8_t)kinds[i];
    r.client = client;
    recText(r.text, texts[i]);
    written_++;
  }
}

void CaptureRing::served(uint32_t since) {
  if (since > served_) served_ = since > seq_ ? seq_ : since;
}

int CaptureRing::snapshot(uint32_t since, CaptureRec* out, int max, bool* more) const {
  int n = 0, total = 0;
  uint32_t after = since;
  for (const CaptureRec& r : r_) total += r.id && r.seq > since;
  while (n < max) {  // the next record by seq after `after`
    int best = -1;
    for (int k = 0; k < CAPTURE_SLOTS; k++)
      if (r_[k].id && r_[k].seq > after && (best < 0 || r_[k].seq < r_[best].seq)) best = k;
    if (best < 0) break;
    out[n++] = r_[best];
    after = r_[best].seq;
  }
  if (more) *more = total > n;
  return n;
}

// ---------------------------------------------------------------- DNS
void CaptureDns::reset() { memset(this, 0, sizeof *this); }

void CaptureDns::add(const char* name, uint16_t qtype, uint8_t client) {
  char nm[CAPTURE_DNS_NAME];  // no printf: the caller holds a spinlock
  const size_t len = strlen(name);
  if (len < sizeof nm) {
    memcpy(nm, name, len + 1);
  } else {
    memcpy(nm, name, sizeof nm - 3);
    nm[sizeof nm - 3] = nm[sizeof nm - 2] = '.';
    nm[sizeof nm - 1] = 0;
  }
  capPrintable(nm);
  const uint8_t bit = qtype == 1 ? 1 : qtype == 28 ? 2 : 4;
  gen++;
  for (int i = 0; i < n; i++) {
    if (!caseEq(e[i].name, nm)) continue;
    if (e[i].n < UINT16_MAX) e[i].n++;
    e[i].types |= bit;
    return;
  }
  if (n >= CAPTURE_DNS_N) {
    other++;
    return;
  }
  CapDnsEnt& x = e[n++];
  memcpy(x.name, nm, sizeof x.name);
  x.n = 1;
  x.types = bit;
  x.client = client;
}

// ---------------------------------------------------------------- the devices on the Wi-Fi
void CaptureClients::reset() { memset(this, 0, sizeof *this); }

static int byMac(const CaptureClients& c, const uint8_t mac[6]) {
  for (int i = 0; i < c.n; i++)
    if (!memcmp(c.c[i].mac, mac, 6)) return i;
  return -1;
}

void CaptureClients::join(const uint8_t mac[6], uint32_t t, bool before) {
  int i = byMac(*this, mac);
  if (i < 0) {
    if (n < CAPTURE_CLIENTS) {
      i = n++;
    } else {  // full: the device that left longest ago makes room; none left -> counted, not listed
      for (int k = 0; k < n; k++)
        if (c[k].gone && (i < 0 || c[k].left_ms < c[i].left_ms)) i = k;
      if (i < 0) {
        over++;
        return;
      }
    }
    memset(&c[i], 0, sizeof c[i]);
    memcpy(c[i].mac, mac, 6);
    c[i].joined_ms = t;
    c[i].before = before;
  }
  c[i].gone = false;  // a rejoin: back, its first join kept (the join / leave records tell the rest)
}

void CaptureClients::lease(uint32_t ip, const uint8_t mac[6], uint32_t t) {
  if (byMac(*this, mac) < 0) join(mac, t);
  const int i = byMac(*this, mac);
  if (i < 0) return;
  for (int k = 0; k < n; k++)
    if (k != i && c[k].ip == ip) c[k].ip = 0;  // the address moved to another device
  c[i].ip = ip;
}

void CaptureClients::leave(const uint8_t mac[6], uint32_t t) {
  const int i = byMac(*this, mac);
  if (i < 0) return;
  c[i].gone = true;
  c[i].left_ms = t;
}

void CaptureClients::count(uint32_t ip, bool active) {
  if (!ip) return;
  for (int i = 0; i < n; i++) {
    if (c[i].ip != ip) continue;
    if (c[i].n < UINT16_MAX) c[i].n++;
    c[i].active = c[i].active || active;
    return;
  }
}

void CaptureClients::vendor(char out[9]) const {
  out[0] = 0;
  for (int i = 0; i < n; i++)
    if (c[i].active) {
      capVendor(c[i].mac, out);
      return;
    }
}

// ---------------------------------------------------------------- the start / stop order
static const char* const ACT_NAMES[] = {"fwd_abort", "publish_offline", "mqtt_off", "alloc", "open", "log_start",
                                        "close", "free", "mqtt_on", "log_stop", "log_refused"};
const char* capActName(CapAct a) { return (size_t)a < sizeof ACT_NAMES / sizeof ACT_NAMES[0] ? ACT_NAMES[(int)a] : ""; }
static const char* const END_NAMES[] = {"", "stop", "idle", "restart", "heap"};
const char* capEndName(CapEnd e) { return (size_t)e < sizeof END_NAMES / sizeof END_NAMES[0] ? END_NAMES[(int)e] : ""; }

CapPress CaptureMachine::start(uint32_t now, bool ota, bool restart, uint32_t idle_ms) {
  if (st_ != CapSt::Off) return CapPress::On;
  if (ota) return CapPress::Ota;
  if (restart) return CapPress::Restart;
  st_ = CapSt::Drain;
  stop_ = aborted_ = offline_ = mqtt_off_ = alloc_ = open_ = false;
  end_ = CapEnd::None;
  t_start_ = t_step_ = poll_ms_ = now;
  t_on_ = 0;
  idle_ms_ = idle_ms < CAPTURE_IDLE_MIN_MS ? CAPTURE_IDLE_MIN_MS : idle_ms > CAPTURE_IDLE_MS ? CAPTURE_IDLE_MS : idle_ms;
  return CapPress::Ok;
}

CapActs CaptureMachine::finish(CapEnd why) {
  CapActs a = {};
  if (open_) a.add(CapAct::Close);
  if (alloc_) a.add(CapAct::Free);
  if (mqtt_off_ || offline_) a.add(CapAct::MqttOn);  // the client back, or "online" again over the retained "offline"
  a.add(why == CapEnd::Heap ? CapAct::LogRefused : CapAct::LogStop);
  st_ = CapSt::Off;
  stop_ = aborted_ = offline_ = mqtt_off_ = alloc_ = open_ = false;
  end_ = why;
  return a;
}

CapActs CaptureMachine::tick(const CapTickIn& in) {
  CapActs a = {};
  if (st_ == CapSt::Off) return a;
  if (stop_ || in.restart) return finish(in.restart ? CapEnd::Restart : CapEnd::Stop);
  const uint32_t now = in.now_ms;
  switch (st_) {
    case CapSt::Drain:
      if (!aborted_) {
        a.add(CapAct::FwdAbort);
        aborted_ = true;
        t_step_ = now;
      }
      if (in.net_idle || now - t_step_ >= CAPTURE_DRAIN_MS) {
        a.add(CapAct::PublishOffline);
        offline_ = true;
        st_ = CapSt::Offline;
        t_step_ = now;
      }
      break;
    case CapSt::Offline:
      if (in.offline_done || now - t_step_ >= CAPTURE_OFFLINE_MS) {
        a.add(CapAct::MqttOff);
        mqtt_off_ = true;
        st_ = CapSt::Gate;
        t_step_ = now;
      }
      break;
    case CapSt::Gate:
      if (!in.mqtt_gone && now - t_step_ < CAPTURE_MQTT_GONE_MS) break;
      if (!in.heap_ok) return finish(CapEnd::Heap);
      a.add(CapAct::Alloc);
      alloc_ = true;
      a.add(CapAct::Open);
      open_ = true;
      a.add(CapAct::LogStart);
      st_ = CapSt::On;
      t_on_ = poll_ms_ = now;
      break;
    case CapSt::On: {
      // a poll stamped after this tick read its clock (a GET on the other core) is "just now", never 49 days ago
      const int32_t gone = (int32_t)(now - poll_ms_);
      if (gone > 0 && (uint32_t)gone >= idle_ms_) return finish(CapEnd::Idle);
      break;
    }
    default:
      break;
  }
  return a;
}

CapActs CaptureMachine::allocFailed() {
  alloc_ = open_ = false;  // nothing was allocated, nothing opened
  return finish(CapEnd::Heap);
}

uint32_t CaptureMachine::idleLeft(uint32_t now) const {
  if (st_ != CapSt::On) return 0;
  const int32_t g = (int32_t)(now - poll_ms_);
  const uint32_t gone = g > 0 ? (uint32_t)g : 0;
  return gone >= idle_ms_ ? 0 : idle_ms_ - gone;
}

// ---------------------------------------------------------------- the GET body
static void ipText(uint32_t ip, char out[16]) {
  snprintf(out, 16, "%u.%u.%u.%u", (unsigned)(ip & 0xff), (unsigned)(ip >> 8 & 0xff), (unsigned)(ip >> 16 & 0xff),
           (unsigned)(ip >> 24));
}

void captureRenderJson(Out& o, const CaptureView& v) {
  o.raw("{\"state\":");
  o.jsonStr(v.state);
  o.raw(",\"ended\":");
  o.jsonStr(v.ended);
  o.raw(",\"blocked\":");
  o.jsonStr(v.blocked ? v.blocked : "");
  o.printf(",\"elapsed_s\":%lu,\"idle_left_s\":%lu,\"started_utc\":", (unsigned long)v.elapsed_s, (unsigned long)v.idle_left_s);
  if (v.started_utc) o.printf("%lu", (unsigned long)v.started_utc);
  else o.raw("null");
  o.printf(",\"clock\":%s,\"ntp\":{\"asked\":%lu,\"answered\":%lu},\"uploads_skipped\":%lu,\"vendor\":", v.clock ? "true" : "false",
           (unsigned long)v.ntp_asked, (unsigned long)v.ntp_answered, (unsigned long)v.uploads_skipped);
  if (v.vendor[0]) o.jsonStr(v.vendor);
  else o.raw("null");
  o.printf(",\"seq\":%lu,\"lost\":%lu,\"more\":%s,\"clients\":[", (unsigned long)v.seq, (unsigned long)v.lost, v.more ? "true" : "false");
  if (v.clients)
    for (int i = 0; i < v.clients->n; i++) {
      const CapClient& c = v.clients->c[i];
      char ip[16], ven[9];
      ipText(c.ip, ip);
      capVendor(c.mac, ven);
      o.raw(i ? ",{\"ip\":" : "{\"ip\":");
      if (c.ip) o.jsonStr(ip);
      else o.raw("null");
      o.raw(",\"vendor\":");
      o.jsonStr(ven);
      if (c.before) o.raw(",\"joined_s\":null");
      else o.printf(",\"joined_s\":%lu", (unsigned long)(c.joined_ms / 1000));
      if (c.gone) o.printf(",\"left_s\":%lu", (unsigned long)(c.left_ms / 1000));
      else o.raw(",\"left_s\":null");
      o.printf(",\"n\":%u,\"active\":%s}", (unsigned)c.n, c.active ? "true" : "false");
    }
  o.printf("],\"clients_over\":%lu,\"dns\":{\"other\":%lu,\"names\":[", (unsigned long)(v.clients ? v.clients->over : 0),
           (unsigned long)(v.dns ? v.dns->other : 0));
  if (v.dns)
    for (int i = 0; i < v.dns->n; i++) {
      const CapDnsEnt& d = v.dns->e[i];
      o.raw(i ? ",{\"name\":" : "{\"name\":");
      o.jsonStr(d.name);
      char ty[16];
      Buf t(ty, sizeof ty);
      if (d.types & 1) t.put("A");
      if (d.types & 2) t.put(t.n ? ",AAAA" : "AAAA");
      if (d.types & 4) t.put(t.n ? ",other" : "other");
      o.printf(",\"n\":%u,\"types\":\"%s\",\"client\":%u}", (unsigned)d.n, ty, (unsigned)d.client);
    }
  o.raw("]},\"events\":[");
  for (int i = 0; i < v.nrecs; i++) {
    const CaptureRec& r = v.recs[i];
    o.printf("%s{\"id\":%lu,\"seq\":%lu,\"group\":%lu,\"kind\":\"%s\",\"client\":%u,\"n\":%u,\"t_first_s\":%lu,\"t_last_s\":%lu,\"text\":",
             i ? "," : "", (unsigned long)r.id, (unsigned long)r.seq, (unsigned long)r.group, capKindName((CapKind)r.kind),
             (unsigned)r.client, (unsigned)r.n, (unsigned long)(r.t_first / 1000), (unsigned long)(r.t_last / 1000));
    o.jsonStr(r.text);
    o.raw("}");
  }
  o.raw("]}");
}

}  // namespace fc
