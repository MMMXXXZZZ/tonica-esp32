#include "lan_discovery.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <windns.h>
#include <algorithm>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include "listenica/platform.h"

using listenica::sleep_ms;

static const char* TAG = "lan";
static const wchar_t* kService = L"_listenica._tcp.local";

static std::wstring widen(const std::string& s) {
  int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
  std::wstring w(n > 0 ? size_t(n - 1) : 0, L'\0');
  if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], n);
  return w;
}

std::string lan_own_ip() {
  // a connected UDP socket reveals the source address the OS would use (nothing is sent)
  SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (s == INVALID_SOCKET) return "";
  sockaddr_in to = {};
  to.sin_family = AF_INET;
  to.sin_port = htons(53);
  inet_pton(AF_INET, "1.1.1.1", &to.sin_addr);
  std::string out;
  if (connect(s, (sockaddr*)&to, sizeof to) == 0) {
    sockaddr_in me = {};
    int len = sizeof me;
    char buf[16];
    if (getsockname(s, (sockaddr*)&me, &len) == 0 && inet_ntop(AF_INET, &me.sin_addr, buf, sizeof buf)) out = buf;
  }
  closesocket(s);
  return out;
}

// ---------------------------------------------------------------- announce
static DNS_SERVICE_REGISTER_REQUEST s_reg;  // must outlive the registration

static VOID WINAPI on_registered(DWORD status, PVOID, PDNS_SERVICE_INSTANCE inst) {
  if (status == ERROR_SUCCESS) SLOGI(TAG, "announced on the network (mDNS _listenica._tcp)");
  else SLOGW(TAG, "mDNS announcement failed (%lu)", status);
  if (inst) DnsServiceFreeInstance(inst);
}

bool lan_announce(const std::string& instance_name, uint16_t port) {
  std::string ip = lan_own_ip();
  IP4_ADDRESS a4 = 0;
  if (ip.empty() || inet_pton(AF_INET, ip.c_str(), &a4) != 1) return false;
  char host[256] = "";
  DWORD hn = sizeof host;
  GetComputerNameExA(ComputerNameDnsHostname, host, &hn);
  std::wstring inst = widen(instance_name) + L"." + kService;
  std::wstring hostname = widen(host) + L".local";
  PCWSTR keys[] = {L"role"}, values[] = {L"host"};
  PDNS_SERVICE_INSTANCE si = DnsServiceConstructInstance(inst.c_str(), hostname.c_str(), &a4, nullptr, port, 0, 0, 1, keys, values);
  if (!si) return false;
  s_reg = {};
  s_reg.Version = DNS_QUERY_REQUEST_VERSION1;
  s_reg.InterfaceIndex = 0;
  s_reg.pServiceInstance = si;
  s_reg.pRegisterCompletionCallback = on_registered;
  DWORD r = DnsServiceRegister(&s_reg, nullptr);
  if (r != DNS_REQUEST_PENDING) {
    SLOGW(TAG, "DnsServiceRegister failed (%lu)", r);
    return false;
  }
  return true;
}

// ---------------------------------------------------------------- discover
struct Browse {
  std::mutex mu;
  std::vector<std::string> ips;
  std::vector<std::wstring> instances;
};

static void add_ip(Browse* b, IP4_ADDRESS a) {
  char buf[16];
  in_addr in;
  in.s_addr = a;
  if (!inet_ntop(AF_INET, &in, buf, sizeof buf)) return;
  std::lock_guard<std::mutex> l(b->mu);
  if (std::find(b->ips.begin(), b->ips.end(), buf) == b->ips.end()) b->ips.push_back(buf);
}

static VOID WINAPI on_browse(DWORD status, PVOID ctx, PDNS_RECORD rec) {
  Browse* b = static_cast<Browse*>(ctx);
  if (status == ERROR_SUCCESS) {
    // DnsServiceBrowse is a wide-character API: its records are DNS_RECORDW whatever UNICODE says
    for (PDNS_RECORDW r = reinterpret_cast<PDNS_RECORDW>(rec); r; r = r->pNext) {
      if (r->wType == DNS_TYPE_A) {
        add_ip(b, r->Data.A.IpAddress);
      } else if (r->wType == DNS_TYPE_PTR && r->Data.PTR.pNameHost) {
        std::lock_guard<std::mutex> l(b->mu);
        b->instances.push_back(r->Data.PTR.pNameHost);
      }
    }
  }
  if (rec) DnsRecordListFree(rec, DnsFreeRecordList);
}

struct Resolve {
  Browse* b;
  std::mutex mu;
  std::condition_variable cv;
  bool done = false;
};

static VOID WINAPI on_resolved(DWORD status, PVOID ctx, PDNS_SERVICE_INSTANCE inst) {
  Resolve* r = static_cast<Resolve*>(ctx);
  if (status == ERROR_SUCCESS && inst && inst->ip4Address) add_ip(r->b, *inst->ip4Address);
  if (inst) DnsServiceFreeInstance(inst);
  std::lock_guard<std::mutex> l(r->mu);
  r->done = true;
  r->cv.notify_all();
}

std::vector<std::string> lan_discover(uint32_t timeout_ms) {
  // never freed: a late callback after the cancel must not touch freed memory (a rare, tiny leak)
  Browse& b = *new Browse;
  DNS_SERVICE_BROWSE_REQUEST req = {};
  req.Version = DNS_QUERY_REQUEST_VERSION1;
  req.InterfaceIndex = 0;
  req.QueryName = kService;
  req.pBrowseCallback = on_browse;
  req.pQueryContext = &b;
  DNS_SERVICE_CANCEL cancel = {};
  if (DnsServiceBrowse(&req, &cancel) != DNS_REQUEST_PENDING) {
    SLOGW(TAG, "mDNS browse failed");
    return {};
  }
  sleep_ms(timeout_ms);
  DnsServiceBrowseCancel(&cancel);
  sleep_ms(50);  // a callback already running finishes
  // instances whose address didn't come with the answer: resolve each
  std::vector<std::wstring> inst;
  {
    std::lock_guard<std::mutex> l(b.mu);
    inst = b.instances;
  }
  for (auto& name : inst) {
    Resolve& r = *new Resolve;  // freed only once answered (a late callback is harmless)
    r.b = &b;
    DNS_SERVICE_RESOLVE_REQUEST rr = {};
    rr.Version = DNS_QUERY_REQUEST_VERSION1;
    rr.QueryName = &name[0];
    rr.pResolveCompletionCallback = on_resolved;
    rr.pQueryContext = &r;
    DNS_SERVICE_CANCEL rc = {};
    if (DnsServiceResolve(&rr, &rc) != DNS_REQUEST_PENDING) {
      delete &r;
      continue;
    }
    std::unique_lock<std::mutex> l(r.mu);
    if (r.cv.wait_for(l, std::chrono::milliseconds(1500), [&] { return r.done; })) {
      l.unlock();
      delete &r;
    } else {
      l.unlock();
      DnsServiceResolveCancel(&rc);
    }
  }
  std::string me = lan_own_ip();
  std::lock_guard<std::mutex> l(b.mu);
  std::vector<std::string> out;
  for (auto& ip : b.ips)
    if (ip != me) out.push_back(ip);
  return out;
}
