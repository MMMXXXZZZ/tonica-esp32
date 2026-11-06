// Platform glue shared by the ESP-IDF firmware and the host (Windows/Linux)
// test build. Everything above this layer is portable C++17.
#pragma once
#include <cstdint>
#include <cstdarg>
#include <string>
#include <vector>

namespace listenica {

using Bytes = std::vector<uint8_t>;

uint64_t now_ms();     // monotonic milliseconds
int64_t now_us();      // monotonic microseconds (group clock sync)
int64_t unix_ms();     // wall clock, ms since epoch (SNTP-synced on the ESP32)
void sleep_ms(uint32_t ms);

enum class Level { Error, Warn, Info, Debug, Verbose };
void log_write(Level lvl, const char* tag, const char* fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 3, 4)))
#endif
    ;
void set_log_level(Level lvl);

// Path to a PEM CA bundle on the host; ignored on the ESP32 (uses the IDF
// certificate bundle).
void set_ca_bundle_path(const std::string& path);
const std::string& ca_bundle_path();

// Starts a named thread with the given stack size; on the ESP32 the stack is
// placed in PSRAM when psram=true.
void spawn_thread(const char* name, size_t stack, bool psram, int prio, void (*fn)(void*), void* arg);
// Same, pinned to a CPU core on the ESP32 (core < 0: any core). Ignored on the host.
void spawn_thread_on(const char* name, size_t stack, bool psram, int prio, int core, void (*fn)(void*), void* arg);

std::string hex(const uint8_t* d, size_t n);
inline std::string hex(const Bytes& b) { return hex(b.data(), b.size()); }
Bytes unhex(const std::string& s);
std::string base64_encode(const uint8_t* d, size_t n);
inline std::string base64_encode(const Bytes& b) { return base64_encode(b.data(), b.size()); }
Bytes base64_decode(const std::string& s);
std::string strfmt(const char* fmt, ...);

}  // namespace listenica

#define SLOGE(tag, ...) ::listenica::log_write(::listenica::Level::Error, tag, __VA_ARGS__)
#define SLOGW(tag, ...) ::listenica::log_write(::listenica::Level::Warn, tag, __VA_ARGS__)
#define SLOGI(tag, ...) ::listenica::log_write(::listenica::Level::Info, tag, __VA_ARGS__)
#define SLOGD(tag, ...) ::listenica::log_write(::listenica::Level::Debug, tag, __VA_ARGS__)
#define SLOGV(tag, ...) ::listenica::log_write(::listenica::Level::Verbose, tag, __VA_ARGS__)
