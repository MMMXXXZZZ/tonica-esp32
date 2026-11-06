// Decode benchmark (host only): decodes an Ogg Vorbis file the way Player does (pushdata
// API fed in 4 KB chunks, float -> s16 conversion with the normalisation gain) and reports
// speed, the decode/convert split and the decoder's memory needs.
//
//   listenica_bench <file.ogg> [runs]
//
// Relative numbers carry over to the ESP32-S3; absolute speed does not (no PSRAM cache
// misses here). Memory needs are exact.
#define STB_VORBIS_NO_STDIO 1
#define STB_VORBIS_NO_PULLDATA_API 1
#define STB_VORBIS_NO_INTEGER_CONVERSION 1
#ifndef BENCH_OWN_STB
#define STB_VORBIS_HEADER_ONLY 1  // the core library's decoder (the firmware's flags)
#endif
#include "stb_vorbis.c"           // BENCH_OWN_STB: a private copy built with -D variants
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

using Clock = std::chrono::steady_clock;
static double secs(Clock::duration d) { return std::chrono::duration<double>(d).count(); }

int main(int argc, char** argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: listenica_bench <file.ogg> [runs]\n");
    return 2;
  }
  int runs = argc > 2 ? atoi(argv[2]) : 3;
  FILE* f = fopen(argv[1], "rb");
  if (!f) return perror(argv[1]), 1;
  std::vector<uint8_t> file;
  uint8_t buf[65536];
  size_t n;
  while ((n = fread(buf, 1, sizeof buf, f)) > 0) file.insert(file.end(), buf, buf + n);
  fclose(f);
  size_t start = 0;  // Spotify files carry a 0xa7-byte proprietary page first
  if (file.size() > 0xa7 + 4 && memcmp(&file[0xa7], "OggS", 4) == 0 && memcmp(&file[0x1c], "\x01vorbis", 7) != 0) start = 0xa7;

  double best_total = 1e9, best_dec = 0, best_conv = 0;
  uint64_t frames_out = 0;
  int rate = 0, channels = 0;
  stb_vorbis_info info = {};
  for (int run = 0; run < runs; run++) {
    std::vector<uint8_t> in;
    in.reserve(64 * 1024);
    size_t pos = start, in_pos = 0;
    auto feed = [&] {  // 4 KB at a time, like the CDN ring reads
      if (in_pos > 0 && in_pos >= in.size() / 2) {
        in.erase(in.begin(), in.begin() + long(in_pos));
        in_pos = 0;
      }
      size_t k = std::min<size_t>(4096, file.size() - pos);
      in.insert(in.end(), file.begin() + long(pos), file.begin() + long(pos + k));
      pos += k;
      return k > 0;
    };
    stb_vorbis* v = nullptr;
    while (!v) {
      if (!feed()) return fprintf(stderr, "no Vorbis stream\n"), 1;
      int used = 0, err = 0;
      v = stb_vorbis_open_pushdata(in.data(), int(in.size()), &used, &err, nullptr);
      if (v) in_pos = size_t(used);
      else if (err != VORBIS_need_more_data) return fprintf(stderr, "open failed: %d\n", err), 1;
    }
    info = stb_vorbis_get_info(v);
    rate = int(info.sample_rate);
    channels = info.channels;
    std::vector<int16_t> pending;
    double t_dec = 0, t_conv = 0;
    uint64_t out_frames = 0;
    auto t0 = Clock::now();
    for (;;) {
      int ch = 0, samples = 0;
      float** out = nullptr;
      auto d0 = Clock::now();
      int used = in.size() > in_pos ? stb_vorbis_decode_frame_pushdata(v, in.data() + in_pos, int(in.size() - in_pos), &ch, &out, &samples) : 0;
      t_dec += secs(Clock::now() - d0);
      if (used == 0 && samples == 0) {
        if (!feed()) break;
        continue;
      }
      in_pos += size_t(used);
      if (samples <= 0) continue;
      auto c0 = Clock::now();
      const float g = 0.9f * 32767.0f;
      pending.resize(size_t(samples) * 2);
      for (int i = 0; i < samples; i++) {
        float l = out[0][i], r = ch > 1 ? out[1][i] : l;
        auto cv = [](float x) -> int16_t { return x > 32767.f ? 32767 : x < -32768.f ? -32768 : int16_t(x); };
        pending[size_t(i) * 2] = cv(l * g);
        pending[size_t(i) * 2 + 1] = cv(r * g);
      }
      t_conv += secs(Clock::now() - c0);
      out_frames += uint64_t(samples);
    }
    double total = secs(Clock::now() - t0);
    stb_vorbis_close(v);
    frames_out = out_frames;
    if (total < best_total) best_total = total, best_dec = t_dec, best_conv = t_conv;
  }
  double audio = double(frames_out) / rate;
  printf("audio %.2f s, %d Hz, %d ch, %.0f kbps\n", audio, rate, channels, (file.size() - start) * 8 / audio / 1000);
  printf("best of %d: %.3f s total = %.0fx realtime (decode %.3f s, convert %.3f s = %.1f%%)\n", runs, best_total,
         audio / best_total, best_dec, best_conv, 100 * best_conv / best_total);
  printf("decoder memory: setup %u B, setup temp %u B, temp %u B, max frame %d samples\n",
         info.setup_memory_required, info.setup_temp_memory_required, info.temp_memory_required, info.max_frame_size);
  return 0;
}
