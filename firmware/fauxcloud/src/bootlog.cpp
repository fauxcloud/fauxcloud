// bootlog.cpp -- see bootlog.h.
#include "bootlog.h"

#include <cstdio>
#include <cstring>

#include "web_render.h"

namespace fc {

namespace {
constexpr char CUT[] = " [cut]\n";
constexpr size_t CUT_LEN = sizeof CUT - 1;

char lower(char c) { return c >= 'A' && c <= 'Z' ? (char)(c - 'A' + 'a') : c; }
bool same(const char* a, const char* b, size_t n, bool icase) {
  for (size_t i = 0; i < n; i++)
    if (icase ? lower(a[i]) != lower(b[i]) : a[i] != b[i]) return false;
  return true;
}

// The oldest tail line out: its length with the flag byte and '\n'.
void evictOne(BootLog& b) {
  const char flag = b.tail[b.tail_start];
  size_t k = 1;
  while (k < b.tail_len && b.tail[(b.tail_start + k) % BOOTLOG_TAIL] != '\n') k++;
  if (k < b.tail_len) k++;  // the '\n'
  b.tail_start = (uint16_t)((b.tail_start + k) % BOOTLOG_TAIL);
  b.tail_len = (uint16_t)(b.tail_len - k);
  if (flag != 'H') b.gap++;
}
}  // namespace

void bootlogReset(BootLog& b) { memset(&b, 0, sizeof b); }

size_t bootlogFormat(char* buf, size_t cap, uint64_t ms, const char* tag, bool truncated) {
  if (!buf || cap <= BOOTLOG_LINE_MAX) return 0;
  char pre[44];  // "<ms> [<tag>] ": up to 20 digits + a tag cut to 16 + 4
  size_t p = 0;
  char d[20];
  int nd = 0;
  do d[nd++] = (char)('0' + ms % 10), ms /= 10; while (ms && nd < 20);
  while (nd) pre[p++] = d[--nd];
  pre[p++] = ' ', pre[p++] = '[';
  for (const char* t = tag ? tag : ""; *t && p < 2 + 20 + 16; t++) pre[p++] = *t;
  pre[p++] = ']', pre[p++] = ' ';
  size_t len = 0;
  while (len < cap - 1 && buf[len]) len++;
  const bool cut = truncated || p + len + 1 > BOOTLOG_LINE_MAX;
  const size_t keep = cut ? (len < BOOTLOG_LINE_MAX - CUT_LEN - p ? len : BOOTLOG_LINE_MAX - CUT_LEN - p) : len;
  memmove(buf + p, buf, keep);
  memcpy(buf, pre, p);
  for (size_t i = p; i < p + keep; i++)  // one stored line per logf: a message's own line breaks become spaces
    if (buf[i] == '\n' || buf[i] == '\r') buf[i] = ' ';
  size_t n = p + keep;
  if (cut) memcpy(buf + n, CUT, CUT_LEN), n += CUT_LEN;
  else buf[n++] = '\n';
  buf[n] = 0;
  return n;
}

size_t bootlogLine(char* out, uint64_t ms, const char* tag, const char* msg, bool truncated) {
  char tmp[BOOTLOG_LINE_MAX * 2 + 1];  // host tests only: the message as logf's buffer would hold it
  size_t m = 0;
  for (const char* q = msg ? msg : ""; *q && m < sizeof tmp - 1; q++) tmp[m++] = *q;
  tmp[m] = 0;
  const size_t n = bootlogFormat(tmp, sizeof tmp, ms, tag, truncated);
  memcpy(out, tmp, n + 1);
  return n;
}

void bootlogPut(BootLog& b, uint64_t ms, const char* /*tag: not a filter*/, const char* line, size_t n, bool boot) {
  if (b.frozen) {
    b.frozen_lost++;
    return;
  }
  if (!n || n > BOOTLOG_LINE_MAX) return;
  bool inHead = false;
  if (!b.head_closed && ms >= BOOTLOG_HEAD_MS) b.head_closed = 1;
  // only Log::Boot lines; an Event never closes the head. The level alone decides, not the tag: the first upload's
  // Boot line (tag HTTP) reaches the head; REQ is never Boot
  if (!b.head_closed && boot) {
    if (b.head_len + n <= BOOTLOG_HEAD) {
      memcpy(b.head + b.head_len, line, n);
      b.head_len = (uint16_t)(b.head_len + n);
      inHead = true;
    } else {
      b.head_closed = 1;  // full: this line and every later one go to the tail only
    }
  }
  const size_t need = n + 1;  // the flag byte
  while (BOOTLOG_TAIL - b.tail_len < need) evictOne(b);
  size_t at = (b.tail_start + b.tail_len) % BOOTLOG_TAIL;
  b.tail[at] = inHead ? 'H' : ' ';
  for (size_t i = 0; i < n; i++) b.tail[(at + 1 + i) % BOOTLOG_TAIL] = line[i];
  b.tail_len = (uint16_t)(b.tail_len + need);
  b.lines++;
}

void bootlogAdd(BootLog& b, uint64_t ms, const char* tag, const char* msg, bool truncated, bool boot) {
  char line[BOOTLOG_LINE_MAX + 1];
  const size_t n = bootlogLine(line, ms, tag, msg, truncated);
  bootlogPut(b, ms, tag, line, n, boot);
}

void bootlogFreeze(BootLog& b) { b.frozen = 1; }

uint32_t bootlogThaw(BootLog& b) {
  const uint32_t n = b.frozen_lost;
  b.frozen = 0;
  b.frozen_lost = 0;
  return n;
}

void bootlogRender(Out& o, const BootLog& b) {
  o.write(b.head, b.head_len);
  if (b.gap) o.printf("--- %lu line(s) not kept here ---\n", (unsigned long)b.gap);
  else o.raw("--- latest lines ---\n");
  size_t off = 0;
  while (off < b.tail_len) {
    const size_t s = (b.tail_start + off) % BOOTLOG_TAIL;
    size_t k = 1;
    while (off + k < b.tail_len && b.tail[(s + k) % BOOTLOG_TAIL] != '\n') k++;
    if (off + k < b.tail_len) k++;  // the '\n'
    if (b.tail[s] != 'H') {         // the text after the flag, in up to two pieces around the wrap
      const size_t t = (s + 1) % BOOTLOG_TAIL, len = k - 1, first = len < BOOTLOG_TAIL - t ? len : BOOTLOG_TAIL - t;
      o.write(b.tail + t, first);
      if (first < len) o.write(b.tail, len - first);
    }
    off += k;
  }
}

// walks the tail; when `out` is given, appends each line's text (without its flag) to it. A damaged line fails the
// walk, unless `partial`: then the whole lines before it are the result (a crash inside a put, a brownout that kept
// only part of RTC memory). A damaged header always fails: the offsets themselves cannot be trusted then.
static bool walkTail(const BootLog& b, char* out, size_t cap, size_t* used, bool partial = false) {
  if (b.head_len > BOOTLOG_HEAD || b.tail_start >= BOOTLOG_TAIL || b.tail_len > BOOTLOG_TAIL || b.head_closed > 1 ||
      b.frozen > 1)
    return false;
  size_t off = 0, n = 0;
  while (off < b.tail_len) {
    const size_t s = (b.tail_start + off) % BOOTLOG_TAIL;
    bool bad = b.tail[s] != 'H' && b.tail[s] != ' ';
    size_t k = 1;
    while (!bad && off + k < b.tail_len && b.tail[(s + k) % BOOTLOG_TAIL] != '\n') {
      const unsigned char ch = (unsigned char)b.tail[(s + k) % BOOTLOG_TAIL];
      if (ch < 0x20 && ch != '\t') bad = true;   // control bytes: not a line of ours
      else if (++k > BOOTLOG_LINE_MAX) bad = true;  // flag + up to LINE_MAX - 1 text bytes before the '\n'
    }
    if (bad || off + k >= b.tail_len) {  // every stored line ends in '\n'
      if (!partial) return false;
      break;
    }
    k++;
    const size_t len = k - 1;  // the text and its '\n'
    if (out) {
      if (n + len > cap) {
        if (!partial) return false;
        break;
      }
      for (size_t i = 0; i < len; i++) out[n + i] = b.tail[(s + 1 + i) % BOOTLOG_TAIL];
    }
    n += len;
    off += k;
  }
  if (used) *used = n;
  return true;
}

bool bootlogSane(const BootLog& b) { return walkTail(b, nullptr, 0, nullptr); }

size_t bootlogKeepPrev(const BootLog& b, BootPrev& p, bool keep, uint32_t restart) {
  size_t n = 0;
  if (!keep || !walkTail(b, p.text, sizeof p.text, &n, true) || !n) {
    p.magic = 0, p.len = 0, p.reserved = 0, p.restart = BOOTPREV_RESTART_UNKNOWN;
    return 0;
  }
  p.magic = BOOTLOG_PREV_MAGIC, p.len = (uint16_t)n, p.reserved = 0, p.restart = restart;
  return n;
}

void bootlogRenderPrev(Out& o, const BootPrev& p, const char* reset, uint32_t fallback) {
  if (p.magic != BOOTLOG_PREV_MAGIC || !p.len || p.len > BOOTLOG_PREV) return;
  const uint32_t restart = p.restart != BOOTPREV_RESTART_UNKNOWN ? p.restart : fallback;
  o.printf("--- previous boot (reset: %s, restart #%lu) ---\n", reset ? reset : "unknown", (unsigned long)restart);
  o.write(p.text, p.len);
}

int bootlogRedact(char* s, size_t cap, const char* needle, const char* repl, bool icase, size_t nmax) {
  if (!s || !cap || !needle || !*needle) return 0;
  if (!repl) repl = "";
  size_t nl = 0;
  while (nl < nmax && needle[nl]) nl++;
  const size_t rl = strlen(repl), room = cap - 1;
  size_t len = 0;
  while (len < room && s[len]) len++;
  s[len] = 0;
  int count = 0;
  bool cut = len == room;  // full on entry: vsnprintf may already have cut the text inside a needle
  for (size_t i = 0; i + nl <= len;) {
    if (!same(s + i, needle, nl, icase)) {
      i++;
      continue;
    }
    const size_t rest = len - i - nl, put = rl < room - i ? rl : room - i;
    size_t keep = rest;
    if (i + put + keep > room) keep = room - i - put, cut = true;
    if (put < rl) cut = true;
    memmove(s + i + put, s + i + nl, keep);
    memcpy(s + i, repl, put);
    len = i + put + keep;
    s[len] = 0;
    i += put;
    count++;
  }
  if (cut)  // a dropped end may have started a needle: its kept beginning goes too
    for (size_t k = (nl - 1 < len ? nl - 1 : len); k > 0; k--)
      if (same(s + len - k, needle, k, icase)) {
        s[len - k] = 0;
        break;
      }
  return count;
}

}  // namespace fc
