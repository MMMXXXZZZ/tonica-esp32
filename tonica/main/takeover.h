#pragma once
// Who may use the speaker, and self-recovery.
//
// Three tiers (policy in tonica/settings.conf, Kconfig TONICA_TAKEOVER_*):
//   owner           the account saved in NVS (first to pair, or the 'creds' console
//                   command). Always wins: can take the speaker from anyone.
//   temporary owner an account that claimed the speaker in the pairing window after
//                   a fresh boot (GRACE_SEC, while the owner's music isn't playing).
//                   Owner-like for its session: can take the speaker back from
//                   guests, and the policy protects it the way it protects the owner.
//   guest           anyone else, as the policy allows.
// Guest and temporary-owner logins live in RTC memory only (never flash): a power
// cycle, or any return to the owner, forgets them.
#include <string>
#include "listenica/ap_session.h"

enum class Takeover { Refuse, AcceptOwner, AcceptGuest };

struct TakeoverInput {
  std::string user;        // account asking for the speaker
  std::string owner;       // saved owner ("" = none yet)
  std::string temp_owner;  // current temporary owner ("" = none)
  bool session_guest = false;   // the board runs a non-owner session now
  bool holder_attached = false; // owner / temporary owner holding the session is connected:
                                // speaker active and playing, or their app online (owner_priority only)
  bool grace_open = false;      // pairing window open
};
struct TakeoverResult {
  Takeover action;
  bool temp_owner = false;  // AcceptGuest: the account is (or becomes) the temporary owner
};
// policy: 0 open, 1 owner priority, 2 owner only (Settings::eff_takeover)
TakeoverResult takeover_decide(const TakeoverInput& in, int policy);
const char* takeover_policy_name(int policy);

// Guest login handed to the next boot (RTC, survives a software reset only).
bool guest_boot_set(const listenica::Credentials& c);
bool guest_boot_take(listenica::Credentials& out);  // consumes it: the boot after is the owner's again

// Temporary owner (RTC): set when claimed in the pairing window, cleared on any return to the owner.
void temp_owner_set(const std::string& user);
std::string temp_owner_get();  // "" after a power cycle or once cleared
void temp_owner_clear();

// Playback was running when an OTA started: resume it after the reboot (RTC, soft reset only).
void resume_after_reboot_set(bool on);
bool resume_after_reboot_take();

// Clean restart that records why (shown at the next boot and in /status as rr=).
void reboot_with_reason(const char* why);
const char* last_reboot_reason();  // "" if the last reset was not one of ours
