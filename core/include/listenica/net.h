// Blocking TCP and TLS streams on top of mbedTLS net_sockets (works on lwIP
// and on Winsock/BSD sockets alike).
#pragma once
#include <memory>
#include <string>
#include "listenica/platform.h"

namespace listenica {

class Stream {
 public:
  virtual ~Stream() = default;
  // >0 bytes read, 0 = timeout (no data), <0 = closed / error
  virtual int read(uint8_t* buf, size_t n, uint32_t timeout_ms) = 0;
  virtual bool write_all(const uint8_t* buf, size_t n) = 0;
  virtual void close() = 0;
  virtual bool is_open() const = 0;
  // Why the stream stopped being open ("" while open or after a local close).
  virtual std::string close_cause() const { return ""; }

  // helpers
  bool read_exact(uint8_t* buf, size_t n, uint32_t timeout_ms);
  bool write_all(const std::string& s) { return write_all((const uint8_t*)s.data(), s.size()); }
};

std::unique_ptr<Stream> tcp_connect(const std::string& host, uint16_t port, uint32_t timeout_ms = 10000);
std::unique_ptr<Stream> tls_connect(const std::string& host, uint16_t port, uint32_t timeout_ms = 15000);

void net_init();  // once at startup (WSAStartup on Windows, PSA init)

}  // namespace listenica
