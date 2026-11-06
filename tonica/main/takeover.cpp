#include "takeover.h"
#include "event_log.h"
#include <cstddef>
#include <cstring>
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_rom_crc.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char* TAG = "takeover";

TakeoverResult takeover_decide(const TakeoverInput& in, int policy) {
  // 1. the owner always wins (and the first account to pair becomes the owner)
  if (in.owner.empty() || in.user == in.owner) return {Takeover::AcceptOwner};
  // 2. pairing window: anyone may claim the speaker and becomes the temporary owner
  if (in.grace_open) return {Takeover::AcceptGuest, true};
  // 3. the temporary owner can always take the speaker back from guests
  if (!in.temp_owner.empty() && in.user == in.temp_owner) return {Takeover::AcceptGuest, true};
  // 4. a guest: the policy protects whoever holds the session as owner or temporary owner
  //    (a plain guest session never blocks another guest)
  if (policy == 2) return {Takeover::Refuse};
  if (policy == 1) {
    bool holder_is_owner_tier = !in.session_guest || !in.temp_owner.empty();
    return {(holder_is_owner_tier && in.holder_attached) ? Takeover::Refuse : Takeover::AcceptGuest};
  }
  return {Takeover::AcceptGuest};
}

const char* takeover_policy_name(int policy) {
  return policy == 2 ? "owner_only" : policy == 1 ? "owner_priority" : "open";
}

// ------------------------------------------------------------ RTC hand-over
namespace {
constexpr uint32_t kGuestMagic = 0x47554553;   // "GUES"
constexpr uint32_t kTempMagic = 0x54454d50;    // "TEMP"
constexpr uint32_t kReasonMagic = 0x52454153;  // "REAS"
struct GuestSlot {
  uint32_t magic, crc;
  char user[96];
  uint32_t auth_type;
  uint32_t len;
  uint8_t data[768];
};
struct TempOwnerSlot {
  uint32_t magic, crc;
  char user[96];
};
struct ReasonSlot {
  uint32_t magic;
  char why[64];
};
RTC_NOINIT_ATTR GuestSlot s_guest;
RTC_NOINIT_ATTR TempOwnerSlot s_temp;
RTC_NOINIT_ATTR ReasonSlot s_reason;
RTC_NOINIT_ATTR uint32_t s_resume_magic;
constexpr uint32_t kResumeMagic = 0x52534d45;  // "RSME"
char s_last_reason[64] = "";
bool s_reason_read = false;

uint32_t crc_of(const void* p, size_t n) { return esp_rom_crc32_le(0, static_cast<const uint8_t*>(p), n); }
uint32_t guest_crc(const GuestSlot& g) {
  return crc_of(&g.user, sizeof g - offsetof(GuestSlot, user));
}
bool soft_reset() { return esp_reset_reason() == ESP_RST_SW; }  // RTC slots only count across our own restarts
}  // namespace

bool guest_boot_set(const listenica::Credentials& c) {
  if (c.username.size() >= sizeof s_guest.user || c.auth_data.size() > sizeof s_guest.data) {
    ESP_LOGW(TAG, "guest login too large for the RTC slot (%u bytes)", unsigned(c.auth_data.size()));
    return false;
  }
  memset(&s_guest, 0, sizeof s_guest);
  memcpy(s_guest.user, c.username.data(), c.username.size());
  s_guest.auth_type = c.auth_type;
  s_guest.len = uint32_t(c.auth_data.size());
  memcpy(s_guest.data, c.auth_data.data(), c.auth_data.size());
  s_guest.crc = guest_crc(s_guest);
  s_guest.magic = kGuestMagic;
  return true;
}

bool guest_boot_take(listenica::Credentials& out) {
  bool ok = s_guest.magic == kGuestMagic && s_guest.len <= sizeof s_guest.data && s_guest.crc == guest_crc(s_guest) &&
            soft_reset();  // a power cycle always returns to the owner
  if (ok) {
    out.username.assign(s_guest.user, strnlen(s_guest.user, sizeof s_guest.user));
    out.auth_type = s_guest.auth_type;
    out.auth_data.assign(s_guest.data, s_guest.data + s_guest.len);
  }
  memset(&s_guest, 0, sizeof s_guest);  // one boot only
  return ok;
}

void temp_owner_set(const std::string& user) {
  memset(&s_temp, 0, sizeof s_temp);
  strncpy(s_temp.user, user.c_str(), sizeof s_temp.user - 1);
  s_temp.crc = crc_of(s_temp.user, sizeof s_temp.user);
  s_temp.magic = kTempMagic;
}

std::string temp_owner_get() {
  if (s_temp.magic != kTempMagic || s_temp.crc != crc_of(s_temp.user, sizeof s_temp.user)) return "";
  return std::string(s_temp.user, strnlen(s_temp.user, sizeof s_temp.user));
}

void temp_owner_clear() { memset(&s_temp, 0, sizeof s_temp); }

void resume_after_reboot_set(bool on) { s_resume_magic = on ? kResumeMagic : 0; }

bool resume_after_reboot_take() {
  bool on = s_resume_magic == kResumeMagic && soft_reset();
  s_resume_magic = 0;
  return on;
}

void reboot_with_reason(const char* why) {
  ESP_LOGW(TAG, "rebooting: %s", why);
  event_log("reboot: %s", why);
  event_log_flush();
  s_reason.magic = kReasonMagic;
  strncpy(s_reason.why, why, sizeof s_reason.why - 1);
  s_reason.why[sizeof s_reason.why - 1] = 0;
  // from a task of its own, so an HTTP reply in flight (zeroconf addUser) still goes out
  xTaskCreate([](void*) {
    vTaskDelay(pdMS_TO_TICKS(800));
    esp_restart();
  }, "reboot", 2048, nullptr, 5, nullptr);
}

const char* last_reboot_reason() {
  if (!s_reason_read) {
    s_reason_read = true;
    if (s_reason.magic == kReasonMagic && soft_reset()) {
      strncpy(s_last_reason, s_reason.why, sizeof s_last_reason - 1);
      s_last_reason[sizeof s_last_reason - 1] = 0;
    }
    s_reason.magic = 0;
  }
  return s_last_reason;
}
