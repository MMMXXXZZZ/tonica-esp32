#include "provisioning.h"
#include <cctype>
#include <cstdio>
#include <cstring>
#include <memory>
#include <sstream>
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "listenica/platform.h"
#include "wifi_setup.h"

static const char* TAG = "provision";
static WifiSetupHooks s_setup;  // set by main once Wi-Fi is up; empty until then
void console_set_setup_hooks(const WifiSetupHooks& hooks) { s_setup = hooks; }

static void setup_reply(const char* tag, const std::string& json) {
  std::string line = std::string("SETUP ") + tag + " " + json + "\n";  // one write: never split by logs
  fputs(line.c_str(), stdout);
  fflush(stdout);
}
// Playlists and voice previews go over the network: own task, PSRAM stack (no flash I/O there).
static void setup_playlists_task(void*) {
  int status = 0;
  setup_reply("playlists", s_setup.playlists_json ? s_setup.playlists_json(status) : "{\"error\":\"not ready\"}");
}
static void setup_preview_task(void* arg) {
  std::unique_ptr<std::string> body(static_cast<std::string*>(arg));
  setup_reply("preview", s_setup.preview_json ? s_setup.preview_json(*body) : "{\"ok\":false,\"message\":\"not ready\"}");
}
static const char* NS = "soloist";
static void (*g_tone)(uint32_t) = nullptr;
void console_set_tone_handler(void (*fn)(uint32_t)) { g_tone = fn; }
static void (*g_follow)(const char*) = nullptr;
void console_set_follow_handler(void (*fn)(const char*)) { g_follow = fn; }
static void (*g_update)(const char*) = nullptr;
void console_set_update_handler(void (*fn)(const char*)) { g_update = fn; }

static std::string nvs_str(nvs_handle_t h, const char* k) {
  size_t n = 0;
  if (nvs_get_str(h, k, nullptr, &n) != ESP_OK || n == 0) return "";
  std::string s(n, '\0');
  nvs_get_str(h, k, &s[0], &n);
  s.resize(n ? n - 1 : 0);
  return s;
}

void settings_load(Settings& s) {
  nvs_handle_t h;
  if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return;
  s.ssid = nvs_str(h, "ssid");
  s.password = nvs_str(h, "pass");
  s.name = nvs_str(h, "name");
  // Migration (2026-11-02 rename): the old default saved as an override is not a user choice
  if (s.name == "Soloist ESP32") s.name.clear();
  s.ota_token = nvs_str(h, "otatoken");
  int32_t txp = 20;
  if (nvs_get_i32(h, "txpower", &txp) == ESP_OK) s.txpower_dbm = txp;
  int32_t vol = -1;
  if (nvs_get_i32(h, "volume", &vol) == ESP_OK) s.volume = vol;
  s.creds.username = nvs_str(h, "user");
  uint32_t t = 1;
  nvs_get_u32(h, "atype", &t);
  s.creds.auth_type = t;
  s.creds.auth_data = listenica::unhex(nvs_str(h, "adata"));
  auto geti = [&](const char* k, int& out) {
    int32_t v;
    if (nvs_get_i32(h, k, &v) == ESP_OK) out = v;
  };
  geti("bitrate", s.bitrate);
  geti("takeover", s.takeover);
  geti("grace", s.grace_sec);
  geti("gidle", s.guest_idle_min);
  geti("gmax", s.guest_max_min);
  geti("wdog", s.watchdog_min);
  geti("autoupd", s.auto_update);
  geti("rswin", s.resume_window_min);
  s.preset_uri = nvs_str(h, "puri");
  s.preset_mode = nvs_str(h, "pmode");
  s.resume_ctx = nvs_str(h, "rsctx");
  s.resume_track = nvs_str(h, "rstrk");
  geti("rspos", s.resume_pos_ms);
  s.preset_name = nvs_str(h, "pname");
  s.tts_key = nvs_str(h, "ttskey");
  s.tts_voice = nvs_str(h, "ttsvoice");
  s.tts_style = nvs_str(h, "ttsstyle");
  nvs_close(h);
}

static void put(const char* k, const std::string& v) {
  nvs_handle_t h;
  if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return;
  nvs_set_str(h, k, v.c_str());
  nvs_commit(h);
  nvs_close(h);
}

void settings_save_volume(uint16_t v) {
  nvs_handle_t h;
  if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return;
  nvs_set_i32(h, "volume", v);
  nvs_commit(h);
  nvs_close(h);
}

void settings_save_name(const std::string& name) { put("name", name); }

