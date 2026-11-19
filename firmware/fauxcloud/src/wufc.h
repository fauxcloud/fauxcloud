// wufc.h -- the WU forecast line. The
// contributor's api.weather.com 5-day forecast, fetched identity-encoded and streamed through a byte-wise JSON scanner
// that keeps six values (nothing of the ~6 KB body is held, no inflate); a RAM-only store under gLock; the stale and
// label rules; the /api/live object and the HA attributes. Pure and host-tested; compiled into every build, used only
// by FWD_TLS=1 images (the job glue in app_fwd.cpp).
//
// What is kept: tomorrow's day phrase (daypart[0].wxPhraseLong[2], cut at 40 B on a UTF-8 boundary), its high
// (temperatureMax[1]) and low (temperatureMin[1]), the precipitation chance (daypart[0].precipChance[2], nullable)
// and validTimeUtc[1] (the day's label). Not kept: the narrative, amounts, the night phrase, days 2-5. The indexes do
// not move after 15:00 local (today's slot 0 goes null, tomorrow's stay: "Tomorrow" from the
// next day's day slot, nothing for today); WU rolls the days over at 03:00 local, so between 00:00 and that rollover
// index 1 is the day that has just begun ("Today").
#pragma once
#include <cstddef>
#include <cstdint>

#include "tz.h"

namespace fc {

class Out;

constexpr size_t WUFC_BODY_CAP = 8192;      // 5 630-6 015 B measured + room for longer localised strings
constexpr int WUFC_PHRASE_MAX = 40;         // bytes, UTF-8-safe cut
constexpr int WUFC_GEO_DECIMALS = 1;        // the geocode's precision (~11 km), a constant, not a setting
constexpr uint32_t WUFC_STALE_S = 7200;     // shown while the fetch is at most 2 h old
constexpr int8_t WUFC_UNSET = INT8_MIN;

// How a parse ended (the scheduler's outcome adds the network ones)
enum class WufcParse : uint8_t { Ok, NoData, Bad, Encoding, TooLarge };

struct WufcData {
  int8_t hi, lo, pop;  // °C, °C, % (pop WUFC_UNSET = not given)
  char phrase[WUFC_PHRASE_MAX + 1];
  uint32_t valid_t;    // validTimeUtc[1]
};

// The byte-wise scanner (FwdResp's stream sink; its state lives in the job block, never on the net-out stack).
class WufcSink {
 public:
  WufcSink() { reset(); }
  void reset();
  void feed(const char* d, size_t n);
  static void sinkFn(void* self, const char* d, size_t n) { ((WufcSink*)self)->feed(d, n); }
  // The answer's verdict after a clean end (metarCleanEnd's rule: a half answer is never a line). `cut` = the body ran
  // over WUFC_BODY_CAP; `now` = real time for the plausibility of validTimeUtc.
  WufcParse finish(bool cut, uint32_t now, WufcData* out) const;
  bool gzip() const { return gzip_; }
  bool bad() const { return bad_; }

 private:
  enum : uint8_t { S_VALUE, S_KEY_OR_END, S_KEY, S_COLON, S_AFTER, S_STR, S_ESC, S_UNI, S_NUM, S_LIT };
  static constexpr int DEPTH_MAX = 6, KEY_MAX = 24;
  void beginValue();
  void endValue();
  void endString();
  void putByte(uint8_t c);
  void putCode(uint32_t cp);
  int want() const;  // which field the current scalar is (-1 = none)
  uint8_t st_, depth_, n_seen_;
  bool is_arr_[DEPTH_MAX];
  uint16_t count_[DEPTH_MAX];  // elements begun in an array
  uint16_t idx_;               // the index of the scalar being read (in its array)
  uint8_t k1_, k3_;            // the key under the root, the key inside daypart[0]
  uint8_t dp_idx_;             // the index of the daypart element we are in
  char key_[KEY_MAX + 1];
  uint8_t key_n_;
  bool key_long_, str_is_key_;
  int8_t cap_;                 // the field a string / number goes to (-1 none)
  uint8_t uni_n_;
  uint16_t uni_;
  char num_[16];
  uint8_t num_n_;
  bool first_, gzip_, bad_, null_lit_;
  uint8_t first_byte_;
  // captured
  bool have_[5];
  int32_t v_[4];               // 0 hi, 1 lo, 2 valid, 3 pop
  char phrase_[WUFC_PHRASE_MAX + 1];
  uint8_t phrase_n_;
  bool phrase_full_;
};

// The RAM store (never written to flash), the loop's under gLock
struct WufcStore {
  WufcData d;
  uint32_t fetched_t;  // 0 = none
  uint32_t gen;        // bumped on every change (the live ETag word); a drop bumps it too
};
void wufcPut(WufcStore* s, const WufcData& d, uint32_t now);
void wufcDrop(WufcStore* s);  // WU off, key cleared / changed, refused pause: the line leaves the next body
// The label of the stored day at `now` (bridge local time): "tomorrow" (its valid day is tomorrow), "today" (it is
// today: the 00:00-03:00 window before WU's rollover), nullptr = not shown (stale, none, or another day).
const char* wufcDay(const WufcStore& s, uint32_t now, const PosixTz& tz);
// /api/live: ,"wufc":{"day","phrase","hi","lo","pop"|absent,"t"} while shown, nothing otherwise
void wufcLiveJson(Out& o, const WufcStore& s, uint32_t now, const PosixTz& tz);
// HA attributes of the bridge sensor wu_tomorrow: {"day","t_max_c","t_min_c","precip_pct","fetched","source"}
int wufcAttributes(char* buf, size_t cap, const WufcStore& s, uint32_t now, const PosixTz& tz);

// NVS blob `wufc` (versions migrate, never reset); the key is the separate write-only string `wk`
struct WufcCfg {
  uint8_t ver;       // 1
  uint8_t on;        // the switch "Forecast from Weather Underground"
  uint8_t verified;  // an accepted "Fetch now" with this key (a key change clears it; the switch needs it)
  uint8_t reserved;
};
constexpr uint8_t WUFC_CFG_VER = 1;
constexpr size_t WUFC_KEY_MAX = 32;
void wufcCfgDefault(WufcCfg* c);
// A stored blob: true when it is a version this firmware reads (an older one migrates here; version 1 is the first)
bool wufcCfgLoad(const uint8_t* blob, size_t n, WufcCfg* out);
// The request path (no host): "/v3/wx/forecast/daily/5day?geocode=<lat>,<lon>&units=m&language=en-US&format=json&apiKey=<key>"
int wufcPath(char* buf, size_t cap, float lat, float lon, const char* key);
// A key the API could take: 32 hex characters (WU's API keys); anything else is refused at the admin PUT
bool wufcKeyOk(const char* k);

}  // namespace fc
