#include "listenica/player.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include "listenica/crypto.h"
#include "listenica/group.h"

#define STB_VORBIS_NO_STDIO 1
#define STB_VORBIS_NO_PULLDATA_API 1
#define STB_VORBIS_NO_INTEGER_CONVERSION 1
extern "C" {
#include "stb_vorbis.h"
}

static const char* TAG = "player";

namespace listenica {

static const uint8_t kAudioIv[16] = {0x72, 0xe0, 0x67, 0xfb, 0xdd, 0xcb, 0xcf, 0x77,
                                     0xeb, 0xe8, 0xbc, 0x64, 0x3f, 0x63, 0x0d, 0x93};
static constexpr size_t kSpotifyHeader = 0xa7;  // proprietary page before the real Ogg stream
static constexpr uint32_t kRate = 44100;

Player::Player(ApSession* ap, SpClient* sp, AudioSink* sink, PlayerConfig cfg)
    : ap_(ap), sp_(sp), sink_(sink), cfg_(cfg), ring_(cfg.ring_bytes), pcm_(cfg.pcm_bytes - cfg.pcm_bytes % 4) {
  pcm_.reset();
  // realtime output: high priority, pinned away from the Wi-Fi core
  spawn_thread_on("audio_out", 8 * 1024, false, 15, cfg_.output_core, &Player::output_entry, this);
  spawn_thread_on("player", 48 * 1024, true, 6, cfg_.decoder_core, &Player::thread_entry, this);
  spawn_thread("cdn_fetch", 16 * 1024, true, 5, &Player::fetch_entry, this);
  spawn_thread("resolver", 12 * 1024, true, 5, &Player::resolver_entry, this);
}

Player::~Player() {
  quit_ = true;
  {
    std::lock_guard<std::mutex> l(mu_);
    cmds_.push_back({Cmd::Quit});
  }
  cv_.notify_all();
  ring_.abort();
  pcm_.abort();
  fetch_cv_.notify_all();
  while (!thread_done_ || !out_done_) sleep_ms(10);
}

void Player::thread_entry(void* p) { static_cast<Player*>(p)->run(); }
void Player::output_entry(void* p) { static_cast<Player*>(p)->output_loop(); }
void Player::fetch_entry(void* p) { static_cast<Player*>(p)->fetch_loop(); }
void Player::resolver_entry(void* p) { static_cast<Player*>(p)->resolver_loop(); }

// ------------------------------------------------------------ public API
void Player::load(const std::string& uri, uint32_t pos, bool play) {
  std::lock_guard<std::mutex> l(mu_);
  // a newer load supersedes queued commands and any stale gapless 'next'
  cmds_.clear();
  next_uri_.clear();
  cmds_.push_back({Cmd::Load, uri, pos, play});
  cv_.notify_all();
}
void Player::play() {
  std::lock_guard<std::mutex> l(mu_);
  cmds_.push_back({Cmd::Play});
  cv_.notify_all();
}
void Player::pause() {
  std::lock_guard<std::mutex> l(mu_);
  cmds_.push_back({Cmd::Pause});
  cv_.notify_all();
}
void Player::seek(uint32_t pos) {
  std::lock_guard<std::mutex> l(mu_);
  cmds_.push_back({Cmd::Seek, "", pos});
  cv_.notify_all();
}
void Player::stop() {
  std::lock_guard<std::mutex> l(mu_);
  cmds_.clear();
  cmds_.push_back({Cmd::Stop});
  cv_.notify_all();
}
bool Player::preload_known(const std::string& uri) {
  if (preload_inflight_ == uri) return true;
  for (auto& q : preload_q_)
    if (q.uri == uri) return true;
  for (auto& r : preloaded_)
    if (r->uri == uri) return true;
  return false;
}

void Player::preload(const std::string& uri, bool full) {
  std::lock_guard<std::mutex> l(mu_);
  if (uri.empty()) return;
  for (auto& r : preloaded_)  // cached head-only but now wanted in full: complete it on use (cheap)
    if (r->uri == uri) return;
  for (auto& q : preload_q_)
    if (q.uri == uri) {
      q.full = q.full || full;
      return;
    }
  if (preload_inflight_ == uri) return;
  if (full) preload_q_.push_front({uri, true});  // the next track goes first
  else preload_q_.push_back({uri, false});
  while (preload_q_.size() > kPreloadSlots) preload_q_.pop_back();
  cv_.notify_all();
}
bool Player::announce(const int16_t* pcm, size_t n, uint32_t rate, bool to_group) {
  if (!n || !rate) return false;
  uint32_t id = 0;
  if (to_group && group_) {
    std::lock_guard<std::mutex> l(ann_mu_);
    id = ++ann_id_;
  }
  if (id) group_->voice_clip(id, "", pcm, n, rate);  // sent now; started with our first voice sample
  // linear resample to the output rate (speech: no filter needed)
  size_t out_n = size_t(uint64_t(n) * kRate / rate);
  std::vector<int16_t> v(out_n);
  for (size_t i = 0; i < out_n; i++) {
    uint64_t p = uint64_t(i) * rate * 65536 / kRate;  // Q16 source position
    size_t k = size_t(p >> 16);
    int32_t f = int32_t(p & 0xffff);
    int32_t a = pcm[std::min(k, n - 1)], b = pcm[std::min(k + 1, n - 1)];
    v[i] = int16_t(a + (((b - a) * f) >> 16));
  }
  std::lock_guard<std::mutex> l(ann_mu_);
  ann_.swap(v);
  ann_pos_ = 0;
  ann_tail_ = kRate * 4 / 10;
  ann_fwd_ = id != 0;
  return true;
}

bool Player::key_refused(const std::string& uri) {
  std::lock_guard<std::mutex> l(refused_mu_);
  for (auto& u : refused_)
    if (u == uri) return true;
  return false;
}

bool Player::announcing() {
  std::lock_guard<std::mutex> l(ann_mu_);
  return ann_pos_ < ann_.size() || ann_tail_ > 0 || duck_g_ < 65535;
}

void Player::set_volume(uint16_t v) {
  volume_ = v;
  sink_->set_hw_volume(v);  // used instead of digital scaling when the sink supports it
}
void Player::set_next(const std::string& uri) {
  std::lock_guard<std::mutex> l(mu_);
  next_uri_ = uri;
}

void Player::inject_resolved(const std::string& uri, const TrackMeta& meta, const uint8_t key[16],
                             const std::vector<std::string>& urls) {
  auto r = std::make_shared<Resolved>();
  r->uri = uri;
  r->meta = meta;
  memcpy(r->key, key, 16);
  r->urls = urls;
  r->file.format = OGG_VORBIS_160;
  r->resolved_at = now_ms();
  r->ok = true;
  r->cdn_ready = true;  // key + urls supplied by the caller
  std::lock_guard<std::mutex> l(mu_);
  preloaded_.push_back(r);
  while (preloaded_.size() > kPreloadSlots) preloaded_.pop_front();
}

uint32_t Player::position_ms() {
  uint64_t f = base_frames_ + written_frames_;
  size_t lat = sink_->latency_frames();
  f = f > lat ? f - lat : 0;
  return uint32_t(f * 1000 / kRate);
}
std::string Player::current_uri() {
  std::lock_guard<std::mutex> l(mu_);
  return cur_ ? cur_->uri : std::string();
}
TrackMeta Player::current_meta() {
  std::lock_guard<std::mutex> l(mu_);
  return cur_ ? cur_->meta : TrackMeta();
}
PlayerStats Player::stats() {
  std::lock_guard<std::mutex> l(mu_);
  PlayerStats s = stats_;
  s.bytes_fetched = fetch_offset_;
  return s;
}

bool Player::pop_cmd(Cmd& c, uint32_t wait_ms) {
  std::unique_lock<std::mutex> l(mu_);
  if (cmds_.empty() && wait_ms) cv_.wait_for(l, std::chrono::milliseconds(wait_ms), [&] { return !cmds_.empty(); });
  if (cmds_.empty()) return false;
  c = cmds_.front();
  cmds_.pop_front();
  return true;
}

// -------------------------------------------------------------- resolve
bool Player::complete_cdn(Resolved& out, bool optional_key) {
  if (out.cdn_ready) return true;
  uint64_t t0 = now_ms();
  // Audio key (AP connection) and storage-resolve (spclient HTTPS) are independent:
  // run them in parallel (measured ~160 ms saved on the PC).
  struct Job {
    std::mutex mu;
    std::condition_variable cv;
    bool done = false, ok = false;
    SpClient* sp = nullptr;
    Bytes file_id;
    std::vector<std::string> urls;
  };
  auto job = std::make_shared<Job>();
  job->sp = sp_;
  job->file_id = out.file.file_id;
  spawn_thread("sresolve", 8192, true, 5, [](void* a) {
    auto* holder = static_cast<std::shared_ptr<Job>*>(a);
    std::shared_ptr<Job> j = *holder;
    delete holder;
    bool ok = j->sp->storage_resolve(j->file_id, j->urls);
    {
      std::lock_guard<std::mutex> l(j->mu);
      j->ok = ok;
      j->done = true;
    }
    j->cv.notify_all();
  }, new std::shared_ptr<Job>(job));
  bool refused = false;
  bool key_ok = ap_ && ap_->request_audio_key(out.meta.gid, out.file.file_id, out.key, 3000, &refused, optional_key);
  if (refused) {
    std::lock_guard<std::mutex> l(refused_mu_);
    refused_.push_back(out.uri);
    while (refused_.size() > 8) refused_.pop_front();
  }
  bool urls_ok;
  {
    std::unique_lock<std::mutex> l(job->mu);
    job->cv.wait_for(l, std::chrono::seconds(20), [&] { return job->done; });
    urls_ok = job->done && job->ok;
    if (urls_ok) out.urls = job->urls;
  }
  if (!key_ok) {
    SLOGW(TAG, "%s: audio key request failed", out.uri.c_str());
    return false;
  }
  if (!urls_ok) return false;
  out.resolved_at = now_ms();
  out.cdn_ready = true;
  SLOGD(TAG, "key + CDN urls for '%s' in %u ms", out.meta.name.c_str(), (unsigned)(now_ms() - t0));
  return true;
}

bool Player::resolve(const std::string& uri, Resolved& out, bool need_cdn, HttpClient& head_http, bool cdn_early,
                     bool preload) {
  out = Resolved();
  out.uri = uri;
  uint64_t t0 = now_ms();
  if (resolve_override_) {
    out.ok = resolve_override_(uri, out.meta, out.key, out.urls);
    out.file.format = OGG_VORBIS_160;
    out.resolved_at = now_ms();
    out.cdn_ready = out.ok;
    return out.ok;
  }
  if (!sp_->track_metadata(uri, out.meta) || !out.meta.ok) {
    SLOGW(TAG, "no playable files for %s", uri.c_str());
    return false;
  }
  // choose a format: preferred Vorbis bitrate, else any Vorbis
  static const int order160[] = {OGG_VORBIS_160, OGG_VORBIS_96, OGG_VORBIS_320};
  static const int order320[] = {OGG_VORBIS_320, OGG_VORBIS_160, OGG_VORBIS_96};
  static const int order96[] = {OGG_VORBIS_96, OGG_VORBIS_160, OGG_VORBIS_320};
  const int* order = cfg_.preferred_format == OGG_VORBIS_320 ? order320
                     : cfg_.preferred_format == OGG_VORBIS_96 ? order96
                                                               : order160;
  bool found = false;
  for (int i = 0; i < 3 && !found; i++)
    for (auto& f : out.meta.files)
      if (f.format == order[i]) {
        out.file = f;
        found = true;
        break;
      }
  if (!found) {
    SLOGW(TAG, "%s: no Ogg Vorbis file among %u formats", uri.c_str(), (unsigned)out.meta.files.size());
    return false;
  }
  // A load that seeks past the head needs key + CDN urls right away: fetch them (AP + spclient)
  // while the head downloads (its own connection) instead of after it.
  struct Early {
    std::mutex mu;
    std::condition_variable cv;
    bool done = false;
    Player* self = nullptr;
    Resolved* out = nullptr;
  };
  std::shared_ptr<Early> early;
  if (cdn_early && !out.cdn_ready) {
    early = std::make_shared<Early>();
    early->self = this;
    early->out = &out;
    spawn_thread("cdn_early", 8192, true, 5, [](void* a) {
      auto* holder = static_cast<std::shared_ptr<Early>*>(a);
      std::shared_ptr<Early> e = *holder;
      delete holder;
      e->self->complete_cdn(*e->out);  // bounded: key (3 s per ask, back-offs <= 18 s), urls <= 20 s
      {
        std::lock_guard<std::mutex> l(e->mu);
        e->done = true;
      }
      e->cv.notify_all();
    }, new std::shared_ptr<Early>(early));
  }
  auto join_early = [&] {
    if (!early) return;
    std::unique_lock<std::mutex> l(early->mu);
    early->cv.wait(l, [&] { return early->done; });  // it writes into `out`: never return before it ends
  };
  // Head file: plaintext first ~128 KB of the (decrypted) file, served without
  // the audio key. Playback starts from it while key + CDN urls are fetched.
  for (int attempt = 0; attempt < 2 && !cfg_.head_url.empty() && out.head.empty(); attempt++) {
    Bytes head;
    head.reserve(132 * 1024);
    size_t expect = 0;
    // A load that seeks only needs the Vorbis headers at the start (a few KB): its first 64 KB
    // instead of the ~400-500 KB head (~2 s on the board). A server ignoring Range sends it all.
    Headers hh;
    if (cdn_early) hh["Range"] = "bytes=0-65535";
    auto hr = head_http.stream("GET", cfg_.head_url + hex(out.file.file_id), hh,
                                [&](const uint8_t* d, size_t n) {
                                  head.insert(head.end(), d, d + n);
                                  return head.size() <= 512 * 1024;
                                },
                                5000,
                                [&](const HttpResponse& h) { expect = size_t(strtoull(h.header("content-length").c_str(), nullptr, 10)); });
    // Only a complete head is usable: a truncated one runs the decoder dry
    // before the CDN stream is ready (heard as an underrun right after a skip).
    bool complete = expect ? head.size() == expect : hr.status == 200;
    if ((hr.status == 200 || hr.status == 206) && complete && head.size() > 0xa7 + 4096 && memcmp(head.data(), "OggS", 4) == 0) {
      out.head.swap(head);
    } else {
      SLOGW(TAG, "head file for %s unusable (HTTP %d, %u of %u bytes)", uri.c_str(), hr.status, unsigned(head.size()),
            unsigned(expect));
      if ((hr.status != 200 && hr.status != 206) || complete) break;  // only a truncated transfer is worth retrying
      head_http.close();
    }
  }
  join_early();
  if ((need_cdn || out.head.empty()) && !complete_cdn(out, preload) && !(preload && !out.head.empty()))
    return false;  // a preload keeps its head without the key: the load asks for it
  out.resolved_at = now_ms();
  out.ok = true;
  SLOGI(TAG, "resolved '%s' by %s (%u ms, format %d) in %u ms%s", out.meta.name.c_str(), out.meta.artist.c_str(),
        (unsigned)out.meta.duration_ms, out.file.format, (unsigned)(now_ms() - t0),
        out.head.empty() ? "" : strfmt(" (head %u KB)", unsigned(out.head.size() / 1024)).c_str());
  return true;
}

std::shared_ptr<Player::Resolved> Player::take_resolved(const std::string& uri, bool seeking) {
  {
    std::unique_lock<std::mutex> l(mu_);
    // The resolver thread is already working on this track (e.g. a skip right
    // after the preload started): join it instead of resolving it twice.
    if (preload_inflight_ == uri)
      cv_.wait_for(l, std::chrono::milliseconds(3000), [&] { return preload_inflight_ != uri || quit_; });
    // CDN URLs are signed and expire; reuse a preload for up to 10 minutes
    for (auto it = preloaded_.begin(); it != preloaded_.end(); ++it) {
      if ((*it)->uri != uri) continue;
      auto r = *it;
      preloaded_.erase(it);
      if (now_ms() - r->resolved_at < 600000) return r;
      break;
    }
  }
  auto r = std::make_shared<Resolved>();
  // fast path: metadata + head (+ key and CDN urls in parallel when the load seeks)
  if (!resolve(uri, *r, /*need_cdn=*/false, head_http_load_, /*cdn_early=*/seeking)) return nullptr;
  return r;
}

void Player::resolver_loop() {
  while (!quit_) {
    std::string uri;
    bool full = true;
    {
      std::unique_lock<std::mutex> l(mu_);
      cv_.wait_for(l, std::chrono::milliseconds(500), [&] { return !preload_q_.empty() || quit_; });
      if (preload_q_.empty()) continue;
      uri = preload_q_.front().uri;
      full = preload_q_.front().full;
      preload_q_.pop_front();
    }
    // Let the current track's stream start-up finish first (CDN ring full or
    // file complete): a concurrent head download crawled for seconds and was
    // sometimes cut off. The preload has the rest of the track to complete.
    for (int i = 0; i < 80 && !quit_ && fetch_active_ && ring_.size() < ring_.capacity() * 9 / 10; i++) sleep_ms(100);
    {
      std::lock_guard<std::mutex> l(mu_);
      if (cur_ && cur_->uri == uri) continue;  // already playing
      bool cached = false;
      for (auto& r : preloaded_) cached = cached || r->uri == uri;
      if (cached) continue;
      preload_inflight_ = uri;
    }
    auto r = std::make_shared<Resolved>();
    bool ok = resolve(uri, *r, /*need_cdn=*/full, head_http_preload_, /*cdn_early=*/false, /*preload=*/true);
    {
      std::lock_guard<std::mutex> l(mu_);
      if (ok) {
        preloaded_.push_back(r);
        while (preloaded_.size() > kPreloadSlots) preloaded_.pop_front();
      }
      preload_inflight_.clear();
    }
    cv_.notify_all();
    if (ok) SLOGI(TAG, "preloaded %s%s", uri.c_str(), full ? "" : " (head only)");
  }
}

// ---------------------------------------------------------------- fetch
void Player::start_fetch(std::shared_ptr<Resolved> track, uint64_t offset, FetchMode mode) {
  uint32_t gen = ring_.reset();
  if (offset == 0) file_size_ = 0;
  fetch_gen_ = gen;
  std::lock_guard<std::mutex> l(fetch_mu_);
  fetch_job_.reset(new FetchJob{track, offset, gen, mode});
  fetch_cv_.notify_all();
}

void Player::fetch_loop() {
  HttpClient http;
  while (!quit_) {
    std::unique_ptr<FetchJob> job;
    {
      std::unique_lock<std::mutex> l(fetch_mu_);
      fetch_cv_.wait_for(l, std::chrono::milliseconds(500), [&] { return fetch_job_ != nullptr || quit_; });
      job = std::move(fetch_job_);
    }
    if (!job || !job->res) continue;
    fetch_active_ = true;
    uint64_t off = job->offset;
    Resolved& res = *job->res;
    // 1) plaintext head first (no key needed): the decoder starts immediately
    if (!res.head.empty() && off < res.head.size()) {
      bool ok = true;
      while (off < res.head.size() && ok) {
        size_t c = std::min<size_t>(res.head.size() - size_t(off), 8192);
        ok = ring_.write(res.head.data() + off, c, job->gen);
        off += c;
        fetch_offset_ = off;
      }
      if (!ok) {
        fetch_active_ = false;
        continue;  // superseded
      }
    }
    if (job->mode == FetchHeadOnly && !res.head.empty()) {  // the decoder only needs its Vorbis headers
      fetch_active_ = false;
      continue;
    }
    bool probe = job->mode != FetchNormal;
    // 2) key + CDN urls (overlaps with playback of the head)
    if (!res.cdn_ready && !complete_cdn(res)) {
      SLOGE(TAG, "could not get key/CDN for '%s'", res.meta.name.c_str());
      ring_.set_eof(job->gen, true);
      fetch_active_ = false;
      continue;
    }
    t_cdn_ready_ = now_ms();
    t_cdn_byte_ = 0;
    Aes128Ctr aes(res.key, kAudioIv);
    Bytes scratch(4096);
    uint8_t* tmp = scratch.data();
    aes.seek(off);
    size_t url_i = 0;
    int failures = 0;
    bool done = false;
    while (!done && !quit_ && ring_.generation() == job->gen) {
      const std::string& url = job->res->urls[url_i % job->res->urls.size()];
      bool superseded = false;
      uint64_t got_this_req = 0, drop = 0;
      bool was_probe = probe;
      std::string range = probe ? strfmt("bytes=%llu-%llu", (unsigned long long)off, (unsigned long long)(off + kProbeBytes - 1))
                                : strfmt("bytes=%llu-", (unsigned long long)off);
      auto resp = http.stream("GET", url, {{"Range", range}},
                              [&](const uint8_t* d, size_t n) {
                                // A probe superseded by a re-aim (it lands before the probe has
                                // fully arrived on a slow link): read out its last few KB instead
                                // of dropping the connection the re-aim is about to reuse.
                                if (superseded) return true;
                                // decrypt into a scratch copy, then push to the ring
                                size_t done_n = 0;
                                while (done_n < n) {
                                  size_t c = std::min(n - done_n, scratch.size());
                                  memcpy(tmp, d + done_n, c);
                                  aes.apply(tmp, c);
                                  if (drop) {  // server ignored Range: skip what we already have
                                    size_t k = size_t(std::min<uint64_t>(drop, c));
                                    drop -= k;
                                    done_n += k;
                                    if (k == c) continue;
                                    memmove(tmp, tmp + k, c - k);
                                    c -= k;
                                  }
                                  if (!ring_.write(tmp, c, job->gen)) {
                                    superseded = true;
                                    return was_probe;
                                  }
                                  if (!t_cdn_byte_) t_cdn_byte_ = now_ms();
                                  done_n += c;
                                  off += c;
                                  got_this_req += c;
                                  fetch_offset_ = off;
                                }
                                return true;
                              },
                              20000,
                              [&](const HttpResponse& h) {
                                std::string cr = h.header("content-range");  // bytes a-b/total
                                size_t sl = cr.rfind('/');
                                if (sl != std::string::npos) file_size_ = strtoull(cr.c_str() + sl + 1, nullptr, 10);
                                // a 200 means the server ignored Range: decrypt from 0, drop to `off`
                                if (h.status == 200 && off != 0) {
                                  drop = off;
                                  aes.seek(0);
                                }
                              });
      if (superseded) break;
      if (resp.status == 416) {  // range starts past the end: the head already covered the file
        ring_.set_eof(job->gen);
        done = true;
        break;
      }
      if (resp.status == 206 || resp.status == 200) {
        std::string cr = resp.header("content-range");  // bytes a-b/total
        size_t slash = cr.rfind('/');
        if (slash != std::string::npos) file_size_ = strtoull(cr.c_str() + slash + 1, nullptr, 10);
        if (file_size_ && off >= file_size_) {
          ring_.set_eof(job->gen);
          done = true;
          break;
        }
      }
      if (was_probe && resp.status == 206 && got_this_req > 0) {
        // Probe read in full, connection still open: wait for the decoder. Landed -> stream on from
        // here; a re-aim -> a new job (generation) on this same connection; probe used up without a
        // verdict (ring empty for 300 ms) -> it needs more: stream on.
        probe = false;
        uint64_t empty_since = 0;
        for (uint64_t t0 = now_ms(); !quit_ && ring_.generation() == job->gen && seek_go_gen_ != job->gen &&
                                     now_ms() - t0 < 10000;
             sleep_ms(5)) {
          if (ring_.size() > 0) empty_since = 0;
          else if (!empty_since) empty_since = now_ms();
          else if (now_ms() - empty_since > 300) break;
        }
        continue;
      }
      // incomplete: reconnect from where we are, rotating CDN hosts on HTTP errors
      if (resp.status != 206 && resp.status != 200) url_i++;
      if (got_this_req == 0) failures++;
      else failures = 0;
      {
        std::lock_guard<std::mutex> l(mu_);
        stats_.cdn_reconnects++;
      }
      SLOGW(TAG, "CDN stream interrupted at %llu (HTTP %d), reconnecting", (unsigned long long)off, resp.status);
      if (failures >= 6) {
        SLOGE(TAG, "CDN gave up after %d failures", failures);
        ring_.set_eof(job->gen, true);
        break;
      }
      http.close();
      sleep_ms(200 * failures);
    }
    fetch_active_ = false;
  }
}

// ------------------------------------------------------- PCM / output stage
void Player::pcm_reset(std::shared_ptr<Resolved> track, uint64_t base_frames) {
  std::lock_guard<std::mutex> l(seg_mu_);
  pcm_.reset();
  pcm_written_ = 0;
  pcm_consumed_ = 0;
  segs_.clear();
  segs_.push_back({0, std::move(track), base_frames});
  base_frames_ = base_frames;
  written_frames_ = 0;
  flush_req_ = true;
}

bool Player::pcm_push(const int16_t*, size_t) { return true; }  // (unused; kept for ABI clarity)

void Player::output_loop() {
  std::vector<int16_t> buf(1024 * 2);
  bool outputting = false, rebuffering = false;
  // A spoken notification while no music is flowing plays on its own. True if it wrote audio.
  auto voice_alone = [&]() -> bool {
    size_t vf = 0;
    {
      std::lock_guard<std::mutex> al(ann_mu_);
      if (ann_pos_ < ann_.size()) {
        if (ann_pos_ == 0 && ann_fwd_ && group_) {  // followers start with us
          ann_fwd_ = false;
          group_->voice_start(ann_id_, "", now_us() + int64_t(sink_->latency_frames()) * 1000000 / kRate);
        }
        vf = std::min(buf.size() / 2, ann_.size() - ann_pos_);
        for (size_t i = 0; i < vf; i++) buf[i * 2] = buf[i * 2 + 1] = ann_[ann_pos_ + i];
        ann_pos_ += vf;
      } else if (!ann_.empty()) {
        std::vector<int16_t>().swap(ann_);
        ann_pos_ = 0;
        ann_tail_ = 0;
        duck_g_ = 65535;
      }
    }
    if (!vf) return false;
    uint32_t v = sink_->hw_volume_active() ? 0xFFFF : volume_.load();
    uint32_t g = uint32_t((uint64_t(v) * v) >> 16);
    if (g < 65535)
      for (size_t i = 0; i < vf * 2; i++) buf[i] = int16_t((int32_t(buf[i]) * int32_t(g)) >> 16);
    sink_->write(buf.data(), vf);
    return true;
  };
  uint32_t last_gen = pcm_.generation();
  while (!quit_) {
    // segment boundaries crossed by the output position
    std::vector<std::pair<PlayerEvent, std::string>> evs;
    {
      std::lock_guard<std::mutex> l(seg_mu_);
      while (segs_.size() > 1 && segs_[1].start <= pcm_consumed_) {
        Segment prev = segs_.front();
        segs_.pop_front();
        Segment& cur = segs_.front();
        {
          std::lock_guard<std::mutex> l2(mu_);
          stats_.tracks_finished++;
          if (cur.track) {
            cur_ = cur.track;
            stats_.tracks_started++;
          }
        }
        base_frames_ = cur.base_frames;
        written_frames_ = 0;
        if (cur.track) {
          evs.push_back({PlayerEvent::TrackChanged, cur.track->uri});
        } else {
          evs.push_back({PlayerEvent::EndOfTrack, prev.track ? prev.track->uri : std::string()});
          playing_ = false;
        }
      }
    }
    for (auto& e : evs) emit(e.first, e.second);
    if (flush_req_.exchange(false)) sink_->flush();
    auto group_idle = [&] {  // followers stop with us (paused, rebuffering)
      if (!group_) return;
      uint64_t c;
      {
        std::lock_guard<std::mutex> l(seg_mu_);
        c = pcm_consumed_;
      }
      group_->anchor(pcm_.generation(), c, now_us(), false, volume_.load());
    };
    if (!playing_) {
      group_idle();
      outputting = false;
      if (faded_out_) fade_g_ = 0;  // paused with a fade: fade back in on play
      if (voice_alone()) continue;  // nothing playing: a notification plays on its own
      sleep_ms(10);
      continue;
    }
    // a flush (load/seek/skip) is not starvation: start the new audio immediately
    uint32_t cur_gen = pcm_.generation();
    if (cur_gen != last_gen) {
      last_gen = cur_gen;
      fade_g_ = 65535;  // loads, seeks and skips start at full level
      outputting = false;
      rebuffering = false;
    }
    // after an underrun, rebuffer most of the PCM ring (~6 s) before resuming (few long gaps beat many short ones)
    if (rebuffering) {
      if (pcm_.size() < std::min(pcm_.capacity() * 3 / 4, size_t(kRate) * 4 * 6) && decoder_active_) {  // ~6 s
        group_idle();
        if (!voice_alone()) sleep_ms(20);
        continue;
      }
      rebuffering = false;
    }
    if (fade_out_req_ && fade_g_ == 0) {  // pause fade done: hold until the decoder pauses the sink
      faded_out_ = true;
      sleep_ms(5);
      continue;
    }
    if (!fade_out_req_ && faded_out_) faded_out_ = false;  // resumed: fade_g_ ramps up from 0
    uint32_t gen = 0;
    uint64_t consumed_end = 0;
    int r = pcm_.read((uint8_t*)buf.data(), buf.size() * 2, 20, &gen);
    if (r <= 0) {
      if (outputting && decoder_active_) {
        std::lock_guard<std::mutex> l(mu_);
        stats_.underruns++;
        rebuffering = true;
        SLOGW(TAG, "output underrun (decoder starved), rebuffering");
      }
      outputting = false;
      voice_alone();
      continue;
    }
    size_t frames = size_t(r) / 4;
    {
      std::lock_guard<std::mutex> l(seg_mu_);
      if (gen != pcm_.generation()) continue;  // flushed while reading: drop stale audio
      pcm_consumed_ += uint64_t(r);
      written_frames_ += frames;
      consumed_end = pcm_consumed_;
    }
    // fades (pause/play, ~150 ms) and the announcement overlay (music ducked to -12 dB)
    {
      const uint32_t kFadeStep = 65535 / (kRate * 15 / 100) + 1, kDuckStep = 65535 / (kRate / 4) + 1;
      const uint32_t kDuck = 16384;
      uint32_t fade_t = fade_out_req_ ? 0 : 65535;
      std::unique_lock<std::mutex> al(ann_mu_);
      bool ann = ann_pos_ < ann_.size() || ann_tail_ > 0 || duck_g_ < 65535;
      if (!ann) al.unlock();
      if (ann || fade_g_ != fade_t) {
        for (size_t i = 0; i < frames; i++) {
          if (fade_g_ < fade_t) fade_g_ = std::min(fade_t, fade_g_ + kFadeStep);
          else if (fade_g_ > fade_t) fade_g_ = fade_g_ > kFadeStep ? fade_g_ - kFadeStep : 0;
          int32_t voice = 0;
          if (ann) {
            bool speaking = ann_pos_ < ann_.size();
            uint32_t duck_t = (speaking || ann_tail_ > 0) ? kDuck : 65535;
            if (duck_g_ > duck_t) duck_g_ = duck_g_ > duck_t + kDuckStep ? duck_g_ - kDuckStep : duck_t;
            else if (duck_g_ < duck_t) duck_g_ = std::min<uint32_t>(duck_t, duck_g_ + kDuckStep / 2);  // slower back up
            if (speaking && duck_g_ == kDuck) {  // speak once the music is down
              if (ann_pos_ == 0 && ann_fwd_ && group_) {  // followers start with this sample
                ann_fwd_ = false;
                group_->voice_start(ann_id_, "", now_us() + int64_t(sink_->latency_frames() + i) * 1000000 / kRate);
              }
              voice = ann_[ann_pos_++];
            }
            else if (!speaking && ann_tail_ > 0) ann_tail_--;
          }
          uint32_t mg = uint32_t((uint64_t(fade_g_) * duck_g_) >> 16);
          for (int c = 0; c < 2; c++) {
            int32_t s = ((int32_t(buf[i * 2 + c]) * int32_t(mg)) >> 16) + ((voice * int32_t(fade_g_)) >> 16);
            buf[i * 2 + c] = int16_t(std::max<int32_t>(-32768, std::min<int32_t>(32767, s)));
          }
        }
        if (ann && ann_pos_ >= ann_.size() && ann_tail_ == 0 && duck_g_ == 65535 && !ann_.empty()) {
          std::vector<int16_t>().swap(ann_);  // done: free the clip
          ann_pos_ = 0;
        }
      }
    }
    // volume at the output so changes are immediate despite the deep buffer
    uint32_t v = sink_->hw_volume_active() ? 0xFFFF : volume_.load();
    uint32_t g = uint32_t((uint64_t(v) * v) >> 16);  // squared curve, Q16
    if (g < 65535) {
      for (size_t i = 0; i < frames * 2; i++) buf[i] = int16_t((int32_t(buf[i]) * int32_t(g)) >> 16);
    }
    sink_->write(buf.data(), frames);
    outputting = true;
    if (group_)  // the end of this block becomes audible once the sink's buffered frames have played
      group_->anchor(gen, consumed_end, now_us() + int64_t(sink_->latency_frames()) * 1000000 / kRate, true, volume_.load());
  }
  out_done_ = true;
}

// ---------------------------------------------------------------- decode
void Player::run() {
  Cmd c;
  while (!quit_) {
    if (!pop_cmd(c, 500)) continue;
    switch (c.kind) {
      case Cmd::Quit:
        thread_done_ = true;
        return;
      case Cmd::Load: {
        playing_ = false;
        loaded_ = false;
        ring_.reset();
        pcm_reset(nullptr, 0);
        emit(PlayerEvent::Loading, c.uri);
        uint64_t t0 = now_ms();
        auto r = take_resolved(c.uri, c.pos > 0);
        if (!r) {
          {
            std::lock_guard<std::mutex> l(mu_);
            stats_.load_failures++;
          }
          emit(PlayerEvent::LoadFailed, c.uri);
          break;
        }
        {
          std::lock_guard<std::mutex> l(mu_);
          cur_ = r;
          stats_.tracks_started++;
        }
        loaded_ = true;
        load_t0_ = t0;
        t_resolved_ = now_ms();
        t_cdn_ready_ = t_cdn_byte_ = 0;
        seek_reaims_ = 0;
        decode_track(r, c.pos, c.play);
        break;
      }
      case Cmd::Stop:
        playing_ = false;
        loaded_ = false;
        ring_.reset();
        pcm_reset(nullptr, 0);
        emit(PlayerEvent::Stopped, "");
        break;
      default:
        break;  // play/pause/seek with nothing loaded
    }
  }
  thread_done_ = true;
}

static float db(float x) { return 20.0f * log10f(x + 1e-9f); }

// Decodes `track` from start_ms into the PCM ring, then (gapless) continues
// with the track set via set_next(). Returns when the decoded material ends,
// or when a Load/Stop/Quit (re-queued for run()) interrupts it.
bool Player::decode_track(std::shared_ptr<Resolved> track, uint32_t start_ms, bool start_playing) {
#if LISTENICA_DIAG
  uint64_t dec_us = 0, dec_samples = 0;  // decode-speed measurement
#endif
  bool first_track = true;
  uint8_t tmp[4096];
  std::vector<int16_t> pending;  // decoded PCM not yet in the ring
  size_t pending_off = 0;
  decoder_active_ = true;
  playing_ = start_playing;
  sink_->set_paused(!start_playing);
#if LISTENICA_DIAG
  float acc_sq = 0, acc_peak = 0;
#endif
  uint32_t acc_n = 0;
  bool first_audio = start_playing;  // a paused load (e.g. resuming after an update) is no skip-latency sample

  // Handles one command while the decoder is busy. Returns false if the
  // decoder must stop (command re-queued for run()).
  auto handle_play_pause = [&](const Cmd& c) {
    if (c.kind == Cmd::Play) {
      if (!playing_) {
        playing_ = true;
        sink_->set_paused(false);
        emit(PlayerEvent::Playing, current_uri());
      }
      return true;
    }
    if (c.kind == Cmd::Pause) {
      if (playing_) {
        // fade out first (~150 ms; the output thread holds once silent), then pause the sink
        fade_out_req_ = true;
        for (int i = 0; i < 40 && !faded_out_; i++) sleep_ms(10);
        playing_ = false;
        fade_out_req_ = false;
        sink_->set_paused(true);
        emit(PlayerEvent::Paused, current_uri());
      }
      return true;
    }
    return false;
  };

  while (track) {
    std::string uri = track->uri;
    if (first_track) pcm_reset(track, uint64_t(start_ms) * kRate / 1000);
    start_fetch(track, 0, first_track && start_ms > 0 ? FetchHeadOnly : FetchNormal);
    Bytes in;
    in.reserve(64 * 1024);
    size_t in_pos = 0;
    uint32_t pgen = pcm_.generation();
    auto fail = [&](const char* why) {
      SLOGE(TAG, "%s (%s)", why, uri.c_str());
      decoder_active_ = false;
      if (first_track) {
        emit(PlayerEvent::LoadFailed, uri);
      } else {
        std::lock_guard<std::mutex> l(seg_mu_);  // let the previous track drain, then EndOfTrack
        segs_.push_back({pcm_written_, nullptr, 0});
      }
      return false;
    };

    // --- skip Spotify header, read normalisation, open decoder
    Bytes head;
    while (head.size() < kSpotifyHeader + 4096) {
      int r = ring_.read(tmp, sizeof tmp, 15000);
      if (r <= 0) return fail("no audio data from CDN");
      head.insert(head.end(), tmp, tmp + r);
    }
    norm_gain_ = 1.0f;
    if (cfg_.normalisation && head.size() >= 160) {
      float g[4];
      memcpy(g, &head[144], 16);  // track_gain_db, track_peak, album_gain_db, album_peak
      float gain = powf(10.0f, (g[0] + cfg_.normalisation_pregain_db) / 20.0f);
      if (g[1] > 0 && gain * g[1] > 1.0f) gain = 1.0f / g[1];
      norm_gain_ = gain;
    }
    if (memcmp(&head[kSpotifyHeader], "OggS", 4) != 0) return fail("decrypted stream is not Ogg (bad key?)");
    in.assign(head.begin() + kSpotifyHeader, head.end());
    stb_vorbis* v = nullptr;
    for (;;) {
      int used = 0, err = 0;
      v = stb_vorbis_open_pushdata(in.data(), int(in.size()), &used, &err, nullptr);
      if (v) {
        in_pos = size_t(used);
        if (group_) group_->vorbis_header(norm_gain_, in.data(), in_pos);  // followers decode the same
        break;
      }
      if (err != VORBIS_need_more_data || in.size() > 256 * 1024) return fail("vorbis open failed");
      int r = ring_.read(tmp, sizeof tmp, 15000);
      if (r <= 0) return fail("stream ended during vorbis setup");
      in.insert(in.end(), tmp, tmp + r);
    }
    const uint64_t first_audio_byte = kSpotifyHeader + in_pos;  // after the 3 header packets

    // Seeking: no seek index in Spotify files. Estimate a byte offset ~1.5 s
    // early, resync on an Ogg page, read the exact granule position, then
    // discard decoded samples up to the target (sample-accurate).
    bool need_offset_sync = false;
    uint64_t seek_target = 0, seek_off = 0, discard = 0;
    int seek_attempts = 0;
    bool group_flush = false;     // the decoder was flushed: tell followers with the next frame
    uint64_t group_end = 0;       // decoder input consumed since its setup or last flush
    std::vector<uint8_t> gframe;  // the bytes of the last decode call, until its outcome is known
    bool gframe_pending = false;
    auto group_frame = [&](uint32_t skip, uint32_t kept) {  // one per decode call that consumed bytes
      if (!gframe_pending) return;
      gframe_pending = false;
      uint64_t pos;
      {
        std::lock_guard<std::mutex> l(seg_mu_);
        pos = pcm_written_;  // pending is empty when a frame is decoded: its output lands here
      }
      group_->vorbis_frame(pgen, pos, skip, kept, group_flush, group_end, gframe.data(), gframe.size());
      group_flush = false;
    };
    auto fetch_from = [&](uint64_t off) {
      if (group_) group_frame(0, 0);
      group_flush = true;
      group_end = 0;
      stb_vorbis_flush_pushdata(v);
      in.clear();
      in_pos = 0;
      seek_off = off;
      start_fetch(track, off, FetchProbe);
    };
    auto bytes_per_ms = [&]() -> double {
      uint64_t dur = track->meta.duration_ms ? track->meta.duration_ms : 1;
      uint64_t total = file_size_ ? file_size_.load() : 0;
      if (total > first_audio_byte) return double(total - first_audio_byte) / double(dur);
      // Size not known yet (the head file plays before any CDN reply): the nominal bitrate.
      // The old fixed 20 (160 kbps) landed a 320 kbps seek at half the position, then decoded
      // and discarded the gap: 18 s to the first audio, and Spotify abandoned the transfer.
      int f = track->file.format;
      return f == OGG_VORBIS_320 ? 40.0 : f == OGG_VORBIS_96 ? 12.0 : 20.0;
    };
    auto do_seek = [&](uint32_t ms) {
      uint64_t dur = track->meta.duration_ms ? track->meta.duration_ms : 1;
      if (ms >= dur) ms = uint32_t(dur > 1000 ? dur - 1000 : 0);
      double aim = double(ms) - 1500.0;
      uint64_t off = aim <= 0 ? first_audio_byte : first_audio_byte + uint64_t(aim * bytes_per_ms());
      seek_target = uint64_t(ms) * kRate / 1000;
      pcm_reset(track, seek_target);
      pgen = pcm_.generation();
      pending.clear();
      pending_off = 0;
      discard = 0;
      seek_attempts = 0;
      need_offset_sync = true;
      fetch_from(off);
    };
    if (first_track && start_ms > 0) do_seek(start_ms);
    if (first_track) emit(start_playing ? PlayerEvent::Playing : PlayerEvent::Paused, uri);

    bool starving = false, eof = false, ok = true, ring_full = false;
    for (;;) {
      // --- commands
      Cmd c;
      // wait briefly only when the PCM ring was full on the last attempt
      while (pop_cmd(c, ring_full ? 10 : 0)) {
        ring_full = false;
        if (handle_play_pause(c)) continue;
        if (c.kind == Cmd::Seek) {
          std::shared_ptr<Resolved> audible;
          {
            std::lock_guard<std::mutex> l(mu_);
            audible = cur_;
          }
          if (audible != track) {
            // already pre-decoding the next track: restart the audible one at the target
            std::lock_guard<std::mutex> l(mu_);
            cmds_.push_front({Cmd::Load, audible ? audible->uri : uri, c.pos, playing_.load()});
            stb_vorbis_close(v);
            decoder_active_ = false;
            return false;
          }
          do_seek(c.pos);
          eof = false;
          continue;
        }
        {
          std::lock_guard<std::mutex> l(mu_);  // Load / Stop / Quit: hand back to run()
          cmds_.push_front(c);
        }
        stb_vorbis_close(v);
        decoder_active_ = false;
        return false;
      }
      if (quit_) {
        stb_vorbis_close(v);
        decoder_active_ = false;
        return false;
      }

      // --- move pending PCM into the ring (never blocks)
      if (pending_off < pending.size()) {
        size_t bytes = (pending.size() - pending_off) * 2;
        size_t w = pcm_.write_some((const uint8_t*)(pending.data() + pending_off), bytes, pgen, 4);
        if (pgen != pcm_.generation()) {
          pending.clear();
          pending_off = 0;
          continue;
        }
        uint64_t at;
        {
          std::lock_guard<std::mutex> l(seg_mu_);
          at = pcm_written_;
          pcm_written_ += w;
        }
        (void)at;
        pending_off += w / 2;
        if (pending_off < pending.size()) {
          ring_full = true;  // wait (commands polled above)
          continue;
        }
        ring_full = false;
        pending.clear();
        pending_off = 0;
      }

      // --- decode one frame
      int ch = 0, samples = 0;
      float** out = nullptr;
#if LISTENICA_DIAG
      auto dt0 = std::chrono::steady_clock::now();
#endif
      if (group_) group_frame(0, 0);  // the previous decode's output was not kept
      int used = in.size() > in_pos
                     ? stb_vorbis_decode_frame_pushdata(v, in.data() + in_pos, int(in.size() - in_pos), &ch, &out, &samples)
                     : 0;
      if (group_ && used > 0) {
        gframe.assign(in.begin() + long(in_pos), in.begin() + long(in_pos) + used);
        gframe_pending = true;
        group_end += uint64_t(used);
      }
#if LISTENICA_DIAG
      if (samples > 0) {  // decode speed (CPU time only: waits for data are excluded)
        dec_us += uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - dt0).count());
        dec_samples += uint64_t(samples);
        if (dec_samples >= 5 * uint64_t(kRate)) {  // every ~5 s of audio
          float x = dec_us ? float(dec_samples) / float(kRate) / (float(dec_us) / 1e6f) : 0;
          std::lock_guard<std::mutex> l(mu_);
          stats_.decode_x = stats_.decode_x > 0 ? 0.7f * stats_.decode_x + 0.3f * x : x;
          int f = track->file.format;
          stats_.decode_kbps = f == OGG_VORBIS_320 ? 320 : f == OGG_VORBIS_96 ? 96 : 160;
          dec_us = dec_samples = 0;
        }
      }
#endif
      if (used == 0) {
        if (eof) break;  // end of this track's data
        if (in_pos > 0) {
          in.erase(in.begin(), in.begin() + in_pos);
          in_pos = 0;
        }
        // no hysteresis here: decode whatever arrives; the output stage rebuffers
        int r = ring_.read(tmp, sizeof tmp, 100);
        if (r > 0) {
          in.insert(in.end(), tmp, tmp + r);
          if (starving) {
            starving = false;
            SLOGW(TAG, "network buffer recovered");
          }
        } else if (r < 0) {
          if (ring_.eof_error()) {
            ok = false;
            break;
          }
          eof = true;
        } else if (!starving && !first_audio) {
          starving = true;
          SLOGW(TAG, "network starved (decoder waiting for CDN data)");
        }
        if (in.size() > 512 * 1024) {
          std::lock_guard<std::mutex> l(mu_);
          stats_.decode_errors++;
          ok = false;
          break;
        }
        continue;
      }
      in_pos += size_t(used);
      if (samples <= 0) continue;
      int skip = 0;
      if (need_offset_sync) {
        int loc = stb_vorbis_get_sample_offset(v);
        if (loc < 0) continue;
        uint64_t frame_start = uint64_t(loc) > uint64_t(samples) ? uint64_t(loc) - samples : 0;
        SLOGD(TAG, "seek: target %llu ms, fetched from %llu (size %llu, %.1f B/ms), landed at %llu ms",
              (unsigned long long)(seek_target * 1000 / kRate), (unsigned long long)seek_off,
              (unsigned long long)file_size_.load(), bytes_per_ms(), (unsigned long long)(frame_start * 1000 / kRate));
        // Late: go back (sample-accurate). Up to 4 s early is decoded and discarded instead.
        // Re-aims reuse the probe's connection; the margin stays 1.5 s (a growing one made the
        // two rules bounce: 4 re-aims, 2026-11-05).
        if (frame_start > seek_target + kRate / 20 && seek_attempts < 3 && seek_off > first_audio_byte) {
          double back_ms = double(frame_start - seek_target) * 1000.0 / kRate + 1500.0;
          uint64_t back = uint64_t(back_ms * bytes_per_ms());
          seek_attempts++;
          seek_reaims_++;
          fetch_from(seek_off > first_audio_byte + back ? seek_off - back : first_audio_byte);
          continue;
        }
        // Landed well short (estimate too low): re-aim with the byte rate measured where it landed,
        // instead of decoding and discarding everything up to the target.
        if (frame_start + kRate * 4 < seek_target && seek_attempts < 3 && frame_start > kRate &&
            seek_off > first_audio_byte) {
          double measured = double(seek_off - first_audio_byte) / (double(frame_start) * 1000.0 / kRate);
          double aim = double(seek_target) * 1000.0 / kRate - 1500.0;
          seek_attempts++;
          seek_reaims_++;
          fetch_from(first_audio_byte + uint64_t(std::max(0.0, aim) * measured));
          continue;
        }
        need_offset_sync = false;
        seek_go_gen_ = fetch_gen_.load();  // landed: the fetcher streams on from the probe
        if (frame_start < seek_target) discard = seek_target - frame_start;
        {
          std::lock_guard<std::mutex> l(seg_mu_);
          if (!segs_.empty()) segs_.front().base_frames = frame_start + discard;
          base_frames_ = frame_start + discard;
        }
        emit(PlayerEvent::PositionCorrected, uri);
      }
      if (discard) {
        uint64_t d = std::min<uint64_t>(discard, uint64_t(samples));
        discard -= d;
        skip = int(d);
        if (skip >= samples) continue;
      }
      if (group_) group_frame(uint32_t(skip), uint32_t(samples - skip));
      // --- convert (normalisation only; volume is applied at the output)
      float g = norm_gain_ * 32767.0f;
      pending.resize(size_t(samples - skip) * 2);
      for (int i = skip; i < samples; i++) {
        float l = out[0][i];
        float r = ch > 1 ? out[1][i] : l;
#if LISTENICA_DIAG
        acc_peak = std::max(acc_peak, std::max(fabsf(l), fabsf(r)));
        acc_sq += (l * l + r * r) * 0.5f;
#endif
        auto cv = [](float x) -> int16_t {
          if (x > 32767.f) return 32767;
          if (x < -32768.f) return -32768;
          return int16_t(lrintf(x));
        };
        pending[size_t(i - skip) * 2] = cv(l * g);
        pending[size_t(i - skip) * 2 + 1] = cv(r * g);
      }
      pending_off = 0;
      acc_n += uint32_t(samples - skip);
      // Pace refills once ~1.5 s is buffered. Bursting MBs into PSRAM at full
      // speed saturates the shared flash/PSRAM bus on the ESP32-S3, which stalls
      // instruction fetch for the USB/I2S realtime tasks. ~4x realtime is plenty.
      if (cfg_.pace_decoder && pcm_.size() > size_t(kRate) * 4 * 3 / 2)
        sleep_ms(uint32_t((samples - skip) * 1000 / kRate / 4) + 1);
      if (first_audio) {
        first_audio = false;
        std::lock_guard<std::mutex> l(mu_);
        stats_.last_load_ms = uint32_t(now_ms() - load_t0_);
        // where the time went: track resolved / key + CDN urls ready / first CDN byte (0 = played
        // from the head file only), all since the load; plus seek re-aims
        auto since = [&](uint64_t t) { return t ? unsigned(t - load_t0_) : 0u; };
        SLOGI(TAG, "first audio decoded after %u ms (resolved %u, key+urls %u, first CDN byte %u, re-aims %d)",
              (unsigned)stats_.last_load_ms, since(t_resolved_), since(t_cdn_ready_), since(t_cdn_byte_), seek_reaims_.load());
      }
      if (acc_n >= kRate) {
        std::lock_guard<std::mutex> l(mu_);
#if LISTENICA_DIAG
        stats_.peak_dbfs_1s = db(acc_peak);
        stats_.rms_dbfs_1s = db(sqrtf(acc_sq / float(acc_n)));
        acc_sq = acc_peak = 0;
#endif
        stats_.frames_out += acc_n;
        acc_n = 0;
      }
    }
    stb_vorbis_close(v);
    if (!ok) return fail("stream failed");

