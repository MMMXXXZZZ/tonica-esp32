#include "listenica/sinfonica.h"
#include "listenica/platform.h"
#include "listenica/player.h"

static const char* TAG = "sinfonica";

namespace listenica {

Sinfonica::Sinfonica(Player* player, AudioSink* sink, Config cfg, Hooks hooks)
    : player_(player), sink_(sink), cfg_(std::move(cfg)), hooks_(std::move(hooks)) {}

bool Sinfonica::start() {
  GroupServer::Hooks h;
  h.status = [this] {
    GroupStatus st;
    st.playing = hooks_.playing && hooks_.playing();
    st.following = following();
    st.followers = server_.clients();
    return st;
  };
  h.invited = [this](const std::string& ip) { follow(ip); };
  h.control = [this](const std::string& from, const std::string& c) {
    if (hooks_.control) hooks_.control(from, c);
  };
  h.clients_changed = [this] {
    if (hooks_.role_changed) hooks_.role_changed();
  };
  server_.set_hooks(std::move(h));
  if (!server_.start()) return false;
  player_->set_group(&server_);
  return true;
}

Sinfonica::Role Sinfonica::role() const {
  return following() ? Role::Follower : server_.clients() > 0 ? Role::Leader : Role::Alone;
}

std::string Sinfonica::leader() const {
  std::lock_guard<std::mutex> l(mu_);
  return following() ? leader_ : std::string();
}

void Sinfonica::follower_entry(void* self) { static_cast<Sinfonica*>(self)->follower_run(); }

void Sinfonica::follower_run() {
  std::string ip;
  {
    std::lock_guard<std::mutex> l(mu_);
    ip = leader_;
  }
  GroupClient gc;
  if (gc.connect(ip, cfg_.device_id)) {
    {
      std::lock_guard<std::mutex> l(mu_);
      gc_ = &gc;
    }
    SLOGI(TAG, "following %s", ip.c_str());
    GroupFollower f(&gc, sink_, cfg_.follow_offset_us);
    f.run(follow_stop_);
    {
      std::lock_guard<std::mutex> l(mu_);
      gc_ = nullptr;
    }
    gc.close();
  }
  follow_stop_ = true;
  SLOGI(TAG, "no longer following %s", ip.c_str());
  if (hooks_.role_changed) hooks_.role_changed();
}

bool Sinfonica::follow(const std::string& leader_ip) {
  if (!follow_stop_) return false;
  if (hooks_.playing && hooks_.playing() && hooks_.stop_local) {
    hooks_.stop_local();  // both would write to the sink
    for (int i = 0; i < 40 && hooks_.playing(); i++) sleep_ms(50);
  }
  if (server_.clients() > 0) server_.dismiss_followers();  // a leader that joins another group brings no one
  {
    std::lock_guard<std::mutex> l(mu_);
    leader_ = leader_ip;
  }
  follow_stop_ = false;
  if (hooks_.role_changed) hooks_.role_changed();
  if (cfg_.follow_core >= 0)
    spawn_thread_on("follower", 8192, true, cfg_.follow_prio, cfg_.follow_core, &Sinfonica::follower_entry, this);
  else
    spawn_thread("follower", 8192, true, cfg_.follow_prio, &Sinfonica::follower_entry, this);
  return true;
}

bool Sinfonica::control(const std::string& command) {
  std::lock_guard<std::mutex> l(mu_);
  return gc_ && gc_->control(command);
}

int Sinfonica::leader_volume() {
  std::lock_guard<std::mutex> l(mu_);
  if (!gc_) return -1;
  GroupClient::Anchor a = gc_->anchor();
  return a.recv_us ? int(a.volume) : -1;
}

bool Sinfonica::leader_playing() {
  std::lock_guard<std::mutex> l(mu_);
  return gc_ && gc_->anchor().playing;
}

void Sinfonica::speak_to(const std::string& device_id, const int16_t* pcm, size_t n, uint32_t rate) {
  uint32_t id = ++voice_id_ | 0x80000000u;
  server_.voice_clip(id, device_id, pcm, n, rate);
  server_.voice_start(id, device_id, now_us() + 300000);  // the clip (~12 KB/s) arrives well before
}

void Sinfonica::gesture_entry(void* self) { static_cast<Sinfonica*>(self)->gesture_run(); }

void Sinfonica::gesture_run() {
  bool ok = false;
  bool leading = hooks_.playing && hooks_.playing();
  std::vector<std::string> peers = hooks_.discover ? hooks_.discover() : std::vector<std::string>{};
  SLOGI(TAG, "%u other speaker(s) on the network", unsigned(peers.size()));
  for (const std::string& ip : peers) {
    GroupStatus st;
    if (!group_status(ip, st)) continue;
    if (leading) {  // call the idle ones; a speaker playing its own session is left alone
      if (st.following || st.playing) continue;
      if (group_invite(ip)) {
        SLOGI(TAG, "invited %s", ip.c_str());
        ok = true;
      }
    } else if (st.playing && !st.following) {  // join the playing one
      ok = follow(ip);
      break;
    }
  }
  gesture_state_ = ok ? 1 : -1;
}

int Sinfonica::gesture() {
  if (following()) {  // a follower leaves
    SLOGI(TAG, "leaving the group");
    leave();
    return 1;
  }
  if (server_.clients() > 0) {  // the leader dissolves its group
    SLOGI(TAG, "dissolving the group (%d follower(s))", server_.clients());
    server_.dismiss_followers();
    return 1;
  }
  gesture_state_ = 0;
  spawn_thread("sinf_gesture", 6144, true, 4, &Sinfonica::gesture_entry, this);  // mDNS + TCP: no flash I/O
  return 0;
}

}  // namespace listenica
