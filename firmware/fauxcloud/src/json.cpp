// json.cpp -- see json.h.
#include "json.h"

#include <cstdlib>
#include <cstring>

namespace fc {

static constexpr int MAX_DEPTH = 8;

void JsonReader::ws() {
  while (p_ < end_ && (*p_ == ' ' || *p_ == '\t' || *p_ == '\r' || *p_ == '\n')) p_++;
}

char JsonReader::peek() {
  ws();
  return p_ < end_ ? *p_ : 0;
}

bool JsonReader::atEnd() {
  ws();
  return p_ >= end_ || *p_ == 0;
}

bool JsonReader::beginObject() {
  if (err_ || peek() != '{') return !(err_ = true);
  p_++;
  first_ = true;
  return true;
}

bool JsonReader::beginArray() {
  if (err_ || peek() != '[') return !(err_ = true);
  p_++;
  first_ = true;
  return true;
}

bool JsonReader::nextKey(char* key, size_t cap) {
  if (err_) return false;
  char c = peek();
  if (c == '}') {
    p_++;
    first_ = false;  // the enclosing level has now read the member that held this object
    return false;
  }
  if (!first_) {
    if (c != ',') return !(err_ = true) && false;
    p_++;
    c = peek();
  }
  if (c != '"' || !readString(key, cap)) return !(err_ = true) && false;
  if (peek() != ':') return !(err_ = true) && false;
  p_++;
  first_ = false;
  return true;
}

bool JsonReader::nextElement() {
  if (err_) return false;
  char c = peek();
  if (c == ']') {
    p_++;
    first_ = false;
    return false;
  }
  if (!first_) {
    if (c != ',') return !(err_ = true) && false;
    p_++;
  }
  first_ = false;
  return true;
}

static int hexv(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

void utf8Truncate(char* s, size_t max_bytes) {
  size_t n = strlen(s);
  if (n > max_bytes) n = max_bytes;
  s[n] = 0;
  // Find the lead byte of the last sequence and drop that sequence if it is incomplete.
  size_t i = n;
  while (i > 0 && ((unsigned char)s[i - 1] & 0xC0) == 0x80) i--;
  if (i == 0) {
    s[0] = 0;  // only continuation bytes: nothing valid
    return;
  }
  const unsigned char c = (unsigned char)s[i - 1];
  const size_t need = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : (c >> 3) == 0x1E ? 4 : 1;
  if (n - (i - 1) < need) s[i - 1] = 0;
}

bool JsonReader::readString(char* out, size_t cap, bool* truncated) {
  if (truncated) *truncated = false;
  if (err_ || peek() != '"') return !(err_ = true);
  p_++;
  size_t n = 0;
  bool cut = false;
  auto put = [&](unsigned char ch) {
    if (n + 1 < cap) out[n++] = (char)ch;
    else cut = true;
  };
  while (p_ < end_ && *p_ != '"') {
    unsigned char c = (unsigned char)*p_++;
    if (c < 0x20) return !(err_ = true);  // raw control character inside a string
    if (c != '\\') { put(c); continue; }
    if (p_ >= end_) return !(err_ = true);
    char e = *p_++;
    switch (e) {
      case '"': put('"'); break;
      case '\\': put('\\'); break;
      case '/': put('/'); break;
      case 'b': put('\b'); break;
      case 'f': put('\f'); break;
      case 'n': put('\n'); break;
      case 'r': put('\r'); break;
      case 't': put('\t'); break;
      case 'u': {
        auto hex4 = [&](uint32_t* v) {
          if (end_ - p_ < 4) return false;
          *v = 0;
          for (int i = 0; i < 4; i++) {
            int h = hexv(p_[i]);
            if (h < 0) return false;
            *v = *v << 4 | (uint32_t)h;
          }
          p_ += 4;
          return true;
        };
        uint32_t cp;
        if (!hex4(&cp)) return !(err_ = true);
        if (cp >= 0xD800 && cp <= 0xDBFF) {  // high surrogate: needs a low one
          uint32_t lo;
          if (end_ - p_ < 6 || p_[0] != '\\' || p_[1] != 'u') return !(err_ = true);
          p_ += 2;
          if (!hex4(&lo) || lo < 0xDC00 || lo > 0xDFFF) return !(err_ = true);
          cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
        } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
          return !(err_ = true);
        }
        if (cp < 0x80) {
          put((unsigned char)cp);
        } else if (cp < 0x800) {
          put((unsigned char)(0xC0 | cp >> 6));
          put((unsigned char)(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
          put((unsigned char)(0xE0 | cp >> 12));
          put((unsigned char)(0x80 | (cp >> 6 & 0x3F)));
          put((unsigned char)(0x80 | (cp & 0x3F)));
        } else {
          put((unsigned char)(0xF0 | cp >> 18));
          put((unsigned char)(0x80 | (cp >> 12 & 0x3F)));
          put((unsigned char)(0x80 | (cp >> 6 & 0x3F)));
          put((unsigned char)(0x80 | (cp & 0x3F)));
        }
        break;
      }
      default:
        return !(err_ = true);
    }
  }
  if (p_ >= end_) return !(err_ = true);  // unterminated
  p_++;
  if (cap) {
    out[n] = 0;
    if (cut) utf8Truncate(out, n);  // a sequence may have been cut at the buffer end
  }
  if (truncated) *truncated = cut;
  return true;
}

bool JsonReader::readNumber(double* out) {
  if (err_) return false;
  ws();
  char buf[40];
  size_t n = 0;
  while (p_ < end_ && n + 1 < sizeof buf &&
         ((*p_ >= '0' && *p_ <= '9') || *p_ == '-' || *p_ == '+' || *p_ == '.' || *p_ == 'e' || *p_ == 'E'))
    buf[n++] = *p_++;
  buf[n] = 0;
  if (!n) return !(err_ = true);
  char* end = nullptr;
  *out = strtod(buf, &end);
  if (!end || *end) return !(err_ = true);
  return true;
}

bool JsonReader::readNumberText(char* out, size_t cap) {
  if (err_ || !cap) return false;
  ws();
  size_t n = 0;
  while (p_ < end_ && ((*p_ >= '0' && *p_ <= '9') || *p_ == '-' || *p_ == '+' || *p_ == '.' || *p_ == 'e' || *p_ == 'E')) {
    if (n + 1 >= cap) return !(err_ = true);
    out[n++] = *p_++;
  }
  out[n] = 0;
  if (!n) return !(err_ = true);
  return true;
}

bool JsonReader::readBool(bool* out) {
  if (err_) return false;
  ws();
  if (end_ - p_ >= 4 && strncmp(p_, "true", 4) == 0) { *out = true; p_ += 4; return true; }
  if (end_ - p_ >= 5 && strncmp(p_, "false", 5) == 0) { *out = false; p_ += 5; return true; }
  return !(err_ = true);
}

bool JsonReader::skipDepth(int depth) {
  if (depth > MAX_DEPTH) return !(err_ = true);
  char c = peek();
  if (c == '{') {
    p_++;
    first_ = true;
    char k[2];
    while (nextKey(k, sizeof k))  // keys are skipped (truncated read)
      if (!skipDepth(depth + 1)) return false;
    return !err_;
  }
  if (c == '[') {
    p_++;
    first_ = true;
    while (nextElement())
      if (!skipDepth(depth + 1)) return false;
    return !err_;
  }
  if (c == '"') {
    char tmp[2];
    return readString(tmp, sizeof tmp);
  }
  if (c == 't' || c == 'f') {
    bool b;
    return readBool(&b);
  }
  if (c == 'n') {
    if (end_ - p_ >= 4 && strncmp(p_, "null", 4) == 0) { p_ += 4; return true; }
    return !(err_ = true);
  }
  double d;
  return readNumber(&d);
}

bool JsonReader::skipValue() {
  bool ok = skipDepth(0);
  first_ = false;
  return ok;
}

size_t jsonQuote(char* dst, size_t cap, const char* src) {
  size_t n = 0;
  auto put = [&](char c) {
    if (n + 1 < cap) dst[n] = c;
    n++;
  };
  put('"');
  for (const unsigned char* s = (const unsigned char*)src; *s; s++) {
    if (*s == '"' || *s == '\\') {
      put('\\');
      put((char)*s);
    } else if (*s < 0x20) {
      static const char HEX[] = "0123456789abcdef";
      put('\\'); put('u'); put('0'); put('0');
      put(HEX[*s >> 4]);
      put(HEX[*s & 15]);
    } else {
      put((char)*s);
    }
  }
  put('"');
  if (cap) dst[n < cap ? n : cap - 1] = 0;
  return n;
}

}  // namespace fc
