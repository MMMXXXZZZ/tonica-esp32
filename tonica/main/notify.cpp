#include "notify.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <vector>
#include "cJSON.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "mbedtls/base64.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "listenica/adpcm.h"
#include "listenica/http.h"
#include "listenica/platform.h"
#include "listenica/player.h"
#include "status_led.h"

using namespace listenica;

static const char* TAG = "notify";
static const char* kTtsUrl = "https://generativelanguage.googleapis.com/v1beta/interactions";
static const char* kModel = "gemini-3.8-flash-tts";

// ------------------------------------------------------------------ state
static Player* s_player = nullptr;
static NotifyHooks s_hooks;
static std::mutex s_mu;
static std::condition_variable s_cv;
static NotifyConfig s_cfg;
static bool s_cfg_set = false;

struct Job {
  enum Kind { Stored, Say, Track } kind;
  Phrase phrase = Phrase::Count;
  std::string text;
  uint64_t queued_ms = 0;
  std::string where = kHere;
};
static std::deque<Job> s_jobs;
static bool s_working = false;

// Stored phrases: what is in flash, and what is being rendered for it.
static uint32_t s_want[16];      // phrase_hash of each phrase for the current config (s_mu)
static uint64_t s_backoff_until = 0;
static std::atomic<int> s_wifi_event{0};  // 1 lost, 2 back (set from sys_evt)
static bool s_said_lost = false;          // worker only

std::string notify_default_style() { return "Waking and authoritative, but respecting multilingual names"; }
std::string notify_default_voice() { return "Kore"; }

bool notify_enabled() {
  std::lock_guard<std::mutex> l(s_mu);
  return s_cfg_set && !s_cfg.key.empty();
}

static std::string phrase_text(Phrase p, const NotifyConfig& c) {
  switch (p) {
    case Phrase::Liked: return "Added to your Liked Songs.";
    case Phrase::Pair: return "Pair mode is open. Choose " + c.device_name + " in the Spotify app.";
    case Phrase::Setup: return "Setup hotspot is open. Join " + c.setup_ssid + " to change the settings.";
    case Phrase::AutoplayAlbum: return "That was the end of the album. Continuing with similar music.";
    case Phrase::AutoplayPlaylist: return "That was the end of the playlist. Continuing with similar music.";
    case Phrase::AutoplayOther: return "Continuing with similar music.";
    case Phrase::WifiLost: return "Wi-Fi connection lost. Reconnecting.";
    case Phrase::Online: return "Back online.";
    case Phrase::LoginExpired: return "The Spotify login has expired. Choose " + c.device_name + " in the Spotify app to sign in again.";
    case Phrase::Guest: return "A guest is now playing.";
    case Phrase::Refused: return "Spotify won't let this account play on " + c.device_name + ". Try another speaker.";
    default: return "";
  }
}

static uint32_t fnv(uint32_t h, const std::string& s) {
  for (unsigned char ch : s) h = (h ^ ch) * 16777619u;
  return (h ^ 0xff) * 16777619u;
}

// One fingerprint per phrase: the model, the voice, the style and that phrase's text. Changing a
// phrase's text re-renders that phrase only; a new voice or style re-renders them all.
static uint32_t phrase_hash(Phrase p, const NotifyConfig& c) {
  uint32_t h = fnv(2166136261u, "tnv3");
  h = fnv(h, kModel);
  h = fnv(h, c.voice);
  h = fnv(h, c.style);
  h = fnv(h, phrase_text(p, c));
  return h ? h : 1;
}

// ------------------------------------------------------------------ flash area
// Header (first 4 KB sector), then one fixed slot per phrase (24 kHz IMA ADPCM). The header is
// rewritten after every stored batch, so each phrase is usable as soon as it is in flash.
static constexpr uint32_t kAreaOff = 0xC20000, kAreaSize = 0x3E0000, kHdrSize = 4096;
struct Hdr {
  char magic[4];  // "TNV3" (TNV2 had one hash for all phrases: read as empty, rendered once more)
  uint32_t rate;
  struct {
    uint32_t off, len;  // bytes, from the area start (len 0: empty)
    uint32_t hash;      // phrase_hash of what the slot holds
  } e[16];
};
static_assert(int(Phrase::Count) <= 16, "header table too small");

static const esp_partition_t* s_part = nullptr;
static bool s_area_ok = false;

static void area_init() {
  s_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "voice");
  if (s_part) {
    s_area_ok = s_part->size >= 0x100000;
  } else {
    // older partition table: the same area, unlisted, after ota_1
    uint32_t size = 0;
    s_area_ok = esp_flash_get_size(esp_flash_default_chip, &size) == ESP_OK && size >= kAreaOff + kAreaSize;
    const esp_partition_t* p = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_1, nullptr);
    if (p && p->address + p->size > kAreaOff) s_area_ok = false;  // something lives there: don't touch it
  }
  if (!s_area_ok) ESP_LOGW(TAG, "no flash area for stored phrases");
}
static uint32_t area_size() { return s_part ? s_part->size : kAreaSize; }