int64_t settings_load_last_time() {
  nvs_handle_t h;
  int64_t t = 0;
  if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return 0;
  nvs_get_i64(h, "lasttime", &t);
  nvs_close(h);
  return t;
}
void settings_save_last_time(int64_t t) {
  nvs_handle_t h;
  if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return;
  nvs_set_i64(h, "lasttime", t);
  nvs_commit(h);
  nvs_close(h);
}

std::string settings_load_endpoints() {
  nvs_handle_t h;
  if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return "";
  std::string s = nvs_str(h, "endpts");
  nvs_close(h);
  return s;
}
void settings_save_endpoints(const std::string& packed) { put("endpts", packed); }

void settings_save_runtime(const Settings& s) {
  nvs_handle_t h;
  if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return;
  auto seti = [&](const char* k, int v) {  // write only on change: NVS writes wear flash and stall audio
    int32_t cur;
    if (nvs_get_i32(h, k, &cur) != ESP_OK || cur != v) nvs_set_i32(h, k, v);
  };
  auto sets = [&](const char* k, const std::string& v) {
    if (nvs_str(h, k) != v) nvs_set_str(h, k, v.c_str());
  };
  seti("bitrate", s.bitrate);
  seti("takeover", s.takeover);
  seti("grace", s.grace_sec);
  seti("gidle", s.guest_idle_min);
  seti("gmax", s.guest_max_min);
  seti("wdog", s.watchdog_min);
  seti("autoupd", s.auto_update);
  seti("rswin", s.resume_window_min);
  sets("puri", s.preset_uri);
  sets("pmode", s.preset_mode);
  sets("pname", s.preset_name);
  sets("ttskey", s.tts_key);
  sets("ttsvoice", s.tts_voice);
  sets("ttsstyle", s.tts_style);
  nvs_commit(h);
  nvs_close(h);
}

void settings_save_resume(const std::string& ctx, const std::string& track, int pos_ms) {
  nvs_handle_t h;
  if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return;
  nvs_set_str(h, "rsctx", ctx.c_str());
  nvs_set_str(h, "rstrk", track.c_str());
  nvs_set_i32(h, "rspos", pos_ms);
  nvs_commit(h);
  nvs_close(h);
}

void settings_save_wifi(const std::string& ssid, const std::string& password) {
  put("ssid", ssid);
  put("pass", password);
}

void settings_save_creds(const listenica::Credentials& c) {
  nvs_handle_t h;
  if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return;
  nvs_set_str(h, "user", c.username.c_str());
  nvs_set_u32(h, "atype", c.auth_type);
  nvs_set_str(h, "adata", listenica::hex(c.auth_data).c_str());
  nvs_commit(h);
  nvs_close(h);
}

struct ConsoleCtx {
  Settings* s;
  void (*on_status)();
};

