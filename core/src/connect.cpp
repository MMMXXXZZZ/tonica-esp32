#include "listenica/connect.h"
#include <algorithm>
#include <cstring>
#include <set>
#include "cJSON.h"
#include "listenica/crypto.h"
#include "listenica/pb.h"
#include "listenica/version.h"

static const char* TAG = "connect";

namespace listenica {

// librespot sends only these three; giving up the session is PUT .../inactive, never BECAME_INACTIVE (7):
// Spotify answered our reason-7 PUTs with HTTP 422 (2026-11-05).
enum PutReason { NEW_DEVICE = 3, PLAYER_STATE_CHANGED = 4, VOLUME_CHANGED = 5 };

static std::string jstr(cJSON* o, const char* k) {
  cJSON* v = cJSON_GetObjectItem(o, k);
  return cJSON_IsString(v) ? std::string(v->valuestring) : std::string();
}
static bool jbool(cJSON* o, const char* k, bool d = false) {
  cJSON* v = cJSON_GetObjectItem(o, k);
  return cJSON_IsBool(v) ? cJSON_IsTrue(v) : d;
}
static bool jhas(cJSON* o, const char* k) { return cJSON_GetObjectItem(o, k) != nullptr; }
static double jnum(cJSON* o, const char* k, double d = 0) {
  cJSON* v = cJSON_GetObjectItem(o, k);
  return cJSON_IsNumber(v) ? v->valuedouble : d;
}

std::string Connect::random_hex(size_t n) {
  Bytes b(n);
  random_bytes(b.data(), n);
  return hex(b);
}

Connect::Connect(ConnectConfig cfg, SpClient* sp, Player* player)
    : cfg_(std::move(cfg)), sp_(sp), player_(player), volume_(cfg_.initial_volume) {
  session_id_ = random_hex(16);
  player_->set_volume(volume_);
  player_->set_event_handler([this](PlayerEvent pe, const std::string& uri) {
    Event e;
    e.kind = Event::PlayerEv;
    e.pev = pe;
    e.uri = uri;
    push(e);
  });
}

Connect::~Connect() { stop(); }

void Connect::start() {
  quit_ = false;
  loop_done_ = false;
  dealer_done_ = false;
  spawn_thread("connect", 16 * 1024, true, 5, &Connect::loop_entry, this);
  spawn_thread("dealer", 12 * 1024, true, 5, &Connect::dealer_entry, this);
}

void Connect::stop() {
  if (quit_) return;
  quit_ = true;
  qcv_.notify_all();
  while (!loop_done_ || !dealer_done_ || ctxf_busy_) sleep_ms(20);
}

void Connect::loop_entry(void* p) { static_cast<Connect*>(p)->loop(); }
void Connect::dealer_entry(void* p) { static_cast<Connect*>(p)->dealer_loop(); }

void Connect::apply_volume(uint16_t v, const char* why) {
  volume_ = v;
  player_->set_volume(v);
  SLOGI(TAG, "volume -> %d%% (%s)", int(uint32_t(v) * 100 / 65535), why);
  if (on_volume_) on_volume_(v);
}

void Connect::push(Event e) {
  std::lock_guard<std::mutex> l(qmu_);
  q_.push_back(std::move(e));
  qcv_.notify_all();
}

// ---------------------------------------------------------------- dealer
void Connect::dealer_loop() {
  int backoff = 1;
  while (!quit_) {
    std::string token;
    if (!sp_->access_token(token)) {
      SLOGW(TAG, "dealer: no access token, retrying in %ds", backoff);
      sleep_ms(1000 * backoff);
      backoff = std::min(backoff * 2, 60);
      continue;
    }
    size_t c = cfg_.dealer_hostport.rfind(':');
    std::string host = cfg_.dealer_hostport.substr(0, c);
    WebSocket ws;
    std::string url = cfg_.dealer_url.empty() ? "wss://" + host + ":443/?access_token=" + token
                                              : cfg_.dealer_url + "?access_token=" + token;
    if (!ws.connect(url)) {
      SLOGW(TAG, "dealer connect failed, retrying in %ds", backoff);
      sleep_ms(1000 * backoff);
      backoff = std::min(backoff * 2, 60);
      continue;
    }
    SLOGI(TAG, "dealer connected (%s)", host.c_str());
    dealer_up_ = true;
    backoff = 1;
    uint64_t last_ping = now_ms();
    while (!quit_ && ws.is_open()) {
      // drain replies
      {
        std::lock_guard<std::mutex> l(out_mu_);
        while (!outbox_.empty()) {
          ws.send_text(outbox_.front());
          outbox_.pop_front();
        }
      }
      if (now_ms() - last_ping > 30000) {
        ws.send_text("{\"type\":\"ping\"}");
        last_ping = now_ms();
      }
      if (now_ms() - ws.last_pong_ms > 100000) {
        SLOGW(TAG, "dealer: no pong for 100 s, reconnecting");
        break;
      }
      std::string msg;
      int r = ws.recv(msg, 200);
      if (r < 0) break;
      if (r == 0) continue;
      cJSON* j = cJSON_Parse(msg.c_str());
      if (!j) continue;
      std::string type = jstr(j, "type");
      if (type == "pong") {
        ws.last_pong_ms = now_ms();
      } else if (type == "message") {
        Event e;
        e.kind = Event::DealerMessage;
        e.uri = jstr(j, "uri");
        cJSON* hdrs = cJSON_GetObjectItem(j, "headers");
        std::string te = hdrs ? jstr(hdrs, "Transfer-Encoding") : "";
        if (e.uri.rfind("hm://pusher/v1/connections/", 0) == 0 && hdrs) {
          e.json = jstr(hdrs, "Spotify-Connection-Id");
        }
        cJSON* pl = cJSON_GetObjectItem(j, "payloads");
        cJSON* first = cJSON_IsArray(pl) ? cJSON_GetArrayItem(pl, 0) : nullptr;
        if (cJSON_IsString(first)) {
          Bytes raw = base64_decode(first->valuestring);
          if (te == "gzip") {
            Bytes out;
            if (gunzip(raw.data(), raw.size(), out)) raw.swap(out);
            else SLOGW(TAG, "dealer %s: gzip payload (%u bytes) failed to inflate", e.uri.c_str(), unsigned(raw.size()));
          }
          e.payload = std::move(raw);
        } else if (first) {
          char* s = cJSON_PrintUnformatted(first);
          e.payload.assign(s, s + strlen(s));
          cJSON_free(s);
        }
        push(std::move(e));
      } else if (type == "request") {
        Event e;
        e.kind = Event::DealerRequest;
        e.key = jstr(j, "key");
        e.uri = jstr(j, "message_ident");
        cJSON* hdrs = cJSON_GetObjectItem(j, "headers");
        std::string te = hdrs ? jstr(hdrs, "Transfer-Encoding") : "";
        cJSON* pl = cJSON_GetObjectItem(j, "payload");
        std::string comp = pl ? jstr(pl, "compressed") : "";
        Bytes raw = base64_decode(comp);
        if (te == "gzip" || (raw.size() > 2 && raw[0] == 0x1f && raw[1] == 0x8b)) {
          Bytes out;
          if (gunzip(raw.data(), raw.size(), out)) raw.swap(out);
        }
        e.json.assign(raw.begin(), raw.end());
        push(std::move(e));
      }
      cJSON_Delete(j);
    }
    dealer_up_ = false;
    ws.close();
    SLOGW(TAG, "dealer disconnected");
    sleep_ms(1000);
  }
  dealer_done_ = true;
}

// ------------------------------------------------------------ event loop
void Connect::loop() {
  uint64_t last_tick = 0;
  while (!quit_) {
    Event e;
    bool have = false;
    {
      std::unique_lock<std::mutex> l(qmu_);
      qcv_.wait_for(l, std::chrono::milliseconds(250), [&] { return !q_.empty() || quit_; });
      if (!q_.empty()) {
        e = std::move(q_.front());
        q_.pop_front();
        have = true;
      }
    }
    if (have) {
      switch (e.kind) {
        case Event::DealerMessage:
          handle_message(e);
          break;
        case Event::DealerRequest:
          handle_request(e);
          break;
        case Event::PlayerEv:
          handle_player_event(e.pev, e.uri);
          break;
        case Event::LocalActivate:
          if (!active_) {
            // Spotify keeps the last playback state even with every app closed, so a
            // transfer nearly always "succeeds". Take over only a *live* session (another
            // device is active right now); otherwise play the preset, if there is one.
            put_state(PLAYER_STATE_CHANGED);  // the reply carries a fresh cluster
            bool live = !cluster_active_.empty() && cluster_active_ != cfg_.device_id;
            Preset p;
            {
              std::lock_guard<std::mutex> l(snap_mu_);
              p = pending_preset_;
            }
            // a session paused recently (e.g. on this speaker, then the phone was put away) is still
            // "the music I was listening to": resume it within the window, else start the preset
            int64_t age_min = cluster_state_ms_ > 0 ? (unix_ms() - cluster_state_ms_) / 60000 : -1;
            bool recent = age_min >= 0 && age_min < int64_t(p.session_window_min);
            if (live || recent || p.uri.empty()) {
              play_after_transfer_until_ = now_ms() + 10000;  // a press means play, even if the session was paused
              int st = sp_->transfer_to_self();
              SLOGI(TAG, "activate: %s (last change %lld min ago) -> transfer HTTP %d",
                    live ? "taking over the active device" : "resuming the last session", (long long)age_min, st);
              if (st == 200 || st == 204) notice(live ? Notice::TookOver : Notice::Resumed);
            } else {
              SLOGI(TAG, "activate: no device active, last session %lld min old; playing preset %s (%s)",
                    (long long)age_min, p.uri.c_str(), p.mode.c_str());
              if (!play_preset(p)) SLOGW(TAG, "preset could not be played");
              else notice(Notice::PresetStarted);
            }
          }
          break;
        case Event::RefreshCluster:
          put_state(PLAYER_STATE_CHANGED);  // the reply carries a fresh cluster
          break;
        case Event::ContextFetched: {
          std::unique_ptr<CtxFetch> f;
          {
            std::lock_guard<std::mutex> l(ctxf_mu_);
            f.swap(ctxf_done_);
          }
          if (f && f->seq == ctxf_seq_ && f->uri == context_uri_ && active_) apply_fetched_context(*f);
          break;
        }
        case Event::LocalDeactivate:
          become_inactive("stopped locally");
          break;
        default:
          break;
      }
    }
    if (state_update_at_ms_ && now_ms() >= state_update_at_ms_) {
      state_update_at_ms_ = 0;
      if (active_) put_state(PLAYER_STATE_CHANGED);
    }
    if (now_ms() - last_tick >= 1000) {
      last_tick = now_ms();
      tick();
    }
  }
  become_inactive("shutting down");
  loop_done_ = true;
}

bool Connect::autoplay_on() {
  if (autoplay_attr_ >= 0) return autoplay_attr_ == 1;
  return cfg_.autoplay_enabled && cfg_.autoplay_enabled();
}

// Autoplay switched in a Spotify app while we are logged in (librespot spirc.rs):
//   spotify:user:attributes:update  UserAttributesUpdate { map<string,string> pairs = 1 }
//   spotify:user:attributes:mutated UserAttributesMutation { repeated MutatedField{ name = 1 } fields = 1 }
// A mutation names the attribute without a value: the stored value toggles.
void Connect::handle_user_attributes(const Event& e) {
  if (e.uri.rfind("spotify:user:attributes:update", 0) == 0) {
    pb::Reader(e.payload).each(1, [&](const pb::Field& entry) {
      pb::Reader er(entry);
      if (er.str_of(1) == "autoplay") {
        autoplay_attr_ = er.str_of(2) == "1" ? 1 : 0;
        SLOGI(TAG, "account autoplay now %s (attribute update)", autoplay_attr_ ? "on" : "off");
      }
    });
  } else {
    pb::Reader(e.payload).each(1, [&](const pb::Field& f) {
      if (pb::Reader(f).str_of(1) == "autoplay") {
        autoplay_attr_ = autoplay_on() ? 0 : 1;
        SLOGI(TAG, "account autoplay now %s (toggled in an app)", autoplay_attr_ ? "on" : "off");
        autoplay_for_.clear();  // a prefetched continuation may no longer apply
      }
    });
  }
}

void Connect::handle_message(const Event& e) {
  if (e.uri.rfind("spotify:user:attributes:", 0) == 0) return handle_user_attributes(e);
  if (e.uri.rfind("hm://pusher/v1/connections/", 0) == 0) {
    connection_id_ = e.json;
    SLOGI(TAG, "connection id received; announcing device '%s'", cfg_.name.c_str());
    put_state(NEW_DEVICE);
    reannounce_at_ms_[0] = now_ms() + 5000;
    reannounce_at_ms_[1] = now_ms() + 60000;
    return;
  }
  if (e.uri.rfind("hm://connect-state/v1/connect/volume", 0) == 0) {
    int64_t v = int64_t(pb::Reader(e.payload).varint_of(1));
    v = std::max<int64_t>(0, std::min<int64_t>(65535, v));
    apply_volume(uint16_t(v), "remote");
    commands_handled_++;
    put_state(VOLUME_CHANGED);
    return;
  }
  if (e.uri.rfind("hm://connect-state/v1/cluster", 0) == 0) {
    // ClusterUpdate { cluster=1 { active_device_id=2 } }
    pb::Field cl;
    if (!pb::Reader(e.payload).find(1, cl)) {
      // librespot goes inactive on a *parsed* update without a cluster; our reader can't tell that
      // from a garbled payload, so a malformed push is only logged.
      SLOGW(TAG, "cluster update without a cluster (%u bytes): ignored", unsigned(e.payload.size()));
      return;
    }
    // Cluster.device = map<string, DeviceInfo>(4); DeviceInfo.volume = 2
    note_cluster(cl);
    pb::Reader(cl).each(4, [&](const pb::Field& ent) {
      pb::Reader er(ent);
      std::string id = er.str_of(1);
      pb::Field di;
      if (!id.empty() && er.find(2, di)) {
        pb::Reader dr(di);
        pb::Field vf;
        if (dr.find(2, vf)) cluster_volumes_[id] = uint32_t(vf.v);
        std::string nm = dr.str_of(3);  // DeviceInfo.name
        if (!nm.empty()) cluster_names_[id] = nm;
      }
    });
    std::string active_dev = pb::Reader(cl).str_of(2);
    if (active_ && active_dev != cfg_.device_id) {
      become_inactive("playback moved to another device");
    } else if (active_ && !state_update_at_ms_) {
      state_update_at_ms_ = now_ms() + 200;  // librespot's workaround: re-send our state after others' updates
    }
    return;
  }
  if (e.uri.rfind("social-connect/v2/session_update", 0) == 0) return handle_session_update(e);
  if (e.uri.rfind("hm://connect-state/v1/connect/logout", 0) == 0) {
    SLOGW(TAG, "logout requested by server");
    return;
  }
  SLOGD(TAG, "unhandled message %s", e.uri.c_str());
}

void Connect::handle_request(const Event& e) {
  bool ok = false, replied = false;
  auto reply = [&](bool success) {
    if (e.key.empty()) return;  // local commands have no dealer key and need no reply
    // librespot dealer Responder: {"type":"reply","key":..,"payload":{"success":..}}; cJSON escapes the key
    cJSON* o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "type", "reply");
    cJSON_AddStringToObject(o, "key", e.key.c_str());
    cJSON_AddBoolToObject(cJSON_AddObjectToObject(o, "payload"), "success", success);
    char* txt = cJSON_PrintUnformatted(o);
    std::string r = txt ? txt : "";
    cJSON_free(txt);
    cJSON_Delete(o);
    std::lock_guard<std::mutex> l(out_mu_);
    outbox_.push_back(r);
    replied = true;
  };
  cJSON* j = cJSON_Parse(e.json.c_str());
  if (j) {
    cJSON* cmd = cJSON_GetObjectItem(j, "command");
    last_cmd_device_ = jstr(j, "sent_by_device_id");
    last_cmd_msg_id_ = uint32_t(jnum(j, "message_id"));
    std::string endpoint = cmd ? jstr(cmd, "endpoint") : "";
    SLOGI(TAG, "command '%s' from %s", endpoint.c_str(), last_cmd_device_.substr(0, 8).c_str());
    ok = cmd && handle_command(cmd, endpoint);
    commands_handled_++;
    cJSON_Delete(j);
  }
  // librespot order: handle, publish the new state, then reply. Answering first made Spotify
  // judge the transfer before our state said "active" (2026-11-05: phone kept re-sending it).
  // Handling must stay fast: a transfer no longer resolves the whole context first.
  if (ok) put_state(PLAYER_STATE_CHANGED);
  if (!replied) reply(ok);
}