// Flash access goes through a small internal-RAM bounce buffer (the caller's buffers live in PSRAM).
static bool area_read(uint32_t off, void* dst, size_t n) {
  uint8_t* bounce = (uint8_t*)heap_caps_malloc(1024, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (!bounce) return false;
  bool ok = true;
  for (size_t done = 0; done < n && ok;) {
    size_t k = std::min<size_t>(1024, n - done);
    ok = (s_part ? esp_partition_read(s_part, off + done, bounce, k)
                 : esp_flash_read(esp_flash_default_chip, bounce, kAreaOff + off + done, k)) == ESP_OK;
    if (ok) memcpy((uint8_t*)dst + done, bounce, k);
    done += k;
  }
  heap_caps_free(bounce);
  return ok;
}
static bool area_erase(uint32_t off, size_t n) {
  return (s_part ? esp_partition_erase_range(s_part, off, n)
                 : esp_flash_erase_region(esp_flash_default_chip, kAreaOff + off, n)) == ESP_OK;
}
static bool area_write(uint32_t off, const void* src, size_t n) {
  uint8_t* bounce = (uint8_t*)heap_caps_malloc(1024, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (!bounce) return false;
  bool ok = true;
  for (size_t done = 0; done < n && ok;) {
    size_t k = std::min<size_t>(1024, n - done);
    memcpy(bounce, (const uint8_t*)src + done, k);
    ok = (s_part ? esp_partition_write(s_part, off + done, bounce, k)
                 : esp_flash_write(esp_flash_default_chip, bounce, kAreaOff + off + done, k)) == ESP_OK;
    done += k;
  }
  heap_caps_free(bounce);
  return ok;
}

static bool read_hdr(Hdr& h) {
  if (!s_area_ok || !area_read(0, &h, sizeof h)) return false;
  return memcmp(h.magic, "TNV3", 4) == 0;
}

// IMA ADPCM: listenica/adpcm.h (shared with Sinfonica followers, which decode forwarded speech).

static std::vector<int16_t> load_phrase_now(Phrase p, uint32_t want, uint32_t& rate) {
  Hdr h;
  if (!read_hdr(h)) return {};
  auto& e = h.e[int(p)];
  if (!e.len || e.hash != want || e.off + e.len > area_size()) return {};
  std::vector<uint8_t> enc(e.len);
  if (!area_read(e.off, enc.data(), enc.size())) return {};
  std::vector<int16_t> pcm = adpcm_decode(enc.data(), enc.size());
  rate = h.rate;
  return pcm;
}

// Flash reads also disable the cache, which a task with a PSRAM stack (the worker) must
// not do: the read runs on a short-lived task with an internal-RAM stack.
static std::vector<int16_t> load_phrase(Phrase p, uint32_t want, uint32_t& rate) {
  struct Ctx {
    Phrase p;
    uint32_t want;
    uint32_t rate;
    std::vector<int16_t> out;
    SemaphoreHandle_t done;
  };
  Ctx c{p, want, rate, {}, xSemaphoreCreateBinary()};
  if (!c.done) return {};
  auto fn = [](void* a) {
    Ctx* c = static_cast<Ctx*>(a);
    c->out = load_phrase_now(c->p, c->want, c->rate);
    xSemaphoreGive(c->done);
    vTaskDelete(nullptr);
  };
  if (xTaskCreatePinnedToCore(fn, "voice_rd", 3072, &c, 4, nullptr, 0) != pdPASS) {
    vSemaphoreDelete(c.done);
    return {};
  }
  xSemaphoreTake(c.done, portMAX_DELAY);
  vSemaphoreDelete(c.done);
  rate = c.rate;
  return std::move(c.out);
}

// Each phrase has a fixed slot, so phrases are written a few at a time as soon as nothing
// plays, instead of all being held in PSRAM until the last one is rendered (2026-11-03:
// holding 7 of 12 used ~3.5 MB and the next render ran out of memory).
static uint32_t slot_size() {
  uint32_t s = (area_size() - kHdrSize) / uint32_t(Phrase::Count);
  return std::min<uint32_t>(s & ~4095u, 0x15000);  // <= 7.1 s at 24 kHz, IMA ADPCM (12 KB/s)
}
static uint32_t slot_off(int i) { return kHdrSize + uint32_t(i) * slot_size(); }

struct Pending {
  int idx;
  uint32_t hash;
  std::vector<uint8_t> data;  // IMA ADPCM
};
static std::deque<Pending> s_pending;  // rendered, not yet in flash (worker only)
// What the flash header says (worker only; read at start-up on an internal-RAM stack).
static Hdr s_hdr;

static bool stored(int i, const uint32_t* want) { return s_hdr.e[i].len && s_hdr.e[i].hash == want[i]; }

struct WriteJob {
  volatile int result;  // 0 running, 1 ok, -1 failed
};

static void wait_idle() {
  while (s_hooks.may_write_flash && !s_hooks.may_write_flash()) vTaskDelay(pdMS_TO_TICKS(1000));
}

static bool write_hdr() {
  wait_idle();
  if (!area_erase(0, kHdrSize)) return false;
  wait_idle();
  return area_write(0, &s_hdr, sizeof s_hdr);  // a torn write leaves no valid header: all re-rendered
}

// Runs on its own task with an internal-RAM stack: flash erase/write disables the cache, and a task
// whose stack is in PSRAM must not be running then. Each phrase: its entry is cleared in the header
// first (so it is never read half-written), then its slot is rewritten, then the entry is set.
static void writer_task(void* arg) {
  WriteJob* job = (WriteJob*)arg;
  bool ok = true;
  memcpy(s_hdr.magic, "TNV3", 4);
  s_hdr.rate = 24000;
  for (auto& pd : s_pending) {
    uint32_t off = slot_off(pd.idx), n = uint32_t(pd.data.size());
    s_hdr.e[pd.idx] = {0, 0, 0};
    ok = write_hdr();
    for (uint32_t e = 0; ok && e < slot_size(); e += 65536) {  // erase in 64 KB steps, each while idle
      wait_idle();
      ok = area_erase(off + e, std::min<uint32_t>(65536, slot_size() - e));
    }
    if (ok) {
      wait_idle();
      ok = area_write(off, pd.data.data(), n);
    }
    if (ok) {
      s_hdr.e[pd.idx] = {off, n, pd.hash};
      ok = write_hdr();
    }
    if (!ok) break;
  }
  job->result = ok ? 1 : -1;
  vTaskDelete(nullptr);
}

// ------------------------------------------------------------------ Gemini TTS
// The longest string value of any "data" key (the audio, base64): its range in the body.
static bool find_audio_b64(const Bytes& body, size_t& at, size_t& len) {
  const char* d = (const char*)body.data();
  size_t n = body.size(), p = 0;
  len = 0;
  for (;;) {
    const char* f = (const char*)memmem(d + p, n - p, "\"data\"", 6);
    if (!f) break;
    p = size_t(f - d) + 6;
    while (p < n && (d[p] == ' ' || d[p] == ':' || d[p] == '\n' || d[p] == '\r' || d[p] == '\t')) p++;
    if (p >= n || d[p] != '"') continue;
    const char* e = (const char*)memchr(d + p + 1, '"', n - p - 1);
    if (!e) break;
    size_t l = size_t(e - d) - p - 1;
    if (l > len) {
      at = p + 1;
      len = l;
    }
    p = size_t(e - d);
  }
  return len > 0;
}

// Enough PSRAM for one reply (~0.4 MB base64 + ~0.3 MB decoded + slack)? Without exceptions,
// a failed allocation aborts, so check first.
static bool tts_memory_ok() {
  return heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) >= 1200 * 1024;
}

static std::string api_error(const HttpResponse& r) {
  if (r.status == 0) return "no connection to Google";
  if (r.status == 400 || r.status == 401 || r.status == 403) {
    std::string t = r.text();
    if (t.find("API_KEY_INVALID") != std::string::npos || t.find("API key not valid") != std::string::npos)
      return "the API key is not valid";
    return "Google refused the request (HTTP " + std::to_string(r.status) + ")";
  }
  if (r.status == 429) return "Google's rate limit was reached; try again in a minute";
  return "Google returned HTTP " + std::to_string(r.status);
}

// Renders `text`; returns 24 kHz mono PCM (empty on failure, err set).
static std::vector<int16_t> tts(const std::string& key, const std::string& voice, const std::string& style,
                                const std::string& text, std::string& err, int* http_status = nullptr) {
  cJSON* j = cJSON_CreateObject();
  cJSON_AddStringToObject(j, "model", kModel);
  cJSON* input = cJSON_AddArrayToObject(j, "input");
  cJSON* ui = cJSON_CreateObject();
  cJSON_AddItemToArray(input, ui);
  cJSON_AddStringToObject(ui, "type", "user_input");
  cJSON* content = cJSON_AddArrayToObject(ui, "content");
  cJSON* t = cJSON_CreateObject();
  cJSON_AddItemToArray(content, t);
  cJSON_AddStringToObject(t, "type", "text");
  cJSON_AddStringToObject(t, "text", text.c_str());
  if (!style.empty()) {
    cJSON* ann = cJSON_AddArrayToObject(t, "annotations");
    cJSON* a = cJSON_CreateObject();
    cJSON_AddItemToArray(ann, a);
    cJSON_AddStringToObject(a, "type", "speech_metadata");
    cJSON_AddStringToObject(a, "style", style.c_str());
  }
  cJSON* rf = cJSON_AddObjectToObject(j, "response_format");
  cJSON_AddStringToObject(rf, "type", "audio");
  cJSON* gc = cJSON_AddObjectToObject(j, "generation_config");
  cJSON* sc = cJSON_AddArrayToObject(gc, "speech_config");
  cJSON* v = cJSON_CreateObject();
  cJSON_AddItemToArray(sc, v);
  cJSON_AddStringToObject(v, "voice", voice.c_str());
  char* out = cJSON_PrintUnformatted(j);
  Bytes body(out, out + strlen(out));
  cJSON_free(out);
  cJSON_Delete(j);

  HttpClient http;
  HttpResponse r = http.request("POST", kTtsUrl, {{"Content-Type", "application/json"}, {"x-goog-api-key", key}}, body, 60000);
  http.close();
  if (http_status) *http_status = r.status;
  if (r.status != 200) {
    err = api_error(r);
    return {};
  }
  size_t at = 0, blen = 0;
  if (!find_audio_b64(r.body, at, blen)) {
    err = "no audio in Google's reply";
    return {};
  }
  // decode straight from the reply buffer (no copies of the ~0.4 MB string)
  size_t olen = 0;
  mbedtls_base64_decode(nullptr, 0, &olen, r.body.data() + at, blen);
  Bytes wav(olen);
  if (!olen || mbedtls_base64_decode(wav.data(), wav.size(), &olen, r.body.data() + at, blen) != 0) {
    err = "undecodable audio in Google's reply";
    return {};
  }
  wav.resize(olen);
  Bytes().swap(r.body);
  // WAV: walk the chunks to "data"; anything else is taken as raw 24 kHz PCM
  size_t off = 0, len = wav.size();
  if (wav.size() > 12 && memcmp(wav.data(), "RIFF", 4) == 0) {
    size_t p = 12;
    len = 0;
    while (p + 8 <= wav.size()) {
      uint32_t n = wav[p + 4] | wav[p + 5] << 8 | wav[p + 6] << 16 | uint32_t(wav[p + 7]) << 24;
      if (memcmp(&wav[p], "data", 4) == 0) {
        off = p + 8;
        len = std::min<size_t>(n, wav.size() - off);
        break;
      }
      p += 8 + n + (n & 1);
    }
  }
  std::vector<int16_t> pcm(len / 2);
  if (len) memcpy(pcm.data(), wav.data() + off, pcm.size() * 2);
  if (pcm.empty()) err = "empty audio";
  return pcm;
}

// ------------------------------------------------------------------ worker
static bool remote(const std::string& where) { return !where.empty() && where != kEveryone; }

static void speak(const std::vector<int16_t>& pcm, uint32_t rate, const std::string& where) {
  if (pcm.empty()) return;
  if (remote(where)) {
    if (s_hooks.to_follower) s_hooks.to_follower(where, pcm.data(), pcm.size(), rate);
  } else if (s_player) {
    s_player->announce(pcm.data(), pcm.size(), rate, where == kEveryone);
  }
}

// The player resamples the voice into a new buffer (44.1 kHz): make sure it fits first (a failed
// allocation aborts) and refuse anything absurdly long. False = not played.
static bool speak_checked(const std::vector<int16_t>& pcm, uint32_t rate, const std::string& where = kHere) {
  if (pcm.empty() || !s_player) return false;
  if (remote(where)) {  // the follower resamples it; nothing is buffered here
    if (!s_hooks.to_follower) return false;
    s_hooks.to_follower(where, pcm.data(), pcm.size(), rate);
    return true;
  }
  size_t need = size_t(uint64_t(pcm.size()) * 44100 / rate) * 2;
  if (pcm.size() > size_t(rate) * 30) {
    ESP_LOGW(TAG, "speech too long (%u s); not played", unsigned(pcm.size() / rate));
    return false;
  }
  if (heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) < need + 64 * 1024) {
    ESP_LOGW(TAG, "not enough free PSRAM to play %u bytes of speech", unsigned(need));
    return false;
  }
  return s_player->announce(pcm.data(), pcm.size(), rate, where == kEveryone);
}

