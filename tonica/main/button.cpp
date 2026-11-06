#include "button.h"
#include <algorithm>
#include "driver/gpio.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "status_led.h"

static const char* TAG = "button";
static constexpr gpio_num_t kPin = GPIO_NUM_0;  // BOOT
static constexpr uint32_t kPollMs = 10, kClickMaxMs = 500, kGapMs = 500;
static constexpr uint32_t kHoldGroup = 1000, kHoldLike = 1000, kHoldPair = 4000, kHoldOwner = 7000, kHoldWifi = 10000, kHoldCancel = 13000;
static constexpr int kVolMin = 3277, kVolMax = 65535;  // 5% .. 100%
static constexpr int kVolPerSec = 65535 / 4;           // full sweep in ~4 s
static constexpr uint32_t kPublishEveryMs = 250;

static ButtonActions s_act;

static uint32_t ms() { return uint32_t(esp_timer_get_time() / 1000); }
static bool down() { return gpio_get_level(kPin) == 0; }

static void fail(const char* why) {
  ESP_LOGW(TAG, "%s", why);
  status_led_fx(LedFx::Fail);
}

// Wait for an asynchronous result, keeping the pending effect up; true = success.
static bool wait_for(const std::function<int()>& state, uint32_t timeout_ms) {
  for (uint32_t t0 = ms(); ms() - t0 < timeout_ms; vTaskDelay(pdMS_TO_TICKS(50))) {
    int s = state();
    if (s != 0) return s > 0;
  }
  return false;
}

static void clicks(int n, uint32_t first_press_ago_ms) {
  switch (n) {
    case 1: {
      ActionResult r = s_act.toggle_play ? s_act.toggle_play() : ActionResult::Fail;
      if (r == ActionResult::Fail) return fail("1 press: nothing to play or pause");
      if (r == ActionResult::Pending) {
        status_led_fx(LedFx::PendingWhite);
        bool ok = wait_for([] { return s_act.is_playing() ? 1 : 0; }, 8000);
        if (ok) status_led_fx(LedFx::FadeWhite);
        else fail("1 press: playback did not start");
      }
      return;
    }
    case 2:
      if (!s_act.next || !s_act.next()) fail("2 presses: no session to skip in");
      return;
    case 4:
      if (!s_act.whats_playing || !s_act.whats_playing()) fail("4 presses: nothing playing or notifications off");
      return;
    default:  // 3, or 5+: an extra tap or a contact bounce must not swallow "previous"
      if (!s_act.previous || !s_act.previous(first_press_ago_ms)) fail("3 presses: no session to skip in");
      return;
  }
}

// Volume ramp while held; dir -1 = down (press + hold), +1 = up (double-press + hold)
static void ramp(int dir) {
  int v = s_act.volume_get ? s_act.volume_get() : -1;
  if (v < 0) {
    while (down()) vTaskDelay(pdMS_TO_TICKS(kPollMs));
    return fail("volume: no session");
  }
  ESP_LOGI(TAG, "volume %s from %d%%", dir < 0 ? "down" : "up", v * 100 / 65535);
  float fv = float(v);
  uint32_t last = ms(), last_pub = 0;
  bool at_limit = false;
  while (down()) {
    vTaskDelay(pdMS_TO_TICKS(20));
    uint32_t now = ms();
    fv += dir * float(kVolPerSec) * float(now - last) / 1000.0f;
    last = now;
    fv = std::clamp(fv, float(kVolMin), float(kVolMax));
    uint16_t nv = uint16_t(fv);
    bool limit = (nv == kVolMin && dir < 0) || (nv == kVolMax && dir > 0);
    if (limit && !at_limit) status_led_fx(LedFx::LevelLimit, uint8_t(nv >> 8));
    else if (!limit) status_led_fx(LedFx::Level, uint8_t(nv >> 8));
    at_limit = limit;
    s_act.volume_set_now(nv);
    if (now - last_pub >= kPublishEveryMs) {
      s_act.volume_publish(nv);
      last_pub = now;
    }
  }
  uint16_t final_v = uint16_t(fv);
  s_act.volume_publish(final_v);
  ESP_LOGI(TAG, "volume %d%%", final_v * 100 / 65535);
  vTaskDelay(pdMS_TO_TICKS(1000));
  status_led_fx(LedFx::FadeWhite);
}

// 3 presses + hold: Sinfónica, fires on release after kHoldGroup.
static void group_hold(uint32_t pressed_at) {
  bool shown = false;
  uint32_t held = 0;
  while (down()) {
    held = ms() - pressed_at;
    if (held >= kHoldGroup && !shown) {
      status_led_fx(LedFx::GroupHold);
      shown = true;
    }
    vTaskDelay(pdMS_TO_TICKS(kPollMs));
  }
  if (held < kHoldGroup) return status_led_fx(LedFx::None);
  ESP_LOGI(TAG, "3 presses + hold: Sinfonica");
  ActionResult r = s_act.group ? s_act.group() : ActionResult::Fail;
  if (r == ActionResult::Fail) return fail("Sinfonica: nothing to do");
  if (r == ActionResult::Pending) {
    status_led_fx(LedFx::GroupPending);
    if (!wait_for(s_act.group_state, 10000)) return fail("Sinfonica: no other Tonica answered");
  }
  status_led_fx(LedFx::None);  // the role colour takes over
}

