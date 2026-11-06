#include "listenica/zeroconf.h"
#include <cstring>
#include "cJSON.h"
extern "C" {
#include "tinyaes/aes.h"  // software AES-192: the ESP32-S3 AES engine has no 192-bit keys
}
#include "mbedtls/md.h"
#include "mbedtls/pkcs5.h"
#include "listenica/crypto.h"
#include "listenica/version.h"

namespace listenica {

// RFC 2409 group 1 (768-bit), as used by the AP handshake and librespot's DhLocalKeys
static const uint8_t kPrime[96] = {
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xc9, 0x0f, 0xda, 0xa2, 0x21, 0x68, 0xc2, 0x34,
    0xc4, 0xc6, 0x62, 0x8b, 0x80, 0xdc, 0x1c, 0xd1, 0x29, 0x02, 0x4e, 0x08, 0x8a, 0x67, 0xcc, 0x74,
    0x02, 0x0b, 0xbe, 0xa6, 0x3b, 0x13, 0x9b, 0x22, 0x51, 0x4a, 0x08, 0x79, 0x8e, 0x34, 0x04, 0xdd,
    0xef, 0x95, 0x19, 0xb3, 0xcd, 0x3a, 0x43, 0x1b, 0x30, 0x2b, 0x0a, 0x6d, 0xf2, 0x5f, 0x14, 0x37,
    0x4f, 0xe1, 0x35, 0x6d, 0x6d, 0x51, 0xc2, 0x45, 0xe4, 0x85, 0xb5, 0x76, 0x62, 0x5e, 0x7e, 0xc6,
    0xf4, 0x4c, 0x42, 0xe9, 0xa6, 0x3a, 0x36, 0x20, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
};

ZeroconfKeys::ZeroconfKeys() {
  priv_.resize(95);
  random_bytes(priv_.data(), priv_.size());
  pub_ = modexp(Bytes{2}, priv_, Bytes(kPrime, kPrime + 96));
}

ZeroconfKeys::ZeroconfKeys(const Bytes& priv) : priv_(priv) {
  pub_ = modexp(Bytes{2}, priv_, Bytes(kPrime, kPrime + 96));
}

std::string ZeroconfKeys::public_key_b64() const { return base64_encode(pub_); }

Bytes ZeroconfKeys::shared_secret(const Bytes& remote) const {
  return modexp(remote, priv_, Bytes(kPrime, kPrime + 96));
}

std::string zeroconf_get_info_json(const ZeroconfKeys& keys, const ZeroconfInfo& info) {
  cJSON* j = cJSON_CreateObject();
  cJSON_AddNumberToObject(j, "status", 101);
  cJSON_AddStringToObject(j, "statusString", "OK");
  cJSON_AddNumberToObject(j, "spotifyError", 0);
  cJSON_AddStringToObject(j, "version", "2.9.0");
  cJSON_AddStringToObject(j, "deviceID", info.device_id.c_str());
  cJSON_AddStringToObject(j, "deviceType", info.device_type.c_str());
  cJSON_AddStringToObject(j, "remoteName", info.name.c_str());
  cJSON_AddStringToObject(j, "publicKey", keys.public_key_b64().c_str());
  cJSON_AddStringToObject(j, "brandDisplayName", info.brand.c_str());
  cJSON_AddStringToObject(j, "modelDisplayName", info.model.c_str());
  cJSON_AddStringToObject(j, "libraryVersion", LISTENICA_VERSION);
  cJSON_AddStringToObject(j, "resolverVersion", "1");
  cJSON_AddStringToObject(j, "groupStatus", "NONE");
  // Experiment 2026-11-03: Spotify's own ZeroConf docs (commercial hardware) show "accesstoken";
  // librespot and go-librespot send "default" (a stored-credentials blob), and a login handed
  // over that way never became a working Connect device. addUser logs what the phone sends.
  cJSON_AddStringToObject(j, "tokenType", "accesstoken");
  cJSON_AddStringToObject(j, "availability", "");  // required by Spotify's getInfo table
  cJSON_AddStringToObject(j, "clientID", KEYMASTER_CLIENT_ID);
  cJSON_AddNumberToObject(j, "productID", 0);
  cJSON_AddStringToObject(j, "scope", "streaming");
  cJSON_AddStringToObject(j, "accountReq", "PREMIUM");
  cJSON_AddStringToObject(j, "activeUser", info.active_user.c_str());
  cJSON_AddItemToObject(j, "aliases", cJSON_CreateArray());
  char* s = cJSON_PrintUnformatted(j);
  std::string out(s);
  cJSON_free(s);
  cJSON_Delete(j);
  return out;
}

static std::array<uint8_t, 20> hmac(const uint8_t* k, size_t kl, const void* d, size_t n) {
  return hmac_sha1(k, kl, (const uint8_t*)d, n);
}

bool zeroconf_decode_inner_blob(const std::string& username, const std::string& inner_b64,
                                const std::string& device_id, Credentials& out) {
  // key = sha1(pbkdf2_hmac_sha1(sha1(device_id), username, 256, 20)) || be32(20)
  auto secret = sha1((const uint8_t*)device_id.data(), device_id.size());
  uint8_t key[24];
  if (mbedtls_pkcs5_pbkdf2_hmac_ext(MBEDTLS_MD_SHA1, secret.data(), secret.size(), (const uint8_t*)username.data(),
                                    username.size(), 0x100, 20, key) != 0)
    return false;
  auto h = sha1(key, 20);
  memcpy(key, h.data(), 20);
  key[20] = 0, key[21] = 0, key[22] = 0, key[23] = 20;

  Bytes data = base64_decode(inner_b64);
  if (data.size() < 16 || data.size() % 16) return false;
  struct AES_ctx aes;
  AES_init_ctx(&aes, key);
  for (size_t i = 0; i < data.size(); i += 16) AES_ECB_decrypt(&aes, &data[i]);
  size_t l = data.size();
  for (size_t i = 0; i < l - 0x10; i++) data[l - i - 1] ^= data[l - i - 0x11];

  // u8, bytes(username), u8, int(auth_type), u8, bytes(auth_data)
  size_t p = 0;
  auto u8 = [&](uint8_t& v) {
    if (p >= l) return false;
    v = data[p++];
    return true;
  };
  auto rint = [&](uint32_t& v) {
    uint8_t lo, hi;
    if (!u8(lo)) return false;
    if (!(lo & 0x80)) {
      v = lo;
      return true;
    }
    if (!u8(hi)) return false;
    v = (lo & 0x7f) | (uint32_t(hi) << 7);
    return true;
  };
  auto rbytes = [&](Bytes& b) {
    uint32_t n;
    if (!rint(n) || p + n > l) return false;
    b.assign(data.begin() + p, data.begin() + p + n);
    p += n;
    return true;
  };
  uint8_t tmp;
  Bytes inner_user;
  uint32_t auth_type;
  if (!u8(tmp) || !rbytes(inner_user) || !u8(tmp) || !rint(auth_type) || !u8(tmp) || !rbytes(out.auth_data)) return false;
  // integrity: a wrong key yields random bytes; the embedded username must match
  if (std::string(inner_user.begin(), inner_user.end()) != username) return false;
  if (auth_type > 64) return false;
  out.username = username;
  out.auth_type = auth_type;
  return !out.auth_data.empty();
}

bool zeroconf_decode_add_user(const ZeroconfKeys& keys, const std::string& username, const std::string& blob_b64,
                              const std::string& client_key_b64, const std::string& device_id, Credentials& out,
                              std::string& err) {
  Bytes blob = base64_decode(blob_b64);
  Bytes client_key = base64_decode(client_key_b64);
  if (blob.size() < 16 + 20 || client_key.empty()) {
    err = "bad request";
    return false;
  }
  Bytes shared = keys.shared_secret(client_key);
  auto base = sha1(shared.data(), shared.size());
  auto checksum_key = hmac(base.data(), 16, "checksum", 8);
  auto encryption_key = hmac(base.data(), 16, "encryption", 10);
  const uint8_t* iv = blob.data();
  const uint8_t* enc = blob.data() + 16;
  size_t enc_len = blob.size() - 16 - 20;
  auto mac = hmac(checksum_key.data(), checksum_key.size(), enc, enc_len);
  if (memcmp(mac.data(), blob.data() + blob.size() - 20, 20) != 0) {
    err = "ERROR-MAC";
    return false;
  }
  Bytes inner(enc, enc + enc_len);
  Aes128Ctr ctr(encryption_key.data(), iv);
  ctr.apply(inner.data(), inner.size());
  if (!zeroconf_decode_inner_blob(username, std::string(inner.begin(), inner.end()), device_id, out)) {
    err = "bad blob";
    return false;
  }
  return true;
}

std::string url_decode(const std::string& s) {
  std::string o;
  for (size_t i = 0; i < s.size(); i++) {
    if (s[i] == '+') {
      o += ' ';
    } else if (s[i] == '%' && i + 2 < s.size()) {
      o += char(strtol(s.substr(i + 1, 2).c_str(), nullptr, 16));
      i += 2;
    } else {
      o += s[i];
    }
  }
  return o;
}

std::string form_get(const std::string& body, const std::string& key) {
  size_t p = 0;
  while (p <= body.size()) {
    size_t amp = body.find('&', p);
    std::string kv = body.substr(p, amp == std::string::npos ? std::string::npos : amp - p);
    size_t eq = kv.find('=');
    if (eq != std::string::npos && url_decode(kv.substr(0, eq)) == key) return url_decode(kv.substr(eq + 1));
    if (amp == std::string::npos) break;
    p = amp + 1;
  }
  return "";
}

}  // namespace listenica
