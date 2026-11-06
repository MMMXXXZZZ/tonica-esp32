#pragma once
// Audible notifications: short spoken messages mixed over the music (or alone when
// nothing plays), rendered by Gemini TTS with the owner's API key.
//
// Live messages (track names, who took over) are rendered on demand (~5 s).
// Fixed messages are rendered once per voice/style and kept in flash, so they also
// work offline ("Wi-Fi lost"). The flash area is the unused space after the two app
// slots (declared as the "voice" partition in partitions.csv; boards whose partition
// table predates that use the same address directly). Flash is written only while
// nothing plays: erases stall the audio path.
//
// Everything here is off while no API key is set.
#include <cstdint>
#include <functional>
#include <string>

namespace listenica {
class Player;
}

enum class Phrase : int {
  Liked,
  Pair,
  Setup,
  AutoplayAlbum,
  AutoplayPlaylist,
  AutoplayOther,
  WifiLost,
  Online,
  LoginExpired,
  Guest,
  Refused,
  Count
};

struct NotifyConfig {
  std::string key, voice, style;
  std::string device_name, setup_ssid;  // used in the fixed phrases
};

struct NotifyHooks {
  std::function<bool()> online;          // Wi-Fi up (live TTS possible)
  std::function<bool()> may_write_flash;  // nothing plays: flash erases are harmless
  // Sinfonica: speak on one follower only (24 kHz mono), for a gesture made on that follower.
  std::function<void(const std::string& device_id, const std::int16_t* pcm, size_t n, uint32_t rate)> to_follower;
};

// Where a notification is heard (Sinfonica): kHere = this speaker only (the default; a leader's own
// button feedback, setup and Wi-Fi messages), kEveryone = this speaker and its followers (what the
// whole group should hear), or a follower's device id = that follower only.
constexpr const char* kHere = "";
constexpr const char* kEveryone = "*";

void notify_start(listenica::Player* player, NotifyHooks hooks);
void notify_configure(const NotifyConfig& c);  // at start-up and after a save; re-renders changed phrases
bool notify_enabled();

void notify_phrase(Phrase p, const std::string& where = kHere);  // stored phrase (silently skipped until rendered)
// Wi-Fi went down/up. Safe on sys_evt's small stack (sets an atomic): the worker says
// "Wi-Fi lost" if music was playing, then "Back online" once it returns.
void notify_wifi(bool up);
void notify_say(const std::string& text, const std::string& where = kHere);  // live
// "<prefix><title>, by <artists>." once the current track plays (live).
void notify_track(const std::string& prefix, const std::string& where = kHere);
bool notify_busy();  // queued, rendering or speaking

std::string notify_default_style();
std::string notify_default_voice();
std::string notify_status();  // for /status: phrases=<n>/<total> etc.

// Setup page helpers (blocking; run on the HTTP server task).
// Voices: {"ok":true,"voices":[[id,name,lang,gender,accent],...]} or {"ok":false,"message":...}.
std::string notify_voices_json(const std::string& key);
// Speaks a sample with these settings now; {"ok":true} or {"ok":false,"message":...}.
std::string notify_preview_json(const std::string& key, const std::string& voice, const std::string& style);
