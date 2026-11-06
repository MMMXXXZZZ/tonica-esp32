#pragma once
// Persistent event log: the last 48 notable events (boots and their reasons,
// takeovers, watchdog actions, OTA, setup saves, session drops), kept across
// reboots and power cycles so overnight problems can be diagnosed.
//
// Flash writes stall the audio path, so events are stored to NVS only while
// nothing plays (event_log_flush_if_idle) and before any deliberate reboot.
// GET /events shows them, oldest first.
#include <string>

void event_log_init();                                  // after nvs_flash_init: load the saved events
void event_log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void event_log_flush_if_idle(bool playing);             // main loop
void event_log_flush();                                 // now (before a reboot)
std::string event_log_text();
