#include "event_log.h"
#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <deque>
#include <mutex>
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"

static const char* TAG = "events";
static constexpr size_t kMax = 48;
static constexpr const char* kNs = "soloist";  // the settings namespace (kept from the former project name)
static constexpr const char* kKey = "events";

static std::mutex s_mu;
static std::deque<std::string> s_events;
static bool s_dirty = false;

void event_log_init() {
  nvs_handle_t h;
  if (nvs_open(kNs, NVS_READONLY, &h) != ESP_OK) return;
  size_t n = 0;
  if (nvs_get_blob(h, kKey, nullptr, &n) == ESP_OK && n > 0) {
    std::string blob(n, '\0');
    if (nvs_get_blob(h, kKey, blob.data(), &n) == ESP_OK) {
      std::lock_guard<std::mutex> l(s_mu);
      size_t p = 0;
      while (p < blob.size()) {
        size_t e = blob.find('\n', p);
        if (e == std::string::npos) e = blob.size();
        if (e > p) s_events.emplace_back(blob.substr(p, e - p));
        p = e + 1;
      }
      while (s_events.size() > kMax) s_events.pop_front();
    }
  }
  nvs_close(h);
}

void event_log(const char* fmt, ...) {
  char msg[160];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(msg, sizeof msg, fmt, ap);
  va_end(ap);
  char ts[32];
  time_t now = time(nullptr);
  if (now > 1700000000) {  // SNTP synced: wall clock (UTC)
    struct tm t;
    gmtime_r(&now, &t);
    strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%SZ", &t);
  } else {
    snprintf(ts, sizeof ts, "up %llus", (unsigned long long)(esp_timer_get_time() / 1000000));
  }
  ESP_LOGI(TAG, "%s", msg);
  std::lock_guard<std::mutex> l(s_mu);
  s_events.emplace_back(std::string(ts) + "  " + msg);
  while (s_events.size() > kMax) s_events.pop_front();
  s_dirty = true;
}

void event_log_flush() {
  std::string blob;
  {
    std::lock_guard<std::mutex> l(s_mu);
    if (!s_dirty) return;
    for (auto& e : s_events) blob += e + "\n";
    s_dirty = false;
  }
  nvs_handle_t h;
  if (nvs_open(kNs, NVS_READWRITE, &h) != ESP_OK) return;
  if (nvs_set_blob(h, kKey, blob.data(), blob.size()) != ESP_OK) ESP_LOGW(TAG, "could not store the event log");
  nvs_commit(h);
  nvs_close(h);
}

void event_log_flush_if_idle(bool playing) {
  if (!playing) event_log_flush();
}

std::string event_log_text() {
  std::lock_guard<std::mutex> l(s_mu);
  std::string out;
  for (auto& e : s_events) out += e + "\n";
  return out.empty() ? std::string("(no events)\n") : out;
}
