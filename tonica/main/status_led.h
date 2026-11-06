#pragma once
#include <cstdint>

// Built-in WS2812 on GPIO48 (YD-ESP32-S3; the RGB solder jumper must be bridged).
//
// Grammar (same logic as ~/.moisture): pulse LENGTH = severity, COUNT = which
// code; all active codes CYCLE, one group per 4 s tick, so no code masks another;
// informational pulses are dim, faults full brightness; a group always completes.
// Colour adds meaning on this RGB LED: red = fault, blue = informational.
//
//   breathing blue      firmware update being received/installed (highest priority)
//   soft green breath   pairing window after boot: another account may claim the speaker (below faults);
//                       also a fresh board waiting for its first Spotify account (no E1 while waiting)
//   cyan blink, 1 s     no Wi-Fi saved: the setup hotspot is open (no E1 while waiting)
//   steady full red     E1 boot failed: not healthy within 60 s
//   long x2 red         E2 previous boot ended in crash / watchdog / brownout (5 min)
//   long x3 red         E3 OTA rollback: the update failed, old image restored (5 min)
//   long x4 red         E4 playback stopped: Spotify refused this account's tracks, or tracks keep failing (10 min)
//   short x1 dim blue   booting
//   short x2 dim blue   no Wi-Fi
//   short x3 dim blue   Wi-Fi up, Spotify session down
//   steady aubergine    Sinfónica leader: other Tónicas follow this one (#2b1a33, the site's colour)
//   aubergine blink/10s Sinfónica follower: plays another Tónica's stream
//   off                 healthy, nothing to report
// The same conditions appear in /status (boot=..., led=...) and the log.
enum class BootIssue { None, Crash, Watchdog, Brownout, OtaRollback };

void status_led_init();
void status_led_set(uint8_t r, uint8_t g, uint8_t b);
BootIssue boot_issue_detect();  // reset reason + once-only rollback check; call after nvs init
const char* boot_issue_name(BootIssue);
void boot_led_start(BootIssue issue);  // starts the LED sequencer task
void status_led_conditions(bool wifi_up, bool spotify_up);  // call periodically
void status_led_ota(bool active);
void status_led_grace(bool active);
// Setup the user still has to do (not a failure): suppresses E1 and shows what is awaited.
enum class Waiting : uint8_t { None, Wifi, Account };
void status_led_waiting(Waiting w);
void status_led_playback_error(const char* label);  // E4 for 10 min (label for /status); nullptr clears
enum class GroupRole : uint8_t { None, Leader, Follower };
void status_led_group(GroupRole role);  // shown when no status code is active

// Button feedback (tonica/main/button.cpp). Overrides the status codes while
// set (OTA breathing still wins); timed effects end by themselves.
enum class LedFx : uint8_t {
  None,          // back to status codes
  Tick,          // one dim white blink (a press registered)
  Fail,          // red blinks for 3 s
  PendingWhite,  // slow white breathing until replaced (e.g. waiting for audio)
  FadeWhite,     // white fading out (done)
  Level,         // steady white, brightness = level (volume ramp)
  LevelLimit,    // quick double flash at the volume limit, then Level
  LikeHold,      // slow red<->pink breathing (holding: release = Like)
  LikePending,   // faster red<->pink (sent, waiting for Spotify)
  LikeDone,      // fastest red<->pink, dimming to 0 over 3 s
  GreenSteady,   // holding: release = pairing window
  GreenFlash,    // someone claimed the speaker in the pairing window
  AmberSteady,   // holding: release = back to the owner
  AmberFast,     // returning to the owner (until the reboot)
  CyanSteady,    // holding: release = Wi-Fi setup
  CyanBlink,     // Wi-Fi setup hotspot waiting for a phone
  CyanBreathe,   // Wi-Fi setup: a phone is connected
  Blank,         // holding past the last action: release cancels
  GroupHold,     // 3 presses + hold: release = Sinfónica (aubergine, brighter)
  GroupPending,  // looking for the other Tónicas: aubergine breathing (until replaced, max 10 s)
  TtsPending,    // a spoken notification is being rendered: slow dim white breathing (until replaced, max 90 s)
  TtsFail,       // it failed: level = red blink count (1 no answer, 2 refused, 3 rate limit,
                 // 4 unusable reply, 5 no memory / too long), shown twice
};
void status_led_fx(LedFx fx, uint8_t level = 0);
const char* status_led_summary();  // e.g. "E2 crash + no wifi", for /status
