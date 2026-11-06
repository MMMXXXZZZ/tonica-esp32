// Host (Windows / Linux) implementation of listenica/platform.h
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <mutex>
#include <random>
#include <thread>
#include "listenica/crypto.h"
#include "listenica/platform.h"
#include "listenica/ring.h"

namespace listenica {

static Level g_level = Level::Info;
void set_log_level(Level l) { g_level = l; }

uint64_t now_ms() {
  using namespace std::chrono;
  return uint64_t(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}
int64_t now_us() {
  using namespace std::chrono;
  return int64_t(duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count());
}
int64_t unix_ms() {
  using namespace std::chrono;
  return int64_t(duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count());
}
void sleep_ms(uint32_t ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

void log_write(Level lvl, const char* tag, const char* fmt, ...) {
  if (lvl > g_level) return;
  static std::mutex mu;
  static const char* L = "EWIDV";
  char msg[1024];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(msg, sizeof msg, fmt, ap);
  va_end(ap);
  int64_t t = unix_ms();
  time_t s = time_t(t / 1000);
  struct tm tmv;
#ifdef _WIN32
  localtime_s(&tmv, &s);
#else
  localtime_r(&s, &tmv);
#endif
  std::lock_guard<std::mutex> l(mu);
  fprintf(stderr, "%02d:%02d:%02d.%03d %c %-9s %s\n", tmv.tm_hour, tmv.tm_min, tmv.tm_sec, int(t % 1000),
          L[int(lvl)], tag, msg);
  fflush(stderr);
}

void spawn_thread(const char*, size_t, bool, int, void (*fn)(void*), void* arg) {
  std::thread(fn, arg).detach();
}
void spawn_thread_on(const char*, size_t, bool, int, int, void (*fn)(void*), void* arg) {
  std::thread(fn, arg).detach();
}

void random_bytes(uint8_t* out, size_t n) {
  static std::random_device rd;  // MSVC/libstdc++: OS CSPRNG
  static std::mutex mu;
  std::lock_guard<std::mutex> l(mu);
  for (size_t i = 0; i < n; i++) out[i] = uint8_t(rd());
}

void* big_alloc(size_t n) { return malloc(n); }
void big_free(void* p) { free(p); }

}  // namespace listenica
