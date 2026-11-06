#pragma once
// BOOT button (GPIO0, active low) gestures:
//   1 / 2 / 3+ presses       pause-resume (idle: take over, else play the preset) / next / previous
//                            (previous: back to the song played before if pressed in its first 3 s, else restart)
//   press + hold             volume down while held (5%..100%, linear)
//   double-press + hold      volume up while held
//   3 presses + hold 1 s     Sinfónica (LED: aubergine): on the playing Tónica, call the idle ones on
//                            the LAN to follow it (again: dissolve the group); on an idle one, follow
//                            the playing one; on a follower, leave. A follower's other gestures
//                            control the group's music on the leader.
//   hold 1 s                 Like the current track          (LED: red<->pink)
//   hold 4 s                 reopen the pairing window       (LED: green)
//   hold 7 s                 return to the owner             (LED: amber)
//   hold 10 s                Wi-Fi setup                     (LED: cyan)
//   hold > 13 s              cancel                          (LED: off)
// Presses count when < 500 ms; presses belong together when the gap is < 500 ms.
// Hold actions fire on release; the LED previews which one you will get.
// Never hold BOOT while powering on or resetting: that enters flashing mode.
#include <cstdint>
#include <functional>

enum class ActionResult { Done, Pending, Fail };

struct ButtonActions {
  std::function<ActionResult()> toggle_play;  // Pending: wait for audio (is_playing)
  std::function<bool()> is_playing;
  std::function<bool()> next;
  std::function<bool(uint32_t first_press_ago_ms)> previous;
  std::function<int()> volume_get;                // 0..65535, -1 = no session
  std::function<void(uint16_t)> volume_set_now;   // hardware volume, immediately
  std::function<void(uint16_t)> volume_publish;   // tell Spotify (throttled by the caller of this module)
  std::function<ActionResult()> like;             // Pending: poll like_state()
  std::function<int()> like_state;                // 0 pending, 1 ok, -1 failed
  std::function<bool()> pairing_window, return_to_owner, wifi_setup;
  std::function<bool()> whats_playing;  // 4 presses: speak the current track (audible notifications)
  std::function<ActionResult()> group;  // 3 presses + hold: Sinfónica; Pending: poll group_state()
  std::function<int()> group_state;     // 0 pending, 1 ok, -1 failed
};

void button_start(ButtonActions actions);
