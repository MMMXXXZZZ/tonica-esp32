#pragma once
#include "listenica/platform.h"
#include <array>
#include <memory>

namespace listenica {

void random_bytes(uint8_t* out, size_t n);
std::array<uint8_t, 20> sha1(const uint8_t* d, size_t n);
std::array<uint8_t, 20> hmac_sha1(const uint8_t* key, size_t klen, const uint8_t* d, size_t n);

// Big-endian modular exponentiation: out = base^exp mod mod, padded to mod size.
Bytes modexp(const Bytes& base, const Bytes& exp, const Bytes& mod);
bool rsa_verify_pkcs1_sha1(const Bytes& modulus, const Bytes& sig, const uint8_t* msg, size_t n);

// AES-128-CTR with a 128-bit big-endian counter, seekable by byte offset.
class Aes128Ctr {
 public:
  Aes128Ctr(const uint8_t key[16], const uint8_t iv[16]);
  ~Aes128Ctr();
  void seek(uint64_t offset);
  void apply(uint8_t* d, size_t n);  // in place

 private:
  void* ctx_;
  uint8_t iv_[16];
  uint8_t counter_[16];
  uint8_t stream_[16];
  size_t used_ = 16;
};

// gzip/zlib/raw-deflate inflate. Returns false on corrupt input.
bool gunzip(const uint8_t* d, size_t n, Bytes& out);

}  // namespace listenica
