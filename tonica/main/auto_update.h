#pragma once
// Automatic updates from GitHub Releases, signed.
//
// The latest release is found with esp_ghota (vendored, tonica/components/esp_ghota): its firmware
// asset "tonica-<v>-tonica_esp32.bin" and the detached signature "<same>.sig" (ECDSA P-256 over the
// image bytes, DER), made by tools/release.ps1 with a key that never leaves the release PC
// (secrets/ota_signing_key.pem). The matching public key is built in (ota_pubkey.pem).
//
// The install is Tónica's own: esp_https_ota streams the image into the next OTA slot, the bytes
// written are hashed and the signature checked, and only then does the boot partition change. A
// bad or missing signature discards the download. The new image boots pending verification and
// rolls back if it doesn't come up (ota_guard in net_services.cpp).
//
// Firmware rules kept: checks and installs only while idle (nothing playing, no notification, not
// in a Sinfónica group, no setup hotspot), aborted if playback starts mid-download; flash I/O runs
// on a short-lived internal-RAM-stack task; TLS memory is in PSRAM (CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC).
// A -dev build never updates by itself (a release would replace a working build); the console
// 'update' command still checks, and 'update install' installs, for testing.
#include <functional>

struct AutoUpdateHooks {
  std::function<bool()> enabled;  // the setting (Settings::eff_auto_update)
  std::function<bool()> idle;     // safe to download and write flash now
};

// Starts the scheduler: a first check some minutes after boot, then about once a day.
void auto_update_start(AutoUpdateHooks hooks);

// Console 'update [install]': runs one check (and the install, if asked) on a task of its own;
// the result is printed as "UPDATE <message>".
void auto_update_console(const char* arg);
