// macOS audio output: an AudioQueue fed from a small ring (callbacks on the queue's own thread).
// Latency = frames not yet played (written minus the queue's sample time) + the default output
// device's reported latency, safety offset and stream latency (Bluetooth devices report theirs).
#include "speaker.h"
#include <AudioToolbox/AudioToolbox.h>
#include <CoreAudio/CoreAudio.h>
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <vector>
#include "listenica/platform.h"

namespace {

constexpr uint32_t kRate = 44100, kBufFrames = 1024, kBufs = 4;

uint32_t device_latency_frames() {
  AudioObjectID dev = kAudioObjectUnknown;
  UInt32 sz = sizeof dev;
  AudioObjectPropertyAddress a = {kAudioHardwarePropertyDefaultOutputDevice, kAudioObjectPropertyScopeGlobal,
                                  kAudioObjectPropertyElementMain};
  if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, 0, nullptr, &sz, &dev) != noErr) return 0;
  uint32_t total = 0;
  for (AudioObjectPropertySelector sel : {kAudioDevicePropertyLatency, kAudioDevicePropertySafetyOffset}) {
    UInt32 v = 0;
    sz = sizeof v;
    a = {sel, kAudioObjectPropertyScopeOutput, kAudioObjectPropertyElementMain};
    if (AudioObjectGetPropertyData(dev, &a, 0, nullptr, &sz, &v) == noErr) total += v;
  }
  a = {kAudioDevicePropertyStreams, kAudioObjectPropertyScopeOutput, kAudioObjectPropertyElementMain};
  if (AudioObjectGetPropertyDataSize(dev, &a, 0, nullptr, &sz) == noErr && sz >= sizeof(AudioStreamID)) {
    std::vector<AudioStreamID> streams(sz / sizeof(AudioStreamID));
    if (AudioObjectGetPropertyData(dev, &a, 0, nullptr, &sz, streams.data()) == noErr) {
      UInt32 v = 0;
      UInt32 vs = sizeof v;
      AudioObjectPropertyAddress sl = {kAudioStreamPropertyLatency, kAudioObjectPropertyScopeGlobal,
                                       kAudioObjectPropertyElementMain};
      if (AudioObjectGetPropertyData(streams[0], &sl, 0, nullptr, &vs, &v) == noErr) total += v;
    }
  }
  return total;  // device frames; at 44.1/48 kHz close enough to our frames for a delay estimate
}

class MacSpeaker : public HostSpeaker {
 public:
  MacSpeaker(uint32_t extra_ms, uint32_t queue_ms = 60) : queue_(queue_ms * kRate / 1000) { set_extra_ms(extra_ms); }
  ~MacSpeaker() override {
    if (q_) {
      AudioQueueStop(q_, true);
      AudioQueueDispose(q_, true);
    }
  }
  bool open() {
    AudioStreamBasicDescription f = {};
    f.mSampleRate = kRate;
    f.mFormatID = kAudioFormatLinearPCM;
    f.mFormatFlags = kLinearPCMFormatFlagIsSignedInteger | kLinearPCMFormatFlagIsPacked;
    f.mFramesPerPacket = 1;
    f.mChannelsPerFrame = 2;
    f.mBitsPerChannel = 16;
    f.mBytesPerFrame = 4;
    f.mBytesPerPacket = 4;
    if (AudioQueueNewOutput(&f, &MacSpeaker::callback, this, nullptr, nullptr, 0, &q_) != noErr) return false;
    for (uint32_t i = 0; i < kBufs; i++) {
      AudioQueueBufferRef b = nullptr;
      if (AudioQueueAllocateBuffer(q_, kBufFrames * 4, &b) != noErr) return false;
      fill(b);  // silence until the player writes
      AudioQueueEnqueueBuffer(q_, b, 0, nullptr);
    }
    dev_latency_ = device_latency_frames();
    return AudioQueueStart(q_, nullptr) == noErr;
  }
  void write(const int16_t* frames, size_t n) override {
    std::unique_lock<std::mutex> l(mu_);
    while (n) {
      cv_.wait(l, [&] { return ring_.size() / 2 < queue_ + kBufFrames; });
      size_t room = queue_ + kBufFrames - ring_.size() / 2, take = std::min(n, room);
      ring_.insert(ring_.end(), frames, frames + take * 2);
      frames += take * 2;
      n -= take;
    }
  }
  size_t latency_frames() override {
    AudioTimeStamp ts = {};
    int64_t played = 0;
    if (q_ && AudioQueueGetCurrentTime(q_, nullptr, &ts, nullptr) == noErr && (ts.mFlags & kAudioTimeStampSampleTimeValid))
      played = int64_t(ts.mSampleTime);
    std::lock_guard<std::mutex> l(mu_);
    int64_t in_queue = std::max<int64_t>(0, int64_t(enqueued_) - played);
    return size_t(in_queue) + ring_.size() / 2 + dev_latency_ + extra_frames_.load();
  }
  void set_paused(bool p) override {
    if (!q_) return;
    if (p) AudioQueuePause(q_);
    else AudioQueueStart(q_, nullptr);
  }

 private:
  AudioQueueRef q_ = nullptr;
  std::mutex mu_;
  std::condition_variable cv_;
  std::vector<int16_t> ring_;  // interleaved, waiting for a queue buffer
  uint64_t enqueued_ = 0;      // frames handed to the queue
  uint32_t queue_, dev_latency_ = 0;

  void fill(AudioQueueBufferRef b) {  // caller may or may not hold mu_ (open: before any write)
    size_t have = std::min<size_t>(ring_.size() / 2, kBufFrames);
    auto* out = static_cast<int16_t*>(b->mAudioData);
    if (have) memcpy(out, ring_.data(), have * 4);
    if (have < kBufFrames) memset(out + have * 2, 0, (kBufFrames - have) * 4);  // underrun: silence
    ring_.erase(ring_.begin(), ring_.begin() + long(have * 2));
    b->mAudioDataByteSize = kBufFrames * 4;
    enqueued_ += kBufFrames;
  }
  static void callback(void* self, AudioQueueRef q, AudioQueueBufferRef b) {
    auto* s = static_cast<MacSpeaker*>(self);
    {
      std::lock_guard<std::mutex> l(s->mu_);
      s->fill(b);
    }
    s->cv_.notify_all();
    AudioQueueEnqueueBuffer(q, b, 0, nullptr);
  }
};

}  // namespace

std::unique_ptr<HostSpeaker> open_speaker(uint32_t extra_ms) {
  auto s = std::make_unique<MacSpeaker>(extra_ms);
  if (!s->open()) return nullptr;
  return s;
}

uint32_t speaker_default_extra_ms() { return 0; }