bool Connect::handle_command(cJSON* cmd, const std::string& ep) {
  if (ep == "transfer") {
    std::string data = jstr(cmd, "data");
    if (data.empty()) return false;
    {
      std::string from = jstr(cmd, "from_device_identifier");
      if (from.empty()) from = last_cmd_device_;
      auto nit = cluster_names_.find(from);
      notice(Notice::Transfer, nit == cluster_names_.end() ? std::string() : nit->second);
    }
    if (cfg_.adopt_source_volume) {
      std::string from = jstr(cmd, "from_device_identifier");
      if (from.empty()) from = last_cmd_device_;
      auto it = cluster_volumes_.find(from);
      if (it != cluster_volumes_.end()) {
        uint16_t v = uint16_t(std::min<uint32_t>(it->second, cfg_.adopt_volume_cap));
        SLOGI(TAG, "transfer from %s: adopting its volume %u%%", from.substr(0, 8).c_str(), unsigned(v * 100 / 65535));
        apply_volume(v, "transfer");
      }
    }
    return handle_transfer(base64_decode(data));
  }
  if (ep == "play") return handle_play(cmd);
  if (!active_ && ep != "set_options" && ep != "set_shuffling_context") {
    // most commands only make sense for the active device
    if (ep != "resume") return false;
  }
  if (ep == "pause") {
    player_->pause();
    is_playing_ = true;
    is_paused_ = true;
    return true;
  }
  if (ep == "resume") {
    if (!player_->is_loaded()) return load_current(0, true);
    player_->play();
    is_paused_ = false;
    is_playing_ = true;
    return true;
  }
  if (ep == "seek_to") {
    double v = jnum(cmd, "value", -1);
    if (v < 0) return false;
    // "relative": "beginning" (default) or "current"
    if (jstr(cmd, "relative") == "current") v += player_->position_ms();
    player_->seek(uint32_t(std::max(0.0, v)));
    return true;
  }
  if (ep == "skip_next") {
    cJSON* track = cJSON_GetObjectItem(cmd, "track");
    if (track) {
      int idx = find_track(jstr(track, "uid"), jstr(track, "uri"));
      if (idx >= 0) {
        auto it = std::find(order_.begin(), order_.end(), size_t(idx));
        if (it != order_.end()) {
          pos_in_order_ = size_t(it - order_.begin());
          current_ = tracks_[size_t(idx)];
          playing_queue_track_ = false;
          return load_current(0, true);
        }
      }
    }
    return advance(true);
  }
  // local_pressed_ago_ms: set by the button (a triple press is only recognised ~1.5 s after it starts)
  if (ep == "skip_prev") return go_prev(uint32_t(jnum(cmd, "local_pressed_ago_ms", 0)));
  if (ep == "set_shuffling_context") {
    shuffle_ = jbool(cmd, "value");
    rebuild_order(order_.empty() ? -1 : int(order_[pos_in_order_]));
    return true;
  }
  if (ep == "set_repeating_context") {
    repeat_ctx_ = jbool(cmd, "value");
    return true;
  }
  if (ep == "set_repeating_track") {
    repeat_track_ = jbool(cmd, "value");
    return true;
  }
  if (ep == "set_options") {
    if (jhas(cmd, "repeating_context")) repeat_ctx_ = jbool(cmd, "repeating_context");
    if (jhas(cmd, "repeating_track")) repeat_track_ = jbool(cmd, "repeating_track");
    if (jhas(cmd, "shuffling_context")) {
      shuffle_ = jbool(cmd, "shuffling_context");
      rebuild_order(order_.empty() ? -1 : int(order_[pos_in_order_]));
    }
    return true;
  }
  if (ep == "add_to_queue") {
    cJSON* t = cJSON_GetObjectItem(cmd, "track");
    if (!t) return false;
    CtxTrack ct;
    ct.uri = jstr(t, "uri");
    ct.uid = jstr(t, "uid");
    if (ct.uid.empty()) ct.uid = "q" + random_hex(8);
    ct.provider = "queue";
    ct.metadata = {{"is_queued", "true"}};
    queue_.push_back(ct);
    queue_revision_ = random_hex(8);
    preloaded_for_.clear();
    return true;
  }
  if (ep == "set_queue") {
    // The phone's next_tracks is the whole upcoming list and it is authoritative (librespot
    // state/tracks.rs set_next_tracks). Seen from iOS 2026-11-05: a reordered queued track came
    // back with an empty provider and no is_queued flag, only the uid we gave it; a removed
    // "Next from" track is simply absent; adding sends add_to_queue instead.
    std::set<std::string> our_queue;
    for (auto& q : queue_) our_queue.insert(q.uid);
    size_t start = std::min(next_replays_pos_ ? pos_in_order_ : pos_in_order_ + 1, order_.size());
    size_t end = std::max(start, std::min(published_ctx_end_, order_.size()));
    std::deque<CtxTrack> queue;
    std::vector<size_t> upcoming;  // tracks_ indices, in the phone's order
    cJSON* it;
    cJSON_ArrayForEach(it, cJSON_GetObjectItem(cmd, "next_tracks")) {
      std::string uri = jstr(it, "uri"), uid = jstr(it, "uid"), prov = jstr(it, "provider");
      if (uri.empty()) uri = current_.uri;  // the current track dragged into the list (librespot)
      cJSON* md = cJSON_GetObjectItem(it, "metadata");
      bool queued = prov == "queue" || (md && jstr(md, "is_queued") == "true") || (!uid.empty() && our_queue.count(uid));
      if (queued) {
        CtxTrack ct;
        ct.uri = uri;
        ct.uid = uid.empty() ? "q" + random_hex(8) : uid;
        ct.provider = "queue";
        ct.metadata = {{"is_queued", "true"}};
        queue.push_back(ct);
        continue;
      }
      std::string k = uri_kind(uri);
      if (k != "track" && k != "episode") continue;  // delimiters and the like
      int idx = -1;
      for (size_t i = start; i < end && idx < 0; i++) {  // the window we published first
        const CtxTrack& t = tracks_[order_[i]];
        if (uid.empty() ? t.uri == uri : t.uid == uid) idx = int(order_[i]);
      }
      if (idx < 0) idx = find_track(uid, uri);
      if (idx >= 0) upcoming.push_back(size_t(idx));
    }
    queue_.swap(queue);
    // the upcoming part of the order: the phone's context tracks, then everything past the window
    // it was shown (tracks it left out of that window are skipped this time round)
    std::set<size_t> used(upcoming.begin(), upcoming.end());
    std::vector<size_t> order(order_.begin(), order_.begin() + long(start));
    order.insert(order.end(), upcoming.begin(), upcoming.end());
    for (size_t i = end; i < order_.size(); i++)
      if (!used.count(order_[i])) order.push_back(order_[i]);
    order_.swap(order);
    if (next_replays_pos_) pos_in_order_ = start;  // keep "next = order_[pos_in_order_]"
    queue_revision_ = jstr(cmd, "queue_revision");
    preloaded_for_.clear();
    next_for_player_.clear();  // tick() hands the player the new 'next'
    return true;
  }
  if (ep == "update_context") {
    cJSON* ctx = cJSON_GetObjectItem(cmd, "context");
    if (ctx && jstr(ctx, "uri") == context_uri_) {
      cJSON* pages = cJSON_GetObjectItem(ctx, "pages");
      if (cJSON_GetArraySize(pages) > 0) {
        std::string cur_uid = current_.uid, cur_uri = current_.uri;
        tracks_.clear();
        append_pages_json(pages);
        int idx = find_track(cur_uid, cur_uri);
        rebuild_order(idx);
      }
    }
    return true;
  }
  SLOGW(TAG, "unsupported endpoint '%s'", ep.c_str());
  return false;
}

