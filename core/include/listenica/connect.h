// Spotify Connect device: dealer websocket + connect-state, translating
// remote commands (transfer, play, pause, seek, skip, volume, queue,
// shuffle/repeat) into Player actions and publishing our player state.
#pragma once
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include "listenica/pb.h"
#include "listenica/http.h"
#include "listenica/player.h"
#include "listenica/spclient.h"

typedef struct cJSON cJSON;

namespace listenica {

struct ConnectConfig {
  std::string name = "Tónica";
  std::string device_id;
  std::string dealer_hostport = "dealer.spotify.com:443";
  std::string dealer_url;  // full URL override (tests); access_token is appended
  uint16_t initial_volume = 0x4CCC;  // 30% on first boot; firmware restores the last-used volume
  bool adopt_source_volume = true;    // on transfer, continue at the source device's volume...
  std::function<bool()> autoplay_enabled;  // account's Autoplay setting (from the AP session); unset = off
  uint16_t adopt_volume_cap = 0xFFFF; // optional cap on an adopted volume; default 100% = none (user choice 2026-11-02)
  int volume_steps = 64;
  int device_type = 4;  // SPEAKER
};

struct CtxTrack {
  std::string uri, uid, provider = "context";
  std::vector<std::pair<std::string, std::string>> metadata;
};

// Thread-safe copy of the Connect state, refreshed after every state publish.
struct ConnectSnapshot {
  bool active = false, is_playing = false, is_paused = true, is_buffering = false;
  std::string track_uri, context_uri;
  uint32_t position_ms = 0, duration_ms = 0;
  int64_t timestamp_ms = 0;
  uint16_t volume = 0;
  int others_online = -1;  // the account's other online devices (phone apps...), -1 = no cluster seen yet
  bool shuffle = false, repeat_ctx = false, repeat_track = false;
  std::vector<CtxTrack> prev, next;  // next = queue then context
};

class Connect {
 public:
  Connect(ConnectConfig cfg, SpClient* sp, Player* player);
  ~Connect();
  void start();
  void stop();

  // observability (tests, status logs)
  uint16_t volume() const { return volume_; }
  bool active() const { return active_; }
  bool dealer_connected() const { return dealer_up_; }
  uint32_t commands_handled() const { return commands_handled_; }
  uint32_t state_puts() const { return state_puts_; }
  uint32_t state_put_failures() const { return state_put_failures_; }
  // Called (from the Connect task) whenever the volume changes, e.g. to persist it.
  void set_volume_listener(std::function<void(uint16_t)> fn) { on_volume_ = std::move(fn); }
  // Local control (WebSocket API, buttons): same handling as remote commands.
  // `command_json` is a Connect command object, e.g. {"endpoint":"pause"}.
  void local_command(const std::string& command_json);
  void local_volume(uint16_t v);
  // Take over playback from the active device; if there is nothing to take over and a
  // preset is given, play it. preset: "liked" (Liked Songs) or a spotify: context URI;
  // mode: default | shuffle | newest | radio | resume (resume_track/pos: where it stopped).
  struct Preset {
    std::string uri, mode = "default", resume_track;
    uint32_t resume_pos_ms = 0;
    uint32_t session_window_min = 120;  // resume the last session instead if it changed within this
  };
  void local_activate(const Preset& preset);
  void local_activate();  // take over only (no preset)
  // The context URI a preset plays ("liked" -> the account's collection).
  std::string preset_context(const std::string& preset_uri);
  void local_deactivate();  // stop and give up the active role
  ConnectSnapshot snapshot();
  int others_online() const { return others_online_; }  // from the last cluster seen
  // Fresh count: re-uploads the state and reads the cluster in Spotify's reply (thread-safe; blocks <= timeout).
  int query_others_online(uint32_t timeout_ms = 3000);
  void set_state_listener(std::function<void()> fn) { on_state_ = std::move(fn); }
  // Moments worth telling a listener about (spoken notifications). Called from the Connect task.
  // KeysRefused: 2 tracks in a row were refused their key (the account is blocked); LoadFailures:
  // 5 tracks in a row failed for any reason. Both pause playback instead of skipping on.
  enum class Notice { TookOver, Resumed, PresetStarted, Autoplay, Transfer, KeysRefused, LoadFailures };
  // arg: Autoplay = the finished context's kind (album/playlist/collection/artist/...);
  //      Transfer = the sending device's name ("" if unknown); others: "".
  void set_notice_listener(std::function<void(Notice, const std::string&)> fn) { on_notice_ = std::move(fn); }
  // Sinfónica (set before start(); polled once a second on the Connect task). While `followers`
  // returns N > 0 this device is listed as a group, "Sinfónica of N+1" with is_group set. While
  // `following` is true it plays another speaker's stream: it gives up playback here and is
  // hidden from Spotify's device lists (Capabilities.hidden, as the controller's observer is).
  void set_group_source(std::function<int()> followers, std::function<bool()> following) {
    group_followers_fn_ = std::move(followers);
    group_following_fn_ = std::move(following);
  }

