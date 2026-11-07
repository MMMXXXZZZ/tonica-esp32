#include <sys/time.h>
#include <algorithm>
// tonica: Spotify Connect receiver for the ESP32-S3 (N16R8).
#include <atomic>
#include <cstring>
#include <memory>
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_system.h"
#include <fcntl.h>
#include "lwip/sockets.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "i2s_sink.h"
#include "net_services.h"
#include "zeroconf_service.h"
#include <mutex>
#include "status_led.h"
#include "takeover.h"
#include "button.h"
#include "wifi_setup.h"
#include "auto_update.h"
#include "event_log.h"
#include "notify.h"
#include "device_login.h"
#include "cJSON.h"
#include "sinfonica.h"
#include "listenica/pb.h"
#include "usb_audio_sink.h"
#include "nvs_flash.h"
#include "provisioning.h"
#include "listenica/ap_session.h"
#include "listenica/connect.h"
#include "listenica/crypto.h"
#include "listenica/net.h"
#include "listenica/player.h"
#include "listenica/spclient.h"
#include "listenica/version.h"

using namespace listenica;
static const char* TAG = "main";

static Settings g_settings;
static EventGroupHandle_t g_wifi_events;
static constexpr int WIFI_UP = BIT0;
static std::unique_ptr<ApSession> g_ap;
static std::unique_ptr<SpClient> g_sp;
static std::unique_ptr<Player> g_player;
static std::unique_ptr<Connect> g_connect;
static I2sSink* g_sink = nullptr;
static UsbAudioSink g_usb;
static RouterSink* g_router = nullptr;
static std::string g_device_id;
static std::mutex g_cred_mu;  // guards g_settings.creds / g_owner (zeroconf runs in the httpd task)
// Session account: the owner (NVS) or, for this boot only, a guest (RTC; never saved).
static std::string g_owner;
static bool g_guest = false;
static std::string g_temp_owner;        // claimed in the pairing window (RTC; "" = none)
static bool g_session_temp_owner = false;  // this boot's session is the temporary owner's
static uint64_t g_grace_until_ms = 0;  // pairing window after a fresh (owner) boot; 0 = none
static bool g_grace_manual = false;    // opened with the button: open even while the owner's music plays
static std::atomic<int> g_like_state{0};  // button Like: 0 pending, 1 ok, -1 failed
static std::atomic<bool> g_session_ok{false};  // Wi-Fi + AP + dealer up (set by the main loop)
static BootIssue g_boot_issue = BootIssue::None;
static ZeroconfHooks g_zc_hooks;  // phone pairing starts once there is an owner
static std::string g_zc_name;
static bool g_zc_started = false;
static BootIssue boot_issue_detect_cached() { return g_boot_issue; }

// Wi-Fi handlers run on sys_evt (2.3 KB stack): they only record what happened; logging and
// spoken notices happen elsewhere (formatted logging + queueing there overflowed it, 2026-11-03).
static std::atomic<int> g_wifi_drop_reason{-1};

static void on_wifi(void*, esp_event_base_t base, int32_t id, void* data) {
  if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
    esp_wifi_connect();
  } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
    auto* d = static_cast<wifi_event_sta_disconnected_t*>(data);
    ESP_LOGW(TAG, "Wi-Fi disconnected (reason %d), reconnecting", d->reason);
    g_wifi_drop_reason = d->reason;  // event-logged by the main loop
    bool was_up = xEventGroupGetBits(g_wifi_events) & WIFI_UP;
    xEventGroupClearBits(g_wifi_events, WIFI_UP);
    if (was_up) notify_wifi(false);  // atomic only; the notify worker decides whether to speak
    // Not while the setup hotspot is up: each attempt scans every channel, which disturbs the
    // hotspot and makes its network scan fail. The boot wait retries; closing it resumes.
    if (!wifi_setup_active()) esp_wifi_connect();
  } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
    notify_wifi(true);
    auto* e = static_cast<ip_event_got_ip_t*>(data);
    ESP_LOGI(TAG, "got IP " IPSTR, IP2STR(&e->ip_info.ip));
    // DNS from DHCP plus public backups: one failing resolver (Quad9 handed out by the router
    // stopped answering, 2026-11-03) otherwise takes the whole Spotify session down.
    {
      esp_netif_dns_info_t d = {};
      esp_netif_get_dns_info(e->esp_netif, ESP_NETIF_DNS_MAIN, &d);
      ESP_LOGI(TAG, "DNS from DHCP: " IPSTR, IP2STR(&d.ip.u_addr.ip4));
      esp_netif_dns_info_t b = {};
      esp_netif_get_dns_info(e->esp_netif, ESP_NETIF_DNS_BACKUP, &b);
      if (b.ip.u_addr.ip4.addr == 0 || b.ip.u_addr.ip4.addr == d.ip.u_addr.ip4.addr) {
        b = {};
        b.ip.type = ESP_IPADDR_TYPE_V4;
        b.ip.u_addr.ip4.addr = ESP_IP4TOADDR(1, 1, 1, 1);
        esp_netif_set_dns_info(e->esp_netif, ESP_NETIF_DNS_BACKUP, &b);
      }
      esp_netif_dns_info_t f = {};
      f.ip.type = ESP_IPADDR_TYPE_V4;
      f.ip.u_addr.ip4.addr = ESP_IP4TOADDR(8, 8, 8, 8);
      esp_netif_set_dns_info(e->esp_netif, ESP_NETIF_DNS_FALLBACK, &f);
    }
    xEventGroupSetBits(g_wifi_events, WIFI_UP);
  }
}

static void wifi_start() {
  static bool started = false;  // a board set up from the hotspot is already running Wi-Fi
  if (started) return;
  started = true;
  g_wifi_events = xEventGroupCreate();
  ESP_ERROR_CHECK(esp_netif_init());
  ESP_ERROR_CHECK(esp_event_loop_create_default());
  esp_netif_t* nif = esp_netif_create_default_wifi_sta();
  esp_netif_set_hostname(nif, "tonica");
  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&cfg));
  esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi, nullptr);
  esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_wifi, nullptr);
  wifi_config_t wc = {};
  strncpy((char*)wc.sta.ssid, g_settings.ssid.c_str(), sizeof wc.sta.ssid - 1);
  strncpy((char*)wc.sta.password, g_settings.password.c_str(), sizeof wc.sta.password - 1);
  wc.sta.threshold.authmode = g_settings.password.empty() ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;
  wc.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
  wc.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
  ESP_ERROR_CHECK(esp_wifi_start());
  esp_wifi_set_ps(WIFI_PS_NONE);  // streaming: no modem sleep
  // Lower TX power: plenty of link margin indoors, and much smaller supply
  // current spikes (which otherwise crackle through bus-powered USB DACs).
  esp_wifi_set_max_tx_power(int8_t(g_settings.txpower_dbm * 4));
  ESP_LOGI(TAG, "Wi-Fi max TX power %d dBm", g_settings.txpower_dbm);
}

// Health: lwIP sockets in use (limit CONFIG_LWIP_MAX_SOCKETS) - a leak shows as a steady climb.
static int open_sockets() {
  int n = 0;
  for (int fd = LWIP_SOCKET_OFFSET; fd < LWIP_SOCKET_OFFSET + CONFIG_LWIP_MAX_SOCKETS; fd++)
    if (fcntl(fd, F_GETFL, 0) >= 0) n++;
  return n;
}

// Pairing window: open for GRACE_SEC after a fresh boot while the owner's music isn't playing.
static bool owner_playing() {
  if (g_guest || !g_connect) return false;
  ConnectSnapshot snap = g_connect->snapshot();
  return snap.active && snap.is_playing && !snap.is_paused;
}
static int grace_left_s() {
  uint64_t now = now_ms();
  return g_grace_until_ms > now ? int((g_grace_until_ms - now) / 1000) : 0;
}
static bool grace_open() { return grace_left_s() > 0 && (g_grace_manual || !owner_playing()); }

static std::string device_name() { return g_settings.name.empty() ? CONFIG_TONICA_DEVICE_NAME : g_settings.name; }

static bool music_playing() {
  if (!g_connect) return false;
  ConnectSnapshot s = g_connect->snapshot();
  return s.active && s.is_playing && !s.is_paused;
}

static NotifyConfig notify_config() {
  NotifyConfig c;
  c.key = g_settings.tts_key;
  c.voice = g_settings.tts_voice;
  c.style = g_settings.tts_style.empty() ? notify_default_style() : g_settings.tts_style;
  c.device_name = device_name();
  c.setup_ssid = "Tónica Setup " + g_device_id.substr(0, 4);
  return c;
}

