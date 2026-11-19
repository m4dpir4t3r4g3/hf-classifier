#include "log_capture.hpp"

#include <mutex>
#include <string>
#include <thread>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <windows.h>

#include <cstdio>
#include <share.h>

#include "hfc/file.hpp"
#endif

namespace hfc {

namespace {

constexpr std::size_t kKeep = 2u << 20;
std::mutex g_mu;
std::string g_text;  // the last kKeep bytes, under g_mu
#ifdef _WIN32
bool g_capturing = false;
std::thread g_reader;
#endif

}  // namespace

std::string capturedOutput() {
  std::lock_guard<std::mutex> l(g_mu);
  return g_text;
}

#ifdef _WIN32

bool startOutputCapture(const std::string& path, const std::string& previousPath) {
  if (g_capturing) return false;
  const std::wstring wpath = widen(path);
  MoveFileExW(wpath.c_str(), widen(previousPath).c_str(), MOVEFILE_REPLACE_EXISTING);
  std::FILE* file = _wfsopen(wpath.c_str(), L"wb", _SH_DENYNO);
  int fds[2];
  if (_pipe(fds, 64 * 1024, _O_BINARY | _O_NOINHERIT) != 0) {
    if (file) std::fclose(file);
    return false;
  }
  // A program without a console has no valid stdout or stderr descriptors:
  // give them one (NUL), then put the pipe in its place.
  FILE* f = nullptr;
  _wfreopen_s(&f, L"NUL", L"w", stdout);
  _wfreopen_s(&f, L"NUL", L"w", stderr);
  if (_dup2(fds[1], _fileno(stdout)) != 0 || _dup2(fds[1], _fileno(stderr)) != 0) {
    _close(fds[0]);
    _close(fds[1]);
    if (file) std::fclose(file);
    return false;
  }
  _close(fds[1]);
  setvbuf(stdout, nullptr, _IONBF, 0);
  setvbuf(stderr, nullptr, _IONBF, 0);
  g_capturing = true;
  const int readFd = fds[0];
  g_reader = std::thread([readFd, file] {
    char buf[4096];
    int n;
    while ((n = _read(readFd, buf, sizeof(buf))) > 0) {
      if (file) {
        std::fwrite(buf, 1, static_cast<std::size_t>(n), file);
        std::fflush(file);
      }
      std::lock_guard<std::mutex> l(g_mu);
      g_text.append(buf, static_cast<std::size_t>(n));
      if (g_text.size() > kKeep + kKeep / 4) {
        // From a whole line on.
        std::size_t cut = g_text.size() - kKeep;
        const std::size_t nl = g_text.find('\n', cut);
        if (nl != std::string::npos) cut = nl + 1;
        g_text.erase(0, cut);
      }
    }
    _close(readFd);
    if (file) std::fclose(file);
  });
  return true;
}

void stopOutputCapture() {
  if (!g_capturing) return;
  std::fflush(stdout);
  std::fflush(stderr);
  // The pipe's last writers gone, the reader sees its end.
  _close(_fileno(stdout));
  _close(_fileno(stderr));
  if (g_reader.joinable()) g_reader.join();
  g_capturing = false;
}

#else

bool startOutputCapture(const std::string&, const std::string&) { return false; }
void stopOutputCapture() {}

#endif

}  // namespace hfc
