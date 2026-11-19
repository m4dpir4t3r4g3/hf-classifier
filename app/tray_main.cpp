// HFClassifier.exe: the classifier for everyday use on Windows. Runs without
// a console window, with an icon in the notification area (open the window,
// open the data folder, exit), opens its window at start (Settings: "Open
// the window at start"), and reports start-up problems in a message box,
// in Bulgarian or English. Its output goes to hf_classifier.log in the data
// folder (the previous run's to hf_classifier.previous.log). Started a
// second time (the shortcut clicked again), it opens the first one's window
// and exits, leaving the first one's log alone. The options are
// hf_classifier's; with --report it makes a diagnostic report, says where,
// shows it in Explorer, and exits (the Start menu's "diagnostic report").
#include <windows.h>
#include <shellapi.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "hfc/file.hpp"
#include "log_capture.hpp"
#include "platform.hpp"
#include "runner.hpp"

using namespace hfc;

namespace {

constexpr UINT kTrayMessage = WM_APP + 1;
constexpr UINT kReadyMessage = WM_APP + 2;
constexpr UINT kFailedMessage = WM_APP + 3;
constexpr UINT kDoneMessage = WM_APP + 4;
constexpr UINT kReportMessage = WM_APP + 5;
constexpr UINT kOpenCommand = 1, kFolderCommand = 2, kExitCommand = 3, kReportCommand = 4;

struct Texts {
  const wchar_t* tip;
  const wchar_t* open;
  const wchar_t* folder;
  const wchar_t* exit;
  const wchar_t* stopping;
  const wchar_t* runningTitle;
  const wchar_t* runningText;
  const wchar_t* portBusy;     // %ls: the details
  const wchar_t* startFailed;  // %ls: why
  const wchar_t* badOption;    // %ls: what
  const wchar_t* report;
  const wchar_t* reportBusy;
  const wchar_t* reportDone;   // %ls: the file
  const wchar_t* reportFailed; // %ls: why
};

const Texts kEnglish = {
    L"HF Classifier",
    L"&Open HF Classifier",
    L"Open the &data folder (log, settings)",
    L"E&xit",
    L"HF Classifier: stopping...",
    L"HF Classifier is running",
    L"It runs in the notification area. Click its icon to open the window.",
    L"The port of the program's window is in use by another program (%ls).\n\n"
    L"Close that program, or choose another port: \"http_port\" in settings.json in the data folder.",
    L"HF Classifier cannot start:\n\n%ls",
    L"HF Classifier cannot start with these options:\n\n%ls",
    L"Make a diagnostic &report",
    L"HF Classifier: making a diagnostic report...",
    L"The diagnostic report is ready:\n\n%ls\n\nExplorer shows it now. Send this ZIP file to whoever helps you with the program; README.txt in it says what it contains.",
    L"The diagnostic report could not be made:\n\n%ls",
};

const Texts kBulgarian = {
    L"HF класификатор",
    L"&Отвори HF класификатора",
    L"Отвори &папката с данни (дневник, настройки)",
    L"&Изход",
    L"HF класификатор: спиране...",
    L"HF класификаторът работи",
    L"Работи в областта за уведомяване. Щракнете върху иконата му, за да отворите прозореца.",
    L"Портът на прозореца на програмата е зает от друга програма (%ls).\n\n"
    L"Затворете другата програма или изберете друг порт: \"http_port\" в settings.json в папката с данни.",
    L"HF класификаторът не може да стартира:\n\n%ls",
    L"HF класификаторът не може да стартира с тези параметри:\n\n%ls",
    L"Направи диагностичен &отчет",
    L"HF класификатор: прави се диагностичен отчет...",
    L"Диагностичният отчет е готов:\n\n%ls\n\nExplorer го показва сега. Изпратете този ZIP файл на този, който ви помага с програмата; README.txt в него казва какво съдържа.",
    L"Диагностичният отчет не можа да бъде направен:\n\n%ls",
};

const Texts* g_text = &kEnglish;
HWND g_window = nullptr;
NOTIFYICONDATAW g_icon{};
UINT g_taskbarCreated = 0;
std::atomic<bool> g_stop{false};
std::thread g_runner;
std::wstring g_url, g_failure;
RunOptions g_options;
std::thread g_reporter;
std::atomic<bool> g_reporting{false};
std::wstring g_reportPath, g_reportError;  // set by the reporter thread before kReportMessage
bool g_failureIsPort = false, g_showBalloon = false, g_stopping = false, g_iconShown = false;
std::mutex g_urlMutex;

std::wstring format(const wchar_t* fmt, const std::wstring& arg) {
  std::vector<wchar_t> buf(std::wcslen(fmt) + arg.size() + 16);
  std::swprintf(buf.data(), buf.size(), fmt, arg.c_str());
  return buf.data();
}

void addIcon() {
  g_icon = NOTIFYICONDATAW{};
  g_icon.cbSize = sizeof(g_icon);
  g_icon.hWnd = g_window;
  g_icon.uID = 1;
  g_icon.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
  g_icon.uCallbackMessage = kTrayMessage;
  g_icon.hIcon = static_cast<HICON>(LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1), IMAGE_ICON,
                                               GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0));
  wcsncpy_s(g_icon.szTip, g_text->tip, _TRUNCATE);
  Shell_NotifyIconW(NIM_ADD, &g_icon);
}

