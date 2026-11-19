// wufc.cpp -- see wufc.h (the WU forecast line).
#include "wufc.h"

#include <cstdio>
#include <cstring>

#include "ha_discovery.h"  // formatIsoUtc
#include "web_render.h"    // Out

namespace fc {

namespace {
enum : uint8_t { K_OTHER, K_TMAX, K_TMIN, K_VALID, K_DAYPART };
enum : uint8_t { K3_OTHER, K3_POP, K3_PHRASE };
enum : int8_t { F_HI, F_LO, F_VALID, F_POP, F_PHRASE };
bool hexDigit(char c, int* v) {
  if (c >= '0' && c <= '9') return *v = c - '0', true;
  if (c >= 'a' && c <= 'f') return *v = c - 'a' + 10, true;
  if (c >= 'A' && c <= 'F') return *v = c - 'A' + 10, true;
  return false;
}
}  // namespace

void WufcSink::reset() {
  st_ = S_VALUE;
  depth_ = n_seen_ = 0;
  memset(is_arr_, 0, sizeof is_arr_);
  memset(count_, 0, sizeof count_);
  idx_ = 0;
  k1_ = K_OTHER, k3_ = K3_OTHER, dp_idx_ = 0xFF;
  key_[0] = 0, key_n_ = 0, key_long_ = false, str_is_key_ = false;
  cap_ = -1;
  uni_n_ = 0, uni_ = 0;
  num_n_ = 0;
  first_ = true, gzip_ = false, bad_ = false, null_lit_ = false;
  first_byte_ = 0;
  memset(have_, 0, sizeof have_);
  memset(v_, 0, sizeof v_);
  phrase_[0] = 0, phrase_n_ = 0, phrase_full_ = false;
}

int WufcSink::want() const {
  if (depth_ == 2 && is_arr_[1] && idx_ == 1)
    return k1_ == K_TMAX ? F_HI : k1_ == K_TMIN ? F_LO : k1_ == K_VALID ? F_VALID : -1;
  if (depth_ == 4 && k1_ == K_DAYPART && is_arr_[1] && dp_idx_ == 0 && !is_arr_[2] && is_arr_[3] && idx_ == 2)
    return k3_ == K3_POP ? F_POP : k3_ == K3_PHRASE ? F_PHRASE : -1;
  return -1;
}

// A value begins in the innermost container: its index there (and which daypart element we enter)
void WufcSink::beginValue() {
  idx_ = 0;
  if (depth_ && is_arr_[depth_ - 1]) {
    idx_ = count_[depth_ - 1];
    if (count_[depth_ - 1] < 0xFFFF) count_[depth_ - 1]++;
    if (depth_ == 2 && k1_ == K_DAYPART) dp_idx_ = (uint8_t)(idx_ > 0xFE ? 0xFE : idx_);
  }
  cap_ = (int8_t)want();
}

void WufcSink::endValue() {
  st_ = S_AFTER;
  cap_ = -1;
}

void WufcSink::putByte(uint8_t c) {
  if (c < 0x20) return;  // control characters are dropped
  if (str_is_key_) {
    if (key_n_ < KEY_MAX) key_[key_n_++] = (char)c;
    else key_long_ = true;
    return;
  }
  if (cap_ != F_PHRASE || phrase_full_) return;
  if (phrase_n_ < WUFC_PHRASE_MAX) phrase_[phrase_n_++] = (char)c;
  else phrase_full_ = true;
}

void WufcSink::putCode(uint32_t cp) {
  if (cp >= 0xD800 && cp <= 0xDFFF) cp = '?';  // a surrogate half: not decoded
  uint8_t b[3];
  int n;
  if (cp < 0x80) b[0] = (uint8_t)cp, n = 1;
  else if (cp < 0x800) b[0] = (uint8_t)(0xC0 | cp >> 6), b[1] = (uint8_t)(0x80 | (cp & 0x3F)), n = 2;
  else b[0] = (uint8_t)(0xE0 | cp >> 12), b[1] = (uint8_t)(0x80 | ((cp >> 6) & 0x3F)), b[2] = (uint8_t)(0x80 | (cp & 0x3F)), n = 3;
  if (!str_is_key_ && cap_ == F_PHRASE && phrase_n_ + n > WUFC_PHRASE_MAX) {
    phrase_full_ = true;  // never half a character
    return;
  }
  for (int i = 0; i < n; i++) putByte(b[i]);
}

void WufcSink::endString() {
  if (str_is_key_) {
    key_[key_n_] = 0;
    const char* k = key_long_ ? "" : key_;
    if (depth_ == 1) {
      k1_ = !strcmp(k, "temperatureMax") ? K_TMAX : !strcmp(k, "temperatureMin") ? K_TMIN
            : !strcmp(k, "validTimeUtc") ? K_VALID : !strcmp(k, "daypart") ? K_DAYPART : K_OTHER;
      if (k1_ != K_OTHER) n_seen_ |= (uint8_t)(1u << k1_);
    } else if (depth_ == 3 && k1_ == K_DAYPART && dp_idx_ == 0) {
      k3_ = !strcmp(k, "precipChance") ? K3_POP : !strcmp(k, "wxPhraseLong") ? K3_PHRASE : K3_OTHER;
    }
    str_is_key_ = false;
    st_ = S_COLON;
    return;
  }
  if (cap_ == F_PHRASE) {  // a raw multi-byte character cut by the 40-B limit is dropped whole
    int i = phrase_n_ - 1;
    while (i > 0 && ((uint8_t)phrase_[i] & 0xC0) == 0x80) i--;
    if (i >= 0 && phrase_n_) {
      const uint8_t lead = (uint8_t)phrase_[i];
      const int need = lead < 0x80 ? 1 : (lead & 0xE0) == 0xC0 ? 2 : (lead & 0xF0) == 0xE0 ? 3 : (lead & 0xF8) == 0xF0 ? 4 : 1;
      if (phrase_n_ - i < need) phrase_n_ = (uint8_t)i;
    }
    phrase_[phrase_n_] = 0;
    have_[F_PHRASE] = phrase_n_ > 0;
  }
  endValue();
}

void WufcSink::feed(const char* d, size_t n) {
  for (size_t i = 0; i < n && !bad_; i++) {
    const uint8_t c = (uint8_t)d[i];
    if (first_) {  // the gzip magic (1f 8b): WU ignored Accept-Encoding: identity
      if (c == 0x1F) {
        first_byte_ = c;
        first_ = false;
        continue;
      }
      first_ = false;
    } else if (first_byte_ == 0x1F) {
      gzip_ = c == 0x8B;
      bad_ = true;
      return;
    }
    bool again = true;
    while (again && !bad_) {
      again = false;
      const bool ws = c == ' ' || c == '\t' || c == '\r' || c == '\n';
      switch (st_) {
        case S_VALUE:
          if (ws) break;
          if (c == ']' && depth_ && is_arr_[depth_ - 1] && !count_[depth_ - 1]) {  // an empty array
            depth_--;
            endValue();
            break;
          }
          if (depth_ == 0 && c != '{') {  // the root must be an object
            bad_ = true;
            break;
          }
          beginValue();
          if (c == '{' || c == '[') {
            if (depth_ >= DEPTH_MAX) {
              bad_ = true;
              break;
            }
            is_arr_[depth_] = c == '[';
            count_[depth_] = 0;
            depth_++;
            if (depth_ == 2 && k1_ != K_DAYPART) k3_ = K3_OTHER;
            st_ = c == '[' ? S_VALUE : S_KEY_OR_END;
          } else if (c == '"') {
            str_is_key_ = false;
            st_ = S_STR;
          } else if (c == '-' || (c >= '0' && c <= '9')) {
            num_[0] = (char)c, num_n_ = 1;
            st_ = S_NUM;
          } else if (c == 't' || c == 'f' || c == 'n') {
            null_lit_ = c == 'n';
            st_ = S_LIT;
          } else {
            bad_ = true;
          }
          break;
        case S_KEY_OR_END:
        case S_KEY:
          if (ws) break;
          if (c == '}' && st_ == S_KEY_OR_END) {
            depth_--;
            endValue();
          } else if (c == '"') {
            str_is_key_ = true;
            key_n_ = 0, key_long_ = false;
            st_ = S_STR;
          } else {
            bad_ = true;
          }
          break;
        case S_COLON:
          if (ws) break;
          if (c == ':') st_ = S_VALUE;
          else bad_ = true;
          break;
        case S_AFTER:
          if (ws) break;
          if (!depth_) {
            bad_ = true;  // something after the root object
          } else if (c == ',') {
            st_ = is_arr_[depth_ - 1] ? S_VALUE : S_KEY;
          } else if ((c == '}' && !is_arr_[depth_ - 1]) || (c == ']' && is_arr_[depth_ - 1])) {
            depth_--;
            endValue();
          } else {
            bad_ = true;
          }
          break;
        case S_STR:
          if (c == '\\') st_ = S_ESC;
          else if (c == '"') endString();
          else putByte(c);
          break;
        case S_ESC:
          st_ = S_STR;
          if (c == '"' || c == '\\' || c == '/') putByte(c);
          else if (c == 'u') st_ = S_UNI, uni_n_ = 0, uni_ = 0;
          else if (!strchr("bfnrt", (char)c)) bad_ = true;  // \b \f \n \r \t: control characters, dropped
          break;
        case S_UNI: {
          int v;
          if (!hexDigit((char)c, &v)) {
            bad_ = true;
            break;
          }
          uni_ = (uint16_t)(uni_ << 4 | v);
          if (++uni_n_ == 4) {
            putCode(uni_);
            st_ = S_STR;
          }
          break;
        }
        case S_NUM:
          if ((c >= '0' && c <= '9') || c == '.' || c == 'e' || c == 'E' || c == '+' || c == '-') {
            if (num_n_ < sizeof num_ - 1) num_[num_n_++] = (char)c;
            break;
          }
          if (cap_ >= F_HI && cap_ <= F_POP) {  // integers (a fraction is cut: WU sends whole numbers here)
            num_[num_n_] = 0;
            int64_t v = 0;
            bool neg = num_[0] == '-', digit = false;
            for (int k = neg ? 1 : 0; k < num_n_ && num_[k] >= '0' && num_[k] <= '9'; k++) {
              digit = true;
              if (v < 100000000000LL) v = v * 10 + (num_[k] - '0');
            }
            if (digit) v_[cap_] = (int32_t)(neg ? -v : v > INT32_MAX ? INT32_MAX : v), have_[cap_] = true;
          }
          endValue();
          again = true;  // the byte that ended the number is read again
          break;
        case S_LIT:
          if (c >= 'a' && c <= 'z') break;
          endValue();  // null leaves a field unset; true / false are not ours
          again = true;
          break;
      }
    }
  }
}

WufcParse WufcSink::finish(bool cut, uint32_t now, WufcData* out) const {
  if (gzip_) return WufcParse::Encoding;
  if (cut) return WufcParse::TooLarge;
  if (bad_ || depth_ || st_ != S_AFTER) return WufcParse::Bad;
  const uint8_t shape = (1u << K_TMAX) | (1u << K_TMIN) | (1u << K_VALID) | (1u << K_DAYPART);
  if ((n_seen_ & shape) != shape) return WufcParse::Bad;  // JSON, but not the 5-day forecast
  if (!have_[F_HI] || !have_[F_LO] || !have_[F_VALID] || !have_[F_PHRASE]) return WufcParse::NoData;
  const int32_t hi = v_[F_HI], lo = v_[F_LO], valid = v_[F_VALID];
  if (hi < -60 || hi > 60 || lo < -60 || lo > 60 || lo > hi + 2) return WufcParse::Bad;
  if (have_[F_POP] && (v_[F_POP] < 0 || v_[F_POP] > 100)) return WufcParse::Bad;
  if (now && (valid < 0 || (uint32_t)valid + 86400u < now || (uint32_t)valid > now + 3u * 86400u)) return WufcParse::Bad;
  out->hi = (int8_t)hi, out->lo = (int8_t)lo;
  out->pop = have_[F_POP] ? (int8_t)v_[F_POP] : WUFC_UNSET;
  memcpy(out->phrase, phrase_, sizeof out->phrase);
  out->valid_t = (uint32_t)valid;
  return WufcParse::Ok;
}

// ---------------------------------------------------------------- store
void wufcPut(WufcStore* s, const WufcData& d, uint32_t now) {
  s->d = d;
  s->fetched_t = now ? now : 1;
  s->gen++;
}
void wufcDrop(WufcStore* s) {
  if (!s->fetched_t) return;
  s->fetched_t = 0;
  memset(&s->d, 0, sizeof s->d);
  s->gen++;
}

static int64_t localDay(int64_t t, const PosixTz& tz) {
  const int64_t l = t + localOffset(tz, t);
  return l >= 0 ? l / 86400 : (l - 86399) / 86400;
}

const char* wufcDay(const WufcStore& s, uint32_t now, const PosixTz& tz) {
  if (!s.fetched_t || now < s.fetched_t || now - s.fetched_t > WUFC_STALE_S) return nullptr;
  const int64_t dv = localDay(s.d.valid_t, tz), dn = localDay(now, tz);
  return dv == dn + 1 ? "tomorrow" : dv == dn ? "today" : nullptr;
}

void wufcLiveJson(Out& o, const WufcStore& s, uint32_t now, const PosixTz& tz) {
  const char* day = wufcDay(s, now, tz);
  if (!day) return;
  char iso[24];
  formatIsoUtc(iso, sizeof iso, s.fetched_t);
  o.printf(",\"wufc\":{\"day\":\"%s\",\"phrase\":", day);
  o.jsonStr(s.d.phrase);
  o.printf(",\"hi\":%d,\"lo\":%d,\"pop\":", (int)s.d.hi, (int)s.d.lo);
  if (s.d.pop == WUFC_UNSET) o.raw("null");
  else o.printf("%d", (int)s.d.pop);
  o.printf(",\"t\":\"%s\"}", iso);
}

int wufcAttributes(char* buf, size_t cap, const WufcStore& s, uint32_t now, const PosixTz& tz) {
  const char* day = wufcDay(s, now, tz);
  if (!day) return snprintf(buf, cap, "{}");
  char iso[24], pop[8];
  formatIsoUtc(iso, sizeof iso, s.fetched_t);
  if (s.d.pop == WUFC_UNSET) snprintf(pop, sizeof pop, "null");
  else snprintf(pop, sizeof pop, "%d", (int)s.d.pop);
  const int n = snprintf(buf, cap,
                         "{\"day\":\"%s\",\"t_max_c\":%d,\"t_min_c\":%d,\"precip_pct\":%s,\"fetched\":\"%s\","
                         "\"source\":\"Weather Underground\"}",
                         day, (int)s.d.hi, (int)s.d.lo, pop, iso);
  return n < 0 || (size_t)n >= cap ? -1 : n;
}

// ---------------------------------------------------------------- cfg, request
void wufcCfgDefault(WufcCfg* c) { *c = WufcCfg{WUFC_CFG_VER, 0, 0, 0}; }

bool wufcCfgLoad(const uint8_t* blob, size_t n, WufcCfg* out) {
  wufcCfgDefault(out);
  if (!blob || n < 1 || blob[0] != WUFC_CFG_VER || n < sizeof(WufcCfg)) return false;
  memcpy(out, blob, sizeof(WufcCfg));
  out->on = out->on ? 1 : 0;
  out->verified = out->verified ? 1 : 0;
  return true;
}

int wufcPath(char* buf, size_t cap, float lat, float lon, const char* key) {
  const int n = snprintf(buf, cap, "/v3/wx/forecast/daily/5day?geocode=%.*f,%.*f&units=m&language=en-US&format=json&apiKey=%s",
                         WUFC_GEO_DECIMALS, (double)lat, WUFC_GEO_DECIMALS, (double)lon, key);
  return n < 0 || (size_t)n >= cap ? -1 : n;
}

bool wufcKeyOk(const char* k) {
  if (!k || strlen(k) != WUFC_KEY_MAX) return false;
  for (const char* p = k; *p; p++) {
    int v;
    if (!hexDigit(*p, &v)) return false;
  }
  return true;
}

}  // namespace fc
