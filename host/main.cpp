// listenica_host: runs the portable core on a PC for testing.
//
//   listenica_host login  --token <oauth-access-token>        -> secrets/credentials.json
//   listenica_host stream --uri spotify:track:... --out x.pcm  (decode one track to raw s16le)
//   listenica_host connect --name "Listenica Host" [--pcm x.pcm] [--realtime] [--seconds N]
//
// Options: --secrets <dir> (default ../secrets relative to cwd or ./secrets),
//          --ca <pem> (CA bundle), -v (debug logs)
#include <atomic>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <thread>
#include "cJSON.h"
#include <cstdlib>
#include <new>

// Allocation counter (host only): how often the core allocates while playing.
static std::atomic<uint64_t> g_allocs{0}, g_alloc_bytes{0};
void* operator new(size_t n) {
  g_allocs++;
  g_alloc_bytes += n;
  if (void* p = std::malloc(n ? n : 1)) return p;
  throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, size_t) noexcept { std::free(p); }
#include "listenica/group.h"
#include "listenica/sinfonica.h"
#include "desktop.h"
#include "lan_discovery.h"
#include "speaker.h"
#include "listenica/ap_session.h"
#include "listenica/connect.h"
#include "listenica/crypto.h"
#include "listenica/device_signin.h"
#include "listenica/net.h"
#include "listenica/player.h"
#include "listenica/spclient.h"

using namespace listenica;
static const char* TAG = "host";

static std::map<std::string, std::string> g_opt;
static bool has(const char* k) { return g_opt.count(k) > 0; }
static std::string opt(const char* k, const std::string& d = "") { return has(k) ? g_opt[k] : d; }

