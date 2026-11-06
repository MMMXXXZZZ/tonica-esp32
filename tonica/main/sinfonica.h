#pragma once
// Sinfónica on Tónica (docs/sinfonica-study.md): a thin layer over listenica::Sinfonica with the
// board's mDNS discovery (_listenica._tcp) and LED colour.
#include <functional>
#include <string>
#include "button.h"
#include "listenica/audio_sink.h"
#include "listenica/player.h"

struct SinfonicaHooks {
  std::function<bool()> playing;     // this board's own session is playing
  std::function<void()> stop_local;  // give up this board's own session (before following)
  // run a follower's gesture here: "toggle", "next", "prev <ms>", "vol <v>", "like", "whats"
  std::function<void(const std::string& from, const std::string& command)> control;
};

void sinfonica_start(listenica::Player* player, listenica::AudioSink* sink, const std::string& device_id, SinfonicaHooks hooks);
int sinfonica_followers();  // speakers following this one
bool sinfonica_following();
bool sinfonica_follow(const std::string& leader_ip);  // false: already following
void sinfonica_leave();
// Follower side: forward a button command to the leader; the leader's volume (-1 unknown) and play state.
bool sinfonica_control(const std::string& command);
int sinfonica_leader_volume();
bool sinfonica_leader_playing();
// The 3 presses + hold gesture (see button.h). Pending: poll sinfonica_button_state().
ActionResult sinfonica_button();
int sinfonica_button_state();
// Leader: a spoken notification for one follower (its device id).
void sinfonica_speak_to(const std::string& device_id, const int16_t* pcm, size_t n, uint32_t rate);
void sinfonica_update_led();  // the role colour (also called on every role change)