// ------------------------------------------------------------- contexts
void Connect::append_pages_json(cJSON* pages) { append_pages(pages, tracks_, next_page_url_); }

void Connect::append_pages(cJSON* pages, std::vector<CtxTrack>& tracks, std::string& next) {
  cJSON* page;
  cJSON_ArrayForEach(page, pages) {
    cJSON* tr = cJSON_GetObjectItem(page, "tracks");
    cJSON* t;
    cJSON_ArrayForEach(t, tr) {
      CtxTrack ct;
      ct.uri = jstr(t, "uri");
      ct.uid = jstr(t, "uid");
      if (ct.uri.empty()) continue;
      std::string k = uri_kind(ct.uri);
      if (k != "track" && k != "episode") continue;
      tracks.push_back(ct);
    }
    next = jstr(page, "next_page_url");
  }
}

// Thread-safe (SpClient is): used by the loop and by the background refresh. Streamed: the JSON
// is never held in memory (see ContextScanner).
bool Connect::fetch_context(SpClient* sp, const std::string& uri, std::vector<CtxTrack>& tracks, std::string& url,
                            std::string& next) {
  static const size_t kMaxTracks = 4000;  // ~150 B each in PSRAM
  auto take = [&](ContextScanner& s) {
    for (auto& t : s.tracks) {
      std::string k = uri_kind(t.uri);
      if (k != "track" && k != "episode") continue;
      CtxTrack ct;
      ct.uri = std::move(t.uri);
      ct.uid = std::move(t.uid);
      tracks.push_back(std::move(ct));
    }
    next = s.next_page_url;
  };
  ContextScanner s(kMaxTracks);
  if (!sp->context_resolve(uri, s)) {
    sp->close_context_connection();
    return false;
  }
  url = s.url;
  take(s);
  // follow a few next pages so long playlists are mostly available
  for (int i = 0; i < 8 && !next.empty() && tracks.size() < kMaxTracks; i++) {
    ContextScanner p(kMaxTracks - tracks.size());
    std::string page_url = next;
    next.clear();
    if (!sp->context_resolve(page_url, p)) break;
    take(p);
  }
  sp->close_context_connection();  // a socket only while fetching
  return true;
}

