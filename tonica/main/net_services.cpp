#include "esp_timer.h"
#include <memory>
#include "net_services.h"
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mdns.h"
#include "listenica/version.h"
#include "status_led.h"
#include "event_log.h"

static const char* TAG = "netsvc";

// ------------------------------------------------------------ log ring
static constexpr size_t kLogCap = 48 * 1024;
static char* s_log = nullptr;
static size_t s_log_head = 0, s_log_len = 0;
static std::mutex s_log_mu;
static vprintf_like_t s_orig_vprintf = nullptr;

static int log_hook(const char* fmt, va_list ap) {
  char line[256];
  va_list ap2;
  va_copy(ap2, ap);
  int n = vsnprintf(line, sizeof line, fmt, ap2);
  va_end(ap2);
  if (n > 0 && s_log) {
    size_t len = std::min<size_t>(size_t(n), sizeof line - 1);
    std::lock_guard<std::mutex> l(s_log_mu);
    for (size_t i = 0; i < len; i++) {
      char c = line[i];
      if (c == '\x1b') continue;  // strip colour escape start (rest of "[0;32m" is harmless)
      s_log[(s_log_head + s_log_len) % kLogCap] = c;
      if (s_log_len < kLogCap) s_log_len++;
      else s_log_head = (s_log_head + 1) % kLogCap;
    }
  }
  return s_orig_vprintf ? s_orig_vprintf(fmt, ap) : vprintf(fmt, ap);
}

void log_capture_start() {
  if (s_log) return;
  s_log = (char*)heap_caps_malloc(kLogCap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!s_log) return;
  s_orig_vprintf = esp_log_set_vprintf(log_hook);
}

// ------------------------------------------------------------ http
static std::string s_token, s_dev_id;
static httpd_handle_t s_srv = nullptr;
httpd_handle_t net_services_server() { return s_srv; }
static std::function<std::string()> s_status;

static bool targets_me(httpd_req_t* req) {
  char id[64] = {0};
  if (httpd_req_get_hdr_value_str(req, "X-Device-Id", id, sizeof id) != ESP_OK) return false;
  size_t n = strlen(id);
  return n >= 6 && n <= s_dev_id.size() && s_dev_id.compare(0, n, id) == 0;
}

static bool authorized(httpd_req_t* req) {
  if (s_token.empty()) return false;
  char tok[96] = {0};
  if (httpd_req_get_hdr_value_str(req, "X-Token", tok, sizeof tok) != ESP_OK) return false;
  return s_token == tok;
}

static esp_err_t h_status(httpd_req_t* req) {
  std::string s = s_status ? s_status() : std::string("no status\n");
  httpd_resp_set_type(req, "text/plain");
  return httpd_resp_send(req, s.data(), s.size());
}

static esp_err_t h_log(httpd_req_t* req) {
  httpd_resp_set_type(req, "text/plain; charset=utf-8");
  std::string copy;
  {
    std::lock_guard<std::mutex> l(s_log_mu);
    copy.reserve(s_log_len);
    for (size_t i = 0; i < s_log_len; i++) copy += s_log[(s_log_head + i) % kLogCap];
  }
  return httpd_resp_send(req, copy.data(), copy.size());
}

static esp_err_t h_events(httpd_req_t* req) {
  std::string t = event_log_text();
  httpd_resp_set_type(req, "text/plain; charset=utf-8");
  return httpd_resp_send(req, t.data(), t.size());
}

static void reboot_later(void*) {
  vTaskDelay(pdMS_TO_TICKS(700));
  esp_restart();
}

static esp_err_t h_reboot(httpd_req_t* req) {
  if (!authorized(req)) return httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "bad token");
  httpd_resp_sendstr(req, "rebooting\n");
  xTaskCreate(reboot_later, "reboot", 2048, nullptr, 5, nullptr);
  return ESP_OK;
}

static void (*s_on_ota_start)() = nullptr;
void net_services_on_ota_start(void (*fn)()) { s_on_ota_start = fn; }

