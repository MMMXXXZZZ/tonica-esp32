#include "listenica/group.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include "listenica/adpcm.h"
#include "mbedtls/net_sockets.h"
#define STB_VORBIS_NO_STDIO 1
#define STB_VORBIS_NO_PULLDATA_API 1
#define STB_VORBIS_NO_INTEGER_CONVERSION 1
extern "C" {
#include "stb_vorbis.h"
}
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#elif defined(ESP_PLATFORM)
#include <lwip/sockets.h>
#else  // macOS, Linux
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#endif

static const char* TAG = "group";

namespace listenica {

using namespace group;

// ---------------------------------------------------------------- framing
static void put_u32(std::vector<uint8_t>& b, uint32_t v) {
  for (int i = 0; i < 4; i++) b.push_back(uint8_t(v >> (8 * i)));
}
static void put_u64(std::vector<uint8_t>& b, uint64_t v) {
  for (int i = 0; i < 8; i++) b.push_back(uint8_t(v >> (8 * i)));
}
static uint32_t get_u32(const uint8_t* p) {
  uint32_t v = 0;
  for (int i = 3; i >= 0; i--) v = v << 8 | p[i];
  return v;
}
static uint64_t get_u64(const uint8_t* p) {
  uint64_t v = 0;
  for (int i = 7; i >= 0; i--) v = v << 8 | p[i];
  return v;
}

static bool send_all(mbedtls_net_context* n, const uint8_t* d, size_t len) {
  while (len) {
    int r = mbedtls_net_send(n, d, len);
    if (r <= 0) return false;
    d += r;
    len -= size_t(r);
  }
  return true;
}

static bool send_msg(mbedtls_net_context* n, uint8_t type, const std::vector<uint8_t>& head, const uint8_t* body = nullptr,
                     size_t body_len = 0) {
  std::vector<uint8_t> h;
  h.reserve(5 + head.size());
  h.push_back(type);
  put_u32(h, uint32_t(head.size() + body_len));
  h.insert(h.end(), head.begin(), head.end());
  return send_all(n, h.data(), h.size()) && (!body_len || send_all(n, body, body_len));
}

// 1: got it, 0: timeout, -1: closed or error
static int recv_exact(mbedtls_net_context* n, uint8_t* d, size_t len, uint32_t timeout_ms) {
  size_t got = 0;
  while (got < len) {
    int r = mbedtls_net_recv_timeout(n, d + got, len - got, timeout_ms);
    if (r == MBEDTLS_ERR_SSL_TIMEOUT) {
      if (got == 0) return 0;
      continue;  // mid-message: keep waiting
    }
    if (r <= 0) return -1;
    got += size_t(r);
  }
  return 1;
}

static int recv_msg(mbedtls_net_context* n, uint8_t& type, std::vector<uint8_t>& payload, uint32_t timeout_ms) {
  uint8_t h[5];
  int r = recv_exact(n, h, 5, timeout_ms);
  if (r <= 0) return r;
  type = h[0];
  uint32_t len = get_u32(h + 1);
  if (len > 1024 * 1024) return -1;
  payload.resize(len);
  return len ? (recv_exact(n, payload.data(), len, 5000) > 0 ? 1 : -1) : 1;
}

// A follower that stops taking data (gone, or its link stalled) must not hold the leader: its sends
// time out and the connection closes, so it rejoins fresh instead of being served for minutes.
static void send_timeout(mbedtls_net_context* n, uint32_t ms) {
#ifdef _WIN32
  DWORD t = ms;
  setsockopt(n->fd, SOL_SOCKET, SO_SNDTIMEO, (const char*)&t, sizeof t);
#else
  struct timeval t = {};
  t.tv_sec = decltype(t.tv_sec)(ms / 1000);
  t.tv_usec = decltype(t.tv_usec)((ms % 1000) * 1000);  // int on macOS, long elsewhere
  setsockopt(n->fd, SOL_SOCKET, SO_SNDTIMEO, &t, sizeof t);
#endif
}

static void no_delay(mbedtls_net_context* n) {
  int one = 1;
  setsockopt(n->fd, IPPROTO_TCP, TCP_NODELAY, (const char*)&one, sizeof one);
}

// ---------------------------------------------------------------- leader
// The log: serialized wire records ([type u8][len u32][payload]), circular, absolute offsets.
// Kept always (512 KB of PSRAM, ~12 s at 320 kbps) so a follower that joins starts at once from
// the frames around the play position instead of waiting for what the decoder makes next.
GroupServer::GroupServer(size_t log_bytes) : cap_(log_bytes) {}

GroupServer::~GroupServer() {}

bool GroupServer::start() {
  auto* l = new mbedtls_net_context;
  mbedtls_net_init(l);
  // IPv4 explicitly: with no address, Windows binds the IPv6 wildcard only (V6ONLY), and followers
  // connect over IPv4 (the board could not reach a PC leader, 2026-11-05)
  int r = mbedtls_net_bind(l, "0.0.0.0", kPort, MBEDTLS_NET_PROTO_TCP);
  if (r != 0) {
    SLOGW(TAG, "bind :%s failed (-0x%04x)", kPort, -r);
    delete l;
    return false;
  }
  listen_ = l;
  spawn_thread("group_accept", 6144, true, 3, &GroupServer::accept_entry, this);
  SLOGI(TAG, "Sinfonica leader listening on :%s", kPort);
  return true;
}

void GroupServer::accept_entry(void* self) { static_cast<GroupServer*>(self)->accept_loop(); }

void GroupServer::accept_loop() {
  auto* l = static_cast<mbedtls_net_context*>(listen_);
  for (;;) {
    auto* c = new mbedtls_net_context;
    mbedtls_net_init(c);
    unsigned char ip[16];
    size_t ip_len = 0;
    if (mbedtls_net_accept(l, c, ip, sizeof ip, &ip_len) != 0) {
      delete c;
      sleep_ms(500);
      continue;
    }
    no_delay(c);
    char ips[16] = "";
    if (ip_len == 4) snprintf(ips, sizeof ips, "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
    spawn_thread("group_conn", 6144, true, 4, &GroupServer::conn_entry, new Conn{this, c, ips});
  }
}

void GroupServer::conn_entry(void* arg) {
  Conn* cn = static_cast<Conn*>(arg);
  auto* n = static_cast<mbedtls_net_context*>(cn->net);
  uint8_t type = 0;
  std::vector<uint8_t> p;
  GroupServer* self = cn->self;
  if (recv_msg(n, type, p, 3000) > 0 && type == Hello && p.size() >= 1) {
    if (p[0] == 2) {  // one-shot: Ask -> Status
      if (recv_msg(n, type, p, 3000) > 0 && type == Ask && p.size() >= 1) {
        GroupStatus st = self->hooks_.status ? self->hooks_.status() : GroupStatus{};
        std::vector<uint8_t> b{uint8_t(st.playing), uint8_t(st.following), uint8_t(std::min(st.followers, 255))};
        send_msg(n, Status, b);
        if (p[0] == 2 && self->hooks_.invited && !cn->ip.empty()) {
          SLOGI(TAG, "invited to follow %s", cn->ip.c_str());
          self->hooks_.invited(cn->ip);
        }
      }
    } else {
      {
        std::lock_guard<std::mutex> l(self->fds_mu_);
        self->fds_.push_back(n->fd);
      }
      if (p[0] == 1) self->time_loop(n, std::string(p.begin() + 1, p.end()));
      else {
        send_timeout(n, 4000);
        self->data_loop(n, std::string(p.begin() + 1, p.end()), cn->ip);
      }
      std::lock_guard<std::mutex> l(self->fds_mu_);
      self->fds_.erase(std::remove(self->fds_.begin(), self->fds_.end(), n->fd), self->fds_.end());
    }
  }
  mbedtls_net_free(n);
  delete n;
  delete cn;
}

void GroupServer::push_voice(const std::string& target, std::vector<uint8_t> rec) {
  std::lock_guard<std::mutex> l(mu_);
  voices_.push_back({++voice_seq_, target, std::move(rec)});
  while (voices_.size() > 6) voices_.pop_front();
  cv_.notify_all();
}

void GroupServer::voice_clip(uint32_t id, const std::string& target, const int16_t* pcm, size_t n, uint32_t rate) {
  if (clients_ == 0 || !n) return;
  std::vector<uint8_t> enc = adpcm_encode(pcm, n);
  std::vector<uint8_t> b;
  put_u32(b, id);
  put_u32(b, rate);
  b.push_back(uint8_t(std::min<size_t>(target.size(), 255)));
  b.insert(b.end(), target.begin(), target.begin() + std::min<size_t>(target.size(), 255));
  std::vector<uint8_t> rec;
  rec.push_back(VoiceClip);
  put_u32(rec, uint32_t(b.size() + enc.size()));
  rec.insert(rec.end(), b.begin(), b.end());
  rec.insert(rec.end(), enc.begin(), enc.end());
  push_voice(target, std::move(rec));
}

void GroupServer::voice_start(uint32_t id, const std::string& target, int64_t at_us) {
  if (clients_ == 0) return;
  std::vector<uint8_t> b;
  put_u32(b, id);
  put_u64(b, uint64_t(at_us));
  std::vector<uint8_t> rec;
  rec.push_back(VoiceStart);
  put_u32(rec, uint32_t(b.size()));
  rec.insert(rec.end(), b.begin(), b.end());
  push_voice(target, std::move(rec));
}

void GroupServer::dismiss_followers() {
  std::lock_guard<std::mutex> l(fds_mu_);
  for (int fd : fds_) shutdown(fd, 2);  // their loops fail and close (SHUT_RDWR / SD_BOTH)
}

// ---------------------------------------------------------------- one-shot requests
static bool ask(const std::string& host, uint8_t op, GroupStatus* out, uint32_t timeout_ms) {
  mbedtls_net_context n;
  mbedtls_net_init(&n);
  bool ok = false;
  if (mbedtls_net_connect(&n, host.c_str(), kPort, MBEDTLS_NET_PROTO_TCP) == 0 && send_msg(&n, Hello, {2}) &&
      send_msg(&n, Ask, {op})) {
    uint8_t type = 0;
    std::vector<uint8_t> p;
    if (recv_msg(&n, type, p, timeout_ms) > 0 && type == Status && p.size() >= 3) {
      if (out) {
        out->playing = p[0] != 0;
        out->following = p[1] != 0;
        out->followers = p[2];
      }
      ok = true;
    }
  }
  mbedtls_net_free(&n);
  return ok;
}

bool group_status(const std::string& host, GroupStatus& out, uint32_t timeout_ms) { return ask(host, 1, &out, timeout_ms); }
bool group_invite(const std::string& host, uint32_t timeout_ms) { return ask(host, 2, nullptr, timeout_ms); }

void GroupServer::time_loop(void* net, const std::string& from) {
  auto* n = static_cast<mbedtls_net_context*>(net);
  uint8_t type;
  std::vector<uint8_t> p;
  for (;;) {
    int r = recv_msg(n, type, p, 30000);
    if (r <= 0) return;
    if (type == Control) {
      if (hooks_.control) hooks_.control(from, std::string(p.begin(), p.end()));
      continue;
    }
    if (type != TimeReq || p.size() < 8) continue;
    std::vector<uint8_t> out(p.begin(), p.begin() + 8);
    put_u64(out, uint64_t(now_us()));  // as late as possible before the send
    if (!send_msg(n, TimeResp, out)) return;
  }
}

void GroupServer::log_copy(uint64_t at, uint8_t* out, size_t n) const {
  for (size_t i = 0; i < n;) {
    size_t a = size_t((at + i) % cap_), run = std::min(n - i, cap_ - a);
    memcpy(out + i, log_.data() + a, run);
    i += run;
  }
}

void GroupServer::append(uint8_t type, const std::vector<uint8_t>& head, const uint8_t* body, size_t body_len) {
  // caller holds mu_
  if (log_.empty()) log_.resize(cap_);  // PSRAM on the board (large allocation)
  size_t len = 5 + head.size() + body_len;
  if (len > cap_ / 4) return;
  while (log_fill_ + len > cap_) {  // drop the oldest records
    uint8_t h[5];
    log_copy(log_head_ - log_fill_, h, 5);
    log_fill_ -= 5 + get_u32(h + 1);
  }
  std::vector<uint8_t> rec;
  rec.reserve(len);
  rec.push_back(type);
  put_u32(rec, uint32_t(head.size() + body_len));
  rec.insert(rec.end(), head.begin(), head.end());
  if (body_len) rec.insert(rec.end(), body, body + body_len);
  for (size_t i = 0; i < len;) {
    size_t a = size_t((log_head_ + i) % cap_), run = std::min(len - i, cap_ - a);
    memcpy(log_.data() + a, rec.data() + i, run);
    i += run;
  }
  log_head_ += len;
  log_fill_ += len;
  cv_.notify_all();
}

void GroupServer::vorbis_header(float norm_gain, const uint8_t* d, size_t n) {
  std::vector<uint8_t> h;
  std::lock_guard<std::mutex> l(mu_);
  uint32_t id = ++hdr_id_;
  put_u32(h, id);
  uint32_t g;
  memcpy(&g, &norm_gain, 4);
  put_u32(h, g);
  std::vector<uint8_t> rec;  // kept apart too: a follower needs it long after it left the log
  rec.push_back(VorbisHeader);
  put_u32(rec, uint32_t(h.size() + n));
  rec.insert(rec.end(), h.begin(), h.end());
  rec.insert(rec.end(), d, d + n);
  hdrs_.push_back({id, std::move(rec)});
  while (hdrs_.size() > 4) hdrs_.erase(hdrs_.begin());
}

void GroupServer::vorbis_frame(uint32_t gen, uint64_t pos, uint32_t skip, uint32_t kept, bool flush, uint64_t end,
                               const uint8_t* d, size_t n) {
  std::vector<uint8_t> h;
  put_u32(h, gen);
  put_u64(h, pos);
  put_u32(h, skip);
  put_u32(h, kept);
  std::lock_guard<std::mutex> l(mu_);
  put_u32(h, hdr_id_);
  h.push_back(flush ? 1 : 0);
  put_u64(h, end);
  append(VorbisFrame, h, d, n);
}

void GroupServer::data_loop(void* net, const std::string& from, const std::string& ip) {
  auto* n = static_cast<mbedtls_net_context*>(net);
  clients_++;
  if (hooks_.clients_changed) hooks_.clients_changed();
  SLOGI(TAG, "follower %s connected (%d)", ip.c_str(), clients_.load());
  uint32_t sent_seq = 0, client_hdr = 0;
  uint32_t voice_sent = voice_seq_;  // voices from before it joined are not replayed
  bool need_flush = true;
  uint64_t cur = 0;
  bool started = false;
  std::vector<uint8_t> out, rec;
  // Pace: send frames at most kAhead past what the leader is playing, and never what has played.
  // Sending as fast as the decoder runs (~4x realtime) saturated the board's Wi-Fi with PCM:
  // its CDN download starved, pings took 50-160 ms, a follower fell behind for good (2026-11-05).
  const uint64_t kBehind = kRate * kFrameBytes / 2, kAhead = 2 * kRate * kFrameBytes;
  for (;;) {
    out.clear();
    {
      std::unique_lock<std::mutex> l(mu_);
      if (!started) {  // from the oldest record: frames before the play position are skipped below
        cur = log_head_ - log_fill_;
        started = true;
      }
      auto sendable = [&]() -> bool {
        if (cur >= log_head_) return false;
        uint8_t hh[5 + 12];
        log_copy(cur, hh, sizeof hh);
        if (hh[0] != VorbisFrame) return true;
        uint32_t g = get_u32(hh + 5);
        uint64_t p = get_u64(hh + 9);
        return !(anchor_.seq && g == anchor_.gen && p > anchor_.pos + kAhead);
      };
      cv_.wait_for(l, std::chrono::milliseconds(200),
                   [&] { return anchor_.seq != sent_seq || voice_seq_ != voice_sent || sendable(); });
      for (auto& vr : voices_) {  // spoken notifications first: they are due within ~0.2 s
        if (int32_t(vr.seq - voice_sent) <= 0) continue;
        if (vr.target.empty() || vr.target == from) out.insert(out.end(), vr.rec.begin(), vr.rec.end());
      }
      voice_sent = voice_seq_;
      if (anchor_.seq != sent_seq) {
        sent_seq = anchor_.seq;
        std::vector<uint8_t> b;
        put_u32(b, anchor_.gen);
        put_u64(b, anchor_.pos);
        put_u64(b, uint64_t(anchor_.at_us));
        b.push_back(anchor_.playing ? 1 : 0);
        b.push_back(uint8_t(anchor_.volume));
        b.push_back(uint8_t(anchor_.volume >> 8));
        out.push_back(group::Msg::Anchor);
        put_u32(out, uint32_t(b.size()));
        out.insert(out.end(), b.begin(), b.end());
      }
      if (cur < log_head_ - log_fill_) {  // fell out of the log
        cur = log_head_ - log_fill_;
        need_flush = true;
      }
      while (cur < log_head_ && out.size() < 16384) {
        uint8_t hh[5];
        log_copy(cur, hh, 5);
        uint32_t len = get_u32(hh + 1);
        if (hh[0] != VorbisFrame || len < 33) {
          cur += 5 + len;
          continue;
        }
        rec.resize(5 + len);
        log_copy(cur, rec.data(), rec.size());
        const uint8_t* p = rec.data() + 5;
        uint32_t g = get_u32(p), skip = get_u32(p + 12), kept = get_u32(p + 16), hdr = get_u32(p + 20);
        uint64_t pos = get_u64(p + 4);
        (void)skip;
        bool same = anchor_.seq && g == anchor_.gen;
        if (anchor_.seq && (int32_t(g - anchor_.gen) < 0 ||
                            (same && pos + uint64_t(kept) * kFrameBytes + kBehind < anchor_.pos))) {
          cur += 5 + len;  // already played (or an older stream): skip it
          need_flush = true;
          continue;
        }
        if (same && pos > anchor_.pos + kAhead) break;  // pacing: later
        if (hdr != client_hdr) {  // this frame's decoder setup first
          const std::vector<uint8_t>* hr = nullptr;
          for (auto& x : hdrs_)
            if (x.id == hdr) hr = &x.rec;
          if (!hr) {
            cur += 5 + len;
            need_flush = true;
            continue;
          }
          out.insert(out.end(), hr->begin(), hr->end());
          client_hdr = hdr;
          need_flush = false;  // a fresh decoder
        } else if (need_flush) {
          out.push_back(VorbisFlush);
          put_u32(out, 0);
          need_flush = false;
        }
        out.insert(out.end(), rec.begin(), rec.end());
        cur += 5 + len;
      }
    }
    if (!out.empty() && !send_all(n, out.data(), out.size())) {
      SLOGW(TAG, "follower %s: sending failed or timed out; dropping it", ip.c_str());
      break;
    }
  }
  clients_--;
  if (hooks_.clients_changed) hooks_.clients_changed();
  SLOGI(TAG, "follower %s left (%d)", ip.c_str(), clients_.load());
}

void GroupServer::anchor(uint32_t gen, uint64_t pos, int64_t at_us, bool playing, uint16_t volume) {
  if (clients_ == 0) return;
  int64_t now = now_us();
  std::lock_guard<std::mutex> l(mu_);
  bool changed = gen != anchor_.gen || playing != anchor_.playing || volume != anchor_.volume;
  if (!changed && now - last_anchor_us_ < 100000) return;  // ~10 a second is plenty
  last_anchor_us_ = now;
  anchor_.gen = gen;
  anchor_.pos = pos;
  anchor_.at_us = at_us;
  anchor_.playing = playing;
  anchor_.volume = volume;
  anchor_.seq++;
  cv_.notify_all();
}

// ---------------------------------------------------------------- follower
bool GroupClient::connect(const std::string& host, const std::string& device_id) {
  auto* d = new mbedtls_net_context;
  auto* t = new mbedtls_net_context;
  mbedtls_net_init(d);
  mbedtls_net_init(t);
  if (mbedtls_net_connect(d, host.c_str(), kPort, MBEDTLS_NET_PROTO_TCP) != 0 ||
      mbedtls_net_connect(t, host.c_str(), kPort, MBEDTLS_NET_PROTO_TCP) != 0) {
    SLOGW(TAG, "cannot reach the leader at %s:%s", host.c_str(), kPort);
    return false;
  }
  no_delay(d);
  no_delay(t);
  std::vector<uint8_t> hd{0}, ht{1};
  hd.insert(hd.end(), device_id.begin(), device_id.end());
  ht.insert(ht.end(), device_id.begin(), device_id.end());
  if (!send_msg(d, Hello, hd) || !send_msg(t, Hello, ht)) return false;
  data_ = d;
  time_ = t;
  alive_ = true;
  threads_ = 2;
  spawn_thread("gc_data", 8192, true, 5, &GroupClient::data_entry, this);  // PSRAM stacks: no flash I/O here
  spawn_thread("gc_time", 6144, true, 6, &GroupClient::time_entry, this);
  return true;
}

void GroupClient::data_entry(void* s) { static_cast<GroupClient*>(s)->data_loop(); }
void GroupClient::time_entry(void* s) { static_cast<GroupClient*>(s)->time_loop(); }

void GroupClient::time_loop() {
  auto* n = static_cast<mbedtls_net_context*>(time_);
  uint8_t type;
  std::vector<uint8_t> p;
  while (alive_) {
    std::vector<uint8_t> req;
    int64_t t0 = now_us();
    put_u64(req, uint64_t(t0));
    {
      std::lock_guard<std::mutex> l(tsend_mu_);
      if (!send_msg(n, TimeReq, req)) break;
    }
    int r = recv_msg(n, type, p, 2000);
    int64_t t1 = now_us();
    if (r < 0) break;
    if (r > 0 && type == TimeResp && p.size() >= 16 && int64_t(get_u64(p.data())) == t0) {
      int64_t ts = int64_t(get_u64(p.data() + 8));
      Sample s{t1 - t0, ts - (t0 + (t1 - t0) / 2)};
      std::lock_guard<std::mutex> l(mu_);
      samples_.push_back(s);
      if (samples_.size() > 80) samples_.erase(samples_.begin());
    }
    sleep_ms(100);
  }
  alive_ = false;
  threads_--;
}

bool GroupClient::control(const std::string& command) {
  std::lock_guard<std::mutex> l(tsend_mu_);
  if (!alive_ || !time_) return false;
  return send_msg(static_cast<mbedtls_net_context*>(time_), Control, std::vector<uint8_t>(command.begin(), command.end()));
}

bool GroupClient::offset_us(int64_t& off, int64_t* rtt) {
  std::lock_guard<std::mutex> l(mu_);
  if (samples_.size() < 5) return false;
  const Sample* best = &samples_[0];
  for (auto& s : samples_)
    if (s.rtt < best->rtt) best = &s;
  off = best->off;
  if (rtt) *rtt = best->rtt;
  return true;
}

// Decoded PCM into the circular buffer at stream byte `pos` (caller holds mu_).
void GroupClient::store_pcm(uint32_t g, uint64_t pos, const uint8_t* d, size_t len) {
  if (buf_.empty()) buf_.resize(kCap);  // PSRAM on the board
  if (g == gen_ && pos < head_) {  // overlap: keep only what is new
    if (pos + len <= head_) return;
    size_t skip = size_t(head_ - pos);
    d += skip;
    len -= skip;
    pos = head_;
  }
  if (g != gen_ || pos != head_) {  // a new stream, or a skip forward: restart here
    gen_ = g;
    head_ = pos;
    fill_ = 0;
  }
  for (size_t i = 0; i < len;) {
    size_t at = size_t((head_ + i) % kCap), run = std::min(len - i, kCap - at);
    memcpy(buf_.data() + at, d + i, run);
    i += run;
  }
  head_ += len;
  fill_ = std::min<uint64_t>(fill_ + len, kCap);
}

// Replays the leader's decoder: the same bytes in the same order give the same packets, so each
// decode here matches the next frame's metadata (where its output goes, how much of it to keep).
void GroupClient::data_loop() {
  auto* n = static_cast<mbedtls_net_context*>(data_);
  uint8_t type;
  std::vector<uint8_t> p;
  stb_vorbis* v = nullptr;
  uint32_t hdr = 0;
  float norm = 1.0f;
  std::vector<uint8_t> in;  // Vorbis bytes not yet decoded (from in_pos)
  size_t in_pos = 0;
  struct Meta {
    uint32_t gen;
    uint64_t pos;
    uint32_t skip, kept;
    uint64_t end;
  };
  std::deque<Meta> meta;
  std::vector<int16_t> pcm;
  uint64_t consumed = 0;  // decoder input position, on the leader's scale (`end`)
  bool based = false;     // consumed set from the first frame after a setup/flush
  auto reset_input = [&] {
    in.clear();
    in_pos = 0;
    meta.clear();
    based = false;
  };
  while (alive_) {
    int r = recv_msg(n, type, p, 5000);
    if (r < 0) break;
    if (r == 0) continue;
    if (type == group::Msg::Anchor && p.size() >= 23) {
      std::lock_guard<std::mutex> l(mu_);
      anchor_.gen = get_u32(p.data());
      anchor_.pos = get_u64(p.data() + 4);
      anchor_.at_us = int64_t(get_u64(p.data() + 12));
      anchor_.playing = p[20] != 0;
      anchor_.volume = uint16_t(p[21] | p[22] << 8);
      anchor_.recv_us = now_us();
    } else if (type == VorbisHeader && p.size() > 8) {
      if (v) stb_vorbis_close(v);
      int used = 0, err = 0;
      v = stb_vorbis_open_pushdata(p.data() + 8, int(p.size() - 8), &used, &err, nullptr);
      hdr = v ? get_u32(p.data()) : 0;
      uint32_t g = get_u32(p.data() + 4);
      memcpy(&norm, &g, 4);
      reset_input();
      if (!v) SLOGW(TAG, "follower: Vorbis header rejected (%d)", err);
    } else if (type == VoiceClip && p.size() >= 9 && p.size() >= size_t(9 + p[8])) {
      size_t at = 9 + p[8];
      uint32_t rate = get_u32(p.data() + 4);
      std::vector<int16_t> src = adpcm_decode(p.data() + at, p.size() - at);
      auto vc = std::make_shared<Voice>();
      vc->id = get_u32(p.data());
      if (rate && !src.empty()) {  // to 44.1 kHz, as the leader's player does (linear: speech)
        size_t out_n = size_t(uint64_t(src.size()) * kRate / rate);
        vc->pcm.resize(out_n);
        for (size_t i = 0; i < out_n; i++) {
          uint64_t q = uint64_t(i) * rate * 65536 / kRate;
          size_t k = size_t(q >> 16);
          int32_t f = int32_t(q & 0xffff);
          int32_t a = src[std::min(k, src.size() - 1)], b = src[std::min(k + 1, src.size() - 1)];
          vc->pcm[i] = int16_t(a + (((b - a) * f) >> 16));
        }
      }
      std::lock_guard<std::mutex> l(mu_);
      voice_ = vc;
    } else if (type == VoiceStart && p.size() >= 12) {
      std::lock_guard<std::mutex> l(mu_);
      if (voice_ && voice_->id == get_u32(p.data())) voice_->at_us = int64_t(get_u64(p.data() + 4));
    } else if (type == VorbisFlush) {
      if (v) stb_vorbis_flush_pushdata(v);
      reset_input();
    } else if (type == VorbisFrame && p.size() >= 33) {
      const uint8_t* q = p.data();
      if (!v || get_u32(q + 20) != hdr) continue;  // no decoder for it
      if (q[24]) {  // the leader flushed its decoder before this frame (a seek)
        stb_vorbis_flush_pushdata(v);
        reset_input();
      }
      uint64_t end = get_u64(q + 25);
      size_t len = p.size() - 33;
      if (!based) {  // the first frame after a setup or flush: its start is our input's start
        consumed = end - len;
        based = true;
      }
      meta.push_back({get_u32(q), get_u64(q + 4), get_u32(q + 12), get_u32(q + 16), end});
      in.insert(in.end(), q + 33, q + p.size());
      while (in.size() > in_pos) {
        int ch = 0, samples = 0;
        float** out = nullptr;
        int used = stb_vorbis_decode_frame_pushdata(v, in.data() + in_pos, int(in.size() - in_pos), &ch, &out, &samples);
        if (used == 0) break;  // needs the next frame's bytes too
        in_pos += size_t(used);
        consumed += uint64_t(used);
        while (!meta.empty() && meta.front().end < consumed) meta.pop_front();  // passed over (a resync)
        if (meta.empty() || meta.front().end != consumed) continue;  // not on a frame boundary: no output
        Meta m = meta.front();
        meta.pop_front();
        if (samples <= int(m.skip) || !m.kept) continue;
        size_t keep = std::min<size_t>(m.kept, size_t(samples) - m.skip);
        pcm.resize(keep * 2);
        float g = norm * 32767.0f;  // as the leader converts (Player::decode_track)
        auto cv = [](float x) -> int16_t {
          if (x > 32767.f) return 32767;
          if (x < -32768.f) return -32768;
          return int16_t(lrintf(x));
        };
        for (size_t i = 0; i < keep; i++) {
          float l = out[0][m.skip + i], rr = ch > 1 ? out[1][m.skip + i] : l;
          pcm[i * 2] = cv(l * g);
          pcm[i * 2 + 1] = cv(rr * g);
        }
        std::lock_guard<std::mutex> l(mu_);
        store_pcm(m.gen, m.pos, (const uint8_t*)pcm.data(), keep * kFrameBytes);
      }
      if (in_pos > 65536) {  // drop what was decoded
        in.erase(in.begin(), in.begin() + long(in_pos));
        in_pos = 0;
      }
    }
  }
  if (v) stb_vorbis_close(v);
  alive_ = false;
  threads_--;
}

std::shared_ptr<GroupClient::Voice> GroupClient::voice() {
  std::lock_guard<std::mutex> l(mu_);
  return voice_;
}

GroupClient::Anchor GroupClient::anchor() {
  std::lock_guard<std::mutex> l(mu_);
  return anchor_;
}

size_t GroupClient::read(uint32_t gen, uint64_t pos, uint8_t* out, size_t n) {
  std::lock_guard<std::mutex> l(mu_);
  if (gen != gen_ || buf_.empty() || pos < head_ - fill_ || pos >= head_) return 0;
  size_t take = size_t(std::min<uint64_t>(n, head_ - pos));
  for (size_t i = 0; i < take;) {
    size_t at = size_t((pos + i) % kCap), run = std::min(take - i, kCap - at);
    memcpy(out + i, buf_.data() + at, run);
    i += run;
  }
  return take;
}

void GroupClient::close() {
  alive_ = false;
  if (data_) shutdown(static_cast<mbedtls_net_context*>(data_)->fd, 2);  // wakes the blocked threads (SHUT_RDWR / SD_BOTH)
  if (time_) shutdown(static_cast<mbedtls_net_context*>(time_)->fd, 2);
  while (threads_ > 0) sleep_ms(20);
  std::lock_guard<std::mutex> l(tsend_mu_);  // control() may be sending
  for (void* n : {data_, time_}) {
    if (!n) continue;
    mbedtls_net_free(static_cast<mbedtls_net_context*>(n));
    delete static_cast<mbedtls_net_context*>(n);
  }
  data_ = time_ = nullptr;
}

// ---------------------------------------------------------------- follower playback
void GroupFollower::run(const std::atomic<bool>& stop, uint32_t log_ms) {
  // The sink may be shared with this speaker's own player, which pauses it when its own session
  // pauses (waveOutPause / AudioQueuePause). Followers write into it: it must run. A host whose
  // session was paused before it joined stayed silent (2026-11-05).
  sink_->set_paused(false);
  const uint32_t kBlock = 441;  // 10 ms
  int64_t cur = -1;             // stream byte we play next (-1: not synced)
  uint32_t cur_gen = 0;
  int64_t last_anchor_recv = 0;
  std::vector<int64_t> starts;  // recent anchors' apparent stream start times (leader us)
  uint64_t last_log = now_ms();
  int dup = 0, drop = 0, jumps = 0, under = 0;
  bool primed = false;  // audio of the current stream has arrived
  double err_sum = 0, err_abs_max = 0;
  int err_n = 0;
  uint16_t hw_vol = 0;
  bool hw_vol_set = false;
  std::vector<uint8_t> tmp((kBlock + 2) * 4);
  std::vector<int16_t> out(kBlock * 2);
  // Spoken notifications, mixed as the leader's player does: music ducked to -12 dB from ~0.2 s
  // before the voice (the leader speaks once its duck is down), back up slower after a 0.4 s tail.
  std::shared_ptr<GroupClient::Voice> vc;
  const uint32_t kDuck = 16384, kDuckStep = 65535 / (kRate / 4) + 1;
  uint32_t duck = 65535;
  auto voice_sample = [&](int64_t t_leader) -> int32_t {  // advances the duck by one frame
    int64_t at = vc ? vc->at_us.load() : 0;
    int32_t v = 0;
    bool window = false;
    if (at) {
      int64_t k = (t_leader - at) * int64_t(kRate) / 1000000, n = int64_t(vc->pcm.size());
      window = k >= -int64_t(kRate) / 5 && k < n + int64_t(kRate) * 4 / 10;
      if (k >= 0 && k < n) v = vc->pcm[size_t(k)];
    }
    uint32_t t = window ? kDuck : 65535;
    if (duck > t) duck = duck > t + kDuckStep ? duck - kDuckStep : t;
    else if (duck < t) duck = std::min<uint32_t>(t, duck + kDuckStep / 2);
    return v;
  };
  // An unstable link: more than 200 ms of dropouts (or forced re-syncs) within 30 s while
  // playing. Reported once per connection; the leader decides whether to say so.
  int window_bad = 0;
  uint64_t window_start = now_ms();
  bool reported = false;
  while (!stop && gc_->alive()) {
    vc = gc_->voice();
    // when this block starts to be heard, our clock
    int64_t t_local = now_us() + int64_t(sink_->latency_frames()) * 1000000 / kRate + offset_us_;
    int64_t off = 0;
    bool have_clock = gc_->offset_us(off);
    GroupClient::Anchor a = gc_->anchor();
    if (a.recv_us != last_anchor_recv) {  // a new anchor: its apparent stream start time
      last_anchor_recv = a.recv_us;
      if (a.gen != cur_gen || !a.playing) starts.clear();
      if (a.playing) {
        starts.push_back(a.at_us - int64_t(a.pos / 4) * 1000000 / kRate);
        if (starts.size() > 30) starts.erase(starts.begin());
      }
    }
    bool active = have_clock && a.playing && !starts.empty() && now_us() - a.recv_us < 1500000;
    if (!active) {
      std::fill(out.begin(), out.end(), int16_t(0));
      cur = -1;
      if (have_clock && vc && vc->at_us.load()) {  // a notification while the music is paused
        uint32_t g = sink_->hw_volume_active() ? 0xFFFF : uint32_t((uint64_t(a.volume) * a.volume) >> 16);
        for (uint32_t i = 0; i < kBlock; i++) {
          int32_t v = voice_sample(t_local + off + int64_t(i) * 1000000 / kRate);
          out[i * 2] = out[i * 2 + 1] = int16_t((v * int32_t(g)) >> 16);
        }
      }
    } else {
      // trimmed mean of the recent apparent start times: anchors jitter with the leader's sink level
      std::vector<int64_t> s = starts;
      std::sort(s.begin(), s.end());
      size_t cut = s.size() / 4;
      int64_t sum = 0;
      for (size_t i = cut; i < s.size() - cut; i++) sum += s[i];
      int64_t start = sum / int64_t(s.size() - 2 * cut);
      int64_t target = ((t_local + off - start) * int64_t(kRate) / 1000000) * 4;  // byte heard at t_local
      bool fresh = cur < 0 || a.gen != cur_gen;
      int64_t err_frames = fresh ? 0 : (cur - target) / 4;
      uint32_t take = kBlock;
      if (fresh || err_frames > int64_t(kRate) * 30 / 1000 || err_frames < -int64_t(kRate) * 30 / 1000) {
        if (!fresh) {
          jumps++;
          window_bad += 5;  // a forced re-sync is audible: count it like 50 ms of dropout
        }
        cur = target < 0 ? 0 : target;
        cur_gen = a.gen;
      } else {
        double ms = double(err_frames) * 1000.0 / kRate;
        err_sum += ms;
        err_abs_max = std::max(err_abs_max, ms < 0 ? -ms : ms);
        err_n++;
        if (err_frames > 44) take = kBlock - 1, dup++;        // ahead by > 1 ms: repeat a frame
        else if (err_frames < -44) take = kBlock + 1, drop++;  // behind: skip a frame
      }
      if (fresh) primed = false;
      size_t got = gc_->read(a.gen, uint64_t(cur), tmp.data(), take * 4);
      if (got) primed = true;
      if (got < take * 4) {
        memset(tmp.data() + got, 0, take * 4 - got);
        // silence before a stream's first audio arrives (a join, a skip) is expected, not an underrun
        if (primed) {
          under++;
          window_bad++;
        }
      }
      cur += int64_t(take) * 4;
      if (!hw_vol_set || a.volume != hw_vol) {  // the leader's volume: in hardware where the sink can
        hw_vol = a.volume;
        hw_vol_set = true;
        sink_->set_hw_volume(a.volume);
      }
      uint32_t g = sink_->hw_volume_active() ? 0xFFFF : uint32_t((uint64_t(a.volume) * a.volume) >> 16);
      const int16_t* in = (const int16_t*)tmp.data();
      for (uint32_t i = 0; i < kBlock; i++) {
        uint32_t j = take == kBlock ? i : take < kBlock ? std::min(i, take - 1) : (i < kBlock / 2 ? i : i + 1);
        int32_t v = voice_sample(t_local + off + int64_t(i) * 1000000 / kRate);
        for (int c = 0; c < 2; c++) {
          int32_t m = ((int32_t(in[j * 2 + c]) * int32_t(duck)) >> 16) + v;
          m = std::max<int32_t>(-32768, std::min<int32_t>(32767, m));
          out[i * 2 + c] = int16_t((m * int32_t(g)) >> 16);
        }
      }
    }
    sink_->write(out.data(), kBlock);  // blocks: the sink paces us
    if (now_ms() - window_start > 30000) {
      window_start = now_ms();
      window_bad = 0;
    }
    if (window_bad >= 20 && !reported) {
      reported = true;
      SLOGW(TAG, "the link to the leader is unstable (dropouts); telling the leader");
      gc_->control("unstable");
    }
    if (now_ms() - last_log > log_ms) {
      last_log = now_ms();
      int64_t rtt = 0, o = 0;
      gc_->offset_us(o, &rtt);
      SLOGI(TAG, "%s rtt %lld us, sink queue %u ms | err avg %+.2f ms max %.2f ms | +%d/-%d frames, jumps %d, underruns %d",
            !active ? "idle     " : primed ? "following" : "waiting  ", (long long)rtt, unsigned(sink_->latency_frames() * 1000 / kRate),
            err_n ? err_sum / err_n : 0.0, err_abs_max, dup, drop, jumps, under);
      err_sum = err_abs_max = 0;
      err_n = dup = drop = 0;
    }
  }
}

}  // namespace listenica
