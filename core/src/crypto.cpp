#include "listenica/crypto.h"
#include <cstring>
#include "mbedtls/aes.h"
#include "mbedtls/bignum.h"
#include "mbedtls/md.h"
#include "mbedtls/sha1.h"
extern "C" {
#include "puff.h"
}

namespace listenica {

std::array<uint8_t, 20> sha1(const uint8_t* d, size_t n) {
  std::array<uint8_t, 20> out{};
  mbedtls_sha1(d, n, out.data());
  return out;
}

std::array<uint8_t, 20> hmac_sha1(const uint8_t* key, size_t klen, const uint8_t* d, size_t n) {
  std::array<uint8_t, 20> out{};
  mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA1), key, klen, d, n, out.data());
  return out;
}

Bytes modexp(const Bytes& base, const Bytes& exp, const Bytes& mod) {
  mbedtls_mpi B, E, N, X;
  mbedtls_mpi_init(&B);
  mbedtls_mpi_init(&E);
  mbedtls_mpi_init(&N);
  mbedtls_mpi_init(&X);
  mbedtls_mpi_read_binary(&B, base.data(), base.size());
  mbedtls_mpi_read_binary(&E, exp.data(), exp.size());
  mbedtls_mpi_read_binary(&N, mod.data(), mod.size());
  Bytes out(mod.size());
  if (mbedtls_mpi_exp_mod(&X, &B, &E, &N, nullptr) == 0)
    mbedtls_mpi_write_binary(&X, out.data(), out.size());
  mbedtls_mpi_free(&B);
  mbedtls_mpi_free(&E);
  mbedtls_mpi_free(&N);
  mbedtls_mpi_free(&X);
  return out;
}

bool rsa_verify_pkcs1_sha1(const Bytes& modulus, const Bytes& sig, const uint8_t* msg, size_t n) {
  static const uint8_t kDigestInfo[] = {0x30, 0x21, 0x30, 0x09, 0x06, 0x05, 0x2b, 0x0e,
                                        0x03, 0x02, 0x1a, 0x05, 0x00, 0x04, 0x14};
  Bytes m = modexp(sig, Bytes{0x01, 0x00, 0x01}, modulus);
  auto h = sha1(msg, n);
  size_t k = modulus.size();
  size_t tlen = sizeof kDigestInfo + 20;
  if (k < tlen + 11) return false;
  Bytes expect(k, 0xff);
  expect[0] = 0x00;
  expect[1] = 0x01;
  expect[k - tlen - 1] = 0x00;
  memcpy(&expect[k - tlen], kDigestInfo, sizeof kDigestInfo);
  memcpy(&expect[k - 20], h.data(), 20);
  return m == expect;
}

Aes128Ctr::Aes128Ctr(const uint8_t key[16], const uint8_t iv[16]) {
  auto* c = new mbedtls_aes_context;
  mbedtls_aes_init(c);
  mbedtls_aes_setkey_enc(c, key, 128);
  ctx_ = c;
  memcpy(iv_, iv, 16);
  memcpy(counter_, iv, 16);
}

Aes128Ctr::~Aes128Ctr() {
  auto* c = static_cast<mbedtls_aes_context*>(ctx_);
  mbedtls_aes_free(c);
  delete c;
}

static void add_be128(uint8_t ctr[16], uint64_t v) {
  for (int i = 15; i >= 0 && v; i--) {
    uint64_t s = (uint64_t)ctr[i] + (v & 0xff);
    ctr[i] = uint8_t(s);
    v = (v >> 8) + (s >> 8);
  }
}

void Aes128Ctr::seek(uint64_t offset) {
  memcpy(counter_, iv_, 16);
  add_be128(counter_, offset / 16);
  used_ = 16;
  size_t skip = offset % 16;
  if (skip) {
    mbedtls_aes_crypt_ecb(static_cast<mbedtls_aes_context*>(ctx_), MBEDTLS_AES_ENCRYPT, counter_, stream_);
    add_be128(counter_, 1);
    used_ = skip;
  }
}

void Aes128Ctr::apply(uint8_t* d, size_t n) {
  auto* c = static_cast<mbedtls_aes_context*>(ctx_);
  for (size_t i = 0; i < n; i++) {
    if (used_ == 16) {
      mbedtls_aes_crypt_ecb(c, MBEDTLS_AES_ENCRYPT, counter_, stream_);
      add_be128(counter_, 1);
      used_ = 0;
    }
    d[i] ^= stream_[used_++];
  }
}

bool gunzip(const uint8_t* d, size_t n, Bytes& out) {
  size_t off = 0;
  if (n >= 10 && d[0] == 0x1f && d[1] == 0x8b) {  // gzip header (RFC 1952)
    uint8_t flg = d[3];
    off = 10;
    if (flg & 4) {
      if (off + 2 > n) return false;
      off += 2 + (d[off] | d[off + 1] << 8);
    }
    if (flg & 8) while (off < n && d[off++]) {}
    if (flg & 16) while (off < n && d[off++]) {}
    if (flg & 2) off += 2;
  } else if (n >= 2 && (d[0] & 0x0f) == 8 && ((d[0] << 8) | d[1]) % 31 == 0) {
    off = 2;  // zlib header
  }
  if (off >= n) return false;
  unsigned long srclen = (unsigned long)(n - off), dstlen = 0;
  if (puff(nullptr, &dstlen, d + off, &srclen) != 0) return false;
  out.resize(dstlen);
  srclen = (unsigned long)(n - off);
  return puff(out.data(), &dstlen, d + off, &srclen) == 0;
}

}  // namespace listenica
