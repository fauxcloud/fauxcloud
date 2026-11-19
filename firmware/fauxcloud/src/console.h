// console.h -- the console ring (one ring for all stations, each line tagged
// with its station id): the last raw request lines the stations sent,
// for the admin's Console tab and for pasting into a compatibility issue. `ID` and `PASSWORD` values are replaced
// with `***` BEFORE a line is stored, so a stored line is always safe to publish. Plain C++, host-compilable.
#pragma once
#include <cstddef>
#include <cstdint>

namespace fc {

class Out;

constexpr int CONSOLE_LINES = 6;  // shared by every station (1.7 KB in all instead of 1.3 KB per station)
constexpr int CONSOLE_ID_LEN = 16;  // = STATION_ID_LEN
constexpr size_t CONSOLE_LINE_MAX = 528;  // "GET " + URI (CONFIG_HTTPD_MAX_URI_LEN 512) + terminator
// Stored lines are cut to 255 bytes AFTER redaction (a WU query is ~250 B; saves 5.4 KB at 4
// stations). The cut can only drop text, never expose a redacted value.
constexpr size_t CONSOLE_STORE_MAX = 256;

// "GET" + "/weatherstation/updateweatherstation.php?ID=ST1&PASSWORD=x&tempf=50" ->
// "GET /weatherstation/updateweatherstation.php?ID=***&PASSWORD=***&tempf=50". Keys match case-insensitively,
// also URL-encoded; everything else stays byte for byte. Truncated (never a secret) when longer than cap - 1.
void redactRequestLine(const char* method, const char* uri, char* out, size_t cap);

// a string from the network made safe for one log line, in place -- every byte outside 0x21..0x7e
// becomes '?' (DNS names, Host, User-Agent, a setup-mode URI), as tls_sni.cpp does for the SNI.
// keep_space: free text (a User-Agent) keeps its spaces; only control and non-ASCII bytes go.
void logSafe(char* s, bool keep_space = false);
// the length of the first `segments` path segments of a URI (query cut): "/v01/set/wid/X/key/Y" -> 8
// for 2 ("/v01/set"). Weathercloud's path-style upload carries the WID and the key in later segments.
int pathPrefixLen(const char* uri, int segments);
// a request URI as a log line may show it: the first two path segments, "/<more>" when the path goes on, "?<query>"
// when it has a query (a deeper path or a query may carry a console's secret), then logSafe. Cut to fit cap.
void logUri(const char* uri, char* out, size_t cap);

class ConsoleRing {
 public:
  struct Line {
    char id[CONSOLE_ID_LEN];  // station id ("" = forgotten)
    uint32_t seq;  // that station's upload counter for this request (gaps = lines not kept)
    uint32_t t;    // unix seconds (uptime seconds before SNTP)
    char text[CONSOLE_STORE_MAX];
  };
  void push(const char* id, const char* redacted, uint32_t t, uint32_t seq);  // cuts to CONSOLE_STORE_MAX - 1
  void forget(const char* id);    // a forgotten station's lines are not shown again
  uint32_t first(const char* id) const;  // oldest seq kept for that station (0 = none)
  uint32_t last(const char* id) const;   // newest seq kept for that station (0 = none)
  int size() const { return count_; }
  const Line& at(int i) const;    // 0 = oldest

 private:
  Line l_[CONSOLE_LINES];
  uint8_t head_ = 0, count_ = 0;
};

// {"first":n,"last":n,"items":[{"seq","t","line"}...]}: the lines of station `id` whose seq > since.
void renderConsole(Out& o, const ConsoleRing& c, const char* id, uint32_t since);

}  // namespace fc
