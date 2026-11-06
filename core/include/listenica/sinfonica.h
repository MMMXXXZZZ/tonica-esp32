// Sinfónica membership (docs/sinfonica-study.md): one per speaker (a Tónica, or the Listenica host).
// Every speaker runs a GroupServer, so it can lead; it can also follow another speaker, and the
// "group" gesture (a Tónica's 3 presses + hold, the host's `group` command) forms, joins, leaves
// or dissolves a group. A follower's other gestures are sent to the leader as Control messages.
// The platform supplies discovery (mDNS), its own session's state and the sink.
#pragma once
#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <vector>
#include "listenica/audio_sink.h"
#include "listenica/group.h"

namespace listenica {

class Player;

class Sinfonica {
 public:
  struct Config {
    std::string device_id;      // tells followers apart (targeted speech, the "unstable" gate)
    int64_t follow_offset_us = 0;  // output delay the sink can't report (the Windows audio path)
    int follow_core = -1, follow_prio = 14;  // the follower's thread (Tónica: core 1, beside audio_out)
  };
  struct Hooks {
    std::function<bool()> playing;                   // this speaker's own session is playing
    std::function<void()> stop_local;                // give it up (before following)
    std::function<std::vector<std::string>()> discover;  // the other speakers' IPv4 addresses
    // a follower's gesture for this (leading) speaker to run: "toggle", "next", "prev <ms>",
    // "vol <0..65535>", "like", "whats"; `from` is the follower's device id
    std::function<void(const std::string& from, const std::string& command)> control;
    std::function<void()> role_changed;              // leading/following/alone changed (the LED)
  };
  enum class Role { Alone, Leader, Follower };

  Sinfonica(Player* player, AudioSink* sink, Config cfg, Hooks hooks);
  bool start();
  Role role() const;
  int followers() const { return server_.clients(); }
  bool following() const { return !follow_stop_; }
  std::string leader() const;
  bool follow(const std::string& leader_ip);  // false: already following
  void leave() { follow_stop_ = true; }
  // follower side: send a gesture to the leader; the leader's volume (-1 unknown) and play state
  bool control(const std::string& command);
  int leader_volume();
  bool leader_playing();
  // The group gesture: 1 done, 0 pending (poll gesture_state()), -1 nothing to do.
  int gesture();
  int gesture_state() const { return gesture_state_; }  // 0 pending, 1 ok, -1 failed
  // Leader side: a spoken notification for one follower only (its device id), heard ~0.3 s from now.
  void speak_to(const std::string& device_id, const int16_t* pcm, size_t n, uint32_t rate);
  GroupServer& server() { return server_; }

 private:
  Player* player_;
  AudioSink* sink_;
  Config cfg_;
  Hooks hooks_;
  GroupServer server_;
  std::atomic<bool> follow_stop_{true};
  mutable std::mutex mu_;
  std::string leader_;
  GroupClient* gc_ = nullptr;  // while following (mu_)
  std::atomic<int> gesture_state_{0};
  std::atomic<uint32_t> voice_id_{0x80000000u};  // apart from the player's clip ids
  static void follower_entry(void* self);
  void follower_run();
  static void gesture_entry(void* self);
  void gesture_run();
};

}  // namespace listenica
