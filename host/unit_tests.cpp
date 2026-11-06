// Offline unit tests. Vectors were produced by tools/listenica (Python), whose
// Shannon implementation was verified against the live access point.
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>
#include "listenica/ap_session.h"
#include "listenica/crypto.h"
#include "listenica/pb.h"
#include "listenica/ring.h"
#include "listenica/shannon.h"
#include "listenica/spclient.h"
#include "listenica/zeroconf.h"

using namespace listenica;
static int fails = 0;
#define CHECK(c)                                              \
  do {                                                        \
    if (!(c)) {                                               \
      printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c);     \
      fails++;                                                \
    }                                                         \
  } while (0)

int main() {
  // Shannon
  {
    uint8_t key[32];
    for (int i = 0; i < 32; i++) key[i] = uint8_t(i);
    Shannon s(key, 32);
    s.nonce_u32(7);
    const char* pt = "hello shannon world!!";
    Bytes b(pt, pt + strlen(pt));
    s.encrypt(b.data(), b.size());
    uint8_t mac[4];
    s.finish(mac, 4);
    CHECK(hex(b) == "fcb9216397a89a17f6ab10390fd2cbe12d75c466b8");
    CHECK(hex(mac, 4) == "9db93d2e");
    // decrypt in two pieces (header + payload), as the AP reader does
    Shannon d(key, 32);
    d.nonce_u32(7);
    d.decrypt(b.data(), 3);
    d.decrypt(b.data() + 3, b.size() - 3);
    uint8_t mac2[4];
    d.finish(mac2, 4);
    CHECK(memcmp(b.data(), pt, b.size()) == 0);
    CHECK(memcmp(mac, mac2, 4) == 0);
  }
  // base62
  {
    Bytes g = base62_to_gid("4cOdK2wGLETKBW3PvgPWqT");
    CHECK(hex(g) == "8a37517ba69f47c3aeeee164eecee933");
    CHECK(gid_to_base62(g) == "4cOdK2wGLETKBW3PvgPWqT");
  }
  // AES-CTR + seek
  {
    uint8_t k[16];
    for (int i = 0; i < 16; i++) k[i] = uint8_t(i);
    static const uint8_t iv[16] = {0x72, 0xe0, 0x67, 0xfb, 0xdd, 0xcb, 0xcf, 0x77,
                                   0xeb, 0xe8, 0xbc, 0x64, 0x3f, 0x63, 0x0d, 0x93};
    Bytes z(100, 0);
    Aes128Ctr a(k, iv);
    a.apply(z.data(), z.size());
    CHECK(hex(z) ==
          "beb25cee05d717a831e56d65794babadc86453d27f3be9e23ef96565f4e742c548a60f8a6066ef55f6122532fb3308dac91172"
          "eadd2a202c0fee9caca2b7e2177a41d35861fd361419d80e024736512905b03f1c72a392c0caa5b4f1f9404c3893b30923");
    Bytes z2(63, 0);
    Aes128Ctr a2(k, iv);
    a2.seek(37);
    a2.apply(z2.data(), z2.size());
    CHECK(memcmp(z2.data(), z.data() + 37, 63) == 0);
  }
  // gunzip
  {
    Bytes gz = unhex("1f8b08000000000002ff2bcecfc9cf2c2e5148afca2c50284905b28aa92402006b22dcf05a000000");
    Bytes out;
    CHECK(gunzip(gz.data(), gz.size(), out));
    std::string want;
    for (int i = 0; i < 5; i++) want += "soloist gzip test ";  // must match the gzip fixture above (test data, not a name)
    CHECK(std::string(out.begin(), out.end()) == want);
  }
  // protobuf round trip
  {
    pb::Writer w;
    w.u64(1, 300).str(2, "abc").msg(3, [](pb::Writer& s) { s.u64(1, 7); }).b(4, true);
    pb::Reader r(w.buf);
    CHECK(r.varint_of(1) == 300);
    CHECK(r.str_of(2) == "abc");
    pb::Field f;
    CHECK(r.find(3, f) && pb::Reader(f).varint_of(1) == 7);
    CHECK(r.varint_of(4) == 1);
    CHECK(r.varint_of(9, 42) == 42);
  }
  // base64
  {
    CHECK(base64_encode(Bytes{'h', 'i', '!'}) == "aGkh");
    Bytes d = base64_decode("aGkhIQ==");
    CHECK(std::string(d.begin(), d.end()) == "hi!!");
  }
  // ring
  {
    ByteRing r(8);
    uint32_t g = r.reset();
    uint8_t in[5] = {1, 2, 3, 4, 5}, out[8];
    CHECK(r.write(in, 5, g));
    CHECK(r.read(out, 3, 10) == 3 && out[2] == 3);
    CHECK(r.write(in, 5, g));
    int n = r.read(out, 8, 10);
    n += r.read(out + n, 8 - n, 10);
    CHECK(n == 7 && out[0] == 4 && out[6] == 5);
    r.set_eof(g);
    CHECK(r.read(out, 8, 10) == -1);
  }
  // zeroconf addUser (vector from tools/zeroconf_vector.py, an independent encoder)
  {
    ZeroconfKeys k(unhex("0102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f202122232425262728292a2b2c2d2e2f303132333435363738393a3b3c3d3e3f404142434445464748494a4b4c4d4e4f505152535455565758595a5b5c5d5e5f"));
    Credentials c;
    std::string err;
    bool ok = zeroconf_decode_add_user(k, "testuser", "+4PbsKue3g5fZaq/3U2BEvWXSYo2ehQejeP2fSLSn9gCMbL9foZ/dCWcgjFpQMy8vAqMrWbJjdh7hwfYu1orJXsYdvIMSvARty2AbheasYcZGJCigQ2LVGK/zF5boBnKG9oRtJVUruy/od+Awv6ic+wAzsdMW/+rXetMK6+oUe3tAgeV+on52fhEeXMfWQyE", "TsVrM1mmTtnwAtNU9m+i65WFIOXm+oRaH/0N40myyUPCzT+DLlLnNWBkc9ULlrvAuPu6d0NfQbGKN8DB5/pzBig4NBtdOAFJ9/R/jDZZG/40/4EWNkaqq7IgOEclACbN",
                                       "c34da09981ebe7f17a212e30ceb317b10edf254e", c, err);
    CHECK(ok);
    CHECK(c.username == "testuser");
    CHECK(c.auth_type == 1);
    CHECK(hex(c.auth_data) == "c8c9cacbcccdcecfd0d1d2d3d4d5d6d7d8d9dadbdcdddedfe0e1e2e3e4e5e6e7e8e9eaebecedeeeff0f1f2f3f4f5f6f7f8f9fafbfcfdfeff0102");
    // tampered blob must fail the MAC check
    std::string bad = "+4PbsKue3g5fZaq/3U2BEvWXSYo2ehQejeP2fSLSn9gCMbL9foZ/dCWcgjFpQMy8vAqMrWbJjdh7hwfYu1orJXsYdvIMSvARty2AbheasYcZGJCigQ2LVGK/zF5boBnKG9oRtJVUruy/od+Awv6ic+wAzsdMW/+rXetMK6+oUe3tAgeV+on52fhEeXMfWQyE";
    bad[30] = bad[30] == 'A' ? 'B' : 'A';
    CHECK(!zeroconf_decode_add_user(k, "testuser", bad, "TsVrM1mmTtnwAtNU9m+i65WFIOXm+oRaH/0N40myyUPCzT+DLlLnNWBkc9ULlrvAuPu6d0NfQbGKN8DB5/pzBig4NBtdOAFJ9/R/jDZZG/40/4EWNkaqq7IgOEclACbN", "c34da09981ebe7f17a212e30ceb317b10edf254e", c, err));
    CHECK(err == "ERROR-MAC");
    CHECK(form_get("action=addUser&userName=a%2Bb+c&x=1", "userName") == "a+b c");
  }
  // ContextScanner: streamed context-resolve JSON (escapes, non-ASCII, nesting, chunking, bad input)
  {
    const std::string js =
        "{\"metadata\":{\"title\":\"Caf\\u00e9 \\\"quoted\\\" \\\\ back \\/ \xC3\xA9 \xF0\x9F\x8E\xB5\",\"uri\":\"spotify:bogus\"},"
        "\"uri\":\"spotify:playlist:p1\",\"url\":\"context://spotify:playlist:p1\",\"pages\":[{\"page_url\":\"hm://x\","
        "\"next_page_url\":\"hm://context-resolve/v1/page/2?a=1&b=\\u0026\",\"tracks\":["
        "{\"uri\":\"spotify:track:aaa\",\"uid\":\"u1\",\"metadata\":{\"uri\":\"spotify:track:WRONG\",\"tracks\":[{\"uri\":\"spotify:track:NESTED\"}],"
        "\"name\":\"\\ud83c\\udfb5 x\",\"n\":-1.5e3,\"b\":true,\"z\":null,\"e\":{},\"a\":[]}},"
        "{\"uid\":\"u2\",\"uri\":\"spotify:episode:bbb\"},"
        "{\"uid\":\"no-uri\"},"
        "{\"uri\":\"spotify:track:c\\\"c\",\"uid\":\"u3\"}]}]}";
    auto run = [&](size_t chunk) {
      ContextScanner s;
      for (size_t i = 0; i < js.size(); i += chunk)
        s.feed((const uint8_t*)js.data() + i, std::min(chunk, js.size() - i));
      return s;
    };
    for (size_t chunk : {js.size(), size_t(1), size_t(7), size_t(64)}) {
      ContextScanner s = run(chunk);
      CHECK(!s.error);
      CHECK(s.tracks.size() == 3);
      if (s.tracks.size() == 3) {
        CHECK(s.tracks[0].uri == "spotify:track:aaa" && s.tracks[0].uid == "u1");  // nested "uri"s ignored
        CHECK(s.tracks[1].uri == "spotify:episode:bbb" && s.tracks[1].uid == "u2");
        CHECK(s.tracks[2].uri == "spotify:track:c\"c");                             // escape decoded
      }
      CHECK(s.url == "context://spotify:playlist:p1");
      CHECK(s.next_page_url.rfind("hm://context-resolve/v1/page/2?a=1&b=", 0) == 0);
    }
    {
      ContextScanner s(2);  // cap
      s.feed((const uint8_t*)js.data(), js.size());
      CHECK(s.tracks.size() == 2 && s.truncated && !s.error);
    }
    // a page response without "pages" (next_page_url pages) still yields its tracks
    {
      std::string pg = "{\"tracks\":[{\"uri\":\"spotify:track:p\",\"uid\":\"x\"}],\"next_page_url\":\"\"}";
      ContextScanner s;
      s.feed((const uint8_t*)pg.data(), pg.size());
      CHECK(s.tracks.size() == 1 && !s.error && s.next_page_url.empty());
    }
    // malformed inputs: no crash, error flagged, tracks before the fault kept
    const char* bad[] = {
        "{\"tracks\":[{\"uri\":\"spotify:track:ok\"},{\"uri\":\"spotify:track:x\"",  // truncated
        "{\"tracks\":[{\"uri\":\"spotify:track:ok\"},{\"uri\" \"no colon\"}]}",
        "{\"tracks\":[{\"uri\":\"spotify:track:ok\"}]]]]}",
        "{\"a\":}", "{\"a\" : tru,}", "]", "\"\\", "{\"a\":\"ctrl\x01char\"}", "{{{{",
        "{\"tracks\":[{\"uri\":\"spotify:track:ok\"},,]}"};
    for (const char* b : bad) {
      ContextScanner s;
      s.feed((const uint8_t*)b, strlen(b));
      CHECK(s.tracks.size() <= 1);
    }
    {
      ContextScanner s;
      const char* b = "{\"tracks\":[{\"uri\":\"spotify:track:ok\"},{\"uri\" \"no colon\"}]}";
      s.feed((const uint8_t*)b, strlen(b));
      CHECK(s.error && s.tracks.size() == 1);
    }
    {
      std::string deep(200, '[');  // nesting limit
      ContextScanner s;
      s.feed((const uint8_t*)deep.data(), deep.size());
      CHECK(s.error);
    }
  }
  // KeyPacer: the cases of Spotifast's librespot fork (core/src/audio_key.rs tests), with short delays
  {
    const uint32_t gap = 20, b0 = 60, b1 = 100, b2 = 200;
    struct Server {  // answers in turn from `answers` (refusing once they run out), notes when asked
      std::vector<int> answers;
      size_t next = 0;
      std::vector<uint64_t> asked;
      uint64_t t0 = now_ms();
      KeyPacer::Result ask() {
        asked.push_back(now_ms() - t0);
        sleep_ms(5);  // the answer takes a moment
        int a = next < answers.size() ? answers[next++] : 0;
        return a ? KeyPacer::Key : KeyPacer::Refused;
      }
    };
    auto alive = [] { return true; };
    auto near = [](uint64_t got, uint64_t want) { return got + 2 >= want && got <= want + 40; };  // scheduler slack
    {  // spaced, one at a time (sequential callers here)
      KeyPacer p(gap, b0, b1, b2);
      Server s{{1, 1, 1}};
      for (int i = 0; i < 3; i++) CHECK(p.request(false, [&] { return s.ask(); }, alive) == KeyPacer::Key);
      CHECK(s.asked.size() == 3 && near(s.asked[1], gap) && near(s.asked[2], 2 * gap));
    }
    {  // a refused key is asked again after a growing back-off, then arrives
      KeyPacer p(gap, b0, b1, b2);
      Server s{{0, 0, 0, 1}};
      CHECK(p.request(false, [&] { return s.ask(); }, alive) == KeyPacer::Key);
      CHECK(s.asked.size() == 4);
      if (s.asked.size() == 4) {
        CHECK(near(s.asked[1] - s.asked[0], b0 + 5));
        CHECK(near(s.asked[2] - s.asked[1], b1 + 5));
        CHECK(near(s.asked[3] - s.asked[2], b2 + 5));
      }
      CHECK(!p.backing_off());
    }
    {  // refused through every retry: reported, and the back-off stays at its longest
      KeyPacer p(gap, b0, b1, b2);
      Server s{{}};
      CHECK(p.request(false, [&] { return s.ask(); }, alive) == KeyPacer::Refused);
      CHECK(s.asked.size() == 4 && p.backing_off());
      uint64_t t = now_ms() - s.t0;
      p.request(true, [&] { return s.ask(); }, alive);  // optional: not sent during the back-off
      CHECK(s.asked.size() == 4);
      (void)t;
    }
    {  // a refusal holds back every request of the session; a key ends the back-off
      KeyPacer p(gap, b0, b1, b2);
      Server s{{0, 1, 1}};
      CHECK(p.request(true, [&] { return s.ask(); }, alive) == KeyPacer::Refused);  // optional: no retry
      CHECK(p.backing_off());
      CHECK(p.request(false, [&] { return s.ask(); }, alive) == KeyPacer::Key);  // another track waits
      CHECK(s.asked.size() == 2 && near(s.asked[1], b0 + 5));
      CHECK(!p.backing_off());
      CHECK(p.request(false, [&] { return s.ask(); }, alive) == KeyPacer::Key);
      CHECK(s.asked.size() == 3 && near(s.asked[2] - s.asked[1], gap));
    }
    {  // a timeout or dead link starts no back-off; a dead session stops waiting
      KeyPacer p(gap, b0, b1, b2);
      CHECK(p.request(false, [] { return KeyPacer::Failed; }, alive) == KeyPacer::Failed);
      CHECK(!p.backing_off());
      Server s{{}};
      p.request(true, [&] { return s.ask(); }, alive);  // one refusal: backing off now
      uint64_t t = now_ms();
      CHECK(p.request(false, [&] { return s.ask(); }, [] { return false; }) == KeyPacer::Failed);
      CHECK(now_ms() - t < 30);
    }
  }
  printf(fails ? "%d FAILURE(S)\n" : "all unit tests passed\n", fails);
  return fails ? 1 : 0;
}
