#pragma once
#include <string>
#include "listenica/ap_session.h"
#include "sdkconfig.h"

// Persistent settings in NVS namespace "soloist" (the project's former name, kept so
// saved Wi-Fi/login/owner/volume survive the rename), editable over the serial
// console (see tools/provision.py):
//   wifi <ssid> <password>
//   creds <username> <auth_type> <auth_data_hex>
//   name <device name...>
//   otatoken <hex>            enable network OTA (POST /ota with X-Token)
//   status | reboot | forget
struct Settings {
  std::string ssid, password, name;
  int txpower_dbm = 20;  // Wi-Fi max TX power (dBm); lower it (txpower cmd) when bus-powered from a PC
  int volume = -1;
  std::string ota_token;  // empty = OTA disabled  // last-used Connect volume (0..65535), -1 = never set  // Wi-Fi max TX power; lower = smaller supply current spikes
  listenica::Credentials creds;

  // Runtime settings (setup page). -1 / "" = not set here: the build default from
  // tonica/settings.conf applies (see the effective getters below).
  int bitrate = -1;         // 96 | 160 | 320
  int takeover = -1;        // 0 open, 1 owner priority, 2 owner only
  int grace_sec = -1, guest_idle_min = -1, guest_max_min = -1, watchdog_min = -1, resume_window_min = -1;
  std::string preset_uri;   // "" = build default; "off"; "liked"; or a spotify: URI
  std::string preset_mode;  // default | shuffle | newest | radio | resume
  std::string resume_ctx, resume_track;  // where the preset last stopped (Resume mode)
  int resume_pos_ms = 0;
  std::string preset_name;                // shown/spoken name of the preset playlist ("" = unknown)
  std::string tts_key, tts_voice, tts_style;  // audible notifications (Gemini TTS); empty key = off

  int eff_bitrate() const { return bitrate > 0 ? bitrate : CONFIG_TONICA_BITRATE; }
  int eff_takeover() const {
    if (takeover >= 0) return takeover;
#if CONFIG_TONICA_TAKEOVER_OWNER_ONLY
    return 2;
#elif CONFIG_TONICA_TAKEOVER_OWNER_PRIORITY
    return 1;
#else
    return 0;
#endif
  }
  int eff_grace_sec() const { return grace_sec >= 0 ? grace_sec : CONFIG_TONICA_GRACE_SEC; }
  int eff_guest_idle_min() const { return guest_idle_min >= 0 ? guest_idle_min : CONFIG_TONICA_GUEST_IDLE_MIN; }
  int eff_guest_max_min() const { return guest_max_min >= 0 ? guest_max_min : CONFIG_TONICA_GUEST_MAX_MIN; }
  int eff_resume_window_min() const { return resume_window_min >= 0 ? resume_window_min : CONFIG_TONICA_RESUME_WINDOW_MIN; }
  int eff_watchdog_min() const { return watchdog_min >= 0 ? watchdog_min : CONFIG_TONICA_WATCHDOG_MIN; }
  std::string eff_preset_uri() const {
    std::string u = preset_uri.empty() ? std::string(CONFIG_TONICA_PRESET) : preset_uri;
    return u == "off" ? std::string() : u;
  }
  std::string eff_preset_mode() const { return preset_mode.empty() ? std::string("default") : preset_mode; }

  bool has_wifi() const { return !ssid.empty(); }
  bool has_creds() const { return !creds.username.empty() && !creds.auth_data.empty(); }
};

void settings_load(Settings& s);
void settings_save_creds(const listenica::Credentials& c);
void settings_save_volume(uint16_t v);
void settings_save_wifi(const std::string& ssid, const std::string& password);
void settings_save_name(const std::string& name);
void settings_save_runtime(const Settings& s);  // bitrate, takeover, timeouts, preset
void settings_save_resume(const std::string& ctx, const std::string& track, int pos_ms);
// Spotify endpoints that worked last time ("ap,ap|dealer|spclient"), so boot can skip apresolve.
int64_t settings_load_last_time();          // unix seconds saved earlier (0 = none)
void settings_save_last_time(int64_t t);    // flash write: internal-RAM stack, nothing playing
std::string settings_load_endpoints();
void settings_save_endpoints(const std::string& packed);  // flash write: internal-RAM stack, nothing playing
void console_start(Settings* s, void (*on_status)());
void console_set_tone_handler(void (*fn)(uint32_t ms));
// Sinfonica prototype: 'follow <leader ip>' / 'follow off'
void console_set_follow_handler(void (*fn)(const char* arg));
// The setup page's actions over the serial console, for the web installer (same code as the
// hotspot page). Replies are one line each: "SETUP <state|save|playlists|preview> <json>".
//   setupstate | setup <json> | setupplaylists | setuppreview <json>
struct WifiSetupHooks;
void console_set_setup_hooks(const WifiSetupHooks& hooks);
