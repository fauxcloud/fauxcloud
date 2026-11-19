// bootlog.h -- every logf() line also lands in a fixed RAM log the admin can download
// (GET /api/admin/bootlog): a write-once HEAD from reset (the boot facts) plus a rolling TAIL (the latest lines). Pure C++
// over a caller-owned struct (the sketch keeps it in RTC slow memory, app::gBootLog, zeroed at every boot); no locking
// (the sketch's logf glue holds one spinlock around put / freeze / thaw and formats outside it), no heap, no Arduino.
#pragma once
#include <cstddef>
#include <cstdint>

namespace fc {

class Out;

constexpr size_t BOOTLOG_HEAD = 2048, BOOTLOG_TAIL = 2048;
constexpr size_t BOOTLOG_LINE_MAX = 200;      // bytes per stored line including the '\n'; longer ends in " [cut]\n"
constexpr uint64_t BOOTLOG_HEAD_MS = 120000;  // the head closes at this uptime (or when a line does not fit)

// The tail stores each line as one flag byte + the text + '\n': 'H' = the head holds this line too (the reader skips
// it, and its eviction is no loss), ' ' = tail only. The head stores plain lines.
struct BootLog {
  uint32_t lines;        // lines stored (frozen ones not counted)
  uint32_t gap;          // tail-only lines evicted from the tail: lost
  uint32_t frozen_lost;  // lines refused while frozen, handed back by bootlogThaw()
  uint16_t head_len, tail_start, tail_len;
  uint8_t head_closed, frozen;
  char head[BOOTLOG_HEAD];
  char tail[BOOTLOG_TAIL];
};

// the previous boot's tail across a restart. The sketch keeps one BootPrev in RTC slow memory after
// the boot log (the RTC layout is append-only); at boot, before the ring is reset, bootlogKeepPrev copies the tail's lines (oldest first,
// head copies included: the previous head is gone) as plain text -- only when the reset is not a power-on (RTC content
// is undefined then) and the header is sane; a damaged line ends the copy (the whole lines before it are kept), a
// damaged header or no whole line empties the block. RTC to RTC: no heap, no NVS.
// The block also carries the previous boot's own restart number (written to an RTC word by that boot once its NVS
// counter was read, copied here at the next boot), so the header names that boot even when the NVS counter's write
// failed or NVS was wiped since. Its layout version is its magic: PRV2 = with `restart` (appended at the end: the RTC
// layout is append-only).
constexpr size_t BOOTLOG_PREV = 2048;
constexpr uint32_t BOOTLOG_PREV_MAGIC = 0x32565250;  // "PRV2" (the layout without `restart` was "PREV")
constexpr uint32_t BOOTPREV_RESTART_UNKNOWN = 0xFFFFFFFFu;  // the previous boot wrote no number (an image before it)
struct BootPrev {
  uint32_t magic;  // BOOTLOG_PREV_MAGIC = text holds len bytes of whole lines
  uint16_t len;
  uint16_t reserved;
  char text[BOOTLOG_PREV];
  uint32_t restart;  // the previous boot's restart number, BOOTPREV_RESTART_UNKNOWN = not recorded
};
static_assert(sizeof(BootPrev) == 2060, "tools/rtc_layout.txt: app::gPrevTail");
// The ring's header in range and its tail a sequence of whole lines (a flag byte 'H' / ' ', <= BOOTLOG_LINE_MAX bytes of
// printable text, '\n'); anything else is RTC garbage (a cold plug, another image's layout).
bool bootlogSane(const BootLog& b);
// keep = the reset left RTC memory intact (not a power-on); restart = the previous boot's number from its RTC word
// (BOOTPREV_RESTART_UNKNOWN when it wrote none). Returns the bytes kept (0: emptied).
size_t bootlogKeepPrev(const BootLog& b, BootPrev& p, bool keep, uint32_t restart = BOOTPREV_RESTART_UNKNOWN);
// The download's last section: "--- previous boot (reset: <reset>, restart #<n>) ---" and the lines; nothing when empty.
// n = the number the block carries, else `fallback` (the NVS counter's previous value).
void bootlogRenderPrev(Out& o, const BootPrev& p, const char* reset, uint32_t fallback);

void bootlogReset(BootLog& b);
// Outside the lock, IN PLACE (no second line buffer on the caller's stack): `buf` holds the message
// (NUL-terminated, buffer cap > BOOTLOG_LINE_MAX) and becomes "<ms> [<tag>] <msg>\n", cut to BOOTLOG_LINE_MAX with
// " [cut]\n" when longer or when `truncated` (vsnprintf already cut the message). No printf. Returns the length.
size_t bootlogFormat(char* buf, size_t cap, uint64_t ms, const char* tag, bool truncated);
// The same into `out` (BOOTLOG_LINE_MAX + 1) from a separate message (the host tests).
size_t bootlogLine(char* out, uint64_t ms, const char* tag, const char* msg, bool truncated);
// Under the lock: stores one line from bootlogLine. The head takes only `boot` lines (Log::Boot, the explicit boot
// facts) until BOOTLOG_HEAD_MS or until one does not fit; every line goes to the tail; the level decides,
// not the tag (REQ lines are never Boot).
// While frozen only counts it.
void bootlogPut(BootLog& b, uint64_t ms, const char* tag, const char* line, size_t n, bool boot = true);
// bootlogLine + bootlogPut (the host tests).
void bootlogAdd(BootLog& b, uint64_t ms, const char* tag, const char* msg, bool truncated = false, bool boot = true);
// The reader (one at a time: the httpd worker) freezes the log, sends it straight from the buffers, then thaws it.
// bootlogThaw returns the lines refused meanwhile (0 = none); the caller logs "<n> line(s) not kept while the log was sent".
void bootlogFreeze(BootLog& b);
uint32_t bootlogThaw(BootLog& b);
// The body after the caller's header line: head lines, the separator ("--- <gap> line(s) not kept here ---" when
// gap > 0, else "--- latest lines ---"), then the tail lines the head does not hold, oldest first. Never a partial line.
void bootlogRender(Out& o, const BootLog& b);
// Replaces every occurrence of `needle` in `s` (NUL-terminated, buffer `cap`) by `repl`, in place; `icase` compares
// ASCII letters case-insensitively; the needle is read up to `nmax` bytes (a field another task may be rewriting is
// never read past its array). An empty needle is a no-op. When the result outgrows cap - 1 the end is dropped,
// and a trailing piece that starts the needle goes too (no part of a needle is ever left in). Returns the replacements.
int bootlogRedact(char* s, size_t cap, const char* needle, const char* repl, bool icase, size_t nmax = 64);

}  // namespace fc
