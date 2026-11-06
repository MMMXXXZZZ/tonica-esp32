// Spotify access-point session: DH handshake, Shannon-framed packets,
// login, keep-alive and audio-key requests.
#pragma once
#include <atomic>
#include <condition_variable>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include "listenica/net.h"
#include "listenica/shannon.h"

namespace listenica {

struct Credentials {
  std::string username;
  uint32_t auth_type = 1;  // AUTHENTICATION_STORED_SPOTIFY_CREDENTIALS
  Bytes auth_data;
};

enum : uint32_t { AUTH_USER_PASS = 0, AUTH_STORED = 1, AUTH_TOKEN = 3 };

struct ApResolve {
  std::vector<std::string> accesspoint, dealer, spclient;
};
bool apresolve(ApResolve& out);

// Audio key pacing, as in Spotifast's librespot fork (core/src/audio_key.rs, PR #1844): one
// request at a time, at least min_gap apart; after a refusal no key is asked for backoff[0],
// then [1], then [2] (for each further refusal in a row), until a key arrives. A needed key is
// asked again after each wait, up to `retries` times. An optional one (prefetch) is not sent
// during a back-off. Refusals come in bursts that asking again at once prolongs (librespot#1649).
class KeyPacer {
 public:
  enum Result { Key = 1, Refused = 0, Failed = -1 };  // Failed: timeout, dead link, or skipped
  KeyPacer(uint32_t min_gap_ms = 200, uint32_t b0 = 3000, uint32_t b1 = 5000, uint32_t b2 = 10000, int retries = 3)
      : gap_(min_gap_ms), backoff_{b0, b1, b2}, retries_(retries) {}
  // ask(): send one request and wait for its answer. alive(): give up waiting when false.
  Result request(bool optional, const std::function<Result()>& ask, const std::function<bool()>& alive);
  bool backing_off() const;

 private:
  Result ask_once(bool wait, const std::function<Result()>& ask, const std::function<bool()>& alive);
  uint32_t gap_, backoff_[3];
  int retries_;
  std::mutex turn_;            // held for a whole request: one in flight
  uint64_t last_sent_ms_ = 0;  // guarded by turn_
  mutable std::mutex mu_;
  int refusals_ = 0;           // in a row, since the last key that arrived
  uint64_t until_ms_ = 0;      // no key is asked for before this
};

class ApSession {
 public:
  explicit ApSession(std::string device_id) : device_id_(std::move(device_id)) {}
  ~ApSession();

  // Connect + handshake + login. On success `reusable` holds credentials to
  // persist (they replace whatever was used to log in).
  bool connect_and_login(const std::string& ap_hostport, const Credentials& in, Credentials& reusable,
                         std::string* err = nullptr);
  void start();  // background packet reader
  void stop();
  bool alive() const { return alive_; }

  // Audio keys, paced as in Spotifast's librespot fork (core/src/audio_key.rs, PR #1844):
  // one request at a time, at least 200 ms apart; after a refusal the whole session asks for no
  // key for 3 s, then 5, then 10 for each further refusal in a row (until a key arrives).
  // A needed key is asked again after each wait, up to 3 times, before the refusal is
  // reported. An optional one (prefetch) fails at once during a back-off without asking.
  // refused: Spotify answered with a key error (not a timeout, a dead link or a back-off skip).
  bool request_audio_key(const Bytes& track_gid, const Bytes& file_id, uint8_t key_out[16],
                         uint32_t timeout_ms = 3000, bool* refused = nullptr, bool optional = false);
  bool key_backing_off() const { return key_pacer_.backing_off(); }

  std::string country() const;
  int autoplay() const { return autoplay_; }  // account setting: 1 on, 0 off, -1 unknown
  const std::string& username() const { return username_; }
  uint64_t last_ping_ms() const { return last_ping_ms_; }
  // Once !alive(): why the session ended, how long it lasted and the keep-alive state,
  // e.g. "closed by the server (FIN) after 120 s; last ping 60 s ago, 1 pong, 1 ack".
  std::string end_summary() const;
  // Keep-alive counters for /status: "pings/pongs/acks".
  std::string keepalive_stats() const;

 private:
  std::string device_id_, username_;
  std::unique_ptr<Stream> sock_;
  std::unique_ptr<Shannon> enc_, dec_;
  uint32_t send_nonce_ = 0, recv_nonce_ = 0;
  std::mutex send_mu_;
  mutable std::mutex mu_;
  std::condition_variable cv_;
  std::string country_;
  std::atomic<int> autoplay_{-1};
  std::atomic<bool> alive_{false}, stop_{false}, reader_done_{true};
  std::atomic<uint64_t> last_ping_ms_{0};
  // Keep-alive, as librespot does it: the server pings, the client answers 60 s later,
  // the server acknowledges (PongAck) and pings again ~60 s after that.
  enum class Ka { ExpectingPing, PendingPong, ExpectingPongAck };
  Ka ka_ = Ka::ExpectingPing;
  uint64_t ka_deadline_ms_ = 0;  // ExpectingPing/PongAck: give up; PendingPong: send the pong
  uint64_t login_ms_ = 0;
  std::atomic<uint32_t> pings_{0}, pongs_{0}, acks_{0};
  std::string end_cause_;  // guarded by mu_
  uint32_t key_seq_ = 0;
  struct KeyWait {
    bool done = false, ok = false;
    uint8_t key[16];
  };
  std::map<uint32_t, KeyWait*> key_waits_;
  KeyPacer key_pacer_;
  int ask_audio_key(const Bytes& gid, const Bytes& file_id, uint8_t key_out[16], uint32_t timeout_ms);

  bool handshake(std::string* err);
  bool send_packet(uint8_t cmd, const Bytes& payload);
  // 1 = packet, 0 = nothing arrived within timeout_ms, -1 = link unusable (`why` says how)
  int recv_packet(uint8_t& cmd, Bytes& payload, uint32_t timeout_ms, std::string* why = nullptr);
  static void reader_entry(void* self);
  void reader_loop();
  void handle_packet(uint8_t cmd, const Bytes& payload);  // reader thread only
};

}  // namespace listenica