// LED for a spoken notification that failed (see LedFx::TtsFail)
static void led_tts_fail(int status_or_code) {
  int n = status_or_code == 0 ? 1                                                   // no answer
          : status_or_code == 400 || status_or_code == 401 || status_or_code == 403 ? 2  // refused
          : status_or_code == 429 ? 3                                              // rate limit
          : status_or_code == -5 ? 5                                               // memory / length
                                 : 4;                                              // unusable reply
  status_led_fx(LedFx::TtsFail, uint8_t(n));
}

// "A", "A and B", "A, B, and C"
static std::string join_artists(const std::vector<std::string>& a) {
  if (a.empty()) return "";
  if (a.size() == 1) return a[0];
  if (a.size() == 2) return a[0] + " and " + a[1];
  std::string s;
  for (size_t i = 0; i + 1 < a.size(); i++) s += (i ? ", " : "") + a[i];
  return s + ", and " + a.back();
}

static std::string lower(std::string s) {
  for (auto& ch : s) ch = char(tolower((unsigned char)ch));
  return s;
}

// "Striptease (feat. Eartheater)" -> "Striptease" when every credited name is already in the
// artist list (it would be said twice); other brackets ("(Live)", "- Remix") stay.
static std::string spoken_title(std::string t, const std::vector<std::string>& artists) {
  std::string lt = lower(t);
  for (const char* tag : {"(feat. ", "[feat. ", "(with ", "[with ", "(ft. ", "(featuring "}) {
    size_t b = lt.find(tag);
    if (b == std::string::npos) continue;
    char close = t[b] == '(' ? ')' : ']';
    size_t e = t.find(close, b);
    if (e == std::string::npos) continue;
    std::string inner = lower(t.substr(b, e - b));
    bool all = true;
    size_t p = strlen(tag);
    while (p < inner.size()) {  // names split by ", " / " & " / " and "
      size_t q = inner.size();
      for (const char* sep : {", ", " & ", " and "}) {
        size_t f = inner.find(sep, p);
        if (f != std::string::npos && f < q) q = f;
      }
      std::string name = inner.substr(p, q - p);
      bool found = false;
      for (auto& a : artists) found = found || lower(a) == name;
      all = all && found;
      p = q;
      for (const char* sep : {", ", " & ", " and "})
        if (inner.compare(p, strlen(sep), sep) == 0) {
          p += strlen(sep);
          break;
        }
      if (q == inner.size()) break;
    }
    if (!all) continue;
    while (b > 0 && t[b - 1] == ' ') b--;
    t.erase(b, e + 1 - b);
    lt = lower(t);
  }
  return t;
}

