// Sinfónica prototype (docs/sinfonica-study.md): a leader shares its decoded PCM and play-out
// anchors with followers, which play in sync with it.
//
// Wire format: [type u8][length u32 LE][payload], all integers little endian.
//   follower -> leader  Hello        {kind u8, device id}  0 = data, 1 = time, 2 = one-shot request
//   follower -> leader  Control      {button command}     (time connection) a follower's button
//   any -> speaker      Ask          {op u8}              (one-shot) 1 = status, 2 = follow the asker
//   speaker -> any      Status       {playing u8, following u8, followers u8}
//   leader -> follower  VoiceClip    {id u32, rate u32, target_len u8, target, IMA ADPCM}  a spoken notification
//   leader -> follower  VoiceStart   {id u32, at_us i64}  the clip becomes audible at leader time at_us
//   follower -> leader  Control "unstable"                its audio keeps dropping out (see GroupFollower)
// A voice's target is "" (every follower) or one follower's device id (its Hello).
//   follower -> leader  TimeReq      {t_client i64}       (time connection)
//   leader -> follower  TimeResp     {t_client i64, t_leader i64}
//   leader -> follower  Anchor       {gen u32, pos u64, at_us i64, playing u8, volume u16}
//   leader -> follower  VorbisHeader {id u32, norm_gain f32, Vorbis header packets}
//   leader -> follower  VorbisFrame  {gen u32, pos u64, skip u32, kept u32, header u32, flush u8, end u64, bytes}
//   leader -> follower  VorbisFlush  {}                   frames were skipped: flush the decoder
// The leader forwards the decrypted Vorbis bytes its decoder consumed, one decode call at a time;
// the follower replays them through the same decoder, so its PCM is the leader's, sample for
// sample: a frame's first `skip` samples are dropped and the next `kept` land at stream byte `pos`.
// `end` is where the frame's bytes end in the decoder's input since its setup or last flush: a
// follower that starts mid-stream (joining, or skipping played frames) resyncs on page starts and
// consumes several frames in one call, so outputs are matched to frames by byte offset, not by count.
// `pos` counts bytes of the leader's PCM stream since its last flush (`gen` changes on a flush:
// load, seek, skip). An anchor says: byte `pos` becomes audible at leader time `at_us`.
// Timing follows Snapcast (timestamps, min-RTT clock offset) and Lyrion (play points).
#pragma once
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <deque>
#include <memory>
#include <mutex>
#include <vector>
#include <string>
#include "listenica/audio_sink.h"
#include "listenica/platform.h"

namespace listenica {

namespace group {
enum Msg : uint8_t {
  Hello = 1, TimeReq = 2, TimeResp = 3, Anchor = 5, VorbisHeader = 6, VorbisFrame = 7, VorbisFlush = 8,
  Control = 9, Ask = 10, Status = 11, VoiceClip = 12, VoiceStart = 13
};
constexpr const char* kPort = "7979";
constexpr uint32_t kRate = 44100;
constexpr uint32_t kFrameBytes = 4;
}  // namespace group

// What a speaker reports about itself (Ask op 1).
struct GroupStatus {
  bool playing = false;    // its own player is playing
  bool following = false;  // it follows another speaker
  int followers = 0;       // speakers following it
};
// One-shot requests to the speaker at `host` (its GroupServer). false: no answer.
bool group_status(const std::string& host, GroupStatus& out, uint32_t timeout_ms = 1500);
bool group_invite(const std::string& host, uint32_t timeout_ms = 1500);  // asks it to follow us

class GroupServer {
 public:
  explicit GroupServer(size_t log_bytes = 512 * 1024);  // ~12 s of 320 kbps Vorbis
  ~GroupServer();
  bool start();
  int clients() const { return clients_; }
  // Called on the server's connection threads (PSRAM stacks: no flash I/O in them).
  struct Hooks {
    std::function<GroupStatus()> status;
    std::function<void(const std::string& leader_ip)> invited;  // the asker wants us to follow it
    std::function<void(const std::string& from, const std::string& command)> control;  // a follower's gesture
    std::function<void()> clients_changed;  // a follower joined or left
  };
  void set_hooks(Hooks h) { hooks_ = std::move(h); }  // before start()
  void dismiss_followers();  // drops every follower's connections (dissolves the group)
  // A spoken notification for the followers: `target` "" = all of them, else one device id. The clip
  // goes out at once; voice_start says when it becomes audible (leader clock) and plays it.
  void voice_clip(uint32_t id, const std::string& target, const int16_t* pcm, size_t n, uint32_t rate);
  void voice_start(uint32_t id, const std::string& target, int64_t at_us);
  // Decoder thread: a new decoder's setup (the three Vorbis header packets) and its loudness gain.
  void vorbis_header(float norm_gain, const uint8_t* d, size_t n);
  // Decoder thread: the bytes one decode call consumed; of its output, `skip` samples are dropped
  // and `kept` land at PCM byte `pos` of stream `gen`. flush: the decoder was flushed before it.
  void vorbis_frame(uint32_t gen, uint64_t pos, uint32_t skip, uint32_t kept, bool flush, uint64_t end, const uint8_t* d,
                    size_t n);
  // Output thread: byte `pos` of `gen` becomes audible at leader time `at_us`.
  void anchor(uint32_t gen, uint64_t pos, int64_t at_us, bool playing, uint16_t volume);