// "Liked Songs, shuffled. " (the preset a button press started)
static std::string preset_intro() {
  std::string u = g_settings.eff_preset_uri(), name = u == "liked" ? "Liked Songs" : g_settings.preset_name;
  std::string m = g_settings.eff_preset_mode();
  std::string how = m == "shuffle" ? "shuffled" : m == "newest" ? "newest first" : m == "radio" ? "radio" : m == "resume" ? "resuming" : "";
  if (name.empty()) name = "Your preset";
  return name + (how.empty() ? "" : ", " + how) + ". ";
}

// Join a network while the hotspot stays up; restore the previous one on failure.
static bool try_join_wifi(const std::string& ssid, const std::string& pass) {
  {
    wifi_config_t old = {};
    esp_wifi_get_config(WIFI_IF_STA, &old);
    wifi_config_t wc = {};
    strncpy((char*)wc.sta.ssid, ssid.c_str(), sizeof wc.sta.ssid - 1);
    strncpy((char*)wc.sta.password, pass.c_str(), sizeof wc.sta.password - 1);
    wc.sta.threshold.authmode = pass.empty() ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;
    wc.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    wc.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    esp_wifi_disconnect();
    xEventGroupClearBits(g_wifi_events, WIFI_UP);
    esp_wifi_set_config(WIFI_IF_STA, &wc);
    esp_wifi_connect();
    bool ok = xEventGroupWaitBits(g_wifi_events, WIFI_UP, pdFALSE, pdTRUE, pdMS_TO_TICKS(20000)) & WIFI_UP;
    if (!ok) {  // back to the previous network
      esp_wifi_disconnect();
      esp_wifi_set_config(WIFI_IF_STA, &old);
      esp_wifi_connect();
    }
    return ok;
  }
}

static std::string setup_state_json() {
  cJSON* j = cJSON_CreateObject();
  cJSON* d = cJSON_AddObjectToObject(j, "device");
  cJSON_AddStringToObject(d, "name", device_name().c_str());
  cJSON_AddStringToObject(d, "host", ("tonica-" + g_device_id.substr(0, 6) + ".local").c_str());
  cJSON_AddStringToObject(d, "owner", g_owner.c_str());  // the saved account's Spotify username ("" = none yet)
  {
    std::string si = device_login_json();  // owner sign-in with a code, while it runs
    if (!si.empty()) cJSON_AddItemToObject(j, "signin", cJSON_Parse(si.c_str()));
  }
  cJSON* w = cJSON_AddObjectToObject(j, "wifi");
  cJSON_AddStringToObject(w, "current", g_settings.ssid.c_str());
  wifi_ap_record_t ap = {};
  if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) cJSON_AddNumberToObject(w, "rssi", ap.rssi);
  cJSON* st = cJSON_AddObjectToObject(j, "settings");
  cJSON_AddNumberToObject(st, "bitrate", g_settings.eff_bitrate());
  cJSON_AddNumberToObject(st, "takeover", g_settings.eff_takeover());
  cJSON_AddNumberToObject(st, "grace_sec", g_settings.eff_grace_sec());
  cJSON_AddNumberToObject(st, "guest_idle_min", g_settings.eff_guest_idle_min());
  cJSON_AddNumberToObject(st, "guest_max_min", g_settings.eff_guest_max_min());
  cJSON_AddNumberToObject(st, "watchdog_min", g_settings.eff_watchdog_min());
  cJSON_AddNumberToObject(st, "resume_window_min", g_settings.eff_resume_window_min());
  cJSON_AddNumberToObject(st, "auto_update", g_settings.eff_auto_update() ? 1 : 0);
  std::string pu = g_settings.eff_preset_uri();
  cJSON_AddStringToObject(st, "preset_uri", pu.empty() ? "off" : pu.c_str());
  cJSON_AddStringToObject(st, "preset_mode", g_settings.eff_preset_mode().c_str());
  cJSON* nt = cJSON_AddObjectToObject(j, "notify");
  cJSON_AddBoolToObject(nt, "key_set", !g_settings.tts_key.empty());
  cJSON_AddStringToObject(nt, "voice", (g_settings.tts_voice.empty() ? notify_default_voice() : g_settings.tts_voice).c_str());
  cJSON_AddStringToObject(nt, "style", (g_settings.tts_style.empty() ? notify_default_style() : g_settings.tts_style).c_str());
  cJSON_AddStringToObject(nt, "status", notify_status().c_str());
  cJSON_AddBoolToObject(nt, "can_play", g_player != nullptr);  // false mid-setup (before the restart)
  char* out = cJSON_PrintUnformatted(j);
  std::string r(out);
  cJSON_free(out);
  cJSON_Delete(j);
  return r;
}

// The account's playlists: spclient rootlist (librespot get_rootlist). SelectedListContent
// .contents(5) = ListItems { items(3) { uri(1) }, meta_items(4) { attributes(2) { name(1) }, length(3) } }.
static std::string setup_playlists_json(int& status) {
  status = 503;
  if (!g_sp || !g_ap || !g_ap->alive()) return "{\"error\":\"no Spotify session yet\"}";
  std::string path = "/playlist/v2/user/" + g_sp->username() +
                     "/rootlist?decorate=revision,attributes,length,owner,capabilities,status_code&from=0&length=200";
  HttpResponse r = g_sp->request("GET", path);
  if (r.status != 200) return "{\"error\":\"Spotify answered HTTP " + std::to_string(r.status) + "\"}";
  pb::Field contents;
  if (!pb::Reader(r.body).find(5, contents)) return "{\"error\":\"unexpected reply\"}";
  std::vector<std::string> uris, names;
  std::vector<int> lengths;
  pb::Reader(contents).each(3, [&](const pb::Field& it) { uris.push_back(pb::Reader(it).str_of(1)); });
  pb::Reader(contents).each(4, [&](const pb::Field& m) {
    pb::Reader mr(m);
    pb::Field attrs;
    names.push_back(mr.find(2, attrs) ? pb::Reader(attrs).str_of(1) : std::string());
    lengths.push_back(int(mr.varint_of(3)));
  });
  cJSON* j = cJSON_CreateObject();
  cJSON* arr = cJSON_AddArrayToObject(j, "playlists");
  for (size_t i = 0; i < uris.size(); i++) {
    if (uris[i].rfind("spotify:playlist:", 0) != 0) continue;  // folders are start-group/end-group markers
    cJSON* o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "uri", uris[i].c_str());
    cJSON_AddStringToObject(o, "name", i < names.size() && !names[i].empty() ? names[i].c_str() : uris[i].c_str());
    cJSON_AddNumberToObject(o, "tracks", i < lengths.size() ? lengths[i] : 0);
    cJSON_AddItemToArray(arr, o);
  }
  char* out = cJSON_PrintUnformatted(j);
  std::string res(out);
  cJSON_free(out);
  cJSON_Delete(j);
  status = 200;
  return res;
}

// Network time: TLS needs a plausible date. Started once, from whichever path needs it first.
static bool g_sntp_started = false;
static void ensure_sntp() {
  if (g_sntp_started) return;
  g_sntp_started = true;
  esp_sntp_config_t sc = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
  esp_netif_sntp_init(&sc);
}

// Owner sign-in with a code (see device_login.h): from the boot wait, or from the setup page
// right after its Wi-Fi step when the board has no owner yet. replace: the setup page's "Sign in
// with another account" (e.g. after lending the speaker); the current owner stays until the new
// account is approved, then the board restarts into it.
static void start_owner_signin(bool replace = false) {
  if (!replace) status_led_waiting(Waiting::Account);
  device_login_start([replace](const Credentials& c) {  // runs on the sign-in task (internal stack)
    std::string before;
    {
      std::lock_guard<std::mutex> l(g_cred_mu);
      before = g_owner;
      g_settings.creds = c;
      g_owner = c.username;
    }
    settings_save_creds(c);
    if (!replace) {
      event_log("owner signed in with a code");
      return;
    }
    temp_owner_clear();  // the previous session's temporary owner doesn't carry over
    event_log("owner changed with a code (%s)", before == c.username ? "same account" : "new account");
    reboot_with_reason("setup page: owner changed");
  });
}

static bool g_hotspot_boot = false;  // started without Wi-Fi: only a restart leaves hotspot mode
static std::atomic<bool> g_setup_wifi_saved{false};  // the setup page saved a working network