static void run_job(const Job& j, const NotifyConfig& c, const uint32_t* want) {
  if (j.kind == Job::Stored) {
    uint32_t rate = 24000;
    auto pcm = load_phrase(j.phrase, want[int(j.phrase)], rate);
    if (pcm.empty()) {
      ESP_LOGI(TAG, "phrase %d not stored yet; skipped", int(j.phrase));
      return;
    }
    speak(pcm, rate, j.where);
    return;
  }
  std::string text = j.text;
  if (j.kind == Job::Track) {
    // wait for the track to actually play (a button start takes 1-3 s)
    TrackMeta m;
    for (int i = 0; i < 150; i++) {
      if (s_player->is_playing()) {
        m = s_player->current_meta();
        if (m.ok && !m.name.empty()) break;
      }
      sleep_ms(100);
    }
    if (!m.ok || m.name.empty()) return;
    std::vector<std::string> artists = m.artists.empty() ? std::vector<std::string>{m.artist} : m.artists;
    std::string by = join_artists(artists);
    text += spoken_title(m.name, artists) + (by.empty() ? "" : ", by " + by) + ".";
  }
  if (s_hooks.online && !s_hooks.online()) {
    led_tts_fail(0);
    return;
  }
  if (!tts_memory_ok()) {
    ESP_LOGW(TAG, "not enough free PSRAM for TTS right now; skipped");
    led_tts_fail(-5);
    return;
  }
  std::string err;
  uint64_t t0 = now_ms();
  int st = 0;
  status_led_fx(LedFx::TtsPending);
  auto pcm = tts(c.key, c.voice, c.style, text, err, &st);
  if (pcm.empty()) {
    ESP_LOGW(TAG, "TTS failed: %s", err.c_str());
    led_tts_fail(st == 200 ? -4 : st);  // 200 with no usable audio counts as an unusable reply
    return;
  }
  ESP_LOGI(TAG, "said \"%s\" (%.1f s audio, rendered in %u ms)", text.c_str(), pcm.size() / 24000.0,
           unsigned(now_ms() - t0));
  if (!speak_checked(pcm, 24000, j.where)) {
    led_tts_fail(-5);
    return;
  }
  status_led_fx(LedFx::FadeWhite);
}

