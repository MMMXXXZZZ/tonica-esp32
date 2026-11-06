#include "wifi_setup.h"
#include <algorithm>
#include <atomic>
#include <cstring>
#include <vector>
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "listenica/zeroconf.h"
#include "status_led.h"

static const char* TAG = "wifi_setup";

namespace {
std::atomic<bool> s_active{false}, s_stop{false}, s_done{false};
std::atomic<int> s_clients{0};
WifiSetupHooks s_hooks;
httpd_handle_t s_srv = nullptr;
std::string s_ap_ssid;
std::vector<std::pair<std::string, int>> s_networks;  // ssid, rssi
esp_netif_t* s_ap_netif = nullptr;

const char* kCaptivePaths[] = {"/", "/generate_204", "/gen_204", "/hotspot-detect.html", "/connecttest.txt",
                               "/ncsi.txt", "/library/test/success.html"};

void stop();

void scan() {
  wifi_scan_config_t sc = {};
  sc.show_hidden = false;
  if (esp_wifi_scan_start(&sc, true) != ESP_OK) return;
  uint16_t n = 24;
  std::vector<wifi_ap_record_t> recs(n);
  if (esp_wifi_scan_get_ap_records(&n, recs.data()) != ESP_OK) return;
  s_networks.clear();
  for (uint16_t i = 0; i < n; i++) {
    std::string ssid((const char*)recs[i].ssid);
    if (ssid.empty() || ssid == s_ap_ssid) continue;
    auto it = std::find_if(s_networks.begin(), s_networks.end(), [&](auto& p) { return p.first == ssid; });
    if (it == s_networks.end()) s_networks.push_back({ssid, recs[i].rssi});
    else it->second = std::max(it->second, int(recs[i].rssi));
  }
  std::sort(s_networks.begin(), s_networks.end(), [](auto& a, auto& b) { return a.second > b.second; });
}

extern const char setup_page_start[] asm("_binary_setup_page_html_start");
extern const char setup_page_end[] asm("_binary_setup_page_html_end");

std::string json_escape(const std::string& s) {
  std::string o;
  for (unsigned char c : s) {
    if (c == '"' || c == '\\') o += '\\', o += char(c);
    else if (c < 0x20) {
      char b[8];
      snprintf(b, sizeof b, "\\u%04x", c);
      o += b;
    } else o += char(c);
  }
  return o;
}

esp_err_t send_json(httpd_req_t* req, const std::string& j, int status = 200) {
  if (status != 200) httpd_resp_set_status(req, status == 503 ? "503 Service Unavailable" : "400 Bad Request");
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  return httpd_resp_send(req, j.data(), j.size());
}

esp_err_t h_page(httpd_req_t* req) {  // EMBED_TXTFILES appends a NUL: not part of the page
  httpd_resp_set_type(req, "text/html; charset=utf-8");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  return httpd_resp_send(req, setup_page_start, setup_page_end - setup_page_start - 1);
}

esp_err_t h_state(httpd_req_t* req) {
  std::string nets = "[";
  for (auto& n : s_networks) {
    if (nets.size() > 1) nets += ",";
    nets += "{\"ssid\":\"" + json_escape(n.first) + "\",\"rssi\":" + std::to_string(n.second) + "}";
  }
  nets += "]";
  std::string st = s_hooks.state_json ? s_hooks.state_json() : "{}";
  return send_json(req, "{\"networks\":" + nets + (st.size() > 2 ? "," + st.substr(1) : "}"));
}

esp_err_t h_playlists(httpd_req_t* req) {
  int status = 503;
  std::string j = s_hooks.playlists_json ? s_hooks.playlists_json(status) : "{\"error\":\"unavailable\"}";
  return send_json(req, j, status);
}

static bool read_body(httpd_req_t* req, std::string& body, int max) {
  int len = req->content_len;
  if (len <= 0 || len > max) return false;
  body.assign(len, '\0');
  int got = 0;
  while (got < len) {
    int r = httpd_req_recv(req, &body[got], len - got);
    if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
    if (r <= 0) return false;
    got += r;
  }
  return true;
}

esp_err_t h_voices(httpd_req_t* req) {
  std::string body;
  if (!read_body(req, body, 1024)) return send_json(req, "{\"ok\":false,\"message\":\"Bad request.\"}", 400);
  return send_json(req, s_hooks.voices_json ? s_hooks.voices_json(body) : "{\"ok\":false}");
}

esp_err_t h_preview(httpd_req_t* req) {
  std::string body;
  if (!read_body(req, body, 2048)) return send_json(req, "{\"ok\":false,\"message\":\"Bad request.\"}", 400);
  return send_json(req, s_hooks.preview_json ? s_hooks.preview_json(body) : "{\"ok\":false}");
}

esp_err_t h_captive(httpd_req_t* req) {  // OS connectivity checks and "/" -> the setup page
  httpd_resp_set_status(req, "302 Found");
  httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/setup");
  return httpd_resp_send(req, nullptr, 0);
}

void close_task(void* arg) {  // after the reply has reached the phone
  bool reboot = arg != nullptr;
  vTaskDelay(pdMS_TO_TICKS(1500));
  stop();
  if (reboot && s_hooks.reboot) s_hooks.reboot();
  vTaskDelete(nullptr);
}

esp_err_t h_save(httpd_req_t* req) {
  int len = req->content_len;
  if (len <= 0 || len > 4096) return send_json(req, "{\"ok\":false,\"message\":\"Bad request.\"}", 400);
  std::string body(len, '\0');
  int got = 0;
  while (got < len) {
    int r = httpd_req_recv(req, &body[got], len - got);
    if (r <= 0) return ESP_FAIL;
    got += r;
  }
  bool close = false, reboot = false;
  status_led_fx(LedFx::PendingWhite);
  std::string reply = s_hooks.save ? s_hooks.save(body, close, reboot) : "{\"ok\":false}";
  esp_err_t e = send_json(req, reply);
  if (close) {
    s_done = true;
    status_led_fx(LedFx::GreenFlash);
    xTaskCreatePinnedToCore(close_task, "setup_close", 3072, reboot ? (void*)1 : nullptr, 3, nullptr, 0);
  } else {
    status_led_fx(s_clients > 0 ? LedFx::CyanBreathe : LedFx::CyanBlink);
  }
  return e;
}

// Captive DNS: answer every A query with 192.168.4.1.
void dns_task(void*) {
  int s = socket(AF_INET, SOCK_DGRAM, 0);
  sockaddr_in a = {};
  a.sin_family = AF_INET;
  a.sin_port = htons(53);
  a.sin_addr.s_addr = htonl(INADDR_ANY);
  if (s < 0 || bind(s, (sockaddr*)&a, sizeof a) != 0) {
    ESP_LOGW(TAG, "DNS: bind failed");
    if (s >= 0) close(s);
    vTaskDelete(nullptr);
  }
  timeval tv = {1, 0};
  setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
  uint8_t buf[512];
  while (!s_stop) {
    sockaddr_in from = {};
    socklen_t fl = sizeof from;
    int n = recvfrom(s, buf, sizeof buf - 16, 0, (sockaddr*)&from, &fl);
    if (n < 12) continue;
    // keep the header + first question, then append one A answer
    size_t q = 12;
    while (q < size_t(n) && buf[q]) q += buf[q] + 1;
    q += 5;  // terminating 0 + QTYPE + QCLASS
    if (q > size_t(n)) continue;
    buf[2] = 0x81;  // response, recursion desired
    buf[3] = 0x80;  // recursion available, no error
    buf[6] = 0, buf[7] = 1;  // ANCOUNT = 1
    buf[8] = buf[9] = buf[10] = buf[11] = 0;
    const uint8_t ans[] = {0xc0, 0x0c, 0, 1, 0, 1, 0, 0, 0, 60, 0, 4, 192, 168, 4, 1};
    memcpy(buf + q, ans, sizeof ans);
    sendto(s, buf, q + sizeof ans, 0, (sockaddr*)&from, fl);
  }
  close(s);
  vTaskDelete(nullptr);
}

void on_ap_event(void*, esp_event_base_t, int32_t id, void*) {
  if (id == WIFI_EVENT_AP_STACONNECTED) s_clients++;
  else if (id == WIFI_EVENT_AP_STADISCONNECTED && s_clients > 0) s_clients--;
  if (!s_done) status_led_fx(s_clients > 0 ? LedFx::CyanBreathe : LedFx::CyanBlink);
}

void stop() {
  for (const char* p : kCaptivePaths) httpd_unregister_uri_handler(s_srv, p, HTTP_GET);
  httpd_unregister_uri_handler(s_srv, "/setup", HTTP_GET);
  httpd_unregister_uri_handler(s_srv, "/setup/state", HTTP_GET);
  httpd_unregister_uri_handler(s_srv, "/setup/playlists", HTTP_GET);
  httpd_unregister_uri_handler(s_srv, "/setup/save", HTTP_POST);
  httpd_unregister_uri_handler(s_srv, "/setup/voices", HTTP_POST);
  httpd_unregister_uri_handler(s_srv, "/setup/preview", HTTP_POST);
  esp_event_handler_unregister(WIFI_EVENT, WIFI_EVENT_AP_STACONNECTED, on_ap_event);
  esp_event_handler_unregister(WIFI_EVENT, WIFI_EVENT_AP_STADISCONNECTED, on_ap_event);
  s_stop = true;
  esp_wifi_set_mode(WIFI_MODE_STA);
  s_active = false;
}

void timeout_task(void* arg) {
  uint32_t timeout_s = uint32_t(uintptr_t(arg));
  uint64_t end = esp_timer_get_time() / 1000 + uint64_t(timeout_s) * 1000;
  while (!s_done && uint64_t(esp_timer_get_time() / 1000) < end) vTaskDelay(pdMS_TO_TICKS(1000));
  if (!s_done) {
    ESP_LOGW(TAG, "no network configured in %u s; hotspot closed", unsigned(timeout_s));
    stop();
    status_led_fx(LedFx::Fail);
  }
  vTaskDelete(nullptr);
}
}  // namespace

