#include "classifier_app.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "hfc/http_get.hpp"
#include "hfc/sigmf.hpp"

namespace hfc {

namespace {

std::string num(double v, int decimals) {
  if (!std::isfinite(v)) return "null";
  char b[64];
  std::snprintf(b, sizeof(b), "%.*f", decimals, v);
  return b;
}

double unixNowSeconds() { return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count(); }

double param(const HttpRequest& q, const char* name, double fallback) {
  auto it = q.query.find(name);
  if (it == q.query.end() || it->second.empty()) return fallback;
  const double iso = parseIsoUtc(it->second);
  if (iso > 0) return iso;
  char* end = nullptr;
  const double v = std::strtod(it->second.c_str(), &end);
  return end && *end == '\0' ? v : fallback;
}

Query queryOf(const HttpRequest& r) {
  Query q;
  q.fromUtc = param(r, "from", 0);
  q.toUtc = param(r, "to", 1e300);
  q.minHz = param(r, "min_khz", 0) * 1000.0;
  q.maxHz = param(r, "max_khz", 1e297) * 1000.0;
  auto it = r.query.find("label");
  if (it != r.query.end()) q.label = it->second;
  it = r.query.find("text");
  if (it != r.query.end()) q.text = it->second;
  q.limit = static_cast<int>(std::clamp(param(r, "limit", 1000), 1.0, 100000.0));
  return q;
}

HttpResponse json(std::string body) {
  HttpResponse r;
  r.contentType = "application/json";
  r.body = std::move(body);
  return r;
}


// A file of `folder` the browser asks for by name: plain names ending in `ext`
// only, regular files, and not unreasonably large.
HttpResponse servedFile(const std::string& folder, const std::string& name, const std::string& ext,
                        const char* contentType) {
  bool fine = !folder.empty() && name.size() > ext.size() && name.size() < 100 && name[0] != '.' &&
              name.compare(name.size() - ext.size(), ext.size(), ext) == 0;
  for (char c : name) fine = fine && (std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '_' || c == '-');
  if (!fine || name.find("..") != std::string::npos) return HttpResponse::notFound();
  const auto path = std::filesystem::u8path(folder) / name;
  std::error_code ec;
  const auto size = std::filesystem::is_regular_file(path, ec) ? std::filesystem::file_size(path, ec) : 0;
  if (ec || size == 0 || size > (std::uintmax_t(64) << 20)) return HttpResponse::notFound();
  std::ifstream in(path, std::ios::binary);
  if (!in) return HttpResponse::notFound();
  HttpResponse r;
  r.contentType = contentType;
  r.body.resize(static_cast<std::size_t>(size));
  if (!in.read(&r.body[0], static_cast<std::streamsize>(size))) return HttpResponse::notFound();
  return r;
}

}  // namespace

std::string emissionJson(int rx, const char* event, const Emission& e, const std::string& label,
                         const std::string& features) {
  return std::string("{\"type\":\"emission\",\"event\":\"") + event + "\",\"rx\":" + std::to_string(rx) +
         ",\"id\":" + std::to_string(e.id) + ",\"start\":" + num(e.startUtc, 3) + ",\"end\":" + num(e.endUtc, 3) +
         ",\"lo\":" + num(e.loHz, 1) + ",\"hi\":" + num(e.hiHz, 1) + ",\"centre\":" + num(e.centreHz, 1) +
         ",\"occ_lo\":" + num(e.occLoHz, 1) + ",\"occ_hi\":" + num(e.occHiHz, 1) +
         ",\"snr\":" + num(e.meanSnrDb, 1) + ",\"peak\":" + num(e.peakSnrDb, 1) + ",\"duty\":" + num(e.duty, 2) +
         ",\"drift\":" + num(e.driftHzPerSecond, 3) + ",\"label\":" + jsonQuote(label) +
         (features.empty() ? std::string() : ",\"features\":" + features) + "}";
}

std::string decodeJson(int rx, const DecodeEvent& d) {
  return std::string("{\"type\":\"decode\",\"rx\":") + std::to_string(rx) + ",\"id\":" + std::to_string(d.emissionId) +
         ",\"utc\":" + num(d.utc, 3) + ",\"decoder\":" + jsonQuote(d.decoder) + ",\"freq\":" + num(d.freqHz, 1) +
         ",\"quality\":" + num(d.quality, 2) + ",\"first\":" + (d.first ? "true" : "false") +
         ",\"text\":" + jsonQuote(d.text) + "}";
}

std::vector<uint8_t> waterfallMessage(int rx, const WaterfallRow& row) {
  const std::size_t n = row.db.size();
  std::vector<uint8_t> m(4 + 1 + 2 + 24 + n);
  std::memcpy(m.data(), "WF01", 4);
  m[4] = static_cast<uint8_t>(rx);
  m[5] = static_cast<uint8_t>(n & 0xFF);
  m[6] = static_cast<uint8_t>(n >> 8);
  auto putDouble = [&](std::size_t at, double v) {
    uint64_t bits;
    std::memcpy(&bits, &v, 8);
    for (int i = 0; i < 8; ++i) m[at + static_cast<std::size_t>(i)] = static_cast<uint8_t>(bits >> (8 * i));
  };
  putDouble(7, row.utc);
  putDouble(15, row.lowHz);
  putDouble(23, row.highHz);
  std::vector<float> f = row.floorDb;
  float ref = 0;
  if (!f.empty()) {
    std::nth_element(f.begin(), f.begin() + static_cast<std::ptrdiff_t>(f.size() / 2), f.end());
    ref = f[f.size() / 2];
  }
  for (std::size_t i = 0; i < n; ++i) {
    const double v = 40.0 + 4.0 * (row.db[i] - ref);
    m[31 + i] = static_cast<uint8_t>(std::clamp(std::lround(v), 0L, 255L));
  }
  return m;
}

struct ClassifierApp::Receiver {
  int rx = 0;
  std::string name;
  bool controllable = false;
  std::unique_ptr<Pipeline> pipeline;
  std::thread thread;
  std::mutex m;
  std::condition_variable cv, idle;
  std::deque<IqInput> q;
  double queuedSeconds = 0;
  double droppedSeconds = 0;  // of IQ dropped because processing fell behind, under m
  bool busy = false, stop = false, flush = false;
  double rate = 0, centre = 0;
  // Emissions now active, with their label and features, for browsers that connect.
  struct Active {
    Emission e;
    std::string label, features;
  };
  std::map<uint64_t, Active> active;
  // A new frequency list, taken over by the receiver's thread.
  std::shared_ptr<const FrequencyList> newList;  // under m
  // A new decoder policy and emissions to leave (under m).
  std::unique_ptr<DecoderHost::Config> newPolicy;
  std::vector<uint64_t> ignore;
  // The usage report: the account at the last report, when it was made,
  // and its JSON (under recMu).
  DecoderHost::Usage lastUsage;
  double lastIq = 0;
  double busySeconds = 0, lastBusy = 0;  // wall time spent processing blocks (receiver thread only)
  std::chrono::steady_clock::time_point lastUsageAt = std::chrono::steady_clock::now();
  std::map<uint64_t, double> lastSessionCpu;
  std::string usageJson;
  // The recording asked for (under m), and the one running (the receiver's
  // thread writes; the numbers for the UI under recMu).
  struct RecordRequest {
    bool start = false, stop = false;
    double seconds = 0, markHz = 0;
    std::string task, note, base;
  } recReq;
  SigmfWriter writer;
  mutable std::mutex recMu;
  bool recording = false;
  double recSeconds = 0, recWanted = 0, recMarkHz = 0, recLastSent = 0;
  std::string recTask, recFile, recEnded;  // recEnded: why the last one stopped
};

ClassifierApp::ClassifierApp() = default;
ClassifierApp::~ClassifierApp() { stop(); }

bool ClassifierApp::start(const Config& cfg, const std::string& source, std::string* error) {
  cfg_ = cfg;
  source_ = source;
  if (!store_.open(cfg.dbPath, error)) return false;
  store_.beginSession(std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count(),
                      source, 0, 0);
  web_.handle("/", [](const HttpRequest&) {
    HttpResponse r;
    r.contentType = "text/html; charset=utf-8";
    r.body = uiPage();
    return r;
  });
  web_.handle("/icon.png", [](const HttpRequest&) {
    HttpResponse r;
    r.contentType = "image/png";
    r.body = uiIcon();
    return r;
  });
  web_.handle("/api/status", [this](const HttpRequest&) { return json(statusJson()); });
  web_.handle("/api/emissions", [this](const HttpRequest& q) { return apiEmissions(q); });
  web_.handle("/api/decodes", [this](const HttpRequest& q) { return apiDecodes(q); });
  web_.handle("/export.csv", [this](const HttpRequest& q) { return apiExport(q, true); });
  web_.handle("/export.json", [this](const HttpRequest& q) { return apiExport(q, false); });
  web_.handle("/pictures/", [this](const HttpRequest& q) {
    std::string folder;
    {
      std::lock_guard<std::mutex> l(sideMu_);
      folder = picturesFolder_;
    }
    return servedFile(folder, q.path.substr(std::min<std::size_t>(q.path.size(), 10)), ".png", "image/png");
  });
  web_.handle("/api/markers", [this](const HttpRequest& q) { return apiMarkers(q); });
  web_.handle("/api/databases", [this](const HttpRequest&) { return json(databasesJson()); });
  web_.handle("/api/tasks", [this](const HttpRequest&) { return json(tasksJson()); });
  web_.handle("/api/my_markers", [this](const HttpRequest&) { return json(myMarkersJson()); });
  web_.handle("/api/windows", [this](const HttpRequest&) { return json(windowsJson()); });
  web_.handle("/api/usage", [this](const HttpRequest&) { return json(usageJson()); });
  web_.handle("/reports/", [this](const HttpRequest& q) {
    std::string folder;
    {
      std::lock_guard<std::mutex> l(sideMu_);
      folder = reportsFolder_;
    }
    const std::string name = q.path.substr(std::min<std::size_t>(q.path.size(), 9));
    HttpResponse r = servedFile(folder, name, ".zip", "application/zip");
    if (r.status == 200) r.extraHeaders = "Content-Disposition: attachment; filename=\"" + name + "\"\r\n";
    return r;
  });
  {
    std::lock_guard<std::mutex> l(sideMu_);
    if (!frequencies_) frequencies_ = cfg.pipeline.decoders.frequencies;
  }
  web_.websocket("/ws", [this](int c) { onWsOpen(c); }, [this](int c, const std::string& t) { onWsMessage(c, t); });
  if (!web_.start(cfg.httpAddress, cfg.httpPort, error)) {
    store_.close();
    return false;
  }
  started_ = true;
  reloadRules();  // the user's windows, before the first receiver
  return true;
}

void ClassifierApp::stop() {
  if (!started_) return;
  started_ = false;
  if (downloadThread_.joinable()) downloadThread_.join();
  if (diagnosticsThread_.joinable()) diagnosticsThread_.join();
  {
    std::lock_guard<std::mutex> l(rxMu_);
    for (auto& [id, r] : rx_) {
      {
        std::lock_guard<std::mutex> lk(r->m);
        r->stop = true;
      }
      r->cv.notify_all();
    }
  }
  for (auto& [id, r] : rx_)
    if (r->thread.joinable()) r->thread.join();
  web_.stop();
  std::lock_guard<std::mutex> l(storeMu_);
  store_.endSession(std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count());
  store_.close();
}

void ClassifierApp::setReceiver(int rx, const std::string& name, bool controllable) {
  {
    std::lock_guard<std::mutex> l(rxMu_);
    addReceiverLocked(rx, name, controllable);
  }
  web_.broadcastText(statusJson());
}

void ClassifierApp::addReceiverLocked(int rx, const std::string& name, bool controllable) {
  auto& r = rx_[rx];
  if (!r) {
    r = std::make_unique<Receiver>();
    r->rx = rx;
    r->pipeline = std::make_unique<Pipeline>(cfg_.pipeline);
    Receiver* rp = r.get();
    rp->pipeline->setEmissionCallback([this, rp](Pipeline::EmissionEvent k, const Emission& e) { onEmission(*rp, k, e); });
    rp->pipeline->setDecodeCallback([this, rp](const DecodeEvent& d) { onDecode(*rp, d); });
    rp->pipeline->setFeaturesCallback([this, rp](uint64_t id, const EmissionFeatures& f) { onFeatures(*rp, id, f); });
    rp->pipeline->setWaterfallCallback([this, rp](const WaterfallRow& row) { onWaterfall(*rp, row); });
    rp->thread = std::thread([this, rp] { run(*rp); });
  }
  r->name = name;
  r->controllable = controllable;
}

void ClassifierApp::feed(int rx, IqInput block) {
  Receiver* r;
  {
    std::lock_guard<std::mutex> l(rxMu_);
    auto it = rx_.find(rx);
    if (it == rx_.end()) return;
    r = it->second.get();
  }
  if (block.sampleRate <= 0 || block.iq.empty()) return;
  const double secs = static_cast<double>(block.iq.size() / 2) / block.sampleRate;
  {
    std::lock_guard<std::mutex> l(r->m);
    if (r->queuedSeconds + secs > cfg_.maxQueueSeconds) {
      dropped_ += block.iq.size() / 2;
      r->droppedSeconds += secs;
      return;
    }
    r->queuedSeconds += secs;
    r->q.push_back(std::move(block));
  }
  r->cv.notify_all();
}

void ClassifierApp::drain() {
  std::vector<Receiver*> rs;
  {
    std::lock_guard<std::mutex> l(rxMu_);
    for (auto& [id, r] : rx_) rs.push_back(r.get());
  }
  for (Receiver* r : rs) {
    std::unique_lock<std::mutex> l(r->m);
    r->idle.wait(l, [&] { return (r->q.empty() && !r->busy && !r->flush) || r->stop; });
  }
}

void ClassifierApp::flushReceivers() {
  {
    std::lock_guard<std::mutex> l(rxMu_);
    for (auto& [id, r] : rx_) {
      {
        std::lock_guard<std::mutex> lk(r->m);
        r->flush = true;
      }
      r->cv.notify_all();
    }
  }
  drain();
}

void ClassifierApp::run(Receiver& r) {
  std::unique_lock<std::mutex> l(r.m);
  for (;;) {
    r.cv.wait_for(l, std::chrono::milliseconds(250), [&] { return r.stop || r.flush || !r.q.empty(); });
    if (r.stop) break;
    if (r.q.empty() && !r.flush) {
      // Idle: deliver FT8/FT4 results that finished in the meantime.
      l.unlock();
      r.pipeline->poll();
      reportUsage(r);
      l.lock();
      continue;
    }
    if (!r.q.empty()) {
      IqInput b = std::move(r.q.front());
      r.q.pop_front();
      r.queuedSeconds = std::max(0.0, r.queuedSeconds - static_cast<double>(b.iq.size() / 2) / b.sampleRate);
      r.busy = true;
      const auto blockStart = std::chrono::steady_clock::now();
      std::shared_ptr<const FrequencyList> list = std::move(r.newList);
      r.newList.reset();
      std::unique_ptr<DecoderHost::Config> policy = std::move(r.newPolicy);
      std::vector<uint64_t> ignore;
      ignore.swap(r.ignore);
      Receiver::RecordRequest req = std::move(r.recReq);
      r.recReq = {};
      l.unlock();
      if (list) r.pipeline->setFrequencies(list);
      if (policy) r.pipeline->setDecoderPolicy(*policy);
      for (uint64_t id : ignore) r.pipeline->ignore(id);
      if (req.stop) stopRecording(r, "stopped");
      if (req.start && !r.writer.isOpen()) {
        SigmfWriter::Global g;
        g.sampleRate = b.sampleRate;
        g.hardware = r.name;
        g.recorder = "hf_classifier";
        g.description = "task: " + req.task + (req.note.empty() ? "" : "; " + req.note);
        std::string err;
        std::lock_guard<std::mutex> lr(r.recMu);
        if (r.writer.open(req.base, g, &err)) {
          r.recording = true;
          r.recSeconds = 0;
          r.recWanted = req.seconds;
          r.recMarkHz = req.markHz;
          r.recTask = req.task;
          r.recFile = req.base + ".sigmf-data";
          r.recEnded.clear();
          r.recLastSent = -1;
        } else {
          r.recEnded = "cannot_write";
        }
      }
      if (r.writer.isOpen() && b.sampleRate != r.rate && r.rate > 0) stopRecording(r, "rate_changed");
      recordBlock(r, b);
      if (b.sampleRate != r.rate || b.centreHz != r.centre) {
        {
          std::lock_guard<std::mutex> lr(rxMu_);  // statusJson() reads them
          r.rate = b.sampleRate;
          r.centre = b.centreHz;
        }
        r.pipeline->configure(b.sampleRate, b.centreHz, b.utc);
        if (r.rx == 0) {
          std::lock_guard<std::mutex> ls(storeMu_);
          store_.setSessionTuning(b.sampleRate, b.centreHz);
        }
        web_.broadcastText(statusJson());
      }
      r.pipeline->process(b.iq.data(), b.iq.size() / 2, b.utc);
      r.pipeline->poll();
      r.busySeconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - blockStart).count();
      reportUsage(r);
      l.lock();
      r.busy = false;
    } else {
      r.busy = true;
      l.unlock();
      r.pipeline->flush();
      l.lock();
      r.busy = false;
      r.flush = false;
    }
    if (r.q.empty() && !r.flush) r.idle.notify_all();
  }
  l.unlock();
  stopRecording(r, "stopped");
  r.pipeline->flush();
  r.idle.notify_all();
}

