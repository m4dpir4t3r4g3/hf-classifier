// What the program needs from the operating system to run by itself:
// finding SDRconnect, telling whether it is running, starting the headless
// SDRconnect and stopping it again, opening the program's window, and the
// user's language. Windows is the target; on other systems these do what
// makes sense there (no SDRconnect to find; a browser to open).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace hfc {

struct SdrconnectInstall {
  std::string folder;    // where SDRconnect is installed ("" if not found)
  std::string headless;  // SDRconnect_headless.exe there, if it exists
};
// Looks where SDRconnect's installer puts it (Program Files, folder
// SDRplay\SDRconnect) and in Windows' list of installed programs.
// `headlessPath`, if given and existing, wins.
SdrconnectInstall findSdrconnect(const std::string& headlessPath);

// Is a program of this file name (e.g. "SDRconnect.exe") running? 1 yes,
// 0 no, -1 cannot tell (not Windows).
int processRunning(const char* exeName);

// A program this one started, stopped again by stop() or when destroyed.
class ChildProcess {
 public:
  ChildProcess() = default;
  ~ChildProcess();
  ChildProcess(const ChildProcess&) = delete;
  ChildProcess& operator=(const ChildProcess&) = delete;
  // `logPath` (optional): the program's output (stdout and stderr) goes
  // to that file, new each time.
  bool start(const std::string& exe, const std::vector<std::string>& args, bool hidden, const std::string& logPath = "");
  bool running();
  // Its exit code once it has ended (-1: still running, or never started).
  int exitCode() const { return exitCode_; }
  void stop();

 private:
#ifdef _WIN32
  void* process_ = nullptr;
#else
  long pid_ = 0;
#endif
  int exitCode_ = -1;
};

// Opens the UI at `url`: on Windows in an app window of Edge or Chrome
// (no tabs or address bar), else in the default browser.
void openWindow(const std::string& url);
// Opens a folder in the system's file manager.
void openFolder(const std::string& path);
// "bg" or "en", from the system's display language.
std::string systemLanguage();
bool isLocalHost(const std::string& host);
double nowUtc();
// The computer, for the diagnostic report.
struct SystemInfo {
  std::string os;          // e.g. "Windows 10.0 build 22631.4317 (Windows 10 Pro, 23H2)"
  std::string machine;     // processor architecture
  int cpus = 0;            // logical processors
  double memoryGb = 0;     // physical memory
  std::string executable;  // this program's file
};
SystemInfo systemInfo();
// The version in a program file's version resource ("1.0.10.0"); "" if it
// has none, or not on Windows.
std::string fileVersion(const std::string& path);

// Asks Windows to run this process at full speed, not in "efficiency mode"
// (EcoQoS): Windows 11 puts programs without a visible window there, and
// on hybrid processors (Intel 12th generation and later) that keeps them on
// the efficiency cores at a low clock. True if Windows took it (false on
// other systems and before Windows 10 1709).
bool preferFullSpeed();

// Asks a running instance of this program on 127.0.0.1:port whether it is
// one (GET /api/status, version in the answer).
bool otherInstanceAt(uint16_t port);

}  // namespace hfc