// One step of rendering/storing the fixed phrases; true if there is more to do.
static bool render_step(const NotifyConfig& c, const uint32_t* want) {
  const int total = int(Phrase::Count);
  int todo = -1;  // the next phrase that is neither stored nor rendered
  for (int i = 0; i < total && todo < 0; i++) {
    bool pend = false;
    for (auto& pd : s_pending) pend = pend || (pd.idx == i && pd.hash == want[i]);
    if (!stored(i, want) && !pend) todo = i;
  }
  if (!s_area_ok || c.key.empty() || (todo < 0 && s_pending.empty())) return false;
  if (now_ms() < s_backoff_until) return true;
  // store what is rendered once nothing plays; render at most 3 ahead of what is stored
  if (!s_pending.empty() && (todo < 0 || s_pending.size() >= 3 || !s_hooks.may_write_flash || s_hooks.may_write_flash())) {
    if (s_hooks.may_write_flash && !s_hooks.may_write_flash()) {
      s_backoff_until = now_ms() + 2000;  // waiting for a quiet moment to store
      return true;
    }
    static WriteJob wj;
    wj.result = 0;
    if (xTaskCreatePinnedToCore(writer_task, "voice_wr", 4096, &wj, 4, nullptr, 0) != pdPASS) {
      s_backoff_until = now_ms() + 10000;
      return true;
    }
    while (wj.result == 0) sleep_ms(100);
    if (wj.result != 1) {
      ESP_LOGW(TAG, "storing phrases failed; retrying in 10 min");
      s_backoff_until = now_ms() + 600000;
    } else {
      ESP_LOGI(TAG, "stored %u phrase(s)", unsigned(s_pending.size()));
    }
    s_pending.clear();
    return true;
  }
  if (todo < 0) return true;
  if (s_hooks.online && !s_hooks.online()) {
    s_backoff_until = now_ms() + 10000;
    return true;
  }
  if (!tts_memory_ok()) {
    s_backoff_until = now_ms() + 30000;
    return true;
  }
  std::string err;
  int st = 0;
  auto pcm = tts(c.key, c.voice, c.style, phrase_text(Phrase(todo), c), err, &st);
  if (pcm.empty()) {
    ESP_LOGW(TAG, "rendering phrase %d failed: %s", todo, err.c_str());
    // a bad key stops until the settings change; anything else retries later
    s_backoff_until = now_ms() + (st == 400 || st == 401 || st == 403 ? 3600000 : st == 429 ? 300000 : 20000);
    return true;
  }
  size_t cap = size_t(slot_size()) * 2;  // samples per slot (4 bits each)
  if (pcm.size() > cap) {  // too long for its slot: cut with a short fade
    pcm.resize(cap);
    for (size_t i = 0; i < 1200 && i < cap; i++) pcm[cap - 1 - i] = int16_t(int32_t(pcm[cap - 1 - i]) * int32_t(i) / 1200);
  }
  s_pending.push_back({todo, want[todo], adpcm_encode(pcm.data(), pcm.size())});
  return true;
}