bool Connect::fetch_context_until(const std::string& uri, const std::string& want_uid, int want_idx) {
  ContextScanner s;
  auto has = [&](const ContextScanner& sc) {
    if (!want_uid.empty()) {
      for (auto& t : sc.tracks)
        if (t.uid == want_uid) return true;
      return false;
    }
    return want_idx >= 0 && sc.tracks.size() > size_t(want_idx);
  };
  bool ok = sp_->context_resolve(uri, s, has);
  sp_->close_context_connection();
  if (!ok || !has(s)) return false;
  for (auto& t : s.tracks) {
    std::string k = uri_kind(t.uri);
    if (k != "track" && k != "episode") continue;
    CtxTrack ct;
    ct.uri = std::move(t.uri);
    ct.uid = std::move(t.uid);
    tracks_.push_back(std::move(ct));
  }
  if (context_url_.empty()) context_url_ = s.url;
  SLOGI(TAG, "context %s: first %u tracks read to start; the rest follows", uri.c_str(), unsigned(tracks_.size()));
  return !tracks_.empty();
}

bool Connect::resolve_context(const std::string& uri) {
  std::vector<CtxTrack> got;
  std::string url, next;
  if (!fetch_context(sp_, uri, got, url, next)) return false;
  if (context_url_.empty()) context_url_ = url;
  tracks_.insert(tracks_.end(), got.begin(), got.end());
  next_page_url_ = next;
  SLOGI(TAG, "context %s resolved: %u tracks", uri.c_str(), (unsigned)got.size());
  return !got.empty();
}

bool Connect::start_autoplay() {
  if (!autoplay_on() || context_uri_.empty()) return false;
  if (context_uri_.rfind("spotify:station:", 0) == 0 && tracks_.empty()) return false;
  std::string json;
  if (autoplay_for_ == context_uri_ && !autoplay_json_.empty()) {
    json.swap(autoplay_json_);  // prefetched when the last track started
  } else {
    std::vector<std::string> recent;  // the last tracks played, newest last
    size_t from = order_.size() > 10 ? order_.size() - 10 : 0;
    for (size_t i = from; i < order_.size(); i++) recent.push_back(tracks_[order_[i]].uri);
    if (!sp_->autoplay_context(context_uri_, recent, json)) return false;
  }
  autoplay_for_.clear();
  autoplay_first_ = CtxTrack();
  cJSON* j = cJSON_Parse(json.c_str());
  if (!j) return false;
  std::string prev_ctx = context_uri_;
  set_context_from_json(j);
  cJSON_Delete(j);
  for (auto& t : tracks_) t.provider = "autoplay";
  if (tracks_.empty()) {
    SLOGW(TAG, "autoplay for %s returned no tracks", prev_ctx.c_str());
    return false;
  }
  SLOGI(TAG, "end of %s: autoplay continues with %u tracks (%s)", prev_ctx.c_str(), (unsigned)tracks_.size(),
        context_uri_.c_str());
  {
    // spotify:album:x -> album; spotify:user:x:collection -> collection; spotify:playlist:x -> playlist
    std::string kind;
    size_t a = prev_ctx.find(':'), b = a == std::string::npos ? a : prev_ctx.find(':', a + 1);
    if (b != std::string::npos) kind = prev_ctx.substr(a + 1, b - a - 1);
    if (kind == "user") kind = prev_ctx.find(":collection") != std::string::npos ? "collection" : "playlist";
    notice(Notice::Autoplay, kind);
  }
  shuffle_ = false;
  rebuild_order(0);
  current_ = tracks_[0];
  playing_queue_track_ = false;
  return true;
}

void Connect::prefetch_autoplay() {
  if (!active_ || repeat_ctx_ || repeat_track_ || !queue_.empty() || order_.empty()) return;
  if (pos_in_order_ + 1 < order_.size() || next_replays_pos_) return;  // not on the last track
  if (context_incomplete()) return;  // "last" only of the part the transfer carried
  if (!autoplay_on() || context_uri_.empty()) return;
  if (autoplay_for_ == context_uri_) return;  // tried already (success or not)
  autoplay_for_ = context_uri_;
  autoplay_json_.clear();
  autoplay_first_ = CtxTrack();
  std::vector<std::string> recent;
  size_t from = order_.size() > 10 ? order_.size() - 10 : 0;
  for (size_t i = from; i < order_.size(); i++) recent.push_back(tracks_[order_[i]].uri);
  std::string json;
  if (!sp_->autoplay_context(context_uri_, recent, json)) return;
  cJSON* j = cJSON_Parse(json.c_str());
  if (!j) return;
  cJSON* pages = cJSON_GetObjectItem(j, "pages");
  cJSON* page = pages ? cJSON_GetArrayItem(pages, 0) : nullptr;
  cJSON* tr = page ? cJSON_GetObjectItem(page, "tracks") : nullptr;
  cJSON* t0 = tr ? cJSON_GetArrayItem(tr, 0) : nullptr;
  if (t0) {
    autoplay_first_.uri = jstr(t0, "uri");
    autoplay_first_.uid = jstr(t0, "uid");
    autoplay_first_.provider = "autoplay";
  }
  cJSON_Delete(j);
  if (!autoplay_first_.uri.empty()) {
    autoplay_json_.swap(json);
    SLOGI(TAG, "last track of %s: autoplay continuation ready (next: %s)", context_uri_.c_str(),
          autoplay_first_.uri.c_str());
  }
}

void Connect::set_context_from_json(cJSON* ctx) {
  ctx_refresh_ = false;
  clear_autoplay_prefetch();  // a continuation belongs to the context it was fetched for
  context_uri_ = jstr(ctx, "uri");
  context_url_ = jstr(ctx, "url");
  context_metadata_.clear();
  cJSON* md = cJSON_GetObjectItem(ctx, "metadata");
  cJSON* it;
  cJSON_ArrayForEach(it, md) if (cJSON_IsString(it)) context_metadata_.push_back({it->string, it->valuestring});
  tracks_.clear();
  next_page_url_.clear();
  append_pages_json(cJSON_GetObjectItem(ctx, "pages"));
}

int Connect::find_track(const std::string& uid, const std::string& uri) {
  if (!uid.empty())
    for (size_t i = 0; i < tracks_.size(); i++)
      if (tracks_[i].uid == uid) return int(i);
  if (!uri.empty())
    for (size_t i = 0; i < tracks_.size(); i++)
      if (tracks_[i].uri == uri) return int(i);
  return -1;
}

void Connect::rebuild_order(int keep) {
  order_.resize(tracks_.size());
  for (size_t i = 0; i < order_.size(); i++) order_[i] = i;
  if (shuffle_ && order_.size() > 1) {
    for (size_t i = order_.size() - 1; i > 0; i--) {
      uint32_t r;
      random_bytes((uint8_t*)&r, sizeof r);
      std::swap(order_[i], order_[r % (i + 1)]);
    }
    if (keep >= 0) {  // current track first, rest shuffled
      auto it = std::find(order_.begin(), order_.end(), size_t(keep));
      std::iter_swap(order_.begin(), it);
    }
    pos_in_order_ = 0;
  } else {
    pos_in_order_ = keep >= 0 ? size_t(keep) : 0;
  }
  preloaded_for_.clear();
}

static std::string json_to_playorigin_and_back(cJSON* po, Bytes& out) {
  pb::Writer w;
  if (po) {
    w.str(1, jstr(po, "feature_identifier"));
    w.str(2, jstr(po, "feature_version"));
    w.str(3, jstr(po, "view_uri"));
    w.str(4, jstr(po, "external_referrer"));
    w.str(5, jstr(po, "referrer_identifier"));
    w.str(6, jstr(po, "device_identifier"));
    cJSON* fc = cJSON_GetObjectItem(po, "feature_classes");
    cJSON* it;
    cJSON_ArrayForEach(it, fc) if (cJSON_IsString(it)) w.str(7, it->valuestring);
  }
  out = w.buf;
  return "";
}

