// Tónica's side of Sinfónica: mDNS discovery, the LED colour, and the core's Sinfonica.
#include "sinfonica.h"
#include <algorithm>
#include <memory>
#include <vector>
#include "esp_log.h"
#include "esp_netif.h"
#include "listenica/sinfonica.h"
#include "mdns.h"
#include "notify.h"
#include "status_led.h"

using namespace listenica;

static const char* TAG = "sinfonica";
static std::unique_ptr<Sinfonica> s_sinf;
static bool s_fail_said = true;  // re-armed by each gesture that searches the network

// The other Tónicas (and Listenica hosts) on the LAN: their IPv4 addresses, from mDNS.
static std::vector<std::string> discover() {
  std::vector<std::string> out;
  esp_netif_ip_info_t me = {};
  esp_netif_get_ip_info(esp_netif_get_handle_from_ifkey("WIFI_STA_DEF"), &me);
  mdns_result_t* res = nullptr;
  if (mdns_query_ptr("_listenica", "_tcp", 1500, 16, &res) != ESP_OK) return out;
  auto add = [&](const esp_ip4_addr_t& a4) {
    if (a4.addr == me.ip.addr) return;
    char ip[16];
    esp_ip4addr_ntoa(&a4, ip, sizeof ip);
    if (std::find(out.begin(), out.end(), ip) == out.end()) out.push_back(ip);
  };
  for (mdns_result_t* r = res; r; r = r->next) {
    bool have_v4 = false;
    for (mdns_ip_addr_t* a = r->addr; a; a = a->next) {
      if (a->addr.type != ESP_IPADDR_TYPE_V4) continue;
      have_v4 = true;
      add(a->addr.u_addr.ip4);
    }
    // macOS may answer without an IPv4 record (seen 2026-11-05: a Mac host was listed but never
    // invited): look its host name up instead
    esp_ip4_addr_t a4 = {};
    if (!have_v4 && r->hostname && mdns_query_a(r->hostname, 1000, &a4) == ESP_OK) add(a4);
  }
  mdns_query_results_free(res);
  return out;
}

void sinfonica_update_led() {
  if (!s_sinf) return;
  Sinfonica::Role r = s_sinf->role();
  status_led_group(r == Sinfonica::Role::Follower ? GroupRole::Follower
                   : r == Sinfonica::Role::Leader ? GroupRole::Leader
                                                  : GroupRole::None);
}

void sinfonica_start(Player* player, AudioSink* sink, const std::string& device_id, SinfonicaHooks hooks) {
  Sinfonica::Config cfg;
  cfg.device_id = device_id;
  cfg.follow_core = 1;  // beside audio_out
  cfg.follow_prio = 14;
  Sinfonica::Hooks h;
  h.playing = std::move(hooks.playing);
  h.stop_local = std::move(hooks.stop_local);
  h.discover = discover;
  h.control = std::move(hooks.control);
  h.role_changed = sinfonica_update_led;
  s_sinf = std::make_unique<Sinfonica>(player, sink, cfg, std::move(h));
  if (!s_sinf->start()) ESP_LOGW(TAG, "group server did not start");
}

int sinfonica_followers() { return s_sinf ? s_sinf->followers() : 0; }
bool sinfonica_following() { return s_sinf && s_sinf->following(); }
bool sinfonica_follow(const std::string& ip) { return s_sinf && s_sinf->follow(ip); }
void sinfonica_leave() {
  if (s_sinf) s_sinf->leave();
}
bool sinfonica_control(const std::string& c) { return s_sinf && s_sinf->control(c); }
int sinfonica_leader_volume() { return s_sinf ? s_sinf->leader_volume() : -1; }
bool sinfonica_leader_playing() { return s_sinf && s_sinf->leader_playing(); }
ActionResult sinfonica_button() {
  if (!s_sinf) return ActionResult::Fail;
  int r = s_sinf->gesture();
  if (r == 0) s_fail_said = false;
  return r > 0 ? ActionResult::Done : r == 0 ? ActionResult::Pending : ActionResult::Fail;
}
int sinfonica_button_state() {
  int st = s_sinf ? s_sinf->gesture_state() : -1;
  if (st < 0 && !s_fail_said) {  // the red blinks alone read like an error code: say what happened
    s_fail_said = true;
    notify_say("No other Tónica answered on this network.");
  }
  return st;
}

void sinfonica_speak_to(const std::string& id, const int16_t* pcm, size_t n, uint32_t rate) {
  if (s_sinf) s_sinf->speak_to(id, pcm, n, rate);
}
