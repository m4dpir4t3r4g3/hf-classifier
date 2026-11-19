#include "hfc/file.hpp"

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#else
#include <sys/stat.h>
#include <cerrno>
#endif

namespace hfc {

#ifdef _WIN32
std::wstring widen(const std::string& s) {
  if (s.empty()) return std::wstring();
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
  std::wstring w(static_cast<std::size_t>(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), &w[0], n);
  return w;
}

std::string narrow(const std::wstring& w) {
  if (w.empty()) return std::string();
  const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
  std::string s(static_cast<std::size_t>(n), '\0');
  WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), &s[0], n, nullptr, nullptr);
  return s;
}

std::FILE* openFile(const std::string& path, const char* mode) {
  return _wfopen(widen(path).c_str(), widen(mode).c_str());
}

bool makeDirectory(const std::string& path) {
  const std::wstring w = widen(path);
  if (CreateDirectoryW(w.c_str(), nullptr)) return true;
  const DWORD a = GetFileAttributesW(w.c_str());
  return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

bool fileExists(const std::string& path) {
  const DWORD a = GetFileAttributesW(widen(path).c_str());
  return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

std::vector<std::string> utf8Arguments(int argc, char** argv) {
  std::vector<std::string> out;
  int n = 0;
  LPWSTR* w = CommandLineToArgvW(GetCommandLineW(), &n);
  if (w && n == argc) {
    for (int i = 1; i < n; ++i) out.push_back(narrow(w[i]));
  } else {
    for (int i = 1; i < argc; ++i) out.push_back(argv[i]);
  }
  if (w) LocalFree(w);
  return out;
}
#else
std::FILE* openFile(const std::string& path, const char* mode) { return std::fopen(path.c_str(), mode); }

bool makeDirectory(const std::string& path) {
  if (mkdir(path.c_str(), 0755) == 0) return true;
  struct stat st;
  return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

bool fileExists(const std::string& path) {
  struct stat st;
  return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

std::vector<std::string> utf8Arguments(int argc, char** argv) { return std::vector<std::string>(argv + 1, argv + argc); }
#endif

}  // namespace hfc