// ------------------------------------------------------------ recording ----

void ClassifierApp::recordBlock(Receiver& r, const IqInput& b) {
  if (!r.writer.isOpen()) return;
  const bool first = r.writer.samplesWritten() == 0;
  const bool ok = r.writer.write(b.iq.data(), b.iq.size() / 2, first || b.centreHz != r.centre, b.centreHz, b.utc);
  bool send = false, done = false;
  {
    std::lock_guard<std::mutex> l(r.recMu);
    r.recSeconds = static_cast<double>(r.writer.samplesWritten()) / b.sampleRate;
    done = !ok || r.recSeconds >= r.recWanted;
    send = r.recSeconds - r.recLastSent >= 1.0;
    if (send) r.recLastSent = r.recSeconds;
  }
  if (done) stopRecording(r, ok ? "done" : "cannot_write");
  else if (send) web_.broadcastText(recordingJson(r));
}

void ClassifierApp::stopRecording(Receiver& r, const char* why) {
  if (!r.writer.isOpen()) return;
  // The frequency the recording was made for, over all of it.
  if (r.recMarkHz > 0) r.writer.annotate(0, r.writer.samplesWritten(), r.recTask, "wanted near " + num(r.recMarkHz, 0) + " Hz");
  std::string err;
  const bool ok = r.writer.close(&err);
  {
    std::lock_guard<std::mutex> l(r.recMu);
    r.recording = false;
    r.recEnded = ok ? why : "cannot_write";
  }
  web_.broadcastText(recordingJson(r));
}

