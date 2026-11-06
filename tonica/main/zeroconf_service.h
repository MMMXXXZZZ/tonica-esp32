#pragma once
#include <functional>
#include <string>
#include "esp_http_server.h"
#include "listenica/ap_session.h"

// Spotify Connect ZeroConf on the shared HTTP server: GET/POST /zc
// (getInfo / addUser) plus the `_spotify-connect._tcp` mDNS service. Lets the
// Spotify app hand its login to the speaker ("pick the speaker in the app"),
// so no PC-side OAuth/provisioning is needed.
struct ZeroconfHooks {
  std::function<std::string()> active_user;                    // "" if none
  // Applies the takeover policy: returns false to refuse; on accept it switches
  // session (owner: persisted; guest: RAM only) by rebooting shortly after.
  std::function<bool(const listenica::Credentials&)> on_new_user;
  // addUser for the account already signed in: nothing to switch, but the phone only does this
  // when it can't find the speaker among the account's devices. Re-announce it to Spotify.
  std::function<void()> on_same_user;
};
void zeroconf_service_start(httpd_handle_t srv, const std::string& device_id, const std::string& name,
                            ZeroconfHooks hooks);
