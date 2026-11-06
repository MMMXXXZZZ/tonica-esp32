// Track player: resolves a track (metadata, audio key, CDN URLs), streams
// and decrypts the Ogg Vorbis file, decodes it and feeds an AudioSink.
#pragma once
// Diagnostics (decode-speed and level meters, test tone): off on the board, on in the host
// test build (host/CMakeLists.txt), so offline tests keep their measurements.
#ifndef LISTENICA_DIAG
#define LISTENICA_DIAG 0
#endif
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include "listenica/ap_session.h"
#include "listenica/audio_sink.h"
#include "listenica/ring.h"
#include "listenica/spclient.h"

namespace listenica {

struct PlayerStats {
  uint32_t tracks_started = 0, tracks_finished = 0, load_failures = 0;
  uint32_t underruns = 0, decode_errors = 0, cdn_reconnects = 0;
  uint64_t bytes_fetched = 0, frames_out = 0;
  uint32_t last_load_ms = 0;  // load() -> first audio
  float peak_dbfs_1s = -120, rms_dbfs_1s = -120;
  // Decoder speed: seconds of audio decoded per second of decode CPU time (rolling),
  // measured at decode_kbps. Headroom for a higher quality ~ decode_x * decode_kbps / kbps.
  float decode_x = 0;
  int decode_kbps = 0;
};

// TrackChanged: a gapless transition to the track set with set_next() (uri = new track).
enum class PlayerEvent { Loading, Playing, Paused, EndOfTrack, LoadFailed, Stopped, PositionCorrected, TrackChanged };

struct PlayerConfig {
  int preferred_format = OGG_VORBIS_160;  // OGG_VORBIS_96/160/320
  std::string head_url = "https://heads-fa-tls13.spotifycdn.com/head/";  // product-info head-files-url
  bool normalisation = false;
  float normalisation_pregain_db = 0.0f;
  size_t ring_bytes = 256 * 1024;      // encrypted/compressed CDN data
  size_t pcm_bytes = 8 * 44100 * 4;    // decoded PCM between decoder and output (8 s, PSRAM)
  int output_core = 1;                 // ESP32: pin the realtime output task (-1 = any)
  int decoder_core = 1;                // decoder below the realtime tasks on core 1; core 0 = Wi-Fi/network/TLS
  bool pace_decoder = true;            // limit refill bursts (~4x realtime) once 1.5 s is buffered
};

class GroupServer;

class Player {
 public:
  using EventFn = std::function<void(PlayerEvent, const std::string& uri)>;
  Player(ApSession* ap, SpClient* sp, AudioSink* sink, PlayerConfig cfg = {});
  ~Player();
  void set_event_handler(EventFn fn) { on_event_ = std::move(fn); }

  void load(const std::string& uri, uint32_t position_ms, bool play);
  void play();
  void pause();
  void seek(uint32_t position_ms);
  void stop();
  void preload(const std::string& uri, bool full = true);  // resolve ahead of time (full: key + CDN too)
  // Track to continue with, gaplessly, when the current one ends ("" = stop).
  void set_next(const std::string& uri);
  void set_volume(uint16_t v);           // 0..65535 (Spotify connect scale)
  // Spoken overlay mixed over the music, which is ducked meanwhile (experiment).
  // pcm: mono s16 at `rate`. With nothing playing the voice plays alone.
  // to_group: Sinfonica followers speak it too, starting with this speaker's first voice sample.
  bool announce(const int16_t* pcm, size_t n, uint32_t rate, bool to_group = false);
  bool announcing();  // a voice is queued or playing (incl. the music's return)
  // Spotify refused this track's decryption key (account-level refusals hit every track).
  bool key_refused(const std::string& uri);
  void set_session(ApSession* ap) { ap_ = ap; }
  // Test hook: replace metadata/key/storage-resolve with a function.
  using ResolveFn = std::function<bool(const std::string& uri, TrackMeta& meta, uint8_t key[16],
                                       std::vector<std::string>& urls)>;
  void set_resolve_override(ResolveFn fn) { resolve_override_ = std::move(fn); }
  // Test hook: provide an already-resolved track (key + CDN URLs) so load()
  // skips metadata/key/storage-resolve. Used by offline integration tests.
  void inject_resolved(const std::string& uri, const TrackMeta& meta, const uint8_t key[16],
                       const std::vector<std::string>& urls);

  uint32_t position_ms();
  bool is_playing() const { return playing_; }
  void set_group(GroupServer* g) { group_ = g; }
  bool is_loaded() const { return loaded_; }
  std::string current_uri();
  TrackMeta current_meta();
  PlayerStats stats();

 private:
  struct Resolved {
    std::string uri;
    TrackMeta meta;
    AudioFileRef file;
    uint8_t key[16];
    std::vector<std::string> urls;
    uint64_t resolved_at = 0;
    bool ok = false;
    Bytes head;              // plaintext first bytes of the file (Spotify head-file CDN)
    bool cdn_ready = false;  // key + CDN urls known
  };
  struct Cmd {
    enum Kind { Load, Play, Pause, Seek, Stop, Quit } kind;
    std::string uri;
    uint32_t pos = 0;
    bool play = false;
  };

  ApSession* ap_;
  SpClient* sp_;
  AudioSink* sink_;
  GroupServer* group_ = nullptr;  // Sinfonica prototype: followers get our PCM and play-out anchors
  PlayerConfig cfg_;
  EventFn on_event_;
  ResolveFn resolve_override_;