static void handle_line(ConsoleCtx* ctx, const std::string& line) {
  std::istringstream is(line);
  std::string cmd;
  is >> cmd;
  if (cmd == "wifi") {
    std::string ssid, pass;
    is >> ssid;
    std::getline(is, pass);
    if (!pass.empty() && pass[0] == ' ') pass.erase(0, 1);
    put("ssid", ssid);
    put("pass", pass);
    printf("OK wifi saved (ssid=%s); reboot to apply\n", ssid.c_str());
  } else if (cmd == "wifix") {
    // wifix <ssid hex> [<password hex>]: hex-encoded UTF-8, so names with spaces or any other
    // character survive (the web installer sends this one; 'wifi' splits the name at a space).
    std::string hs, hp;
    is >> hs >> hp;
    auto is_hex = [](const std::string& h) {
      if (h.size() % 2) return false;
      for (char c : h)
        if (!isxdigit((unsigned char)c)) return false;
      return true;
    };
    auto sb = listenica::unhex(hs), pb = listenica::unhex(hp);
    if (hs.empty() || !is_hex(hs) || !is_hex(hp) || sb.size() > 32 || pb.size() > 64) {
      printf("ERR usage: wifix <ssid hex, 1-32 bytes> [<password hex, 0-64 bytes>]\n");
    } else {
      std::string ssid(sb.begin(), sb.end()), pass(pb.begin(), pb.end());
      put("ssid", ssid);
      put("pass", pass);
      printf("OK wifi saved (ssid=%s); reboot to apply\n", ssid.c_str());
    }
  } else if (cmd == "creds") {
    listenica::Credentials c;
    std::string hexdata;
    is >> c.username >> c.auth_type >> hexdata;
    c.auth_data = listenica::unhex(hexdata);
    if (c.username.empty() || c.auth_data.empty()) {
      printf("ERR usage: creds <username> <auth_type> <hex>\n");
      return;
    }
    settings_save_creds(c);
    ctx->s->creds = c;
    printf("OK creds saved (%u bytes)\n", (unsigned)c.auth_data.size());
  } else if (cmd == "name") {
    std::string n;
    std::getline(is, n);
    if (!n.empty() && n[0] == ' ') n.erase(0, 1);
    put("name", n);
    printf("OK name saved; reboot to apply\n");
  } else if (cmd == "forget") {
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) == ESP_OK) {
      nvs_erase_all(h);
      nvs_commit(h);
      nvs_close(h);
    }
    printf("OK settings erased\n");
  } else if (cmd == "otatoken") {
    std::string t;
    is >> t;
    put("otatoken", t);
    printf("OK otatoken saved (%u chars); reboot to apply\n", (unsigned)t.size());
  } else if (cmd == "txpower") {
    int dbm = 13;
    is >> dbm;
    if (dbm < 2 || dbm > 20) {
      printf("ERR txpower range 2..20 dBm\n");
    } else {
      nvs_handle_t h;
      if (nvs_open(NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_i32(h, "txpower", dbm);
        nvs_commit(h);
        nvs_close(h);
      }
      esp_wifi_set_max_tx_power(int8_t(dbm * 4));  // units of 0.25 dBm
      ctx->s->txpower_dbm = dbm;
      printf("OK txpower %d dBm (applied now and saved)\n", dbm);
    }
  } else if (cmd == "follow") {
    std::string arg;
    is >> arg;
    if (g_follow) g_follow(arg.c_str());
    else printf("ERR not ready\n");
  } else if (cmd == "update") {  // update: check GitHub; update install: install a newer signed release
    std::string arg;
    is >> arg;
    if (g_update) g_update(arg.c_str());
    else printf("ERR not ready\n");
  } else if (cmd == "tone") {
    uint32_t ms = 3000;
    is >> ms;
    if (g_tone) g_tone(ms);
    else printf("ERR no audio\n");
  } else if (cmd == "setupstate") {
    // the hotspot page adds the scanned networks; over USB there is no scan list
    std::string st = s_setup.state_json ? s_setup.state_json() : "{}";
    setup_reply("state", st.size() > 2 ? "{\"networks\":[]," + st.substr(1) : std::string("{\"networks\":[]}"));
  } else if (cmd == "setup") {
    std::string body;
    std::getline(is, body);
    if (!s_setup.save) {
      setup_reply("save", "{\"ok\":false,\"message\":\"Tónica is still starting. Try again in a few seconds.\"}");
    } else {
      bool close = false, reboot = false;
      setup_reply("save", s_setup.save(body, close, reboot));
      if (reboot && s_setup.reboot) {
        listenica::sleep_ms(300);  // let the reply leave the UART
        s_setup.reboot();
      }
    }
  } else if (cmd == "setupplaylists") {
    listenica::spawn_thread("setup_pl", 8192, true, 3, setup_playlists_task, nullptr);
  } else if (cmd == "setuppreview") {
    auto* body = new std::string;
    std::getline(is, *body);
    listenica::spawn_thread("setup_pv", 8192, true, 3, setup_preview_task, body);
  } else if (cmd == "status") {
    if (ctx->on_status) ctx->on_status();
  } else if (cmd == "reboot") {
    printf("OK rebooting\n");
    fflush(stdout);
    listenica::sleep_ms(200);
    esp_restart();
  } else if (!cmd.empty()) {
    printf("ERR unknown command '%s'\n", cmd.c_str());
  }
  fflush(stdout);
}

static void console_task(void* p) {
  auto* ctx = static_cast<ConsoleCtx*>(p);
  // Read raw bytes from UART0 (the console UART) via the driver.
  uart_driver_install(UART_NUM_0, 4096, 0, 0, nullptr, 0);
  std::string line;
  uint8_t buf[128];
  for (;;) {
    int n = uart_read_bytes(UART_NUM_0, buf, sizeof buf, pdMS_TO_TICKS(100));
    for (int i = 0; i < n; i++) {
      char c = char(buf[i]);
      if (c == '\r' || c == '\n') {
        if (!line.empty()) handle_line(ctx, line);
        line.clear();
      } else if (line.size() < 4096) {  // 'setup <json>' carries the whole settings form
        line += c;
      }
    }
  }
}

void console_start(Settings* s, void (*on_status)()) {
  auto* ctx = new ConsoleCtx{s, on_status};
  xTaskCreate(console_task, "console", 8192, ctx, 3, nullptr);  // runs the setup save (cJSON, NVS)
  ESP_LOGI(TAG, "serial console ready (wifi/creds/name/status/reboot)");
}