    // push the tail of this track into the ring before switching
    while (pending_off < pending.size() && !quit_) {
      size_t w = pcm_.write_some((const uint8_t*)(pending.data() + pending_off), (pending.size() - pending_off) * 2, pgen, 4);
      if (pgen != pcm_.generation()) break;
      uint64_t at;
      {
        std::lock_guard<std::mutex> l(seg_mu_);
        at = pcm_written_;
        pcm_written_ += w;
      }
      (void)at;
      pending_off += w / 2;
      if (pending_off < pending.size()) {
        Cmd c;
        if (pop_cmd(c, 10) && !handle_play_pause(c)) {
          {
            std::lock_guard<std::mutex> l(mu_);
            cmds_.push_front(c);
          }
          decoder_active_ = false;
          return false;
        }
      }
    }
    pending.clear();
    pending_off = 0;

    // --- gapless: continue with the next track in the same PCM stream
    std::string next;
    {
      std::lock_guard<std::mutex> l(mu_);
      next = next_uri_;
      next_uri_.clear();
    }
    std::shared_ptr<Resolved> nt = next.empty() ? nullptr : take_resolved(next);
    {
      std::lock_guard<std::mutex> l(seg_mu_);
      if (nt) {
        segs_.push_back({pcm_written_, nt, 0});
        SLOGI(TAG, "gapless: '%s' queued after the current track", nt->meta.name.c_str());
      } else {
        segs_.push_back({pcm_written_, nullptr, 0});  // the output emits EndOfTrack here
      }
    }
    track = nt;
    first_track = false;
  }
  decoder_active_ = false;
  return true;
}

}  // namespace listenica