// Plain hold: preview the action by LED, fire on release.
static void hold_ladder(uint32_t pressed_at) {
  LedFx shown = LedFx::None;
  uint32_t held = 0;
  while (down()) {
    held = ms() - pressed_at;
    LedFx want = held >= kHoldCancel ? LedFx::Blank
                 : held >= kHoldWifi ? LedFx::CyanSteady
                 : held >= kHoldOwner ? LedFx::AmberSteady
                 : held >= kHoldPair ? LedFx::GreenSteady
                 : held >= kHoldLike ? LedFx::LikeHold
                                     : LedFx::None;
    if (want != shown) {
      if (want != LedFx::None) status_led_fx(want);
      shown = want;
    }
    vTaskDelay(pdMS_TO_TICKS(kPollMs));
  }
  if (held >= kHoldCancel) {
    ESP_LOGI(TAG, "hold %u ms: cancelled", unsigned(held));
    status_led_fx(LedFx::None);
  } else if (held >= kHoldWifi) {
    ESP_LOGI(TAG, "hold: Wi-Fi setup");
    if (!s_act.wifi_setup || !s_act.wifi_setup()) fail("Wi-Fi setup is not available yet");
  } else if (held >= kHoldOwner) {
    ESP_LOGI(TAG, "hold: return to the owner");
    if (s_act.return_to_owner && s_act.return_to_owner()) status_led_fx(LedFx::AmberFast);
    else fail("already the owner's session");
  } else if (held >= kHoldPair) {
    ESP_LOGI(TAG, "hold: pairing window");
    if (s_act.pairing_window && s_act.pairing_window()) status_led_fx(LedFx::None);  // the green breath takes over
    else fail("pairing window unavailable");
  } else if (held >= kHoldLike) {
    ESP_LOGI(TAG, "hold: Like");
    ActionResult r = s_act.like ? s_act.like() : ActionResult::Fail;
    if (r == ActionResult::Fail) return fail("Like failed");
    if (r == ActionResult::Pending) {
      status_led_fx(LedFx::LikePending);
      if (!wait_for(s_act.like_state, 8000)) return fail("Like not confirmed by Spotify");
    }
    status_led_fx(LedFx::LikeDone);
  } else {
    status_led_fx(LedFx::None);  // shorter than any action
  }
}

static void button_task(void*) {
  int count = 0;            // completed short presses in the current group
  uint32_t group_start = 0;  // when the group's first press went down
  uint32_t last_release = 0;
  bool was_down = false;
  uint32_t pressed_at = 0;
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(kPollMs));
    bool d = down();
    uint32_t now = ms();
    if (d && !was_down) {  // press
      pressed_at = now;
      if (count == 0) group_start = now;
      was_down = true;
      continue;
    }
    if (d && was_down && now - pressed_at >= kClickMaxMs) {  // became a hold
      if (count == 1) ramp(-1);       // press + hold: volume down
      else if (count == 2) ramp(+1);  // double-press + hold: volume up
      else if (count == 3) group_hold(pressed_at);  // 3 presses + hold: Sinfonica
      else hold_ladder(pressed_at);
      count = 0;
      was_down = false;
      continue;
    }
    if (!d && was_down) {  // short press released
      was_down = false;
      if (now - pressed_at < 30) continue;  // contact bounce
      count++;
      last_release = now;
      status_led_fx(LedFx::Tick);
      continue;
    }
    if (!d && count > 0 && now - last_release >= kGapMs) {  // group complete
      int n = count;
      count = 0;
      ESP_LOGI(TAG, "%d press%s", n, n == 1 ? "" : "es");
      clicks(n, now - group_start);
    }
  }
}

void button_start(ButtonActions actions) {
  s_act = std::move(actions);
  gpio_config_t c = {};
  c.pin_bit_mask = 1ULL << kPin;
  c.mode = GPIO_MODE_INPUT;
  c.pull_up_en = GPIO_PULLUP_ENABLE;
  gpio_config(&c);
  // Priority 10: above the network tasks (5), which saturate core 0 for a few seconds
  // whenever a track starts; at 3 presses were missed entirely in that window.
  // The task only polls every 10 ms, so it costs them nothing. PSRAM stack: nothing here writes flash.
  xTaskCreatePinnedToCoreWithCaps(button_task, "button", 4096, nullptr, 10, nullptr, 0, MALLOC_CAP_SPIRAM);
}
