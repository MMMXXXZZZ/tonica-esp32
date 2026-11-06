// Shannon stream cipher with MAC (Hawkes & Rose, Qualcomm reference design),
// used to frame traffic to the Spotify access point. Each message is
// processed with a fresh nonce, then finish() yields the 4-byte MAC.
#pragma once
#include <cstddef>
#include <cstdint>

namespace listenica {

class Shannon {
 public:
  explicit Shannon(const uint8_t* key, size_t len);
  void nonce_u32(uint32_t n);
  void encrypt(uint8_t* buf, size_t n);
  void decrypt(uint8_t* buf, size_t n);
  void finish(uint8_t* mac, size_t n);

 private:
  static constexpr int N = 16;
  uint32_t R[N], CRC[N], initR[N];
  uint32_t konst = 0, sbuf = 0, mbuf = 0;
  int nbuf = 0;
  void cycle();
  void macfunc(uint32_t i);
  void diffuse();
  void load_key(const uint8_t* k, size_t len);
};

}  // namespace listenica
