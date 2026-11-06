#include "speaker.h"
#include "waveout_sink.h"

std::unique_ptr<HostSpeaker> open_speaker(uint32_t extra_ms) {
  auto s = std::make_unique<WaveOutSink>(extra_ms);
  if (!s->open()) return nullptr;
  return s;
}

uint32_t speaker_default_extra_ms() { return 0; }  // see print_latency_notice in main.cpp
