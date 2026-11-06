#include "device_login.h"
#include <atomic>
#include <ctime>
#include <mutex>
#include "cJSON.h"
#include "esp_log.h"
#include "listenica/device_signin.h"
#include "listenica/platform.h"

using namespace listenica;
static const char* TAG = "signin";

static std::mutex s_mu;
static std::string s_state, s_code, s_url, s_message;
static uint64_t s_expires_at = 0;
static std::atomic<bool> s_running{false};
static std::function<void(const Credentials&)> s_done;

static void set_state(const std::string& st, const std::string& msg = "") {
  std::lock_guard<std::mutex> l(s_mu);
  s_state = st;
  s_message = msg;
  if (st == "done") s_code.clear();
}

// The flow itself is listenica::device_signin (core, shared with the host).
static void login_task(void*) {
  // HTTPS needs a plausible date: right after a wipe the clock starts at 1970 until SNTP answers
  for (int i = 0; i < 60 && time(nullptr) < 1700000000; i++) {
    set_state("starting");
    sleep_ms(1000);
  }
  SigninEvents ev;
  ev.code = [](const std::string& code, const std::string& url, int expires_s) {
    {
      std::lock_guard<std::mutex> l(s_mu);
      s_code = code;
      s_url = url;
      s_expires_at = now_ms() + uint64_t(expires_s) * 1000;
    }
    printf("SIGNIN code %s %s\n", code.c_str(), url.c_str());  // the web installer reads this line
    fflush(stdout);
  };
  ev.state = [](const std::string& st, const std::string& msg) { set_state(st, msg); };
  Credentials re;
  if (device_signin(ev, re)) {
    ESP_LOGI(TAG, "signed in");
    if (s_done) s_done(re);
  }
  s_running = false;
}

void device_login_start(std::function<void(const Credentials&)> on_signed_in) {
  if (s_running.exchange(true)) return;
  s_done = std::move(on_signed_in);
  // internal-RAM stack: the callback saves the login to flash
  spawn_thread("signin", 12288, false, 4, login_task, nullptr);
}

std::string device_login_json() {
  std::lock_guard<std::mutex> l(s_mu);
  if (s_state.empty()) return "";
  cJSON* j = cJSON_CreateObject();
  cJSON_AddStringToObject(j, "state", s_state.c_str());
  if (!s_code.empty()) {
    cJSON_AddStringToObject(j, "code", s_code.c_str());
    cJSON_AddStringToObject(j, "url", s_url.c_str());
    uint64_t now = now_ms();
    cJSON_AddNumberToObject(j, "expires_in", s_expires_at > now ? double((s_expires_at - now) / 1000) : 0.0);
  }
  if (!s_message.empty()) cJSON_AddStringToObject(j, "message", s_message.c_str());
  char* out = cJSON_PrintUnformatted(j);
  std::string r(out);
  cJSON_free(out);
  cJSON_Delete(j);
  return r;
}
