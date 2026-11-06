// Windows waveOut output for the host (Sinfonica prototype: an audible leader or follower).
// write() blocks while more than `queue_ms` is queued; latency_frames() is what is queued plus a
// fixed `extra_ms` for the rest of the Windows audio path, which waveOut can't report.
#pragma once
#ifdef _WIN32
#include <windows.h>
#include <mmsystem.h>
#include <vector>
#include "speaker.h"
#include "listenica/platform.h"

class WaveOutSink : public HostSpeaker {
 public:
  WaveOutSink(uint32_t extra_ms, uint32_t queue_ms = 60) : queue_(queue_ms * 441 / 10) { set_extra_ms(extra_ms); }
  ~WaveOutSink() override {
    if (!wo_) return;
    waveOutReset(wo_);
    for (auto& h : hdr_) waveOutUnprepareHeader(wo_, &h, sizeof(WAVEHDR));
    waveOutClose(wo_);
  }
  bool open() {
    WAVEFORMATEX f = {};
    f.wFormatTag = WAVE_FORMAT_PCM;
    f.nChannels = 2;
    f.nSamplesPerSec = 44100;
    f.wBitsPerSample = 16;
    f.nBlockAlign = 4;
    f.nAvgBytesPerSec = 44100 * 4;
    if (waveOutOpen(&wo_, WAVE_MAPPER, &f, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) return false;
    data_.assign(kBufs, std::vector<int16_t>(kMaxFrames * 2));
    hdr_.resize(kBufs);
    for (int i = 0; i < kBufs; i++) {
      hdr_[i] = {};
      hdr_[i].lpData = (LPSTR)data_[i].data();
      hdr_[i].dwBufferLength = kMaxFrames * 4;
      waveOutPrepareHeader(wo_, &hdr_[i], sizeof(WAVEHDR));
      hdr_[i].dwFlags |= WHDR_DONE;
    }
    return true;
  }
  void write(const int16_t* frames, size_t n) override {
    while (n) {
      size_t chunk = n < kMaxFrames ? n : kMaxFrames;
      while (queued() > queue_ || !(hdr_[next_].dwFlags & WHDR_DONE)) listenica::sleep_ms(1);
      memcpy(data_[next_].data(), frames, chunk * 4);
      hdr_[next_].dwBufferLength = DWORD(chunk * 4);
      hdr_[next_].dwFlags &= ~WHDR_DONE;
      waveOutWrite(wo_, &hdr_[next_], sizeof(WAVEHDR));
      written_ += chunk;
      next_ = (next_ + 1) % kBufs;
      frames += chunk * 2;
      n -= chunk;
    }
  }
  size_t latency_frames() override { return size_t(queued()) + extra_frames_.load(); }
  void flush() override {}  // keep it simple: queued audio plays out (<= queue_ms)
  void set_paused(bool p) override {
    if (p) waveOutPause(wo_);
    else waveOutRestart(wo_);
  }

 private:
  static constexpr int kBufs = 32;
  static constexpr size_t kMaxFrames = 1024;
  HWAVEOUT wo_ = nullptr;
  std::vector<std::vector<int16_t>> data_;
  std::vector<WAVEHDR> hdr_;
  uint64_t written_ = 0;
  int next_ = 0;
  uint32_t queue_;
  uint64_t queued() {
    MMTIME mt = {};
    mt.wType = TIME_SAMPLES;
    waveOutGetPosition(wo_, &mt, sizeof mt);
    uint64_t played = mt.u.sample;
    return written_ > played ? written_ - played : 0;
  }
};
#endif