bool wifi_setup_active() { return s_active; }

bool wifi_setup_start(httpd_handle_t srv, const std::string& device_id, uint32_t timeout_s, WifiSetupHooks hooks) {
  if (s_active.exchange(true) || !srv) return false;
  s_hooks = std::move(hooks);
  s_srv = srv;
  s_stop = s_done = false;
  s_clients = 0;
  s_ap_ssid = "Tónica-Setup-" + device_id.substr(0, 4);
  if (!s_ap_netif) s_ap_netif = esp_netif_create_default_wifi_ap();

  wifi_config_t ap = {};
  memcpy(ap.ap.ssid, s_ap_ssid.data(), s_ap_ssid.size());
  ap.ap.ssid_len = uint8_t(s_ap_ssid.size());
  ap.ap.authmode = WIFI_AUTH_OPEN;  // only after a 15 s button hold or with no Wi-Fi saved; 5-minute limit
  ap.ap.max_connection = 2;
  wifi_ap_record_t cur = {};
  ap.ap.channel = esp_wifi_sta_get_ap_info(&cur) == ESP_OK ? cur.primary : 1;  // AP must share the STA channel
  esp_wifi_set_mode(WIFI_MODE_APSTA);
  esp_wifi_set_config(WIFI_IF_AP, &ap);
  scan();

  esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_AP_STACONNECTED, on_ap_event, nullptr);
  esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_AP_STADISCONNECTED, on_ap_event, nullptr);
  const httpd_uri_t routes[] = {
      {.uri = "/setup", .method = HTTP_GET, .handler = h_page, .user_ctx = nullptr},
      {.uri = "/setup/state", .method = HTTP_GET, .handler = h_state, .user_ctx = nullptr},
      {.uri = "/setup/playlists", .method = HTTP_GET, .handler = h_playlists, .user_ctx = nullptr},
      {.uri = "/setup/save", .method = HTTP_POST, .handler = h_save, .user_ctx = nullptr},
      {.uri = "/setup/voices", .method = HTTP_POST, .handler = h_voices, .user_ctx = nullptr},
      {.uri = "/setup/preview", .method = HTTP_POST, .handler = h_preview, .user_ctx = nullptr},
  };
  for (auto& r : routes) httpd_register_uri_handler(srv, &r);
  for (const char* path : kCaptivePaths) {
    httpd_uri_t c = {.uri = path, .method = HTTP_GET, .handler = h_captive, .user_ctx = nullptr};
    httpd_register_uri_handler(srv, &c);
  }
  xTaskCreatePinnedToCore(dns_task, "captive_dns", 3072, nullptr, 3, nullptr, 0);
  if (timeout_s) xTaskCreatePinnedToCore(timeout_task, "setup_timeout", 2560, (void*)uintptr_t(timeout_s), 2, nullptr, 0);
  status_led_fx(LedFx::CyanBlink);
  ESP_LOGI(TAG, "hotspot '%s' up (open, channel %d)%s: join it and open http://192.168.4.1/setup", s_ap_ssid.c_str(),
           ap.ap.channel, timeout_s ? "" : ", until a network is configured");
  return true;
}
