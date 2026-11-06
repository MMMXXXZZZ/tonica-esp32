// Minimal protobuf wire-format writer/reader. No codegen: field numbers are
// taken from librespot/protocol/proto and written at the call site.
#pragma once
#include <cstring>
#include <functional>
#include <string>
#include "listenica/platform.h"

namespace listenica {
namespace pb {

class Writer {
 public:
  Bytes buf;

  void varint(uint64_t v) {
    while (v >= 0x80) {
      buf.push_back(uint8_t(v) | 0x80);
      v >>= 7;
    }
    buf.push_back(uint8_t(v));
  }
  void tag(uint32_t field, uint32_t wt) { varint((uint64_t(field) << 3) | wt); }
  Writer& u64(uint32_t f, uint64_t v) {
    tag(f, 0);
    varint(v);
    return *this;
  }
  Writer& i64(uint32_t f, int64_t v) { return u64(f, uint64_t(v)); }
  Writer& b(uint32_t f, bool v) { return u64(f, v ? 1 : 0); }
  Writer& bytes(uint32_t f, const uint8_t* d, size_t n) {
    tag(f, 2);
    varint(n);
    buf.insert(buf.end(), d, d + n);
    return *this;
  }
  Writer& bytes(uint32_t f, const Bytes& d) { return bytes(f, d.data(), d.size()); }
  Writer& str(uint32_t f, const std::string& s) { return bytes(f, (const uint8_t*)s.data(), s.size()); }
  Writer& dbl(uint32_t f, double v) {
    tag(f, 1);
    uint8_t b8[8];
    memcpy(b8, &v, 8);  // little-endian targets only (x86, xtensa)
    buf.insert(buf.end(), b8, b8 + 8);
    return *this;
  }
  Writer& flt(uint32_t f, float v) {
    tag(f, 5);
    uint8_t b4[4];
    memcpy(b4, &v, 4);
    buf.insert(buf.end(), b4, b4 + 4);
    return *this;
  }
  Writer& msg(uint32_t f, const std::function<void(Writer&)>& fill) {
    Writer sub;
    fill(sub);
    return bytes(f, sub.buf);
  }
  Writer& msg(uint32_t f, const Writer& sub) { return bytes(f, sub.buf); }
  // map<string,string> entry
  Writer& map_entry(uint32_t f, const std::string& k, const std::string& v) {
    return msg(f, [&](Writer& e) { e.str(1, k).str(2, v); });
  }
};

struct Field {
  uint32_t num = 0;
  uint32_t wt = 0;
  uint64_t v = 0;  // varint / fixed value
  const uint8_t* data = nullptr;
  size_t len = 0;
  std::string str() const { return std::string((const char*)data, len); }
  Bytes bytes() const { return Bytes(data, data + len); }
  double dbl() const {
    double d;
    memcpy(&d, &v, 8);
    return d;
  }
};

class Reader {
 public:
  Reader(const uint8_t* d, size_t n) : p_(d), end_(d + n), begin_(d) {}
  explicit Reader(const Bytes& b) : Reader(b.data(), b.size()) {}
  Reader(const Field& f) : Reader(f.data, f.len) {}
  bool ok() const { return !err_; }

  bool next(Field& f) {
    if (p_ >= end_ || err_) return false;
    uint64_t key;
    if (!rv(key)) return fail();
    f.num = uint32_t(key >> 3);
    f.wt = uint32_t(key & 7);
    f.data = nullptr;
    f.len = 0;
    switch (f.wt) {
      case 0:
        if (!rv(f.v)) return fail();
        break;
      case 1:
        if (end_ - p_ < 8) return fail();
        memcpy(&f.v, p_, 8);
        p_ += 8;
        break;
      case 2: {
        uint64_t n;
        if (!rv(n) || uint64_t(end_ - p_) < n) return fail();
        f.data = p_;
        f.len = size_t(n);
        p_ += n;
        break;
      }
      case 5: {
        if (end_ - p_ < 4) return fail();
        uint32_t x;
        memcpy(&x, p_, 4);
        f.v = x;
        p_ += 4;
        break;
      }
      default:
        return fail();
    }
    return true;
  }

  // first occurrence of field `num`
  bool find(uint32_t num, Field& out) {
    Reader r(start_or(), end_);
    Field f;
    while (r.next(f))
      if (f.num == num) {
        out = f;
        return true;
      }
    return false;
  }
  uint64_t varint_of(uint32_t num, uint64_t dflt = 0) {
    Field f;
    return find(num, f) ? f.v : dflt;
  }
  std::string str_of(uint32_t num) {
    Field f;
    return find(num, f) ? f.str() : std::string();
  }
  Bytes bytes_of(uint32_t num) {
    Field f;
    return find(num, f) ? f.bytes() : Bytes();
  }
  void each(uint32_t num, const std::function<void(const Field&)>& fn) {
    Reader r(start_or(), end_);
    Field f;
    while (r.next(f))
      if (f.num == num) fn(f);
  }

 private:
  const uint8_t* p_;
  const uint8_t* end_;
  const uint8_t* begin_;
  bool err_ = false;
  Reader(const uint8_t* d, const uint8_t* e) : p_(d), end_(e), begin_(d) {}
  const uint8_t* start_or() const { return begin_; }
  bool fail() {
    err_ = true;
    return false;
  }
  bool rv(uint64_t& v) {
    v = 0;
    for (int s = 0; s < 64 && p_ < end_; s += 7) {
      uint8_t b = *p_++;
      v |= uint64_t(b & 0x7f) << s;
      if (!(b & 0x80)) return true;
    }
    return false;
  }
};

}  // namespace pb
}  // namespace listenica