bool Connect::play_preset(const Preset& p) {
  std::string uri = preset_context(p.uri);
  bool liked = p.uri == "liked";
  cJSON* cmd = cJSON_CreateObject();
  cJSON* opts = cJSON_AddObjectToObject(cmd, "options");
  if (p.mode == "radio" && !liked) {  // endless station seeded by the playlist / album / artist
    std::string json;
    if (!sp_->autoplay_context(uri, {}, json)) {
      cJSON_Delete(cmd);
      return false;
    }
    cJSON* ctx = cJSON_Parse(json.c_str());
    if (!ctx) {
      cJSON_Delete(cmd);
      return false;
    }
    cJSON_AddItemToObject(cmd, "context", ctx);
  } else {
    cJSON* ctx = cJSON_AddObjectToObject(cmd, "context");
    cJSON_AddStringToObject(ctx, "uri", uri.c_str());
    cJSON_AddStringToObject(ctx, "url", ("context://" + uri).c_str());
    cJSON* over = cJSON_AddObjectToObject(opts, "player_options_override");
    cJSON_AddBoolToObject(over, "shuffling_context", p.mode == "shuffle");
    if (p.mode == "shuffle") cJSON_AddBoolToObject(opts, "local_random_start", true);
    if (p.mode == "newest" && !liked) cJSON_AddBoolToObject(opts, "local_reverse", true);  // Liked Songs: newest first already
    if (p.mode == "resume" && !p.resume_track.empty()) {
      cJSON* skip = cJSON_AddObjectToObject(opts, "skip_to");
      cJSON_AddStringToObject(skip, "track_uri", p.resume_track.c_str());
      cJSON_AddNumberToObject(opts, "seek_to", p.resume_pos_ms);
    }
  }
  bool ok = handle_play(cmd);
  cJSON_Delete(cmd);
  if (ok) put_state(PLAYER_STATE_CHANGED);
  return ok;
}

bool Connect::handle_play(cJSON* cmd) {
  cJSON* ctx = cJSON_GetObjectItem(cmd, "context");
  if (!ctx) return false;
  json_to_playorigin_and_back(cJSON_GetObjectItem(cmd, "play_origin"), play_origin_);
  set_context_from_json(ctx);
  cJSON* opts = cJSON_GetObjectItem(cmd, "options");
  cJSON* skip = opts ? cJSON_GetObjectItem(opts, "skip_to") : nullptr;
  cJSON* over = opts ? cJSON_GetObjectItem(opts, "player_options_override") : nullptr;
  if (over) {
    if (jhas(over, "shuffling_context")) shuffle_ = jbool(over, "shuffling_context");
    if (jhas(over, "repeating_context")) repeat_ctx_ = jbool(over, "repeating_context");
    if (jhas(over, "repeating_track")) repeat_track_ = jbool(over, "repeating_track");
  }
  std::string want_uid = skip ? jstr(skip, "track_uid") : "";
  std::string want_uri = skip ? jstr(skip, "track_uri") : "";
  int want_idx = skip && jhas(skip, "track_index") ? int(jnum(skip, "track_index")) : -1;
  bool fetch_later = false;
  if (tracks_.empty() && !context_uri_.empty()) {
    std::string kind = uri_kind(context_uri_), want_kind = uri_kind(want_uri);
    bool local_order = opts && (jbool(opts, "local_reverse") || jbool(opts, "local_random_start"));
    if (kind == "track" || kind == "episode") {
      CtxTrack t;
      t.uri = context_uri_;
      tracks_.push_back(t);
    } else if ((want_kind == "track" || want_kind == "episode") && !local_order) {
      // The command names the track: start it now and fetch the context in the background, as
      // after a transfer. Resolving a 3094-track playlist first took 2.9 s before the reply,
      // next to the ~3 s after which a transfer failed (2026-11-05).
      CtxTrack t;
      t.uri = want_uri;
      t.uid = want_uid;
      tracks_.push_back(t);
      fetch_later = true;
    } else if (!local_order && (!want_uid.empty() || want_idx >= 0 || !shuffle_) &&
               fetch_context_until(context_uri_, want_uid, want_uid.empty() && want_idx < 0 ? 0 : want_idx)) {
      // the track is known only by uid/index: read the context just far enough to find it, start
      // it, fetch the rest in the background. Seen 2026-11-05: a full 3094-track resolve first
      // took 3.8 s and Spotify answered the sender 410 after 3.2 s.
      fetch_later = true;
    } else {
      tracks_.clear();
      resolve_context(context_uri_);  // no position at all, a local order, or not on the first page
    }
  }
  if (tracks_.empty()) return false;
  // Local preset options (button): newest first = reversed playlist order (playlists are
  // appended to, so their end holds the most recently added); random start for shuffle.
  if (opts && jbool(opts, "local_reverse")) std::reverse(tracks_.begin(), tracks_.end());
  int idx = find_track(want_uid, want_uri);
  if (idx < 0) idx = (want_idx >= 0 && want_idx < int(tracks_.size())) ? want_idx : 0;
  if (opts && jbool(opts, "local_random_start") && want_uid.empty() && want_uri.empty() && want_idx < 0) {
    uint32_t r;
    random_bytes((uint8_t*)&r, sizeof r);
    idx = int(r % tracks_.size());
  }
  queue_.clear();
  rebuild_order(idx);
  current_ = tracks_[size_t(idx)];
  playing_queue_track_ = false;
  set_active(true);
  playback_id_ = random_hex(16);
  uint32_t seek = opts ? uint32_t(jnum(opts, "seek_to", 0)) : 0;
  bool paused = opts && jbool(opts, "initially_paused");
  ctx_refresh_ = fetch_later;
  return load_current(seek, !paused);
}

static CtxTrack ctx_track_from_pb(const pb::Field& f) {
  pb::Reader r(f);
  CtxTrack t;
  t.uri = r.str_of(1);
  t.uid = r.str_of(2);
  if (t.uri.empty()) {
    Bytes gid = r.bytes_of(3);
    if (gid.size() == 16) t.uri = "spotify:track:" + gid_to_base62(gid);
  }
  return t;
}

bool Connect::handle_transfer(const Bytes& ts) {
  pb::Reader r(ts);
  pb::Field opts, playback, session, queue;
  if (r.find(1, opts)) {
    pb::Reader o(opts);
    shuffle_ = o.varint_of(1) != 0;
    repeat_ctx_ = o.varint_of(2) != 0;
    repeat_track_ = o.varint_of(3) != 0;
  }
  int64_t ts_ms = 0, pos_ms = 0;
  bool paused = false;
  CtxTrack cur;
  if (r.find(2, playback)) {
    pb::Reader p(playback);
    ts_ms = int64_t(p.varint_of(1));
    pos_ms = int64_t(int32_t(p.varint_of(2)));
    paused = p.varint_of(4) != 0;
    pb::Field ct;
    if (p.find(5, ct)) cur = ctx_track_from_pb(ct);
  }
  tracks_.clear();
  next_page_url_.clear();
  context_metadata_.clear();
  clear_autoplay_prefetch();
  std::string current_uid;
  if (r.find(3, session)) {
    pb::Reader s(session);
    play_origin_ = s.bytes_of(1);
    current_uid = s.str_of(3);
    pb::Field ctx;
    if (s.find(2, ctx)) {
      pb::Reader c(ctx);
      context_uri_ = c.str_of(1);
      context_url_ = c.str_of(2);
      c.each(3, [&](const pb::Field& m) {
        pb::Reader mr(m);
        context_metadata_.push_back({mr.str_of(1), mr.str_of(2)});
      });
      c.each(5, [&](const pb::Field& page) {
        pb::Reader pr(page);
        pr.each(4, [&](const pb::Field& t) {
          CtxTrack ct = ctx_track_from_pb(t);
          if (!ct.uri.empty()) tracks_.push_back(ct);
        });
        std::string nxt = pr.str_of(2);
        if (!nxt.empty()) next_page_url_ = nxt;
      });
    }
  }
  queue_.clear();
  if (r.find(4, queue)) {
    pb::Reader(queue).each(1, [&](const pb::Field& t) {
      CtxTrack ct = ctx_track_from_pb(t);
      ct.provider = "queue";
      if (!ct.uri.empty()) queue_.push_back(ct);
    });
  }
  if (context_uri_ == "-") context_uri_.clear();
  if (tracks_.empty() && !context_uri_.empty()) {
    std::string kind = uri_kind(context_uri_);
    if (kind == "track" || kind == "episode") {
      CtxTrack t;
      t.uri = context_uri_;
      tracks_.push_back(t);
    } else if (cur.uri.empty()) {
      resolve_context(context_uri_);  // nothing to start with: resolve now
    }  // else: start with the transferred track; the whole context follows (ctx_refresh_)
  }
  int idx = find_track(cur.uid.empty() ? current_uid : cur.uid, cur.uri);
  if (idx < 0 && !cur.uri.empty()) {
    // current track not in (the resolved part of) the context: play it standalone first
    tracks_.insert(tracks_.begin(), cur);
    idx = 0;
  }
  if (idx < 0) {
    SLOGW(TAG, "transfer without a playable track");
    return false;
  }
  rebuild_order(idx);
  current_ = tracks_[size_t(idx)];
  playing_queue_track_ = false;
  // queue may begin with the current track if it was playing from the queue
  if (!queue_.empty() && queue_.front().uri == current_.uri) queue_.pop_front();
  if (!paused && pos_ms > 0 && ts_ms > 0) pos_ms += unix_ms() - ts_ms;
  if (pos_ms < 0) pos_ms = 0;
  // A transfer this device asked for (a button press) means "play": a session left paused
  // elsewhere resumes playing here (the position above stays where it was paused).
  if (paused && now_ms() < play_after_transfer_until_) {
    paused = false;
    SLOGI(TAG, "transfer requested by a local press: playing");
  }
  play_after_transfer_until_ = 0;
  set_active(true);
  playback_id_ = random_hex(16);
  SLOGI(TAG, "transfer: context=%s tracks=%u index=%d pos=%lldms %s", context_uri_.c_str(), (unsigned)tracks_.size(),
        idx, (long long)pos_ms, paused ? "paused" : "playing");
  {
    // The transfer state carries only a window of the context (seen: 10 of an
    // album's 118 tracks), so skipping ran into "end of context". Start playing
    // now; fetch the whole context on the next tick.
    std::string kind = uri_kind(context_uri_);
    ctx_refresh_ = !context_uri_.empty() && kind != "track" && kind != "episode";
  }
  return load_current(uint32_t(pos_ms), !paused);
}

