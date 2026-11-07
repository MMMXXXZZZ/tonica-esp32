#include "auto_update.h"
#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_random.h"
#include "esp_ghota.h"
#include "mbedtls/pk.h"
#include "mbedtls/sha256.h"
#include "event_log.h"
#include "listenica/platform.h"
#include "takeover.h"

using namespace listenica;
static const char* TAG = "autoupd";

extern const char ota_pubkey_start[] asm("_binary_ota_pubkey_pem_start");
extern const char ota_pubkey_end[] asm("_binary_ota_pubkey_pem_end");  // EMBED_TXTFILES: ends in a NUL

namespace {
constexpr const char* kFirmwareGlob = "tonica-*-tonica_esp32.bin";
constexpr const char* kSignatureGlob = "tonica-*-tonica_esp32.bin.sig";
constexpr uint32_t kFirstCheckMs = 5 * 60 * 1000;    // after boot: past ota_guard's 3-minute window
constexpr uint32_t kDailyMs = 24 * 3600 * 1000;
constexpr uint32_t kBusyRetryMs = 30 * 60 * 1000;    // not idle: try again soon
constexpr uint32_t kFailRetryMs = 6 * 3600 * 1000;   // GitHub unreachable, bad file: not in a tight loop
constexpr size_t kMaxSignature = 160;                // DER ECDSA P-256 is 70-72 bytes

AutoUpdateHooks s_hooks;
std::atomic<bool> s_running{false};
ghota_client_handle_t* s_gh = nullptr;  // only touched by the one running worker

enum class Result { UpToDate, Available, Installed, Busy, Failed };
struct Job {
  bool install;
  bool console;
  Result result = Result::Failed;
  std::string message;
};

bool is_dev_build() { return strstr(esp_app_get_description()->version, "-dev") != nullptr; }

std::string version_str(const semver_t& v) {
  char b[48];
  snprintf(b, sizeof b, "%d.%d.%d", v.major, v.minor, v.patch);
  return b;
}

// GitHub's asset API redirects (302) to its file host with a long signed URL, which the client must
// send as the next request line: with the default 512-byte buffers it fails with "Out of buffer"
// (seen on the board, 2026-11-07). Both buffers are PSRAM-backed heap here and freed after.
constexpr int kHttpBuf = 4096;

esp_err_t accept_binary(esp_http_client_handle_t c) {  // GitHub's asset API: the file, not its JSON
  return esp_http_client_set_header(c, "Accept", "application/octet-stream");
}

struct SigBuf {
  uint8_t data[kMaxSignature];
  size_t len = 0;
  bool overflow = false;
};

esp_err_t sig_event(esp_http_client_event_t* e) {
  if (e->event_id != HTTP_EVENT_ON_DATA || esp_http_client_get_status_code(e->client) != 200) return ESP_OK;
  auto* b = static_cast<SigBuf*>(e->user_data);  // the redirect's own body is skipped (status 302)
  if (b->len + size_t(e->data_len) > sizeof b->data) {
    b->overflow = true;
    return ESP_OK;
  }
  memcpy(b->data + b->len, e->data, size_t(e->data_len));
  b->len += size_t(e->data_len);
  return ESP_OK;
}

bool fetch_signature(const char* url, SigBuf& out, std::string& why) {
  esp_http_client_config_t hc = {};
  hc.url = url;
  hc.crt_bundle_attach = esp_crt_bundle_attach;
  hc.event_handler = sig_event;
  hc.user_data = &out;
  hc.timeout_ms = 15000;
  hc.buffer_size = kHttpBuf;
  hc.buffer_size_tx = kHttpBuf;
  esp_http_client_handle_t c = esp_http_client_init(&hc);
  if (!c) return why = "no memory for the signature request", false;
  accept_binary(c);
  esp_err_t e = esp_http_client_perform(c);  // follows GitHub's redirect to its file host
  int status = esp_http_client_get_status_code(c);
  esp_http_client_cleanup(c);
  if (e != ESP_OK || status != 200) return why = "signature download failed (" + std::string(esp_err_to_name(e)) + ", HTTP " + std::to_string(status) + ")", false;
  if (out.overflow || out.len < 8) return why = "the signature file has an unexpected size", false;
  return true;
}

// SHA-256 of the first `len` bytes written to the update slot, then the signature check.
bool signature_valid(const esp_partition_t* part, size_t len, const SigBuf& sig, std::string& why) {
  uint8_t* buf = static_cast<uint8_t*>(heap_caps_malloc(4096, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  if (!buf) return why = "no memory to check the signature", false;
  mbedtls_sha256_context sha;
  mbedtls_sha256_init(&sha);
  mbedtls_sha256_starts(&sha, 0);
  bool read_ok = true;
  for (size_t off = 0; off < len && read_ok;) {
    size_t n = len - off < 4096 ? len - off : 4096;
    read_ok = esp_partition_read(part, off, buf, n) == ESP_OK;
    if (read_ok) mbedtls_sha256_update(&sha, buf, n);
    off += n;
  }
  uint8_t hash[32];
  mbedtls_sha256_finish(&sha, hash);
  mbedtls_sha256_free(&sha);
  heap_caps_free(buf);
  if (!read_ok) return why = "couldn't read the downloaded image back", false;

  mbedtls_pk_context pk;
  mbedtls_pk_init(&pk);
  int r = mbedtls_pk_parse_public_key(&pk, reinterpret_cast<const unsigned char*>(ota_pubkey_start),
                                      size_t(ota_pubkey_end - ota_pubkey_start));
  if (r == 0) r = mbedtls_pk_verify(&pk, MBEDTLS_MD_SHA256, hash, sizeof hash, sig.data, sig.len);
  mbedtls_pk_free(&pk);
  if (r != 0) {
    char b[64];
    snprintf(b, sizeof b, "signature check failed (mbedtls -0x%04x)", unsigned(-r));
    return why = b, false;
  }
  return true;
}

void run(Job& job) {
  const esp_partition_t* running = esp_ota_get_running_partition();
  esp_ota_img_states_t st;
  if (esp_ota_get_state_partition(running, &st) == ESP_OK && st == ESP_OTA_IMG_PENDING_VERIFY) {
    job = {job.install, job.console, Result::Busy, "this image isn't confirmed yet (just updated)"};
    return;
  }
  if (!s_gh) {
    ghota_config_t c = {};
    strncpy(c.filenamematch, kFirmwareGlob, sizeof c.filenamematch - 1);
    strncpy(c.signaturenamematch, kSignatureGlob, sizeof c.signaturenamematch - 1);
    // owner/repo: CONFIG_GITHUB_OWNER / CONFIG_GITHUB_REPO (sdkconfig.defaults)
    s_gh = ghota_init(&c);
    if (!s_gh) return void(job.message = "couldn't start the GitHub client (version not parseable?)");
  }
  if (ghota_check(s_gh) != ESP_OK) return void(job.message = "no release with a firmware file found, or GitHub unreachable");
  semver_t* cur = ghota_get_current_version(s_gh);
  semver_t* latest = ghota_get_latest_version(s_gh);
  // shallow copies: free() only (semver_free would free strings the handle still owns)
  const bool newer = cur && latest && semver_gt(*latest, *cur) == 1;
  const std::string have = cur ? version_str(*cur) : "?", want = latest ? version_str(*latest) : "?";
  free(cur);
  free(latest);
  if (!newer) {
    job.result = Result::UpToDate;
    job.message = "up to date (" + std::string(esp_app_get_description()->version) + "; latest release " + want + ")";
    return;
  }
  // A release that already failed its self-test here and rolled back is not tried again (it would
  // download, roll back and come back every day); a newer release still is.
  if (const esp_partition_t* bad = esp_ota_get_last_invalid_partition()) {
    esp_app_desc_t d;
    if (esp_ota_get_partition_description(bad, &d) == ESP_OK && want == d.version) {
      job.result = Result::UpToDate;
      job.message = want + " rolled back here before: skipped until a newer release";
      return;
    }
  }
  const char *fw_name, *fw_url, *sig_name, *sig_url;
  if (ghota_get_assets(s_gh, &fw_name, &fw_url, &sig_name, &sig_url) != ESP_OK)
    return void(job.message = "release " + want + " lists no firmware file");
  if (!job.install) {
    job.result = Result::Available;
    job.message = want + " is available (" + fw_name + (sig_name[0] ? ", signed" : ", NOT signed") + ")";
    return;
  }
  if (std::string(sig_name) != std::string(fw_name) + ".sig")
    return void(job.message = "release " + want + " has no signature for " + fw_name + ": not installed");
  if (s_hooks.idle && !s_hooks.idle()) {
    job.result = Result::Busy;
    job.message = "busy (playing or grouped): " + want + " postponed";
    return;
  }
  SigBuf sig;
  std::string why;
  if (!fetch_signature(sig_url, sig, why)) return void(job.message = why);

  ESP_LOGI(TAG, "installing %s (from %s)", want.c_str(), have.c_str());
  event_log("auto-update: downloading %s", want.c_str());
  esp_http_client_config_t hc = {};
  hc.url = fw_url;
  hc.crt_bundle_attach = esp_crt_bundle_attach;
  hc.keep_alive_enable = true;
  hc.timeout_ms = 20000;
  hc.buffer_size = kHttpBuf;
  hc.buffer_size_tx = kHttpBuf;
  esp_https_ota_config_t oc = {};
  oc.http_config = &hc;
  oc.http_client_init_cb = accept_binary;
  esp_https_ota_handle_t ota = nullptr;
  esp_err_t e = esp_https_ota_begin(&oc, &ota);
  if (e != ESP_OK) return void(job.message = "download didn't start (" + std::string(esp_err_to_name(e)) + ")");
  {  // the image header (first bytes) before the rest: this project, and the version the tag names
    esp_app_desc_t d;
    e = esp_https_ota_get_img_desc(ota, &d);
    const esp_app_desc_t* me = esp_app_get_description();
    if (e != ESP_OK || strncmp(d.project_name, me->project_name, sizeof d.project_name) != 0 || want != d.version) {
      esp_https_ota_abort(ota);
      std::string got = e == ESP_OK ? std::string(d.project_name) + " " + d.version : esp_err_to_name(e);
      event_log("auto-update: %s rejected: image header says %s", want.c_str(), got.c_str());
      return void(job.message = want + " rejected: the image header says " + got);
    }
  }
  const esp_partition_t* slot = esp_ota_get_next_update_partition(nullptr);
  while ((e = esp_https_ota_perform(ota)) == ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
    if (s_hooks.idle && !s_hooks.idle()) {  // music started: the flash writes would freeze it
      esp_https_ota_abort(ota);
      job.result = Result::Busy;
      job.message = "playback started: " + want + " postponed";
      return;
    }
  }
  if (e != ESP_OK || !esp_https_ota_is_complete_data_received(ota)) {
    esp_https_ota_abort(ota);
    return void(job.message = "download failed (" + std::string(esp_err_to_name(e)) + ")");
  }
  size_t len = size_t(esp_https_ota_get_image_len_read(ota));
  if (!slot || !signature_valid(slot, len, sig, why)) {
    esp_https_ota_abort(ota);  // the boot partition never changed
    event_log("auto-update: %s rejected: %s", want.c_str(), why.c_str());
    return void(job.message = want + " rejected: " + why);
  }
  e = esp_https_ota_finish(ota);  // ESP-IDF's own image check, then the boot partition switch
  if (e != ESP_OK) return void(job.message = "image check failed (" + std::string(esp_err_to_name(e)) + ")");
  job.result = Result::Installed;
  job.message = want + " installed (signature OK); restarting";
  event_log("auto-update: %s installed (was %s)", want.c_str(), have.c_str());
  static char reason[48];
  snprintf(reason, sizeof reason, "auto-update to %s", want.c_str());
  reboot_with_reason(reason);
}

void worker(void* arg) {  // internal-RAM stack: esp_https_ota writes flash
  auto* job = static_cast<Job*>(arg);
  run(*job);
  ESP_LOGI(TAG, "%s", job->message.c_str());
  if (job->console) {
    printf("UPDATE %s\n", job->message.c_str());
    fflush(stdout);
    delete job;
  }
  s_running = false;
}

bool start_worker(Job* job) {
  if (s_running.exchange(true)) return false;
  spawn_thread("autoupd", 8192, false, 3, worker, job);
  return true;
}

void scheduler(void*) {  // PSRAM stack: only waits and starts workers, no flash I/O
  uint32_t wait = kFirstCheckMs;
  for (;;) {
    sleep_ms(wait);
    if (is_dev_build() || !s_hooks.enabled || !s_hooks.enabled()) {
      wait = kDailyMs;
      continue;
    }
    if (s_hooks.idle && !s_hooks.idle()) {
      wait = kBusyRetryMs;
      continue;
    }
    Job job{true, false, Result::Failed, {}};
    if (!start_worker(&job)) {
      wait = kBusyRetryMs;  // a console check is running
      continue;
    }
    while (s_running) sleep_ms(1000);
    wait = job.result == Result::Busy ? kBusyRetryMs
           : job.result == Result::Failed ? kFailRetryMs
           : kDailyMs + esp_random() % (3600 * 1000);  // a little spread between speakers
  }
}
}  // namespace

void auto_update_start(AutoUpdateHooks hooks) {
  s_hooks = std::move(hooks);
  ESP_LOGI(TAG, "automatic updates %s%s", s_hooks.enabled && s_hooks.enabled() ? "on" : "off",
           is_dev_build() ? " (development build: never by itself)" : "");
  spawn_thread("autoupd_sched", 3072, true, 2, scheduler, nullptr);
}

void auto_update_console(const char* arg) {
  auto* job = new Job{arg && strcmp(arg, "install") == 0, true, Result::Failed, {}};
  if (!start_worker(job)) {
    delete job;
    printf("UPDATE a check is already running\n");
    fflush(stdout);
  }
}