std::string ClassifierApp::recordingJson(const Receiver& r) const {
  std::lock_guard<std::mutex> l(r.recMu);
  return std::string("{\"type\":\"recording\",\"rx\":") + std::to_string(r.rx) + ",\"active\":" +
         (r.recording ? "true" : "false") + ",\"seconds\":" + num(r.recSeconds, 1) + ",\"of\":" + num(r.recWanted, 0) +
         ",\"task\":" + jsonQuote(r.recTask) + ",\"file\":" + jsonQuote(r.recFile) + ",\"ended\":" +
         jsonQuote(r.recEnded) + "}";
}

namespace {

// Only what is safe in a file name.
std::string fileWord(const std::string& s) {
  std::string out;
  for (char c : s)
    if (std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_') out += c;
  return out.empty() ? "recording" : out.substr(0, 40);
}

}  // namespace

std::string ClassifierApp::record(int rx, const std::vector<JsonField>& a) {
  std::string folder;
  {
    std::lock_guard<std::mutex> l(sideMu_);
    if (recordingsFolder_.empty() || !recordingAllowed_) return "not_available";
    folder = recordingsFolder_;
  }
  Receiver* r;
  {
    std::lock_guard<std::mutex> l(rxMu_);
    auto it = rx_.find(rx);
    if (it == rx_.end()) return "no_receiver";
    r = it->second.get();
  }
  if (jsonField(a, "stop") == "true" || jsonField(a, "stop") == "1") {
    std::lock_guard<std::mutex> l(r->m);
    r->recReq.stop = true;
    return "";
  }
  double rate, centre;
  {
    std::lock_guard<std::mutex> l(rxMu_);
    rate = r->rate;
    centre = r->centre;
  }
  if (!(rate > 0)) return "no_signal";
  {
    std::lock_guard<std::mutex> l(r->recMu);
    if (r->recording) return "already_recording";
  }
  double seconds = std::atof(jsonField(a, "seconds").c_str());
  if (!(seconds >= 5)) seconds = 120;
  // At most 2 GB a recording (4 bytes a sample pair).
  seconds = std::min({seconds, 3600.0, 2e9 / (rate * 4)});
  std::error_code ec;
  std::filesystem::create_directories(std::filesystem::u8path(folder), ec);
  const double now = std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
  std::string stamp = isoUtc(now).substr(0, 19);  // 2026-11-14T15:01:02
  for (char& c : stamp)
    if (c == ':') c = '-';
  const std::string task = fileWord(jsonField(a, "task"));
  const double mark = std::atof(jsonField(a, "freq").c_str());
  std::lock_guard<std::mutex> l(r->m);
  r->recReq.start = true;
  r->recReq.stop = false;
  r->recReq.seconds = seconds;
  r->recReq.markHz = mark;
  r->recReq.task = task;
  r->recReq.note = jsonField(a, "note").substr(0, 200);
  r->recReq.base = (std::filesystem::u8path(folder) /
                    (stamp + "_" + task + "_" + num((mark > 0 ? mark : centre) / 1000.0, 1) + "kHz"))
                       .u8string();
  return "";
}

// ------------------------------------------------------------- usage ----

void ClassifierApp::reportUsage(Receiver& r) {
  const auto now = std::chrono::steady_clock::now();
  const double wall = std::chrono::duration<double>(now - r.lastUsageAt).count();
  if (wall < 2.0) return;
  double iq = 0;
  const DecoderHost::Usage u = r.pipeline->usage(&iq);
  // Shares of one processor core over the interval, largest first.
  std::vector<std::pair<std::string, double>> items;
  double total = 0;
  for (const auto& [name, sec] : u.seconds) {
    auto prev = r.lastUsage.seconds.find(name);
    const double d = sec - (prev == r.lastUsage.seconds.end() ? 0 : prev->second);
    if (d <= 0) continue;
    items.emplace_back(name, 100.0 * d / wall);
    total += d;
  }
  std::sort(items.begin(), items.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
  const double iqDelta = iq - r.lastIq;
  double dropped;
  {
    std::lock_guard<std::mutex> l(r.m);
    dropped = r.droppedSeconds;
  }
  // "dropped": seconds of IQ dropped so far because the processing fell
  // behind (the UI warns while it grows). "busy": % of the interval the
  // receiver's thread spent processing (the decoders' threads besides): at
  // 100 % it cannot keep up. "cpu" adds up all threads, so can pass 100 %.
  const double busy = r.busySeconds - r.lastBusy;
  r.lastBusy = r.busySeconds;
  std::string j = "{\"type\":\"usage\",\"rx\":" + std::to_string(r.rx) + ",\"cpu\":" + num(100.0 * total / wall, 1) +
                  ",\"load\":" + num(iqDelta > 0 ? 100.0 * total / iqDelta : 0, 1) + ",\"busy\":" + num(100.0 * busy / wall, 1) +
                  ",\"dropped\":" + num(dropped, 1) + ",\"items\":[";
  bool first = true;
  for (const auto& [name, pct] : items) {
    j += std::string(first ? "" : ",") + "{\"name\":" + jsonQuote(name) + ",\"pct\":" + num(pct, 2) + "}";
    first = false;
  }
  int decoding = 0, trying = 0, measuring = 0;
  std::map<uint64_t, double> cpuNow;
  j += "],\"sessions\":[";
  first = true;
  for (const auto& si : u.sessions) {
    (si.state == "decoding" ? decoding : si.state == "trying" ? trying : measuring) += 1;
    auto prev = r.lastSessionCpu.find(si.emissionId);
    const double d = si.cpuSeconds - (prev == r.lastSessionCpu.end() ? 0 : prev->second);
    cpuNow[si.emissionId] = si.cpuSeconds;
    j += std::string(first ? "" : ",") + "{\"id\":" + std::to_string(si.emissionId) + ",\"centre\":" + num(si.centreHz, 0) +
         ",\"width\":" + num(si.widthHz, 0) + ",\"snr\":" + num(si.snrDb, 1) + ",\"state\":" + jsonQuote(si.state) +
         ",\"decoder\":" + jsonQuote(si.decoder) + ",\"age\":" + num(si.ageSeconds, 0) + ",\"pct\":" + num(100.0 * std::max(0.0, d) / wall, 2) +
         "}";
    first = false;
  }
  j += "],\"counts\":{\"decoding\":" + std::to_string(decoding) + ",\"trying\":" + std::to_string(trying) +
       ",\"measuring\":" + std::to_string(measuring) + "},\"left\":{\"given_up\":" + std::to_string(u.givenUp) +
       ",\"ignored\":" + std::to_string(u.ignored) + ",\"measured\":" + std::to_string(u.measuredOnly) + "},\"left_ids\":{";
  first = true;
  for (const auto& [id, why] : u.leftRecent) {
    j += std::string(first ? "" : ",") + "\"" + std::to_string(id) + "\":\"" + why + "\"";
    first = false;
  }
  j += "}}";
  r.lastUsage = u;
  r.lastIq = iq;
  r.lastUsageAt = now;
  r.lastSessionCpu.swap(cpuNow);
  {
    std::lock_guard<std::mutex> l(r.recMu);
    r.usageJson = j;
  }
  web_.broadcastText(j);
}

std::string ClassifierApp::usageJson() const {
  std::string out = "{\"receivers\":[";
  std::lock_guard<std::mutex> l(rxMu_);
  bool first = true;
  for (const auto& [id, r] : rx_) {
    std::lock_guard<std::mutex> lr(r->recMu);
    if (r->usageJson.empty()) continue;
    out += std::string(first ? "" : ",") + r->usageJson;
    first = false;
  }
  return out + "]}";
}

void ClassifierApp::setDecoderPolicy(const DecoderHost::Config& given) {
  std::lock_guard<std::mutex> l(rxMu_);
  DecoderHost::Config p = given;
  p.rules = cfg_.pipeline.decoders.rules;  // the user's windows are the app's
  cfg_.pipeline.decoders.maxSessions = p.maxSessions;
  cfg_.pipeline.decoders.measureUnknown = p.measureUnknown;
  cfg_.pipeline.decoders.giveUpSeconds = p.giveUpSeconds;
  cfg_.pipeline.decoders.minSnrDb = p.minSnrDb;
  cfg_.pipeline.decoders.disabled = p.disabled;
  for (auto& [id, r] : rx_) {
    std::lock_guard<std::mutex> lk(r->m);
    r->newPolicy = std::make_unique<DecoderHost::Config>(p);
  }
}

// ------------------------------------- downloads and the user's markers ----

const char* myMarkersFile() { return "my_markers.json"; }
const char* const kWindowsFile = "decode_windows.json";

namespace {

std::string pathIn(const std::string& folder, const std::string& name) {
  return (std::filesystem::u8path(folder) / name).u8string();
}

std::string readFile(const std::string& path) {
  std::ifstream in(std::filesystem::u8path(path), std::ios::binary);
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

bool writeFile(const std::string& path, const std::string& data) {
  const std::string tmp = path + ".part";
  {
    std::ofstream out(std::filesystem::u8path(tmp), std::ios::binary);
    out.write(data.data(), static_cast<std::streamsize>(data.size()));
    if (!out) return false;
  }
  std::error_code ec;
  std::filesystem::rename(std::filesystem::u8path(tmp), std::filesystem::u8path(path), ec);
  if (!ec) return true;
  // Not replaced (on Windows: the file is open elsewhere): a failure, even
  // though the leftover is removed (whose own result must not hide it).
  std::error_code rm;
  std::filesystem::remove(std::filesystem::u8path(tmp), rm);
  return false;
}

struct MyMarker {
  std::string id, label, note, demod, added;
  std::string decoder;  // "decode only with" ("" = as the program decides)
  double hz = 0;
};

// The user's windows on the waterfall (decode_windows.json in the frequency
// folder, so that it travels with the lists to a PC without internet).
struct DecodeWindow {
  std::string id, decoder, added;
  double lo = 0, hi = 0;
};

std::vector<DecodeWindow> readWindows(const std::string& path) {
  std::vector<DecodeWindow> out;
  std::vector<JsonField> doc, list;
  if (!parseJsonObject(readFile(path), doc, nullptr) || !parseJsonArray(jsonField(doc, "windows"), list, nullptr)) return out;
  for (const auto& e : list) {
    std::vector<JsonField> f;
    if (!parseJsonObject(e.value, f, nullptr)) continue;
    DecodeWindow w;
    w.id = jsonField(f, "id");
    w.lo = std::atof(jsonField(f, "lo_hz").c_str());
    w.hi = std::atof(jsonField(f, "hi_hz").c_str());
    w.decoder = jsonField(f, "decoder");
    w.added = jsonField(f, "added");
    if (!w.id.empty() && w.hi > w.lo && (w.decoder == "none" || DecoderHost::choosable(w.decoder))) out.push_back(w);
  }
  return out;
}

std::string windowsDoc(const std::vector<DecodeWindow>& ws, bool pretty) {
  std::string s = pretty ? "{\"windows\":[\n" : "{\"windows\":[";
  for (std::size_t i = 0; i < ws.size(); ++i) {
    const DecodeWindow& w = ws[i];
    s += std::string(i ? (pretty ? ",\n" : ",") : "") + "{\"id\":" + jsonQuote(w.id) + ",\"lo_hz\":" + num(w.lo, 0) +
         ",\"hi_hz\":" + num(w.hi, 0) + ",\"decoder\":" + jsonQuote(w.decoder) + ",\"added\":" + jsonQuote(w.added) + "}";
  }
  return s + (pretty ? "\n]" : "]");
}

std::vector<MyMarker> readMarkers(const std::string& path) {
  std::vector<MyMarker> out;
  std::vector<JsonField> doc, list;
  if (!parseJsonObject(readFile(path), doc, nullptr) || !parseJsonArray(jsonField(doc, "markers"), list, nullptr)) return out;
  for (const auto& e : list) {
    std::vector<JsonField> f;
    if (!parseJsonObject(e.value, f, nullptr)) continue;
    MyMarker m;
    m.id = jsonField(f, "id");
    m.hz = std::atof(jsonField(f, "hz").c_str());
    m.label = jsonField(f, "label");
    m.note = jsonField(f, "note");
    m.demod = jsonField(f, "demod");
    m.added = jsonField(f, "added");
    m.decoder = jsonField(f, "decoder");
    if (!m.decoder.empty() && !DecoderHost::choosable(m.decoder)) m.decoder.clear();
    if (!m.id.empty() && m.hz > 0) out.push_back(m);
  }
  return out;
}

std::string markersDoc(const std::vector<MyMarker>& ms) {
  std::string s = "{\"markers\":[\n";
  for (std::size_t i = 0; i < ms.size(); ++i) {
    const MyMarker& m = ms[i];
    s += std::string(i ? ",\n" : "") + "{\"id\":" + jsonQuote(m.id) + ",\"hz\":" + num(m.hz, 0) + ",\"label\":" + jsonQuote(m.label) +
         ",\"note\":" + jsonQuote(m.note) + ",\"demod\":" + jsonQuote(m.demod) + ",\"added\":" + jsonQuote(m.added) +
         ",\"decoder\":" + jsonQuote(m.decoder) + "}";
  }
  return s + "\n]}\n";
}

}  // namespace

void addMyMarkers(const std::string& path, FrequencyList& list) {
  for (const auto& m : readMarkers(path))
    list.addEntry(m.hz, m.note.empty() ? m.label : m.label + " - " + m.note, "my markers", m.demod);
}

std::vector<ClassifierApp::Download> ClassifierApp::defaultDownloads(double unixTime) {
  // EiBi's seasons: A from the last Sunday of March, B from the last Sunday
  // of October (B26: winter 2026/27). Roughly: April to October is A.
  const std::time_t t = static_cast<std::time_t>(unixTime);
  std::tm tm{};
#ifdef _WIN32
  gmtime_s(&tm, &t);
#else
  gmtime_r(&t, &tm);
#endif
  const int year = tm.tm_year + 1900, month = tm.tm_mon + 1;
  char season = month >= 4 && month <= 10 ? 'a' : 'b';
  int yy = (month <= 3 ? year - 1 : year) % 100;
  char cur[32], prev[32];
  std::snprintf(cur, sizeof cur, "sked-%c%02d.csv", season, yy);
  if (season == 'a') std::snprintf(prev, sizeof prev, "sked-b%02d.csv", (yy + 99) % 100);
  else std::snprintf(prev, sizeof prev, "sked-a%02d.csv", yy);
  return {
      {"KiwiSDR community labels", "http://kiwisdr.com/dx/dx_community.json", "dx_community.json", 100, "", ""},
      {"KiwiSDR label categories", "http://kiwisdr.com/dx/dx_community_config.json", "dx_community_config.json", 0, "", ""},
      {"EiBi schedule", std::string("http://www.eibispace.de/dx/") + cur, cur, 1000, std::string("http://www.eibispace.de/dx/") + prev, prev},
  };
}

void ClassifierApp::setDownloads(std::vector<Download> d) {
  std::lock_guard<std::mutex> l(sideMu_);
  downloads_ = std::move(d);
}

void ClassifierApp::reloadDatabases() {
  std::function<std::shared_ptr<const FrequencyList>()> reload;
  {
    std::lock_guard<std::mutex> l(sideMu_);
    reload = frequencyReload_;
  }
  reloadRules();
  if (!reload) return;
  setFrequencies(reload());
  web_.broadcastText(databasesJson());
}

void ClassifierApp::runDownloads() {
  std::vector<Download> list;
  std::string folder;
  {
    std::lock_guard<std::mutex> l(sideMu_);
    list = downloads_.empty() ? defaultDownloads(unixNowSeconds()) : downloads_;
    folder = frequenciesFolder_;
  }
  std::error_code ec;
  std::filesystem::create_directories(std::filesystem::u8path(folder), ec);
  std::string items;
  auto progress = [&](const char* state) {
    web_.broadcastText(std::string("{\"type\":\"download\",\"state\":\"") + state + "\",\"items\":[" + items + "]}");
  };
  progress("running");
  bool any = false;
  for (const Download& d : list) {
    std::string file = d.file, error;
    std::size_t entries = 0;
    HttpResult r = httpGet(d.url);
    if (!r.error.empty() && !d.fallbackUrl.empty()) {
      HttpResult f = httpGet(d.fallbackUrl);
      if (f.error.empty()) r = f, file = d.fallbackFile;
    }
    if (!r.error.empty()) {
      error = r.error;
    } else if (d.minEntries > 0) {
      // Kept only if it reads as such a list (a server's error page does not).
      FrequencyList probe;
      entries = probe.addText(r.body, file);
      if (entries < d.minEntries) error = "not_a_list";
    }
    if (error.empty() && !writeFile(pathIn(folder, file), r.body)) error = "cannot_write";
    if (error.empty()) any = true;
    items += std::string(items.empty() ? "" : ",") + "{\"name\":" + jsonQuote(d.name) + ",\"file\":" + jsonQuote(file) +
             ",\"ok\":" + (error.empty() ? "true" : "false") + ",\"entries\":" + std::to_string(entries) +
             ",\"bytes\":" + std::to_string(r.body.size()) + ",\"error\":" + jsonQuote(error) + "}";
    std::printf("download %s (%s): %s\n", d.name.c_str(), r.finalUrl.c_str(), error.empty() ? "ok" : error.c_str());
    std::fflush(stdout);
    progress("running");
  }
  if (any) reloadDatabases();
  progress("done");
  downloading_ = false;
}

std::string ClassifierApp::myMarkersJson() const {
  std::string folder;
  {
    std::lock_guard<std::mutex> l(sideMu_);
    folder = frequenciesFolder_;
  }
  if (folder.empty()) return "{\"markers\":[]}";
  std::string s = "{\"markers\":[";
  bool first = true;
  for (const auto& m : readMarkers(pathIn(folder, myMarkersFile()))) {
    s += std::string(first ? "" : ",") + "{\"id\":" + jsonQuote(m.id) + ",\"hz\":" + num(m.hz, 0) + ",\"label\":" + jsonQuote(m.label) +
         ",\"note\":" + jsonQuote(m.note) + ",\"demod\":" + jsonQuote(m.demod) + ",\"added\":" + jsonQuote(m.added) +
         ",\"decoder\":" + jsonQuote(m.decoder) + "}";
    first = false;
  }
  return s + "]}";
}

// The user's windows and markers with a decoder, as the receivers' rules.
void ClassifierApp::reloadRules() {
  std::string folder;
  {
    std::lock_guard<std::mutex> l(sideMu_);
    folder = frequenciesFolder_;
  }
  auto rules = std::make_shared<std::vector<DecodeRule>>();
  if (!folder.empty()) {
    for (const auto& w : readWindows(pathIn(folder, kWindowsFile))) rules->push_back({w.lo, w.hi, w.decoder});
    // A marker's: its frequency +- 1.5 kHz (it may be the signal's centre or
    // its dial frequency).
    for (const auto& m : readMarkers(pathIn(folder, myMarkersFile())))
      if (!m.decoder.empty()) rules->push_back({m.hz - 1500.0, m.hz + 1500.0, m.decoder});
  }
  std::lock_guard<std::mutex> l(rxMu_);
  const auto& old = cfg_.pipeline.decoders.rules;
  bool same = old && old->size() == rules->size();
  for (std::size_t i = 0; same && i < rules->size(); ++i)
    same = (*old)[i].loHz == (*rules)[i].loHz && (*old)[i].hiHz == (*rules)[i].hiHz && (*old)[i].decoder == (*rules)[i].decoder;
  if (same || (!old && rules->empty())) return;
  cfg_.pipeline.decoders.rules = rules;
  for (auto& [id, r] : rx_) {
    std::lock_guard<std::mutex> lk(r->m);
    auto p = r->newPolicy ? std::make_unique<DecoderHost::Config>(*r->newPolicy) : std::make_unique<DecoderHost::Config>(cfg_.pipeline.decoders);
    p->rules = rules;
    r->newPolicy = std::move(p);
  }
}

std::string ClassifierApp::windowsJson() const {
  std::string folder;
  {
    std::lock_guard<std::mutex> l(sideMu_);
    folder = frequenciesFolder_;
  }
  // (windowsDoc opens the object with its "windows" list.)
  std::string s = windowsDoc(folder.empty() ? std::vector<DecodeWindow>() : readWindows(pathIn(folder, kWindowsFile)), false) +
                  ",\"decoders\":[";
  bool first = true;
  for (const auto& d : DecoderHost::chosenDecoders()) {
    s += std::string(first ? "" : ",") + jsonQuote(d);
    first = false;
  }
  return s + "],\"can_save\":" + (folder.empty() ? "false" : "true") + "}";
}

std::string ClassifierApp::saveWindow(const std::vector<JsonField>& a) {
  std::string folder;
  {
    std::lock_guard<std::mutex> l(sideMu_);
    folder = frequenciesFolder_;
  }
  if (folder.empty()) return "not_available";
  const double lo = std::atof(jsonField(a, "lo_hz").c_str()), hi = std::atof(jsonField(a, "hi_hz").c_str());
  const std::string decoder = jsonField(a, "decoder"), id = jsonField(a, "id");
  if (!(lo >= 1e3 && hi > lo + 20 && hi <= 3e9 && hi - lo <= 2e6)) return "bad_window";
  if (decoder != "none" && !DecoderHost::choosable(decoder)) return "unknown_decoder";
  {
    std::lock_guard<std::mutex> l(markersMu_);
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::u8path(folder), ec);
    const std::string path = pathIn(folder, kWindowsFile);
    auto ws = readWindows(path);
    DecodeWindow* w = nullptr;
    for (auto& x : ws)
      if (!id.empty() && x.id == id) w = &x;
    if (!id.empty() && !w) return "no_such_window";
    if (!w) {
      ws.push_back({});
      w = &ws.back();
      w->id = "w" + std::to_string(static_cast<long long>(unixNowSeconds() * 1000));
      for (const auto& x : ws)
        if (&x != w && x.id == w->id) w->id += "x";
      w->added = isoUtc(unixNowSeconds()).substr(0, 19) + "Z";
    }
    w->lo = std::round(lo);
    w->hi = std::round(hi);
    w->decoder = decoder;
    if (!writeFile(path, windowsDoc(ws, true) + "}\n")) return "cannot_write";
  }
  reloadRules();
  web_.broadcastText("{\"type\":\"windows\"}");
  return "";
}

std::string ClassifierApp::deleteWindow(const std::string& id) {
  std::string folder;
  {
    std::lock_guard<std::mutex> l(sideMu_);
    folder = frequenciesFolder_;
  }
  if (folder.empty()) return "not_available";
  {
    std::lock_guard<std::mutex> l(markersMu_);
    const std::string path = pathIn(folder, kWindowsFile);
    auto ws = readWindows(path);
    const auto it = std::find_if(ws.begin(), ws.end(), [&](const DecodeWindow& w) { return w.id == id; });
    if (it == ws.end()) return "no_such_window";
    ws.erase(it);
    if (!writeFile(path, windowsDoc(ws, true) + "}\n")) return "cannot_write";
  }
  reloadRules();
  web_.broadcastText("{\"type\":\"windows\"}");
  return "";
}

std::string ClassifierApp::saveMarker(const std::vector<JsonField>& a) {
  std::string folder;
  {
    std::lock_guard<std::mutex> l(sideMu_);
    folder = frequenciesFolder_;
  }
  if (folder.empty()) return "not_available";
  const double hz = std::atof(jsonField(a, "hz").c_str());
  std::string label = jsonField(a, "label"), note = jsonField(a, "note"), demod = jsonField(a, "demod"), id = jsonField(a, "id");
  const std::string decoder = jsonField(a, "decoder");
  if (!decoder.empty() && !DecoderHost::choosable(decoder)) return "unknown_decoder";
  if (!(hz >= 3e3 && hz <= 3e9)) return "bad_frequency";
  if (label.empty()) return "no_label";
  if (label.size() > 80 || note.size() > 300) return "too_long";
  if (!demod.empty() && demod != "USB" && demod != "LSB" && demod != "CW" && demod != "AM") return "unknown_mode";
  {
    std::lock_guard<std::mutex> l(markersMu_);
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::u8path(folder), ec);
    const std::string path = pathIn(folder, myMarkersFile());
    auto ms = readMarkers(path);
    MyMarker* m = nullptr;
    for (auto& x : ms)
      if (!id.empty() && x.id == id) m = &x;
    if (!id.empty() && !m) return "no_such_marker";
    if (!m) {
      ms.push_back({});
      m = &ms.back();
      m->id = "m" + std::to_string(static_cast<long long>(unixNowSeconds() * 1000));
      for (const auto& x : ms)
        if (&x != m && x.id == m->id) m->id += "x";
      m->added = isoUtc(unixNowSeconds()).substr(0, 19) + "Z";
    }
    m->hz = std::round(hz);
    m->label = label;
    m->note = note;
    m->demod = demod;
    m->decoder = decoder;
    if (!writeFile(path, markersDoc(ms))) return "cannot_write";
  }
  reloadDatabases();  // (and the windows: a marker's decoder is one)
  web_.broadcastText("{\"type\":\"my_markers\"}");
  return "";
}

std::string ClassifierApp::deleteMarker(const std::string& id) {
  std::string folder;
  {
    std::lock_guard<std::mutex> l(sideMu_);
    folder = frequenciesFolder_;
  }
  if (folder.empty()) return "not_available";
  {
    std::lock_guard<std::mutex> l(markersMu_);
    const std::string path = pathIn(folder, myMarkersFile());
    auto ms = readMarkers(path);
    const std::size_t before = ms.size();
    ms.erase(std::remove_if(ms.begin(), ms.end(), [&](const MyMarker& m) { return m.id == id; }), ms.end());
    if (ms.size() == before) return "no_such_marker";
    if (!writeFile(path, markersDoc(ms))) return "cannot_write";
  }
  reloadDatabases();
  web_.broadcastText("{\"type\":\"my_markers\"}");
  return "";
}

// ------------------------------------------- frequency lists and tasks ----

std::shared_ptr<const FrequencyList> ClassifierApp::frequencies() const {
  std::lock_guard<std::mutex> l(sideMu_);
  return frequencies_;
}

void ClassifierApp::setFrequencies(std::shared_ptr<const FrequencyList> list) {
  {
    std::lock_guard<std::mutex> l(sideMu_);
    frequencies_ = list;
  }
  std::lock_guard<std::mutex> l(rxMu_);
  cfg_.pipeline.decoders.frequencies = list;  // for receivers declared later
  for (auto& [id, r] : rx_) {
    std::lock_guard<std::mutex> lk(r->m);
    r->newList = list;
  }
}

void ClassifierApp::setFrequencyReload(std::function<std::shared_ptr<const FrequencyList>()> reload,
                                       const std::string& folder) {
  std::lock_guard<std::mutex> l(sideMu_);
  frequencyReload_ = std::move(reload);
  frequenciesFolder_ = folder;
}

void ClassifierApp::setRecordings(const std::string& folder, bool allowed) {
  std::lock_guard<std::mutex> l(sideMu_);
  recordingsFolder_ = folder;
  recordingAllowed_ = allowed;
}

void ClassifierApp::setPictures(const std::string& folder) {
  std::lock_guard<std::mutex> l(sideMu_);
  picturesFolder_ = folder;
}

void ClassifierApp::setFolderOpener(std::function<void(const std::string&)> open) {
  std::lock_guard<std::mutex> l(sideMu_);
  openFolder_ = std::move(open);
}

void ClassifierApp::setDiagnostics(DiagnosticsFn make, const std::string& folder) {
  std::lock_guard<std::mutex> l(sideMu_);
  diagnostics_ = std::move(make);
  reportsFolder_ = folder;
}

void ClassifierApp::runDiagnostics(bool rates) {
  DiagnosticsFn make;
  {
    std::lock_guard<std::mutex> l(sideMu_);
    make = diagnostics_;
  }
  web_.broadcastText("{\"type\":\"diagnostics\",\"state\":\"running\",\"rates\":" + std::string(rates ? "true" : "false") + "}");
  std::string error;
  const std::string path = make(rates, [this](const std::string& line) {
    web_.broadcastText("{\"type\":\"diagnostics\",\"state\":\"line\",\"line\":" + jsonQuote(line) + "}");
  }, &error);
  if (path.empty()) {
    web_.broadcastText("{\"type\":\"diagnostics\",\"state\":\"failed\",\"message\":" + jsonQuote(error) + "}");
  } else {
    const std::string name = std::filesystem::u8path(path).filename().u8string();
    web_.broadcastText("{\"type\":\"diagnostics\",\"state\":\"done\",\"file\":" + jsonQuote(name) +
                       ",\"path\":" + jsonQuote(path) + "}");
  }
  diagnosing_ = false;
}

namespace {

std::string markerJson(const ListedFrequency& e, double now) {
  return "{\"hz\":" + num(e.hz, 0) + ",\"label\":" + jsonQuote(e.label) + ",\"mode\":" + jsonQuote(e.mode) +
         ",\"source\":" + jsonQuote(e.source) + ",\"demod\":" + jsonQuote(e.demod) + ",\"on\":" +
         (e.onAir(now) ? "true" : "false") + (e.startMin >= 0 ? ",\"start\":" + std::to_string(e.startMin) + ",\"stop\":" +
                                                                  std::to_string(e.stopMin)
                                                            : std::string()) +
         (e.category >= 0 ? ",\"cat\":" + std::to_string(e.category) : std::string()) + "}";
}

std::string lowered(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

// `word` in `text`, not as part of a longer word ("fax" in "Halifax" is not).
bool hasWord(const std::string& text, const std::string& word) {
  auto alnum = [](char c) { return std::isalnum(static_cast<unsigned char>(c)) != 0; };
  for (std::size_t at = text.find(word); at != std::string::npos; at = text.find(word, at + 1))
    if ((at == 0 || !alnum(text[at - 1])) && (at + word.size() >= text.size() || !alnum(text[at + word.size()]))) return true;
  return false;
}

double unixNow() { return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count(); }

}  // namespace

// /api/markers?lo_khz=&hi_khz= : the entries in a band (the waterfall's);
// ?text= : entries whose label, mode or source contains the text (the
// search of the databases window). `limit` (default 2000).
HttpResponse ClassifierApp::apiMarkers(const HttpRequest& q) {
  const auto list = frequencies();
  const double lo = param(q, "lo_khz", 0) * 1e3, hi = param(q, "hi_khz", 1e7) * 1e3, now = unixNow();
  const std::size_t limit = static_cast<std::size_t>(std::clamp(param(q, "limit", 2000), 1.0, 20000.0));
  auto it = q.query.find("text");
  const std::string text = it == q.query.end() ? "" : lowered(it->second);
  std::string out = "{\"markers\":[";
  std::size_t n = 0, more = 0;
  if (list)
    for (const auto& e : list->entries()) {
      if (e.hz < lo || e.hz > hi) continue;
      if (!text.empty() && lowered(e.label + " " + e.mode + " " + e.source).find(text) == std::string::npos) continue;
      if (n >= limit) {
        ++more;
        continue;
      }
      out += (n++ ? "," : "") + markerJson(e, now);
    }
  return json(out + "],\"more\":" + std::to_string(more) + "}");
}

std::string ClassifierApp::databasesJson() const {
  const auto list = frequencies();
  std::string folder;
  bool canReload, canOpen;
  {
    std::lock_guard<std::mutex> l(sideMu_);
    folder = frequenciesFolder_;
    canReload = static_cast<bool>(frequencyReload_);
    canOpen = static_cast<bool>(openFolder_);
  }
  // KiwiSDR's label types (dx_community_config.json, if downloaded): the
  // names and colours of the categories the markers carry.
  std::string cats;
  if (!folder.empty()) {
    std::vector<JsonField> doc, types;
    if (parseJsonObject(readFile(pathIn(folder, "dx_community_config.json")), doc, nullptr) &&
        parseJsonArray(jsonField(doc, "dx_type"), types, nullptr))
      for (const auto& t : types) {
        std::vector<JsonField> f;
        if (!parseJsonObject(t.value, f, nullptr)) continue;
        const std::string name = jsonField(f, "name"), color = jsonField(f, "color");
        // Only what a CSS colour can be made of.
        bool safe = color.size() < 40;
        for (char c : color) safe = safe && (std::isalnum(static_cast<unsigned char>(c)) || c == '(' || c == ')' || c == ',' || c == '%' || c == '#' || c == ' ' || c == '.');
        cats += std::string(cats.empty() ? "" : ",") + "{\"key\":" + std::to_string(std::atoi(jsonField(f, "key").c_str())) +
                ",\"name\":" + jsonQuote(name.substr(0, 40)) + ",\"color\":" + jsonQuote(safe ? color : "") + "}";
      }
  }
  std::string out = "{\"type\":\"databases\",\"folder\":" + jsonQuote(folder) + ",\"categories\":[" + cats + "],\"can_reload\":" +
                    (canReload ? "true" : "false") + ",\"can_open\":" + (canOpen ? "true" : "false") + ",\"databases\":[";
  bool first = true;
  if (list)
    for (const auto& [name, count] : list->sources()) {
      out += std::string(first ? "" : ",") + "{\"name\":" + jsonQuote(name) + ",\"entries\":" + std::to_string(count) + "}";
      first = false;
    }
  return out + "]}";
}

// The tasks as catalogue/recording_tasks.json defines them, each with the
// frequencies the loaded lists give for it and the recordings made so far.
std::string ClassifierApp::tasksJson() const {
  const auto list = frequencies();
  std::string folder;
  bool allowed;
  {
    std::lock_guard<std::mutex> l(sideMu_);
    folder = recordingsFolder_;
    allowed = recordingAllowed_ && !recordingsFolder_.empty();
  }
  // Recordings made: the task is the word after the time in the file name.
  std::map<std::string, int> have;
  if (!folder.empty()) {
    std::error_code ec;
    for (std::filesystem::directory_iterator it(std::filesystem::u8path(folder), ec), end; !ec && it != end; it.increment(ec)) {
      const std::string name = it->path().filename().u8string();
      const std::string ext = ".sigmf-meta";
      if (name.size() < ext.size() + 21 || name.compare(name.size() - ext.size(), ext.size(), ext) != 0) continue;
      const std::size_t a = 20, b = name.rfind('_');
      if (b != std::string::npos && b > a) ++have[name.substr(a, b - a)];
    }
  }
  const double now = unixNow();
  std::vector<JsonField> doc, tasks;
  std::string out = "{\"folder\":" + jsonQuote(folder) + ",\"can_record\":" + (allowed ? "true" : "false") + ",\"tasks\":[";
  if (parseJsonObject(recordingTasksText(), doc, nullptr) && parseJsonArray(jsonField(doc, "tasks"), tasks, nullptr)) {
    bool firstTask = true;
    for (const auto& t : tasks) {
      std::vector<JsonField> f, modes, find;
      if (!parseJsonObject(t.value, f, nullptr)) continue;
      parseJsonArray(jsonField(f, "modes"), modes, nullptr);
      parseJsonArray(jsonField(f, "find"), find, nullptr);
      const std::string id = jsonField(f, "id");
      std::string freqs;
      std::size_t n = 0, total = 0;
      if (list && (!modes.empty() || !find.empty()))
        for (const auto& e : list->entries()) {
          bool match = false;
          for (const auto& m : modes) match = match || (!e.mode.empty() && e.mode == m.value);
          if (!match && !find.empty()) {
            const std::string label = lowered(e.label);
            for (const auto& w : find) match = match || hasWord(label, lowered(w.value));
          }
          if (!match) continue;
          ++total;
          if (n < 60) freqs += (n++ ? "," : "") + markerJson(e, now);
        }
      out += std::string(firstTask ? "" : ",") + "{\"id\":" + jsonQuote(id) + ",\"group\":" + jsonQuote(jsonField(f, "group")) +
             ",\"title\":" + jsonQuote(jsonField(f, "title")) + ",\"want\":" + std::to_string(std::atoi(jsonField(f, "want").c_str())) +
             ",\"seconds\":" + std::to_string(std::atoi(jsonField(f, "seconds").c_str())) + ",\"have\":" +
             std::to_string(have.count(fileWord(id)) ? have[fileWord(id)] : 0) + ",\"listed\":" + std::to_string(total) +
             ",\"freqs\":[" + freqs + "]}";
      firstTask = false;
    }
  }
  return out + "]}";
}

void ClassifierApp::onEmission(Receiver& r, Pipeline::EmissionEvent k, const Emission& e) {
  const std::string label = r.pipeline->label(e.id);
  const char* ev = k == Pipeline::EmissionEvent::Started ? "start" : k == Pipeline::EmissionEvent::Updated ? "update" : "end";
  {
    std::lock_guard<std::mutex> l(storeMu_);
    store_.putEmission(r.rx, e, label);
  }
  // Updated and sent under r.m, as onWsOpen() takes its snapshot: a browser
  // that connects gets the snapshot and then the changes, in that order.
  std::lock_guard<std::mutex> l(r.m);
  std::string features;
  auto it = r.active.find(e.id);
  if (it != r.active.end()) features = it->second.features;
  if (k == Pipeline::EmissionEvent::Ended) {
    r.active.erase(e.id);
  } else {
    auto& a = r.active[e.id];
    a.e = e;
    a.label = label;
  }
  web_.broadcastText(emissionJson(r.rx, ev, e, label, features));
}

void ClassifierApp::onFeatures(Receiver& r, uint64_t id, const EmissionFeatures& f) {
  const std::string json = f.json();
  {
    std::lock_guard<std::mutex> l(storeMu_);
    store_.putFeatures(r.rx, id, json);
  }
  std::lock_guard<std::mutex> l(r.m);
  auto it = r.active.find(id);
  if (it == r.active.end()) return;
  it->second.features = json;
  web_.broadcastText(emissionJson(r.rx, "update", it->second.e, it->second.label, json));
}

void ClassifierApp::onDecode(Receiver& r, const DecodeEvent& event) {
  DecodeEvent d = event;
  if (d.image) {
    // The picture as a file; the text names it ("[picture NAME]"), which
    // the UI turns into the picture.
    std::string folder, name;
    {
      std::lock_guard<std::mutex> l(sideMu_);
      folder = picturesFolder_;
    }
    if (!folder.empty()) {
      std::string stamp = isoUtc(d.utc).substr(0, 19);
      for (char& c : stamp)
        if (c == ':') c = '-';
      name = std::string(d.decoder == "SSTV" ? "sstv_" : "fax_") + stamp + "_" + num(d.freqHz / 1000.0, 1) + "kHz.png";
      std::error_code ec;
      std::filesystem::create_directories(std::filesystem::u8path(folder), ec);
      std::ofstream out(std::filesystem::u8path(folder) / name, std::ios::binary);
      const std::string png = faxPng(*d.image);
      out.write(png.data(), static_cast<std::streamsize>(png.size()));
      if (!out) name.clear();
    }
    const std::size_t at = d.text.find("[picture]");
    if (at != std::string::npos) d.text.replace(at, 9, name.empty() ? "picture (not kept)" : "[picture " + name + "]");
    d.image.reset();
  }
  {
    std::lock_guard<std::mutex> l(storeMu_);
    store_.putDecode(r.rx, d);
  }
  {
    std::lock_guard<std::mutex> l(r.m);
    auto it = r.active.find(d.emissionId);
    if (it != r.active.end() && it->second.label.empty() && d.first) it->second.label = d.decoder;
  }
  web_.broadcastText(decodeJson(r.rx, d));
}

void ClassifierApp::onWaterfall(Receiver& r, const WaterfallRow& row) {
  if (web_.clients() == 0) return;
  web_.broadcastBinary(waterfallMessage(r.rx, row), true);
}

std::string ClassifierApp::statusJson() const {
  std::string s = "{\"type\":\"status\",\"version\":" + jsonQuote(HFC_VERSION) + ",\"source\":" + jsonQuote(source_) +
                  ",\"dropped\":" + std::to_string(dropped_.load());
  {
    std::lock_guard<std::mutex> l(sideMu_);
    s += ",\"mode\":" + jsonQuote(mode_) + ",\"source_state\":" + jsonQuote(sourceState_) +
         ",\"source_detail\":" + jsonQuote(sourceDetail_) + ",\"system_language\":" + jsonQuote(systemLanguage_) +
         ",\"log\":" + jsonQuote(cfg_.dbPath) + ",\"diagnostics\":" + (diagnostics_ ? "true" : "false");
    if (settingsProvider_) s += ",\"settings\":" + settingsProvider_();
  }
  s += ",\"receivers\":[";
  std::lock_guard<std::mutex> l(rxMu_);
  bool first = true;
  for (const auto& [id, r] : rx_) {
    if (!first) s += ",";
    first = false;
    s += "{\"rx\":" + std::to_string(id) + ",\"name\":" + jsonQuote(r->name) +
         ",\"controllable\":" + (r->controllable ? "true" : "false") + ",\"rate\":" + num(r->rate, 0) +
         ",\"centre\":" + num(r->centre, 0) + "}";
  }
  return s + "]}";
}

void ClassifierApp::setSourceState(const std::string& state, const std::string& detail) {
  {
    std::lock_guard<std::mutex> l(sideMu_);
    if (state == sourceState_ && detail == sourceDetail_) return;
    sourceState_ = state;
    sourceDetail_ = detail;
  }
  if (started_) web_.broadcastText(statusJson());
}

void ClassifierApp::setMode(const std::string& mode) {
  std::lock_guard<std::mutex> l(sideMu_);
  mode_ = mode;
}

void ClassifierApp::setSystemLanguage(const std::string& language) {
  std::lock_guard<std::mutex> l(sideMu_);
  systemLanguage_ = language;
}

void ClassifierApp::setSettingsHandlers(SettingsProvider provider, SettingsHandler apply, std::function<void()> reconnect) {
  std::lock_guard<std::mutex> l(sideMu_);
  settingsProvider_ = std::move(provider);
  settingsApply_ = std::move(apply);
  reconnect_ = std::move(reconnect);
}

void ClassifierApp::onWsOpen(int client) {
  web_.sendText(client, statusJson());
  {
    std::vector<std::string> rec;
    {
      std::lock_guard<std::mutex> l(rxMu_);
      for (const auto& [id, r] : rx_) rec.push_back(recordingJson(*r));
    }
    for (const auto& m : rec) web_.sendText(client, m);
  }
  // This run's emissions that have ended (the latest 300), so that the
  // table matches the decodes sent below.
  std::vector<StoredEmission> ended;
  {
    std::lock_guard<std::mutex> l(storeMu_);
    Query q;
    q.session = store_.session();
    q.limit = 300;
    ended = store_.emissions(q);
  }
  for (auto it = ended.rbegin(); it != ended.rend(); ++it) {
    if (!it->ended) continue;  // those on the air come with the snapshot below
    Emission e;
    e.id = it->id;
    e.startUtc = it->startUtc;
    e.endUtc = it->endUtc;
    e.loHz = it->loHz;
    e.hiHz = it->hiHz;
    e.centreHz = it->centreHz;
    e.peakSnrDb = static_cast<float>(it->peakSnrDb);
    e.meanSnrDb = static_cast<float>(it->meanSnrDb);
    e.duty = it->duty;
    e.driftHzPerSecond = it->driftHzPerSecond;
    web_.sendText(client, emissionJson(it->rx, "end", e, it->label, it->features));
  }
  // What is on the air now, and the latest decodes.
  {
    std::lock_guard<std::mutex> l(rxMu_);
    for (auto& [id, r] : rx_) {
      std::lock_guard<std::mutex> lk(r->m);
      for (const auto& [eid, a] : r->active) web_.sendText(client, emissionJson(id, "start", a.e, a.label, a.features));
    }
  }
  // This run's latest decodes (earlier runs numbered their emissions from 1
  // too: theirs would be attached to today's emissions of the same number).
  std::vector<StoredDecode> recent;
  {
    std::lock_guard<std::mutex> l(storeMu_);
    Query q;
    q.session = store_.session();
    q.limit = 200;
    recent = store_.decodes(q);
  }
  for (auto it = recent.rbegin(); it != recent.rend(); ++it) {
    DecodeEvent d;
    d.emissionId = it->emissionId;
    d.utc = it->utc;
    d.decoder = it->decoder;
    d.text = it->text;
    d.freqHz = it->freqHz;
    d.quality = it->quality;
    d.first = it->first;
    web_.sendText(client, decodeJson(it->rx, d));
  }
}

void ClassifierApp::onWsMessage(int client, const std::string& text) {
  std::vector<JsonField> f;
  if (!parseJsonObject(text, f, nullptr)) return;
  const std::string cmd = jsonField(f, "cmd");
  const int rx = std::atoi(jsonField(f, "rx").c_str());
  std::string result;
  bool controllable = false;
  {
    std::lock_guard<std::mutex> l(rxMu_);
    auto it = rx_.find(rx);
    controllable = it != rx_.end() && it->second->controllable;
  }
  if (cmd == "settings" || cmd == "reconnect") {
    SettingsHandler apply;
    std::function<void()> reconnect;
    {
      std::lock_guard<std::mutex> l(sideMu_);
      apply = settingsApply_;
      reconnect = reconnect_;
    }
    if (cmd == "settings") result = apply ? apply(f) : "not_available";
    else if (reconnect) reconnect();
    else result = "not_available";
    if (result.empty()) web_.broadcastText(statusJson());
  } else if (cmd == "ignore") {
    // Leave an emission: its session ends, it gets no new one.
    const uint64_t id = std::strtoull(jsonField(f, "id").c_str(), nullptr, 10);
    std::lock_guard<std::mutex> l(rxMu_);
    auto it = rx_.find(rx);
    if (it == rx_.end() || id == 0) result = "no_receiver";
    else {
      std::lock_guard<std::mutex> lk(it->second->m);
      it->second->ignore.push_back(id);
    }
  } else if (cmd == "record") {
    result = record(rx, f);
  } else if (cmd == "reload_databases") {
    bool can;
    {
      std::lock_guard<std::mutex> l(sideMu_);
      can = static_cast<bool>(frequencyReload_);
    }
    if (!can) result = "not_available";
    else reloadDatabases();
  } else if (cmd == "download_databases") {
    std::string folder;
    {
      std::lock_guard<std::mutex> l(sideMu_);
      folder = frequenciesFolder_;
    }
    if (folder.empty()) result = "not_available";
    else if (downloading_.exchange(true)) result = "already_downloading";
    else {
      if (downloadThread_.joinable()) downloadThread_.join();
      downloadThread_ = std::thread([this] { runDownloads(); });
    }
  } else if (cmd == "diagnostics") {
    bool can;
    {
      std::lock_guard<std::mutex> l(sideMu_);
      can = static_cast<bool>(diagnostics_);
    }
    if (!can) result = "not_available";
    else if (diagnosing_.exchange(true)) result = "already_running";
    else {
      if (diagnosticsThread_.joinable()) diagnosticsThread_.join();
      const bool rates = jsonField(f, "rates") == "1" || jsonField(f, "rates") == "true";
      diagnosticsThread_ = std::thread([this, rates] { runDiagnostics(rates); });
    }
  } else if (cmd == "marker_save") {
    result = saveMarker(f);
  } else if (cmd == "marker_delete") {
    result = deleteMarker(jsonField(f, "id"));
  } else if (cmd == "window_save") {
    result = saveWindow(f);
  } else if (cmd == "window_delete") {
    result = deleteWindow(jsonField(f, "id"));
  } else if (cmd == "open_folder") {
    std::function<void(const std::string&)> open;
    std::string folder;
    {
      std::lock_guard<std::mutex> l(sideMu_);
      open = openFolder_;
      const std::string which = jsonField(f, "which");
      folder = which == "recordings" ? recordingsFolder_ : which == "reports" ? reportsFolder_ : frequenciesFolder_;
    }
    if (!open || folder.empty()) result = "not_available";
    else {
      std::error_code ec;
      std::filesystem::create_directories(std::filesystem::u8path(folder), ec);
      open(folder);
    }
  } else if (cmd != "listen" && cmd != "tune") {
    result = "unknown_command";
  } else {
    std::lock_guard<std::mutex> l(controlMu_);
    if (!controllable || !control_) result = "not_controllable";
    else result = control_(rx, cmd, f);
  }
  web_.sendText(client, std::string("{\"type\":\"result\",\"cmd\":") + jsonQuote(cmd) +
                            ",\"ok\":" + (result.empty() ? "true" : "false") + ",\"message\":" + jsonQuote(result) + "}");
}

HttpResponse ClassifierApp::apiEmissions(const HttpRequest& r) {
  std::vector<StoredEmission> es;
  {
    std::lock_guard<std::mutex> l(storeMu_);
    es = store_.emissions(queryOf(r));
  }
  std::string s = "[";
  for (std::size_t i = 0; i < es.size(); ++i) {
    const auto& e = es[i];
    if (i) s += ",\n";
    s += "{\"session\":" + std::to_string(e.session) + ",\"id\":" + std::to_string(e.id) + ",\"rx\":" + std::to_string(e.rx) +
         ",\"start\":" + num(e.startUtc, 3) + ",\"end\":" + num(e.endUtc, 3) + ",\"lo\":" + num(e.loHz, 1) +
         ",\"hi\":" + num(e.hiHz, 1) + ",\"centre\":" + num(e.centreHz, 1) + ",\"snr\":" + num(e.meanSnrDb, 1) +
         ",\"peak\":" + num(e.peakSnrDb, 1) + ",\"duty\":" + num(e.duty, 2) + ",\"drift\":" + num(e.driftHzPerSecond, 3) +
         ",\"ended\":" + (e.ended ? "true" : "false") + ",\"label\":" + jsonQuote(e.label) +
         (e.features.empty() ? std::string() : ",\"features\":" + e.features) + "}";
  }
  return json(s + "]");
}

HttpResponse ClassifierApp::apiDecodes(const HttpRequest& r) {
  std::vector<StoredDecode> ds;
  {
    std::lock_guard<std::mutex> l(storeMu_);
    auto sit = r.query.find("session");
    auto eit = r.query.find("emission");
    auto rit = r.query.find("rx");
    if (sit != r.query.end() && eit != r.query.end())
      ds = store_.decodesOf(std::atoll(sit->second.c_str()), rit == r.query.end() ? 0 : std::atoi(rit->second.c_str()),
                            std::strtoull(eit->second.c_str(), nullptr, 10));
    else
      ds = store_.decodes(queryOf(r));
  }
  std::string s = "[";
  for (std::size_t i = 0; i < ds.size(); ++i) {
    const auto& d = ds[i];
    if (i) s += ",\n";
    s += "{\"session\":" + std::to_string(d.session) + ",\"id\":" + std::to_string(d.emissionId) +
         ",\"rx\":" + std::to_string(d.rx) + ",\"utc\":" + num(d.utc, 3) + ",\"decoder\":" + jsonQuote(d.decoder) +
         ",\"freq\":" + num(d.freqHz, 1) + ",\"quality\":" + num(d.quality, 2) + ",\"first\":" +
         (d.first ? "true" : "false") + ",\"text\":" + jsonQuote(d.text) + "}";
  }
  return json(s + "]");
}

HttpResponse ClassifierApp::apiExport(const HttpRequest& r, bool csv) {
  HttpResponse out;
  {
    std::lock_guard<std::mutex> l(storeMu_);
    out.body = csv ? store_.exportCsv(queryOf(r)) : store_.exportJson(queryOf(r));
  }
  out.contentType = csv ? "text/csv; charset=utf-8" : "application/json";
  out.extraHeaders = std::string("Content-Disposition: attachment; filename=\"hf_classifier.") + (csv ? "csv" : "json") +
                     "\"\r\n";
  return out;
}

}  // namespace hfc
