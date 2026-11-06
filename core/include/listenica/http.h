// Small HTTP/1.1 client (HTTPS only, keep-alive, chunked + content-length)
// and a WebSocket client, both over listenica::Stream.
#pragma once
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include "listenica/net.h"

namespace listenica {

struct Url {
  std::string scheme, host, path;  // path includes query
  uint16_t port = 443;
  static bool parse(const std::string& s, Url& out);
};

using Headers = std::map<std::string, std::string>;  // response keys lower-cased

struct HttpResponse {
  int status = 0;  // 0 = transport error
  Headers headers;
  Bytes body;
  std::string text() const { return std::string(body.begin(), body.end()); }
  std::string header(const std::string& k) const {
    auto it = headers.find(k);
    return it == headers.end() ? std::string() : it->second;
  }
};

// One persistent connection per client instance; requests are serialized.
class HttpClient {
 public:
  HttpResponse request(const std::string& method, const std::string& url, const Headers& hdrs = {},
                       const Bytes& body = {}, uint32_t timeout_ms = 15000);

  // Streaming: body chunks are delivered to on_data; return false to abort
  // (the connection is then dropped). Returns status/headers, body empty.
  HttpResponse stream(const std::string& method, const std::string& url, const Headers& hdrs,
                      const std::function<bool(const uint8_t*, size_t)>& on_data, uint32_t timeout_ms = 15000,
                      const std::function<void(const HttpResponse&)>& on_headers = nullptr);
  void close();

 private:
  std::mutex mu_;
  std::unique_ptr<Stream> conn_;
  std::string conn_host_;
  uint16_t conn_port_ = 0;
  Bytes rbuf_;  // bytes read past the current position
  Bytes scratch_;
  bool ensure(const Url& u, uint32_t timeout_ms);
  bool read_line(std::string& line, uint32_t timeout_ms);
  bool read_n(size_t n, const std::function<bool(const uint8_t*, size_t)>& sink, uint32_t timeout_ms);
  HttpResponse do_request(const std::string& method, const Url& u, const Headers& hdrs, const Bytes& body,
                          const std::function<bool(const uint8_t*, size_t)>* sink, uint32_t timeout_ms,
                          bool retry, const std::function<void(const HttpResponse&)>* on_headers = nullptr);
};

class WebSocket {
 public:
  bool connect(const std::string& url, uint32_t timeout_ms = 15000);
  bool send_text(const std::string& s);
  bool send_ping();
  // Receive one complete message. Returns: 1 message (text/binary in out),
  // 0 timeout, -1 closed. Pongs set last_pong_ms and are swallowed.
  int recv(std::string& out, uint32_t timeout_ms);
  void close();
  bool is_open() const { return s_ && s_->is_open(); }
  uint64_t last_pong_ms = 0;

 private:
  std::unique_ptr<Stream> s_;
  Bytes rbuf_;
  bool send_frame(uint8_t opcode, const uint8_t* d, size_t n);
  int fill(size_t need, uint32_t timeout_ms);
};

std::string url_encode(const std::string& s);

}  // namespace listenica