static void worker(void*) {
  for (;;) {
    Job j;
    bool have = false;
    NotifyConfig c;
    uint32_t want[16];
    {
      std::unique_lock<std::mutex> l(s_mu);
      s_cv.wait_for(l, std::chrono::milliseconds(1000), [] { return !s_jobs.empty(); });
      if (!s_jobs.empty()) {
        j = s_jobs.front();
        s_jobs.pop_front();
        have = true;
        s_working = true;
      }
      c = s_cfg;
      memcpy(want, s_want, sizeof want);
    }
    int we = s_wifi_event.exchange(0);
    if (we == 1 && !s_said_lost && s_player && s_player->is_playing()) {
      s_said_lost = true;
      notify_phrase(Phrase::WifiLost);  // stored: plays over the buffered music, offline
    } else if (we == 2 && s_said_lost) {
      s_said_lost = false;
      notify_phrase(Phrase::Online);
    }
    if (have) {
      // stale requests (e.g. queued while offline) are dropped rather than spoken late
      if (now_ms() - j.queued_ms < 30000 && !c.key.empty()) run_job(j, c, want);
      std::lock_guard<std::mutex> l(s_mu);
      s_working = false;
      continue;
    }
    if (render_step(c, want)) sleep_ms(50);
  }
}

// ------------------------------------------------------------------ API
void notify_start(Player* player, NotifyHooks hooks) {
  s_player = player;
  s_hooks = std::move(hooks);
  area_init();
  Hdr h;
  if (read_hdr(h)) s_hdr = h;
  spawn_thread_on("notify", 12 * 1024, true, 3, 0, worker, nullptr);
}

void notify_configure(const NotifyConfig& c) {
  std::lock_guard<std::mutex> l(s_mu);
  NotifyConfig n = c;
  if (n.voice.empty()) n.voice = notify_default_voice();
  bool changed = false;
  for (int i = 0; i < int(Phrase::Count); i++) {
    uint32_t h = phrase_hash(Phrase(i), n);
    changed = changed || h != s_want[i];
    s_want[i] = h;
  }
  if (changed) s_backoff_until = 0;
  s_cfg = n;
  s_cfg_set = true;
  ESP_LOGI(TAG, "%s", n.key.empty() ? "off (no API key)" : "on");
}

