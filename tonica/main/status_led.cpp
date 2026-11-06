#include "status_led.h"
#include <atomic>
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "led_strip.h"
#include "nvs.h"
#include <cmath>
#include <cstdio>
#include <string>
#include <cstring>

static const char* TAG = "led";
static led_strip_handle_t s_strip = nullptr;
static BootIssue s_issue = BootIssue::None;
static std::atomic<bool> s_ota{false};
static std::atomic<bool> s_grace{false};
static std::atomic<Waiting> s_wait{Waiting::None};
static std::atomic<LedFx> s_fx{LedFx::None};
static std::atomic<uint8_t> s_fx_level{0};
static std::atomic<uint32_t> s_fx_start{0};
static std::atomic<GroupRole> s_role{GroupRole::None};
// The site's aubergine (#2b1a33) as the WS2812 shows it: its green die is far brighter than red and
// blue, so the raw value reads whitish (the user, 2026-11-05). Green nearly off, red under blue.
static constexpr uint8_t kAubR = 20, kAubG = 1, kAubB = 28;
static uint32_t fx_now() { return uint32_t(esp_timer_get_time() / 1000); }

void status_led_fx(LedFx fx, uint8_t level) {
  s_fx_level = level;
  s_fx_start = fx_now();
  s_fx = fx;
}

// 0..1 raised-cosine breath with the given period
static float breath(uint32_t t, uint32_t period) {
  float ph = float(t % period) / float(period);
  return 0.5f - 0.5f * cosf(ph * 6.2831853f);
}

// Renders the current button effect; false = none active (show status codes).
static bool render_fx(uint8_t& r, uint8_t& g, uint8_t& b) {
  LedFx fx = s_fx;
  if (fx == LedFx::None) return false;
  uint32_t t = fx_now() - s_fx_start;
  auto red_pink = [&](uint32_t period, float env) {  // red (90,0,0) <-> pink (90,18,45)
    float k = breath(t, period);
    r = uint8_t(90 * env), g = uint8_t(18 * k * env), b = uint8_t(45 * k * env);
  };
  r = g = b = 0;
  switch (fx) {
    case LedFx::Tick:
      if (t > 90) break;
      r = g = b = 24;
      return true;
    case LedFx::Fail:
      if (t > 3000) break;
      if ((t / 250) % 2 == 0) r = 160;
      return true;
    case LedFx::PendingWhite: {
      if (t > 10000) break;  // safety: never stick
      uint8_t v = uint8_t(4 + 40 * breath(t, 2000));
      r = g = b = v;
      return true;
    }
    case LedFx::FadeWhite:
      if (t > 800) break;
      r = g = b = uint8_t(40 * (800 - t) / 800);
      return true;
    case LedFx::LevelLimit:
      if (t < 300) {
        r = g = b = ((t / 75) % 2 == 0) ? 120 : 0;
        return true;
      }
      s_fx = LedFx::Level;
      [[fallthrough]];
    case LedFx::Level:
      r = g = b = uint8_t(2 + uint32_t(s_fx_level) * 110 / 255);
      return true;
    case LedFx::LikeHold:
      red_pink(3000, 1.0f);
      return true;
    case LedFx::LikePending:
      if (t > 10000) break;
      red_pink(1500, 1.0f);
      return true;
    case LedFx::LikeDone:
      if (t > 3000) break;
      red_pink(500, float(3000 - t) / 3000.0f);
      return true;
    case LedFx::GreenSteady:
      g = 30;
      return true;
    case LedFx::GreenFlash:
      if (t > 600) break;
      if ((t / 150) % 2 == 0) g = 120;
      return true;
    case LedFx::AmberSteady:
      r = 90, g = 35;
      return true;
    case LedFx::AmberFast: {
      if (t > 30000) break;
      float k = breath(t, 500);
      r = uint8_t(10 + 110 * k), g = uint8_t(4 + 40 * k);
      return true;
    }
    case LedFx::CyanSteady:
      g = 60, b = 60;
      return true;
    case LedFx::CyanBlink:
      if ((t / 1000) % 2 == 0) g = 40, b = 40;
      return true;
    case LedFx::CyanBreathe: {
      float k = breath(t, 2000);
      g = b = uint8_t(4 + 60 * k);
      return true;
    }
    case LedFx::Blank:
      return true;
    case LedFx::GroupHold:
      r = kAubR * 2, g = kAubG * 2, b = kAubB * 2;
      return true;
    case LedFx::GroupPending: {
      if (t > 10000) break;  // safety: never stick
      float k = 0.2f + 0.8f * breath(t, 1500);
      r = uint8_t(kAubR * k), g = uint8_t(kAubG * k), b = uint8_t(kAubB * k);
      return true;
    }
    case LedFx::TtsPending: {
      if (t > 90000) break;  // safety: never stick
      uint8_t v = uint8_t(2 + 14 * breath(t, 4000));
      r = g = b = v;
      return true;
    }
    case LedFx::TtsFail: {  // N red blinks (300 ms on, 300 ms off), a pause, then again
      uint32_t n = s_fx_level.load();
      if (n == 0) n = 1;
      const uint32_t cycle = n * 600 + 1200;
      if (t > 2 * cycle) break;
      uint32_t tt = t % cycle;
      if (tt < n * 600 && tt % 600 < 300) r = 160;
      return true;
    }
    case LedFx::None:
      break;
  }
  s_fx = LedFx::None;  // timed effect finished
  return false;
}

