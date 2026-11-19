#include "diagnostics.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "hfc/file.hpp"
#include "hfc/json.hpp"
#include "hfc/net.hpp"
#include "hfc/sigmf.hpp"
#include "hfc/zip.hpp"
#include "platform.hpp"

namespace hfc {

namespace {

constexpr std::size_t kMaxLogBytes = 2u << 20;

const char* kReadme =
    "HF Classifier diagnostic report\r\n"
    "===============================\r\n"
    "\r\n"
    "Made by HF Classifier to show why it does not work as expected on this PC.\r\n"
    "Send the whole ZIP file to whoever helps you with the program.\r\n"
    "\r\n"
    "Inside:\r\n"
    "  report.json      the program and its version; Windows, processor and memory;\r\n"
    "                   the settings; the connection to SDRconnect and what it is doing;\r\n"
    "                   where SDRconnect is installed, its version, whether it runs and\r\n"
    "                   answers; and the measurements of how SDRconnect delivers IQ\r\n"
    "                   (as sdrc_probe makes them), or why they were not made\r\n"
    "  probe.txt        those measurements as text\r\n"
    "  settings.json    the settings file\r\n"
    "  *.log            the program's messages (this run and the one before) and those\r\n"
    "                   of SDRconnect_headless, if the program started it (the last 2 MB)\r\n"
    "\r\n"
    "Not inside: recordings, pictures, or the log of received signals.\r\n"
    "The file paths in it can contain your Windows user name. Open the files and\r\n"
    "look before you send them if that matters to you.\r\n"
    "\r\n"
    "\r\n";

// The Bulgarian part, kept readable here; written as UTF-8.
const char* kReadmeBg = u8R"(
Диагностичен отчет на HF класификатора
======================================

Направен от HF класификатора, за да се види защо не работи както се очаква на този компютър.
Изпратете целия ZIP файл на този, който ви помага с програмата.

Съдържание:
  report.json      програмата и версията ѝ; Windows, процесор и памет; настройките;
                   връзката със SDRconnect и какво прави тя; къде е инсталиран
                   SDRconnect, версията му, дали работи и отговаря; и измерванията
                   на това как SDRconnect подава IQ (както ги прави sdrc_probe),
                   или защо не са направени
  probe.txt        тези измервания като текст
  settings.json    файлът с настройките
  *.log            съобщенията на програмата (от това и от предишното стартиране)
                   и на SDRconnect_headless, ако програмата го е стартирала
                   (последните 2 MB)

Не съдържа: записи, картини или дневника на приетите сигнали.
Пътищата до файловете в него може да съдържат потребителското ви име в Windows.
Отворете файловете и ги прегледайте преди изпращане, ако това е важно за вас.
)";

std::string crlf(const std::string& s) {
  std::string out;
  for (char c : s) {
    if (c == '\n') out += "\r\n";
    else out += c;
  }
  return out;
}

std::string timestamp(double unixTime, bool forFile) {
  std::time_t t = static_cast<std::time_t>(unixTime);
  std::tm tm{};
#ifdef _WIN32
  localtime_s(&tm, &t);
#else
  localtime_r(&t, &tm);
#endif
  char b[64];
  std::strftime(b, sizeof(b), forFile ? "%Y%m%d-%H%M%S" : "%Y-%m-%d %H:%M:%S %z", &tm);
  return b;
}

void rawOrNull(JsonWriter& j, const std::string& json) {
  if (json.empty()) j.raw("null");
  else j.raw(json);
}

}  // namespace

std::string fileTail(const std::string& path, std::size_t maxBytes) {
  std::ifstream in(std::filesystem::u8path(path), std::ios::binary);
  if (!in) return "";
  in.seekg(0, std::ios::end);
  const std::streamoff size = in.tellg();
  if (size <= 0) return "";
  const std::streamoff from = size > static_cast<std::streamoff>(maxBytes) ? size - static_cast<std::streamoff>(maxBytes) : 0;
  in.seekg(from);
  std::string s(static_cast<std::size_t>(size - from), '\0');
  in.read(&s[0], static_cast<std::streamsize>(s.size()));
  s.resize(static_cast<std::size_t>(in.gcount()));
  if (from > 0) {
    // From the next whole line on.
    const std::size_t nl = s.find('\n');
    if (nl != std::string::npos) s.erase(0, nl + 1);
    s = "[... the first " + std::to_string(static_cast<long long>(from)) + " bytes are left out ...]\n" + s;
  }
  return s;
}

