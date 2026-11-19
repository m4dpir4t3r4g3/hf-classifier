#include "hfc/ftx_decoder.hpp"

#include <cstring>
#include <mutex>

extern "C" {
#include "common/monitor.h"
#include "ft8/constants.h"
#include "ft8/decode.h"
#include "ft8/message.h"
}

namespace hfc {

namespace {

constexpr int kMinScore = 10;
constexpr int kMaxCandidates = 140;
constexpr int kLdpcIterations = 25;
constexpr int kMaxDecoded = 50;
// The duplicate filter's table: larger than the most messages kept, so the
// probe below always finds a free slot.
constexpr int kDedupeSize = 2 * kMaxDecoded + 1;
constexpr int kHashSize = 256;

}  // namespace

// Callsigns seen in full, so that hashed callsigns in later messages can be
// shown. ft8_lib's interface is a pair of plain C callbacks, so the table in
// use is reached through a pointer set while decoding; the lock makes that
// safe when several decoders run in different threads.
struct FtxDecoder::Hashes {
  struct Entry {
    char callsign[12];
    uint32_t hash;
  };
  Entry table[kHashSize] = {};
  int size = 0;

  static std::mutex& lock() {
    static std::mutex m;
    return m;
  }
  static Hashes*& current() {
    static Hashes* h = nullptr;
    return h;
  }

  void add(const char* callsign, uint32_t hash) {
    const uint16_t h10 = (hash >> 12) & 0x3FFu;
    int i = (h10 * 23) % kHashSize;
    while (table[i].callsign[0]) {
      if ((table[i].hash & 0x3FFFFFu) == hash && std::strcmp(table[i].callsign, callsign) == 0) {
        table[i].hash &= 0x3FFFFFu;
        return;
      }
      i = (i + 1) % kHashSize;
      if (size >= kHashSize - 1) return;
    }
    ++size;
    std::strncpy(table[i].callsign, callsign, 11);
    table[i].callsign[11] = '\0';
    table[i].hash = hash;
  }

  bool lookup(ftx_callsign_hash_type_t type, uint32_t hash, char* callsign) const {
    const uint8_t shift = type == FTX_CALLSIGN_HASH_10_BITS ? 12 : (type == FTX_CALLSIGN_HASH_12_BITS ? 10 : 0);
    const uint16_t h10 = (hash >> (12 - shift)) & 0x3FFu;
    int i = (h10 * 23) % kHashSize;
    for (int n = 0; n < kHashSize && table[i].callsign[0]; ++n) {
      if (((table[i].hash & 0x3FFFFFu) >> shift) == hash) {
        std::strcpy(callsign, table[i].callsign);
        return true;
      }
      i = (i + 1) % kHashSize;
    }
    callsign[0] = '\0';
    return false;
  }

  // Ages entries; forgets those not seen for `maxAge` slots.
  void age(uint8_t maxAge) {
    for (auto& e : table) {
      if (!e.callsign[0]) continue;
      const uint8_t a = static_cast<uint8_t>(e.hash >> 24);
      if (a > maxAge) {
        e.callsign[0] = '\0';
        e.hash = 0;
        --size;
      } else {
        e.hash = (static_cast<uint32_t>(a + 1u) << 24) | (e.hash & 0x3FFFFFu);
      }
    }
  }

  static bool lookupCb(ftx_callsign_hash_type_t type, uint32_t hash, char* callsign) {
    return current()->lookup(type, hash, callsign);
  }
  static void saveCb(const char* callsign, uint32_t hash) { current()->add(callsign, hash); }
};

FtxDecoder::FtxDecoder(bool ft4) : ft4_(ft4), hashes_(std::make_unique<Hashes>()) {}

FtxDecoder::~FtxDecoder() = default;

std::vector<FtxMessage> FtxDecoder::decodeSlot(const float* audio, std::size_t n) {
  monitor_t mon;
  monitor_config_t cfg;
  cfg.f_min = 100;
  cfg.f_max = 3000;
  cfg.sample_rate = 12000;
  cfg.time_osr = 2;
  cfg.freq_osr = 2;
  cfg.protocol = ft4_ ? FTX_PROTOCOL_FT4 : FTX_PROTOCOL_FT8;
  monitor_init(&mon, &cfg);
  for (std::size_t pos = 0; pos + static_cast<std::size_t>(mon.block_size) <= n; pos += static_cast<std::size_t>(mon.block_size))
    monitor_process(&mon, audio + pos);

  std::vector<FtxMessage> out;
  std::vector<ftx_candidate_t> cands(kMaxCandidates);
  const int found = ftx_find_candidates(&mon.wf, kMaxCandidates, cands.data(), kMinScore);
  std::vector<ftx_message_t> decoded(kDedupeSize);
  std::vector<ftx_message_t*> slots(kDedupeSize, nullptr);
  int kept = 0;

  std::lock_guard<std::mutex> lock(Hashes::lock());
  Hashes::current() = hashes_.get();
  ftx_callsign_hash_interface_t hashIf;
  hashIf.lookup_hash = &Hashes::lookupCb;
  hashIf.save_hash = &Hashes::saveCb;
  // At most kMaxDecoded different messages per slot. (ft8_lib's demo, which
  // this follows, probed a table of exactly that size and looped forever on
  // the next new message.)
  for (int i = 0; i < found && kept < kMaxDecoded; ++i) {
    const ftx_candidate_t& c = cands[static_cast<std::size_t>(i)];
    ftx_message_t msg;
    ftx_decode_status_t status;
    if (!ftx_decode_candidate(&mon.wf, &c, kLdpcIterations, &msg, &status)) continue;
    // Drop duplicates (the same message found at neighbouring candidates).
    int h = static_cast<int>(msg.hash % kDedupeSize);
    bool dup = false;
    while (slots[static_cast<std::size_t>(h)]) {
      if (slots[static_cast<std::size_t>(h)]->hash == msg.hash &&
          std::memcmp(slots[static_cast<std::size_t>(h)]->payload, msg.payload, sizeof(msg.payload)) == 0) {
        dup = true;
        break;
      }
      h = (h + 1) % kDedupeSize;
    }
    if (dup) continue;
    decoded[static_cast<std::size_t>(h)] = msg;
    slots[static_cast<std::size_t>(h)] = &decoded[static_cast<std::size_t>(h)];
    ++kept;
    char text[FTX_MAX_MESSAGE_LENGTH];
    ftx_message_offsets_t offsets;
    if (ftx_message_decode(&msg, &hashIf, text, &offsets) != FTX_MESSAGE_RC_OK) continue;
    FtxMessage m;
    m.text = text;
    m.audioHz = (mon.min_bin + c.freq_offset + static_cast<float>(c.freq_sub) / mon.wf.freq_osr) / mon.symbol_period;
    m.dtSeconds = (c.time_offset + static_cast<float>(c.time_sub) / mon.wf.time_osr) * mon.symbol_period;
    m.score = c.score;
    out.push_back(m);
  }
  hashes_->age(10);
  Hashes::current() = nullptr;
  monitor_free(&mon);
  return out;
}

const std::vector<double>& ft8DialFrequenciesKhz() {
  static const std::vector<double> f = {1840, 3573, 5357, 7074, 10136, 14074, 18100, 21074, 24915, 28074};
  return f;
}

const std::vector<double>& ft4DialFrequenciesKhz() {
  static const std::vector<double> f = {3575, 7047.5, 10140, 14080, 18104, 21140, 24919, 28180};
  return f;
}

}  // namespace hfc