static std::string setup_save(const std::string& body, bool& close, bool& reboot) {
  close = reboot = false;
  cJSON* j = cJSON_Parse(body.c_str());
  if (!j) return "{\"ok\":false,\"message\":\"Bad request.\"}";
  auto num = [&](const char* k, int def) {
    cJSON* v = cJSON_GetObjectItem(j, k);
    return cJSON_IsNumber(v) ? v->valueint : def;
  };
  auto str = [&](cJSON* o, const char* k) {
    cJSON* v = cJSON_GetObjectItem(o, k);
    return cJSON_IsString(v) ? std::string(v->valuestring) : std::string();
  };
  // "Close hotspot" with nothing left to save (the page saves as you go)
  if (cJSON_IsTrue(cJSON_GetObjectItem(j, "close_only"))) {
    cJSON_Delete(j);
    close = true;
    if (g_hotspot_boot) reboot = true;  // only a restart leaves hotspot mode
    event_log("setup page closed");
    return std::string("{\"ok\":true,\"reboot\":") + (reboot ? "true" : "false") + "}";
  }
  // "Sign in with another account": a new owner by code. Only from the hotspot or over USB (the
  // web server checks); the hotspot closes on its timeout as usual, and the sign-in carries on.
  if (cJSON_IsTrue(cJSON_GetObjectItem(j, "change_owner"))) {
    cJSON_Delete(j);
    if (!g_session_ok) return "{\"ok\":false,\"message\":\"Tónica isn't online yet: connect it to Wi-Fi first.\"}";
    start_owner_signin(true);
    ESP_LOGI(TAG, "setup page: owner change by code started");
    event_log("owner change started from the setup page");
    return "{\"ok\":true,\"reboot\":false,\"next\":\"signin\"}";
  }
  // keep_open: a save as the user changes a setting; the hotspot stays and nothing restarts
  const bool keep_open = cJSON_IsTrue(cJSON_GetObjectItem(j, "keep_open"));
  // Wi-Fi first: a network that doesn't work means nothing is saved
  cJSON* w = cJSON_GetObjectItem(j, "wifi");
  if (cJSON_IsObject(w)) {
    std::string ssid = str(w, "ssid"), pass = str(w, "password");
    if (ssid.empty() || ssid.size() > 32 || pass.size() > 64) {
      cJSON_Delete(j);
      return "{\"ok\":false,\"message\":\"Network name or password too long.\"}";
    }
    if (!try_join_wifi(ssid, pass)) {
      cJSON_Delete(j);
      return "{\"ok\":false,\"message\":\"Could not connect to that network. Nothing was saved; Tónica stays on its current network.\"}";
    }
    settings_save_wifi(ssid, pass);
    g_settings.ssid = ssid;  // the setup page shows it as the current network
    g_settings.password = pass;
    g_setup_wifi_saved = true;
    bool has_owner;
    {
      std::lock_guard<std::mutex> l(g_cred_mu);
      has_owner = g_settings.has_creds();
    }
    if (!has_owner) {
      // One screen from start to finish: the hotspot stays up and the setup page moves on to
      // the sign-in code, over the network just joined. Settings come after (and the restart).
      ensure_sntp();
      net_init();
      start_owner_signin();
      cJSON_Delete(j);
      ESP_LOGI(TAG, "Wi-Fi saved from the setup page; owner sign-in next");
      return "{\"ok\":true,\"reboot\":false,\"next\":\"signin\"}";
    }
    reboot = true;
  }
  std::string name = str(j, "name");
  if (!name.empty() && name.size() <= 64 && name != device_name()) {
    g_settings.name = name == CONFIG_TONICA_DEVICE_NAME ? "" : name;
    settings_save_name(g_settings.name);
    reboot = true;  // announced at start-up
  }
  int br = num("bitrate", g_settings.eff_bitrate());
  if ((br == 96 || br == 160 || br == 320) && br != g_settings.eff_bitrate()) {
    g_settings.bitrate = br;
    reboot = true;  // the player picks the format at start-up
  }
  int tk = num("takeover", g_settings.eff_takeover());
  if (tk >= 0 && tk <= 2) g_settings.takeover = tk;
  auto clampv = [](int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; };
  g_settings.grace_sec = clampv(num("grace_sec", g_settings.eff_grace_sec()), 0, 3600);
  g_settings.guest_idle_min = clampv(num("guest_idle_min", g_settings.eff_guest_idle_min()), 0, 1440);
  g_settings.guest_max_min = clampv(num("guest_max_min", g_settings.eff_guest_max_min()), 0, 1440);
  g_settings.watchdog_min = clampv(num("watchdog_min", g_settings.eff_watchdog_min()), 0, 1440);
  g_settings.resume_window_min = clampv(num("resume_window_min", g_settings.eff_resume_window_min()), 0, 10080);
  g_settings.auto_update = clampv(num("auto_update", g_settings.eff_auto_update() ? 1 : 0), 0, 1);
  std::string pu = str(j, "preset_uri"), pm = str(j, "preset_mode");
  bool uri_ok = pu == "off" || pu == "liked" || pu.rfind("spotify:playlist:", 0) == 0 ||
                pu.rfind("spotify:album:", 0) == 0 || pu.rfind("spotify:artist:", 0) == 0;
  if (uri_ok) g_settings.preset_uri = pu;
  if (pm == "default" || pm == "shuffle" || pm == "newest" || pm == "resume" || (pm == "radio" && pu != "liked"))
    g_settings.preset_mode = pm;
  std::string pn = str(j, "preset_name");
  if (uri_ok && pn.size() <= 100) g_settings.preset_name = pn;
  // audible notifications: the key is only sent when typed (never echoed back)
  std::string tkey = str(j, "tts_key");
  cJSON* clr = cJSON_GetObjectItem(j, "tts_clear");
  if (cJSON_IsTrue(clr)) g_settings.tts_key.clear();
  else if (!tkey.empty() && tkey.size() <= 200) g_settings.tts_key = tkey;
  std::string tv = str(j, "tts_voice"), ts = str(j, "tts_style");
  if (!tv.empty() && tv.size() <= 64) g_settings.tts_voice = tv == notify_default_voice() ? "" : tv;
  if (ts.size() <= 300) g_settings.tts_style = ts == notify_default_style() ? "" : ts;
  settings_save_runtime(g_settings);  // only changed keys are written
  notify_configure(notify_config());   // re-renders the stored phrases if voice/style changed
  cJSON_Delete(j);
  if (keep_open && !reboot) {  // saved as the user changed it: no restart, the hotspot stays
    ESP_LOGI(TAG, "setting saved from the setup page");
    return "{\"ok\":true,\"reboot\":false}";
  }
  close = true;
  if (g_hotspot_boot) reboot = true;  // started in hotspot mode: restart onto the saved Wi-Fi
  ESP_LOGI(TAG, "settings saved from the setup page%s", reboot ? "; restarting" : "");
  event_log("settings saved from the setup page");
  return std::string("{\"ok\":true,\"reboot\":") + (reboot ? "true" : "false") + "}";
}

static WifiSetupHooks wifi_setup_hooks();
static std::string status_string();

// The saved network may be gone (the speaker moved). Waiting for it forever left no way to
// pick another one: the web server and the button only start after Wi-Fi is up. After 30 s
// without it, open the setup hotspot (no timeout) and keep retrying the saved network in the
// background while no phone is on the hotspot. A save from the page restarts onto the new
// network; if the saved one comes back first, the hotspot closes and start-up carries on.
static void wait_for_wifi_or_open_setup() {
  constexpr uint32_t kFallbackMs = 30000, kRetryMs = 20000;
  if (xEventGroupWaitBits(g_wifi_events, WIFI_UP, pdFALSE, pdTRUE, pdMS_TO_TICKS(kFallbackMs)) & WIFI_UP) return;
  ESP_LOGW(TAG, "saved Wi-Fi \"%s\" not found in %u s: opening the setup hotspot", g_settings.ssid.c_str(),
           unsigned(kFallbackMs / 1000));
  event_log("Wi-Fi \"%s\" not found at boot: setup hotspot opened", g_settings.ssid.c_str());
  if (!net_services_server())
    net_services_start("tonica-" + g_device_id.substr(0, 6), g_settings.ota_token, g_device_id, device_name(), status_string);
  g_hotspot_boot = true;  // a save restarts onto the new network
  if (!wifi_setup_start(net_services_server(), g_device_id, 0, wifi_setup_hooks())) {
    ESP_LOGE(TAG, "setup hotspot failed to start; waiting for the saved Wi-Fi");
    xEventGroupWaitBits(g_wifi_events, WIFI_UP, pdFALSE, pdTRUE, portMAX_DELAY);
    g_hotspot_boot = false;
    return;
  }
  status_led_waiting(Waiting::Wifi);
  while (!(xEventGroupWaitBits(g_wifi_events, WIFI_UP, pdFALSE, pdTRUE, pdMS_TO_TICKS(kRetryMs)) & WIFI_UP)) {
    printf("PROVISION saved Wi-Fi not found: join the 'Tónica-Setup-%.4s' hotspot to pick another network\n",
           g_device_id.c_str());
    // the saved network again, quietly: never while a phone is on the hotspot (the attempt
    // scans every channel) or while the page is testing a new network
    if (wifi_setup_clients() == 0 && !g_setup_wifi_saved) esp_wifi_connect();
  }
  status_led_waiting(Waiting::None);
  if (!g_setup_wifi_saved) {  // the saved network came back by itself
    g_hotspot_boot = false;
    wifi_setup_stop();
    ESP_LOGI(TAG, "saved Wi-Fi \"%s\" is back: setup hotspot closed", g_settings.ssid.c_str());
  }
}

