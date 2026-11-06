#include "listenica/ap_session.h"
#include <cstring>
#include "cJSON.h"
#include "listenica/crypto.h"
#include "listenica/http.h"
#include "listenica/pb.h"
#include "listenica/version.h"

static const char* TAG = "ap";

namespace listenica {

static const uint8_t kServerKey[256] = {
    0xac, 0xe0, 0x46, 0x0b, 0xff, 0xc2, 0x30, 0xaf, 0xf4, 0x6b, 0xfe, 0xc3, 0xbf, 0xbf, 0x86, 0x3d, 0xa1, 0x91, 0xc6,
    0xcc, 0x33, 0x6c, 0x93, 0xa1, 0x4f, 0xb3, 0xb0, 0x16, 0x12, 0xac, 0xac, 0x6a, 0xf1, 0x80, 0xe7, 0xf6, 0x14, 0xd9,
    0x42, 0x9d, 0xbe, 0x2e, 0x34, 0x66, 0x43, 0xe3, 0x62, 0xd2, 0x32, 0x7a, 0x1a, 0x0d, 0x92, 0x3b, 0xae, 0xdd, 0x14,
    0x02, 0xb1, 0x81, 0x55, 0x05, 0x61, 0x04, 0xd5, 0x2c, 0x96, 0xa4, 0x4c, 0x1e, 0xcc, 0x02, 0x4a, 0xd4, 0xb2, 0x0c,
    0x00, 0x1f, 0x17, 0xed, 0xc2, 0x2f, 0xc4, 0x35, 0x21, 0xc8, 0xf0, 0xcb, 0xae, 0xd2, 0xad, 0xd7, 0x2b, 0x0f, 0x9d,
    0xb3, 0xc5, 0x32, 0x1a, 0x2a, 0xfe, 0x59, 0xf3, 0x5a, 0x0d, 0xac, 0x68, 0xf1, 0xfa, 0x62, 0x1e, 0xfb, 0x2c, 0x8d,
    0x0c, 0xb7, 0x39, 0x2d, 0x92, 0x47, 0xe3, 0xd7, 0x35, 0x1a, 0x6d, 0xbd, 0x24, 0xc2, 0xae, 0x25, 0x5b, 0x88, 0xff,
    0xab, 0x73, 0x29, 0x8a, 0x0b, 0xcc, 0xcd, 0x0c, 0x58, 0x67, 0x31, 0x89, 0xe8, 0xbd, 0x34, 0x80, 0x78, 0x4a, 0x5f,
    0xc9, 0x6b, 0x89, 0x9d, 0x95, 0x6b, 0xfc, 0x86, 0xd7, 0x4f, 0x33, 0xa6, 0x78, 0x17, 0x96, 0xc9, 0xc3, 0x2d, 0x0d,
    0x32, 0xa5, 0xab, 0xcd, 0x05, 0x27, 0xe2, 0xf7, 0x10, 0xa3, 0x96, 0x13, 0xc4, 0x2f, 0x99, 0xc0, 0x27, 0xbf, 0xed,
    0x04, 0x9c, 0x3c, 0x27, 0x58, 0x04, 0xb6, 0xb2, 0x19, 0xf9, 0xc1, 0x2f, 0x02, 0xe9, 0x48, 0x63, 0xec, 0xa1, 0xb6,
    0x42, 0xa0, 0x9d, 0x48, 0x25, 0xf8, 0xb3, 0x9d, 0xd0, 0xe8, 0x6a, 0xf9, 0x48, 0x4d, 0xa1, 0xc2, 0xba, 0x86, 0x30,
    0x42, 0xea, 0x9d, 0xb3, 0x08, 0x6c, 0x19, 0x0e, 0x48, 0xb3, 0x9d, 0x66, 0xeb, 0x00, 0x06, 0xa2, 0x5a, 0xee, 0xa1,
    0x1b, 0x13, 0x87, 0x3c, 0xd7, 0x19, 0xe6, 0x55, 0xbd,
};

static const uint8_t kDhPrime[96] = {
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xc9, 0x0f, 0xda, 0xa2, 0x21, 0x68, 0xc2, 0x34,
    0xc4, 0xc6, 0x62, 0x8b, 0x80, 0xdc, 0x1c, 0xd1, 0x29, 0x02, 0x4e, 0x08, 0x8a, 0x67, 0xcc, 0x74,
    0x02, 0x0b, 0xbe, 0xa6, 0x3b, 0x13, 0x9b, 0x22, 0x51, 0x4a, 0x08, 0x79, 0x8e, 0x34, 0x04, 0xdd,
    0xef, 0x95, 0x19, 0xb3, 0xcd, 0x3a, 0x43, 0x1b, 0x30, 0x2b, 0x0a, 0x6d, 0xf2, 0x5f, 0x14, 0x37,
    0x4f, 0xe1, 0x35, 0x6d, 0x6d, 0x51, 0xc2, 0x45, 0xe4, 0x85, 0xb5, 0x76, 0x62, 0x5e, 0x7e, 0xc6,
    0xf4, 0x4c, 0x42, 0xe9, 0xa6, 0x3a, 0x36, 0x20, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
};

enum : uint8_t {
  PKT_PING = 0x04,
  PKT_REQUEST_KEY = 0x0c,
  PKT_AES_KEY = 0x0d,
  PKT_AES_KEY_ERROR = 0x0e,
  PKT_COUNTRY_CODE = 0x1b,
  PKT_PONG = 0x49,
  PKT_PONG_ACK = 0x4a,
  PKT_PRODUCT_INFO = 0x50,
  PKT_LOGIN = 0xab,
  PKT_AP_WELCOME = 0xac,
  PKT_AUTH_FAILURE = 0xad,
};

bool apresolve(ApResolve& out) {
  HttpClient http;
  auto r = http.request("GET", "https://apresolve.spotify.com/?type=accesspoint&type=dealer&type=spclient");
  if (r.status != 200) return false;
  cJSON* j = cJSON_Parse(r.text().c_str());
  if (!j) return false;
  auto list = [&](const char* k, std::vector<std::string>& v) {
    cJSON* a = cJSON_GetObjectItem(j, k);
    cJSON* it;
    cJSON_ArrayForEach(it, a) if (cJSON_IsString(it)) v.push_back(it->valuestring);
  };
  list("accesspoint", out.accesspoint);
  list("dealer", out.dealer);
  list("spclient", out.spclient);
  cJSON_Delete(j);
  return !out.accesspoint.empty();
}

ApSession::~ApSession() { stop(); }

std::string ApSession::country() const {
  std::lock_guard<std::mutex> l(mu_);
  return country_;
}

static void put_be32(Bytes& b, uint32_t v) {
  for (int i = 3; i >= 0; i--) b.push_back(uint8_t(v >> (8 * i)));
}
static uint32_t be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

bool ApSession::handshake(std::string* err) {
  Bytes priv(95);
  random_bytes(priv.data(), priv.size());
  Bytes prime(kDhPrime, kDhPrime + 96);
  Bytes gc = modexp(Bytes{2}, priv, prime);

  uint8_t nonce[16];
  random_bytes(nonce, 16);
  pb::Writer hello;
  hello.msg(10, [](pb::Writer& w) {  // build_info
    w.u64(10, 0);                     // PRODUCT_CLIENT
    w.u64(20, 0);                     // PRODUCT_FLAG_NONE
    w.u64(30, 0x08);                  // PLATFORM_LINUX_X86_64
    w.u64(40, SPOTIFY_VERSION);
  });
  hello.u64(30, 0);  // CRYPTO_SUITE_SHANNON
  hello.msg(50, [&](pb::Writer& w) { w.msg(10, [&](pb::Writer& d) { d.bytes(10, gc).u64(20, 1); }); });
  hello.bytes(60, nonce, 16);
  hello.bytes(70, Bytes{0x1e});

  Bytes pkt{0x00, 0x04};
  put_be32(pkt, uint32_t(2 + 4 + hello.buf.size()));
  pkt.insert(pkt.end(), hello.buf.begin(), hello.buf.end());
  if (!sock_->write_all(pkt.data(), pkt.size())) {
    if (err) *err = "send hello";
    return false;
  }
  Bytes acc = pkt;

  uint8_t hdr[4];
  if (!sock_->read_exact(hdr, 4, 10000)) {
    if (err) *err = "recv hello hdr";
    return false;
  }
  uint32_t size = be32(hdr);
  if (size < 4 || size > 65536) {
    if (err) *err = "bad hello size";
    return false;
  }
  Bytes body(size - 4);
  if (!sock_->read_exact(body.data(), body.size(), 10000)) {
    if (err) *err = "recv hello body";
    return false;
  }
  acc.insert(acc.end(), hdr, hdr + 4);
  acc.insert(acc.end(), body.begin(), body.end());

  pb::Reader resp(body);
  pb::Field f;
  if (resp.find(30, f)) {
    if (err) *err = strfmt("ap login_failed code=%llu", (unsigned long long)pb::Reader(f).varint_of(10));
    return false;
  }
  pb::Field ch, lcc, dh;
  if (!resp.find(10, ch) || !pb::Reader(ch).find(10, lcc) || !pb::Reader(lcc).find(10, dh)) {
    if (err) *err = "no dh challenge";
    return false;
  }
  pb::Reader dhr(dh);
  Bytes gs = dhr.bytes_of(10), sig = dhr.bytes_of(30);
  if (!rsa_verify_pkcs1_sha1(Bytes(kServerKey, kServerKey + 256), sig, gs.data(), gs.size())) {
    if (err) *err = "server signature invalid";
    return false;
  }
  Bytes shared = modexp(gs, priv, prime);

  Bytes data;
  for (uint8_t i = 1; i < 6; i++) {
    Bytes m = acc;
    m.push_back(i);
    auto h = hmac_sha1(shared.data(), shared.size(), m.data(), m.size());
    data.insert(data.end(), h.begin(), h.end());
  }
  auto challenge = hmac_sha1(data.data(), 0x14, acc.data(), acc.size());
  enc_ = std::make_unique<Shannon>(data.data() + 0x14, 32);
  dec_ = std::make_unique<Shannon>(data.data() + 0x34, 32);
  send_nonce_ = recv_nonce_ = 0;

  pb::Writer cr;
  cr.msg(10, [&](pb::Writer& w) {
    w.msg(10, [&](pb::Writer& d) { d.bytes(10, challenge.data(), challenge.size()); });
  });
  cr.msg(20, [](pb::Writer&) {});
  cr.msg(30, [](pb::Writer&) {});
  Bytes out;
  put_be32(out, uint32_t(4 + cr.buf.size()));
  out.insert(out.end(), cr.buf.begin(), cr.buf.end());
  if (!sock_->write_all(out.data(), out.size())) {
    if (err) *err = "send response";
    return false;
  }
  return true;
}

bool ApSession::send_packet(uint8_t cmd, const Bytes& payload) {
  std::lock_guard<std::mutex> l(send_mu_);
  if (!sock_) return false;
  Bytes frame;
  frame.reserve(payload.size() + 7);
  frame.push_back(cmd);
  frame.push_back(uint8_t(payload.size() >> 8));
  frame.push_back(uint8_t(payload.size()));
  frame.insert(frame.end(), payload.begin(), payload.end());
  enc_->nonce_u32(send_nonce_++);
  enc_->encrypt(frame.data(), frame.size());
  uint8_t mac[4];
  enc_->finish(mac, 4);
  frame.insert(frame.end(), mac, mac + 4);
  return sock_->write_all(frame.data(), frame.size());
}

int ApSession::recv_packet(uint8_t& cmd, Bytes& payload, uint32_t timeout_ms, std::string* why) {
  auto broken = [&](const char* what) {
    // A packet cut short leaves the cipher stream out of step: the link can't be used again.
    std::string c = sock_->close_cause();
    if (why) *why = c.empty() ? std::string(what) : c;
    return -1;
  };
  uint8_t hdr[3];
  // wait for the first byte with the caller's timeout, the rest promptly
  int r = sock_->read(hdr, 1, timeout_ms);
  if (r == 0) return 0;
  if (r < 0) return broken("closed");
  if (!sock_->read_exact(hdr + 1, 2, 10000)) return broken("packet header cut short");
  dec_->nonce_u32(recv_nonce_++);
  dec_->decrypt(hdr, 3);
  cmd = hdr[0];
  size_t n = size_t(hdr[1]) << 8 | hdr[2];
  payload.resize(n);
  if (n && !sock_->read_exact(payload.data(), n, 10000)) return broken("packet body cut short");
  dec_->decrypt(payload.data(), n);
  uint8_t mac[4], want[4];
  if (!sock_->read_exact(mac, 4, 10000)) return broken("packet MAC cut short");
  dec_->finish(want, 4);
  if (memcmp(mac, want, 4) != 0) {
    SLOGE(TAG, "MAC mismatch on packet 0x%02x", cmd);
    if (why) *why = strfmt("MAC mismatch on packet 0x%02x", cmd);
    return -1;
  }
  return 1;
}

bool ApSession::connect_and_login(const std::string& ap, const Credentials& in, Credentials& reusable,
                                  std::string* err) {
  size_t c = ap.rfind(':');
  std::string host = ap.substr(0, c);
  uint16_t port = uint16_t(c == std::string::npos ? 4070 : std::stoi(ap.substr(c + 1)));
  SLOGI(TAG, "connecting to %s:%u", host.c_str(), port);
  sock_ = tcp_connect(host, port);
  if (!sock_) {
    if (err) *err = "tcp connect";
    return false;
  }
  if (!handshake(err)) return false;

  pb::Writer login;
  login.msg(10, [&](pb::Writer& w) {
    if (!in.username.empty()) w.str(10, in.username);
    w.u64(20, in.auth_type);
    w.bytes(30, in.auth_data);
  });
  login.msg(50, [&](pb::Writer& w) {
    w.u64(10, 2);  // CPU_X86_64 (matches PLATFORM_LINUX_X86_64 above)
    w.u64(60, 5);  // OS_LINUX
    w.str(90, "tonica-" LISTENICA_VERSION);
    w.str(100, device_id_);
  });
  login.str(70, "librespot 0.8.0");
  if (!send_packet(PKT_LOGIN, login.buf)) {
    if (err) *err = "send login";
    return false;
  }
  uint8_t cmd;
  Bytes data;
  if (recv_packet(cmd, data, 15000) != 1) {
    if (err) *err = "no login reply";
    return false;
  }
  if (cmd == PKT_AUTH_FAILURE) {
    uint64_t code = pb::Reader(data).varint_of(10);
    if (err) *err = strfmt("login failed, error code %llu", (unsigned long long)code);
    return false;
  }
  if (cmd != PKT_AP_WELCOME) {
    if (err) *err = strfmt("unexpected packet 0x%02x", cmd);
    return false;
  }
  pb::Reader w(data);
  username_ = w.str_of(10);
  reusable.username = username_;
  reusable.auth_type = uint32_t(w.varint_of(30));
  reusable.auth_data = w.bytes_of(40);
  SLOGI(TAG, "authenticated (reusable credentials type %u)", (unsigned)reusable.auth_type);
  login_ms_ = now_ms();
  alive_ = true;
  return true;
}

std::string ApSession::end_summary() const {
  uint64_t now = now_ms(), lp = last_ping_ms_;
  std::lock_guard<std::mutex> l(mu_);
  std::string s = (end_cause_.empty() ? std::string("ended") : end_cause_) +
                  strfmt(" after %u s; ", unsigned((now - login_ms_) / 1000));
  s += pings_ ? strfmt("last ping %u s ago", unsigned((now - lp) / 1000)) : std::string("no ping yet");
  s += strfmt(", %u pong%s, %u ack%s", unsigned(pongs_), pongs_ == 1 ? "" : "s", unsigned(acks_), acks_ == 1 ? "" : "s");
  return s;
}

std::string ApSession::keepalive_stats() const {
  return strfmt("%u/%u/%u", unsigned(pings_), unsigned(pongs_), unsigned(acks_));
}

void ApSession::start() {
  stop_ = false;
  reader_done_ = false;
  spawn_thread("ap_reader", 6144, true, 5, &ApSession::reader_entry, this);
}

void ApSession::stop() {
  stop_ = true;
  while (!reader_done_) sleep_ms(20);
  if (sock_) sock_->close();
  alive_ = false;
  std::lock_guard<std::mutex> l(mu_);
  for (auto& kv : key_waits_) kv.second->done = true;
  cv_.notify_all();
}

void ApSession::reader_entry(void* self) { static_cast<ApSession*>(self)->reader_loop(); }

// Keep-alive timing from librespot (core/src/session.rs): Pong 60 s after the Ping, PongAck
// within 20 s, the next Ping within 80 s of the ack. The board used to answer at once; Spotify
// then closed the link at exact 120 s multiples of its age, several times an hour (2026-11-03).
// The first Ping gets 80 s rather than librespot's 20 s: being lenient there costs nothing.
static constexpr uint64_t kPongDelayMs = 60000, kPongAckTimeoutMs = 20000, kPingTimeoutMs = 80000;

void ApSession::reader_loop() {
  last_ping_ms_ = now_ms();
  ka_ = Ka::ExpectingPing;
  ka_deadline_ms_ = now_ms() + kPingTimeoutMs;
  std::string cause;
  while (!stop_) {
    uint8_t cmd;
    Bytes d;
    int r = recv_packet(cmd, d, 500, &cause);
    if (r < 0) break;
    if (r == 1) handle_packet(cmd, d);
    uint64_t now = now_ms();
    if (now >= ka_deadline_ms_) {  // checked every pass, after the packet: a late-but-arrived ack counts
      if (ka_ != Ka::PendingPong) {
        cause = ka_ == Ka::ExpectingPing ? "no ping from the server in 80 s" : "no pong ack from the server in 20 s";
        break;
      }
      send_packet(PKT_PONG, Bytes{0, 0, 0, 0});
      pongs_++;
      ka_ = Ka::ExpectingPongAck;
      ka_deadline_ms_ = now + kPongAckTimeoutMs;
    }
  }
  if (stop_ && cause.empty()) cause = "stopped";
  {
    std::lock_guard<std::mutex> l(mu_);
    end_cause_ = cause;
  }
  SLOGW(TAG, "session ended: %s", end_summary().c_str());
  alive_ = false;
  {
    std::lock_guard<std::mutex> l(mu_);
    for (auto& kv : key_waits_) kv.second->done = true;
    cv_.notify_all();
  }
  reader_done_ = true;
}

void ApSession::handle_packet(uint8_t cmd, const Bytes& d) {
  switch (cmd) {
    case PKT_PING:
      if (ka_ != Ka::ExpectingPing) SLOGW(TAG, "unexpected ping (keep-alive state %d)", int(ka_));
      last_ping_ms_ = now_ms();
      pings_++;
      ka_ = Ka::PendingPong;
      ka_deadline_ms_ = now_ms() + kPongDelayMs;
      break;
    case PKT_PONG_ACK:
      if (ka_ != Ka::ExpectingPongAck) SLOGW(TAG, "unexpected pong ack (keep-alive state %d)", int(ka_));
      acks_++;
      ka_ = Ka::ExpectingPing;
      ka_deadline_ms_ = now_ms() + kPingTimeoutMs;
      break;
    case PKT_COUNTRY_CODE: {
      std::lock_guard<std::mutex> l(mu_);
      country_.assign(d.begin(), d.end());
      SLOGI(TAG, "country %s", country_.c_str());
      break;
    }
    case PKT_AES_KEY:
    case PKT_AES_KEY_ERROR: {
      if (d.size() < 4) break;
      uint32_t seq = be32(d.data());
      std::lock_guard<std::mutex> l(mu_);
      auto it = key_waits_.find(seq);
      if (it != key_waits_.end()) {
        it->second->ok = cmd == PKT_AES_KEY && d.size() >= 20;
        if (it->second->ok) memcpy(it->second->key, d.data() + 4, 16);
        else SLOGW(TAG, "audio key error: %s", hex(d).c_str());
        it->second->done = true;
        cv_.notify_all();
      }
      break;
    }
    case PKT_PRODUCT_INFO: {
      // XML attributes of the account; <autoplay>1</autoplay> is the app's Autoplay switch
      std::string x(d.begin(), d.end());
      size_t a = x.find("<autoplay>");
      if (a != std::string::npos && a + 10 < x.size()) {
        autoplay_ = x[a + 10] == '1' ? 1 : 0;
        SLOGI(TAG, "account autoplay %s", autoplay_ ? "on" : "off");
      }
      break;
    }
    default:
      SLOGV(TAG, "packet 0x%02x (%u bytes)", cmd, (unsigned)d.size());
      break;
  }
}

// ---------------------------------------------------------------- KeyPacer
bool KeyPacer::backing_off() const {
  std::lock_guard<std::mutex> l(mu_);
  return until_ms_ > now_ms();
}

KeyPacer::Result KeyPacer::request(bool optional, const std::function<Result()>& ask,
                                   const std::function<bool()>& alive) {
  const int retries = optional ? 0 : retries_;
  for (int attempt = 0;; attempt++) {
    Result r = ask_once(!optional, ask, alive);
    if (r != Refused || attempt >= retries) return r;
    SLOGI(TAG, "asking again for a refused audio key (%d/%d)", attempt + 1, retries);
  }
}

KeyPacer::Result KeyPacer::ask_once(bool wait, const std::function<Result()>& ask, const std::function<bool()>& alive) {
  std::lock_guard<std::mutex> turn(turn_);
  for (;;) {  // the back-off, in short sleeps so a dead session or shutdown doesn't hang here
    uint64_t until;
    {
      std::lock_guard<std::mutex> l(mu_);
      until = until_ms_;
    }
    if (!alive()) return Failed;
    uint64_t now = now_ms();
    if (until <= now) break;
    if (!wait) return Failed;
    sleep_ms(uint32_t(std::min<uint64_t>(until - now, 50)));
  }
  uint64_t now = now_ms();
  if (last_sent_ms_ && now < last_sent_ms_ + gap_) sleep_ms(uint32_t(last_sent_ms_ + gap_ - now));
  last_sent_ms_ = now_ms();
  Result r = ask();
  std::lock_guard<std::mutex> l(mu_);
  if (r == Key) {
    refusals_ = 0;
    until_ms_ = 0;
  } else if (r == Refused) {  // a timeout or a lost link says nothing about whether Spotify would give it
    refusals_++;
    uint32_t delay = backoff_[std::min(refusals_, 3) - 1];
    until_ms_ = now_ms() + delay;
    SLOGW(TAG, "Spotify refused an audio key; asking for no key for %u ms", unsigned(delay));
  }
  return r;
}

// ---------------------------------------------------------------- audio keys
bool ApSession::request_audio_key(const Bytes& gid, const Bytes& file_id, uint8_t key_out[16], uint32_t timeout_ms,
                                  bool* refused, bool optional) {
  KeyPacer::Result r = key_pacer_.request(
      optional, [&] { return KeyPacer::Result(ask_audio_key(gid, file_id, key_out, timeout_ms)); },
      [this] { return alive_.load(); });
  if (refused) *refused = r == KeyPacer::Refused;
  return r == KeyPacer::Key;
}

// One request on the wire: 1 key, 0 refused (key error), -1 timeout or dead link.
int ApSession::ask_audio_key(const Bytes& gid, const Bytes& file_id, uint8_t key_out[16], uint32_t timeout_ms) {
  if (!alive_) return -1;
  KeyWait w;
  uint32_t seq;
  {
    std::lock_guard<std::mutex> l(mu_);
    seq = key_seq_++;
    key_waits_[seq] = &w;
  }
  Bytes req = file_id;
  req.insert(req.end(), gid.begin(), gid.end());
  put_be32(req, seq);
  req.push_back(0);
  req.push_back(0);
  bool sent = send_packet(PKT_REQUEST_KEY, req);
  std::unique_lock<std::mutex> l(mu_);
  if (sent) cv_.wait_for(l, std::chrono::milliseconds(timeout_ms), [&] { return w.done; });
  key_waits_.erase(seq);
  if (w.ok) {
    memcpy(key_out, w.key, 16);
    return 1;
  }
  return w.done ? 0 : -1;
}

}  // namespace listenica
