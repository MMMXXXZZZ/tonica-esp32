#pragma once
#include <functional>
#include <string>

// Small LAN services on port 80 (plus mDNS "<hostname>.local"):
//   GET  /status          one-line device status (same as the serial STATUS line)
//   GET  /log             recent log output (ring buffer), replaces the serial cable
//   POST /ota             firmware image upload  (header X-Token: <ota token>)
//   POST /reboot          (header X-Token)
// OTA is disabled unless a token has been provisioned (serial: `otatoken <hex>`).
void log_capture_start();  // mirror ESP_LOG output into a RAM ring (call early)
// device_id: the board's hashed id (hex). OTA requests must carry a matching
// X-Device-Id header (>= 6 hex chars prefix) so the wrong board can't be flashed.
void net_services_start(const std::string& hostname, const std::string& ota_token, const std::string& device_id,
                        const std::string& connect_name, std::function<std::string()> status_fn);
// Mark the running image good (cancels bootloader rollback). Call once the
// firmware has proven itself (Wi-Fi + Spotify login).
#include "esp_http_server.h"
httpd_handle_t net_services_server();  // for registering more handlers (zeroconf)
// Called when an OTA upload starts (before any flash write): flash writes stall the
// audio path, so playback is paused instead of stuttering.
void net_services_on_ota_start(void (*fn)());
void ota_mark_valid();
void ota_guard_start();  // reboot (-> rollback) if the image isn't marked valid in 3 min
