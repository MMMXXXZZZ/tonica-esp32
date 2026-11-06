#pragma once
// Small per-OS helpers for the Listenica host.
#include <string>

// Where the login and device id live when --secrets isn't given:
//   Windows %LOCALAPPDATA%\Listenica, macOS ~/Library/Application Support/Listenica,
//   Linux $XDG_DATA_HOME/listenica (~/.local/share/listenica).
std::string app_data_dir();
// Opens `url` in the default browser (best effort; the URL is printed too).
void open_url(const std::string& url);