static void enqueue(Job j) {
  if (!notify_enabled()) return;
  j.queued_ms = now_ms();
  std::lock_guard<std::mutex> l(s_mu);
  if (s_jobs.size() >= 4) s_jobs.pop_front();
  s_jobs.push_back(std::move(j));
  s_cv.notify_all();
}

void notify_phrase(Phrase p, const std::string& where) { enqueue({Job::Stored, p, "", 0, where}); }
void notify_wifi(bool up) { s_wifi_event = up ? 2 : 1; }
void notify_say(const std::string& text, const std::string& where) { enqueue({Job::Say, Phrase::Count, text, 0, where}); }
void notify_track(const std::string& prefix, const std::string& where) {
  enqueue({Job::Track, Phrase::Count, prefix, 0, where});
}

bool notify_busy() {
  {
    std::lock_guard<std::mutex> l(s_mu);
    if (!s_jobs.empty() || s_working) return true;
  }
  return s_player && s_player->announcing();
}

std::string notify_status() {
  std::lock_guard<std::mutex> l(s_mu);
  if (!s_cfg_set || s_cfg.key.empty()) return "off";
  int have = 0;  // s_hdr is the worker's, read here without a lock: a status line can be a step behind
  for (int i = 0; i < int(Phrase::Count); i++) have += stored(i, s_want);
  if (have == int(Phrase::Count)) return "ready";
  return "phrases_" + std::to_string(have) + "/" + std::to_string(int(Phrase::Count));
}

// ------------------------------------------------------------------ setup page
// The HTTP server's task has a small internal stack: TLS requests run on their own thread.
static std::string on_net_thread(std::function<std::string()> fn) {
  struct Ctx {
    std::function<std::string()> fn;
    std::string out;
    SemaphoreHandle_t done;
  };
  Ctx* c = new Ctx{std::move(fn), std::string(), xSemaphoreCreateBinary()};
  spawn_thread_on("notify_req", 12 * 1024, true, 4, 0, [](void* a) {
    Ctx* c = static_cast<Ctx*>(a);
    c->out = c->fn();
    xSemaphoreGive(c->done);
  }, c);
  xSemaphoreTake(c->done, portMAX_DELAY);
  std::string r = std::move(c->out);
  vSemaphoreDelete(c->done);
  delete c;
  return r;
}

// Pulls a JSON string value for `key` inside [b, e); "" if absent.
static std::string jfield(const std::string& s, size_t b, size_t e, const char* key) {
  std::string k = std::string("\"") + key + "\"";
  size_t p = s.find(k, b);
  if (p == std::string::npos || p >= e) return "";
  p += k.size();
  while (p < e && (s[p] == ' ' || s[p] == ':' || s[p] == '\n' || s[p] == '\t' || s[p] == '\r')) p++;
  if (p >= e || s[p] != '"') return "";
  std::string out;
  for (p++; p < e && s[p] != '"'; p++) {
    if (s[p] == '\\' && p + 1 < e) {
      p++;
      if (s[p] == 'u' && p + 4 < e) {  // keep \uXXXX as-is for the browser's JSON parser
        out += "\\u";
        continue;
      }
      out += s[p] == 'n' ? ' ' : s[p];
      continue;
    }
    out += s[p];
  }
  return out;
}

static void jappend_str(std::string& o, const std::string& v) {
  o += '"';
  for (size_t i = 0; i < v.size(); i++) {
    char ch = v[i];
    if (ch == '"') o += "\\\"";
    else if (ch == '\\' && !(i + 1 < v.size() && v[i + 1] == 'u')) o += "\\\\";
    else if ((unsigned char)ch < 0x20) o += ' ';
    else o += ch;
  }
  o += '"';
}

