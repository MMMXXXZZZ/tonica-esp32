#include "usb_audio_sink.h"
#include <cstring>
#include "esp_log.h"
#include "esp_timer.h"
#include <cmath>
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "usb/usb_host.h"

static const char* TAG = "usb_audio";
static UsbAudioSink* g_self = nullptr;

// UAC driver callbacks run in the driver's background task, which is also the
// task that completes control transfers. Doing open/start (control transfers)
// inside a callback deadlocks it, so callbacks only enqueue and a separate
// task does the work (same structure as the component's audio_player example).
struct UacEvent {
  bool driver;
  uint8_t addr, iface;
  int ev;
  uac_host_device_handle_t h;
};
static QueueHandle_t s_q = nullptr;

static void uac_worker(void* arg) {
  auto* self = static_cast<UsbAudioSink*>(arg);
  UacEvent e;
  for (;;) {
    if (xQueueReceive(s_q, &e, portMAX_DELAY) != pdTRUE) continue;
    if (e.driver)
      self->on_driver_event(e.addr, e.iface, (uac_host_driver_event_t)e.ev);
    else
      self->on_device_event(e.h, (uac_host_device_event_t)e.ev);
  }
}

static void usb_lib_task(void* arg) {
  usb_host_config_t cfg = {};
  cfg.skip_phy_setup = false;
  cfg.intr_flags = ESP_INTR_FLAG_LOWMED;
  ESP_ERROR_CHECK(usb_host_install(&cfg));
  xTaskNotifyGive((TaskHandle_t)arg);
  for (;;) {
    uint32_t flags;
    usb_host_lib_handle_events(portMAX_DELAY, &flags);
    if (flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) usb_host_device_free_all();
  }
}

static void driver_cb(uint8_t addr, uint8_t iface, const uac_host_driver_event_t ev, void*) {
  UacEvent e = {true, addr, iface, int(ev), nullptr};
  if (s_q) xQueueSend(s_q, &e, 0);
}
static void device_cb(uac_host_device_handle_t h, const uac_host_device_event_t ev, void*) {
  if (ev == UAC_HOST_DEVICE_EVENT_TX_DONE) return;  // frequent; not needed (writes block on the ring buffer)
  UacEvent e = {false, 0, 0, int(ev), h};
  if (s_q) xQueueSend(s_q, &e, 0);
}

void UsbAudioSink::start() {
  g_self = this;
  s_q = xQueueCreate(16, sizeof(UacEvent));
  // PSRAM stack: open/start/volume control transfers only, never flash
  xTaskCreatePinnedToCoreWithCaps(uac_worker, "uac_worker", 6144, this, 5, nullptr, 0, MALLOC_CAP_SPIRAM);
  // USB tasks run above lwIP/TLS and away from the Wi-Fi core: late isochronous
  // resubmission makes adaptive USB DACs drift (audible pitch wobble).
  xTaskCreatePinnedToCore(usb_lib_task, "usb_lib", 4096, xTaskGetCurrentTaskHandle(), 20, nullptr, 1);
  ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(5000));
  uac_host_driver_config_t uc = {};
  uc.create_background_task = true;
  uc.task_priority = 20;
  uc.stack_size = 4096;
  uc.core_id = 1;
  uc.callback = driver_cb;
  uc.callback_arg = nullptr;
  ESP_ERROR_CHECK(uac_host_install(&uc));
  ESP_LOGI(TAG, "USB host + UAC driver ready: plug a USB audio adapter into the native USB port");
}

void UsbAudioSink::on_driver_event(uint8_t addr, uint8_t iface, uac_host_driver_event_t ev) {
  if (ev == UAC_HOST_DRIVER_EVENT_TX_CONNECTED) {
    open_device(addr, iface);  // runs in uac_worker, never in the driver task
  }
}