 private:
  struct Event {
    enum Kind { DealerMessage, DealerRequest, PlayerEv, Tick, LocalActivate, LocalDeactivate, RefreshCluster, ContextFetched } kind;
    std::string uri, key, json;
    Bytes payload;
    PlayerEvent pev = PlayerEvent::Stopped;
  };

  ConnectConfig cfg_;
  SpClient* sp_;
  Player* player_;
  std::atomic<bool> quit_{false}, dealer_up_{false}, loop_done_{true}, dealer_done_{true};
  std::atomic<uint16_t> volume_;
  std::atomic<bool> active_{false};
  std::atomic<uint32_t> commands_handled_{0}, state_puts_{0}, state_put_failures_{0};

  std::mutex qmu_;
  std::condition_variable qcv_;
  std::deque<Event> q_;
  std::mutex out_mu_;
  std::deque<std::string> outbox_;  // dealer replies (sent by the dealer thread)

  // --- connect state (event-loop thread only) ---
  std::string connection_id_;
  std::string session_id_, playback_id_, queue_revision_;
  std::string context_uri_, context_url_, next_page_url_;
  std::vector<std::pair<std::string, std::string>> context_metadata_;
  Bytes play_origin_;  // encoded PlayOrigin
  std::vector<CtxTrack> tracks_;   // context tracks in context order
  std::vector<size_t> order_;      // play order (shuffled or identity)
  size_t pos_in_order_ = 0;
  std::deque<CtxTrack> queue_;     // user queue (plays before next context track)
  size_t published_ctx_end_ = 0;   // order_ position after the last context track in our published next_tracks
  bool playing_queue_track_ = false;
  // The transfer carried only a window of the context (often just the current track): fetch it
  // all at once, in the background, on SpClient's own context connection. Deferring it until the
  // track played (to spare the shared connection) let a track end before the list arrived.
  bool ctx_refresh_ = false;
  CtxTrack current_;
  // Tracks actually played before the current one (newest last, max 20): 'previous' goes back
  // through these, so it works with shuffle and across contexts (autoplay, a new playlist).
  std::deque<CtxTrack> history_;
  bool next_replays_pos_ = false;  // went back to a track outside the order: next resumes at pos_in_order_
  bool shuffle_ = false, repeat_ctx_ = false, repeat_track_ = false;
  bool is_playing_ = false, is_paused_ = true, is_buffering_ = false;
  uint32_t duration_ms_ = 0;
  int64_t active_since_ms_ = 0;  // when this device became active (PutStateRequest.started_playing_at); 0 = inactive
  uint32_t message_id_ = 0;
  std::string last_cmd_device_;
  uint32_t last_cmd_msg_id_ = 0;
  std::string preloaded_for_, next_for_player_;
  std::map<std::string, uint32_t> cluster_volumes_;  // device id -> last reported volume
  std::map<std::string, std::string> cluster_names_;  // device id -> name (DeviceInfo.name)
  std::function<void(Notice, const std::string&)> on_notice_;
  std::function<int()> group_followers_fn_;
  std::function<bool()> group_following_fn_;
  int group_followers_ = 0;       // as last published
  bool group_hidden_ = false;     // as last published
  std::string published_name() const;
  int fail_streak_ = 0, refused_streak_ = 0;
  uint64_t play_after_transfer_until_ = 0;  // a local press asked for a transfer: play it (loop thread)  // consecutive tracks that failed to load / were refused a key
  void notice(Notice n, const std::string& arg = std::string()) {
    if (on_notice_) on_notice_(n, arg);
  }
  std::function<void(uint16_t)> on_volume_;
  std::function<void()> on_state_;
  std::atomic<int> others_online_{-1};
  // Seen 2026-11-03 and 11-05 (1 boot in 5): Spotify answered the first registration with HTTP 200
  // and a cluster that included this device, yet left it out of the account's device list (other
  // clients, phones) for 16+ min. One more state upload on the same connection fixed it at once.
  // So every registration is repeated after 5 s and 60 s.
  uint64_t reannounce_at_ms_[2] = {0, 0};
  std::atomic<uint32_t> cluster_seq_{0};  // bumped whenever a cluster has been read
  std::string cluster_active_;            // active device in the last cluster ("" = none; loop thread only)
  // Account Autoplay switch as changed live (dealer user-attribute messages); -1 = use the login value.
  int autoplay_attr_ = -1;
  bool autoplay_on();
  void handle_user_attributes(const Event& e);
  int64_t cluster_state_ms_ = 0;          // player_state.timestamp: when the session last changed (unix ms)
  void note_cluster(const pb::Field& cluster);
  std::mutex snap_mu_;
  ConnectSnapshot snap_;
  void update_snapshot();
  void apply_volume(uint16_t v, const char* why);
  uint64_t last_put_ms_ = 0;

