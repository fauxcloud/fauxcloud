// fwd.h -- forwarding to online weather services: targets, the clean record, the template variables and
// renderer, target JSON, the response parser and the enable plan (one station). Pure C++, host-compiled; the sketch side
// (net-out task, NVS, routes) is app_fwd.cpp, the scheduler is fwd_sched.h.
//
// The query strings are byte-equal to tools/fwd_reference.py (the oracle): the same variables, the same
// conversions in the same order of operations on doubles parsed back from the /api/live texts, and the template
// rules of its render() (Python 3.10+ parse_qsl + urlencode/quote_plus in the query, quote(safe="") in the path).
// test/tools/gen_fwd_vectors.py renders the vectors with fwd_reference.py itself; test/host/test_fwd.inc compares.
#pragma once
#include <cstddef>
#include <cstdint>

#include "config.h"
#include "fields.h"
#include "tz.h"
#include "wu_parse.h"

// Outbound TLS: FWD_TLS=1 is the release. It
// builds BearSSL in (src/tlsc, src/bearssl, vendored; the only outbound TLS path), with HTTPS per target and airport
// weather (METAR + TAF, /api/metar, the dashboard's airport view). FWD_TLS=0 is the HTTP-only fallback: no BearSSL, no
// anchors, no airport weather. It is a CI cell, not a download. Build it with: tools\build.ps1 fauxcloud -Define FWD_TLS=0
#ifndef FWD_TLS
#define FWD_TLS 1
#endif
// FC_DIAG_BENCH (the bench diagnostics) lives in diag_bench.h, next to this flag by inclusion
#include "diag_bench.h"
// whether a new target starts on HTTPS (FWD_TLS builds only). 1 (the default):
// a new target is preselected on HTTPS, the user may pick plain HTTP per target; a stored target keeps its own value.
#ifndef FWD_HTTPS_DEFAULT
#define FWD_HTTPS_DEFAULT 1
#endif

