// metrics.cpp -- see metrics.h.
#include "metrics.h"

#include <cstdio>
#include <cstring>

namespace fc {

void MinuteWindow::feed(uint32_t now_ms, uint32_t v) {
  const uint32_t minute = now_ms / 60000u;
  const int i = (int)(minute % MINUTES);
  if (!used_[i] || minute_[i] != minute) {
    used_[i] = true;
    minute_[i] = minute;
    v_[i] = v;
    return;
  }
  if (kind_ == Kind::Min ? v < v_[i] : v > v_[i]) v_[i] = v;
}

bool MinuteWindow::value(uint32_t now_ms, uint32_t* out, int minutes) const {
  const uint32_t minute = now_ms / 60000u;
  const uint32_t span = minutes < 1 ? 1u : minutes > MINUTES ? (uint32_t)MINUTES : (uint32_t)minutes;
  bool any = false;
  uint32_t best = 0;
  for (int i = 0; i < MINUTES; i++) {
    if (!used_[i] || minute - minute_[i] >= span) continue;
    if (!any || (kind_ == Kind::Min ? v_[i] < best : v_[i] > best)) best = v_[i];
    any = true;
  }
  if (any) *out = best;
  return any;
}

void makeEtag(char out[48], uint32_t boot, uint32_t a, uint32_t b, uint32_t c) {
  snprintf(out, 48, "\"%08lx-%lx-%lx-%lx\"", (unsigned long)boot, (unsigned long)a, (unsigned long)b,
           (unsigned long)c);
}

bool etagMatches(const char* inm, const char* etag) {
  if (!inm || !*inm) return false;
  const size_t n = strlen(etag);
  const char* p = inm;
  while (*p) {
    while (*p == ' ' || *p == ',') p++;
    if (*p == '*') return true;
    if (p[0] == 'W' && p[1] == '/') p += 2;  // weak comparison
    const char* end = strchr(p, ',');
    size_t len = end ? (size_t)(end - p) : strlen(p);
    while (len && p[len - 1] == ' ') len--;
    if (len == n && strncmp(p, etag, n) == 0) return true;
    if (!end) break;
    p = end + 1;
  }
  return false;
}

}  // namespace fc