static WifiSetupHooks wifi_setup_hooks() {
  WifiSetupHooks h;
  h.state_json = setup_state_json;
  h.playlists_json = setup_playlists_json;
  h.save = setup_save;
  h.reboot = [] { reboot_with_reason("setup page: settings saved"); };
  auto field = [](const std::string& body, const char* k) {
    cJSON* j = cJSON_Parse(body.c_str());
    cJSON* v = j ? cJSON_GetObjectItem(j, k) : nullptr;
    std::string r = cJSON_IsString(v) ? v->valuestring : "";
    cJSON_Delete(j);
    return r;
  };
  h.voices_json = [field](const std::string& body) { return notify_voices_json(field(body, "key")); };
  h.preview_json = [field](const std::string& body) {
    std::string st = field(body, "style");
    return notify_preview_json(field(body, "key"), field(body, "voice"), st.empty() ? notify_default_style() : st);
  };
  return h;
}

// Like the current track; `where` hears "Added to your Liked Songs" (a follower's id for its gesture).
static ActionResult like_current(const std::string& where) {
  std::string uri = g_connect->active() ? g_player->current_uri() : std::string();
  if (uri.empty() || uri.rfind("spotify:track:", 0) != 0) return ActionResult::Fail;
  g_like_state = 0;
  static std::string s_uri, s_where;
  s_uri = uri;
  s_where = where;
  // own task: the request may wait behind a state upload (SpClient serializes its HTTP client)
  spawn_thread("like", 8192, true, 3, [](void*) {
    bool ok = g_sp->collection_add(s_uri);
    g_like_state = ok ? 1 : -1;
    if (ok) notify_phrase(Phrase::Liked, s_where);
  }, nullptr);
  return ActionResult::Pending;
}

static bool whats_playing(const std::string& where) {
  if (!notify_enabled() || !music_playing()) return false;
  notify_track("Now playing? ", where);
  return true;
}

// A follower's link keeps dropping out: say so on that follower, once per device per day (once per
// power-on while the clock is unknown). The user's choice (2026-11-05): no stored phrase, on demand.
static void unstable_notice(const std::string& from) {
  static std::mutex mu;
  static std::vector<std::string> said;
  time_t now = time(nullptr);
  struct tm t;
  localtime_r(&now, &t);
  char day[16] = "this-boot";
  if (t.tm_year + 1900 >= 2025) strftime(day, sizeof day, "%Y-%m-%d", &t);
  std::string key = from + "/" + day;
  {
    std::lock_guard<std::mutex> l(mu);
    if (std::find(said.begin(), said.end(), key) != said.end()) return;
    said.push_back(key);
  }
  event_log("Sinfonica: a follower's connection is unstable");
  notify_say("The connection is too unstable for Sinfónica. Taking phones off this Wi-Fi may give the speakers a better signal.", from);
}

static ButtonActions local_button_actions() {
  ButtonActions a;
  a.is_playing = [] {
    ConnectSnapshot s = g_connect->snapshot();
    return s.active && s.is_playing && !s.is_paused;
  };
  a.toggle_play = [] {
    if (g_connect->active()) {
      ConnectSnapshot s = g_connect->snapshot();
      g_connect->local_command(s.is_playing && !s.is_paused ? "{\"endpoint\":\"pause\"}" : "{\"endpoint\":\"resume\"}");
      return ActionResult::Done;
    }
    Connect::Preset p;  // take over; else play the preset (setup page / settings.conf)
    p.uri = g_settings.eff_preset_uri();
    p.mode = g_settings.eff_preset_mode();
    p.session_window_min = uint32_t(g_settings.eff_resume_window_min());
    if (p.mode == "resume" && g_settings.resume_ctx == g_connect->preset_context(p.uri)) {
      p.resume_track = g_settings.resume_track;
      p.resume_pos_ms = uint32_t(g_settings.resume_pos_ms);
    }
    g_connect->local_activate(p);
    return ActionResult::Pending;
  };
  a.next = [] {
    if (!g_connect->active()) return false;
    g_connect->local_command("{\"endpoint\":\"skip_next\"}");
    return true;
  };
  a.previous = [](uint32_t ago) {
    if (!g_connect->active()) return false;
    g_connect->local_command("{\"endpoint\":\"skip_prev\",\"local_pressed_ago_ms\":" + std::to_string(ago) + "}");
    return true;
  };
  a.volume_get = [] { return g_connect->active() ? int(g_connect->volume()) : -1; };
  a.volume_set_now = [](uint16_t v) { g_player->set_volume(v); };
  a.volume_publish = [](uint16_t v) { g_connect->local_volume(v); };
  a.like = [] { return like_current(kHere); };
  a.like_state = [] { return g_like_state.load(); };
  a.pairing_window = [] {
    int sec = g_settings.eff_grace_sec() > 0 ? g_settings.eff_grace_sec() : 180;
    g_grace_manual = true;
    g_grace_until_ms = now_ms() + uint64_t(sec) * 1000;
    ESP_LOGI(TAG, "pairing window reopened by the button for %d s", sec);
    notify_phrase(Phrase::Pair);
    event_log("pair mode opened by the button (%d s)", sec);
    return true;
  };
  a.return_to_owner = [] {
    if (!g_guest) return false;
    reboot_with_reason("button: back to the owner");
    return true;
  };
  a.wifi_setup = [] {
    bool ok = wifi_setup_start(net_services_server(), g_device_id, 300, wifi_setup_hooks());
    if (ok) notify_phrase(Phrase::Setup);
    return ok;
  };
  a.whats_playing = [] { return whats_playing(kHere); };
  return a;
}

// The leader's buttons, also run for its followers' gestures (Sinfonica control messages).
static ButtonActions s_local_btn;

static void run_group_control(const std::string& from, const std::string& c) {
  ESP_LOGI(TAG, "follower's button: %s", c.c_str());
  const ButtonActions& a = s_local_btn;
  if (c == "toggle") {
    if (a.toggle_play) a.toggle_play();
  } else if (c == "next") {
    if (a.next) a.next();
  } else if (c.rfind("prev ", 0) == 0) {
    if (a.previous) a.previous(uint32_t(strtoul(c.c_str() + 5, nullptr, 10)));
  } else if (c.rfind("vol ", 0) == 0) {
    uint16_t v = uint16_t(std::min<unsigned long>(strtoul(c.c_str() + 4, nullptr, 10), 65535));
    if (g_connect && g_connect->active()) {
      a.volume_set_now(v);
      a.volume_publish(v);
    }
  } else if (c == "like") {
    like_current(from);  // the follower hears the confirmation
  } else if (c == "whats") {
    whats_playing(from);
  } else if (c == "unstable") {
    unstable_notice(from);
  }
}

static bool fwd(const char* c) { return sinfonica_following() && sinfonica_control(c); }

