#pragma once
// mDNS (DNS-SD) for the Listenica host on Windows 10+: announces `_listenica._tcp` like a Tónica
// does, and finds the other speakers. Uses the OS's own responder (windns.h DnsService*).
#include <string>
#include <vector>

// Announces this host for as long as the process lives. false: the OS refused it.
bool lan_announce(const std::string& instance_name, uint16_t port);
// The other speakers' IPv4 addresses (this host's own address excluded).
std::vector<std::string> lan_discover(uint32_t timeout_ms = 1500);
// This host's LAN address (the interface that routes to the internet), "" if unknown.
std::string lan_own_ip();