static std::string read_file(const std::string& p) {
  std::ifstream f(p, std::ios::binary);
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

// Where the login and device id live: --secrets <dir>; else ./secrets when it exists (development);
// else %LOCALAPPDATA%\Listenica (created). Saving into a missing folder failed silently before.
static std::string secrets_dir() {
  static std::string dir;
  if (!dir.empty()) return dir;
  if (has("secrets")) {
    dir = opt("secrets");
  } else if (std::filesystem::is_directory("secrets")) {
    dir = "secrets";
  } else {
    dir = app_data_dir();
  }
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  return dir;
}

static std::string device_id() {
  std::string p = secrets_dir() + "/device_id";
  std::string id = read_file(p);
  while (!id.empty() && isspace((unsigned char)id.back())) id.pop_back();
  if (id.size() != 40) {
    uint8_t r[20];
    random_bytes(r, 20);
    id = hex(r, 20);
    std::ofstream(p) << id;
  }
  return id;
}

static bool load_creds(Credentials& c) {
  std::string s = read_file(secrets_dir() + "/credentials.json");
  cJSON* j = cJSON_Parse(s.c_str());
  if (!j) return false;
  cJSON* u = cJSON_GetObjectItem(j, "username");
  cJSON* t = cJSON_GetObjectItem(j, "auth_type");
  cJSON* d = cJSON_GetObjectItem(j, "auth_data");
  bool ok = cJSON_IsString(u) && cJSON_IsNumber(t) && cJSON_IsString(d);
  if (ok) {
    c.username = u->valuestring;
    c.auth_type = uint32_t(t->valuedouble);
    c.auth_data = unhex(d->valuestring);
  }
  cJSON_Delete(j);
  return ok;
}

static void save_creds(const Credentials& c) {
  cJSON* j = cJSON_CreateObject();
  cJSON_AddStringToObject(j, "username", c.username.c_str());
  cJSON_AddNumberToObject(j, "auth_type", c.auth_type);
  cJSON_AddStringToObject(j, "auth_data", hex(c.auth_data).c_str());
  char* s = cJSON_Print(j);
  std::ofstream(secrets_dir() + "/credentials.json") << s;
  cJSON_free(s);
  cJSON_Delete(j);
}

// Writes PCM to a file; optionally paced to real time like a DAC would be.
class FileSink : public AudioSink {
 public:
  FileSink(const std::string& path, bool realtime) : realtime_(realtime) {
    if (!path.empty()) f_ = fopen(path.c_str(), "wb");
  }
  ~FileSink() override {
    if (f_) fclose(f_);
  }
  void write(const int16_t* frames, size_t n) override {
    if (f_) fwrite(frames, 4, n, f_);
    frames_ += n;
    if (realtime_) {
      if (!t0_) t0_ = now_ms();
      uint64_t due = t0_ + paced_ * 1000 / 44100;
      paced_ += n;
      uint64_t now = now_ms();
      if (due + 200 < now) {  // DAC ran dry (underrun): restart the clock like real hardware
        t0_ = now;
        paced_ = n;
      } else if (due > now + 300) {
        sleep_ms(uint32_t(due - now - 300));  // keep ~300 ms "in the DAC"
      }
    }
  }
  void flush() override { reset_clock(); }
  void set_paused(bool p) override {
    if (!p) reset_clock();
  }
  size_t latency_frames() override { return 0; }
  uint64_t frames() const { return frames_; }

 private:
  void reset_clock() {
    t0_ = 0;
    paced_ = 0;
  }
  FILE* f_ = nullptr;
  bool realtime_;
  uint64_t t0_ = 0, paced_ = 0, frames_ = 0;
};

// Spotify sign-in with a code: shown here, confirmed in a browser (opened for you on Windows).
static bool signin_interactive(Credentials& out) {
  SigninEvents ev;
  ev.code = [](const std::string& code, const std::string& url, int expires_s) {
    printf(
        "\n"
        "  Sign in to Spotify\n"
        "  -------------------\n"
        "  1. Open %s\n"
        "     (on this computer it opens by itself; any phone or computer works too)\n"
        "  2. Check that the code shown there is  %s  and confirm.\n"
        "  The code works for %d minutes; a new one appears if it runs out.\n"
        "\n",
        url.c_str(), code.c_str(), expires_s / 60);
    fflush(stdout);
    open_url(url);
  };
  ev.state = [](const std::string& st, const std::string& msg) {
    if (st == "error") printf("  %s\n", msg.c_str());
    else if (st == "signing_in") printf("  Confirmed. Signing in...\n");
    fflush(stdout);
  };
  if (!device_signin(ev, out)) return false;
  save_creds(out);
  printf("  Signed in. The login is kept in %s, so this is needed only once.\n\n", secrets_dir().c_str());
  fflush(stdout);
  return true;
}

static int cmd_login() {
  if (!has("token")) {  // the normal way: a code confirmed in the browser
    Credentials c;
    return signin_interactive(c) ? 0 : 1;
  }
  ApResolve ar;
  if (!apresolve(ar)) {
    SLOGE(TAG, "apresolve failed");
    return 1;
  }
  ApSession ap(device_id());
  Credentials in, out;
  in.auth_type = AUTH_TOKEN;
  std::string tok = opt("token");
  in.auth_data.assign(tok.begin(), tok.end());
  std::string err;
  if (!ap.connect_and_login(ar.accesspoint[0], in, out, &err)) {
    SLOGE(TAG, "login failed: %s", err.c_str());
    return 1;
  }
  save_creds(out);
  printf("credentials saved for %s\n", out.username.c_str());
  return 0;
}

struct Session {
  ApResolve ar;
  std::unique_ptr<ApSession> ap;
  std::unique_ptr<SpClient> sp;
  Credentials creds;
  bool open() {
    if (!load_creds(creds) && !signin_interactive(creds)) return false;  // first run: sign in
    if (!apresolve(ar)) return false;
    ap.reset(new ApSession(device_id()));
    Credentials re;
    std::string err;
    if (!ap->connect_and_login(ar.accesspoint[0], creds, re, &err)) {
      SLOGE(TAG, "AP login failed: %s", err.c_str());
      return false;
    }
    if (!re.auth_data.empty()) {
      creds = re;
      save_creds(re);
    }
    ap->start();
    sp.reset(new SpClient(device_id(), ar.spclient[0]));
    sp->set_credentials(creds);
    for (int i = 0; i < 20 && ap->country().empty(); i++) sleep_ms(100);
    if (!ap->country().empty()) sp->set_country(ap->country());
    return true;
  }
};

static int cmd_stream() {
  Session s;
  if (!s.open()) return 1;
  FileSink sink(opt("out", "out.pcm"), has("realtime"));
  PlayerConfig cfg;
  cfg.pace_decoder = has("realtime");
  if (has("no-head")) cfg.head_url.clear();
  if (has("format")) cfg.preferred_format = atoi(opt("format").c_str());
  Player player(s.ap.get(), s.sp.get(), &sink, cfg);
  std::atomic<int> done{0};
  player.set_event_handler([&](PlayerEvent e, const std::string&) {
    if (e == PlayerEvent::EndOfTrack) done = 1;
    if (e == PlayerEvent::LoadFailed) done = 2;
  });
  uint64_t t0 = now_ms();
  uint32_t seek = uint32_t(atoi(opt("seek", "0").c_str()));
  player.load(opt("uri", "spotify:track:4cOdK2wGLETKBW3PvgPWqT"), seek, true);
  int limit = atoi(opt("seconds", "0").c_str());
  while (!done) {
    sleep_ms(100);
    if (limit && now_ms() - t0 > uint64_t(limit) * 1000) break;
  }
  auto st = player.stats();
  auto m = player.current_meta();
  printf("RESULT %s\n", done == 1 ? "end_of_track" : done == 2 ? "load_failed" : "time_limit");
  printf("track=\"%s\" artist=\"%s\" duration_ms=%u\n", m.name.c_str(), m.artist.c_str(), m.duration_ms);
  printf("frames=%llu (%.2fs) bytes_fetched=%llu first_audio_ms=%u underruns=%u reconnects=%u wall=%.1fs\n",
         (unsigned long long)sink.frames(), sink.frames() / 44100.0, (unsigned long long)st.bytes_fetched,
         st.last_load_ms, st.underruns, st.cdn_reconnects, (now_ms() - t0) / 1000.0);
  s.ap->stop();
  return done == 1 ? 0 : 1;
}

// Shown when the output delay isn't given. Measured 2026-11-05 by ear with a Tónica, a Windows PC
// (built-in output) and a MacBook (built-in speakers): all three in sync with no extra delay.
// (The former 140 ms Windows default was inferred while the board misreported its own delay.)
static void print_latency_notice(const char* flag) {
  printf(
      "\n"
      "  Output delay: measured from what this computer reports for its audio output.\n"
      "  Bluetooth headphones and speakers add delay it may not see. If this computer sounds late\n"
      "  against the other speakers, type  delay +20  in this window (repeat until in step; delay -20\n"
      "  goes back), then start with that value next time, e.g. %s 120\n"
      "\n",
      flag);
  fflush(stdout);
}

static int cmd_connect() {
  Session s;
  if (!s.open()) return 1;
  FileSink file_sink(opt("pcm"), has("realtime") || !has("fast"));
  AudioSink* out = &file_sink;
  // --speaker plays through the computer (speaker.h). out_latency_ms: output delay the OS doesn't
  // report (default 0; tune live with `delay`), used both leading and following.
  // Users run plain `connect`: it plays through the computer and joins Sinfonica unless told otherwise
  // (--pcm <file> writes to a file instead, for tests; --no-group stays out of groups).
  const bool want_speaker = has("speaker") || !has("pcm");
  std::unique_ptr<HostSpeaker> speaker;
  if (want_speaker) {
    if (!has("out_latency_ms")) print_latency_notice("--out_latency_ms");
    speaker = open_speaker(uint32_t(atoi(opt("out_latency_ms", std::to_string(speaker_default_extra_ms())).c_str())));
    if (speaker) out = speaker.get();
    else SLOGE(TAG, "no audio output available on this computer; playing silently");
  }
  PlayerConfig pcfg;
  Player player(s.ap.get(), s.sp.get(), out, pcfg);
  ConnectConfig cc;
  cc.name = opt("name", "Listenica Host");
  cc.device_id = device_id();
  cc.dealer_hostport = s.ar.dealer.empty() ? "dealer.spotify.com:443" : s.ar.dealer[0];
  Connect connect(cc, s.sp.get(), &player);
  // --group: a full Sinfonica member, like a Tonica (announced on the LAN, leads, follows when
  // invited); type `group`, `follow <ip>`, `leave` or `status` in this console.
  std::unique_ptr<Sinfonica> sinf;
  if (has("group") || (want_speaker && !has("no-group"))) {
    Sinfonica::Config sc;
    sc.device_id = cc.device_id;
    sc.follow_offset_us = 0;  // the speaker sink's latency already includes out_latency_ms
    Sinfonica::Hooks sh;
    sh.playing = [&] {
      ConnectSnapshot st = connect.snapshot();
      return st.active && st.is_playing && !st.is_paused;
    };
    sh.stop_local = [&] { connect.local_deactivate(); };
    sh.discover = [] { return lan_discover(); };
    sh.control = [&](const std::string& from, const std::string& c) {
      SLOGI(TAG, "follower %.8s pressed: %s", from.c_str(), c.c_str());
      if (!connect.active()) return;
      ConnectSnapshot st = connect.snapshot();
      if (c == "toggle") connect.local_command(st.is_playing && !st.is_paused ? "{\"endpoint\":\"pause\"}" : "{\"endpoint\":\"resume\"}");
      else if (c == "next") connect.local_command("{\"endpoint\":\"skip_next\"}");
      else if (c.rfind("prev ", 0) == 0) connect.local_command("{\"endpoint\":\"skip_prev\",\"local_pressed_ago_ms\":" + c.substr(5) + "}");
      else if (c == "unstable") SLOGW(TAG, "follower %.8s: its link to this PC keeps dropping out", from.c_str());
      else if (c.rfind("vol ", 0) == 0) connect.local_volume(uint16_t(std::min<unsigned long>(strtoul(c.c_str() + 4, nullptr, 10), 65535)));
    };
    Sinfonica* sp = nullptr;
    sh.role_changed = [&sp] {
      if (!sp) return;
      Sinfonica::Role r = sp->role();
      SLOGI(TAG, "Sinfonica: %s", r == Sinfonica::Role::Leader ? "leading" : r == Sinfonica::Role::Follower ? ("following " + sp->leader()).c_str() : "alone");
    };
    sinf = std::make_unique<Sinfonica>(&player, out, sc, std::move(sh));
    sp = sinf.get();
    if (sinf->start()) {
      lan_announce(cc.name, 7979);
      connect.set_group_source([&] { return sinf->followers(); }, [&] { return sinf->following(); });
      std::thread([&] {  // console
        char line[256];
        while (fgets(line, sizeof line, stdin)) {
          std::string l(line);
          while (!l.empty() && (l.back() == '\n' || l.back() == '\r' || l.back() == ' ')) l.pop_back();
          if (l == "group") {
            int r = sinf->gesture();
            for (int i = 0; r == 0 && i < 100 && sinf->gesture_state() == 0; i++) sleep_ms(100);
            printf(r > 0 ? "OK\n" : (r == 0 && sinf->gesture_state() > 0) ? "OK\n" : "nothing to do: no other speaker answered\n");
          } else if (l.rfind("follow ", 0) == 0) {
            printf(sinf->follow(l.substr(7)) ? "OK\n" : "already following\n");
          } else if (l == "leave") {
            sinf->leave();
            printf("OK\n");
          } else if (l.rfind("delay", 0) == 0) {  // tune this computer's output delay by ear, live
            if (!speaker) {
              printf("no speaker output\n");
            } else {
              std::string v = l.size() > 6 ? l.substr(6) : "";
              int cur = int(speaker->extra_ms()), n = cur;
              if (!v.empty() && (v[0] == '+' || v[0] == '-')) n = cur + atoi(v.c_str());
              else if (!v.empty()) n = atoi(v.c_str());
              if (n < 0) n = 0;
              speaker->set_extra_ms(uint32_t(n));
              printf("output delay %d ms (start with --out_latency_ms %d to keep it). Raise it if this computer "
                     "sounds late against the others, lower it if early.\n", n, n);
            }
          } else if (l == "status") {
            Sinfonica::Role r = sinf->role();
            printf("%s, %d follower(s)\n", r == Sinfonica::Role::Leader ? "leading" : r == Sinfonica::Role::Follower ? "following" : "alone", sinf->followers());
          } else if (sinf->following() && (l == "toggle" || l == "next" || l == "prev" || l == "like" || l == "whats" ||
                                           l.rfind("vol ", 0) == 0)) {
            // a follower's gestures, as a Tonica's button sends them to the leader
            printf(sinf->control(l == "prev" ? "prev 10000" : l) ? "OK\n" : "not connected\n");
          } else if (!l.empty()) {
            printf("commands: group (form/join/leave/dissolve), follow <ip>, leave, status, delay [<ms>|+<ms>|-<ms>]; while following: toggle, next, prev, like, whats, vol <0-65535>\n");
          }
          fflush(stdout);
        }
      }).detach();
    }
  }
  connect.start();
  int limit = atoi(opt("seconds", "0").c_str());
  uint64_t t0 = now_ms(), last = 0;
  while (!limit || now_ms() - t0 < uint64_t(limit) * 1000) {
    sleep_ms(200);
    if (now_ms() - last > 5000) {
      last = now_ms();
      auto st = player.stats();
      auto m = player.current_meta();
      SLOGI(TAG, "status: %s '%s' pos=%u/%u vol=%u rms=%.1fdB underruns=%u frames=%llu", player.is_playing() ? "PLAY" : "IDLE",
            m.name.c_str(), player.position_ms(), m.duration_ms, connect.volume(), st.rms_dbfs_1s, st.underruns,
            (unsigned long long)file_sink.frames());
    }
    if (!s.ap->alive()) {
      SLOGW(TAG, "AP session lost, reconnecting");
      Credentials re;
      std::string err;
      auto ap2 = std::make_unique<ApSession>(device_id());
      if (ap2->connect_and_login(s.ar.accesspoint[0], s.creds, re, &err)) {
        ap2->start();
        player.set_session(ap2.get());
        s.ap.swap(ap2);
        // old session object kept alive until replaced to avoid dangling use
        ap2.reset();
      } else {
        sleep_ms(5000);
      }
    }
  }
  connect.stop();
  return 0;
}

// Live checks that need no Spotify account: TLS + apresolve, AP handshake with
// bogus credentials (expects BadCredentials = 12), and a client token.
static int cmd_probe() {
  int fails = 0;
  ApResolve ar;
  bool ok = apresolve(ar);
  printf("apresolve: %s (%u APs, %u dealers, %u spclients)\n", ok ? "ok" : "FAIL", (unsigned)ar.accesspoint.size(),
         (unsigned)ar.dealer.size(), (unsigned)ar.spclient.size());
  if (!ok) return 1;
  ApSession ap(device_id());
  Credentials bad, out;
  bad.username = "nobody";
  bad.auth_type = AUTH_STORED;
  bad.auth_data = Bytes{1, 2, 3};
  std::string err;
  ok = ap.connect_and_login(ar.accesspoint[0], bad, out, &err);
  bool expected = !ok && err.find("error code 12") != std::string::npos;
  printf("ap handshake + bogus login: %s (%s)\n", expected ? "ok" : "FAIL", err.c_str());
  fails += !expected;
  SpClient sp(device_id(), ar.spclient[0]);
  std::string ct;
  ok = sp.client_token(ct);
  printf("client token: %s (%u chars)\n", ok ? "ok" : "FAIL", (unsigned)ct.size());
  fails += !ok;
  printf(fails ? "PROBE FAILED\n" : "PROBE OK\n");
  return fails ? 1 : 0;
}

// Offline player test against tools/fake_cdn.py: no Spotify session needed.
static int cmd_playfile() {
  FileSink sink(opt("out", "out/playfile.pcm"), has("realtime"));
  PlayerConfig pcfg0;
  pcfg0.pace_decoder = has("realtime");  // fast offline runs decode flat out
  Player player(nullptr, nullptr, &sink, pcfg0);
  std::atomic<int> done{0};
  std::atomic<int> corrected{0};
  player.set_event_handler([&](PlayerEvent e, const std::string&) {
    if (e == PlayerEvent::EndOfTrack) done = 1;
    if (e == PlayerEvent::LoadFailed) done = 2;
    if (e == PlayerEvent::PositionCorrected) corrected++;
  });
  TrackMeta m;
  m.name = "fake";
  m.duration_ms = uint32_t(atoi(opt("duration", "40000").c_str()));
  Bytes key = unhex(opt("key"));
  if (key.size() != 16) {
    fprintf(stderr, "--key must be 32 hex chars\n");
    return 2;
  }
  std::string uri = "spotify:track:fake";
  player.inject_resolved(uri, m, key.data(), {opt("url")});
  uint32_t seek = uint32_t(atoi(opt("seek", "0").c_str()));
  uint64_t t0 = now_ms();
  player.load(uri, seek, true);
  std::atomic<int> track_changes{0};
  if (has("next-url")) {
    // gapless: second track queued while the first plays
    sleep_ms(300);
    TrackMeta m2;
    m2.name = "fake-next";
    m2.duration_ms = uint32_t(atoi(opt("next-duration", "6000").c_str()));
    Bytes key2 = unhex(opt("next-key"));
    player.inject_resolved("spotify:track:fakenext", m2, key2.data(), {opt("next-url")});
    player.set_next("spotify:track:fakenext");
    player.set_event_handler([&](PlayerEvent e, const std::string&) {
      if (e == PlayerEvent::EndOfTrack) done = 1;
      if (e == PlayerEvent::LoadFailed) done = 2;
      if (e == PlayerEvent::PositionCorrected) corrected++;
      if (e == PlayerEvent::TrackChanged) track_changes++;
    });
  }
  uint32_t pause_at = uint32_t(atoi(opt("pause-at", "0").c_str()));
  uint32_t midseek_at = uint32_t(atoi(opt("midseek-at", "0").c_str()));
  uint32_t midseek_to = uint32_t(atoi(opt("midseek-to", "0").c_str()));
  bool paused_once = false, seeked_once = false;
  uint32_t pos_after_seek = 0;
  while (!done && now_ms() - t0 < 120000) {
    sleep_ms(20);
    uint32_t el = uint32_t(now_ms() - t0);
    if (pause_at && !paused_once && el >= pause_at) {
      paused_once = true;
      player.pause();
      sleep_ms(400);  // the pause fades out first (~150 ms of audio still plays)
      uint32_t p1 = player.position_ms();
      sleep_ms(1500);
      uint32_t p2 = player.position_ms();
      printf("pause: pos %u -> %u while paused 1.5s\n", p1, p2);
      player.play();
    }
    if (midseek_at && !seeked_once && el >= midseek_at) {
      seeked_once = true;
      player.seek(midseek_to);
      sleep_ms(1000);
      pos_after_seek = player.position_ms();
      printf("midseek: requested %u, position after 1s = %u\n", midseek_to, pos_after_seek);
    }
  }
  auto st = player.stats();
  printf("ALLOCS %llu (%llu KB)\n", (unsigned long long)g_allocs.load(), (unsigned long long)(g_alloc_bytes.load() / 1024));
  printf("RESULT %s frames=%llu (%.2fs) first_audio_ms=%u reconnects=%u underruns=%u corrected=%d track_changes=%d final_pos=%u wall=%.1fs\n",
         done == 1 ? "end_of_track" : done == 2 ? "load_failed" : "timeout", (unsigned long long)sink.frames(),
         sink.frames() / 44100.0, st.last_load_ms, st.cdn_reconnects, st.underruns, corrected.load(), track_changes.load(),
         player.position_ms(), (now_ms() - t0) / 1000.0);
  return done == 1 ? 0 : 1;
}

// Connect device against tools/mock_spotify.py (plain http/ws, no account).
static int cmd_mockconnect() {
  std::string mock = opt("mock", "http://127.0.0.1:9900");
  SpClient sp(device_id(), "127.0.0.1:9900");
  sp.set_endpoints(mock, mock + "/v3/login", mock + "/v1/clienttoken");
  Credentials c;
  c.username = "mockuser";
  c.auth_data = Bytes{1, 2, 3, 4};
  sp.set_credentials(c);
  FileSink sink(opt("pcm"), true);
  Player player(nullptr, &sp, &sink, PlayerConfig{});
  HttpClient http;
  player.set_resolve_override([&](const std::string& uri, TrackMeta& m, uint8_t key[16], std::vector<std::string>& urls) {
    auto r = http.request("GET", mock + "/mock/resolve?uri=" + url_encode(uri));
    if (r.status != 200) return false;
    cJSON* j = cJSON_Parse(r.text().c_str());
    if (!j) return false;
    Bytes k = unhex(cJSON_GetObjectItem(j, "key")->valuestring);
    memcpy(key, k.data(), 16);
    urls.push_back(cJSON_GetObjectItem(j, "url")->valuestring);
    m.uri = uri;
    m.name = cJSON_GetObjectItem(j, "name")->valuestring;
    m.duration_ms = uint32_t(cJSON_GetObjectItem(j, "duration_ms")->valuedouble);
    m.ok = true;
    cJSON_Delete(j);
    return true;
  });
  ConnectConfig cc;
  cc.name = opt("name", "Tónica Mock");
  cc.device_id = device_id();
  cc.dealer_url = "ws://127.0.0.1:9900/dealer";
  cc.autoplay_enabled = [] { return true; };  // the mock serves /context-resolve/v1/autoplay
  Connect connect(cc, &sp, &player);
  connect.start();
  int limit = atoi(opt("seconds", "120").c_str());
  uint64_t t0 = now_ms();
  while (now_ms() - t0 < uint64_t(limit) * 1000) {
    sleep_ms(200);
    if (has("until-file") && read_file(opt("until-file")).size()) break;
  }
  connect.stop();
  printf("mockconnect done: cmds=%u puts=%u failures=%u\n", connect.commands_handled(), connect.state_puts(),
         connect.state_put_failures());
  return 0;
}

// Sinfonica follower only (no Spotify login): plays the playing speaker's stream in sync through
// the PC's speakers. A quick test of whether this network carries a group.
//   group_follow [--leader <ip>] [--offset_ms N] [--seconds S]
// leader: found over mDNS when omitted. offset_ms: output delay the OS doesn't report (default 0),
// tuned by ear against the leader.
static int cmd_group_follow() {
  if (!has("offset_ms")) print_latency_notice("--offset_ms");
  std::string leader = opt("leader");
  if (leader.empty()) {  // find the playing speaker on the LAN
    for (const std::string& ip : lan_discover()) {
      GroupStatus st;
      if (group_status(ip, st) && st.playing && !st.following) {
        leader = ip;
        break;
      }
    }
    if (leader.empty()) {
      SLOGE(TAG, "no playing speaker found on the network (or pass --leader <ip>)");
      return 1;
    }
    SLOGI(TAG, "found the playing speaker at %s", leader.c_str());
  }
  GroupClient gc;
  if (!gc.connect(leader, device_id())) return 1;
  std::unique_ptr<AudioSink> sink = open_speaker(0);
  if (!sink) {
    SLOGE(TAG, "no audio output available on this computer");
    return 1;
  }
  static std::atomic<bool> stop{false};
  const int limit = atoi(opt("seconds", "0").c_str());
  if (limit) std::thread([limit] { sleep_ms(uint32_t(limit) * 1000); stop = true; }).detach();
  GroupFollower f(&gc, sink.get(), int64_t(atoi(opt("offset_ms", std::to_string(speaker_default_extra_ms())).c_str())) * 1000);
  f.run(stop);
  gc.close();
  return 0;
}

static void print_usage() {
  printf(
      "Listenica host " LISTENICA_BUILD_VERSION
      ": plays Spotify on this PC, and joins Tonica speakers in Sinfonica (multiroom).\n"
      "\n"
      "Usage: listenica_host <command> [options]\n"
      "\n"
      "Commands:\n"
      "  connect        This PC becomes a Spotify speaker named \"Listenica Host\" and a Sinfonica\n"
      "                 member: pick it in the Spotify app, or join a playing Tonica. The first run\n"
      "                 asks you to sign in to Spotify (once). Then type in this window:\n"
      "                   group            join the playing speaker, or call the others to join this one\n"
      "                                    (again: leave, or end the group)\n"
      "                   status           leading, following or alone\n"
      "                   delay [+|-]<ms>  this computer's output delay, live: tune it by ear\n"
      "                   follow <ip>      follow the speaker at that address\n"
      "                   leave            stop following\n"
      "                 While following: toggle, next, prev, like, whats, vol <0-65535>.\n"
      "  group_follow   Network test, no Spotify account needed: finds the playing Tonica and plays\n"
      "                 along with it in sync. Shows how well this network carries a group.\n"
      "  login          Sign in to Spotify with a code confirmed in your browser.\n"
      "  probe          Checks that this PC can reach Spotify (no account needed).\n"
      "  version        Prints this program's version.\n"
      "\n"
      "Options:\n"
      "  --name \"<name>\"        the name shown in Spotify (connect)\n"
      "  --out_latency_ms <n>   extra output delay in ms (connect; default 0; tune live with `delay`)\n"
      "  --offset_ms <n>        the same, for group_follow\n"
      "                         Raise it if this computer sounds late against the others (Bluetooth\n"
      "                         headphones need much more), lower it if it sounds early.\n"
      "  --leader <ip>          group_follow: the speaker to follow (found automatically otherwise)\n"
      "  --no-group             connect: don't take part in Sinfonica\n"
      "  --seconds <n>          stop after n seconds\n"
      "  -v                     detailed log\n"
      "\n"
      "Examples:\n"
      "  listenica_host connect\n"
      "  listenica_host group_follow --offset_ms 300\n"
      "\n"
      "Developer commands: stream, playfile, mockconnect (see host/main.cpp).\n");
}

int main(int argc, char** argv) {
  if (argc < 2 || std::string(argv[1]) == "help" || std::string(argv[1]) == "--help" || std::string(argv[1]) == "-h") {
    print_usage();
    return argc < 2 ? 2 : 0;
  }
  if (std::string(argv[1]) == "version" || std::string(argv[1]) == "--version") {
    printf("listenica_host %s\n", LISTENICA_BUILD_VERSION);
    return 0;
  }
  std::string cmd = argv[1];
  for (int i = 2; i < argc; i++) {
    std::string a = argv[i];
    if (a == "-v") {
      g_opt["v"] = "1";
    } else if (a.rfind("--", 0) == 0) {
      std::string k = a.substr(2);
      if (i + 1 < argc && std::string(argv[i + 1]).rfind("--", 0) != 0)
        g_opt[k] = argv[++i];
      else
        g_opt[k] = "1";
    }
  }
  set_log_level(has("v") ? Level::Debug : Level::Info);
  set_ca_bundle_path(opt("ca", ""));
  net_init();
  if (cmd == "probe") return cmd_probe();
  if (cmd == "playfile") return cmd_playfile();
  if (cmd == "mockconnect") return cmd_mockconnect();
  if (cmd == "login") return cmd_login();
  if (cmd == "stream") return cmd_stream();
  if (cmd == "connect") return cmd_connect();
  if (cmd == "group_follow") return cmd_group_follow();
  fprintf(stderr, "unknown command %s\n\n", cmd.c_str());
  print_usage();
  return 2;
}
