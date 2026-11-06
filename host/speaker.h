#pragma once
// The computer's audio output for the Listenica host: waveOut on Windows, AudioQueue on macOS,
// ALSA on Linux. 44.1 kHz stereo s16. write() blocks while about `queue_ms` is waiting, so the
// player paces itself on it; latency_frames() is what is queued plus the output's own delay as far
// as the OS reports it, plus `extra_ms` for what it doesn't (Windows waveOut reports only its queue;
// macOS and ALSA also report their device latency).
#include <atomic>
#include <cstdint>
#include <memory>
#include "listenica/audio_sink.h"

class HostSpeaker : public listenica::AudioSink {
 public:
  // Output delay the OS doesn't report, in ms. Changeable while playing (the console's `delay`):
  // Sinfonica followers re-aim on the next block, so it can be tuned by ear.
  void set_extra_ms(uint32_t ms) { extra_frames_ = ms * 441 / 10; }
  uint32_t extra_ms() const { return extra_frames_.load() * 10 / 441; }

 protected:
  std::atomic<uint32_t> extra_frames_{0};
};

std::unique_ptr<HostSpeaker> open_speaker(uint32_t extra_ms);  // nullptr: no audio output

// The default `extra_ms` on this OS: 0 everywhere (measured by ear, 2026-11-05); tune with the
// console's `delay` for outputs that hide their delay (Bluetooth).
uint32_t speaker_default_extra_ms();
