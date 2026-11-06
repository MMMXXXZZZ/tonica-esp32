#include "zeroconf_service.h"
#include <memory>
#include "esp_log.h"
#include "mdns.h"
#include "listenica/zeroconf.h"

using namespace listenica;
static const char* TAG = "zeroconf";

struct ZcCtx {
  std::unique_ptr<ZeroconfKeys> keys;
  ZeroconfInfo info;
  ZeroconfHooks hooks;
};
static ZcCtx* s_ctx = nullptr;

static esp_err_t send_json(httpd_req_t* req, const std::string& body) {
  httpd_resp_set_type(req, "application/json");
  return httpd_resp_send(req, body.data(), body.size());
}

static esp_err_t h_zc_get(httpd_req_t* req) {
  char q[128] = {0};
  httpd_req_get_url_query_str(req, q, sizeof q);
  if (form_get(q, "action") != "getInfo")
    return send_json(req, "{\"status\":301,\"spotifyError\":0,\"statusString\":\"ERROR-BAD-REQUEST\"}");
  s_ctx->info.active_user = s_ctx->hooks.active_user ? s_ctx->hooks.active_user() : "";
  return send_json(req, zeroconf_get_info_json(*s_ctx->keys, s_ctx->info));
}

static esp_err_t h_zc_post(httpd_req_t* req) {
  int len = req->content_len;
  if (len <= 0 || len > 8192) return send_json(req, "{\"status\":301,\"spotifyError\":0,\"statusString\":\"ERROR-BAD-REQUEST\"}");
  std::string body(size_t(len), '\0');
  int got = 0;
  while (got < len) {
    int r = httpd_req_recv(req, &body[size_t(got)], size_t(len - got));
    if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
    if (r <= 0) return ESP_FAIL;
    got += r;
  }
  char q[128] = {0};
  httpd_req_get_url_query_str(req, q, sizeof q);
  std::string action = form_get(body, "action");
  if (action.empty()) action = form_get(q, "action");
  if (action != "addUser")
    return send_json(req, "{\"status\":301,\"spotifyError\":0,\"statusString\":\"ERROR-BAD-REQUEST\"}");
  std::string user = form_get(body, "userName");
  // What the phone sent (shapes only, never the values): tokenType decides the blob's contents.
  std::string blob = form_get(body, "blob"), ckey = form_get(body, "clientKey");
  ESP_LOGI(TAG, "addUser: tokenType=%s version=%s blob %u chars, clientKey %u chars, loginId %s",
           form_get(body, "tokenType").c_str(), form_get(body, "version").c_str(), unsigned(blob.size()),
           unsigned(ckey.size()), form_get(body, "loginId").empty() ? "absent" : "present");
  Credentials c;
  std::string err;
  if (!zeroconf_decode_add_user(*s_ctx->keys, user, blob, ckey, s_ctx->info.device_id, c, err)) {
    ESP_LOGW(TAG, "addUser from %s rejected: %s", user.c_str(), err.c_str());
    return send_json(req, std::string("{\"status\":102,\"spotifyError\":1,\"statusString\":\"") + err + "\"}");
  }
  std::string current = s_ctx->hooks.active_user ? s_ctx->hooks.active_user() : "";
  // The hook applies the takeover policy; an accepted switch reboots ~1 s later,
  // after this reply has gone out.
  if (current != c.username && s_ctx->hooks.on_new_user && !s_ctx->hooks.on_new_user(c)) {
    ESP_LOGW(TAG, "addUser refused by the takeover policy");
    // librespot's refusal shape (discovery/src/server.rs): status 102, "Service Unavailable"
    return send_json(req, "{\"status\":102,\"spotifyError\":0,\"statusString\":\"Service Unavailable\"}");
  }
  if (!current.empty() && current == c.username && s_ctx->hooks.on_same_user) {
    // Seen 2026-11-03: a phone retried addUser every ~33 s while Tónica was missing from the
    // account's device list (cause unknown); nudge the device list instead of ignoring it.
    ESP_LOGI(TAG, "addUser for the signed-in account: re-announcing to Spotify");
    s_ctx->hooks.on_same_user();
  }
  ESP_LOGI(TAG, "addUser accepted (auth type %u)", (unsigned)c.auth_type);
  return send_json(req, "{\"status\":101,\"spotifyError\":0,\"statusString\":\"OK\"}");
}

void zeroconf_service_start(httpd_handle_t srv, const std::string& device_id, const std::string& name,
                            ZeroconfHooks hooks) {
  s_ctx = new ZcCtx{std::make_unique<ZeroconfKeys>(), {}, std::move(hooks)};
  s_ctx->info.device_id = device_id;
  s_ctx->info.name = name;
  httpd_uri_t g = {.uri = "/zc", .method = HTTP_GET, .handler = h_zc_get, .user_ctx = nullptr};
  httpd_uri_t p = {.uri = "/zc", .method = HTTP_POST, .handler = h_zc_post, .user_ctx = nullptr};
  httpd_register_uri_handler(srv, &g);
  httpd_register_uri_handler(srv, &p);
  mdns_txt_item_t txt[] = {{"VERSION", "1.0"}, {"CPath", "/zc"}, {"Stack", "SP"}};
  mdns_service_add(name.c_str(), "_spotify-connect", "_tcp", 80, txt, 3);
  ESP_LOGI(TAG, "ZeroConf ready: _spotify-connect._tcp -> :80/zc");
}
