// Spotify Connect ZeroConf pairing (the "select a speaker" flow in the app):
// mDNS `_spotify-connect._tcp` + HTTP `?action=getInfo` / POST `action=addUser`.
// Formats follow librespot discovery/src/server.rs and
// core/src/authentication.rs (Credentials::with_blob).
#pragma once
#include <string>
#include "listenica/ap_session.h"

namespace listenica {

class ZeroconfKeys {
 public:
  ZeroconfKeys();  // random DH key pair (same 768-bit group as the AP handshake)
  explicit ZeroconfKeys(const Bytes& priv);  // fixed key (tests)
  std::string public_key_b64() const;
  Bytes shared_secret(const Bytes& remote_public) const;

 private:
  Bytes priv_, pub_;
};

struct ZeroconfInfo {
  std::string device_id, name, active_user, device_type = "SPEAKER", brand = "listenica", model = "esp32-s3";
};
std::string zeroconf_get_info_json(const ZeroconfKeys& keys, const ZeroconfInfo& info);

// Decrypts an addUser request into AP credentials. Returns false with `err`
// ("ERROR-MAC", "bad blob", ...) on failure.
bool zeroconf_decode_add_user(const ZeroconfKeys& keys, const std::string& username, const std::string& blob_b64,
                              const std::string& client_key_b64, const std::string& device_id, Credentials& out,
                              std::string& err);

// Exposed for tests: the inner (second-stage) blob decoder.
bool zeroconf_decode_inner_blob(const std::string& username, const std::string& inner_b64,
                                const std::string& device_id, Credentials& out);

std::string url_decode(const std::string& s);
std::string form_get(const std::string& body, const std::string& key);  // x-www-form-urlencoded

}  // namespace listenica
