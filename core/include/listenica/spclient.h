// HTTPS APIs: client-token, login5 bearer token, extended metadata,
// storage-resolve, context-resolve and connect-state.
#pragma once
#include <functional>
#include <mutex>
#include <string>
#include <vector>
#include "listenica/ap_session.h"
#include "listenica/http.h"

namespace listenica {

enum AudioFormat : int {
  OGG_VORBIS_96 = 0,
  OGG_VORBIS_160 = 1,
  OGG_VORBIS_320 = 2,
  MP3_256 = 3,
  MP3_320 = 4,
  MP3_160 = 5,
  MP3_96 = 6,
  AAC_24 = 8,
  AAC_48 = 9,
  FLAC_FLAC = 16,
};

struct AudioFileRef {
  Bytes file_id;
  int format = -1;
};

struct TrackMeta {
  Bytes gid;
  std::string uri, name, artist, album, album_uri, artist_uri, image_id;
  std::vector<std::string> artists;  // all of them, in credit order (artist = the first)
  uint32_t duration_ms = 0;
  std::vector<AudioFileRef> files;
  bool is_episode = false;
  bool ok = false;
};

// Streaming scanner for context-resolve JSON. Keeps only what playback needs (each track's
// uri/uid, next_page_url, the top-level url), so a big playlist never sits in memory as text or as
// a cJSON tree: a 3094-track playlist did, and the board aborted on a failed allocation
// (2026-11-05). Any chunking works (even one byte at a time); escapes and non-ASCII bytes inside
// strings are skipped correctly; malformed input stops the scan (error) without harm.
class ContextScanner {
 public:
  explicit ContextScanner(size_t max_tracks = 4000) : max_(max_tracks) {}
  void feed(const uint8_t* d, size_t n);
  void reset() { *this = ContextScanner(max_); }
  struct Track {
    std::string uri, uid;
  };
  std::vector<Track> tracks;
  std::string next_page_url, url;
  bool truncated = false;  // more tracks than max_tracks: the rest were dropped
  bool error = false;      // malformed JSON: scanning stopped there

 private:
  enum Role : uint8_t { Other, TracksArray, TrackObject };
  struct Frame {
    bool object;
    Role role;
  };
  enum State : uint8_t { Value, Key, Colon, AfterValue, InString, InScalar };
  size_t max_;
  std::vector<Frame> stack_;
  State st_ = Value;
  bool str_is_key_ = false, capture_ = false, esc_ = false, done_ = false;
  std::string key_, buf_;
  Track cur_;
  void begin_string();
  void end_string();
  void end_value();
  bool structural(uint8_t ch);
};

class SpClient {
 public:
  SpClient(std::string device_id, std::string spclient_hostport);

  void set_credentials(const Credentials& c);
  Credentials credentials();
  void set_country(const std::string& c) { country_ = c; }
  // Test hook: redirect all endpoints (mock backend).
  void set_endpoints(const std::string& base, const std::string& login5, const std::string& clienttoken) {
    base_url_ = base;
    login5_url_ = login5;
    clienttoken_url_ = clienttoken;
  }

  bool client_token(std::string& out);
  bool access_token(std::string& out);  // login5, cached

  bool track_metadata(const std::string& uri, TrackMeta& out);
  // Returns CDN URLs for a file id.
  bool storage_resolve(const Bytes& file_id, std::vector<std::string>& urls);
  // context-resolve for a context uri or page url, streamed through a ContextScanner.
  // enough (optional): checked as tracks arrive; true stops the download there (a partial read)
  bool context_resolve(const std::string& uri_or_path, ContextScanner& out,
                       const std::function<bool(const ContextScanner&)>& enough = nullptr);
  void close_context_connection() { ctx_.close(); }
  // Add a track to Liked Songs (never removes): POST /collection/v2/write, WriteRequest
  // (librespot collection2v2.proto), set "collection". Verified HTTP 200 on 2026-11-02.
  bool collection_add(const std::string& track_uri);
  // Continuation after a context ends: AutoplayContextRequest{context_uri=1, recent_track_uri=2*}; JSON Context back.
  bool autoplay_context(const std::string& context_uri, const std::vector<std::string>& recent, std::string& json_out);
  // PUT connect-state; returns HTTP status.
  // reply_out: the Cluster protobuf Spotify answers with (all of the account's devices)
  int put_connect_state(const std::string& connection_id, const Bytes& put_state_request, Bytes* reply_out = nullptr);
  int put_inactive(const std::string& connection_id, bool notify);
  int transfer_to_self();
  std::string username();  // the logged-in account  // from==to: take over from the currently active device

  HttpResponse request(const std::string& method, const std::string& path, const Headers& extra = {},
                       const Bytes& body = {}, bool metrics = false);

 private:
  std::string device_id_, base_url_, country_ = "US";
  std::string login5_url_ = "https://login5.spotify.com/v3/login";
  std::string clienttoken_url_ = "https://clienttoken.spotify.com/v1/clienttoken";
  std::mutex mu_;
  Credentials creds_;
  std::string ctoken_, token_;
  uint64_t ctoken_exp_ = 0, token_exp_ = 0;
  // state_: connect-state PUTs only. On the shared api_ connection they waited up to 7 s behind a
  // streamed 3094-track context fetch, so a takeover stopped Tonica late (2026-11-05).
  // ctx_: context fetches (seconds for a big playlist), opened per fetch and closed after it, so
  // they never hold up the player's metadata/storage requests on api_.
  HttpClient api_, auth_, state_, ctx_;
  HttpResponse request_on(HttpClient& c, const std::string& method, const std::string& path, const Headers& extra,
                          const Bytes& body, bool metrics);
};

std::string gid_to_base62(const Bytes& gid);
Bytes base62_to_gid(const std::string& b62);
std::string uri_kind(const std::string& uri);  // "track", "episode", ...
std::string uri_id(const std::string& uri);

}  // namespace listenica
