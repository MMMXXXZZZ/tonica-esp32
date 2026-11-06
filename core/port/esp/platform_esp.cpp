// ESP-IDF implementation of listenica/platform.h
#include <sys/time.h>
#include <cstdio>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_pthread.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "listenica/crypto.h"
#include "listenica/platform.h"
#include "listenica/ring.h"

namespace listenica {

static Level g_level = Level::Info;
void set_log_level(Level l) { g_level = l; }

uint64_t now_ms() { return uint64_t(esp_timer_get_time() / 1000); }
int64_t now_us() { return esp_timer_get_time(); }
int64_t unix_ms() {
  struct timeval tv;
  gettimeofday(&tv, nullptr);
  return int64_t(tv.tv_sec) * 1000 + tv.tv_usec / 1000;
}
void sleep_ms(uint32_t ms) { vTaskDelay(pdMS_TO_TICKS(ms ? ms : 1)); }

void log_write(Level lvl, const char* tag, const char* fmt, ...) {
  if (lvl > g_level) return;
  static const esp_log_level_t map[] = {ESP_LOG_ERROR, ESP_LOG_WARN, ESP_LOG_INFO, ESP_LOG_DEBUG, ESP_LOG_VERBOSE};
  static const char L[] = "EWIDV";
  char msg[384];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(msg, sizeof msg, fmt, ap);
  va_end(ap);
  esp_log_write(map[int(lvl)], tag, "%c (%lu) %s: %s\n", L[int(lvl)], (unsigned long)esp_log_timestamp(), tag, msg);
}

struct ThreadArgs {
  void (*fn)(void*);
  void* arg;
};

static void task_trampoline(void* p) {
  ThreadArgs a = *static_cast<ThreadArgs*>(p);
  delete static_cast<ThreadArgs*>(p);
  a.fn(a.arg);
  vTaskDelete(nullptr);
}

// Network, TLS, decoding and control work all live on core 0 (with Wi-Fi/lwIP);
// core 1 is reserved for the realtime audio path (output task, USB host, UAC).
void spawn_thread(const char* name, size_t stack, bool psram, int prio, void (*fn)(void*), void* arg) {
  spawn_thread_on(name, stack, psram, prio, 0, fn, arg);
}

void spawn_thread_on(const char* name, size_t stack, bool psram, int prio, int core, void (*fn)(void*), void* arg) {
  BaseType_t aff = core < 0 ? tskNO_AFFINITY : core;
  auto* a = new ThreadArgs{fn, arg};
  BaseType_t ok;
  if (psram) {
    ok = xTaskCreatePinnedToCoreWithCaps(task_trampoline, name, stack, a, prio, nullptr, aff,
                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  } else {
    ok = xTaskCreatePinnedToCore(task_trampoline, name, stack, a, prio, nullptr, aff);
  }
  if (ok != pdPASS) ESP_LOGE("platform", "failed to start task %s (stack %u)", name, (unsigned)stack);
}

void random_bytes(uint8_t* out, size_t n) { esp_fill_random(out, n); }

void* big_alloc(size_t n) {
  void* p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  return p ? p : malloc(n);
}
void big_free(void* p) { heap_caps_free(p); }

}  // namespace listenica
