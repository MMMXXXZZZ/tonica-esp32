// IMA ADPCM (4 bits/sample, mono, one continuous stream from predictor 0 / index 0, low nibble
// first): 4x smaller than s16 for a faint hiss that speech over music hides. Used for Tónica's
// stored phrases and for spoken notifications sent to Sinfónica followers.
#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

namespace listenica {

inline const int16_t* adpcm_steps() {
  static const int16_t t[89] = {
      7,     8,     9,     10,    11,    12,    13,    14,    16,    17,    19,    21,    23,    25,    28,
      31,    34,    37,    41,    45,    50,    55,    60,    66,    73,    80,    88,    97,    107,   118,
      130,   143,   157,   173,   190,   209,   230,   253,   279,   307,   337,   371,   408,   449,   494,
      544,   598,   658,   724,   796,   876,   963,   1060,  1166,  1282,  1411,  1552,  1707,  1878,  2066,
      2272,  2499,  2749,  3024,  3327,  3660,  4026,  4428,  4871,  5358,  5894,  6484,  7132,  7845,  8630,
      9493,  10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767};
  return t;
}

struct Adpcm {
  int32_t pred = 0;
  int idx = 0;
  int16_t step_apply(uint8_t code) {  // shared by encoder and decoder
    static const int8_t kIdxAdj[16] = {-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8};
    int32_t step = adpcm_steps()[idx], delta = step >> 3;
    if (code & 4) delta += step;
    if (code & 2) delta += step >> 1;
    if (code & 1) delta += step >> 2;
    pred += (code & 8) ? -delta : delta;
    pred = pred < -32768 ? -32768 : pred > 32767 ? 32767 : pred;
    idx += kIdxAdj[code];
    idx = idx < 0 ? 0 : idx > 88 ? 88 : idx;
    return int16_t(pred);
  }
  uint8_t encode(int16_t s) {
    int32_t diff = int32_t(s) - pred, step = adpcm_steps()[idx];
    uint8_t code = 0;
    if (diff < 0) {
      code = 8;
      diff = -diff;
    }
    if (diff >= step) {
      code |= 4;
      diff -= step;
    }
    if (diff >= step >> 1) {
      code |= 2;
      diff -= step >> 1;
    }
    if (diff >= step >> 2) code |= 1;
    step_apply(code);
    return code;
  }
};

inline std::vector<uint8_t> adpcm_encode(const int16_t* pcm, size_t n) {
  std::vector<uint8_t> out((n + 1) / 2);
  Adpcm a;
  for (size_t i = 0; i < n; i++) {
    uint8_t c = a.encode(pcm[i]);
    out[i / 2] |= (i & 1) ? uint8_t(c << 4) : c;
  }
  return out;
}

inline std::vector<int16_t> adpcm_decode(const uint8_t* d, size_t n) {
  std::vector<int16_t> out(n * 2);
  Adpcm a;
  for (size_t i = 0; i < n; i++) {
    out[i * 2] = a.step_apply(d[i] & 15);
    out[i * 2 + 1] = a.step_apply(d[i] >> 4);
  }
  return out;
}

}  // namespace listenica