void status_led_init() {
  led_strip_config_t sc = {};
  sc.strip_gpio_num = 48;
  sc.max_leds = 1;
  sc.led_model = LED_MODEL_WS2812;
  sc.color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB;
  led_strip_rmt_config_t rc = {};
  rc.clk_src = RMT_CLK_SRC_DEFAULT;
  rc.resolution_hz = 10 * 1000 * 1000;
  if (led_strip_new_rmt_device(&sc, &rc, &s_strip) != ESP_OK) {
    ESP_LOGW(TAG, "WS2812 on GPIO48 unavailable");
    s_strip = nullptr;
  }
}

void status_led_set(uint8_t r, uint8_t g, uint8_t b) {
  if (!s_strip) return;
  if (!r && !g && !b) {
    led_strip_clear(s_strip);
  } else {
    led_strip_set_pixel(s_strip, 0, r, g, b);
    led_strip_refresh(s_strip);
  }
}

BootIssue boot_issue_detect() {
  // An OTA image that failed verification was rolled back by the bootloader
  // (reported once per rolled-back image: its SHA-256 is remembered in NVS)
  if (const esp_partition_t* bad = esp_ota_get_last_invalid_partition()) {
    uint8_t sha[32];
    if (esp_partition_get_sha256(bad, sha) == ESP_OK) {
      nvs_handle_t h;
      if (nvs_open("soloist", NVS_READWRITE, &h) == ESP_OK) {
        uint8_t seen[32] = {0};
        size_t n = sizeof seen;
        bool new_rollback = nvs_get_blob(h, "rb_sha", seen, &n) != ESP_OK || memcmp(seen, sha, 32) != 0;
        if (new_rollback) {
          nvs_set_blob(h, "rb_sha", sha, 32);
          nvs_commit(h);
        }
        nvs_close(h);
        if (new_rollback) return BootIssue::OtaRollback;
      }
    }
  }
  switch (esp_reset_reason()) {
    case ESP_RST_PANIC:
      return BootIssue::Crash;
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT:
      return BootIssue::Watchdog;
    case ESP_RST_BROWNOUT:
      return BootIssue::Brownout;
    default:
      return BootIssue::None;
  }
}

const char* boot_issue_name(BootIssue b) {
  switch (b) {
    case BootIssue::Crash: return "crash";
    case BootIssue::Watchdog: return "watchdog";
    case BootIssue::Brownout: return "brownout";
    case BootIssue::OtaRollback: return "ota_rollback";
    default: return "ok";
  }
}

// ------------------------------------------------------------ sequencer
struct Code {
  int count;
  bool fault;  // long + full red, else short + dim blue
  const char* label;
};

static std::atomic<bool> s_wifi{false}, s_spotify{false};
static char s_summary[96] = "booting";
static std::atomic<const char*> s_play_err{nullptr};
static std::atomic<uint32_t> s_play_err_at{0};
static constexpr uint32_t kTickMs = 20, kGroupEveryMs = 4000, kStickyMs = 5 * 60 * 1000;

static int collect(Code* out, uint32_t ms, bool healthy_once) {
  int n = 0;
  if (s_issue == BootIssue::Crash || s_issue == BootIssue::Watchdog || s_issue == BootIssue::Brownout) {
    if (ms < kStickyMs) out[n++] = {2, true, "E2 previous boot crashed"};
  }
  if (s_issue == BootIssue::OtaRollback && ms < kStickyMs) out[n++] = {3, true, "E3 OTA rollback"};
  if (const char* pe = s_play_err.load()) {
    if (ms - s_play_err_at.load() < 10 * 60 * 1000) out[n++] = {4, true, pe};
    else s_play_err = nullptr;
  }
  if (!healthy_once) {
    out[n++] = {1, false, "booting"};
  } else if (!s_wifi) {
    out[n++] = {2, false, "no wifi"};
  } else if (!s_spotify) {
    out[n++] = {3, false, "spotify session down"};
  }
  return n;
}

