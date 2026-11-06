#include "listenica/net.h"
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <mutex>
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"
#include "mbedtls/error.h"
#include "mbedtls/net_sockets.h"
#include "mbedtls/ssl.h"
#include "mbedtls/x509_crt.h"
#if defined(MBEDTLS_PSA_CRYPTO_C)
#include "psa/crypto.h"
#endif
#ifdef ESP_PLATFORM
#include "esp_crt_bundle.h"
#endif
#ifdef _WIN32
#include <windows.h>
#include <wincrypt.h>  // the system root store (CertOpenSystemStoreA)
#endif

static const char* TAG = "net";

namespace listenica {

bool Stream::read_exact(uint8_t* buf, size_t n, uint32_t timeout_ms) {
  size_t got = 0;
  uint64_t deadline = now_ms() + timeout_ms;
  while (got < n) {
    uint64_t now = now_ms();
    if (now >= deadline) return false;
    int r = read(buf + got, n - got, uint32_t(deadline - now));
    if (r < 0) return false;
    got += size_t(r);
  }
  return true;
}

void net_init() {
  static std::once_flag once;
  std::call_once(once, [] {
#if defined(MBEDTLS_PSA_CRYPTO_C)
    psa_crypto_init();
#endif
  });
}

// ---------------------------------------------------------------- TCP
class TcpStream : public Stream {
 public:
  mbedtls_net_context net;
  bool open = false;
  std::string cause;
  TcpStream() { mbedtls_net_init(&net); }
  ~TcpStream() override { close(); }
  int read(uint8_t* buf, size_t n, uint32_t timeout_ms) override {
    if (!open) return -1;
    int r = mbedtls_net_recv_timeout(&net, buf, n, timeout_ms ? timeout_ms : 1);
    if (r == MBEDTLS_ERR_SSL_TIMEOUT || r == MBEDTLS_ERR_SSL_WANT_READ) return 0;
    if (r <= 0) {
      // mbedtls_net_recv: 0 = orderly close (FIN), CONN_RESET = ECONNRESET/EPIPE, else errno
      cause = r == 0 ? "closed by the server (FIN)"
              : r == MBEDTLS_ERR_NET_CONN_RESET ? "reset (RST)"
                                                : "receive error -0x" + hex4(-r) + " errno " + std::to_string(errno);
      open = false;
      return -1;
    }
    return r;
  }
  std::string close_cause() const override { return cause; }
  static std::string hex4(int v) {
    char b[8];
    snprintf(b, sizeof b, "%04x", unsigned(v) & 0xffff);
    return b;
  }
  bool write_all(const uint8_t* buf, size_t n) override {
    int transient = 0;
    while (n) {
      int r = mbedtls_net_send(&net, buf, n);
      if (r == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
      if (r == MBEDTLS_ERR_NET_SEND_FAILED && transient < 10) {
        // e.g. ENOMEM from lwIP under memory pressure: back off, don't drop the link
        SLOGW(TAG, "tcp send failed (errno %d), retrying", errno);
        transient++;
        sleep_ms(20 * transient);
        continue;
      }
      if (r <= 0) {
        SLOGW(TAG, "tcp send error -0x%04x (errno %d): closing", -r, errno);
        cause = "send error -0x" + hex4(-r) + " errno " + std::to_string(errno);
        open = false;
        return false;
      }
      buf += r;
      n -= size_t(r);
    }
    return true;
  }
  void close() override {
    // Release the socket whenever one exists: the read/write error paths clear
    // `open` first, and gating on it leaked one lwIP socket per dropped AP link
    // (sockets ran out after a night: web server and dealer died, ping still worked).
    if (net.fd >= 0) mbedtls_net_free(&net);  // sets fd = -1, so this is idempotent
    open = false;
  }
  bool is_open() const override { return open; }
};

std::unique_ptr<Stream> tcp_connect(const std::string& host, uint16_t port, uint32_t) {
  net_init();
  auto s = std::make_unique<TcpStream>();
  int r = mbedtls_net_connect(&s->net, host.c_str(), std::to_string(port).c_str(), MBEDTLS_NET_PROTO_TCP);
  if (r != 0) {
    SLOGW(TAG, "tcp connect %s:%u failed (-0x%04x)", host.c_str(), port, -r);
    return nullptr;
  }
  s->open = true;
  return s;
}

// ---------------------------------------------------------------- TLS
#ifndef ESP_PLATFORM
// The trusted roots: --ca <bundle.pem> when given, else (Windows) the system's root store. Without
// either the host would connect unverified; it refuses instead (the host is a user-facing tool).
static mbedtls_x509_crt* host_ca_chain() {
  static mbedtls_x509_crt* chain = nullptr;
  static std::once_flag once;
  std::call_once(once, [] {
    auto* c = new mbedtls_x509_crt;
    mbedtls_x509_crt_init(c);
    int loaded = 0;
    if (!ca_bundle_path().empty()) {
      int r = mbedtls_x509_crt_parse_file(c, ca_bundle_path().c_str());
      if (r < 0) SLOGW(TAG, "CA bundle parse failed (-0x%04x)", -r);
      else loaded = 1;
    }
#ifdef _WIN32
    if (!loaded) {
      HCERTSTORE store = CertOpenSystemStoreA(0, "ROOT");
      for (PCCERT_CONTEXT cc = nullptr; store && (cc = CertEnumCertificatesInStore(store, cc)) != nullptr;)
        if (mbedtls_x509_crt_parse_der(c, cc->pbCertEncoded, cc->cbCertEncoded) == 0) loaded++;
      if (store) CertCloseStore(store, 0);
      SLOGD(TAG, "%d root certificates from the Windows store", loaded);
    }
#else
    // macOS keeps a PEM bundle of the system roots at /etc/ssl/cert.pem; Linux distributions at one of these
    for (const char* p : {"/etc/ssl/cert.pem", "/etc/ssl/certs/ca-certificates.crt", "/etc/pki/tls/certs/ca-bundle.crt",
                          "/etc/ssl/ca-bundle.pem", "/etc/pki/ca-trust/extracted/pem/tls-ca-bundle.pem"}) {
      if (loaded) break;
      if (mbedtls_x509_crt_parse_file(c, p) >= 0 && c->version) {
        loaded = 1;
        SLOGD(TAG, "root certificates from %s", p);
      }
    }
#endif
    if (loaded) chain = c;
    else SLOGE(TAG, "no trusted root certificates (pass --ca <bundle.pem>)");
  });
  return chain;
}
#endif

class TlsStream : public Stream {
 public:
  mbedtls_net_context net;
  mbedtls_ssl_context ssl;
  mbedtls_ssl_config conf;
  mbedtls_entropy_context entropy;
  mbedtls_ctr_drbg_context drbg;
  bool open = false;

