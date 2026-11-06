#include "desktop.h"
#include <cstdlib>
#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#else
#include <spawn.h>
#include <sys/wait.h>
extern char** environ;
#endif

std::string app_data_dir() {
#ifdef _WIN32
  const char* base = getenv("LOCALAPPDATA");
  return std::string(base && *base ? base : ".") + "/Listenica";
#elif defined(__APPLE__)
  const char* home = getenv("HOME");
  return std::string(home && *home ? home : ".") + "/Library/Application Support/Listenica";
#else
  const char* xdg = getenv("XDG_DATA_HOME");
  if (xdg && *xdg) return std::string(xdg) + "/listenica";
  const char* home = getenv("HOME");
  return std::string(home && *home ? home : ".") + "/.local/share/listenica";
#endif
}

void open_url(const std::string& url) {
#ifdef _WIN32
  ShellExecuteA(nullptr, "open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#else
  // no shell: the URL is passed as one argument
#ifdef __APPLE__
  const char* tool = "open";
#else
  const char* tool = "xdg-open";
#endif
  char* argv[] = {const_cast<char*>(tool), const_cast<char*>(url.c_str()), nullptr};
  pid_t pid;
  if (posix_spawnp(&pid, tool, nullptr, nullptr, argv, environ) == 0) {
    int st;
    waitpid(pid, &st, 0);
  }
#endif
}