  std::mutex mu_;
  std::condition_variable cv_;
  std::deque<Cmd> cmds_;
  std::atomic<bool> playing_{false}, loaded_{false}, quit_{false}, thread_done_{false};
  std::atomic<uint16_t> volume_{0xffff};
  // Announcement overlay (44.1 kHz mono) and fades; gains are Q16, output thread only.
  std::mutex refused_mu_;
  std::deque<std::string> refused_;  // last few key-refused track URIs
  std::mutex ann_mu_;
  std::vector<int16_t> ann_;
  size_t ann_pos_ = 0;
  uint32_t ann_tail_ = 0;  // frames to stay ducked after the voice
  uint32_t ann_id_ = 0;     // the clip's id for followers
  bool ann_fwd_ = false;    // followers have the clip: send them its start
  uint32_t duck_g_ = 65535, fade_g_ = 65535;
  std::atomic<bool> fade_out_req_{false}, faded_out_{false};
  // Preloads: the next track fully (metadata, head, key, CDN urls); the ones after it
  // head-only (instant start; key + urls follow while the head plays, as on any skip).
  // Head-only keeps audio-key requests to the track that will actually play next.
  static constexpr size_t kPreloadSlots = 3;
  struct PreloadReq {
    std::string uri;
    bool full;
  };
  std::deque<PreloadReq> preload_q_;
  std::string preload_inflight_;  // uri the resolver thread is working on right now
  std::deque<std::shared_ptr<Resolved>> preloaded_;  // oldest first, at most kPreloadSlots
  bool preload_known(const std::string& uri);       // queued, in flight or cached (mu_ held)
  std::shared_ptr<Resolved> cur_;
  float norm_gain_ = 1.0f;
  uint64_t load_t0_ = 0;
  // start-up breakdown for the first-audio log (ms since boot; the latest fetch job's)
  std::atomic<uint64_t> t_resolved_{0}, t_cdn_ready_{0}, t_cdn_byte_{0};
  std::atomic<int> seek_reaims_{0};

  // position bookkeeping
  std::atomic<uint64_t> base_frames_{0};     // frames before current decode origin
  std::atomic<uint64_t> written_frames_{0};  // frames written since origin
  PlayerStats stats_;

  // fetcher
  ByteRing ring_;
  std::atomic<uint32_t> fetch_gen_{0};
  std::atomic<uint64_t> fetch_offset_{0}, file_size_{0};
  std::atomic<bool> fetch_active_{false};
  // Normal: head file, then the CDN to the end. HeadOnly: just the head (a load that seeks at
  // once; without a head file it probes from 0). Probe: a seek: first only kProbeBytes, then the
  // connection waits for the decoder's verdict (seek_go_gen_) so a re-aim reuses it: a new
  // TLS connection costs ~1.4 s on the board (4 of them made one transfer take 12 s, 2026-11-05).
  enum FetchMode { FetchNormal, FetchHeadOnly, FetchProbe };
  static constexpr uint64_t kProbeBytes = 32 * 1024;
  struct FetchJob {
    std::shared_ptr<Resolved> res;
    uint64_t offset;
    uint32_t gen;
    FetchMode mode;
  };
  std::atomic<uint32_t> seek_go_gen_{0};  // decoder: the probed position is fine (or needs more data)
  std::mutex fetch_mu_;
  std::condition_variable fetch_cv_;
  std::unique_ptr<FetchJob> fetch_job_;

  // decoded-PCM stage (decoder -> pcm_ -> output task -> sink)
  struct Segment {
    uint64_t start;  // byte offset in the PCM stream (since last reset)
    std::shared_ptr<Resolved> track;  // nullptr = end of playback marker
    uint64_t base_frames;
  };
  ByteRing pcm_;
  std::mutex seg_mu_;
  std::deque<Segment> segs_;
  uint64_t pcm_written_ = 0;              // decoder side, guarded by seg_mu_
  std::atomic<uint64_t> pcm_consumed_{0}; // output side
  std::atomic<bool> out_done_{false}, decoder_active_{false}, flush_req_{false};
  std::string next_uri_;
  void pcm_reset(std::shared_ptr<Resolved> track, uint64_t base_frames);
  bool pcm_push(const int16_t* frames, size_t n);  // false if a command needs handling
  static void output_entry(void*);
  void output_loop();

  static void thread_entry(void*);
  static void fetch_entry(void*);
  static void resolver_entry(void*);
  void run();
  void fetch_loop();
  void resolver_loop();
  // cdn_early: fetch key + CDN urls alongside the head file (a load that seeks needs them at once)
  // preload: the key is optional (skipped during a key back-off; asked for when the track plays)
  bool resolve(const std::string& uri, Resolved& out, bool need_cdn, HttpClient& head_http, bool cdn_early = false,
               bool preload = false);
  bool complete_cdn(Resolved& r, bool optional_key = false);  // audio key + storage-resolve (preloads: optional key)
  // One warm head-CDN connection per thread: an HttpClient must never be used
  // by two threads at once (the load and preload paths run concurrently).
  HttpClient head_http_load_, head_http_preload_;
  std::shared_ptr<Resolved> take_resolved(const std::string& uri, bool seeking = false);
  void start_fetch(std::shared_ptr<Resolved> track, uint64_t offset, FetchMode mode = FetchNormal);
  bool decode_track(std::shared_ptr<Resolved> track, uint32_t start_ms, bool start_playing);
  void emit(PlayerEvent e, const std::string& uri) {
    if (on_event_) on_event_(e, uri);
  }
  bool pop_cmd(Cmd& c, uint32_t wait_ms);
};

}  // namespace listenica