std::string diagnosticJson(const DiagnosticInputs& in) {
  const double now = in.unixTime > 0 ? in.unixTime : nowUtc();
  JsonWriter j;
  j.beginObject();
  j.key("report").value("HF Classifier diagnostic report");
  j.key("version").value(HFC_VERSION);
  j.key("utc").value(isoUtc(now));
  j.key("local_time").value(timestamp(now, false));
  j.key("mode").value(in.mode);
  if (!in.note.empty()) j.key("note").value(in.note);

  const SystemInfo si = systemInfo();
  j.key("system").beginObject();
  j.key("os").value(si.os);
  j.key("machine").value(si.machine);
  j.key("logical_processors").value(si.cpus);
  j.key("memory_gb").value(std::round(si.memoryGb * 10) / 10);
  j.key("program").value(si.executable);
  j.key("language").value(systemLanguage());
  j.key("full_speed").value(preferFullSpeed());  // not "efficiency mode" (Windows)
  j.endObject();

  j.key("settings");
  rawOrNull(j, in.settingsJson);

  // SDRconnect on this computer.
  j.key("sdrconnect").beginObject();
  j.key("endpoint").value(in.sdrHost + ":" + std::to_string(in.sdrPort));
  const SdrconnectInstall inst = findSdrconnect(in.headlessPath);
  j.key("install_folder").value(inst.folder);
  j.key("headless").value(inst.headless);
  if (!inst.folder.empty()) {
    const std::string gui = (std::filesystem::u8path(inst.folder) / "SDRconnect.exe").u8string();
    j.key("sdrconnect_exe").value(fileExists(gui) ? gui : std::string());
    j.key("sdrconnect_version").value(fileExists(gui) ? fileVersion(gui) : std::string());
  }
  j.key("headless_version").value(inst.headless.empty() ? std::string() : fileVersion(inst.headless));
  auto running = [](const char* exe) {
    const int r = processRunning(exe);
    return r == 1 ? "yes" : r == 0 ? "no" : "cannot tell";
  };
  j.key("sdrconnect_running").value(running("SDRconnect.exe"));
  j.key("headless_running").value(running("SDRconnect_headless.exe"));
  {
    TcpSocket s;
    std::string err;
    const bool answers = s.connect(in.sdrHost, static_cast<uint16_t>(in.sdrPort), 1000, &err);
    j.key("port_answers").value(answers);
    if (!answers) j.key("port_error").value(err);
  }
  j.endObject();

  j.key("status");
  rawOrNull(j, in.statusJson);
  j.key("usage");
  rawOrNull(j, in.usageJson);

  j.key("probe");
  if (!in.probeJson.empty()) j.raw(in.probeJson);
  else j.beginObject().key("skipped").value(in.probeSkipped.empty() ? std::string("not asked for") : in.probeSkipped).endObject();

  // The data folder: names and sizes only.
  j.key("data_folder").beginObject();
  j.key("path").value(in.dataFolder);
  j.key("files").beginArray();
  if (!in.dataFolder.empty()) {
    std::error_code ec;
    std::vector<std::pair<std::string, long long>> list;
    for (std::filesystem::directory_iterator it(std::filesystem::u8path(in.dataFolder), ec), end; !ec && it != end;
         it.increment(ec)) {
      const bool dir = it->is_directory(ec);
      const long long size = dir ? -1 : static_cast<long long>(it->file_size(ec));
      list.emplace_back(it->path().filename().u8string() + (dir ? "/" : ""), size);
    }
    std::sort(list.begin(), list.end());
    for (const auto& [name, size] : list) {
      j.beginObject().key("name").value(name);
      if (size >= 0) j.key("bytes").value(static_cast<int64_t>(size));
      j.endObject();
    }
  }
  j.endArray();
  j.endObject();
  j.endObject();
  return j.str();
}

std::string writeDiagnosticReport(const DiagnosticInputs& in, std::string* error) {
  const double now = in.unixTime > 0 ? in.unixTime : nowUtc();
  // The program's own messages, into its log before the log is read (a
  // report from a user had this run's log empty).
  std::fflush(stdout);
  std::fflush(stderr);
  std::vector<ZipEntry> entries;
  entries.push_back({"README.txt", kReadme + crlf(kReadmeBg)});
  entries.push_back({"report.json", diagnosticJson(in) + "\n"});
  entries.push_back({"probe.txt", in.probeLog.empty() ? "(not measured: " + (in.probeSkipped.empty() ? std::string("not asked for") : in.probeSkipped) + ")\n" : in.probeLog});
  for (const auto& [name, path] : in.files) {
    if (name == "hf_classifier.log" && !in.programLog.empty()) {
      entries.push_back({name, in.programLog});
      continue;
    }
    if (path.empty() || !fileExists(path)) continue;
    entries.push_back({name, fileTail(path, kMaxLogBytes)});
  }
  std::error_code ec;
  std::filesystem::create_directories(std::filesystem::u8path(in.folder), ec);
  // Two reports in one second get different names (-2, -3...).
  const std::string stem = "hfc_report_" + timestamp(now, true);
  std::string path = (std::filesystem::u8path(in.folder) / (stem + ".zip")).u8string();
  for (int k = 2; fileExists(path) && k < 100; ++k)
    path = (std::filesystem::u8path(in.folder) / (stem + "-" + std::to_string(k) + ".zip")).u8string();
  const std::string zip = makeZip(entries, now);
  if (zip.empty()) {
    if (error) *error = "the report is too big for a ZIP file";
    return "";
  }
  std::FILE* f = openFile(path, "wb");
  if (!f) {
    if (error) *error = "cannot write " + path;
    return "";
  }
  const bool ok = std::fwrite(zip.data(), 1, zip.size(), f) == zip.size();
  if (std::fclose(f) != 0 || !ok) {
    if (error) *error = "cannot write " + path;
    std::filesystem::remove(std::filesystem::u8path(path), ec);
    return "";
  }
  return path;
}

}  // namespace hfc
