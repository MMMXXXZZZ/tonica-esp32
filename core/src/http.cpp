#include "listenica/http.h"
#include <algorithm>
#include <cstring>
#include "listenica/crypto.h"

static const char* TAG = "http";

namespace listenica {

bool Url::parse(const std::string& s, Url& out) {
  size_t p = s.find("://");
  if (p == std::string::npos) return false;
  out.scheme = s.substr(0, p);
  size_t hs = p + 3;
  size_t ps = s.find('/', hs);
  std::string hostport = s.substr(hs, ps == std::string::npos ? std::string::npos : ps - hs);
  out.path = ps == std::string::npos ? "/" : s.substr(ps);
  size_t c = hostport.rfind(':');
  if (c != std::string::npos) {
    out.host = hostport.substr(0, c);
    out.port = uint16_t(std::stoi(hostport.substr(c + 1)));
  } else {
    out.host = hostport;
    out.port = (out.scheme == "https" || out.scheme == "wss") ? 443 : 80;
  }
  return !out.host.empty();
}

std::string url_encode(const std::string& s) {
  std::string o;
  for (unsigned char c : s) {
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
      o += char(c);
    else
      o += strfmt("%%%02X", c);
  }
  return o;
}

static std::string lower(std::string s) {
  for (auto& c : s) c = char(tolower((unsigned char)c));
  return s;
}

// ------------------------------------------------------------ HttpClient
void HttpClient::close() {
  std::lock_guard<std::mutex> l(mu_);
  conn_.reset();
  rbuf_.clear();
}

bool HttpClient::ensure(const Url& u, uint32_t timeout_ms) {
  if (conn_ && conn_->is_open() && conn_host_ == u.host && conn_port_ == u.port) return true;
  conn_.reset();
  rbuf_.clear();
  conn_ = (u.scheme == "http") ? tcp_connect(u.host, u.port, timeout_ms) : tls_connect(u.host, u.port, timeout_ms);
  conn_host_ = u.host;
  conn_port_ = u.port;
  return conn_ != nullptr;
}

bool HttpClient::read_line(std::string& line, uint32_t timeout_ms) {
  line.clear();
  uint64_t deadline = now_ms() + timeout_ms;
  for (;;) {
    auto it = std::find(rbuf_.begin(), rbuf_.end(), '\n');
    if (it != rbuf_.end()) {
      line.assign(rbuf_.begin(), it);
      rbuf_.erase(rbuf_.begin(), it + 1);
      if (!line.empty() && line.back() == '\r') line.pop_back();
      return true;
    }
    if (rbuf_.size() > 16384) return false;
    uint8_t tmp[1024];
    uint64_t now = now_ms();
    if (now >= deadline) return false;
    int r = conn_->read(tmp, sizeof tmp, uint32_t(deadline - now));
    if (r < 0) return false;
    rbuf_.insert(rbuf_.end(), tmp, tmp + r);
  }
}

bool HttpClient::read_n(size_t n, const std::function<bool(const uint8_t*, size_t)>& sink, uint32_t timeout_ms) {
  size_t take = std::min(n, rbuf_.size());
  if (take) {
    if (!sink(rbuf_.data(), take)) return false;
    rbuf_.erase(rbuf_.begin(), rbuf_.begin() + take);
    n -= take;
  }
  if (scratch_.size() < 4096) scratch_.resize(4096);
  uint8_t* tmp = scratch_.data();
  while (n) {
    // per-read timeout: a stall of timeout_ms fails, slow steady streams don't
    int r = conn_->read(tmp, std::min<size_t>(n, scratch_.size()), timeout_ms);
    if (r <= 0) return false;
    if (!sink(tmp, size_t(r))) return false;
    n -= size_t(r);
  }
  return true;
}

HttpResponse HttpClient::do_request(const std::string& method, const Url& u, const Headers& hdrs, const Bytes& body,
                                    const std::function<bool(const uint8_t*, size_t)>* sink, uint32_t timeout_ms,
                                    bool retry, const std::function<void(const HttpResponse&)>* on_headers) {
  HttpResponse resp;
  bool reused = conn_ && conn_->is_open() && conn_host_ == u.host && conn_port_ == u.port;
  if (!ensure(u, timeout_ms)) return resp;
  std::string req = method + " " + u.path + " HTTP/1.1\r\nHost: " + u.host + "\r\n";
  bool has_ua = false;
  for (auto& kv : hdrs) {
    req += kv.first + ": " + kv.second + "\r\n";
    if (lower(kv.first) == "user-agent") has_ua = true;
  }
  if (!has_ua) req += "User-Agent: Spotify/124200290 Linux/0 (tonica)\r\n";
  if (!body.empty() || method == "POST" || method == "PUT") req += strfmt("Content-Length: %u\r\n", (unsigned)body.size());
  req += "Connection: keep-alive\r\n\r\n";
  bool ok = conn_->write_all(req) && (body.empty() || conn_->write_all(body.data(), body.size()));
  std::string line;
  if (ok) ok = read_line(line, timeout_ms);
  if (!ok) {
    conn_.reset();
    rbuf_.clear();
    if (reused && retry) return do_request(method, u, hdrs, body, sink, timeout_ms, false, on_headers);
    return resp;
  }
  // HTTP/1.1 200 OK
  size_t sp = line.find(' ');
  resp.status = sp == std::string::npos ? 0 : atoi(line.c_str() + sp + 1);
  while (read_line(line, timeout_ms) && !line.empty()) {
    size_t c = line.find(':');
    if (c == std::string::npos) continue;
    std::string k = lower(line.substr(0, c));
    size_t vs = line.find_first_not_of(' ', c + 1);
    resp.headers[k] = vs == std::string::npos ? "" : line.substr(vs);
  }
  if (on_headers && *on_headers) (*on_headers)(resp);
  std::function<bool(const uint8_t*, size_t)> collect = [&](const uint8_t* d, size_t n) {
    resp.body.insert(resp.body.end(), d, d + n);
    return true;
  };
  const auto& out = sink ? *sink : collect;
  bool body_ok = true;
  bool keep = lower(resp.header("connection")) != "close";
  if (method == "HEAD" || resp.status == 204 || resp.status == 304) {
  } else if (lower(resp.header("transfer-encoding")).find("chunked") != std::string::npos) {
    for (;;) {
      if (!read_line(line, timeout_ms)) {
        body_ok = false;
        break;
      }
      size_t n = strtoul(line.c_str(), nullptr, 16);
      if (n == 0) {
        while (read_line(line, timeout_ms) && !line.empty()) {}
        break;
      }
      if (!read_n(n, out, timeout_ms) || !read_line(line, timeout_ms)) {
        body_ok = false;
        break;
      }
    }
  } else if (!resp.header("content-length").empty()) {
    size_t n = strtoull(resp.header("content-length").c_str(), nullptr, 10);
    body_ok = read_n(n, out, timeout_ms);
  } else {
    // read to close
    uint8_t tmp[2048];
    int r;
    while ((r = conn_->read(tmp, sizeof tmp, timeout_ms)) > 0)
      if (!out(tmp, size_t(r))) break;
    keep = false;
  }
  if (!body_ok || !keep) {
    conn_.reset();
    rbuf_.clear();
    if (!body_ok && !sink) resp.status = 0;
  }
  return resp;
}

HttpResponse HttpClient::request(const std::string& method, const std::string& url, const Headers& hdrs,
                                 const Bytes& body, uint32_t timeout_ms) {
  Url u;
  if (!Url::parse(url, u)) return {};
  std::lock_guard<std::mutex> l(mu_);
  auto r = do_request(method, u, hdrs, body, nullptr, timeout_ms, true);
  if (r.status == 0) SLOGW(TAG, "%s %s%s: transport error", method.c_str(), u.host.c_str(), u.path.substr(0, 60).c_str());
  return r;
}

HttpResponse HttpClient::stream(const std::string& method, const std::string& url, const Headers& hdrs,
                                const std::function<bool(const uint8_t*, size_t)>& on_data, uint32_t timeout_ms,
                                const std::function<void(const HttpResponse&)>& on_headers) {
  Url u;
  if (!Url::parse(url, u)) return {};
  std::lock_guard<std::mutex> l(mu_);
  return do_request(method, u, hdrs, {}, &on_data, timeout_ms, true, &on_headers);
}

// ------------------------------------------------------------- WebSocket
bool WebSocket::connect(const std::string& url, uint32_t timeout_ms) {
  Url u;
  if (!Url::parse(url, u)) return false;
  s_ = (u.scheme == "ws") ? tcp_connect(u.host, u.port, timeout_ms) : tls_connect(u.host, u.port, timeout_ms);
  if (!s_) return false;
  uint8_t key[16];
  random_bytes(key, 16);
  std::string req = "GET " + u.path + " HTTP/1.1\r\nHost: " + u.host +
                    "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: " + base64_encode(key, 16) +
                    "\r\nSec-WebSocket-Version: 13\r\nUser-Agent: Spotify/124200290 Linux/0 (tonica)\r\n\r\n";
  if (!s_->write_all(req)) return false;
  // read response headers
  rbuf_.clear();
  uint64_t deadline = now_ms() + timeout_ms;
  for (;;) {
    std::string have(rbuf_.begin(), rbuf_.end());
    size_t e = have.find("\r\n\r\n");
    if (e != std::string::npos) {
      rbuf_.erase(rbuf_.begin(), rbuf_.begin() + e + 4);
      if (have.compare(0, 12, "HTTP/1.1 101") != 0) {
        SLOGW(TAG, "ws upgrade refused: %s", have.substr(0, have.find("\r\n")).c_str());
        s_.reset();
        return false;
      }
      break;
    }
    uint8_t tmp[512];
    uint64_t now = now_ms();
    if (now >= deadline) return false;
    int r = s_->read(tmp, sizeof tmp, uint32_t(deadline - now));
    if (r < 0) return false;
    rbuf_.insert(rbuf_.end(), tmp, tmp + r);
  }
  last_pong_ms = now_ms();
  return true;
}

bool WebSocket::send_frame(uint8_t opcode, const uint8_t* d, size_t n) {
  if (!s_) return false;
  Bytes f;
  f.reserve(n + 14);
  f.push_back(0x80 | opcode);
  if (n < 126) {
    f.push_back(0x80 | uint8_t(n));
  } else if (n < 65536) {
    f.push_back(0x80 | 126);
    f.push_back(uint8_t(n >> 8));
    f.push_back(uint8_t(n));
  } else {
    f.push_back(0x80 | 127);
    for (int i = 7; i >= 0; i--) f.push_back(uint8_t(uint64_t(n) >> (8 * i)));
  }
  uint8_t mask[4];
  random_bytes(mask, 4);
  f.insert(f.end(), mask, mask + 4);
  for (size_t i = 0; i < n; i++) f.push_back(d[i] ^ mask[i & 3]);
  return s_->write_all(f.data(), f.size());
}

bool WebSocket::send_text(const std::string& s) { return send_frame(0x1, (const uint8_t*)s.data(), s.size()); }
bool WebSocket::send_ping() { return send_frame(0x9, nullptr, 0); }

int WebSocket::fill(size_t need, uint32_t timeout_ms) {
  uint64_t deadline = now_ms() + timeout_ms;
  while (rbuf_.size() < need) {
    uint8_t tmp[2048];
    uint64_t now = now_ms();
    if (now >= deadline) return 0;
    int r = s_->read(tmp, sizeof tmp, uint32_t(deadline - now));
    if (r < 0) return -1;
    rbuf_.insert(rbuf_.end(), tmp, tmp + r);
  }
  return 1;
}

int WebSocket::recv(std::string& out, uint32_t timeout_ms) {
  if (!s_) return -1;
  out.clear();
  uint64_t deadline = now_ms() + timeout_ms;
  bool have_partial = false;
  for (;;) {
    uint64_t now = now_ms();
    // Once a frame has started, give it a generous budget to complete.
    uint32_t budget = have_partial ? 15000 : (now >= deadline ? 0 : uint32_t(deadline - now));
    int r = fill(2, budget);
    if (r <= 0) return r;
    have_partial = true;
    uint8_t b0 = rbuf_[0], b1 = rbuf_[1];
    size_t hdr = 2;
    uint64_t len = b1 & 0x7f;
    if (len == 126) {
      if ((r = fill(4, 15000)) <= 0) return r < 0 ? -1 : -1;
      len = uint64_t(rbuf_[2]) << 8 | rbuf_[3];
      hdr = 4;
    } else if (len == 127) {
      if ((r = fill(10, 15000)) <= 0) return -1;
      len = 0;
      for (int i = 0; i < 8; i++) len = len << 8 | rbuf_[2 + i];
      hdr = 10;
    }
    bool masked = b1 & 0x80;
    size_t mhdr = hdr + (masked ? 4 : 0);
    if (len > 8 * 1024 * 1024) return -1;
    if ((r = fill(mhdr + size_t(len), 30000)) <= 0) return -1;
    uint8_t* p = rbuf_.data() + mhdr;
    if (masked)
      for (size_t i = 0; i < len; i++) p[i] ^= rbuf_[hdr + (i & 3)];
    uint8_t op = b0 & 0x0f;
    bool fin = b0 & 0x80;
    std::string payload((const char*)p, size_t(len));
    rbuf_.erase(rbuf_.begin(), rbuf_.begin() + mhdr + size_t(len));
    switch (op) {
      case 0x0:
      case 0x1:
      case 0x2:
        out += payload;
        if (fin) return 1;
        break;
      case 0x8:
        close();
        return -1;
      case 0x9:
        send_frame(0xA, (const uint8_t*)payload.data(), payload.size());
        have_partial = !out.empty();
        break;
      case 0xA:
        last_pong_ms = now_ms();
        have_partial = !out.empty();
        if (!have_partial && now_ms() >= deadline) return 0;
        break;
      default:
        break;
    }
  }
}

void WebSocket::close() {
  if (s_) s_->close();
  s_.reset();
}

}  // namespace listenica
