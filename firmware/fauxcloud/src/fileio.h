// fileio.h -- the file system the store writes through. LittleFS on the ESP (app_store.cpp), a RAM double in
// the host tests. Paths are absolute ("/h0_07"). Plain C++, host-compilable.
#pragma once
#include <cstddef>
#include <cstdint>

namespace fc {

class FileIo {
 public:
  virtual ~FileIo() = default;
  virtual long size(const char* path) = 0;  // -1 = missing
  virtual bool read(const char* path, size_t offset, void* buf, size_t len) = 0;  // exactly len bytes
  virtual bool write(const char* path, const void* buf, size_t len) = 0;           // create or truncate (len 0 = empty)
  virtual bool append(const char* path, const void* buf, size_t len) = 0;         // create if missing
  virtual bool writeAt(const char* path, size_t offset, const void* buf, size_t len) = 0;  // inside the file
  virtual bool rename(const char* from, const char* to) = 0;                      // replaces `to`
  virtual bool remove(const char* path) = 0;                                      // true also when missing
  virtual bool format() = 0;                                                      // factory reset, the layout's first build
  // Several buffers into one new file in one open (create or truncate): one metadata commit instead of one per part.
  struct Part {
    const void* buf;
    size_t len;
  };
  virtual bool writeParts(const char* path, const Part* parts, int n) = 0;
  // the entries of the root directory, files and directories, names without the leading '/'. Returns the
  // number of entries passed to `fn`, -1 on error.
  virtual int list(void (*fn)(void* ctx, const char* name, bool dir), void* ctx) = 0;
  // removes a directory and everything in it (an older layout's leftovers /d/...); true also when missing
  virtual bool removeTree(const char* path) = 0;
};

}  // namespace fc