void UsbAudioSink::open_device(uint8_t addr, uint8_t iface) {
  uac_host_device_config_t dc = {};
  dc.addr = addr;
  dc.iface_num = iface;
  dc.buffer_size = 32000;  // ~180 ms at 44.1 kHz/16-bit stereo
  dc.buffer_threshold = 8000;
  dc.callback = device_cb;
  dc.callback_arg = nullptr;
  uac_host_device_handle_t h = nullptr;
  if (uac_host_device_open(&dc, &h) != ESP_OK) {
    ESP_LOGE(TAG, "open failed");
    return;
  }
  uac_host_dev_info_t info;
  uac_host_get_device_info(h, &info);
  ESP_LOGI(TAG, "speaker connected: VID %04x PID %04x, %u alt settings", info.VID, info.PID, info.iface_alt_num);
  uac_host_printf_device_param(h);
  // Prefer 44.1 kHz stereo 16-bit; otherwise the first alt setting, resampled.
  int best_alt = -1, best_score = -1;
  uint32_t best_rate = 0;
  uac_host_dev_alt_param_t best = {};
  for (uint8_t alt = 1; alt <= info.iface_alt_num; alt++) {
    uac_host_dev_alt_param_t p;
    if (uac_host_get_device_alt_param(h, alt, &p) != ESP_OK) continue;
    auto supports = [&](uint32_t f) {
      if (p.sample_freq_type > 0) {
        for (int i = 0; i < p.sample_freq_type && i < UAC_FREQ_NUM_MAX; i++)
          if (p.sample_freq[i] == f) return true;
        return false;
      }
      return f >= p.sample_freq_lower && f <= p.sample_freq_upper;
    };
    uint32_t rate = supports(44100) ? 44100 : supports(48000) ? 48000
                    : (p.sample_freq_type > 0 ? p.sample_freq[0] : p.sample_freq_lower);
    int score = (rate == 44100 ? 4 : rate == 48000 ? 2 : 0) + (p.channels == 2 ? 2 : 0) + (p.bit_resolution == 16 ? 1 : 0);
    if (score > best_score) {
      best_score = score;
      best_alt = alt;
      best_rate = rate;
      best = p;
    }
  }
  if (best_alt < 0) {
    ESP_LOGE(TAG, "no usable alt setting");
    uac_host_device_close(h);
    return;
  }
  uac_host_stream_config_t sc = {};
  sc.channels = best.channels;
  sc.bit_resolution = best.bit_resolution;
  sc.sample_freq = best_rate;
  if (uac_host_device_start(h, &sc) != ESP_OK) {
    ESP_LOGE(TAG, "stream start failed");
    uac_host_device_close(h);
    return;
  }
  apply_volume(h);  // Spotify volume -> adapter gain (better SNR than digital scaling)
  {
    std::lock_guard<std::mutex> l(mu_);
    rate_ = best_rate;
    channels_ = best.channels;
    subframe_ = best.subframe_size;
    bits_ = best.bit_resolution;
    phase_ = 0;
  }
  dev_ = h;
  started_ = true;
  ESP_LOGI(TAG, "streaming to USB adapter: %u Hz, %u ch, %u-bit (%u-byte subframes)%s", (unsigned)rate_, channels_,
           bits_, subframe_, rate_ != 44100 ? ", resampling from 44.1 kHz" : "");
}

void UsbAudioSink::apply_volume(uac_host_device_handle_t h) {
  uint32_t v = want_vol_.load();
  int pct = int((v * 100 + 32767) / 65535);
  esp_err_t e = uac_host_device_set_mute(h, pct == 0);
  if (pct > 0) e = uac_host_device_set_volume(h, uint8_t(pct));
  hw_vol_ok_ = (e == ESP_OK);
  if (e != ESP_OK) ESP_LOGW(TAG, "adapter volume control unavailable (%s); using digital volume", esp_err_to_name(e));
}

void UsbAudioSink::set_volume(uint16_t v) {
  want_vol_ = v;
  std::lock_guard<std::mutex> l(mu_);  // the device can't be closed under us
  auto h = dev_.load();
  if (h && started_) apply_volume(h);  // called from the Connect task, never the UAC driver task
}

void UsbAudioSink::on_device_event(uac_host_device_handle_t h, uac_host_device_event_t ev) {
  if (ev == UAC_HOST_DRIVER_EVENT_DISCONNECTED) {
    ESP_LOGW(TAG, "USB audio adapter disconnected; falling back to I2S");
    started_ = false;
    dev_ = nullptr;
    // Close only once no write (or volume change) is using the handle: closing frees the
    // interface and its lock, and a write waiting on that lock spun with interrupts off
    // until the interrupt watchdog reset the board (unplugged while playing, 2026-11-03).
    std::lock_guard<std::mutex> l(mu_);
    uac_host_device_close(h);
  } else if (ev == UAC_HOST_DEVICE_EVENT_TRANSFER_ERROR) {
    errors_++;
  }
}

static inline void put_sample(uint8_t* p, int32_t s16, uint8_t subframe) {
  // left-justify the 16-bit sample into the subframe (little endian)
  int32_t v = s16 << (8 * (subframe - 2));
  for (uint8_t b = 0; b < subframe; b++) p[b] = uint8_t(v >> (8 * b));
}