static void start_button() {
  s_local_btn = local_button_actions();
  ButtonActions a = s_local_btn;
  // On a follower, the gestures control the group's music on the leader.
  a.toggle_play = [] { return fwd("toggle") ? ActionResult::Done : sinfonica_following() ? ActionResult::Fail : s_local_btn.toggle_play(); };
  a.is_playing = [] { return sinfonica_following() ? sinfonica_leader_playing() : s_local_btn.is_playing(); };
  a.next = [] { return sinfonica_following() ? fwd("next") : s_local_btn.next(); };
  a.previous = [](uint32_t ago) {
    return sinfonica_following() ? fwd(("prev " + std::to_string(ago)).c_str()) : s_local_btn.previous(ago);
  };
  a.volume_get = [] { return sinfonica_following() ? sinfonica_leader_volume() : s_local_btn.volume_get(); };
  a.volume_set_now = [](uint16_t v) {
    if (!sinfonica_following()) s_local_btn.volume_set_now(v);  // a follower plays at the leader's volume
  };
  a.volume_publish = [](uint16_t v) {
    if (sinfonica_following()) sinfonica_control("vol " + std::to_string(v));
    else s_local_btn.volume_publish(v);
  };
  a.like = [] { return sinfonica_following() ? (fwd("like") ? ActionResult::Done : ActionResult::Fail) : s_local_btn.like(); };
  a.whats_playing = [] { return sinfonica_following() ? fwd("whats") : s_local_btn.whats_playing(); };
  a.group = [] { return sinfonica_button(); };
  a.group_state = [] { return sinfonica_button_state(); };
  button_start(std::move(a));
}

static std::string status_string() {
  char out[940];
  wifi_ap_record_t ap = {};
  int rssi = esp_wifi_sta_get_ap_info(&ap) == ESP_OK ? ap.rssi : 0;
  size_t internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  size_t internal_min = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
  size_t psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
  if (!g_player) {
    snprintf(out, sizeof out, "STATUS state=%s rssi=%d heap_int=%u heap_int_min=%u psram=%u\n",
           !g_settings.has_wifi() ? "need_wifi" : !g_settings.has_creds() ? "need_creds" : "starting", rssi,
           (unsigned)internal, (unsigned)internal_min, (unsigned)psram);
    return out;
  }
  auto st = g_player->stats();
  auto m = g_player->current_meta();
  snprintf(out, sizeof out,
      "STATUS id=%.6s boot=%s led=\"%s\" dealer=%d active=%d playing=%d pos=%u/%u vol=%u track=\"%s\" artist=\"%s\" "
      "underruns=%u reconnects=%u started=%u finished=%u failures=%u first_audio_ms=%u frames=%llu cmds=%u puts=%u/%u "
      "usb=%d usb_rate=%u usb_err=%u usb_bytes=%llu usb_uf=%u usb_q_ms=%u rssi=%d heap_int=%u heap_int_min=%u heap_int_lfb=%u psram=%u ap=%d ka=%s socks=%d up=%u guest=%d grace=%d devs=%d tts=%s rr=\"%s\"\n",
      g_device_id.c_str(), boot_issue_name(boot_issue_detect_cached()), status_led_summary(), g_connect->dealer_connected(), g_connect->active(), g_player->is_playing(), (unsigned)g_player->position_ms(),
      (unsigned)m.duration_ms, (unsigned)g_connect->volume(), m.name.c_str(), m.artist.c_str(),
      (unsigned)st.underruns, (unsigned)st.cdn_reconnects, (unsigned)st.tracks_started,
      (unsigned)st.tracks_finished, (unsigned)st.load_failures, (unsigned)st.last_load_ms,
      (unsigned long long)g_router->frames(), (unsigned)g_connect->commands_handled(),
      (unsigned)(g_connect->state_puts() - g_connect->state_put_failures()), (unsigned)g_connect->state_puts(),
      g_usb.connected(), (unsigned)g_usb.out_rate(), (unsigned)g_usb.errors(), (unsigned long long)g_usb.bytes(), (unsigned)uac_host_tx_underflows(), unsigned(g_usb.connected() ? g_usb.queued_frames() * 1000 / 44100 : 0), rssi,
      (unsigned)internal, (unsigned)internal_min,
      (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL), (unsigned)psram, g_ap && g_ap->alive(), g_ap ? g_ap->keepalive_stats().c_str() : "-", open_sockets(),
      (unsigned)(esp_timer_get_time() / 1000000), g_guest ? (g_session_temp_owner ? 2 : 1) : 0, grace_left_s(), g_connect->others_online(), notify_status().c_str(), last_reboot_reason());
  return out;
}

static void print_status() {
  std::string s = status_string();
  fputs(s.c_str(), stdout);
  fflush(stdout);
}

// Endpoint cache: "ap,ap,...|dealer,...|spclient,..." (the AP that worked last time first).
static std::string endpoints_pack(const ApResolve& a) {
  auto j = [](const std::vector<std::string>& v) {
    std::string s;
    for (auto& x : v) s += (s.empty() ? "" : ",") + x;
    return s;
  };
  return j(a.accesspoint) + "|" + j(a.dealer) + "|" + j(a.spclient);
}
static bool endpoints_unpack(const std::string& s, ApResolve& a) {
  std::vector<std::string>* lists[3] = {&a.accesspoint, &a.dealer, &a.spclient};
  size_t li = 0, p = 0;
  for (auto* l : lists) l->clear();
  while (p <= s.size() && li < 3) {
    size_t e = s.find_first_of(",|", p);
    if (e == std::string::npos) e = s.size();
    if (e > p) lists[li]->push_back(s.substr(p, e - p));
    if (e < s.size() && s[e] == '|') li++;
    p = e + 1;
  }
  return !a.accesspoint.empty();
}
static std::mutex g_endpoints_mu;
static std::string g_endpoints_pending;  // fresh apresolve result, saved by the main loop when idle

static bool ap_login(const ApResolve& ar, size_t& ap_index) {
  for (size_t tries = 0; tries < ar.accesspoint.size(); tries++) {
    const std::string& host = ar.accesspoint[(ap_index + tries) % ar.accesspoint.size()];
    auto ap = std::make_unique<ApSession>(g_device_id);
    Credentials re;
    std::string err;
    if (ap->connect_and_login(host, g_settings.creds, re, &err)) {
      if (!re.auth_data.empty() && re.auth_data != g_settings.creds.auth_data) {
        g_settings.creds = re;
        if (!g_guest) settings_save_creds(re);  // a guest's login never reaches flash
      }
      ap->start();
      if (g_player) g_player->set_session(ap.get());
      // keep the previous session object alive briefly: the player may hold it
      static std::unique_ptr<ApSession> retired;
      retired = std::move(g_ap);
      g_ap = std::move(ap);
      ap_index = (ap_index + tries) % ar.accesspoint.size();
      return true;
    }
    ESP_LOGW(TAG, "AP %s: %s", host.c_str(), err.c_str());
    if (err.find("error code 12") != std::string::npos) {
      ESP_LOGE(TAG, "credentials rejected (BadCredentials); sign in again with the code (web installer or setup page)");
      static uint64_t said = 0;
      if (g_player && (said == 0 || now_ms() - said > 3600000)) {
        said = now_ms();
        notify_phrase(Phrase::LoginExpired);
      }
      return false;
    }
  }
  return false;
}


// Guest lease and last-resort recovery. Runs beside the main loop, which can
// block for a long time inside Wi-Fi / AP reconnects.
static void guardian_task(void*) {
  const uint64_t t0 = now_ms();
  uint64_t last_play = t0, last_ok = t0, socks_bad_since = 0, heap_bad_since = 0;
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(5000));
    uint64_t now = now_ms();
    ConnectSnapshot snap = g_connect->snapshot();
    bool playing = snap.is_playing && !snap.is_paused;
    if (playing) last_play = now;
    if (g_session_ok) last_ok = now;
    if (g_guest) {
      if (g_settings.eff_guest_idle_min() > 0 && now - last_play > uint64_t(g_settings.eff_guest_idle_min()) * 60000)
        return reboot_with_reason("guest idle: back to the owner"), vTaskDelete(nullptr);
      if (g_settings.eff_guest_max_min() > 0 && now - t0 > uint64_t(g_settings.eff_guest_max_min()) * 60000)
        return reboot_with_reason("guest time limit: back to the owner"), vTaskDelete(nullptr);
    }
    if (g_settings.eff_watchdog_min() > 0 && !playing && now - last_ok > uint64_t(g_settings.eff_watchdog_min()) * 60000)
      return reboot_with_reason("watchdog: Spotify session down"), vTaskDelete(nullptr);
    // resource exhaustion: sockets (the overnight failure) or internal RAM
    bool socks_bad = open_sockets() >= CONFIG_LWIP_MAX_SOCKETS - 1;
    bool heap_bad = heap_caps_get_free_size(MALLOC_CAP_INTERNAL) < 12 * 1024;
    socks_bad_since = socks_bad ? (socks_bad_since ? socks_bad_since : now) : 0;
    heap_bad_since = heap_bad ? (heap_bad_since ? heap_bad_since : now) : 0;
    // wait for a pause unless it has persisted for 5 minutes
    auto due = [&](uint64_t since) { return since && now - since > (playing ? 300000u : 60000u); };
    if (due(socks_bad_since)) return reboot_with_reason("watchdog: sockets exhausted"), vTaskDelete(nullptr);
    if (due(heap_bad_since)) return reboot_with_reason("watchdog: internal RAM low"), vTaskDelete(nullptr);
  }
}