namespace fc {

class Out;

// ---------------------------------------------------------------- targets
enum class FwdType : uint8_t { Wu, Windy, WowBe, PwsWeather, WuCompat, Template, COUNT };
const char* fwdTypeName(FwdType t);  // wu windy wowbe pwsweather wu_compat template
bool fwdTypeFromName(const char* s, FwdType* out);
uint16_t fwdMinFloor(FwdType t);       // the service minimum: windy / pwsweather 300, the rest 60
// The send interval, fixed (no user choice): 5 minutes, or a service's own minimum + 10 s when that is not
// below 5 minutes (Windy, PWSweather: 310 s -- exactly 300 s met Windy's limit with 429s on network jitter).
uint16_t fwdIntervalS(FwdType t);
bool fwdIsPreset(FwdType t);           // wu windy wowbe pwsweather: fixed host, path, query and success rule

constexpr int FWD_SLOTS = 3;
constexpr size_t FWD_HOST_MAX = 64;     // [A-Za-z0-9.-], plus ":port"
constexpr size_t FWD_PATH_MAX = 128;
constexpr size_t FWD_URL_MAX = 384;
constexpr size_t FWD_SUCCESS_MAX = 16;
constexpr size_t FWD_ID_MAX = 64;
constexpr size_t FWD_KEY_MAX = 96;
constexpr size_t FWD_TARGET_MAX = 768;  // the rendered request target (path + query)
constexpr size_t FWD_NVS_JSON_MAX = 600;
constexpr uint16_t FWD_INTERVAL_S = 300, FWD_MARGIN_S = 10;
constexpr const char* FWD_WU_PATH = "/weatherstation/updateweatherstation.php";

struct FwdHostPath {
  char host[FWD_HOST_MAX + 7];  // host[:port]
  char path[FWD_PATH_MAX + 1];
};
struct FwdTarget {
  bool used;
  FwdType type;
  bool enabled;
  bool https;                          // presets / wu_compat; a template's scheme decides; always false if !FWD_TLS
  char station[STATION_ID_LEN];        // "" = not chosen yet (the caller fills the first station)
  union {                              // by type (RAM budget): never both
    char url[FWD_URL_MAX + 1];         // template
    FwdHostPath wc;                    // wu_compat
  };
  char success[FWD_SUCCESS_MAX + 1];   // wu_compat / template: "body contains", "" = 2xx only
  char id[FWD_ID_MAX + 1];
  uint16_t min_s;                      // the interval: always fwdIntervalS(type) (an old record's value is replaced)
  uint16_t max_s;                      // end of the send window: always 0 = the default min(min_s + 120, 2 min_s)
  bool verified;                       // an accepted "Send one now" with these settings (NVS "verified")
};
FwdTarget fwdDefaultTarget(FwdType t);
uint16_t fwdMaxS(const FwdTarget& t);  // the effective max_s
bool fwdKeyFollows(const FwdTarget& before, const FwdTarget& after);  // false: type/host/path/url changed (key cleared)

// What a PUT changed besides the target fields (the key is write-only and lives in its own NVS entry).
struct FwdChange {
  bool key_given;                      // "key" with a new value (not "", not "<set>")
  bool key_clear;                      // "key": null
  char key[FWD_KEY_MAX + 1];
  bool remove;                         // {"remove": true}
  bool any;                            // any target field changed
  bool has_verified, verified;         // the stored form's "verified" (fwdLoadStored); a PUT cannot set it
};
// Applies a (partial) JSON document; a bad key keeps the previous value and is reported (settingsFromJson style).
// `nvs` = the stored form: no "key", "remove", errors only logged. false = not a JSON object.
bool fwdFromJson(const char* json, size_t len, FwdTarget* t, Errors* err, FwdChange* ch);
void fwdToJson(Out& o, const FwdTarget& t);  // overrides of fwdDefaultTarget(t.type) + "https" always, never the key (NVS form)
// A stored target (NVS). Older records left "https" out when it equalled that build's default, which was HTTPS, so a
// record without the key means HTTPS (FWD_LEGACY_HTTPS; plain HTTP on a FWD_TLS=0 build anyway), never the current
// build's default: changing FWD_HTTPS_DEFAULT never flips a stored target.
constexpr bool FWD_LEGACY_HTTPS = true;
// a record without "verified" predates the flag (additive, never reset): verified if it was switched on, so a
// running service keeps running after the update.
bool fwdLoadStored(const char* json, size_t len, FwdTarget* t, Errors* err);
// adopt (adopt.h): the targets that send `from`'s readings send `to`'s; returns how many changed (bit i of *mask = target i)
int fwdRenameStation(FwdTarget* t, int n, const char* from, const char* to, uint32_t* mask);

// a target is switched on only after an accepted "Send one now" with its current settings.
// After fwdFromJson on a PUT: a change of type, id, key, host, path, URL or success text clears `verified` and with it the
// switch (a key typed again unchanged is no change: the PUT handler drops it first); a
// request to switch an unverified target on is refused with one error (FWD_VERIFY_FIRST) and the target stays off.
// true = this PUT cleared the verification.
extern const char* const FWD_VERIFY_FIRST;
bool fwdVerifyApply(const FwdTarget& before, FwdTarget* t, const FwdChange& ch, Errors* err);

// Template check at save: http(s)://host[:port]/path?query (http:// only if !FWD_TLS), no userinfo / fragment / spaces /
// control bytes, valid %-escapes and UTF-8, every {name} a known variable. false + why.
bool fwdTemplateValid(const char* url, char* why, size_t cap);
bool fwdHostValid(const char* host);  // [A-Za-z0-9.-]{1,64} (+ ":1..65535")
bool fwdPathValid(const char* path);  // "/" + printable ASCII, <= 128

// ---------------------------------------------------------------- the clean record
// The station's view, copied under gLock (snapshot only; rendering happens outside it).
struct FwdLiveIn {
  float v[F_COUNT];
  uint32_t present;   // Sample::present
  uint32_t hidden;    // cfg.disabled
  uint32_t warming;
  uint32_t field_t[F_COUNT];
  uint32_t last_seen;
  float wind10;       // mean of F_WIND_KMH over the recent ring's last 600 s (recentFieldMean), NaN = none
};
// For each kept field the double parsed back from its /api/live text (formatValue), plus wind10 likewise (1 decimal).
struct FwdRecord {
  double v[F_COUNT];
  uint32_t has;
  double wind10;
  bool has_wind10;
  uint32_t t;         // last_seen, unix
};
// Drops: hidden, warming, and fields whose age (last_seen - field_t) is unknown or > min_s. wind10 goes with a hidden
// or warming wind speed.
void fwdRecordBuild(const FwdLiveIn& in, uint32_t min_s, FwdRecord* r);

// The data gate (no send, counted nodata). In order of the check.
enum class FwdGate : uint8_t { Ok, NoStation, Offline, Stale, Restored, NoTime, Old, NoCore, Same };
const char* fwdGateName(FwdGate g);
struct FwdGateIn {
  bool exists, online, sensor_stale, restored, time_ok;
  uint32_t last_seen;
  bool has_temp, has_wind;   // after the drops
  uint32_t ok_seen;          // last_seen of the target's last accepted send (0 = none)
};
FwdGate fwdGate(const FwdGateIn& g, bool test);  // test = "Send one now": the same reading may go again

// ---------------------------------------------------------------- variables and rendering
struct FwdCtx {
  const FwdRecord* r;
  const char* id;
  const char* key;
  const char* software;  // "fauxcloud-<FC_VERSION>"
  const PosixTz* tz;     // the bridge zone (date_local / time_local)
  bool redact;           // {key} renders as a bare "***" (the test page's request target)
};
// The value of {name} (n bytes, not terminated): length, -1 absent (no reading), -2 not a variable name.
int fwdVar(const FwdCtx& c, const char* name, size_t n, char* buf, size_t cap);
bool fwdVarKnown(const char* name, size_t n);
const char* fwdVarName(int i);  // 0 .. FWD_VAR_COUNT - 1: the public, append-only names
extern const int FWD_VAR_COUNT;

enum class FwdErr : uint8_t { None, NoValue, TooLong, BadUrl, UnknownVar };
const char* fwdErrName(FwdErr e);
struct FwdUrl {
  bool https;
  char host[FWD_HOST_MAX + 7];  // host[:port], the Host header
  char why[40];                 // NoValue / UnknownVar: the name
};
// One template URL (fwd_reference.py render()): path placeholders strict, query pairs with an absent value dropped. The
// request target (path?query, <= 768 B) goes to `target` (the caller's buffer: the job's request line on the board).
FwdErr fwdRenderUrl(const char* tpl, const FwdCtx& c, FwdUrl* out, char* target, size_t cap);
// The target's query (preset / wu_compat / template), the https flag applied.
FwdErr fwdRender(const FwdTarget& t, const FwdCtx& c, FwdUrl* out, char* target, size_t cap);

// ---------------------------------------------------------------- results and the response
// NoMemory (FWD_TLS): the byte heap could not hold a TLS handshake -- counted like a heap skip, never retried
// inside the window, three in a row pause the target.
enum class FwdResult : uint8_t { None, Ok, Dup, Limited, Refused, Reject, Fail, TlsUntrusted, NoMemory };
const char* fwdResultName(FwdResult r);  // "" ok dup limited refused reject fail tls_untrusted no_memory

// Streaming parser of one HTTP/1.x response, with read caps: the status line always; headers (<= 1 KB,
// only Retry-After / Content-Length / Transfer-Encoding kept) only for a 429 or when the target needs the body; then
// <= 256 B of body (de-chunked); <= 1536 B in all. Windy and WOW-BE stop after the status line (their bodies are
// never read: WOW-BE echoes the site's coordinates).
class FwdResp {
 public:
  static constexpr size_t CAP_LINE = 128, CAP_HEAD = 1024, CAP_BODY = 256, CAP_TOTAL = 1536;
  void begin(FwdType t, const char* success);
  // the stream mode, after begin(). The status line and headers parse as above; every de-chunked body
  // byte goes to `sink` (in arrival order, any split) until the close, Content-Length, the last chunk or `cap` body
  // bytes (then done, cut() true); the 256-B copy is not kept (body() stays empty, a 2xx classifies Ok for a template
  // without a success string). The sink's state lives with the caller (the job block on the heap, never the stack).
  typedef void (*Sink)(void* ctx, const char* d, size_t n);
  void stream(Sink sink, void* ctx, size_t cap);
  bool cut() const { return cut_; }
  size_t streamed() const { return stream_n_; }
  // Feed bytes as they arrive; true = done, stop reading and close.
  bool feed(const char* d, size_t n);
  // The connection ended (closed or timed out) before feed() said done.
  void end();
  bool done() const { return done_; }
  FwdResult result() const { return result_; }
  int status() const { return status_; }          // -1 = no status line
  uint32_t retryAfter() const { return retry_; }  // seconds, 0 = none
  const char* body() const { return body_; }       // what was read (<= 256 B), NUL-terminated
  size_t bodyLen() const { return body_n_; }