// ------------------------------------------------------------- playback
bool Connect::load_current(uint32_t pos, bool play) {
  if (current_.uri.empty()) return false;
  is_playing_ = true;
  is_paused_ = !play;
  is_buffering_ = true;
  duration_ms_ = 0;
  player_->load(current_.uri, pos, play);
  preloaded_for_.clear();
  next_for_player_.clear();  // tick() re-sends the right 'next' within a second
  return true;
}

const CtxTrack* Connect::peek_next() {
  if (repeat_track_) return &current_;
  if (!queue_.empty()) return &queue_.front();
  if (order_.empty()) return nullptr;
  size_t n = next_replays_pos_ ? pos_in_order_ : pos_in_order_ + 1;
  if (n >= order_.size()) {
    if (context_incomplete()) return nullptr;  // see context_incomplete()
    if (!repeat_ctx_) {
      bool ready = autoplay_for_ == context_uri_ && !autoplay_first_.uri.empty();
      return ready ? &autoplay_first_ : nullptr;
    }
    n = 0;
  }
  return &tracks_[order_[n]];
}

bool Connect::advance(bool user, bool load) {
  if ((ctx_refresh_ || ctxf_busy_) && active_) refresh_context();  // a skip right after a transfer
  const CtxTrack leaving = current_;
  auto go = [&]() {
    if (!leaving.uri.empty() && (leaving.uri != current_.uri || leaving.uid != current_.uid)) {
      history_.push_back(leaving);
      if (history_.size() > 20) history_.pop_front();
    }
    if (load) return load_current(0, true);
    playback_id_ = random_hex(16);  // new playback, same audio stream
    preloaded_for_.clear();
    is_playing_ = true;
    is_paused_ = false;
    return true;
  };
  if (repeat_track_ && !user) return go();
  if (!queue_.empty()) {
    current_ = queue_.front();
    queue_.pop_front();
    playing_queue_track_ = true;
    return go();
  }
  if (order_.empty()) {
    stop_playback();
    return false;
  }
  size_t n = next_replays_pos_ ? pos_in_order_ : pos_in_order_ + 1;
  next_replays_pos_ = false;
  if (n >= order_.size()) {
    if (!repeat_ctx_) {
      if (start_autoplay()) return go();
      SLOGI(TAG, "end of context");
      stop_playback();
      return true;
    }
    if (shuffle_) rebuild_order(-1);
    n = 0;
  }
  pos_in_order_ = n;
  current_ = tracks_[order_[n]];
  playing_queue_track_ = false;
  return go();
}

bool Connect::go_prev(uint32_t pressed_ago_ms) {
  uint32_t pos = player_->position_ms();
  pos = pos > pressed_ago_ms ? pos - pressed_ago_ms : 0;  // position when the user acted
  if (pos > 3000) {
    player_->seek(0);
    return true;
  }
  // Early in a track: back to the song that actually played before (shuffle and
  // context changes don't matter), else fall back to the order.
  if (!history_.empty()) {
    CtxTrack t = history_.back();
    history_.pop_back();
    int idx = find_track(t.uid, t.uri);
    auto it = idx >= 0 ? std::find(order_.begin(), order_.end(), size_t(idx)) : order_.end();
    if (it != order_.end()) {
      pos_in_order_ = size_t(it - order_.begin());
      next_replays_pos_ = false;
    } else {
      next_replays_pos_ = !playing_queue_track_;  // outside the order: next resumes where the order was
    }
    current_ = t;
    playing_queue_track_ = false;
    return load_current(0, true);
  }
  if (order_.empty()) {
    player_->seek(0);
    return true;
  }
  if (pos_in_order_ == 0) {
    if (!repeat_ctx_) {
      player_->seek(0);
      return true;
    }
    pos_in_order_ = order_.size() - 1;
  } else {
    pos_in_order_--;
  }
  current_ = tracks_[order_[pos_in_order_]];
  playing_queue_track_ = false;
  return load_current(0, true);
}

void Connect::stop_playback() {
  player_->stop();
  is_playing_ = false;
  is_paused_ = true;
  is_buffering_ = false;
}

void Connect::handle_player_event(PlayerEvent pe, const std::string& uri) {
  switch (pe) {
    case PlayerEvent::TrackChanged: {
      fail_streak_ = refused_streak_ = 0;  // gapless: the previous track played through
      // gapless transition already happened in the player: advance our state only
      const CtxTrack* n = peek_next();
      if (n && n->uri == uri) {
        advance(false, /*load=*/false);
      } else {
        SLOGW(TAG, "player moved to unexpected %s; reloading", uri.c_str());
        advance(false);
      }
      duration_ms_ = player_->current_meta().duration_ms;
      is_buffering_ = false;
      preload_next_now();
      break;
    }
    case PlayerEvent::Playing:
      is_buffering_ = false;
      is_paused_ = false;
      is_playing_ = true;
      duration_ms_ = player_->current_meta().duration_ms;
      preload_next_now();  // head + key + URLs of the next track: skips start instantly
      break;
    case PlayerEvent::Paused:
      is_buffering_ = false;
      is_paused_ = true;
      duration_ms_ = player_->current_meta().duration_ms;
      break;
    case PlayerEvent::EndOfTrack:
      fail_streak_ = refused_streak_ = 0;  // a track played to its end
      if (active_) {
        advance(false);
      }
      break;
    case PlayerEvent::LoadFailed: {
      // One refusal can be the track; the next, different track refused too means the account.
      fail_streak_++;
      refused_streak_ = player_->key_refused(current_.uri) ? refused_streak_ + 1 : 0;
      bool blocked = refused_streak_ >= 2, failing = fail_streak_ >= 5;
      if (active_ && (blocked || failing)) {
        SLOGE(TAG, "%s; pausing instead of skipping on",
              blocked ? "Spotify refused the audio key for 2 tracks in a row (account-level refusal)"
                      : "5 tracks in a row failed to load");
        fail_streak_ = refused_streak_ = 0;  // a resume tries again
        is_playing_ = true;
        is_paused_ = true;
        is_buffering_ = false;
        notice(blocked ? Notice::KeysRefused : Notice::LoadFailures);
        break;
      }
      SLOGW(TAG, "track failed to load: %s; skipping", current_.uri.c_str());
      if (active_) advance(false);
      break;
    }
    case PlayerEvent::PositionCorrected:
      break;
    default:
      return;
  }
  if (active_) put_state(PLAYER_STATE_CHANGED);
}

std::vector<const CtxTrack*> Connect::peek_ahead(size_t k) {
  std::vector<const CtxTrack*> out;
  if (repeat_track_) {
    out.push_back(&current_);
    return out;
  }
  for (auto& q : queue_) {
    if (out.size() >= k) return out;
    out.push_back(&q);
  }
  size_t n = next_replays_pos_ ? pos_in_order_ : pos_in_order_ + 1;
  for (size_t i = 0; out.size() < k && !order_.empty() && i < order_.size(); i++, n++) {
    if (n >= order_.size()) {
      if (context_incomplete()) break;
      if (!repeat_ctx_ && autoplay_for_ == context_uri_ && !autoplay_first_.uri.empty()) {
        out.push_back(&autoplay_first_);  // the prefetched autoplay continuation
        break;
      }
      if (!repeat_ctx_ || shuffle_) break;  // a reshuffle at the wrap makes the order unknown
      n = 0;
    }
    out.push_back(&tracks_[order_[n]]);
  }
  return out;
}