void setTip(const wchar_t* tip) {
  wcsncpy_s(g_icon.szTip, tip, _TRUNCATE);
  g_icon.uFlags = NIF_TIP;
  Shell_NotifyIconW(NIM_MODIFY, &g_icon);
}

void balloon(const wchar_t* title, const wchar_t* text) {
  g_icon.uFlags = NIF_INFO;
  wcsncpy_s(g_icon.szInfoTitle, title, _TRUNCATE);
  wcsncpy_s(g_icon.szInfo, text, _TRUNCATE);
  g_icon.dwInfoFlags = NIIF_INFO;
  Shell_NotifyIconW(NIM_MODIFY, &g_icon);
}

std::wstring url() {
  std::lock_guard<std::mutex> l(g_urlMutex);
  return g_url;
}

void openUi() {
  const std::wstring u = url();
  if (!u.empty()) openWindow(narrow(u));
}

void openDataFolder() {
  const std::string d = dataFile("");
  ShellExecuteW(nullptr, L"open", widen(d).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

// Shows a finished report: a message, and the file selected in Explorer.
void showReport(const std::wstring& path, const std::wstring& error) {
  if (path.empty()) {
    MessageBoxW(nullptr, format(g_text->reportFailed, error).c_str(), g_text->tip, MB_ICONERROR | MB_OK);
    return;
  }
  const std::wstring args = L"/select,\"" + path + L"\"";
  ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
  MessageBoxW(nullptr, format(g_text->reportDone, path).c_str(), g_text->tip, MB_ICONINFORMATION | MB_OK);
}

// From the menu: the running program is asked over its own web port, as
// "--report" would, in a thread so that the menu stays responsive.
void beginReport() {
  if (g_reporting.exchange(true)) return;
  if (g_reporter.joinable()) g_reporter.join();
  setTip(g_text->reportBusy);
  RunOptions o = g_options;
  {
    std::lock_guard<std::mutex> l(g_urlMutex);
    const std::string u = narrow(g_url);
    const std::size_t colon = u.rfind(':');
    if (colon != std::string::npos) o.settings.httpPort = std::atoi(u.substr(colon + 1).c_str());
  }
  g_reporter = std::thread([o] {
    std::string error;
    const std::string path = makeReport(o, false, nullptr, &error);
    g_reportPath = widen(path);
    g_reportError = widen(error);
    PostMessageW(g_window, kReportMessage, 0, 0);
  });
}

void beginExit() {
  if (g_stopping) return;
  g_stopping = true;
  setTip(g_text->stopping);
  g_stop = true;  // the runner thread posts kDoneMessage when it has stopped
}

void showMenu() {
  HMENU m = CreatePopupMenu();
  AppendMenuW(m, MF_STRING | (url().empty() ? MF_GRAYED : 0), kOpenCommand, g_text->open);
  AppendMenuW(m, MF_STRING, kFolderCommand, g_text->folder);
  AppendMenuW(m, MF_STRING | (url().empty() || g_reporting ? MF_GRAYED : 0), kReportCommand, g_text->report);
  AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(m, MF_STRING, kExitCommand, g_text->exit);
  SetMenuDefaultItem(m, kOpenCommand, FALSE);
  POINT p;
  GetCursorPos(&p);
  SetForegroundWindow(g_window);  // so that the menu closes when clicking elsewhere
  TrackPopupMenu(m, TPM_RIGHTBUTTON, p.x, p.y, 0, g_window, nullptr);
  PostMessageW(g_window, WM_NULL, 0, 0);
  DestroyMenu(m);
}

LRESULT CALLBACK windowProc(HWND w, UINT msg, WPARAM wp, LPARAM lp) {
  if (msg == g_taskbarCreated && g_taskbarCreated != 0) {
    addIcon();  // Explorer restarted
    return 0;
  }
  switch (msg) {
    case kTrayMessage:
      if (lp == WM_LBUTTONUP || lp == WM_LBUTTONDBLCLK) openUi();
      else if (lp == WM_RBUTTONUP || lp == WM_CONTEXTMENU) showMenu();
      return 0;
    case kReadyMessage:
      // The icon only now: a second instance never gets this far.
      if (!g_iconShown) {
        addIcon();
        g_iconShown = true;
        if (g_showBalloon) balloon(g_text->runningTitle, g_text->runningText);
      }
      return 0;
    case kFailedMessage:
      MessageBoxW(nullptr, format(g_failureIsPort ? g_text->portBusy : g_text->startFailed, g_failure).c_str(),
                  g_text->tip, MB_ICONERROR | MB_OK);
      return 0;
    case kReportMessage:
      if (g_reporter.joinable()) g_reporter.join();
      setTip(g_text->tip);
      showReport(g_reportPath, g_reportError);
      g_reporting = false;
      return 0;
    case kDoneMessage:
      if (g_reporter.joinable()) g_reporter.join();
      if (g_runner.joinable()) g_runner.join();
      Shell_NotifyIconW(NIM_DELETE, &g_icon);
      PostQuitMessage(static_cast<int>(wp));
      return 0;
    case WM_COMMAND:
      if (LOWORD(wp) == kOpenCommand) openUi();
      else if (LOWORD(wp) == kFolderCommand) openDataFolder();
      else if (LOWORD(wp) == kReportCommand) beginReport();
      else if (LOWORD(wp) == kExitCommand) beginExit();
      return 0;
    case WM_CLOSE:
      // Asked to close (the installer, "taskkill /IM HFClassifier.exe"):
      // stop as Exit does, saving the log.
      beginExit();
      return 0;
    case WM_QUERYENDSESSION:
      return TRUE;
    case WM_ENDSESSION:
      // Windows is shutting down: stop now (the log is committed on stop).
      if (wp) {
        g_stop = true;
        if (g_runner.joinable()) g_runner.join();
        Shell_NotifyIconW(NIM_DELETE, &g_icon);
      }
      return 0;
  }
  return DefWindowProcW(w, msg, wp, lp);
}

// Output to hf_classifier.log in the data folder (there is no console), a
// new one each run; the last run's becomes hf_classifier.previous.log.
void logToFile() {
  const std::wstring path = widen(dataFile("hf_classifier.log"));
  const std::wstring previous = widen(dataFile("hf_classifier.previous.log"));
  MoveFileExW(path.c_str(), previous.c_str(), MOVEFILE_REPLACE_EXISTING);
  FILE* out = nullptr;
  if (_wfreopen_s(&out, path.c_str(), L"a", stdout) == 0) setvbuf(stdout, nullptr, _IONBF, 0);
  if (_wfreopen_s(&out, path.c_str(), L"a", stderr) == 0) setvbuf(stderr, nullptr, _IONBF, 0);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
  int argc = 0;
  LPWSTR* wargv = CommandLineToArgvW(GetCommandLineW(), &argc);
  std::vector<std::string> args;
  for (int i = 1; i < argc; ++i) args.push_back(narrow(wargv[i]));
  if (wargv) LocalFree(wargv);

  RunOptions o = defaultRunOptions();
  std::string err;
  bool help = false;
  const bool parsed = parseCommandLine(args, o, &err, &help);
  g_text = effectiveLanguage(o.settings) == "bg" ? &kBulgarian : &kEnglish;
  if (!parsed || help) {
    MessageBoxW(nullptr, format(g_text->badOption, widen(help ? usageText() : err)).c_str(), g_text->tip,
                help ? MB_ICONINFORMATION : MB_ICONERROR);
    return help ? 0 : 2;
  }
  if (o.report) {
    // No log of its own: the report includes the program's.
    std::string error;
    const std::string path = makeReport(o, o.reportRates, nullptr, &error);
    showReport(widen(path), widen(error));
    return path.empty() ? 1 : 0;
  }
  // The log only now, and only for the one instance that runs: a second
  // start (it opens the first one's window and exits) must not empty the
  // first one's log.
  const uint16_t port = static_cast<uint16_t>(o.settings.httpPort);
  if (port == 0 || !otherInstanceAt(port)) {
    // Through a pipe this process reads (log_capture.hpp); the file as
    // before if that cannot be set up.
    if (!startOutputCapture(dataFile("hf_classifier.log"), dataFile("hf_classifier.previous.log"))) logToFile();
    // Said before there was a log: say it again in it.
    Settings check;
    std::string bad;
    if (!loadSettings(o.settingsPath, check, &bad)) std::fprintf(stderr, "settings: %s (using defaults)\n", bad.c_str());
  }
  g_options = o;
  // Without a window opened at start, say where the program went.
  g_showBalloon = !o.openWindow;

  WNDCLASSW wc{};
  wc.lpfnWndProc = windowProc;
  wc.hInstance = instance;
  wc.lpszClassName = L"HFClassifierTray";
  wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(1));
  RegisterClassW(&wc);
  g_window = CreateWindowW(wc.lpszClassName, L"HF Classifier", WS_OVERLAPPED, 0, 0, 0, 0, nullptr, nullptr, instance,
                           nullptr);
  g_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");

  RunHooks hooks;
  hooks.ready = [](const std::string& u) {
    {
      std::lock_guard<std::mutex> l(g_urlMutex);
      g_url = widen(u);
    }
    PostMessageW(g_window, kReadyMessage, 0, 0);
  };
  hooks.failed = [](const std::string& code, const std::string& detail) {
    g_failureIsPort = code == "port_busy";
    g_failure = widen(detail);
    // Shown before the runner returns (and the program quits).
    SendMessageW(g_window, kFailedMessage, 0, 0);
  };
  g_runner = std::thread([o, hooks] {
    const int rc = runClassifier(o, g_stop, hooks);
    PostMessageW(g_window, kDoneMessage, rc == kRunAlreadyRunning ? 0 : static_cast<WPARAM>(rc), 0);
  });
  MSG msg;
  while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }
  if (g_runner.joinable()) {
    g_stop = true;
    g_runner.join();
  }
  if (g_reporter.joinable()) g_reporter.join();
  stopOutputCapture();
  return static_cast<int>(msg.wParam);
}