 private:
  enum class St : uint8_t { Status, Headers, Body, ChunkSize, ChunkData, ChunkCrlf, Done };
  void classify();
  void finish();
  bool needBody() const;
  FwdType type_;
  char success_[FWD_SUCCESS_MAX + 1];
  St st_;
  char line_[CAP_LINE + 1];
  size_t line_n_;
  size_t total_, head_n_;
  int status_;
  uint32_t retry_;
  long clen_;            // -1 unknown
  bool chunked_;
  uint32_t chunk_left_;
  char body_[CAP_BODY + 1];
  size_t body_n_;
  bool done_, overflow_;
  FwdResult result_;
  Sink sink_;
  void* sink_ctx_;
  size_t stream_cap_, stream_n_, limit_;
  bool cut_;
  bool bodyByte(char ch);  // false = the body is complete (cap or Content-Length)
};

// ---------------------------------------------------------------- forwarding on => one station, LAN uploads off
// Enabling with n stations: 0/1 -> nothing forgotten; 2 -> `keep` must name one, the other is forgotten.
struct FwdEnablePlan {
  bool ok;
  int forget;          // index into ids[] to forget, -1 = none
  const char* why;     // !ok: the reason (English, for errors[])
};
FwdEnablePlan fwdEnablePlan(const char* const ids[], int n, const char* keep);
// The bridge side of the rule (forwarding on => max_stations 1, lan_uploads off) is in bridgeFromJson (config.cpp).

}  // namespace fc
