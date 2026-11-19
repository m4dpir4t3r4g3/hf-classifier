// sdrc_record: records IQ from SDRconnect to SigMF files (ci16_le), with the
// centre frequency and UTC time of every stretch of contiguous samples.
// Recordings made with it are the test data the classifier is evaluated on.
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "hfc/client.hpp"
#include "hfc/sigmf.hpp"

using namespace hfc;
using namespace hfc::sdrconnect;

namespace {

void usage() {
  std::fprintf(stderr,
               "usage: sdrc_record -o BASE [options]\n"
               "Records IQ from SDRconnect to BASE.sigmf-data and BASE.sigmf-meta (SigMF,\n"
               "ci16_le). A change of sample rate starts a new pair of files (BASE-2, ...).\n"
               "\n"
               "  -o, --out BASE       output path without extension\n"
               "  --host ADDR          SDRconnect address (default 127.0.0.1)\n"
               "  --port N             WebSocket port (default 5454)\n"
               "  --device D           primary (default) or secondary\n"
               "  --seconds S          stop after S seconds (default: Ctrl+C)\n"
               "  --center HZ          tune the device first\n"
               "  --rate HZ            set the sample rate first\n"
               "  --dual SERIAL        select RSPduo dual-tuner mode (\"SERIAL:Dual\") first\n"
               "  --description TEXT   stored in the metadata (what, where, antenna...)\n");
}

std::atomic<bool> g_stop{false};
void onSignal(int) { g_stop = true; }

struct Chunk {
  std::vector<int16_t> iq;
  uint64_t index;
  double rate, centerHz, utc;
  bool discontinuity, retuned, gap;
  uint64_t gapIndex;
  double gapSeconds;
};

}  // namespace

