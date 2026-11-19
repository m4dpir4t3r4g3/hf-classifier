#include "platform.hpp"

#include <filesystem>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <ctime>

#include "hfc/file.hpp"
#include "hfc/net.hpp"

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#include <tlhelp32.h>
#else
#include <signal.h>
#include <fcntl.h>
#include <spawn.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/utsname.h>
#include <unistd.h>
extern char** environ;
#endif

namespace hfc {

double nowUtc() {
  return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
}

bool isLocalHost(const std::string& host) {
  return host == "127.0.0.1" || host == "localhost" || host == "::1" || host == "[::1]";
}

bool otherInstanceAt(uint16_t port) {
  TcpSocket s;
  if (!s.connect("127.0.0.1", port, 1000, nullptr)) return false;
  const std::string req = "GET /api/status HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(port) + "\r\n\r\n";
  if (!s.sendAll(req.data(), req.size())) return false;
  s.setRecvTimeout(2000);
  std::string got;
  char buf[2048];
  long r;
  while (got.size() < 65536 && (r = s.recvSome(buf, sizeof(buf))) > 0) got.append(buf, static_cast<std::size_t>(r));
  return got.find("\"type\":\"status\"") != std::string::npos && got.find("\"version\"") != std::string::npos;
}

#ifdef _WIN32

namespace {

std::wstring env(const wchar_t* name) {
  wchar_t buf[MAX_PATH];
  const DWORD n = GetEnvironmentVariableW(name, buf, MAX_PATH);
  return n > 0 && n < MAX_PATH ? std::wstring(buf, n) : std::wstring();
}

std::wstring regString(HKEY root, const std::wstring& key, const wchar_t* value, REGSAM view) {
  HKEY k;
  if (RegOpenKeyExW(root, key.c_str(), 0, KEY_READ | view, &k) != ERROR_SUCCESS) return std::wstring();
  wchar_t buf[1024];
  DWORD size = sizeof(buf), type = 0;
  std::wstring out;
  if (RegQueryValueExW(k, value, nullptr, &type, reinterpret_cast<BYTE*>(buf), &size) == ERROR_SUCCESS &&
      (type == REG_SZ || type == REG_EXPAND_SZ))
    out.assign(buf, size / sizeof(wchar_t));
  RegCloseKey(k);
  while (!out.empty() && out.back() == L'\0') out.pop_back();
  return out;
}

// SDRconnect's install folder from Windows' list of installed programs.
std::wstring installedSdrconnect() {
  const std::wstring base = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall";
  for (REGSAM view : {KEY_WOW64_64KEY, KEY_WOW64_32KEY})
    for (HKEY root : {HKEY_LOCAL_MACHINE, HKEY_CURRENT_USER}) {
      HKEY k;
      if (RegOpenKeyExW(root, base.c_str(), 0, KEY_READ | view, &k) != ERROR_SUCCESS) continue;
      wchar_t name[256];
      for (DWORD i = 0;; ++i) {
        DWORD len = 256;
        if (RegEnumKeyExW(k, i, name, &len, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
        const std::wstring sub = base + L"\\" + name;
        const std::wstring display = regString(root, sub, L"DisplayName", view);
        if (display.rfind(L"SDRconnect", 0) != 0) continue;
        std::wstring loc = regString(root, sub, L"InstallLocation", view);
        if (!loc.empty()) {
          RegCloseKey(k);
          return loc;
        }
      }
      RegCloseKey(k);
    }
  return std::wstring();
}

std::wstring appPath(const wchar_t* exe) {
  const std::wstring key = std::wstring(L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\App Paths\\") + exe;
  for (HKEY root : {HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE}) {
    std::wstring p = regString(root, key, nullptr, 0);
    if (!p.empty() && GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES) return p;
  }
  return std::wstring();
}

std::wstring quote(const std::wstring& a) { return L"\"" + a + L"\""; }

}  // namespace

SdrconnectInstall findSdrconnect(const std::string& headlessPath) {
  SdrconnectInstall r;
  if (!headlessPath.empty() && fileExists(headlessPath)) {
    r.headless = headlessPath;
    const std::size_t slash = headlessPath.find_last_of("\\/");
    r.folder = slash == std::string::npos ? std::string() : headlessPath.substr(0, slash);
    return r;
  }
  std::vector<std::wstring> folders;
  for (const wchar_t* var : {L"ProgramW6432", L"ProgramFiles", L"ProgramFiles(x86)"}) {
    const std::wstring pf = env(var);
    if (!pf.empty()) folders.push_back(pf + L"\\SDRplay\\SDRconnect");
  }
  const std::wstring reg = installedSdrconnect();
  if (!reg.empty()) folders.insert(folders.begin(), reg);
  for (auto f : folders) {
    while (!f.empty() && (f.back() == L'\\' || f.back() == L'/')) f.pop_back();
    const std::string folder = narrow(f);
    const std::string headless = folder + "\\SDRconnect_headless.exe";
    if (fileExists(headless) || fileExists(folder + "\\SDRconnect.exe")) {
      r.folder = folder;
      if (fileExists(headless)) r.headless = headless;
      return r;
    }
  }
  return r;
}

int processRunning(const char* exeName) {
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snap == INVALID_HANDLE_VALUE) return -1;
  const std::wstring want = widen(exeName);
  PROCESSENTRY32W e;
  e.dwSize = sizeof(e);
  int found = 0;
  for (BOOL ok = Process32FirstW(snap, &e); ok; ok = Process32NextW(snap, &e))
    if (_wcsicmp(e.szExeFile, want.c_str()) == 0) {
      found = 1;
      break;
    }
  CloseHandle(snap);
  return found;
}

ChildProcess::~ChildProcess() { stop(); }

bool ChildProcess::start(const std::string& exe, const std::vector<std::string>& args, bool hidden, const std::string& logPath) {
  stop();
  exitCode_ = -1;
  std::wstring cmd = quote(widen(exe));
  for (const auto& a : args) cmd += L" " + quote(widen(a));
  STARTUPINFOW si{};
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi{};
  std::wstring dir = widen(exe);
  const std::size_t slash = dir.find_last_of(L"\\/");
  dir = slash == std::wstring::npos ? std::wstring() : dir.substr(0, slash);
  // Its output to the log file: an inheritable handle as its stdout and stderr.
  HANDLE log = INVALID_HANDLE_VALUE;
  if (!logPath.empty()) {
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    log = CreateFileW(widen(logPath).c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, CREATE_ALWAYS,
                      FILE_ATTRIBUTE_NORMAL, nullptr);
    if (log != INVALID_HANDLE_VALUE) {
      si.dwFlags |= STARTF_USESTDHANDLES;
      si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
      si.hStdOutput = log;
      si.hStdError = log;
    }
  }
  const BOOL ok = CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, log != INVALID_HANDLE_VALUE ? TRUE : FALSE,
                                 hidden ? CREATE_NO_WINDOW : 0, nullptr, dir.empty() ? nullptr : dir.c_str(), &si, &pi);
  if (log != INVALID_HANDLE_VALUE) CloseHandle(log);  // the child has its own copy
  if (!ok) return false;
  CloseHandle(pi.hThread);
  process_ = pi.hProcess;
  return true;
}

bool ChildProcess::running() {
  if (!process_) return false;
  if (WaitForSingleObject(static_cast<HANDLE>(process_), 0) == WAIT_TIMEOUT) return true;
  DWORD code = 0;
  if (GetExitCodeProcess(static_cast<HANDLE>(process_), &code)) exitCode_ = static_cast<int>(code);
  return false;
}

void ChildProcess::stop() {
  if (!process_) return;
  if (running()) {
    TerminateProcess(static_cast<HANDLE>(process_), 0);
    WaitForSingleObject(static_cast<HANDLE>(process_), 3000);
  }
  CloseHandle(static_cast<HANDLE>(process_));
  process_ = nullptr;
}

void openWindow(const std::string& url) {
  // An app window (no tabs, no address bar) of Edge, which every Windows 10
  // and 11 has, or of Chrome; else whatever the default browser is.
  const std::wstring u = widen(url);
  for (const wchar_t* exe : {L"msedge.exe", L"chrome.exe"}) {
    const std::wstring path = appPath(exe);
    if (path.empty()) continue;
    std::wstring cmd = quote(path) + L" --app=" + quote(u) + L" --window-size=1440,920";
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
      CloseHandle(pi.hThread);
      CloseHandle(pi.hProcess);
      return;
    }
  }
  ShellExecuteW(nullptr, L"open", u.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void openFolder(const std::string& path) {
  ShellExecuteW(nullptr, L"open", std::filesystem::u8path(path).wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

std::string systemLanguage() { return PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_BULGARIAN ? "bg" : "en"; }

namespace {
DWORD regDword(HKEY root, const std::wstring& key, const wchar_t* value) {
  HKEY k;
  if (RegOpenKeyExW(root, key.c_str(), 0, KEY_READ | KEY_WOW64_64KEY, &k) != ERROR_SUCCESS) return 0;
  DWORD v = 0, size = sizeof(v), type = 0;
  if (RegQueryValueExW(k, value, nullptr, &type, reinterpret_cast<BYTE*>(&v), &size) != ERROR_SUCCESS || type != REG_DWORD) v = 0;
  RegCloseKey(k);
  return v;
}
}  // namespace

bool preferFullSpeed() {
  // PROCESS_POWER_THROTTLING_STATE, declared here: older SDK and MinGW
  // headers lack it. SetProcessInformation is looked up, not linked, so that
  // the program still starts where it does not exist.
  struct ThrottlingState {
    ULONG version, controlMask, stateMask;
  } st{1, 0x1 /* EXECUTION_SPEED */, 0 /* off: full speed */};
  using SetInfoFn = BOOL(WINAPI*)(HANDLE, int, LPVOID, DWORD);
  HMODULE k = GetModuleHandleW(L"kernel32.dll");
  auto fn = k ? reinterpret_cast<SetInfoFn>(reinterpret_cast<void*>(GetProcAddress(k, "SetProcessInformation"))) : nullptr;
  const int kProcessPowerThrottling = 4;
  return fn && fn(GetCurrentProcess(), kProcessPowerThrottling, &st, sizeof(st));
}

SystemInfo systemInfo() {
  SystemInfo r;
  // GetVersionEx is shimmed for programs without a manifest; ntdll's
  // RtlGetVersion tells the truth.
  struct OsVersion {
    ULONG size, major, minor, build, platform;
    WCHAR csd[128];
  } v{};
  v.size = sizeof(v);
  using RtlGetVersionFn = LONG(WINAPI*)(OsVersion*);
  if (HMODULE nt = GetModuleHandleW(L"ntdll.dll")) {
    auto fn = reinterpret_cast<RtlGetVersionFn>(reinterpret_cast<void*>(GetProcAddress(nt, "RtlGetVersion")));
    if (fn) fn(&v);
  }
  const std::wstring key = L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion";
  const std::string product = narrow(regString(HKEY_LOCAL_MACHINE, key, L"ProductName", KEY_WOW64_64KEY));
  const std::string display = narrow(regString(HKEY_LOCAL_MACHINE, key, L"DisplayVersion", KEY_WOW64_64KEY));
  const DWORD ubr = regDword(HKEY_LOCAL_MACHINE, key, L"UBR");
  char b[256];
  std::snprintf(b, sizeof(b), "Windows %lu.%lu build %lu.%lu (%s%s%s)", v.major, v.minor, v.build, ubr,
                product.c_str(), display.empty() ? "" : ", ", display.c_str());
  r.os = b;
  SYSTEM_INFO si;
  GetNativeSystemInfo(&si);
  r.cpus = static_cast<int>(si.dwNumberOfProcessors);
  switch (si.wProcessorArchitecture) {
    case PROCESSOR_ARCHITECTURE_AMD64: r.machine = "x64"; break;
    case PROCESSOR_ARCHITECTURE_ARM64: r.machine = "ARM64"; break;
    case PROCESSOR_ARCHITECTURE_INTEL: r.machine = "x86"; break;
    default: r.machine = "other";
  }
  MEMORYSTATUSEX ms;
  ms.dwLength = sizeof(ms);
  if (GlobalMemoryStatusEx(&ms)) r.memoryGb = static_cast<double>(ms.ullTotalPhys) / (1024.0 * 1024 * 1024);
  wchar_t path[MAX_PATH * 2];
  const DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH * 2);
  if (n > 0 && n < MAX_PATH * 2) r.executable = narrow(std::wstring(path, n));
  return r;
}

std::string fileVersion(const std::string& path) {
  const std::wstring w = widen(path);
  DWORD handle = 0;
  const DWORD size = GetFileVersionInfoSizeW(w.c_str(), &handle);
  if (size == 0) return "";
  std::vector<char> data(size);
  if (!GetFileVersionInfoW(w.c_str(), 0, size, data.data())) return "";
  VS_FIXEDFILEINFO* fi = nullptr;
  UINT len = 0;
  if (!VerQueryValueW(data.data(), L"\\", reinterpret_cast<void**>(&fi), &len) || !fi || len < sizeof(*fi)) return "";
  char b[64];
  std::snprintf(b, sizeof(b), "%u.%u.%u.%u", static_cast<unsigned>(HIWORD(fi->dwFileVersionMS)),
                static_cast<unsigned>(LOWORD(fi->dwFileVersionMS)), static_cast<unsigned>(HIWORD(fi->dwFileVersionLS)),
                static_cast<unsigned>(LOWORD(fi->dwFileVersionLS)));
  std::string out = b;
  // SDRconnect's files say 1.0.0.0 there; the product version text may
  // say more.
  struct Translation {
    WORD language, codepage;
  }* tr = nullptr;
  if (VerQueryValueW(data.data(), L"\\VarFileInfo\\Translation", reinterpret_cast<void**>(&tr), &len) && tr &&
      len >= sizeof(Translation)) {
    wchar_t key[64];
    std::swprintf(key, 64, L"\\StringFileInfo\\%04x%04x\\ProductVersion", tr->language, tr->codepage);
    wchar_t* text = nullptr;
    if (VerQueryValueW(data.data(), key, reinterpret_cast<void**>(&text), &len) && text && len > 1) {
      const std::string product = narrow(std::wstring(text, std::wcslen(text)));
      if (!product.empty() && product != out) out += " (product " + product + ")";
    }
  }
  return out;
}

#else  // not Windows

SdrconnectInstall findSdrconnect(const std::string& headlessPath) {
  // SDRconnect for Linux and macOS exists too; its headless binary is found
  // only where the settings say.
  SdrconnectInstall r;
  if (!headlessPath.empty() && fileExists(headlessPath)) {
    r.headless = headlessPath;
    const std::size_t slash = headlessPath.find_last_of('/');
    r.folder = slash == std::string::npos ? std::string() : headlessPath.substr(0, slash);
  }
  return r;
}

int processRunning(const char*) { return -1; }

ChildProcess::~ChildProcess() { stop(); }

bool ChildProcess::start(const std::string& exe, const std::vector<std::string>& args, bool, const std::string& logPath) {
  stop();
  exitCode_ = -1;
  std::vector<std::string> all = {exe};
  all.insert(all.end(), args.begin(), args.end());
  std::vector<char*> argv;
  for (auto& a : all) argv.push_back(&a[0]);
  argv.push_back(nullptr);
  posix_spawn_file_actions_t fa;
  posix_spawn_file_actions_init(&fa);
  if (!logPath.empty()) {
    posix_spawn_file_actions_addopen(&fa, 1, logPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    posix_spawn_file_actions_adddup2(&fa, 1, 2);
  }
  pid_t pid;
  const int rc = posix_spawn(&pid, exe.c_str(), &fa, nullptr, argv.data(), environ);
  posix_spawn_file_actions_destroy(&fa);
  if (rc != 0) return false;
  pid_ = pid;
  return true;
}

bool ChildProcess::running() {
  if (!pid_) return false;
  int status = 0;
  if (waitpid(static_cast<pid_t>(pid_), &status, WNOHANG) == 0) return true;
  exitCode_ = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + (WIFSIGNALED(status) ? WTERMSIG(status) : 0);
  pid_ = 0;
  return false;
}

void ChildProcess::stop() {
  if (!pid_) return;
  if (running()) {
    kill(static_cast<pid_t>(pid_), SIGTERM);
    for (int i = 0; i < 30 && running(); ++i) {
      struct timespec ts {0, 100000000};
      nanosleep(&ts, nullptr);
    }
    if (pid_) {
      kill(static_cast<pid_t>(pid_), SIGKILL);
      waitpid(static_cast<pid_t>(pid_), nullptr, 0);
    }
  }
  pid_ = 0;
}

void openWindow(const std::string& url) {
  pid_t pid;
  std::string u = url;
  char prog[] = "xdg-open";
  char* argv[] = {prog, &u[0], nullptr};
  if (posix_spawnp(&pid, "xdg-open", nullptr, nullptr, argv, environ) == 0) return;
  std::printf("Open %s in your browser.\n", url.c_str());
}

void openFolder(const std::string& path) { openWindow(path); }  // xdg-open takes folders too

SystemInfo systemInfo() {
  SystemInfo r;
  struct utsname u;
  if (uname(&u) == 0) {
    r.os = std::string(u.sysname) + " " + u.release + " " + u.version;
    r.machine = u.machine;
  }
  r.cpus = static_cast<int>(sysconf(_SC_NPROCESSORS_ONLN));
  const long pages = sysconf(_SC_PHYS_PAGES), pageSize = sysconf(_SC_PAGESIZE);
  if (pages > 0 && pageSize > 0) r.memoryGb = static_cast<double>(pages) * static_cast<double>(pageSize) / (1024.0 * 1024 * 1024);
  std::error_code ec;
  const auto exe = std::filesystem::read_symlink("/proc/self/exe", ec);
  if (!ec) r.executable = exe.u8string();
  return r;
}

std::string fileVersion(const std::string&) { return ""; }

bool preferFullSpeed() { return false; }

std::string systemLanguage() {
  for (const char* v : {"LC_ALL", "LC_MESSAGES", "LANG"}) {
    const char* l = std::getenv(v);
    if (l && *l) return std::strncmp(l, "bg", 2) == 0 ? "bg" : "en";
  }
  return "en";
}

#endif

}  // namespace hfc
