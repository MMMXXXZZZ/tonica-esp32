// mDNS for the Listenica host on macOS (Bonjour, built in) and Linux (Avahi's libdns_sd compatibility
// library): announces `_listenica._tcp` like a Tónica does and finds the other speakers.
#include "lan_discovery.h"
#include <arpa/inet.h>
#include <dns_sd.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#include <algorithm>
#include <cstring>
#include <thread>
#include "listenica/platform.h"

static const char* TAG = "lan";
static const char* kType = "_listenica._tcp";

std::string lan_own_ip() {
  // a connected UDP socket reveals the source address the OS would use (nothing is sent)
  int s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (s < 0) return "";
  sockaddr_in to = {};
  to.sin_family = AF_INET;
  to.sin_port = htons(53);
  inet_pton(AF_INET, "1.1.1.1", &to.sin_addr);
  std::string out;
  if (connect(s, (sockaddr*)&to, sizeof to) == 0) {
    sockaddr_in me = {};
    socklen_t len = sizeof me;
    char buf[INET_ADDRSTRLEN];
    if (getsockname(s, (sockaddr*)&me, &len) == 0 && inet_ntop(AF_INET, &me.sin_addr, buf, sizeof buf)) out = buf;
  }
  close(s);
  return out;
}

// Runs a DNS-SD request's callbacks until `ms` pass (or forever with ms < 0).
static void pump(DNSServiceRef ref, int ms) {
  int fd = DNSServiceRefSockFD(ref);
  uint64_t end = listenica::now_ms() + uint64_t(ms < 0 ? 0 : ms);
  for (;;) {
    int64_t left = ms < 0 ? 1000 : int64_t(end) - int64_t(listenica::now_ms());
    if (left <= 0) return;
    fd_set rd;
    FD_ZERO(&rd);
    FD_SET(fd, &rd);
    timeval tv = {long(left / 1000), int((left % 1000) * 1000)};
    int r = select(fd + 1, &rd, nullptr, nullptr, &tv);
    if (r < 0) return;
    if (r > 0 && DNSServiceProcessResult(ref) != kDNSServiceErr_NoError) return;
  }
}

// ---------------------------------------------------------------- announce
static void DNSSD_API on_registered(DNSServiceRef, DNSServiceFlags, DNSServiceErrorType err, const char*, const char*,
                                    const char*, void*) {
  if (err == kDNSServiceErr_NoError) SLOGI(TAG, "announced on the network (mDNS _listenica._tcp)");
  else SLOGW(TAG, "mDNS announcement failed (%d)", int(err));
}

bool lan_announce(const std::string& instance_name, uint16_t port) {
  TXTRecordRef txt;
  TXTRecordCreate(&txt, 0, nullptr);
  TXTRecordSetValue(&txt, "role", 4, "host");
  DNSServiceRef ref = nullptr;  // kept for the life of the process: the announcement lasts as long
  DNSServiceErrorType e = DNSServiceRegister(&ref, 0, 0, instance_name.c_str(), kType, nullptr, nullptr, htons(port),
                                             TXTRecordGetLength(&txt), TXTRecordGetBytesPtr(&txt), on_registered, nullptr);
  TXTRecordDeallocate(&txt);
  if (e != kDNSServiceErr_NoError) {
    SLOGW(TAG, "DNSServiceRegister failed (%d)%s", int(e),
#ifdef __linux__
          " - is avahi-daemon running?"
#else
          ""
#endif
    );
    return false;
  }
  std::thread([ref] { pump(ref, -1); }).detach();  // answers queries and conflicts
  return true;
}

// ---------------------------------------------------------------- discover
struct Found {
  std::string name, type, domain;
  uint32_t iface;
};

static void DNSSD_API on_browse(DNSServiceRef, DNSServiceFlags flags, uint32_t iface, DNSServiceErrorType err,
                                const char* name, const char* type, const char* domain, void* ctx) {
  if (err != kDNSServiceErr_NoError || !(flags & kDNSServiceFlagsAdd)) return;
  static_cast<std::vector<Found>*>(ctx)->push_back({name, type, domain, iface});
}

static void DNSSD_API on_resolve(DNSServiceRef, DNSServiceFlags, uint32_t, DNSServiceErrorType err, const char*,
                                 const char* host, uint16_t, uint16_t, const unsigned char*, void* ctx) {
  if (err == kDNSServiceErr_NoError && host) *static_cast<std::string*>(ctx) = host;
}

static void add_ipv4_of(const std::string& host, std::vector<std::string>& out) {
  addrinfo hints = {}, *res = nullptr;
  hints.ai_family = AF_INET;
  if (getaddrinfo(host.c_str(), nullptr, &hints, &res) != 0) return;  // .local names: mDNS via the OS resolver
  for (addrinfo* a = res; a; a = a->ai_next) {
    char buf[INET_ADDRSTRLEN];
    if (inet_ntop(AF_INET, &((sockaddr_in*)a->ai_addr)->sin_addr, buf, sizeof buf) &&
        std::find(out.begin(), out.end(), buf) == out.end())
      out.push_back(buf);
  }
  freeaddrinfo(res);
}

std::vector<std::string> lan_discover(uint32_t timeout_ms) {
  std::vector<Found> found;
  DNSServiceRef browse = nullptr;
  if (DNSServiceBrowse(&browse, 0, 0, kType, nullptr, on_browse, &found) != kDNSServiceErr_NoError) {
    SLOGW(TAG, "mDNS browse failed");
    return {};
  }
  pump(browse, int(timeout_ms));
  DNSServiceRefDeallocate(browse);
  std::vector<std::string> ips;
  for (auto& f : found) {
    std::string host;
    DNSServiceRef r = nullptr;
    if (DNSServiceResolve(&r, 0, f.iface, f.name.c_str(), f.type.c_str(), f.domain.c_str(), on_resolve, &host) !=
        kDNSServiceErr_NoError)
      continue;
    pump(r, 1500);
    DNSServiceRefDeallocate(r);
    if (!host.empty()) add_ipv4_of(host, ips);
  }
  std::string me = lan_own_ip();
  ips.erase(std::remove(ips.begin(), ips.end(), me), ips.end());
  return ips;
}