size_t UsbAudioSink::queued_frames() {
  // try_lock: a write holds mu_ while it waits for ring space; the ring level is then the
  // full ring, and blocking the caller (the player's position math) would stall it.
  std::unique_lock<std::mutex> l(mu_, std::try_to_lock);
  const size_t fbytes = size_t(channels_) * subframe_;
  const size_t inflight = size_t(CONFIG_UAC_NUM_ISOC_URBS) * CONFIG_UAC_NUM_PACKETS_PER_URB * 441 / 10;  // 1 ms packets
  if (!l.owns_lock()) return last_queued_ + inflight;
  auto h = dev_.load();
  if (!h || !started_ || !fbytes || !rate_) return inflight;
  size_t q = uac_host_tx_queued_bytes(h) / fbytes;   // device-rate frames
  last_queued_ = size_t(uint64_t(q) * 44100 / rate_);  // as 44.1 kHz frames
  return last_queued_ + inflight;
}

bool UsbAudioSink::write(const int16_t* in, size_t n) {
  std::lock_guard<std::mutex> l(mu_);  // held for the whole write: on_device_event closes under it
  auto h = dev_.load();
  if (!h || !started_) return false;
  if (!out_) out_ = static_cast<uint8_t*>(heap_caps_malloc(kOutBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!out_) return false;
  const size_t fbytes = size_t(channels_) * subframe_;
  const size_t max_out = kOutBytes / fbytes;
  const double step = 44100.0 / double(rate_);  // input frames per output frame
  size_t o = 0;
  auto flush_out = [&]() -> bool {
    if (!o) return true;
    if (!started_) {  // unplugged mid-write: stop at once (the close waits for this write)
      o = 0;
      return false;
    }
    int64_t t0 = esp_timer_get_time();
    esp_err_t e = uac_host_device_write(h, out_, uint32_t(o * fbytes), pdMS_TO_TICKS(500));
    int64_t dt = esp_timer_get_time() - t0;
    bytes_ += o * fbytes;
    if (dt > 50000 && slow_logs_ < 20) {
      slow_logs_++;
      ESP_LOGW(TAG, "slow write: %u bytes took %lld ms (%s)", (unsigned)(o * fbytes), dt / 1000, esp_err_to_name(e));
    }
    o = 0;
    if (e != ESP_OK) {
      errors_++;
      return false;
    }
    return true;
  };
  if (rate_ == 44100) {
    for (size_t i = 0; i < n; i++) {
      uint8_t* p = out_ + o * fbytes;
      if (channels_ == 1) {
        put_sample(p, (int32_t(in[2 * i]) + in[2 * i + 1]) / 2, subframe_);
      } else {
        put_sample(p, in[2 * i], subframe_);
        put_sample(p + subframe_, in[2 * i + 1], subframe_);
      }
      if (++o == max_out && !flush_out()) return started_;
    }
    flush_out();
    return true;
  }
  // linear interpolation between last_ (frame -1) and the current input frames
  double pos = phase_;
  while (pos < double(n)) {
    size_t i0 = size_t(pos);
    double fr = pos - double(i0);
    int32_t l0, r0, l1, r1;
    if (i0 == 0) {
      // interpolate from last_ to in[0] when pos in [0,1): treat last_ as index -1
      l0 = last_[0];
      r0 = last_[1];
      l1 = in[0];
      r1 = in[1];
    } else {
      l0 = in[2 * (i0 - 1)];
      r0 = in[2 * (i0 - 1) + 1];
      l1 = in[2 * i0];
      r1 = in[2 * i0 + 1];
    }
    int32_t L = int32_t(l0 + (l1 - l0) * fr), R = int32_t(r0 + (r1 - r0) * fr);
    uint8_t* p = out_ + o * fbytes;
    if (channels_ == 1) {
      put_sample(p, (L + R) / 2, subframe_);
    } else {
      put_sample(p, L, subframe_);
      put_sample(p + subframe_, R, subframe_);
    }
    if (++o == max_out && !flush_out()) return started_;
    pos += step;
  }
  phase_ = pos - double(n);
  last_[0] = in[2 * (n - 1)];
  last_[1] = in[2 * (n - 1) + 1];
  flush_out();
  return true;
}

#if LISTENICA_DIAG
void UsbAudioSink::test_tone(uint32_t ms) {
  static int16_t buf[441 * 2];
  uint32_t n = ms / 10;
  double ph = 0;
  int64_t t0 = esp_timer_get_time();
  uint64_t b0 = bytes_;
  for (uint32_t k = 0; k < n && connected(); k++) {
    for (int i = 0; i < 441; i++) {
      int16_t v = int16_t(6000 * sin(ph));
      ph += 2 * M_PI * 1000.0 / 44100.0;
      buf[2 * i] = buf[2 * i + 1] = v;
    }
    write(buf, 441);
  }
  int64_t dt = esp_timer_get_time() - t0;
  ESP_LOGI(TAG, "tone: %u ms of audio written in %lld ms (%llu bytes)", (unsigned)ms, dt / 1000,
           (unsigned long long)(bytes_ - b0));
}
#endif
