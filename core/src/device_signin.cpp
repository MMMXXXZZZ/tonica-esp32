#include "listenica/device_signin.h"
#include <cctype>
#include "cJSON.h"
#include "listenica/http.h"
#include "listenica/net.h"
#include "listenica/platform.h"
#include "listenica/spclient.h"
#include "listenica/version.h"

static const char* TAG = "signin";

namespace listenica {

static const char* kDeviceAuthUrl = "https://accounts.spotify.com/oauth2/device/authorize";
static const char* kTokenUrl = "https://accounts.spotify.com/api/token";
static const char* kScopes =
    "streaming app-remote-control user-read-playback-state user-modify-playback-state "
    "user-read-currently-playing user-read-private user-library-read playlist-read-private "
    "playlist-read-collaborative";

static std::string form_escape(const std::string& s) {
  static const char* hexd = "0123456789ABCDEF";
  std::string o;
  for (unsigned char c : s) {
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') o += char(c);
    else o += '%', o += hexd[c >> 4], o += hexd[c & 15];
  }
  return o;
}

static cJSON* post_form(HttpClient& http, const char* url, const std::string& form, int& status) {
  Headers h = {{"Content-Type", "application/x-www-form-urlencoded"}, {"Accept", "application/json"}};
  HttpResponse r = http.request("POST", url, h, Bytes(form.begin(), form.end()), 15000);
  status = r.status;
  return cJSON_Parse(r.text().c_str());
}

static std::string jstr(cJSON* j, const char* k) {
  cJSON* v = cJSON_GetObjectItem(j, k);
  return cJSON_IsString(v) ? std::string(v->valuestring) : std::string();
}
static int jint(cJSON* j, const char* k, int def) {
  cJSON* v = cJSON_GetObjectItem(j, k);
  return cJSON_IsNumber(v) ? v->valueint : def;
}

// Token -> access-point login -> the reusable login (username + stored credential).
static bool token_to_login(const std::string& token, Credentials& out, std::string& err) {
  ApResolve ar;
  if (!apresolve(ar) || ar.accesspoint.empty()) {
    err = "couldn't reach Spotify";
    return false;
  }
  Credentials in;
  in.auth_type = AUTH_TOKEN;
  in.auth_data.assign(token.begin(), token.end());
  std::string device_id = "signin-" LISTENICA_VERSION;  // the AP session is closed right after
  for (const auto& host : ar.accesspoint) {
    ApSession ap(device_id);
    if (ap.connect_and_login(host, in, out, &err) && !out.username.empty() && !out.auth_data.empty()) return true;
    SLOGW(TAG, "login via %s failed: %s", host.c_str(), err.c_str());
  }
  return false;
}

bool device_signin(const SigninEvents& ev, Credentials& out) {
  auto state = [&](const char* s, const std::string& m = std::string()) {
    if (ev.state) ev.state(s, m);
  };
  auto cancelled = [&] { return ev.cancelled && ev.cancelled(); };
  HttpClient http;
  const std::string client = form_escape(KEYMASTER_CLIENT_ID);
  while (!cancelled()) {
    state("starting");
    int st = 0;
    cJSON* j = post_form(http, kDeviceAuthUrl, "client_id=" + client + "&scope=" + form_escape(kScopes), st);
    std::string device_code = j ? jstr(j, "device_code") : "";
    if (device_code.empty()) {
      SLOGW(TAG, "code request failed (HTTP %d)", st);
      cJSON_Delete(j);
      state("error", "Couldn't get a sign-in code from Spotify. Retrying.");
      sleep_ms(15000);
      continue;
    }
    int interval = jint(j, "interval", 5), expires = jint(j, "expires_in", 600);
    std::string code = jstr(j, "user_code"), url = jstr(j, "verification_uri_complete");
    if (url.empty()) url = jstr(j, "verification_uri");
    cJSON_Delete(j);
    uint64_t expires_at = now_ms() + uint64_t(expires) * 1000;
    SLOGI(TAG, "sign in at %s (code %s)", url.c_str(), code.c_str());
    if (ev.code) ev.code(code, url, expires);
    state("waiting");
    std::string token;
    while (now_ms() < expires_at && !cancelled()) {
      sleep_ms(uint32_t(interval) * 1000);
      cJSON* t = post_form(http, kTokenUrl,
                           "grant_type=urn%3Aietf%3Aparams%3Aoauth%3Agrant-type%3Adevice_code&device_code=" +
                               form_escape(device_code) + "&client_id=" + client,
                           st);
      std::string e = t ? jstr(t, "error") : "transport";
      token = t ? jstr(t, "access_token") : "";
      cJSON_Delete(t);
      if (!token.empty()) break;
      if (e == "authorization_pending") continue;
      if (e == "slow_down") {
        interval += 5;
        continue;
      }
      if (e == "access_denied") {
        state("error", "Sign-in was declined. Use the new code.");
        break;
      }
      if (e == "expired_token") break;
      SLOGW(TAG, "token poll: HTTP %d %s", st, e.c_str());  // transient: keep polling
    }
    if (token.empty()) continue;  // expired, declined or cancelled: a fresh code (or stop)
    state("signing_in");
    std::string err;
    if (!token_to_login(token, out, err)) {
      state("error", "Signed in, but the login failed (" + err + "). Retrying with a new code.");
      sleep_ms(5000);
      continue;
    }
    SLOGI(TAG, "signed in as %s", out.username.c_str());
    state("done");
    return true;
  }
  return false;
}

}  // namespace listenica
