#include "listenica/shannon.h"
#include <cstring>

namespace listenica {

static constexpr uint32_t INITKONST = 0x6996c53a;
static constexpr int KEYP = 13;

static inline uint32_t rotl(uint32_t x, int c) { return (x << c) | (x >> (32 - c)); }
static inline uint32_t sbox1(uint32_t w) {
  w ^= rotl(w, 5) | rotl(w, 7);
  w ^= rotl(w, 19) | rotl(w, 22);
  return w;
}
static inline uint32_t sbox2(uint32_t w) {
  w ^= rotl(w, 7) | rotl(w, 22);
  w ^= rotl(w, 5) | rotl(w, 19);
  return w;
}
static inline uint32_t le32(const uint8_t* b) {
  return uint32_t(b[0]) | uint32_t(b[1]) << 8 | uint32_t(b[2]) << 16 | uint32_t(b[3]) << 24;
}
static inline void put_le32(uint8_t* b, uint32_t w) {
  b[0] = uint8_t(w);
  b[1] = uint8_t(w >> 8);
  b[2] = uint8_t(w >> 16);
  b[3] = uint8_t(w >> 24);
}

void Shannon::cycle() {
  uint32_t t = R[12] ^ R[13] ^ konst;
  t = sbox1(t) ^ rotl(R[0], 1);
  memmove(R, R + 1, (N - 1) * sizeof(uint32_t));
  R[N - 1] = t;
  t = sbox2(R[2] ^ R[15]);
  R[0] ^= t;
  sbuf = t ^ R[8] ^ R[12];
}

void Shannon::macfunc(uint32_t i) {
  uint32_t t = CRC[0] ^ CRC[2] ^ CRC[15] ^ i;
  memmove(CRC, CRC + 1, (N - 1) * sizeof(uint32_t));
  CRC[N - 1] = t;
  R[KEYP] ^= i;
}

void Shannon::diffuse() {
  for (int i = 0; i < N; i++) cycle();
}

void Shannon::load_key(const uint8_t* k, size_t len) {
  size_t i = 0;
  for (; i + 4 <= len; i += 4) {
    R[KEYP] ^= le32(k + i);
    cycle();
  }
  if (i < len) {
    uint8_t x[4] = {0, 0, 0, 0};
    memcpy(x, k + i, len - i);
    R[KEYP] ^= le32(x);
    cycle();
  }
  R[KEYP] ^= uint32_t(len);
  cycle();
  memcpy(CRC, R, sizeof R);
  diffuse();
  for (int j = 0; j < N; j++) R[j] ^= CRC[j];
}

Shannon::Shannon(const uint8_t* key, size_t len) {
  R[0] = R[1] = 1;
  for (int i = 2; i < N; i++) R[i] = R[i - 1] + R[i - 2];
  konst = INITKONST;
  load_key(key, len);
  konst = R[0];
  memcpy(initR, R, sizeof R);
  nbuf = 0;
}

void Shannon::nonce_u32(uint32_t n) {
  uint8_t b[4] = {uint8_t(n >> 24), uint8_t(n >> 16), uint8_t(n >> 8), uint8_t(n)};
  memcpy(R, initR, sizeof R);
  konst = INITKONST;
  load_key(b, 4);
  konst = R[0];
  nbuf = 0;
}

void Shannon::encrypt(uint8_t* buf, size_t n) {
  size_t i = 0;
  if (nbuf) {
    while (nbuf && i < n) {
      mbuf ^= uint32_t(buf[i]) << (32 - nbuf);
      buf[i] ^= (sbuf >> (32 - nbuf)) & 0xff;
      i++;
      nbuf -= 8;
    }
    if (nbuf) return;
    macfunc(mbuf);
  }
  for (; i + 4 <= n; i += 4) {
    cycle();
    uint32_t t = le32(buf + i);
    macfunc(t);
    put_le32(buf + i, t ^ sbuf);
  }
  if (i < n) {
    cycle();
    mbuf = 0;
    nbuf = 32;
    while (nbuf && i < n) {
      mbuf ^= uint32_t(buf[i]) << (32 - nbuf);
      buf[i] ^= (sbuf >> (32 - nbuf)) & 0xff;
      i++;
      nbuf -= 8;
    }
  }
}

void Shannon::decrypt(uint8_t* buf, size_t n) {
  size_t i = 0;
  if (nbuf) {
    while (nbuf && i < n) {
      buf[i] ^= (sbuf >> (32 - nbuf)) & 0xff;
      mbuf ^= uint32_t(buf[i]) << (32 - nbuf);
      i++;
      nbuf -= 8;
    }
    if (nbuf) return;
    macfunc(mbuf);
  }
  for (; i + 4 <= n; i += 4) {
    cycle();
    uint32_t t = le32(buf + i) ^ sbuf;
    macfunc(t);
    put_le32(buf + i, t);
  }
  if (i < n) {
    cycle();
    mbuf = 0;
    nbuf = 32;
    while (nbuf && i < n) {
      buf[i] ^= (sbuf >> (32 - nbuf)) & 0xff;
      mbuf ^= uint32_t(buf[i]) << (32 - nbuf);
      i++;
      nbuf -= 8;
    }
  }
}

void Shannon::finish(uint8_t* mac, size_t n) {
  if (nbuf) macfunc(mbuf);
  cycle();
  R[KEYP] ^= INITKONST ^ (uint32_t(nbuf) << 3);
  nbuf = 0;
  for (int j = 0; j < N; j++) R[j] ^= CRC[j];
  diffuse();
  size_t i = 0;
  while (i < n) {
    cycle();
    for (int b = 0; b < 4 && i < n; b++, i++) mac[i] = uint8_t(sbuf >> (8 * b));
  }
}

}  // namespace listenica
