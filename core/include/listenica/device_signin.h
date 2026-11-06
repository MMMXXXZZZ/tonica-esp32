// Spotify sign-in with a device code (RFC 8628, the keymaster client id): the user opens a URL on
// any phone or computer and confirms a short code; no password ever reaches this device. The
// access token is then exchanged at an access point for the reusable login (username + stored
// credential). Endpoints and scopes: go-librespot session/oauth2.go, tools/oauth_login.py.
#pragma once
#include <functional>
#include <string>
#include "listenica/ap_session.h"

namespace listenica {

struct SigninEvents {
  // a code to show: the user opens `url` and confirms `code` (expires in `expires_s`)
  std::function<void(const std::string& code, const std::string& url, int expires_s)> code;
  // progress: "starting", "waiting", "signing_in", "error" (msg says why; it retries), "done"
  std::function<void(const std::string& state, const std::string& msg)> state;
  std::function<bool()> cancelled;  // checked between steps
};

// Blocks until signed in (true, `out` set) or cancelled (false). Expired or declined codes are
// replaced with fresh ones.
bool device_signin(const SigninEvents& ev, Credentials& out);

}  // namespace listenica