int main(int argc, char** argv) {
  std::string host = "127.0.0.1", base, description, dualSerial, center, rate;
  int port = kDefaultPort;
  double seconds = 0;
  Device device = Device::Primary;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    auto next = [&]() -> std::string {
      if (i + 1 >= argc) {
        usage();
        std::exit(2);
      }
      return argv[++i];
    };
    if (a == "-o" || a == "--out") base = next();
    else if (a == "--host") host = next();
    else if (a == "--port") port = std::atoi(next().c_str());
    else if (a == "--device") {
      if (!parseDevice(next(), device)) {
        usage();
        return 2;
      }
    } else if (a == "--seconds") seconds = std::atof(next().c_str());
    else if (a == "--center" || a == "--centre") center = next();
    else if (a == "--rate") rate = next();
    else if (a == "--dual") dualSerial = next();
    else if (a == "--description") description = next();
    else {
      usage();
      return a == "-h" || a == "--help" ? 0 : 2;
    }
  }
  if (base.empty()) {
    usage();
    return 2;
  }
  std::signal(SIGINT, onSignal);

  // The reader thread only queues; this thread writes, so that a slow disk
  // cannot hold up the WebSocket (SDRconnect drops IQ for slow readers).
  std::mutex mu;
  std::condition_variable cv;
  std::deque<Chunk> queue;
  std::size_t queued = 0;
  uint64_t overflowPairs = 0, unknownRatePairs = 0;
  Client::Handlers h;
  h.onIq = [&](Device d, const IqBlock& b) {
    if (d != device) return;
    std::lock_guard<std::mutex> lock(mu);
    // Up to about 10 s of IQ at the current rate may wait for the disk.
    if (b.sampleRate > 0 && static_cast<double>(queued) > 10 * b.sampleRate) {
      overflowPairs += b.pairs;
      return;
    }
    // Recorded at the rate SDRconnect reports, never the measured one (it
    // varies from block to block, and each change would start a new file).
    if (b.nominalRate <= 0) {
      unknownRatePairs += b.pairs;
      return;
    }
    queue.push_back({std::vector<int16_t>(b.iq, b.iq + 2 * b.pairs), b.index, b.nominalRate, b.centerHz, b.utc,
                     b.discontinuity, b.retuned, b.gap, b.gapIndex, b.gapSeconds});
    queued += b.pairs;
    cv.notify_one();
  };
  h.onDisconnected = [&](const std::string& why) {
    std::fprintf(stderr, "disconnected: %s\n", why.c_str());
    g_stop = true;
    cv.notify_one();
  };
  Client c(h);
  std::string err;
  if (!c.connect(host, static_cast<uint16_t>(port), 3000, &err)) {
    std::fprintf(stderr, "cannot connect to ws://%s:%d/: %s\n(enable SDRconnect's WebSocket server in Preferences)\n",
                 host.c_str(), port, err.c_str());
    return 1;
  }
  if (!dualSerial.empty()) {
    c.selectDeviceSerial(dualSerial + ":Dual");
    std::this_thread::sleep_for(std::chrono::seconds(3));
  }
  c.enableDevice(device, true);
  if (!rate.empty()) {
    const auto got = c.setAndConfirm(prop::kSampleRate, rate, device, 3000);
    if (!got || std::atof(got->c_str()) != std::atof(rate.c_str()))
      std::fprintf(stderr, "warning: sample rate %s not accepted (SDRconnect reports %s)\n", rate.c_str(),
                   got ? got->c_str() : "nothing");
  }
  if (!center.empty()) c.setAndConfirm(prop::kCenterFrequency, center, device, 3000);
  const std::string hw = "SDRplay " + c.query(prop::kActiveDevice, Device::Primary, 1500).value_or("RSP") +
                         " via SDRconnect (WebSocket API " +
                         c.query(prop::kApiVersion, Device::Primary, 1500).value_or("?") + ", " + deviceName(device) +
                         " device)";
  // So that the rate is known from the first block on.
  c.query(prop::kSampleRate, device, 1500);
  c.enableIq(true);

  SigmfWriter w;
  int part = 1;
  double fileRate = 0;
  // Where recent chunks went in the file (stream index -> file sample), to
  // mark a gap that is found after the samples following it were written.
  struct Placed {
    uint64_t index, fileSample, pairs;
  };
  std::deque<Placed> placed;
  uint64_t nextIndex = 0;
  const auto start = std::chrono::steady_clock::now();
  std::printf("recording %s (Ctrl+C to stop)\n", deviceName(device));
  bool stopping = false;
  for (;;) {
    // At the end, stop the IQ and write what is still queued.
    if (!stopping && (g_stop || (seconds > 0 && std::chrono::duration<double>(
                                                    std::chrono::steady_clock::now() - start).count() >= seconds))) {
      stopping = true;
      c.enableIq(false);
    }
    Chunk ch;
    {
      std::unique_lock<std::mutex> lock(mu);
      if (!stopping) cv.wait_for(lock, std::chrono::milliseconds(200), [&] { return !queue.empty() || g_stop; });
      if (queue.empty()) {
        if (stopping) break;
        continue;
      }
      ch = std::move(queue.front());
      queue.pop_front();
      queued -= ch.iq.size() / 2;
    }
    if (!w.isOpen() || ch.rate != fileRate) {
      if (w.isOpen()) {
        w.close(nullptr);
        ++part;
      }
      fileRate = ch.rate;
      SigmfWriter::Global g;
      g.sampleRate = ch.rate;
      g.hardware = hw;
      g.recorder = std::string("sdrc_record ") + HFC_VERSION;
      g.description = description;
      const std::string path = part == 1 ? base : base + "-" + std::to_string(part);
      if (!w.open(path, g, &err)) {
        std::fprintf(stderr, "%s\n", err.c_str());
        return 1;
      }
      placed.clear();
      std::printf("writing %s.sigmf-data at %.0f S/s, centre %.0f Hz\n", path.c_str(), ch.rate, ch.centerHz);
    }
    const uint64_t pairs = ch.iq.size() / 2;
    const bool first = placed.empty();
    bool newCapture = first || ch.retuned;
    if (!first && ch.index != nextIndex) {
      // This program's own queue overflowed.
      w.annotate(w.samplesWritten(), 0, "gap", "samples dropped: the disk did not keep up");
      newCapture = true;
    }
    if (ch.gap) {
      // Found late: the samples went missing at gapIndex, which may already
      // be in the file.
      char note[160];
      std::snprintf(note, sizeof(note), "about %.2f s of samples were lost before this point (detected by timing)",
                    ch.gapSeconds);
      const double gapUtc = ch.utc - static_cast<double>(ch.index - ch.gapIndex) / ch.rate;
      bool marked = false;
      for (const auto& p : placed)
        if (ch.gapIndex >= p.index && ch.gapIndex < p.index + p.pairs) {
          const uint64_t at = p.fileSample + (ch.gapIndex - p.index);
          w.annotate(at, 0, "gap", note);
          w.addCapture(at, ch.centerHz, gapUtc);
          marked = true;
          break;
        }
      if (!marked) {
        w.annotate(w.samplesWritten(), 0, "gap", note);
        newCapture = true;
      }
    } else if (!first && ch.discontinuity) {
      newCapture = true;
    }
    placed.push_back({ch.index, w.samplesWritten(), pairs});
    while (placed.size() > 100) placed.pop_front();
    if (!w.write(ch.iq.data(), pairs, newCapture, ch.centerHz, ch.utc)) {
      std::fprintf(stderr, "write failed (disk full?)\n");
      break;
    }
    nextIndex = ch.index + pairs;
  }
  c.disconnect();
  if (w.isOpen() && !w.close(&err)) {
    std::fprintf(stderr, "%s\n", err.c_str());
    return 1;
  }
  const auto st = c.iqStats(device);
  std::printf("done: %llu samples written, %llu gaps detected (~%.2f s lost), %llu samples dropped by the queue\n",
              static_cast<unsigned long long>(w.samplesWritten()), static_cast<unsigned long long>(st.gaps),
              st.lostSeconds, static_cast<unsigned long long>(overflowPairs));
  if (unknownRatePairs)
    std::printf("%llu samples not recorded: SDRconnect had not reported the sample rate yet\n",
                static_cast<unsigned long long>(unknownRatePairs));
  return 0;
}
