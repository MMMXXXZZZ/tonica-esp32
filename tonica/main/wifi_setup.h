#pragma once
// Wi-Fi setup hotspot: an open access point "Tónica-Setup-xxxx" with a captive
// portal (every DNS name answers 192.168.4.1, phones pop the page up). The page
// lists nearby networks; a submitted network is tested before it is saved, so a
// typo leaves the speaker on its current network.
//
// Started by holding the button 10 s (5-minute timeout), at boot when no Wi-Fi is
// saved, or at boot when the saved network isn't found within 30 s (no timeout;
// it closes again if that network comes back). LED: cyan blink waiting, cyan breathing while a
// phone is connected, green flash on success (then reboot), red blinks on timeout.
#include <cstdint>
#include <functional>
#include <string>
#include "esp_http_server.h"

// The page (setup_page.html, embedded) talks JSON to:
//   GET  /setup/state      device, current network, settings   (state_json)
//   GET  /setup/playlists  the account's playlists              (playlists_json)
//   POST /setup/save       everything the page shows            (save)
//   POST /setup/voices     {"key"} -> Gemini voice list          (voices_json)
//   POST /setup/preview    {"key","voice","style"} -> speaks      (preview_json)
// A successful save closes the hotspot; it restarts only if the change needs it.
struct WifiSetupHooks {
  std::function<std::string()> state_json;                           // object, without "networks"
  std::function<std::string(int& http_status)> playlists_json;       // {"playlists":[...]} or {"error":...}
  // Apply a save request; returns the JSON reply. close: end the hotspot; reboot: restart after.
  std::function<std::string(const std::string& body, bool& close, bool& reboot)> save;
  std::function<void()> reboot;
  std::function<std::string(const std::string& body)> voices_json, preview_json;
};

// timeout_s = 0: stay up until a network is configured. False if already running.
bool wifi_setup_start(httpd_handle_t srv, const std::string& device_id, uint32_t timeout_s, WifiSetupHooks hooks);
// Atomic read: safe in the Wi-Fi event handler. While true, the station doesn't reconnect on its
// own (connect attempts scan every channel and disturb the hotspot); stop() resumes it.
bool wifi_setup_active();
int wifi_setup_clients();  // phones joined to the hotspot
void wifi_setup_stop();    // close it without a save (the saved network came back)