 private:
  struct AnchorData {
    uint32_t gen = 0;
    uint64_t pos = 0;
    int64_t at_us = 0;
    bool playing = false;
    uint16_t volume = 0;
    uint32_t seq = 0;
  };
  size_t cap_;
  std::vector<uint8_t> log_;  // serialized records, circular: bytes [log_head_ - log_fill_, log_head_)
  uint64_t log_head_ = 0, log_fill_ = 0;
  struct Hdr {
    uint32_t id;
    std::vector<uint8_t> rec;
  };
  std::vector<Hdr> hdrs_;  // the last few decoder setups, serialized
  uint32_t hdr_id_ = 0;
  std::mutex mu_;
  std::condition_variable cv_;
  AnchorData anchor_;
  int64_t last_anchor_us_ = 0;
  std::atomic<int> clients_{0};
  void* listen_ = nullptr;
  Hooks hooks_;
  std::mutex fds_mu_;
  std::vector<int> fds_;  // followers' sockets, for dismiss_followers
  void append(uint8_t type, const std::vector<uint8_t>& head, const uint8_t* body, size_t body_len);
  void log_copy(uint64_t at, uint8_t* out, size_t n) const;
  static void accept_entry(void* self);
  void accept_loop();
  struct Conn {
    GroupServer* self;
    void* net;
    std::string ip;
  };
  static void conn_entry(void* arg);
  void data_loop(void* net, const std::string& from, const std::string& ip);
  void time_loop(void* net, const std::string& from);
  struct VoiceRec {
    uint32_t seq;
    std::string target;
    std::vector<uint8_t> rec;  // serialized VoiceClip / VoiceStart
  };
  std::deque<VoiceRec> voices_;  // the last few (mu_)
  uint32_t voice_seq_ = 0;
  void push_voice(const std::string& target, std::vector<uint8_t> rec);
};

// Follower side (the host's group_follow): keeps the leader's clock and the latest stream.
class GroupClient {
 public:
  bool connect(const std::string& host, const std::string& device_id = std::string());
  // Leader clock minus ours, from the lowest-RTT sample of the last ~8 s. false until known.
  bool offset_us(int64_t& off, int64_t* rtt = nullptr);
  struct Anchor {
    uint32_t gen = 0;
    uint64_t pos = 0;
    int64_t at_us = 0;
    bool playing = false;
    uint16_t volume = 0;
    int64_t recv_us = 0;
  };
  Anchor anchor();
  // The latest spoken notification from the leader (44.1 kHz mono); at_us 0 until it starts.
  struct Voice {
    uint32_t id = 0;
    std::vector<int16_t> pcm;
    std::atomic<int64_t> at_us{0};
  };
  std::shared_ptr<Voice> voice();
  // Copies `n` bytes of stream `gen` from `pos`; returns how many were available (rest unknown).
  size_t read(uint32_t gen, uint64_t pos, uint8_t* out, size_t n);
  bool alive() const { return alive_; }
  // A button gesture for the leader to run: "toggle", "next", "prev <ms since the first press>",
  // "vol <0..65535>", "like", "whats". Thread-safe.
  bool control(const std::string& command);
  void close();  // stops both connections and waits for their threads

 private:
  void* data_ = nullptr;
  void* time_ = nullptr;
  std::atomic<bool> alive_{false};
  std::atomic<int> threads_{0};
  std::mutex mu_, tsend_mu_;
  Anchor anchor_;
  std::shared_ptr<Voice> voice_;
  uint32_t gen_ = 0;
  // received PCM of gen_, circular: stream bytes [head_ - fill_, head_). The leader sends at most
  // ~2.5 s ahead of what plays, so 6 s is ample.
  static constexpr size_t kCap = 6 * group::kRate * group::kFrameBytes;
  std::vector<uint8_t> buf_;
  uint64_t head_ = 0, fill_ = 0;
  void store_pcm(uint32_t gen, uint64_t pos, const uint8_t* d, size_t len);
  struct Sample {
    int64_t rtt, off;
  };
  std::vector<Sample> samples_;  // newest last
  static void data_entry(void* self);
  static void time_entry(void* self);
  void data_loop();
  void time_loop();
};

// Plays a GroupClient's stream on a sink in sync with the leader. Each block's target position
// comes from the leader's clock (min-RTT offset) and a trimmed mean of its recent anchors; one
// frame is repeated or dropped per block when more than 1 ms off, a jump beyond 30 ms.
// The sink's latency_frames() must be what it has queued; offset_us adds a fixed output delay the
// sink can't report (e.g. the Windows audio path).
class GroupFollower {
 public:
  GroupFollower(GroupClient* gc, AudioSink* sink, int64_t offset_us) : gc_(gc), sink_(sink), offset_us_(offset_us) {}
  // Runs until `stop` or the leader is gone, logging a status line every `log_ms`.
  void run(const std::atomic<bool>& stop, uint32_t log_ms = 2000);

 private:
  GroupClient* gc_;
  AudioSink* sink_;
  int64_t offset_us_;
};

}  // namespace listenica
