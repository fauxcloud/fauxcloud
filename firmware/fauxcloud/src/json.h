// json.h -- a small, tolerant JSON reader and a string escaper for the config documents.
//
// No DOM: the caller walks the document with beginObject / nextKey / read* / skipValue. Strings are unescaped
// (including \uXXXX and surrogate pairs, emitted as UTF-8); truncation never splits a UTF-8 sequence. Nesting is
// limited, so hostile input cannot exhaust the stack. Plain C++, host-compilable.
#pragma once
#include <cstddef>
#include <cstdint>

namespace fc {

class JsonReader {
 public:
  JsonReader(const char* s, size_t len) : p_(s), end_(s + len) {
    // A UTF-8 byte order mark (Windows PowerShell 5.1 writes one) is skipped.
    if (len >= 3 && (unsigned char)s[0] == 0xEF && (unsigned char)s[1] == 0xBB && (unsigned char)s[2] == 0xBF) p_ += 3;
  }

  bool beginObject();                     // consumes '{'
  bool nextKey(char* key, size_t cap);    // true + key (':' consumed); false at '}' (consumed) or on error
  bool beginArray();                      // consumes '['
  bool nextElement();                     // true if another element follows; false at ']' (consumed) or error
  bool readString(char* out, size_t cap, bool* truncated = nullptr);
  bool readNumber(double* out);
  bool readNumberText(char* out, size_t cap);  // the number's text as written (no float), NUL-terminated
  bool readBool(bool* out);
  bool skipValue();
  char peek();                            // next non-space character, 0 at the end
  bool failed() const { return err_; }
  bool atEnd();                           // only whitespace left

 private:
  void ws();
  bool skipDepth(int depth);
  const char* p_;
  const char* end_;
  bool err_ = false;
  bool first_ = true;  // no member/element read yet at the current level (comma handling)
};

// Writes src as a quoted, escaped JSON string ("..."). Returns the length it needs (excluding the
// terminator); writes nothing beyond cap and always terminates when cap > 0. Control characters become \u00XX,
// '"' and '\' are escaped, UTF-8 passes through unchanged.
size_t jsonQuote(char* dst, size_t cap, const char* src);

// Truncates s in place to at most max_bytes without splitting a UTF-8 sequence.
void utf8Truncate(char* s, size_t max_bytes);

}  // namespace fc
