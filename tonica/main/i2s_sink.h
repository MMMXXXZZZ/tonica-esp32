#pragma once
#include <atomic>
#include "driver/i2s_std.h"
#include "listenica/audio_sink.h"

// I2S output (Philips standard, 44.1 kHz s16 stereo) to an external DAC such
// as a PCM5102A or MAX98357A, or PDM to RC-filtered pins.
class I2sSink : public listenica::AudioSink {
 public:
  I2sSink(int bck, int ws, int dout, int mclk);  // standard I2S to an external DAC
  struct Pdm {};
  I2sSink(Pdm, int left, int right, int clk);     // PDM DAC mode: RC filter on each data pin
  void write(const int16_t* frames, size_t n) override;
  void flush() override;
  void set_paused(bool p) override;
  size_t latency_frames() override;

  uint64_t frames_written() const { return frames_; }

 private:
  i2s_chan_handle_t tx_ = nullptr;
  std::atomic<uint64_t> frames_{0};
  bool enabled_ = false;
  bool pdm_ = false;
};