static std::string voices_json(const std::string& key_in) {
  std::string key = key_in;
  if (key.empty()) {
    std::lock_guard<std::mutex> l(s_mu);
    key = s_cfg.key;
  }
  if (key.empty()) return "{\"ok\":false,\"message\":\"Enter an API key first.\"}";
  if (s_hooks.online && !s_hooks.online())
    return "{\"ok\":false,\"message\":\"No internet yet: the voice list loads once Tónica is on your Wi-Fi.\"}";
  // Compact list: [id, name, language, gender initial, accent]. The full reply is ~850 KB
  // in 3 pages; parsed by scanning (cJSON would allocate thousands of internal-RAM nodes).
  std::string out;
  out.reserve(200 * 1024);
  out = "{\"ok\":true,\"voices\":[";
  bool first = true;
  std::string token;
  HttpClient http;
  for (int page = 0; page < 6; page++) {
    std::string url = "https://generativelanguage.googleapis.com/v1beta/voices?pageSize=1000";
    if (!token.empty()) url += "&pageToken=" + token;
    HttpResponse r = http.request("GET", url, {{"x-goog-api-key", key}}, {}, 30000);
    if (r.status != 200) {
      http.close();
      if (page == 0) return "{\"ok\":false,\"message\":\"" + api_error(r) + "\"}";
      break;  // keep what arrived
    }
    std::string s(r.body.begin(), r.body.end());
    Bytes().swap(r.body);
    size_t v = s.find("\"voices\"");
    size_t p = v == std::string::npos ? std::string::npos : s.find('[', v);
    while (p != std::string::npos) {
      size_t b = s.find('{', p);
      if (b == std::string::npos) break;
      // end of this object: string-aware brace match
      size_t e = b + 1;
      int depth = 1;
      bool str = false;
      for (; e < s.size() && depth; e++) {
        char ch = s[e];
        if (str) {
          if (ch == '\\') e++;
          else if (ch == '"') str = false;
        } else if (ch == '"') str = true;
        else if (ch == '{') depth++;
        else if (ch == '}') depth--;
      }
      std::string id = jfield(s, b, e, "id");
      if (!id.empty()) {
        out += first ? "[" : ",[";
        first = false;
        jappend_str(out, id);
        out += ',';
        jappend_str(out, jfield(s, b, e, "display_name"));
        out += ',';
        jappend_str(out, jfield(s, b, e, "language_code"));
        out += ',';
        std::string g = jfield(s, b, e, "gender");
        jappend_str(out, g.empty() ? "" : g.substr(0, 1));
        out += ',';
        jappend_str(out, jfield(s, b, e, "accent"));
        out += ']';
      }
      p = e;
      size_t close = s.find_first_not_of(" \n\r\t,", p);
      if (close == std::string::npos || s[close] == ']') break;
    }
    token = jfield(s, 0, s.size(), "next_page_token");
    if (token.empty()) token = jfield(s, 0, s.size(), "nextPageToken");
    if (token.empty()) break;
  }
  http.close();
  out += "]}";
  return out;
}

static std::string preview_json(const std::string& key_in, const std::string& voice, const std::string& style) {
  NotifyConfig c;
  {
    std::lock_guard<std::mutex> l(s_mu);
    c = s_cfg;
  }
  std::string key = key_in.empty() ? c.key : key_in;
  ESP_LOGI(TAG, "preview requested (voice %s)", voice.c_str());
  if (key.empty()) {
    ESP_LOGW(TAG, "preview: no API key");
    return "{\"ok\":false,\"message\":\"Enter an API key first.\"}";
  }
  if (s_hooks.online && !s_hooks.online()) {
    ESP_LOGW(TAG, "preview: not online");
    return "{\"ok\":false,\"message\":\"No internet yet.\"}";
  }
  // mid-setup the player may not exist yet: don't spend a request
  if (!s_player) {
    ESP_LOGW(TAG, "preview: no player yet");
    return "{\"ok\":false,\"message\":\"Preview works once Tónica has finished starting.\"}";
  }
  std::string err;
  std::string name = c.device_name.empty() ? std::string("Tónica") : c.device_name;
  if (!tts_memory_ok()) {
    led_tts_fail(-5);
    return "{\"ok\":false,\"message\":\"Not enough free memory right now. Try again in a moment.\"}";
  }
  int st = 0;
  status_led_fx(LedFx::TtsPending);
  auto pcm = tts(key, voice.empty() ? notify_default_voice() : voice, style,
                 "Hello, this is " + name + ". Now playing? Clair de Lune, by Claude Debussy.", err, &st);
  if (pcm.empty()) {
    ESP_LOGW(TAG, "preview TTS failed: %s", err.c_str());
    led_tts_fail(st == 200 ? -4 : st);
    std::string o = "{\"ok\":false,\"message\":";
    jappend_str(o, err);
    return o + "}";
  }
  if (!speak_checked(pcm, 24000)) {
    led_tts_fail(-5);
    return "{\"ok\":false,\"message\":\"Not enough free memory to play it.\"}";
  }
  ESP_LOGI(TAG, "preview played (%.1f s audio)", pcm.size() / 24000.0);
  status_led_fx(LedFx::FadeWhite);
  return "{\"ok\":true}";
}

std::string notify_voices_json(const std::string& key) {
  return on_net_thread([key] { return voices_json(key); });
}
std::string notify_preview_json(const std::string& key, const std::string& voice, const std::string& style) {
  return on_net_thread([=] { return preview_json(key, voice, style); });
}
