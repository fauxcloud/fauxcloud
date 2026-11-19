// console.cpp -- see console.h.
#include "console.h"

#include <cctype>
#include <cstdio>
#include <cstring>

#include "web_render.h"
#include "wu_parse.h"

namespace fc {

void logSafe(char* s, bool keep_space) {
  for (; *s; s++)
    if ((unsigned char)*s < (keep_space ? 32 : 33) || (unsigned char)*s >= 127) *s = '?';
}

int pathPrefixLen(const char* uri, int segments) {
  int i = 0, seen = 0;
  for (; uri[i] && uri[i] != '?'; i++)
    if (uri[i] == '/' && i > 0 && ++seen == segments) break;
  return i;
}

void logUri(const char* uri, char* out, size_t cap) {
  if (!cap) return;
  const char* q = strchr(uri, '?');
  const int full = q ? (int)(q - uri) : (int)strlen(uri);
  const int room = (int)cap - 16;  // what the two markers need, so a cut never drops them
  int keep = pathPrefixLen(uri, 2);
  if (keep > room) keep = room > 0 ? room : 0;
  snprintf(out, cap, "%.*s%s%s", keep, uri, keep < full ? "/<more>" : "", q ? "?<query>" : "");
  logSafe(out);
}

// Is the raw (possibly URL-encoded) key `k` of length n one of the secret keys? A key too long to
// check (>= 24 bytes raw) counts as secret -- an encoded "PASSWORD" padded with %00s must not escape redaction.
static bool secretKey(const char* k, size_t n) {
  char dec[24];
  if (n >= sizeof dec) return true;
  memcpy(dec, k, n);
  dec[n] = 0;
  urlDecode(dec);
  for (char* p = dec; *p; p++) *p = (char)toupper((unsigned char)*p);
  return strcmp(dec, "ID") == 0 || strcmp(dec, "PASSWORD") == 0;
}

void redactRequestLine(const char* method, const char* uri, char* out, size_t cap) {
  size_t n = 0;
  auto put = [&](const char* s, size_t len) {
    if (n + 1 >= cap) return;
    if (len > cap - 1 - n) len = cap - 1 - n;
    memcpy(out + n, s, len);
    n += len;
  };
  put(method, strlen(method));
  put(" ", 1);
  const char* q = strchr(uri, '?');
  if (!q) {
    put(uri, strlen(uri));
    out[n] = 0;
    return;
  }
  put(uri, (size_t)(q - uri) + 1);
  const char* p = q + 1;
  while (*p) {
    const char* amp = strchr(p, '&');
    const char* end = amp ? amp : p + strlen(p);
    const char* eq = (const char*)memchr(p, '=', (size_t)(end - p));
    if (eq && secretKey(p, (size_t)(eq - p))) {
      put(p, (size_t)(eq - p) + 1);
      put("***", 3);
    } else {
      put(p, (size_t)(end - p));
    }
    if (!amp) break;
    put("&", 1);
    p = amp + 1;
  }
  out[n] = 0;
}

void ConsoleRing::push(const char* id, const char* redacted, uint32_t t, uint32_t seq) {
  Line& l = l_[head_];
  snprintf(l.id, sizeof l.id, "%s", id);
  l.seq = seq;
  l.t = t;
  snprintf(l.text, sizeof l.text, "%s", redacted);
  head_ = (uint8_t)((head_ + 1) % CONSOLE_LINES);
  if (count_ < CONSOLE_LINES) count_++;
}

void ConsoleRing::forget(const char* id) {
  for (Line& l : l_)
    if (strcmp(l.id, id) == 0) l.id[0] = 0;
}

uint32_t ConsoleRing::first(const char* id) const {
  for (int i = 0; i < count_; i++)
    if (strcmp(at(i).id, id) == 0) return at(i).seq;
  return 0;
}

uint32_t ConsoleRing::last(const char* id) const {
  for (int i = count_ - 1; i >= 0; i--)
    if (strcmp(at(i).id, id) == 0) return at(i).seq;
  return 0;
}

const ConsoleRing::Line& ConsoleRing::at(int i) const {
  const int oldest = (head_ + CONSOLE_LINES - count_) % CONSOLE_LINES;
  return l_[(oldest + i) % CONSOLE_LINES];
}

void renderConsole(Out& o, const ConsoleRing& c, const char* id, uint32_t since) {
  o.printf("{\"first\":%lu,\"last\":%lu,\"items\":[", (unsigned long)c.first(id), (unsigned long)c.last(id));
  bool firstItem = true;
  for (int i = 0; i < c.size(); i++) {
    const ConsoleRing::Line& l = c.at(i);
    if (l.seq <= since || strcmp(l.id, id) != 0) continue;
    o.printf("%s{\"seq\":%lu,\"t\":%lu,\"line\":", firstItem ? "" : ",", (unsigned long)l.seq, (unsigned long)l.t);
    o.jsonStr(l.text);
    o.raw("}");
    firstItem = false;
  }
  o.raw("]}");
}

}  // namespace fc
