// Blocking single-producer/single-consumer byte ring with abort/reset,
// used between the CDN fetcher and the decoder.
#pragma once
#include <algorithm>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include "listenica/platform.h"

namespace listenica {

void* big_alloc(size_t n);  // PSRAM on the ESP32
void big_free(void* p);

class ByteRing {
 public:
  explicit ByteRing(size_t cap) : cap_(cap), buf_((uint8_t*)big_alloc(cap)) {}
  ~ByteRing() { big_free(buf_); }

  // Blocks until all n bytes are written or the generation changes / abort.
  // Returns false if aborted or reset during the write.
  bool write(const uint8_t* d, size_t n, uint32_t gen) {
    std::unique_lock<std::mutex> l(mu_);
    while (n) {
      cv_.wait(l, [&] { return size_ < cap_ || gen != gen_ || aborted_; });
      if (gen != gen_ || aborted_) return false;
      size_t space = cap_ - size_;
      size_t w = (head_ + size_) % cap_;
      size_t chunk = std::min({n, space, cap_ - w});
      memcpy(buf_ + w, d, chunk);
      size_ += chunk;
      d += chunk;
      n -= chunk;
      cv_.notify_all();
    }
    return true;
  }
  // Non-blocking: writes as much as fits now (whole multiples of `align`).
  size_t write_some(const uint8_t* d, size_t n, uint32_t gen, size_t align = 1) {
    std::lock_guard<std::mutex> l(mu_);
    if (gen != gen_ || aborted_) return 0;
    size_t can = std::min(n, cap_ - size_);
    can -= can % align;
    size_t done = 0;
    while (done < can) {
      size_t w = (head_ + size_) % cap_;
      size_t chunk = std::min(can - done, cap_ - w);
      memcpy(buf_ + w, d + done, chunk);
      size_ += chunk;
      done += chunk;
    }
    if (done) cv_.notify_all();
    return done;
  }
  // Reads up to n bytes; waits up to timeout_ms for at least one byte.
  // Returns bytes read, 0 on timeout, -1 on EOF (no more data and eof set).
  int read(uint8_t* d, size_t n, uint32_t timeout_ms, uint32_t* gen_out = nullptr) {
    std::unique_lock<std::mutex> l(mu_);
    if (gen_out) *gen_out = gen_;
    if (!cv_.wait_for(l, std::chrono::milliseconds(timeout_ms), [&] { return size_ > 0 || eof_ || aborted_; }))
      return 0;
    if (gen_out) *gen_out = gen_;  // generation the returned bytes belong to
    if (size_ == 0) return -1;
    size_t chunk = std::min({n, size_, cap_ - head_});
    memcpy(d, buf_ + head_, chunk);
    head_ = (head_ + chunk) % cap_;
    size_ -= chunk;
    cv_.notify_all();
    return int(chunk);
  }
  // Drop all data, start a new generation (writers of older generations stop).
  uint32_t reset() {
    std::lock_guard<std::mutex> l(mu_);
    head_ = size_ = 0;
    eof_ = aborted_ = false;
    error_ = false;
    cv_.notify_all();
    return ++gen_;
  }
  void set_eof(uint32_t gen, bool error = false) {
    std::lock_guard<std::mutex> l(mu_);
    if (gen != gen_) return;
    eof_ = true;
    error_ = error;
    cv_.notify_all();
  }
  void abort() {
    std::lock_guard<std::mutex> l(mu_);
    aborted_ = true;
    cv_.notify_all();
  }
  size_t size() {
    std::lock_guard<std::mutex> l(mu_);
    return size_;
  }
  size_t capacity() const { return cap_; }
  bool eof_error() {
    std::lock_guard<std::mutex> l(mu_);
    return error_;
  }
  uint32_t generation() {
    std::lock_guard<std::mutex> l(mu_);
    return gen_;
  }

 private:
  size_t cap_;
  uint8_t* buf_;
  size_t head_ = 0, size_ = 0;
  bool eof_ = false, aborted_ = false, error_ = false;
  uint32_t gen_ = 0;
  std::mutex mu_;
  std::condition_variable cv_;
};

}  // namespace listenica