extern "C" void app_main() {
  esp_err_t e = nvs_flash_init();
  if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    nvs_flash_erase();
    nvs_flash_init();
  }
  log_capture_start();
  event_log_init();
  status_led_init();
  g_boot_issue = boot_issue_detect();
  boot_led_start(g_boot_issue);
  ota_guard_start();
  ESP_LOGI(TAG, "tonica %s, PSRAM %u KB free", LISTENICA_BUILD_VERSION,
           (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
  settings_load(g_settings);
  g_owner = g_settings.creds.username;
  {
    Credentials guest;
    if (guest_boot_take(guest)) {
      g_guest = true;
      g_settings.creds = guest;  // RAM only; NVS still holds the owner's login
      g_temp_owner = temp_owner_get();
      g_session_temp_owner = !g_temp_owner.empty() && guest.username == g_temp_owner;
      ESP_LOGW(TAG, "%s session for this boot (policy %s); the next reboot returns to the owner",
               g_session_temp_owner ? "temporary-owner" : "guest", takeover_policy_name(g_settings.eff_takeover()));
    } else {
      temp_owner_clear();  // back with the owner: any temporary owner is forgotten
    }
  }
  if (*last_reboot_reason()) ESP_LOGW(TAG, "last reboot: %s", last_reboot_reason());
  {
    static const char* const kReset[] = {"unknown", "power-on", "external", "software", "panic", "interrupt watchdog",
                                         "task watchdog", "watchdog", "deep sleep", "brownout", "SDIO"};
    int rr = int(esp_reset_reason());
    event_log("boot: fw %s, reset %s%s%s%s", LISTENICA_BUILD_VERSION, rr >= 0 && rr <= 10 ? kReset[rr] : "other",
              *last_reboot_reason() ? " (" : "", last_reboot_reason(), *last_reboot_reason() ? ")" : "");
  }
  if (!g_guest && g_settings.eff_grace_sec() > 0) {  // only a fresh owner boot opens the window
    g_grace_until_ms = now_ms() + uint64_t(g_settings.eff_grace_sec()) * 1000;
    status_led_grace(true);
    ESP_LOGI(TAG, "pairing window open for %d s: another account may claim the speaker", g_settings.eff_grace_sec());
  }
  console_start(&g_settings, print_status);

  // device id: stable per board, derived from the MAC
  uint8_t mac[6];
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  {
    Bytes seed(mac, mac + 6);
    // Kept from the project's former name on purpose: a new salt = a new device id, so Spotify
    // would see a new speaker and OTA targeting (X-Device-Id) would change.
    const char* salt = "soloist-esp32-device";
    seed.insert(seed.end(), salt, salt + strlen(salt));
    auto h = sha1(seed.data(), seed.size());
    g_device_id = hex(h.data(), h.size());
  }

  if (!g_settings.has_wifi()) {
    // No network saved: open the setup hotspot (the serial 'wifi' command still works too).
    wifi_start();
    console_set_setup_hooks(wifi_setup_hooks());  // web installer: the setup page over USB
    std::string cname = device_name();
    net_services_start("tonica-" + g_device_id.substr(0, 6), g_settings.ota_token, g_device_id, cname, status_string);
    wifi_setup_start(net_services_server(), g_device_id, 0, wifi_setup_hooks());
    status_led_waiting(Waiting::Wifi);  // setup to do, not a failed boot
    g_hotspot_boot = true;
    // Wait for the setup page to save a working network, then carry on with the normal start-up
    // while the hotspot stays open: sign-in, Spotify and audio come up without a restart, so the
    // rest of the setup (and the voice preview) works on the same page.
    while (!g_setup_wifi_saved) {
      printf("PROVISION no Wi-Fi saved: join the 'Tónica-Setup-%.4s' hotspot, or send 'wifi <ssid> <password>' then 'reboot'\n",
             g_device_id.c_str());
      for (int i = 0; i < 20 && !g_setup_wifi_saved; i++) sleep_ms(500);
    }
    g_hotspot_boot = false;  // from here a Close just closes the hotspot
    status_led_waiting(Waiting::None);
    ESP_LOGI(TAG, "Wi-Fi saved from the setup page; starting up with the hotspot still open");
  }
  wifi_start();
  console_set_setup_hooks(wifi_setup_hooks());  // web installer: the setup page over USB
  wait_for_wifi_or_open_setup();
  {
    // unique per board: tonica-<first 6 hex of the hashed device id>.local
    std::string host = "tonica-" + g_device_id.substr(0, 6);
    std::string cname = device_name();
    if (!net_services_server())  // already running when set up from the hotspot
      net_services_start(host, g_settings.ota_token, g_device_id, cname, status_string);
    ZeroconfHooks zh;
    zh.active_user = [] {
      std::lock_guard<std::mutex> l(g_cred_mu);
      return g_settings.creds.username;
    };
    zh.on_new_user = [](const Credentials& c) {
      std::string owner;
      bool had_session;
      {
        std::lock_guard<std::mutex> l(g_cred_mu);
        owner = g_owner;
        had_session = g_settings.has_creds();
      }
      // "Connected" (owner_priority only) for the owner or temporary owner holding the session: this speaker
      // is their active device and it is playing, or one of their apps is online. Measured 2026-11-02 on iOS:
      // a locked phone leaves Spotify's device list ~30 s after locking when paused, ~4 min while the board
      // plays, and at once when another app takes the audio, so playing must count on its own.
      bool attached = false;
      if (g_settings.eff_takeover() == 1 && (!g_guest || g_session_temp_owner) && g_connect && g_connect->active()) {
        ConnectSnapshot snap = g_connect->snapshot();
        attached = (snap.is_playing && !snap.is_paused) || g_connect->query_others_online() > 0;
      }
      TakeoverInput in;
      in.user = c.username;
      in.owner = owner;
      in.temp_owner = g_temp_owner;
      in.session_guest = g_guest;
      in.holder_attached = attached;
      in.grace_open = grace_open();
      TakeoverResult res = takeover_decide(in, g_settings.eff_takeover());
      switch (res.action) {
        case Takeover::Refuse:
          event_log("takeover refused (policy %s)", takeover_policy_name(g_settings.eff_takeover()));
          ESP_LOGW(TAG, "zeroconf: another account asked for the speaker; refused (policy %s, %s %s)",
                   takeover_policy_name(g_settings.eff_takeover()), g_session_temp_owner ? "temporary owner" : "owner",
                   attached ? "connected" : "not connected");
          return false;
        case Takeover::AcceptOwner: {
          // The owner tapped Tónica in the app: end any guest session, but keep the owner's saved
          // login (from the code sign-in). Saving the phone-handed login here replaced a working
          // login with one Spotify never makes the active device (2026-11-03). Phone pairing only
          // runs once there is an owner, so this is always the existing owner.
          if (owner.empty()) {  // (not expected) no owner yet: take this login as before
            {
              std::lock_guard<std::mutex> l(g_cred_mu);
              g_settings.creds = c;
              g_owner = c.username;
            }
            settings_save_creds(c);
          }
          temp_owner_clear();
          ESP_LOGI(TAG, "zeroconf: owner login%s", had_session ? "; restarting session" : "");
          event_log("owner %s", g_guest ? "took the speaker back" : "paired");
          if (had_session) reboot_with_reason(g_guest ? "owner took the speaker back" : "owner re-paired");
          return true;
        }
        case Takeover::AcceptGuest:
          if (!guest_boot_set(c)) return false;
          event_log("%s accepted (policy %s)", res.temp_owner ? "temporary owner" : "guest",
                    takeover_policy_name(g_settings.eff_takeover()));
          if (res.temp_owner) temp_owner_set(c.username);
          ESP_LOGI(TAG, "zeroconf: %s accepted (policy %s); switching session",
                   res.temp_owner ? "temporary owner" : "guest", takeover_policy_name(g_settings.eff_takeover()));
          reboot_with_reason(res.temp_owner ? "temporary owner took the speaker" : "guest took the speaker");
          return true;
      }
      return false;
    };
    zh.on_same_user = [] {
      if (g_connect) g_connect->query_others_online(0);  // queues a state upload; doesn't wait
    };
    // Phone pairing (zeroconf) only once there is an owner: an owner login handed over by a phone
    // never became a working Connect device (2026-11-03); owners sign in with a code instead.
    g_zc_hooks = zh;
    g_zc_name = cname;
    if (net_services_server() && g_settings.has_creds()) {
      zeroconf_service_start(net_services_server(), g_device_id, cname, zh);
      g_zc_started = true;
    }
  }

  ensure_sntp();
  // TLS needs a plausible date, not an exact one (certificates last months). A warm boot keeps
  // the clock; after a power cut, start from the last saved time. SNTP corrects either in the
  // background. Only a board that has never synced waits (that wait cost ~5 s on every boot).
  if (time(nullptr) < 1700000000) {
    int64_t last = settings_load_last_time();
    if (last > 1700000000) {
      struct timeval tv = {.tv_sec = time_t(last), .tv_usec = 0};
      settimeofday(&tv, nullptr);
      ESP_LOGI(TAG, "clock from the last saved time; SNTP will correct it");
    } else if (esp_netif_sntp_sync_wait(pdMS_TO_TICKS(15000)) != ESP_OK) {
      ESP_LOGW(TAG, "SNTP not synced yet");
    }
  }
  net_init();

  for (;;) {
    {
      std::lock_guard<std::mutex> l(g_cred_mu);
      if (g_settings.has_creds()) break;
    }
    start_owner_signin();  // a fresh board waits for its owner to sign in (LED: not E1)
    printf("PROVISION no Spotify login yet: sign in with the code (web installer or setup page)\n");
    sleep_ms(5000);
  }
  status_led_waiting(Waiting::None);
  if (!g_zc_started && net_services_server()) {  // guests can pair now that there is an owner
    zeroconf_service_start(net_services_server(), g_device_id, g_zc_name, g_zc_hooks);
    g_zc_started = true;
  }

  // Boot fast path: the endpoints that worked last time (apresolve refreshes them in the
  // background), and the client/login5 tokens fetched while the AP login runs.
  ApResolve ar;
  bool from_cache = endpoints_unpack(settings_load_endpoints(), ar);
  if (from_cache) {
    ESP_LOGI(TAG, "endpoints from cache (%u APs, first %s)", unsigned(ar.accesspoint.size()), ar.accesspoint[0].c_str());
  } else {
    while (!apresolve(ar)) {
      ESP_LOGW(TAG, "apresolve failed, retrying");
      sleep_ms(3000);
    }
  }
  g_sp = std::make_unique<SpClient>(g_device_id, ar.spclient.empty() ? "spclient.wg.spotify.com:443" : ar.spclient[0]);
  g_sp->set_credentials(g_settings.creds);
  spawn_thread("tokens", 8192, true, 4, [](void*) {
    std::string t;
    if (g_sp->access_token(t)) ESP_LOGI(TAG, "client token + login5 prefetched");
  }, nullptr);
  size_t ap_index = 0;
  while (!ap_login(ar, ap_index)) {
    if (from_cache) {  // stale cache: ask apresolve
      ESP_LOGW(TAG, "cached endpoints failed; resolving fresh ones");
      from_cache = false;
      ap_index = 0;
      while (!apresolve(ar)) sleep_ms(3000);
      continue;
    }
    sleep_ms(5000);
  }
  ota_mark_valid();  // Wi-Fi + Spotify login work: keep this image
  {
    std::rotate(ar.accesspoint.begin(), ar.accesspoint.begin() + long(ap_index), ar.accesspoint.end());
    ap_index = 0;  // the working AP first, now and next boot
    std::string packed = endpoints_pack(ar);
    if (packed != settings_load_endpoints()) settings_save_endpoints(packed);  // main task: internal stack, nothing plays yet
    if (from_cache) {
      static std::string s_working;
      s_working = ar.accesspoint[0];
      spawn_thread("apresolve", 8192, true, 3, [](void*) {
        ApResolve fresh;
        if (!apresolve(fresh) || fresh.accesspoint.empty()) return;
        auto it = std::find(fresh.accesspoint.begin(), fresh.accesspoint.end(), s_working);
        if (it != fresh.accesspoint.end()) std::rotate(fresh.accesspoint.begin(), it, it + 1);  // keep the working AP first
        std::lock_guard<std::mutex> l(g_endpoints_mu);
        g_endpoints_pending = endpoints_pack(fresh);
      }, nullptr);
    }
  }
  for (int i = 0; i < 30 && g_ap->country().empty(); i++) sleep_ms(100);
  if (!g_ap->country().empty()) g_sp->set_country(g_ap->country());

  g_usb.start();
  g_router = new RouterSink(
      [] {
#if CONFIG_TONICA_AUDIO_OUT_PDM
        g_sink = new I2sSink(I2sSink::Pdm{}, CONFIG_TONICA_PDM_LEFT_GPIO, CONFIG_TONICA_PDM_RIGHT_GPIO,
                             CONFIG_TONICA_PDM_CLK_GPIO);
#else
        g_sink = new I2sSink(CONFIG_TONICA_I2S_BCK_GPIO, CONFIG_TONICA_I2S_WS_GPIO, CONFIG_TONICA_I2S_DOUT_GPIO,
                             CONFIG_TONICA_I2S_MCLK_GPIO);
#endif
        return static_cast<listenica::AudioSink*>(g_sink);
      },
      &g_usb);
#if LISTENICA_DIAG
  console_set_tone_handler([](uint32_t ms) { g_usb.test_tone(ms); });
#endif

  PlayerConfig pc;
  pc.preferred_format = g_settings.eff_bitrate() == 320 ? OGG_VORBIS_320
                        : g_settings.eff_bitrate() == 96 ? OGG_VORBIS_96
                                                        : OGG_VORBIS_160;
  g_player = std::make_unique<Player>(g_ap.get(), g_sp.get(), g_router, pc);
  {  // Sinfonica (docs/sinfonica-study.md): every board can lead or follow
    SinfonicaHooks sh;
    sh.playing = [] {
      if (!g_connect) return false;
      ConnectSnapshot s = g_connect->snapshot();
      return s.active && s.is_playing && !s.is_paused;
    };
    sh.stop_local = [] {
      if (g_connect) g_connect->local_deactivate();
    };
    sh.control = [](const std::string& from, const std::string& c) { run_group_control(from, c); };
    sinfonica_start(g_player.get(), g_router, g_device_id, std::move(sh));
    // console 'follow <leader ip>' / 'follow off'
    console_set_follow_handler([](const char* arg) {
      std::string a = arg ? arg : "";
      if (a.empty() || a == "off") {
        sinfonica_leave();
        printf("OK follow off\n");
      } else if (sinfonica_follow(a)) {
        printf("OK following %s\n", a.c_str());
      } else {
        printf("ERR already following\n");
      }
    });
  }

  ConnectConfig cc;
  cc.name = device_name();
  cc.device_id = g_device_id;
  if (!ar.dealer.empty()) cc.dealer_hostport = ar.dealer[0];
  if (g_settings.volume >= 0) cc.initial_volume = uint16_t(g_settings.volume);  // resume last-used level
  cc.autoplay_enabled = [] { return g_ap && g_ap->autoplay() == 1; };  // the account's Autoplay switch
  g_connect = std::make_unique<Connect>(cc, g_sp.get(), g_player.get());
  g_connect->set_group_source(
      [] { return sinfonica_followers(); },
      [] { return sinfonica_following(); });
  // Persist volume changes, debounced so slider drags don't wear the flash.
  static std::atomic<int> s_pending_vol{-1};
  static std::atomic<uint64_t> s_vol_changed_ms{0};
  g_connect->set_volume_listener([](uint16_t v) {
    s_pending_vol = v;
    s_vol_changed_ms = now_ms();
  });
  {
    NotifyHooks nh;
    nh.online = [] { return (xEventGroupGetBits(g_wifi_events) & WIFI_UP) != 0; };
    nh.may_write_flash = [] { return !g_player->is_playing() && !g_player->announcing(); };
    nh.to_follower = [](const std::string& id, const int16_t* pcm, size_t n, uint32_t rate) {
      sinfonica_speak_to(id, pcm, n, rate);
    };
    notify_start(g_player.get(), nh);
    notify_configure(notify_config());
  }
  g_connect->set_notice_listener([](Connect::Notice n, const std::string& arg) {
    static bool guest_announced = false;
    switch (n) {
      case Connect::Notice::PresetStarted:  // button, nothing was playing anywhere
        notify_track(preset_intro(), kEveryone);
        break;
      case Connect::Notice::Resumed:  // button, a recent session continues
        notify_track("Resuming. ", kEveryone);
        break;
      case Connect::Notice::TookOver:  // button, music moved here from a phone: it already shows it
        break;
      case Connect::Notice::Autoplay:
        notify_phrase(arg == "album" ? Phrase::AutoplayAlbum : arg == "playlist" ? Phrase::AutoplayPlaylist : Phrase::AutoplayOther,
                      kEveryone);
        break;
      case Connect::Notice::KeysRefused:
        event_log("playback paused: Spotify refused the audio keys for this account%s", g_guest ? " (guest)" : "");
        status_led_playback_error("E4 Spotify refused this account");
        notify_phrase(Phrase::Refused, kEveryone);
        break;
      case Connect::Notice::LoadFailures:
        event_log("playback paused: 5 tracks in a row failed to load");
        status_led_playback_error("E4 tracks keep failing");
        break;
      case Connect::Notice::Transfer:  // a guest's first transfer after taking the speaker (not the owner's)
        if (g_guest && !guest_announced) {
          guest_announced = true;
          if (arg.empty()) notify_phrase(Phrase::Guest, kEveryone);
          else notify_say("Now playing from " + arg + ".", kEveryone);
        }
        break;
    }
  });
  g_connect->start();
  ESP_LOGI(TAG, "Spotify Connect device '%s' running", cc.name.c_str());
  // OTA: flash writes stall the audio path, so pause instead of stuttering, and resume after the reboot
  net_services_on_ota_start([] {
    event_log("OTA update started");
    event_log_flush();  // before the first flash erase: the update itself writes flash anyway
    // a spoken notification (up to 7 s) finishes first: erases would freeze it mid-word
    for (int i = 0; i < 80 && g_player->announcing(); i++) vTaskDelay(pdMS_TO_TICKS(100));
    if (!g_connect->active()) return;
    ConnectSnapshot snap = g_connect->snapshot();
    if (!snap.is_playing || snap.is_paused) return;
    resume_after_reboot_set(true);
    g_player->pause();                                  // fades out (~150 ms), then pauses
    g_connect->local_command("{\"endpoint\":\"pause\"}");  // and tell Spotify
    // flash erases freeze the audio path: start only once the fade has finished (<= 1 s)
    for (int i = 0; i < 100 && g_player->is_playing(); i++) vTaskDelay(pdMS_TO_TICKS(10));
    vTaskDelay(pdMS_TO_TICKS(60));                      // the sink's last buffered frames
    ESP_LOGI(TAG, "OTA: playback paused; resumes after the update");
  });
  if (resume_after_reboot_take()) {
    xTaskCreatePinnedToCore([](void*) {
      for (int i = 0; i < 60 && !(g_connect->dealer_connected() && g_ap && g_ap->alive()); i++) vTaskDelay(pdMS_TO_TICKS(500));
      ESP_LOGI(TAG, "resuming playback after the update");
      g_connect->local_activate();  // pull the (paused) session back
      for (int i = 0; i < 40 && !g_connect->active(); i++) vTaskDelay(pdMS_TO_TICKS(250));
      if (g_connect->active()) {
        vTaskDelay(pdMS_TO_TICKS(1500));
        g_connect->local_command("{\"endpoint\":\"resume\"}");
      }
      vTaskDelete(nullptr);
    }, "ota_resume", 4096, nullptr, 3, nullptr, 0);
  }
  xTaskCreatePinnedToCore(guardian_task, "guardian", 3072, nullptr, 2, nullptr, 0);
  start_button();
  {  // signed releases from GitHub, installed only while idle (auto_update.h)
    AutoUpdateHooks uh;
    uh.enabled = [] { return g_settings.eff_auto_update(); };
    uh.idle = [] {
      return !music_playing() && !(g_player && g_player->announcing()) && !notify_busy() &&
             !sinfonica_following() && sinfonica_followers() == 0 && !wifi_setup_active();
    };
    auto_update_start(std::move(uh));
    console_set_update_handler(auto_update_console);
  }

  uint64_t last_status = 0;
  for (;;) {
    sleep_ms(500);
    bool wifi_up = xEventGroupGetBits(g_wifi_events) & WIFI_UP;
    bool spotify_up = g_connect->dealer_connected() && g_ap && g_ap->alive();
    g_session_ok = wifi_up && spotify_up;
    status_led_conditions(wifi_up, spotify_up);
    if (g_grace_until_ms) {
      bool open = grace_open();
      status_led_grace(open);
      if (grace_left_s() == 0) {
        g_grace_until_ms = 0;
        g_grace_manual = false;
        ESP_LOGI(TAG, "pairing window closed");
      }
    }
    if (s_pending_vol >= 0 && now_ms() - s_vol_changed_ms > 5000) {
      int v = s_pending_vol.exchange(-1);
      if (v >= 0 && v != g_settings.volume) {
        settings_save_volume(uint16_t(v));
        g_settings.volume = v;
      }
    }
    {
      static bool rs_dirty = false;
      static std::string rs_ctx, rs_track;
      static uint32_t rs_pos = 0;
      if (g_settings.eff_preset_mode() == "resume" && !g_settings.eff_preset_uri().empty()) {
        ConnectSnapshot snap = g_connect->snapshot();
        bool playing = snap.active && snap.is_playing && !snap.is_paused;
        if (playing && snap.context_uri == g_connect->preset_context(g_settings.eff_preset_uri())) {
          rs_ctx = snap.context_uri, rs_track = snap.track_uri, rs_pos = g_player->position_ms();
          rs_dirty = true;
        } else if (rs_dirty && !playing) {  // NVS writes stall the audio path: only once it has stopped
          settings_save_resume(rs_ctx, rs_track, int(rs_pos));
          g_settings.resume_ctx = rs_ctx, g_settings.resume_track = rs_track, g_settings.resume_pos_ms = int(rs_pos);
          rs_dirty = false;
        }
      }
    }
    {  // save the clock hourly (and soon after boot) while nothing plays: next power-on starts from it
      static uint64_t last_time_save = 0;
      time_t t = time(nullptr);
      if (t > 1700000000 && !g_player->is_playing() &&
          (last_time_save == 0 ? now_ms() > 60000 : now_ms() - last_time_save > 3600000)) {
        last_time_save = now_ms();
        settings_save_last_time(int64_t(t));
      }
    }
    {  // refreshed endpoints from the background apresolve: stored once nothing plays (flash write)
      std::string pend;
      {
        std::lock_guard<std::mutex> l(g_endpoints_mu);
        pend.swap(g_endpoints_pending);
      }
      if (!pend.empty()) {
        if (!g_player->is_playing() && pend != settings_load_endpoints()) settings_save_endpoints(pend);
        else if (g_player->is_playing()) {
          std::lock_guard<std::mutex> l(g_endpoints_mu);
          g_endpoints_pending = pend;  // try again later
        }
      }
    }
    {
      static uint64_t last_drop_ev = 0;
      int reason = g_wifi_drop_reason.exchange(-1);
      if (reason >= 0 && (last_drop_ev == 0 || now_ms() - last_drop_ev > 60000)) {
        last_drop_ev = now_ms();
        event_log("Wi-Fi disconnected (reason %d)", reason);
      }
    }
    {
      ConnectSnapshot es = g_connect->snapshot();
      event_log_flush_if_idle(es.active && es.is_playing && !es.is_paused);
    }
    if (now_ms() - last_status > 10000) {
      last_status = now_ms();
      print_status();
    }
    if (!g_ap->alive()) {
      static bool reconnect_failing = false;  // one event per outage, not one per retry
      g_session_ok = false;
      if (!reconnect_failing) {
        std::string why = g_ap->end_summary();
        ESP_LOGW(TAG, "AP session lost (%s); reconnecting", why.c_str());
        event_log("Spotify session lost: %s", why.c_str());
      }
      xEventGroupWaitBits(g_wifi_events, WIFI_UP, pdFALSE, pdTRUE, portMAX_DELAY);
      ap_index++;
      if (ap_login(ar, ap_index)) {
        if (reconnect_failing) event_log("Spotify session back");
        reconnect_failing = false;
      } else {
        if (!reconnect_failing) event_log("Spotify reconnect failed; retrying every 5 s");
        reconnect_failing = true;
        sleep_ms(5000);
      }
    }
  }
}
