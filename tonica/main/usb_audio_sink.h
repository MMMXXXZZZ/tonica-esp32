#pragma once
#include <atomic>
#include <functional>
#include <mutex>
#include "listenica/audio_sink.h"
#include "usb/uac_host.h"

// USB Audio Class output: the ESP32-S3's native USB port runs in host mode
// and drives a USB audio adapter (e.g. a USB-C to 3.5 mm dongle). Uses
// espressif/usb_host_uac. Input is 44.1 kHz s16 stereo; output is converted
// to whatever the adapter supports (rate via linear interpolation, 16/24/32
// bit, mono/stereo).
class UsbAudioSink {
 public:
  void start();  // installs USB host + UAC driver tasks
  bool connected() const { return dev_ != nullptr && started_; }
  // Blocking write (paced by the USB isochronous clock). Returns false if no
  // device is connected (caller falls back to another sink).
  bool write(const int16_t* frames, size_t n);
  uint32_t out_rate() const { return rate_; }
  uint32_t errors() const { return errors_; }
  uint64_t bytes() const { return bytes_; }
  // Frames (at 44.1 kHz) handed over but not yet played: the driver's TX ring plus the URBs in
  // flight (CONFIG_UAC_NUM_ISOC_URBS x CONFIG_UAC_NUM_PACKETS_PER_URB packets of 1 ms).
  size_t queued_frames();
#if LISTENICA_DIAG
  void test_tone(uint32_t ms);  // diagnostics: 1 kHz tone straight to USB
#endif
  void set_volume(uint16_t v);  // Spotify scale; applied to the adapter's feature unit
  bool hw_volume_ok() const { return connected() && hw_vol_ok_; }

  void on_driver_event(uint8_t addr, uint8_t iface, uac_host_driver_event_t ev);
  void on_device_event(uac_host_device_handle_t h, uac_host_device_event_t ev);

 private:
  std::atomic<uac_host_device_handle_t> dev_{nullptr};
  std::atomic<bool> started_{false};
  uint32_t rate_ = 0;
  uint8_t channels_ = 2, subframe_ = 2, bits_ = 16;
  double phase_ = 0.0;  // resampler position between input frames
  int16_t last_[2] = {0, 0};
  std::atomic<uint32_t> errors_{0};
  std::atomic<uint64_t> bytes_{0};
  std::atomic<uint16_t> want_vol_{0xFFFF};
  std::atomic<bool> hw_vol_ok_{false};
  void apply_volume(uac_host_device_handle_t h);
  int slow_logs_ = 0;
  size_t last_queued_ = 0;  // last ring level seen by queued_frames()
  std::mutex mu_;
  // staging for uac_host_device_write (which copies into its own ring): PSRAM, allocated on
  // first use, so it doesn't take 8 KB of internal RAM
  static constexpr size_t kOutBytes = 8192;
  uint8_t* out_ = nullptr;
  void open_device(uint8_t addr, uint8_t iface);
};

// Routes PCM to the USB adapter when one is connected, else to the fallback
// (I2S). Exactly one sink paces playback at any time, so clocks never fight.
class RouterSink : public listenica::AudioSink {
 public:
  // The fallback sink (I2S/PDM: ~16 KB of internal DMA RAM) is created only
  // when audio is actually written without a USB device present.
  RouterSink(std::function<listenica::AudioSink*()> make_fallback, UsbAudioSink* usb)
      : make_fb_(std::move(make_fallback)), usb_(usb) {}
  void write(const int16_t* frames, size_t n) override {
    if (usb_ && usb_->connected() && usb_->write(frames, n)) {
      frames_ += n;
      return;
    }
    if (!fb_ && make_fb_) fb_ = make_fb_();
    if (fb_) fb_->write(frames, n);
    frames_ += n;
  }
  void flush() override {
    if (fb_) fb_->flush();
  }
  void set_paused(bool p) override {
    if (fb_) fb_->set_paused(p);
  }
  size_t latency_frames() override {
    if (usb_ && usb_->connected()) return usb_->queued_frames();
    return fb_ ? fb_->latency_frames() : 0;
  }
  void set_hw_volume(uint16_t v) override {
    if (usb_) usb_->set_volume(v);
  }
  bool hw_volume_active() override { return usb_ && usb_->hw_volume_ok(); }
  uint64_t frames() const { return frames_; }

 private:
  std::function<listenica::AudioSink*()> make_fb_;
  listenica::AudioSink* fb_ = nullptr;
  UsbAudioSink* usb_;
  uint64_t frames_ = 0;
};