void Connect::preload_next_now() {
  prefetch_autoplay();  // no-op unless this is the context's last track
  auto ahead = peek_ahead(3);
  if (ahead.empty() || ahead[0]->uri == preloaded_for_) return;
  preloaded_for_ = ahead[0]->uri;
  player_->preload(ahead[0]->uri, true);  // the next track: everything
  for (size_t i = 1; i < ahead.size(); i++) player_->preload(ahead[i]->uri, false);  // then heads only
}

void Connect::refresh_context_async() {
  ctx_refresh_ = false;
  if (ctxf_busy_) return;
  ctxf_busy_ = true;
  auto* job = new CtxFetch;
  job->uri = context_uri_;
  job->seq = ++ctxf_seq_;
  struct Arg {
    Connect* c;
    CtxFetch* j;
  };
  spawn_thread("ctx_fetch", 12 * 1024, true, 4, [](void* a) {
    auto* ar = static_cast<Arg*>(a);
    ar->j->ok = fetch_context(ar->c->sp_, ar->j->uri, ar->j->tracks, ar->j->url, ar->j->next);
    {
      std::lock_guard<std::mutex> l(ar->c->ctxf_mu_);
      ar->c->ctxf_done_.reset(ar->j);
    }
    Event e;
    e.kind = Event::ContextFetched;
    ar->c->push(std::move(e));
    ar->c->ctxf_busy_ = false;
    delete ar;
  }, new Arg{this, job});
}

void Connect::apply_fetched_context(CtxFetch& f) {
  // anchor: the context track we are at. While a queued track plays that is not current_
  // (a queued track is not in the context, so the whole refresh was discarded, 2026-11-05).
  CtxTrack cur = playing_queue_track_ && pos_in_order_ < order_.size() ? tracks_[order_[pos_in_order_]] : current_;
  auto saved = std::move(tracks_);
  tracks_ = std::move(f.tracks);
  int idx = f.ok ? find_track(cur.uid, cur.uri) : -1;
  if (idx < 0) {  // keep what the transfer gave us
    tracks_ = std::move(saved);
    SLOGW(TAG, "context refresh failed; keeping %u transferred tracks", (unsigned)tracks_.size());
    return;
  }
  if (context_url_.empty()) context_url_ = f.url;
  next_page_url_ = f.next;
  SLOGI(TAG, "context %s resolved: %u tracks", f.uri.c_str(), (unsigned)tracks_.size());
  rebuild_order(idx);
  if (!playing_queue_track_) current_ = tracks_[size_t(idx)];
  next_for_player_.clear();
  put_state(PLAYER_STATE_CHANGED);
}

void Connect::refresh_context() {
  ctx_refresh_ = false;
  ++ctxf_seq_;  // any background fetch in flight is now stale
  auto saved = tracks_;
  auto saved_next = next_page_url_;
  // anchor: the context track we are at. While a queued track plays that is not current_
  // (a queued track is not in the context, so the whole refresh was discarded, 2026-11-05).
  CtxTrack cur = playing_queue_track_ && pos_in_order_ < order_.size() ? tracks_[order_[pos_in_order_]] : current_;
  tracks_.clear();
  next_page_url_.clear();
  int idx = -1;
  if (resolve_context(context_uri_)) idx = find_track(cur.uid, cur.uri);
  if (idx < 0) {  // keep what the transfer gave us
    tracks_ = saved;
    next_page_url_ = saved_next;
    idx = find_track(cur.uid, cur.uri);
    SLOGW(TAG, "context refresh failed; keeping %u transferred tracks", (unsigned)tracks_.size());
  }
  if (playing_queue_track_) {
    // current track came from the queue: keep the context position as it was
    rebuild_order(idx);
  } else if (idx >= 0) {
    rebuild_order(idx);
    current_ = tracks_[size_t(idx)];
  }
  next_for_player_.clear();
  put_state(PLAYER_STATE_CHANGED);
}

void Connect::set_active(bool on) {
  if (on) {
    if (active_) return;
    active_ = true;
    active_since_ms_ = unix_ms();
  } else {
    active_ = false;
    active_since_ms_ = 0;
  }
}

void Connect::become_inactive(const char* why) {
  if (!active_) return;
  SLOGI(TAG, "%s; going inactive", why);
  stop_playback();
  put_state(PLAYER_STATE_CHANGED);  // the stopped state, still as the active device
  set_active(false);
  ctx_refresh_ = false;
  ++ctxf_seq_;  // a context fetch in flight no longer applies
  int st = sp_->put_inactive(connection_id_, false);
  if (st != 200 && st != 204) SLOGW(TAG, "put inactive -> HTTP %d", st);
  update_snapshot();
}

// social-connect/v2/session_update (JSON SessionUpdate): when it names this device as the session
// host's active device, adopt its session id (librespot spirc.rs handle_session_update).
void Connect::handle_session_update(const Event& e) {
  std::string js(e.payload.begin(), e.payload.end());
  cJSON* j = cJSON_Parse(js.c_str());
  if (!j) return;
  cJSON* s = cJSON_GetObjectItem(j, "session");
  std::string host = s ? jstr(s, "host_active_device_id") : "";
  if (host.empty() && s) host = jstr(s, "hostActiveDeviceId");
  std::string sid = s ? jstr(s, "session_id") : "";
  if (sid.empty() && s) sid = jstr(s, "sessionId");
  if (host == cfg_.device_id && !sid.empty() && sid != session_id_) {
    SLOGI(TAG, "session update: adopting session id from Spotify");
    session_id_ = sid;
  }
  cJSON_Delete(j);
}

std::string Connect::published_name() const {
  // the user's choice (2026-11-05): a group is "Sinfónica of N", N speakers including this one
  return group_followers_ > 0 ? "Sinfónica of " + std::to_string(group_followers_ + 1) : cfg_.name;
}

void Connect::tick() {
  {  // Sinfónica: group display and follower hiding (see set_group_source)
    int n = group_followers_fn_ ? group_followers_fn_() : 0;
    bool hide = group_following_fn_ && group_following_fn_();
    if (n != group_followers_ || hide != group_hidden_) {
      SLOGI(TAG, "group: %d follower(s)%s", n, hide ? ", following another speaker (hidden)" : "");
      if (hide && !group_hidden_ && active_) become_inactive("following another speaker");
      group_followers_ = n;
      group_hidden_ = hide;
      put_state(NEW_DEVICE);  // device info changed: name, is_group, hidden
    }
  }
  for (auto& t : reannounce_at_ms_) {  // see reannounce_at_ms_
    if (t && now_ms() >= t) {
      t = 0;
      put_state(NEW_DEVICE);
    }
  }
  if (!active_) return;
  if (ctx_refresh_) refresh_context_async();
  // keep the player's gapless "next" in sync with queue/shuffle/repeat changes
  const CtxTrack* nx = peek_next();
  std::string nuri = nx ? nx->uri : std::string();
  if (nuri != next_for_player_) {
    next_for_player_ = nuri;
    player_->set_next(nuri);
  }
  if (!is_playing_ || is_paused_) return;
  // preload the next track ~30 s before the end
  uint32_t dur = duration_ms_, pos = player_->position_ms();
  if (dur && pos + 30000 >= dur) {
    const CtxTrack* n = peek_next();
    if (n && n->uri != preloaded_for_) {
      preloaded_for_ = n->uri;
      player_->preload(n->uri);
    }
  }
}

// ---------------------------------------------------------- state put
static void write_provided(pb::Writer& w, uint32_t field, const CtxTrack& t) {
  w.msg(field, [&](pb::Writer& p) {
    p.str(1, t.uri);
    if (!t.uid.empty()) p.str(2, t.uid);
    for (auto& kv : t.metadata) p.map_entry(3, kv.first, kv.second);
    p.str(6, t.provider);
  });
}

