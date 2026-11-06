#pragma once
#include <cstddef>
#include <cstdint>

namespace listenica {

// 44.1 kHz, stereo, interleaved s16 PCM consumer. write() should block for
// pacing when the sink is real-time (I2S); non-real-time sinks may return
// immediately.
class AudioSink {
 public:
  virtual ~AudioSink() = default;
  virtual void write(const int16_t* frames, size_t nframes) = 0;
  // Called on pause / seek / track change so buffered audio can be dropped.
  virtual void flush() {}
  virtual void set_paused(bool) {}
  // Frames queued inside the sink but not yet audible (for position math).
  virtual size_t latency_frames() { return 0; }
  // Hardware volume (0..65535). Sinks that can scale in analog/DAC hardware
  // (better SNR than digital scaling) store it and return true while active.
  virtual void set_hw_volume(uint16_t) {}
  virtual bool hw_volume_active() { return false; }
};

}  // namespace listenica