  TlsStream() {
    mbedtls_net_init(&net);
    mbedtls_ssl_init(&ssl);
    mbedtls_ssl_config_init(&conf);
    mbedtls_entropy_init(&entropy);
    mbedtls_ctr_drbg_init(&drbg);
  }
  ~TlsStream() override {
    close();
    mbedtls_ssl_free(&ssl);
    mbedtls_ssl_config_free(&conf);
    mbedtls_ctr_drbg_free(&drbg);
    mbedtls_entropy_free(&entropy);
  }

  bool connect(const std::string& host, uint16_t port, uint32_t timeout_ms) {
    int r;
    if ((r = mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &entropy, (const uint8_t*)"soloist", 7)) != 0)
      return fail("drbg", r);
    if ((r = mbedtls_ssl_config_defaults(&conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM,
                                         MBEDTLS_SSL_PRESET_DEFAULT)) != 0)
      return fail("config", r);
    mbedtls_ssl_conf_rng(&conf, mbedtls_ctr_drbg_random, &drbg);
#ifdef ESP_PLATFORM
    esp_crt_bundle_attach(&conf);
    mbedtls_ssl_conf_authmode(&conf, MBEDTLS_SSL_VERIFY_REQUIRED);
#else
    auto* ca = host_ca_chain();
    if (!ca) return fail("no trusted roots", -1);
    mbedtls_ssl_conf_ca_chain(&conf, ca, nullptr);
    mbedtls_ssl_conf_authmode(&conf, MBEDTLS_SSL_VERIFY_REQUIRED);
#endif
    mbedtls_ssl_conf_read_timeout(&conf, timeout_ms);
    if ((r = mbedtls_ssl_setup(&ssl, &conf)) != 0) return fail("setup", r);
    if ((r = mbedtls_ssl_set_hostname(&ssl, host.c_str())) != 0) return fail("hostname", r);
    if ((r = mbedtls_net_connect(&net, host.c_str(), std::to_string(port).c_str(), MBEDTLS_NET_PROTO_TCP)) != 0)
      return fail("connect", r);
    mbedtls_ssl_set_bio(&ssl, &net, mbedtls_net_send, nullptr, mbedtls_net_recv_timeout);
    uint64_t deadline = now_ms() + timeout_ms;
    while ((r = mbedtls_ssl_handshake(&ssl)) != 0) {
      if (r != MBEDTLS_ERR_SSL_WANT_READ && r != MBEDTLS_ERR_SSL_WANT_WRITE) {
        mbedtls_net_free(&net);
        return fail("handshake", r);
      }
      if (now_ms() > deadline) {
        mbedtls_net_free(&net);
        return fail("handshake timeout", r);
      }
    }
    open = true;
    host_ = host;
    return true;
  }

  int read(uint8_t* buf, size_t n, uint32_t timeout_ms) override {
    if (!open) return -1;
    mbedtls_ssl_conf_read_timeout(&conf, timeout_ms ? timeout_ms : 1);
    for (;;) {
      int r = mbedtls_ssl_read(&ssl, buf, n);
      if (r > 0) return r;
      if (r == MBEDTLS_ERR_SSL_TIMEOUT) return 0;
      if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
#ifdef MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET
      if (r == MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET) continue;
#endif
      if (r != 0 && r != MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY)
        SLOGD(TAG, "tls read %s: -0x%04x", host_.c_str(), -r);
      close();
      return -1;
    }
  }

  bool write_all(const uint8_t* buf, size_t n) override {
    while (n) {
      int r = mbedtls_ssl_write(&ssl, buf, n);
      if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
      if (r <= 0) {
        SLOGD(TAG, "tls write %s: -0x%04x", host_.c_str(), -r);
        close();
        return false;
      }
      buf += r;
      n -= size_t(r);
    }
    return true;
  }

  void close() override {
    if (open) mbedtls_ssl_close_notify(&ssl);
    if (net.fd >= 0) mbedtls_net_free(&net);  // see TcpStream::close
    open = false;
  }
  bool is_open() const override { return open; }

 private:
  std::string host_;
  bool fail(const char* what, int r) {
    char e[100];
    mbedtls_strerror(r, e, sizeof e);
    SLOGW(TAG, "tls %s failed: -0x%04x %s", what, -r, e);
    return false;
  }
};

std::unique_ptr<Stream> tls_connect(const std::string& host, uint16_t port, uint32_t timeout_ms) {
  net_init();
  auto s = std::make_unique<TlsStream>();
  if (!s->connect(host, port, timeout_ms)) return nullptr;
  return s;
}

}  // namespace listenica