Bytes Connect::build_put_state(int reason) {
  pb::Writer req;
  int64_t now = unix_ms();
  req.msg(2, [&](pb::Writer& dev) {
    dev.msg(1, [&](pb::Writer& di) {
      di.b(1, true);
      di.u64(2, volume_);
      di.str(3, published_name());
      di.msg(4, [&](pb::Writer& cap) {
        cap.b(2, true);   // can_be_player
        cap.b(5, true);   // gaia_eq_connect_id
        cap.b(7, true);   // is_observable
        cap.i64(8, cfg_.volume_steps);
        cap.str(9, "audio/track");
        cap.str(9, "audio/episode");
        cap.b(10, true);  // command_acks
        if (group_hidden_) cap.b(12, true);  // hidden: following another speaker
        cap.b(15, true);  // supports_playlist_v2
        cap.b(16, true);  // is_controllable
        cap.b(19, true);  // supports_transfer_command
        cap.b(20, true);  // supports_command_request
        cap.b(22, true);  // needs_full_player_state
        cap.b(23, true);  // supports_gzip_pushes
        cap.b(25, true);  // supports_set_options_command
        cap.u64(30, 1);   // supported_audio_quality: HIGH (160 kbps)
      });
      di.str(6, LISTENICA_VERSION);
      di.u64(7, uint64_t(cfg_.device_type));
      di.str(9, SPOTIFY_SPIRC_VERSION);
      di.str(10, cfg_.device_id);
      di.str(13, KEYMASTER_CLIENT_ID);
      di.str(14, "listenica");
      di.str(15, "esp32-s3");
      if (group_followers_ > 0) di.b(25, true);  // is_group
    });
    dev.msg(2, [&](pb::Writer& ps) {
      ps.i64(1, now);
      if (active_ && !current_.uri.empty()) {
        ps.str(2, context_uri_);
        if (!context_url_.empty()) ps.str(3, context_url_);
        ps.msg(4, [](pb::Writer&) {});
        if (!play_origin_.empty()) ps.bytes(5, play_origin_);
        size_t ctx_idx = order_.empty() ? 0 : order_[pos_in_order_];
        ps.msg(6, [&](pb::Writer& ix) { ix.u64(1, 0).u64(2, playing_queue_track_ ? 0 : ctx_idx); });
        write_provided(ps, 7, current_);
        ps.str(8, playback_id_);
        // librespot state.rs set_status: paused = playing + paused + buffering, speed 0
        bool paused = is_paused_;
        ps.dbl(9, paused ? 0.0 : 1.0);
        ps.i64(10, int64_t(player_->position_ms()));
        ps.i64(11, int64_t(duration_ms_));
        ps.b(12, is_playing_ || paused);
        ps.b(13, paused);
        ps.b(14, paused || is_buffering_);
        ps.b(15, true);  // is_system_initiated (librespot: always)
        ps.msg(16, [&](pb::Writer& o) { o.b(1, shuffle_).b(2, repeat_ctx_).b(3, repeat_track_); });
        ps.msg(17, [](pb::Writer&) {});
        // prev tracks (a few)
        size_t from = pos_in_order_ > 8 ? pos_in_order_ - 8 : 0;
        for (size_t i = from; i < pos_in_order_ && i < order_.size(); i++) write_provided(ps, 19, tracks_[order_[i]]);
        // next: queue then context (cap at 30)
        size_t n = 0;
        for (auto& q : queue_) {
          if (n++ >= 30) break;
          write_provided(ps, 20, q);
        }
        size_t i = std::min(next_replays_pos_ ? pos_in_order_ : pos_in_order_ + 1, order_.size());
        for (; i < order_.size() && n < 30; i++, n++) write_provided(ps, 20, tracks_[order_[i]]);
        published_ctx_end_ = i;
        for (auto& kv : context_metadata_) ps.map_entry(21, kv.first, kv.second);
        if (!queue_revision_.empty()) ps.str(24, queue_revision_);
      } else {
        // librespot state.rs reset(): an inactive device still sends a default player state
        ps.dbl(9, 1.0);
        ps.b(15, true);
        ps.msg(5, [](pb::Writer&) {});
        ps.msg(16, [](pb::Writer&) {});
        ps.msg(18, [](pb::Writer&) {});
      }
      ps.str(23, session_id_);
    });
  });
  req.u64(3, 2);  // CONNECT_STATE
  req.b(4, active_);
  req.u64(5, uint64_t(reason));
  req.u64(6, ++message_id_);
  if (!last_cmd_device_.empty()) {
    req.str(7, last_cmd_device_);
    req.u64(8, last_cmd_msg_id_);
  }
  if (active_ && active_since_ms_) req.u64(9, uint64_t(active_since_ms_));  // started_playing_at = active since
  req.u64(12, uint64_t(now));
  return req.buf;
}

void Connect::local_command(const std::string& command_json) {
  Event e;
  e.kind = Event::DealerRequest;
  e.json = "{\"message_id\":0,\"sent_by_device_id\":\"local\",\"command\":" + command_json + "}";
  push(std::move(e));
}

void Connect::local_volume(uint16_t v) {
  Event e;
  e.kind = Event::DealerMessage;
  e.uri = "hm://connect-state/v1/connect/volume";
  pb::Writer w;
  w.u64(1, v);
  e.payload = w.buf;
  push(std::move(e));
}

std::string Connect::preset_context(const std::string& preset_uri) {
  return preset_uri == "liked" ? "spotify:user:" + sp_->username() + ":collection" : preset_uri;
}

void Connect::local_activate() { local_activate(Preset{}); }

void Connect::local_activate(const Preset& preset) {
  {
    std::lock_guard<std::mutex> l(snap_mu_);
    pending_preset_ = preset;
  }
  Event e;
  e.kind = Event::LocalActivate;
  e.uri = preset.uri;
  push(std::move(e));
}

void Connect::local_deactivate() {
  Event e;
  e.kind = Event::LocalDeactivate;
  push(std::move(e));
}

ConnectSnapshot Connect::snapshot() {
  std::lock_guard<std::mutex> l(snap_mu_);
  return snap_;
}

void Connect::update_snapshot() {
  {
    std::lock_guard<std::mutex> l(snap_mu_);
    snap_.active = active_;
    snap_.is_playing = is_playing_;
    snap_.is_paused = is_paused_;
    snap_.is_buffering = is_buffering_;
    snap_.track_uri = active_ ? current_.uri : std::string();
    snap_.context_uri = active_ ? context_uri_ : std::string();
    snap_.position_ms = player_->position_ms();
    snap_.duration_ms = duration_ms_;
    snap_.timestamp_ms = unix_ms();
    snap_.volume = volume_;
    snap_.others_online = others_online_;
    snap_.shuffle = shuffle_;
    snap_.repeat_ctx = repeat_ctx_;
    snap_.repeat_track = repeat_track_;
    snap_.prev.clear();
    snap_.next.clear();
    size_t from = pos_in_order_ > 10 ? pos_in_order_ - 10 : 0;
    for (size_t i = pos_in_order_; i-- > from && i < order_.size();) snap_.prev.push_back(tracks_[order_[i]]);
    for (auto& q : queue_) snap_.next.push_back(q);
    for (size_t i = pos_in_order_ + 1; i < order_.size() && snap_.next.size() < 50; i++) snap_.next.push_back(tracks_[order_[i]]);
  }
  if (on_state_) on_state_();
}

void Connect::note_cluster(const pb::Field& cluster) {
  int others = 0;
  pb::Reader(cluster).each(4, [&](const pb::Field& ent) {
    std::string id = pb::Reader(ent).str_of(1);
    // the account's other online devices; hidden observers (hobs_) don't count
    if (!id.empty() && id != cfg_.device_id && id.rfind("hobs_", 0) != 0) others++;
  });
  if (others != others_online_.load()) SLOGI(TAG, "cluster: %d other device(s) of this account online", others);
  others_online_ = others;
  cluster_active_ = pb::Reader(cluster).str_of(2);  // Cluster.active_device_id
  pb::Field ps;
  if (pb::Reader(cluster).find(3, ps)) cluster_state_ms_ = int64_t(pb::Reader(ps).varint_of(1));  // PlayerState.timestamp
  cluster_seq_++;
}

int Connect::query_others_online(uint32_t timeout_ms) {
  uint32_t seq0 = cluster_seq_;
  Event e;
  e.kind = Event::RefreshCluster;
  push(std::move(e));
  for (uint64_t t0 = now_ms(); now_ms() - t0 < timeout_ms; sleep_ms(50))
    if (cluster_seq_ != seq0) break;
  return others_online_;
}

void Connect::put_state(int reason) {
  update_snapshot();
  if (connection_id_.empty()) return;
  Bytes body = build_put_state(reason);
  Bytes reply;
  int st = sp_->put_connect_state(connection_id_, body, &reply);
  if (st == 200 && !reply.empty()) {
    // Spotify answers with the whole Cluster: the account's devices right now. The
    // board gets no cluster pushes over the dealer (seen 2026-11-02), so this is
    // where "is the owner's phone online" comes from.
    pb::Field cl;
    cl.data = reply.data();
    cl.len = reply.size();
    note_cluster(cl);
  }
  state_puts_++;
  last_put_ms_ = now_ms();
  if (st != 200) {
    state_put_failures_++;
    SLOGW(TAG, "put state (reason %d) -> HTTP %d", reason, st);
  } else {
    SLOGD(TAG, "put state (reason %d) ok", reason);
  }
}

}  // namespace listenica
