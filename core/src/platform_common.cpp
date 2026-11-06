#include "listenica/platform.h"
#include <cstdio>
#include <cstring>

namespace listenica {

static std::string g_ca_path;
void set_ca_bundle_path(const std::string& p) { g_ca_path = p; }
const std::string& ca_bundle_path() { return g_ca_path; }

std::string hex(const uint8_t* d, size_t n) {
  static const char* k = "0123456789abcdef";
  std::string s(n * 2, '0');
  for (size_t i = 0; i < n; i++) {
    s[2 * i] = k[d[i] >> 4];
    s[2 * i + 1] = k[d[i] & 15];
  }
  return s;
}

Bytes unhex(const std::string& s) {
  auto v = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  Bytes out;
  for (size_t i = 0; i + 1 < s.size(); i += 2) out.push_back(uint8_t(v(s[i]) << 4 | v(s[i + 1])));
  return out;
}

static const char* B64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string base64_encode(const uint8_t* d, size_t n) {
  std::string out;
  out.reserve((n + 2) / 3 * 4);
  for (size_t i = 0; i < n; i += 3) {
    uint32_t v = d[i] << 16 | (i + 1 < n ? d[i + 1] << 8 : 0) | (i + 2 < n ? d[i + 2] : 0);
    out += B64[v >> 18 & 63];
    out += B64[v >> 12 & 63];
    out += i + 1 < n ? B64[v >> 6 & 63] : '=';
    out += i + 2 < n ? B64[v & 63] : '=';
  }
  return out;
}

Bytes base64_decode(const std::string& s) {
  int T[256];
  for (int& t : T) t = -1;
  for (int i = 0; i < 64; i++) T[(uint8_t)B64[i]] = i;
  T[(uint8_t)'-'] = 62;  // url-safe variants
  T[(uint8_t)'_'] = 63;
  Bytes out;
  uint32_t acc = 0;
  int bits = 0;
  for (char c : s) {
    int v = T[(uint8_t)c];
    if (v < 0) continue;
    acc = acc << 6 | v;
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out.push_back(uint8_t(acc >> bits));
    }
  }
  return out;
}

std::string strfmt(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  char buf[256];
  int n = vsnprintf(buf, sizeof buf, fmt, ap);
  va_end(ap);
  if (n < (int)sizeof buf) return std::string(buf, n < 0 ? 0 : n);
  std::string s(n + 1, '\0');
  va_start(ap, fmt);
  vsnprintf(&s[0], n + 1, fmt, ap);
  va_end(ap);
  s.resize(n);
  return s;
}

}  // namespace listenica