static esp_err_t h_ota(httpd_req_t* req) {
  if (!authorized(req)) return httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "bad or missing X-Token");
  if (!targets_me(req)) {
    httpd_resp_set_status(req, "409 Conflict");
    return httpd_resp_sendstr(req, "X-Device-Id does not match this board\n");
  }
  const esp_partition_t* part = esp_ota_get_next_update_partition(nullptr);
  if (!part) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no OTA partition");
  esp_ota_handle_t h = 0;
  size_t img = req->content_len;
  if (img == 0 || img > part->size) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad image size");
  // receive buffer only while an update runs (internal RAM: flash writes read it with the cache off)
  std::unique_ptr<char, void (*)(void*)> mem(static_cast<char*>(heap_caps_malloc(16384, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
                                             heap_caps_free);
  if (!mem) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory for the update");
  char* buf = mem.get();
  const size_t buf_size = 16384;
  if (s_on_ota_start) s_on_ota_start();  // pause playback before the first flash erase
  // known size: the whole range is erased up front in 64 KB blocks (~4x faster than the
  // 4 KB sector-by-sector erases of OTA_WITH_SEQUENTIAL_WRITES); the music is paused anyway
  if (esp_ota_begin(part, img, &h) != ESP_OK)
    return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "ota begin failed");
  ESP_LOGI(TAG, "OTA: receiving %d bytes into %s", req->content_len, part->label);
  status_led_ota(true);
  int remaining = int(img), total = 0;
  uint64_t t0 = esp_timer_get_time();
  while (remaining > 0) {
    int r = httpd_req_recv(req, buf, std::min<int>(remaining, int(buf_size)));
    if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
    if (r <= 0) {
      esp_ota_abort(h);
      status_led_ota(false);
      return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "receive failed");
    }
    if (esp_ota_write(h, buf, size_t(r)) != ESP_OK) {
      esp_ota_abort(h);
      status_led_ota(false);
      return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "flash write failed");
    }
    remaining -= r;
    total += r;
  }
  if (esp_ota_end(h) != ESP_OK) {
    status_led_ota(false);
    return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "image invalid");
  }
  if (esp_ota_set_boot_partition(part) != ESP_OK) {
    status_led_ota(false);
    return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "set boot failed");
  }
  // success: keep breathing until the reboot
  ESP_LOGI(TAG, "OTA: %d bytes written to %s in %.1f s; rebooting", total, part->label,
           double(esp_timer_get_time() - t0) / 1e6);
  char msg[96];
  snprintf(msg, sizeof msg, "OK %d bytes -> %s; rebooting\n", total, part->label);
  httpd_resp_sendstr(req, msg);
  xTaskCreate(reboot_later, "reboot", 2048, nullptr, 5, nullptr);
  return ESP_OK;
}

void net_services_start(const std::string& hostname, const std::string& ota_token, const std::string& device_id,
                        const std::string& connect_name, std::function<std::string()> status_fn) {
  s_token = ota_token;
  s_dev_id = device_id;
  s_status = std::move(status_fn);
  if (mdns_init() == ESP_OK) {
    mdns_hostname_set(hostname.c_str());
    mdns_instance_name_set(hostname.c_str());
    mdns_service_add(nullptr, "_http", "_tcp", 80, nullptr, 0);
    // discovery for tools/ota_push.py --list: id, Connect name, firmware version
    // One family service for every Listenica device; "role" says which kind (tonica = single speaker)
    mdns_txt_item_t txt[] = {{"id", s_dev_id.c_str()}, {"name", connect_name.c_str()}, {"ver", LISTENICA_BUILD_VERSION},
                             {"role", "tonica"}};
    mdns_service_add(nullptr, "_listenica", "_tcp", 80, txt, 4);
    ESP_LOGI(TAG, "mDNS: %s.local", hostname.c_str());
  }
  httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
  cfg.stack_size = 8192;
  cfg.task_priority = 3;  // below the download tasks (5): /status, /log and OTA wait while a track loads
  cfg.core_id = 0;  // keep core 1 free for the audio path
  cfg.max_uri_handlers = 24;  // + Wi-Fi setup page and captive-portal checks while the hotspot runs (20 in use)
  cfg.recv_wait_timeout = 10;
  httpd_handle_t srv = nullptr;
  if (httpd_start(&srv, &cfg) != ESP_OK) {
    ESP_LOGE(TAG, "http server failed to start");
    return;
  }
  httpd_uri_t u[] = {
      {.uri = "/status", .method = HTTP_GET, .handler = h_status, .user_ctx = nullptr},
      {.uri = "/log", .method = HTTP_GET, .handler = h_log, .user_ctx = nullptr},
      {.uri = "/events", .method = HTTP_GET, .handler = h_events, .user_ctx = nullptr},
      {.uri = "/ota", .method = HTTP_POST, .handler = h_ota, .user_ctx = nullptr},
      {.uri = "/reboot", .method = HTTP_POST, .handler = h_reboot, .user_ctx = nullptr},
  };
  for (auto& x : u) httpd_register_uri_handler(srv, &x);
  s_srv = srv;
  ESP_LOGI(TAG, "http on :80 (OTA %s)", s_token.empty() ? "disabled: no token" : "enabled");
}

// An unverified OTA image that hangs (e.g. never gets Wi-Fi) would never
// trigger rollback, because rollback needs a reset. Force one after 3 minutes.
static volatile bool s_marked = false;
static void ota_guard(void*) {
  vTaskDelay(pdMS_TO_TICKS(180000));
  esp_ota_img_states_t st;
  if (!s_marked && esp_ota_get_state_partition(esp_ota_get_running_partition(), &st) == ESP_OK &&
      st == ESP_OTA_IMG_PENDING_VERIFY) {
    ESP_LOGE(TAG, "new image not verified within 3 min: rebooting so the bootloader rolls back");
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_restart();
  }
  vTaskDelete(nullptr);
}

void ota_guard_start() { xTaskCreate(ota_guard, "ota_guard", 2560, nullptr, 3, nullptr); }

void ota_mark_valid() {
  s_marked = true;
  esp_ota_img_states_t st;
  const esp_partition_t* run = esp_ota_get_running_partition();
  if (esp_ota_get_state_partition(run, &st) == ESP_OK && st == ESP_OTA_IMG_PENDING_VERIFY) {
    esp_ota_mark_app_valid_cancel_rollback();
    ESP_LOGI(TAG, "OTA image %s marked valid", run->label);
  }
}
