// Linux audio output: ALSA's "default" device (PipeWire or PulseAudio route it on most desktops).
// writei() blocks while ALSA's buffer (~100 ms) is full, which paces the player; snd_pcm_delay()
// reports the frames still to be heard, including what the sound server holds.
#include "speaker.h"
#include <alsa/asoundlib.h>
#include <mutex>
#include "listenica/platform.h"

namespace {

class AlsaSpeaker : public HostSpeaker {
 public:
  explicit AlsaSpeaker(uint32_t extra_ms) { set_extra_ms(extra_ms); }
  ~AlsaSpeaker() override {
    if (pcm_) {
      snd_pcm_drop(pcm_);
      snd_pcm_close(pcm_);
    }
  }
  bool open() {
    if (snd_pcm_open(&pcm_, "default", SND_PCM_STREAM_PLAYBACK, 0) < 0) {
      pcm_ = nullptr;
      return false;
    }
    // s16le stereo 44.1 kHz, ALSA may resample, ~100 ms of buffer
    return snd_pcm_set_params(pcm_, SND_PCM_FORMAT_S16_LE, SND_PCM_ACCESS_RW_INTERLEAVED, 2, 44100, 1, 100000) == 0;
  }
  void write(const int16_t* frames, size_t n) override {
    std::lock_guard<std::mutex> l(mu_);
    while (n) {
      snd_pcm_sframes_t r = snd_pcm_writei(pcm_, frames, n);
      if (r < 0) {
        if (snd_pcm_recover(pcm_, int(r), 1) < 0) return;  // underrun (-EPIPE) or suspend: recovered
        continue;
      }
      frames += size_t(r) * 2;
      n -= size_t(r);
    }
  }
  size_t latency_frames() override {
    snd_pcm_sframes_t d = 0;
    if (!pcm_ || snd_pcm_delay(pcm_, &d) < 0 || d < 0) d = 0;
    return size_t(d) + extra_frames_.load();
  }
  // Not under mu_: a write blocked on a full buffer holds it. snd_pcm_pause fails harmlessly where
  // the device can't pause (the player stops writing then anyway).
  void set_paused(bool p) override {
    if (pcm_) snd_pcm_pause(pcm_, p ? 1 : 0);
  }

 private:
  snd_pcm_t* pcm_ = nullptr;
  std::mutex mu_;
};

}  // namespace

std::unique_ptr<HostSpeaker> open_speaker(uint32_t extra_ms) {
  auto s = std::make_unique<AlsaSpeaker>(extra_ms);
  if (!s->open()) return nullptr;
  return s;
}

uint32_t speaker_default_extra_ms() { return 0; }
