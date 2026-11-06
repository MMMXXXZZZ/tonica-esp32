#pragma once
// Owner sign-in with a code (OAuth 2.0 device authorization grant, RFC 8628), as go-librespot's
// device_auth does: the board asks Spotify for a short code, the owner approves it at
// spotify.com/pair on any phone or computer, the board polls for the token and logs in to the
// access point with it (AUTHENTICATION_SPOTIFY_TOKEN), which yields the reusable login it keeps.
//
// Replaces owner pairing through the phone (zeroconf): a login handed over that way played but
// Spotify never made the board the active device, and it dropped out of the account's device
// list after a restart (2026-11-03). The OAuth-derived login works.
#include <functional>
#include <string>
#include "listenica/ap_session.h"

// Starts the sign-in task (no-op if it is already running). on_signed_in runs on that task
// (internal-RAM stack: it may write flash) with the reusable login, once.
void device_login_start(std::function<void(const listenica::Credentials&)> on_signed_in);

// {"state":"starting|waiting|signing_in|done|error","code":"ABC123","url":"https://spotify.com/pair?code=ABC123",
//  "expires_in":3540,"message":"..."}; "" when sign-in was never started.
std::string device_login_json();
