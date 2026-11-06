#include "listenica/spclient.h"
#include <cstring>
#include "listenica/crypto.h"
#include "listenica/pb.h"
#include "listenica/version.h"

static const char* TAG = "spclient";

namespace listenica {

static const char* B62 = "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";

std::string gid_to_base62(const Bytes& gid) {
  // 128-bit big number -> 22 base62 digits
  uint8_t n[16] = {0};
  memcpy(n + (16 - std::min<size_t>(16, gid.size())), gid.data(), std::min<size_t>(16, gid.size()));
  std::string out(22, '0');
  for (int i = 21; i >= 0; i--) {
    uint32_t rem = 0;
    for (int b = 0; b < 16; b++) {
      uint32_t cur = rem << 8 | n[b];
      n[b] = uint8_t(cur / 62);
      rem = cur % 62;
    }
    out[i] = B62[rem];
  }
  return out;
}

Bytes base62_to_gid(const std::string& s) {
  uint8_t n[16] = {0};
  for (char c : s) {
    const char* p = strchr(B62, c);
    if (!p) return {};
    uint32_t carry = uint32_t(p - B62);
    for (int b = 15; b >= 0; b--) {
      uint32_t cur = uint32_t(n[b]) * 62 + carry;
      n[b] = uint8_t(cur);
      carry = cur >> 8;
    }
  }
  return Bytes(n, n + 16);
}

std::string uri_kind(const std::string& uri) {
  size_t a = uri.find(':');
  size_t b = uri.find(':', a + 1);
  if (a == std::string::npos || b == std::string::npos) return "";
  return uri.substr(a + 1, b - a - 1);
}

std::string uri_id(const std::string& uri) {
  size_t p = uri.rfind(':');
  return p == std::string::npos ? uri : uri.substr(p + 1);
}

SpClient::SpClient(std::string device_id, std::string hostport) : device_id_(std::move(device_id)) {
  size_t c = hostport.rfind(':');
  std::string host = c == std::string::npos ? hostport : hostport.substr(0, c);
  base_url_ = "https://" + host;
}

void SpClient::set_credentials(const Credentials& c) {
  std::lock_guard<std::mutex> l(mu_);
  creds_ = c;
  token_.clear();
  token_exp_ = 0;
}

Credentials SpClient::credentials() {
  std::lock_guard<std::mutex> l(mu_);
  return creds_;
}

// util::solve_hash_cash from librespot
static bool hashcash(const Bytes& ctx, const Bytes& prefix, int length, uint8_t suffix[16], uint64_t& elapsed_ms) {
  auto md = sha1(ctx.data(), ctx.size());
  uint64_t target = 0;
  for (int i = 12; i < 20; i++) target = target << 8 | md[i];
  uint64_t start = now_ms();
  Bytes buf(prefix);
  buf.resize(prefix.size() + 16);
  for (uint64_t counter = 0;; counter++) {
    uint64_t a = target + counter;
    for (int i = 0; i < 8; i++) {
      buf[prefix.size() + i] = uint8_t(a >> (56 - 8 * i));
      buf[prefix.size() + 8 + i] = uint8_t(counter >> (56 - 8 * i));
    }
    auto h = sha1(buf.data(), buf.size());
    uint64_t tail = 0;
    for (int i = 12; i < 20; i++) tail = tail << 8 | h[i];
    if (length >= 64 ? tail == 0 : (tail & ((1ULL << length) - 1)) == 0) {
      memcpy(suffix, &buf[prefix.size()], 16);
      elapsed_ms = now_ms() - start;
      return true;
    }
    if ((counter & 0xfff) == 0 && now_ms() - start > 5000) return false;
  }
}

bool SpClient::client_token(std::string& out) {
  std::lock_guard<std::mutex> l(mu_);
  if (!ctoken_.empty() && now_ms() < ctoken_exp_) {
    out = ctoken_;
    return true;
  }
  pb::Writer req;
  req.u64(1, 1);  // REQUEST_CLIENT_DATA_REQUEST
  req.msg(2, [&](pb::Writer& cd) {
    cd.str(1, SPOTIFY_SEMANTIC_VERSION).str(2, KEYMASTER_CLIENT_ID);
    cd.msg(3, [&](pb::Writer& sdk) {
      sdk.msg(1, [](pb::Writer& p) {
        p.msg(5, [](pb::Writer& lx) { lx.str(1, "Linux").str(2, "6.1.0").str(3, "#1 SMP").str(4, "x86_64"); });
      });
      sdk.str(2, device_id_);
    });
  });
  Bytes body = req.buf;
  Headers h{{"Accept", "application/x-protobuf"}, {"Content-Type", "application/x-protobuf"}};
  for (int attempt = 0; attempt < 3; attempt++) {
    auto r = auth_.request("POST", clienttoken_url_, h, body);
    if (r.status != 200) {
      SLOGW(TAG, "clienttoken HTTP %d", r.status);
      return false;
    }
    pb::Reader m(r.body);
    uint64_t type = m.varint_of(1);
    if (type == 1) {
      pb::Field g;
      m.find(2, g);
      pb::Reader gr(g);
      ctoken_ = gr.str_of(1);
      uint64_t refresh = gr.varint_of(3, gr.varint_of(2, 3600));
      ctoken_exp_ = now_ms() + (refresh > 60 ? refresh - 30 : refresh) * 1000;
      out = ctoken_;
      SLOGI(TAG, "client token ok");
      return true;
    }
    if (type == 2) {
      pb::Field chs, c0, hp;
      m.find(3, chs);
      pb::Reader cr(chs);
      std::string state = cr.str_of(1);
      if (!cr.find(2, c0) || !pb::Reader(c0).find(4, hp)) return false;
      pb::Reader hpr(hp);
      int length = int(hpr.varint_of(1));
      Bytes prefix = unhex(hpr.str_of(2));
      uint8_t suffix[16];
      uint64_t el;
      if (!hashcash({}, prefix, length, suffix, el)) return false;
      std::string sfx = hex(suffix, 16);
      for (auto& ch : sfx) ch = char(toupper((unsigned char)ch));
      pb::Writer ans;
      ans.u64(1, 2);
      ans.msg(3, [&](pb::Writer& a) {
        a.str(1, state);
        a.msg(2, [&](pb::Writer& x) { x.u64(1, 3).msg(4, [&](pb::Writer& hc) { hc.str(1, sfx); }); });
      });
      body = ans.buf;
      continue;
    }
    SLOGW(TAG, "clienttoken: unexpected response type %llu", (unsigned long long)type);
    return false;
  }
  return false;
}

bool SpClient::access_token(std::string& out) {
  {
    std::lock_guard<std::mutex> l(mu_);
    if (!token_.empty() && now_ms() < token_exp_) {
      out = token_;
      return true;
    }
  }
  std::string ct;
  if (!client_token(ct)) return false;
  Credentials c = credentials();
  pb::Writer base;
  base.msg(1, [&](pb::Writer& ci) { ci.str(1, KEYMASTER_CLIENT_ID).str(2, device_id_); });
  pb::Writer method;
  method.msg(100, [&](pb::Writer& sc) { sc.str(1, c.username).bytes(2, c.auth_data); });
  Bytes ctx;
  pb::Writer solutions;
  bool have_solutions = false;
  Headers h{{"Accept", "application/x-protobuf"}, {"Content-Type", "application/x-protobuf"}, {"client-token", ct}};
  for (int attempt = 0; attempt < 3; attempt++) {
    pb::Writer req;
    req.buf = base.buf;
    if (!ctx.empty()) req.bytes(2, ctx);
    if (have_solutions) req.msg(3, solutions);
    req.buf.insert(req.buf.end(), method.buf.begin(), method.buf.end());
    auto r = auth_.request("POST", login5_url_, h, req.buf);
    if (r.status != 200) {
      SLOGW(TAG, "login5 HTTP %d", r.status);
      return false;
    }
    pb::Reader m(r.body);
    pb::Field f;
    if (m.find(1, f)) {
      pb::Reader ok(f);
      std::lock_guard<std::mutex> l(mu_);
      token_ = ok.str_of(2);
      uint64_t exp = ok.varint_of(4, 3600);
      token_exp_ = now_ms() + (exp > 120 ? exp - 60 : exp) * 1000;
      Bytes sc = ok.bytes_of(3);
      if (!sc.empty()) creds_.auth_data = sc;
      out = token_;
      SLOGI(TAG, "login5 token ok (expires in %llus)", (unsigned long long)exp);
      return true;
    }
    if (m.find(2, f)) {
      SLOGW(TAG, "login5 error %llu", (unsigned long long)f.v);
      if (f.v == 4 || f.v == 6) {  // TIMEOUT / TOO_MANY_ATTEMPTS
        sleep_ms(3000);
        continue;
      }
      return false;
    }
    if (m.find(3, f)) {
      ctx = m.bytes_of(5);
      solutions = pb::Writer();
      have_solutions = true;
      pb::Reader(f).each(1, [&](const pb::Field& chf) {
        pb::Field hcf;
        if (!pb::Reader(chf).find(1, hcf)) return;
        pb::Reader hc(hcf);
        uint8_t suffix[16];
        uint64_t el = 0;
        if (!hashcash(ctx, hc.bytes_of(1), int(hc.varint_of(2)), suffix, el)) return;
        solutions.msg(1, [&](pb::Writer& s) {
          s.msg(1, [&](pb::Writer& hs) {
            hs.bytes(1, suffix, 16);
            hs.msg(2, [&](pb::Writer& d) { d.i64(1, int64_t(el / 1000)).i64(2, int64_t((el % 1000) * 1000000)); });
          });
        });
      });
      continue;
    }
  }
  return false;
}

HttpResponse SpClient::request(const std::string& method, const std::string& path, const Headers& extra,
                               const Bytes& body, bool metrics) {
  return request_on(api_, method, path, extra, body, metrics);
}

HttpResponse SpClient::request_on(HttpClient& client, const std::string& method, const std::string& path,
                                  const Headers& extra, const Bytes& body, bool metrics) {
  std::string tok, ct;
  if (!access_token(tok)) return {};
  client_token(ct);
  std::string url = base_url_ + path;
  if (metrics) url += std::string(path.find('?') == std::string::npos ? "?" : "&") + "product=0&country=" + country_;
  Headers h = extra;
  h["Authorization"] = "Bearer " + tok;
  if (!ct.empty()) h["client-token"] = ct;
  auto r = client.request(method, url, h, body);
  if (r.status == 401) {  // token revoked early: refresh once
    {
      std::lock_guard<std::mutex> l(mu_);
      token_.clear();
    }
    if (!access_token(tok)) return r;
    h["Authorization"] = "Bearer " + tok;
    r = client.request(method, url, h, body);
  }
  return r;
}

bool SpClient::track_metadata(const std::string& uri, TrackMeta& out) {
  out = TrackMeta();
  out.uri = uri;
  out.is_episode = uri_kind(uri) == "episode";
  int kind = out.is_episode ? 12 : 10;  // EPISODE_V4 / TRACK_V4
  pb::Writer req;
  req.msg(2, [&](pb::Writer& er) {
    er.str(1, uri);
    er.msg(2, [&](pb::Writer& q) { q.u64(1, uint64_t(kind)); });
  });
  auto r = request("POST", "/extended-metadata/v0/extended-metadata",
                   {{"Content-Type", "application/x-protobuf"}, {"Accept", "application/x-protobuf"}}, req.buf, true);
  if (r.status != 200) {
    SLOGW(TAG, "extended-metadata HTTP %d", r.status);
    return false;
  }
  pb::Field arr, data, any, val;
  if (!pb::Reader(r.body).find(2, arr) || !pb::Reader(arr).find(3, data)) return false;
  pb::Reader dr(data);
  if (!dr.find(3, any) || !pb::Reader(any).find(2, val)) {
    SLOGW(TAG, "metadata for %s: empty (status %llu)", uri.c_str(), (unsigned long long)pb::Reader(data).varint_of(1));
    return false;
  }
  pb::Reader t(val);
  out.gid = t.bytes_of(1);
  out.name = t.str_of(2);
  if (!out.is_episode) {
    { uint64_t z = t.varint_of(7); out.duration_ms = uint32_t((z >> 1) ^ (~(z & 1) + 1)); }  // sint32 zigzag
    pb::Field al;
    if (t.find(3, al)) {  // album
      pb::Reader a(al);
      out.album = a.str_of(2);
      out.album_uri = "spotify:album:" + gid_to_base62(a.bytes_of(1));
      pb::Field cg, img;
      if (a.find(17, cg) && pb::Reader(cg).find(1, img)) out.image_id = hex(pb::Reader(img).bytes_of(1));
    }
    t.each(4, [&](const pb::Field& ar) {  // Track.artist (repeated)
      pb::Reader a(ar);
      if (out.artist.empty()) {
        out.artist = a.str_of(2);
        out.artist_uri = "spotify:artist:" + gid_to_base62(a.bytes_of(1));
      }
      std::string n = a.str_of(2);
      if (!n.empty()) out.artists.push_back(n);
    });
    auto add_files = [&](pb::Reader& tr) {
      tr.each(12, [&](const pb::Field& ff) {
        pb::Reader fr(ff);
        out.files.push_back({fr.bytes_of(1), int(fr.varint_of(2, 0))});
      });
    };
    add_files(t);
    if (out.files.empty()) {
      // region-restricted original: try alternatives
      t.each(13, [&](const pb::Field& alt) {
        if (!out.files.empty()) return;
        pb::Reader ar2(alt);
        add_files(ar2);
        if (!out.files.empty()) out.gid = ar2.bytes_of(1);
      });
    }
  } else {
    { uint64_t z = t.varint_of(7); out.duration_ms = uint32_t((z >> 1) ^ (~(z & 1) + 1)); }  // sint32 zigzag
    // Episode.audio = 12 (AudioFile)
    t.each(12, [&](const pb::Field& ff) {
      pb::Reader fr(ff);
      out.files.push_back({fr.bytes_of(1), int(fr.varint_of(2, 0))});
    });
  }
  out.ok = !out.files.empty();
  return true;
}

bool SpClient::storage_resolve(const Bytes& file_id, std::vector<std::string>& urls) {
  auto r = request("GET", "/storage-resolve/files/audio/interactive/" + hex(file_id),
                   {{"Accept", "application/x-protobuf"}});
  if (r.status != 200) {
    SLOGW(TAG, "storage-resolve HTTP %d", r.status);
    return false;
  }
  pb::Reader m(r.body);
  uint64_t result = m.varint_of(1);
  m.each(2, [&](const pb::Field& f) { urls.push_back(f.str()); });
  if (result != 0) SLOGW(TAG, "storage-resolve result %llu", (unsigned long long)result);
  return result == 0 && !urls.empty();
}

// ------------------------------------------------------------ ContextScanner
void ContextScanner::begin_string() {
  esc_ = false;
  buf_.clear();
  str_is_key_ = st_ == Key;
  if (str_is_key_) {
    capture_ = true;
  } else {
    bool in_track = !stack_.empty() && stack_.back().role == TrackObject;
    capture_ = (in_track && (key_ == "uri" || key_ == "uid")) || (!in_track && key_ == "next_page_url") ||
               (stack_.size() == 1 && stack_.back().object && key_ == "url");
  }
  st_ = InString;
}

void ContextScanner::end_string() {
  if (str_is_key_) {
    key_.swap(buf_);
    st_ = Colon;
    return;
  }
  if (capture_) {
    if (!stack_.empty() && stack_.back().role == TrackObject) {
      (key_ == "uri" ? cur_.uri : cur_.uid) = buf_;
    } else if (key_ == "next_page_url") {
      next_page_url = buf_;
    } else {
      url = buf_;
    }
  }
  end_value();
}

void ContextScanner::end_value() {
  key_.clear();
  st_ = stack_.empty() ? Value : AfterValue;
  if (stack_.empty()) done_ = true;  // the top-level value is complete
}

// Structural characters outside strings. Returns false on malformed input.
bool ContextScanner::structural(uint8_t ch) {
  switch (ch) {
    case '{':
    case '[': {
      if (st_ != Value || stack_.size() >= 64) return false;
      Role role = Other;
      if (ch == '{' && !stack_.empty() && stack_.back().role == TracksArray) {
        role = TrackObject;
        cur_ = Track();
      } else if (ch == '[' && !stack_.empty() && stack_.back().object && key_ == "tracks") {
        // only a context's own track list: never one nested inside a track (its metadata)
        bool in_track = false;
        for (auto& f : stack_) in_track = in_track || f.role == TrackObject;
        if (!in_track) role = TracksArray;
      }
      stack_.push_back({ch == '{', role});
      key_.clear();
      st_ = ch == '{' ? Key : Value;
      return true;
    }
    case '}':
    case ']': {
      bool obj = ch == '}';
      if (stack_.empty() || stack_.back().object != obj) return false;
      // '}' after a key or ':' and ']' right after ',' are malformed; empty containers are fine
      if (obj ? (st_ != AfterValue && st_ != Key) : (st_ != AfterValue && st_ != Value)) return false;
      if (stack_.back().role == TrackObject && !cur_.uri.empty()) {
        if (tracks.size() < max_) tracks.push_back(std::move(cur_));
        else truncated = true;
      }
      stack_.pop_back();
      end_value();
      return true;
    }
    case ',':
      if (st_ != AfterValue) return false;
      st_ = stack_.back().object ? Key : Value;
      return true;
    case ':':
      if (st_ != Colon) return false;
      st_ = Value;
      return true;
    default:
      return false;
  }
}

void ContextScanner::feed(const uint8_t* d, size_t n) {
  for (size_t i = 0; i < n && !error && !done_; i++) {
    uint8_t ch = d[i];
    if (st_ == InString) {
      if (esc_) {
        esc_ = false;
        if (capture_ && buf_.size() < 1024) buf_ += char(ch == 'n' ? '\n' : ch == 't' ? '\t' : ch);
      } else if (ch == '\\') {
        esc_ = true;  // \uXXXX keeps its hex digits as text: uris and uids never contain escapes
      } else if (ch == '"') {
        end_string();
      } else if (ch < 0x20) {
        error = true;  // raw control character inside a string
      } else if (capture_ && buf_.size() < 1024) {
        buf_ += char(ch);  // bytes >= 0x80 (UTF-8) pass through untouched
      }
      continue;
    }
    if (st_ == InScalar) {
      if (ch == ',' || ch == '}' || ch == ']' || ch == ' ' || ch == '\n' || ch == '\r' || ch == '\t') {
        end_value();
      } else {
        continue;
      }
    }
    if (ch == ' ' || ch == '\n' || ch == '\r' || ch == '\t') continue;
    if (ch == '"') {
      if (st_ != Value && st_ != Key) {
        error = true;
        continue;
      }
      begin_string();
      continue;
    }
    if (st_ == Value && ch != '{' && ch != '[' && ch != ']') {
      if (ch == '-' || (ch >= '0' && ch <= '9') || ch == 't' || ch == 'f' || ch == 'n') {
        st_ = InScalar;  // number / true / false / null: skipped
        continue;
      }
      error = true;
      continue;
    }
    if (!structural(ch)) error = true;
  }
}

// ------------------------------------------------------------ context-resolve
bool SpClient::context_resolve(const std::string& uri_or_path, ContextScanner& out,
                               const std::function<bool(const ContextScanner&)>& enough) {
  std::string path;
  if (uri_or_path.compare(0, 5, "hm://") == 0)
    path = uri_or_path.substr(4);  // "hm://foo/bar" -> "/foo/bar"
  else if (!uri_or_path.empty() && uri_or_path[0] == '/')
    path = uri_or_path;
  else
    path = "/context-resolve/v1/" + uri_or_path;
  for (int attempt = 0; attempt < 2; attempt++) {
    std::string tok, ct;
    if (!access_token(tok)) return false;
    client_token(ct);
    Headers h{{"Accept", "application/json"}, {"Authorization", "Bearer " + tok}};
    if (!ct.empty()) h["client-token"] = ct;
    out.reset();
    bool stopped = false;
    auto r = ctx_.stream("GET", base_url_ + path, h,
                         [&](const uint8_t* d, size_t n) {
                           out.feed(d, n);
                           if (enough && enough(out)) stopped = true;
                           return !stopped;  // false drops the connection (closed after a fetch anyway)
                         },
                         15000);
    if (stopped) return true;
    if (r.status == 401 && attempt == 0) {  // token revoked early: refresh once (as request() does)
      std::lock_guard<std::mutex> l(mu_);
      token_.clear();
      continue;
    }
    if (r.status != 200) {
      SLOGW(TAG, "context-resolve %s HTTP %d", path.c_str(), r.status);
      return false;
    }
    if (out.error) SLOGW(TAG, "context-resolve %s: malformed JSON, kept %u tracks", path.c_str(), unsigned(out.tracks.size()));
    if (out.truncated) SLOGW(TAG, "context-resolve %s: kept the first %u tracks", path.c_str(), unsigned(out.tracks.size()));
    return true;
  }
  return false;
}

bool SpClient::collection_add(const std::string& track_uri) {
  std::string user;
  {
    std::lock_guard<std::mutex> l(mu_);
    user = creds_.username;
  }
  uint8_t rnd[8];
  random_bytes(rnd, sizeof rnd);
  // WriteRequest { username=1, set=2, items=3 { uri=1, added_at=2, is_removed=3 }, client_update_id=4 }
  pb::Writer w;
  w.str(1, user);
  w.str(2, "collection");
  w.msg(3, [&](pb::Writer& it) {
    it.str(1, track_uri);
    it.u64(2, uint64_t(unix_ms() / 1000));
    it.b(3, false);  // add only: an already-liked track stays liked
  });
  w.str(4, hex(rnd, sizeof rnd));
  auto r = request("POST", "/collection/v2/write", {{"Content-Type", "application/vnd.collection-v2.spotify.proto"}}, w.buf);
  if (r.status != 200) SLOGW(TAG, "like %s: HTTP %d", track_uri.c_str(), r.status);
  else SLOGI(TAG, "liked %s", track_uri.c_str());
  return r.status == 200;
}

bool SpClient::autoplay_context(const std::string& context_uri, const std::vector<std::string>& recent,
                                std::string& json_out) {
  pb::Writer w;
  w.str(1, context_uri);
  for (auto& u : recent) w.str(2, u);
  auto r = request("POST", "/context-resolve/v1/autoplay",
                   {{"Content-Type", "application/x-protobuf"}, {"Accept", "application/json"}}, w.buf);
  if (r.status != 200 || r.body.empty()) {
    SLOGW(TAG, "autoplay for %s: HTTP %d", context_uri.c_str(), r.status);
    return false;
  }
  json_out = r.text();
  return true;
}

int SpClient::put_connect_state(const std::string& connection_id, const Bytes& body, Bytes* reply_out) {
  auto r = request_on(state_, "PUT", "/connect-state/v1/devices/" + device_id_,
                      {{"X-Spotify-Connection-Id", connection_id},
                       {"Content-Type", "application/x-protobuf"},
                       {"Accept", "application/x-protobuf"}},
                      body, false);
  if (reply_out) reply_out->swap(r.body);
  return r.status;
}

std::string SpClient::username() {
  std::lock_guard<std::mutex> l(mu_);
  return creds_.username;
}

int SpClient::transfer_to_self() {
  // librespot core/src/spclient.rs: same id for from/to transfers from the active device
  auto r = request("POST", "/connect-state/v1/connect/transfer/from/" + device_id_ + "/to/" + device_id_,
                   {{"Content-Type", "application/json"}},
                   Bytes{'{', '"', 't', 'r', 'a', 'n', 's', 'f', 'e', 'r', '_', 'o', 'p', 't', 'i', 'o', 'n', 's', '"', ':',
                         '{', '"', 'r', 'e', 's', 't', 'o', 'r', 'e', '_', 'p', 'a', 'u', 's', 'e', 'd', '"', ':', '"', 'r',
                         'e', 's', 't', 'o', 'r', 'e', '"', '}', '}'});
  return r.status;
}

int SpClient::put_inactive(const std::string& connection_id, bool notify) {
  // librespot core/src/spclient.rs put_connect_state_inactive: same header as the state PUT
  auto r = request_on(state_, "PUT",
                      "/connect-state/v1/devices/" + device_id_ + "/inactive?notify=" + (notify ? "true" : "false"),
                      {{"X-Spotify-Connection-Id", connection_id}}, {}, false);
  return r.status;
}

}  // namespace listenica