static void boot_led_task(void*) {
  const TickType_t t0 = xTaskGetTickCount();
  auto now = [&] { return uint32_t((xTaskGetTickCount() - t0) * portTICK_PERIOD_MS); };
  bool healthy_once = false, e1_logged = false;
  int rot = 0;
  // group playback state
  Code cur = {0, false, ""};
  int pulse = 0;
  uint32_t phase_until = 0, next_group = 0;
  bool on = false;
  uint8_t lr = 1, lg = 1, lb = 1;  // last painted (avoid redundant RMT traffic)
  auto paint = [&](uint8_t r, uint8_t g, uint8_t b) {
    if (r == lr && g == lg && b == lb) return;
    lr = r, lg = g, lb = b;
    status_led_set(r, g, b);
  };
  for (;;) {
    uint32_t ms = now();
    if (s_wifi && s_spotify) healthy_once = true;
    Code codes[4];
    int n = collect(codes, ms, healthy_once);
    // summary for /status
    {
      std::string s;
      Waiting w = s_wait;
      if (s_ota) s = "updating";
      else if (w == Waiting::Wifi) s = "waiting for Wi-Fi setup";
      else if (w == Waiting::Account) s = "waiting for a Spotify account";
      else if (s_grace) s = "pairing window";
      else if (!healthy_once && ms >= 60000) s = "E1 boot failed";
      for (int i = 0; i < n; i++) {
        if (!s.empty()) s += " + ";
        s += codes[i].label;
      }
      snprintf(s_summary, sizeof s_summary, "%s", s.empty() ? "ok" : s.c_str());
    }
    uint8_t fr, fg, fb;
    if (!s_ota && render_fx(fr, fg, fb)) {  // button feedback
      paint(fr, fg, fb);
      cur.count = 0;
    } else if (s_ota) {  // breathing blue, 2 s period
      float ph = float(ms % 2000) / 2000.0f;
      float k = 0.5f - 0.5f * cosf(ph * 6.2831853f);
      paint(0, 0, uint8_t(4 + 200 * k * k));
      cur.count = 0;
    } else if (s_wait == Waiting::Wifi) {  // setup hotspot open: cyan, 1 s on / 1 s off
      paint(0, (ms / 1000) % 2 == 0 ? 40 : 0, (ms / 1000) % 2 == 0 ? 40 : 0);
      cur.count = 0;
    } else if (s_wait == Waiting::Account) {  // waiting to be picked in Spotify: the pairing breath
      float ph = float(ms % 4000) / 4000.0f;
      float k = 0.5f - 0.5f * cosf(ph * 6.2831853f);
      paint(0, uint8_t(1 + 40 * k * k), 0);
      cur.count = 0;
    } else if (!healthy_once && ms >= 60000) {  // E1: steady full red
      if (!e1_logged) {
        ESP_LOGE(TAG, "E1 boot failed: not healthy after 60 s");
        e1_logged = true;
      }
      paint(255, 0, 0);
      cur.count = 0;
    } else if (s_grace && cur.count == 0 && n == 0) {  // pairing window: soft green, 4 s period, dim
      float ph = float(ms % 4000) / 4000.0f;
      float k = 0.5f - 0.5f * cosf(ph * 6.2831853f);
      paint(0, uint8_t(1 + 40 * k * k), 0);
    } else if (s_role != GroupRole::None && cur.count == 0 && n == 0) {  // Sinfónica role
      bool lit = s_role == GroupRole::Leader || ms % 10000 < 300;  // follower: one blink every 10 s
      if (lit) paint(kAubR, kAubG, kAubB);
      else paint(0, 0, 0);
    } else if (cur.count == 0) {  // between groups
      paint(0, 0, 0);
      if (ms >= next_group && n > 0) {
        cur = codes[rot++ % n];
        pulse = 0;
        on = false;
        phase_until = ms;
      }
    } else if (ms >= phase_until) {  // advance the current group
      if (!on) {
        if (pulse >= cur.count) {
          cur.count = 0;
          next_group = ms + kGroupEveryMs;
          paint(0, 0, 0);
        } else {
          on = true;
          if (cur.fault) paint(255, 0, 0);
          else paint(0, 0, 12);  // dim blue (WS2812 is visible even at low values)
          phase_until = ms + (cur.fault ? 600 : 120);
        }
      } else {
        on = false;
        pulse++;
        paint(0, 0, 0);
        phase_until = ms + (cur.fault ? 300 : 250);
      }
    }
    vTaskDelay(pdMS_TO_TICKS(kTickMs));
  }
}

void boot_led_start(BootIssue issue) {
  s_issue = issue;
  if (issue != BootIssue::None) ESP_LOGW(TAG, "previous boot problem: %s", boot_issue_name(issue));
  // PSRAM stack: this task never touches flash (boot_issue_detect runs in main)
  // Priority 9: button feedback must render while the network tasks (5) are busy at a track start
  xTaskCreatePinnedToCoreWithCaps(boot_led_task, "status_led", 3072, nullptr, 9, nullptr, 0, MALLOC_CAP_SPIRAM);
}

void status_led_conditions(bool wifi_up, bool spotify_up) {
  s_wifi = wifi_up;
  s_spotify = spotify_up;
}
void status_led_ota(bool active) { s_ota = active; }
void status_led_grace(bool active) { s_grace = active; }
void status_led_group(GroupRole role) { s_role = role; }
void status_led_waiting(Waiting w) { s_wait = w; }
void status_led_playback_error(const char* label) {
  s_play_err_at = uint32_t(xTaskGetTickCount() * portTICK_PERIOD_MS);
  s_play_err = label;
}
const char* status_led_summary() { return s_summary; }