  static void loop_entry(void*);
  static void dealer_entry(void*);
  void loop();
  void dealer_loop();
  void push(Event e);

  void handle_message(const Event& e);
  void handle_request(const Event& e);
  bool handle_command(cJSON* cmd, const std::string& endpoint);
  void handle_player_event(PlayerEvent pe, const std::string& uri);
  void tick();
  void preload_next_now();

  bool handle_transfer(const Bytes& transfer_state);
  bool handle_play(cJSON* cmd);
  void set_context_from_json(cJSON* ctx);
  bool resolve_context(const std::string& uri);
  bool start_autoplay();  // end of context: continue with Spotify's autoplay, if the account has it on
  bool play_preset(const Preset& p);
  Preset pending_preset_;  // carried by a LocalActivate event
  // Fetched as soon as the context's last track starts, so its first track can be the
  // player's gapless 'next' (otherwise the jump into autoplay had a gap).
  void prefetch_autoplay();
  std::string autoplay_for_;   // context the prefetch belongs to ("" = none tried)
  std::string autoplay_json_;  // the continuation, unparsed
  CtxTrack autoplay_first_;    // its first track (peek_next at the end of the context)
  void refresh_context();  // synchronous (a skip that needs the whole context now)
  // The transfer gave only part of the context and the rest is still being fetched: past the last
  // known track, the next one is unknown (not autoplay). 2026-11-05: a transfer carrying 1 track
  // ended 18 s later and a stale autoplay continuation played instead of the playlist's next.
  bool context_incomplete() const { return ctx_refresh_ || ctxf_busy_; }
  void clear_autoplay_prefetch() {
    autoplay_for_.clear();
    autoplay_json_.clear();
    autoplay_first_ = CtxTrack();
  }
  // After a transfer, the whole context is fetched on a worker thread: a big playlist takes
  // seconds (3094 tracks: 5 s), and the loop must keep answering dealer commands meanwhile.
  struct CtxFetch {
    std::string uri, url, next;
    std::vector<CtxTrack> tracks;
    bool ok = false;
    uint32_t seq = 0;
  };
  std::mutex ctxf_mu_;
  std::unique_ptr<CtxFetch> ctxf_done_;
  std::atomic<bool> ctxf_busy_{false};
  uint32_t ctxf_seq_ = 0;  // a newer refresh (sync or async) makes an older result stale
  void refresh_context_async();
  void apply_fetched_context(CtxFetch& f);
  static bool fetch_context(SpClient* sp, const std::string& uri, std::vector<CtxTrack>& tracks, std::string& url,
                            std::string& next);
  // Just the start of the context, until the track named by uid or index has arrived (a play
  // command naming no uri): false if it isn't on the first page.
  bool fetch_context_until(const std::string& uri, const std::string& want_uid, int want_idx);
  static void append_pages(cJSON* pages, std::vector<CtxTrack>& tracks, std::string& next);
  void append_pages_json(cJSON* pages);
  void rebuild_order(int keep_index);
  bool load_current(uint32_t position_ms, bool play);
  bool advance(bool user_initiated, bool load = true);
  bool go_prev(uint32_t pressed_ago_ms = 0);  // pressed_ago: judge "early in the track" at the press, not now
  void stop_playback();
  int find_track(const std::string& uid, const std::string& uri);
  const CtxTrack* peek_next();
  std::vector<const CtxTrack*> peek_ahead(size_t k);  // the next k tracks (queue, then order)

  void put_state(int reason);
  // librespot spirc.rs handle_activate / handle_disconnect + state.rs became_inactive:
  void set_active(bool on);
  void become_inactive(const char* why);  // final state PUT, then PUT .../inactive; never reason 7
  // librespot: while active, any cluster push re-sends our state (grouped, 200 ms)
  uint64_t state_update_at_ms_ = 0;
  void handle_session_update(const Event& e);
  Bytes build_put_state(int reason);
  static std::string random_hex(size_t nbytes);
};

}  // namespace listenica
