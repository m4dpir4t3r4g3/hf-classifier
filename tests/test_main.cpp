// Test suite: unit tests for the codecs, the WebSocket framing, JSON, the IQ
// clock and the SigMF writer, and end-to-end tests of the SDRconnect client
// against the fake SDRconnect server. Self-contained, no framework dependency.
// The DSP tests are in test_dsp.cpp, the decoding tests in test_pipeline.cpp.
// Run: hfc_tests [name-filter]
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "fake_sdrconnect.hpp"
#include "test_framework.hpp"
#include "hfc/client.hpp"
#include "hfc/codec.hpp"
#include "hfc/iq_clock.hpp"
#include "hfc/json.hpp"
#include "hfc/sdrconnect.hpp"
#include "hfc/sigmf.hpp"
#include "hfc/web_server.hpp"
#include "hfc/websocket.hpp"

using namespace hfc;

namespace {

std::string hex(const uint8_t* p, std::size_t n) {
  static const char* d = "0123456789abcdef";
  std::string s;
  for (std::size_t i = 0; i < n; ++i) s += d[p[i] >> 4], s += d[p[i] & 15];
  return s;
}

std::string sha1Hex(const std::string& s) {
  const auto h = sha1(s.data(), s.size());
  return hex(h.data(), h.size());
}

void sleepMs(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

// Waits up to `ms` for `cond`.
bool waitFor(const std::function<bool()>& cond, int ms) {
  for (int t = 0; t < ms; t += 10) {
    if (cond()) return true;
    sleepMs(10);
  }
  return cond();
}

}  // namespace

// --- codecs ---------------------------------------------------------------

TEST(sha1_known_answers) {
  // FIPS 180 examples.
  CHECK(sha1Hex("") == "da39a3ee5e6b4b0d3255bfef95601890afd80709");
  CHECK(sha1Hex("abc") == "a9993e364706816aba3e25717850c26c9cd0d89d");
  CHECK(sha1Hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
        "84983e441c3bd26ebaae4aa1f95129e5e54670f1");
  CHECK(sha1Hex(std::string(1000000, 'a')) == "34aa973cd4c4daa4f61eeb2bdbad27316534016f");
}

TEST(base64_rfc4648_vectors) {
  const char* plain[] = {"", "f", "fo", "foo", "foob", "fooba", "foobar"};
  const char* coded[] = {"", "Zg==", "Zm8=", "Zm9v", "Zm9vYg==", "Zm9vYmE=", "Zm9vYmFy"};
  for (int i = 0; i < 7; ++i) {
    CHECK(base64Encode(plain[i], std::strlen(plain[i])) == coded[i]);
    std::vector<uint8_t> back;
    CHECK(base64Decode(coded[i], back));
    CHECK(std::string(back.begin(), back.end()) == plain[i]);
  }
  std::vector<uint8_t> tmp;
  CHECK(!base64Decode("Zm9", tmp));
  CHECK(!base64Decode("Zm=v", tmp));
  CHECK(!base64Decode("Z!9v", tmp));
}

TEST(websocket_accept_rfc6455_example) {
  // RFC 6455, section 1.3.
  CHECK(websocketAccept("dGhlIHNhbXBsZSBub25jZQ==") == "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
}

// --- WebSocket framing --------------------------------------------------

TEST(ws_frames_round_trip_every_length_class) {
  std::mt19937 rng(3);
  for (std::size_t n : {0u, 1u, 125u, 126u, 127u, 65535u, 65536u, 70000u}) {
    for (bool masked : {false, true}) {
      std::vector<uint8_t> payload(n);
      for (auto& b : payload) b = static_cast<uint8_t>(rng());
      const uint8_t key[4] = {0x12, 0x34, 0x56, 0x78};
      const auto wire = encodeWsFrame(WsOpcode::Binary, payload.data(), n, true, masked ? key : nullptr);
      const std::size_t header = (n < 126 ? 2 : n <= 65535 ? 4 : 10) + (masked ? 4 : 0);
      CHECK(wire.size() == header + n);
      WsFrameDecoder dec;
      WsFrame f;
      // Byte by byte for the small ones: every partial state must say NeedMore.
      if (n < 300) {
        for (std::size_t i = 0; i + 1 < wire.size(); ++i) {
          dec.feed(&wire[i], 1);
          CHECK(dec.next(f, nullptr) == WsFrameDecoder::Result::NeedMore);
        }
        dec.feed(&wire.back(), 1);
      } else {
        dec.feed(wire.data(), wire.size());
      }
      CHECK(dec.next(f, nullptr) == WsFrameDecoder::Result::Frame);
      CHECK(f.fin && f.opcode == WsOpcode::Binary && f.masked == masked);
      CHECK(f.payload == payload);
      CHECK(dec.buffered() == 0);
    }
  }
}

TEST(ws_decoder_rejects_malformed_frames) {
  auto bad = [](std::vector<uint8_t> wire, std::size_t limit = 1 << 20) {
    WsFrameDecoder dec(limit);
    dec.feed(wire.data(), wire.size());
    WsFrame f;
    return dec.next(f, nullptr) == WsFrameDecoder::Result::Error;
  };
  CHECK(bad({0xC2, 0x00}));        // RSV1 without an extension
  CHECK(bad({0x83, 0x00}));        // opcode 3 is reserved
  CHECK(bad({0x09, 0x00}));        // fragmented ping
  CHECK(bad({0x89, 0x7E, 0x00, 0x80}));  // ping longer than 125 bytes
  std::vector<uint8_t> big = {0x82, 0x7F, 0, 0, 0, 0, 0, 0x20, 0, 0};  // 2 MB
  CHECK(bad(big, 1 << 20));
}

TEST(ws_decoder_handles_back_to_back_frames) {
  const auto a = encodeWsFrame(WsOpcode::Text, "hello", 5, false, nullptr);
  const auto b = encodeWsFrame(WsOpcode::Ping, "p", 1, true, nullptr);
  const auto c = encodeWsFrame(WsOpcode::Continuation, " world", 6, true, nullptr);
  std::vector<uint8_t> all;
  for (const auto* v : {&a, &b, &c}) all.insert(all.end(), v->begin(), v->end());
  WsFrameDecoder dec;
  dec.feed(all.data(), all.size());
  WsFrame f;
  CHECK(dec.next(f, nullptr) == WsFrameDecoder::Result::Frame && f.opcode == WsOpcode::Text && !f.fin);
  CHECK(dec.next(f, nullptr) == WsFrameDecoder::Result::Frame && f.opcode == WsOpcode::Ping);
  CHECK(dec.next(f, nullptr) == WsFrameDecoder::Result::Frame && f.opcode == WsOpcode::Continuation && f.fin);
  CHECK(std::string(f.payload.begin(), f.payload.end()) == " world");
  CHECK(dec.next(f, nullptr) == WsFrameDecoder::Result::NeedMore);
}

namespace {

// A WebSocket server written by hand, to send frames WebSocket never would
// (fragments, pings in between, masked frames). Answers the handshake, then
// sends `frames` and records what the client sends back.
struct RawServer {
  TcpListener l;
  std::thread t;
  std::vector<WsFrame> received;
  bool start(std::vector<std::vector<uint8_t>> frames) {
    if (!l.listen("127.0.0.1", 0, nullptr)) return false;
    t = std::thread([this, frames] {
      TcpSocket s = l.accept(3000);
      if (!s.valid()) return;
      s.setRecvTimeout(3000);
      std::string req;
      char buf[4096];
      while (req.find("\r\n\r\n") == std::string::npos) {
        const long n = s.recvSome(buf, sizeof(buf));
        if (n <= 0) return;
        req.append(buf, static_cast<std::size_t>(n));
      }
      const std::size_t k = req.find("Sec-WebSocket-Key: ") + 19;
      const std::string key = req.substr(k, req.find("\r\n", k) - k);
      const std::string resp = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                               "Sec-WebSocket-Accept: " + websocketAccept(key) + "\r\n\r\n";
      s.sendAll(resp.data(), resp.size());
      s.setRecvTimeout(30000);  // the client may take a while through 70 MiB on a busy machine
      // Sent while reading: a client that gives up stops reading, and a
      // send that waits for it would never return.
      std::thread sender([&s, frames] {
        for (const auto& f : frames)
          if (!s.sendAll(f.data(), f.size())) return;
      });
      WsFrameDecoder dec;
      for (;;) {
        WsFrame f;
        const auto r = dec.next(f, nullptr);
        if (r == WsFrameDecoder::Result::Frame) {
          received.push_back(f);
          if (f.opcode == WsOpcode::Close) break;
          continue;
        }
        if (r == WsFrameDecoder::Result::Error) break;
        const long n = s.recvSome(buf, sizeof(buf));
        if (n <= 0) break;
        dec.feed(reinterpret_cast<const uint8_t*>(buf), static_cast<std::size_t>(n));
      }
      s.shutdown();  // ends a send still waiting
      sender.join();
    });
    return true;
  }
  ~RawServer() {
    if (t.joinable()) t.join();
  }
};

std::vector<uint8_t> frame(WsOpcode op, const std::string& payload, bool fin, bool masked = false) {
  const uint8_t key[4] = {1, 2, 3, 4};
  return encodeWsFrame(op, payload.data(), payload.size(), fin, masked ? key : nullptr);
}

}  // namespace

TEST(ws_client_reassembles_fragments_around_a_ping) {
  RawServer srv;
  CHECK(srv.start({frame(WsOpcode::Text, "frag", false), frame(WsOpcode::Ping, "p1", true),
                   frame(WsOpcode::Continuation, "men", false), frame(WsOpcode::Continuation, "ted", true)}));
  WebSocket ws;
  CHECK(ws.connect("127.0.0.1", srv.l.port(), "/", 3000, nullptr));
  WsMessage m;
  CHECK(ws.receive(m, nullptr) && !m.binary && m.text() == "fragmented");
  ws.sendClose();
  srv.t.join();
  // The ping was answered with a pong carrying its payload, from the client (masked).
  bool pong = false;
  for (const auto& f : srv.received)
    if (f.opcode == WsOpcode::Pong && std::string(f.payload.begin(), f.payload.end()) == "p1" && f.masked) pong = true;
  CHECK(pong);
}

TEST(ws_client_rejects_masked_frames_and_oversized_messages) {
  {
    RawServer srv;
    CHECK(srv.start({frame(WsOpcode::Text, "masked", true, true)}));
    WebSocket ws;
    CHECK(ws.connect("127.0.0.1", srv.l.port(), "/", 3000, nullptr));
    WsMessage m;
    std::string err;
    CHECK(!ws.receive(m, &err) && err.find("masked") != std::string::npos);
    srv.t.join();
    CHECK(!srv.received.empty() && srv.received.back().opcode == WsOpcode::Close);
  }
  {
    // 70 fragments of 1 MiB: each frame is under the 64 MiB limit, the
    // message is not.
    std::vector<std::vector<uint8_t>> frames;
    const std::string mib(1 << 20, 'x');
    frames.push_back(frame(WsOpcode::Binary, mib, false));
    for (int i = 0; i < 69; ++i) frames.push_back(frame(WsOpcode::Continuation, mib, false));
    RawServer srv;
    CHECK(srv.start(frames));
    WebSocket ws;
    CHECK(ws.connect("127.0.0.1", srv.l.port(), "/", 3000, nullptr));
    WsMessage m;
    std::string err;
    CHECK(!ws.receive(m, &err) && err == "message too big");
    ws.shutdown();
    srv.t.join();
    // Its close code says why (1009).
    bool code1009 = false;
    for (const auto& f : srv.received)
      if (f.opcode == WsOpcode::Close && f.payload.size() == 2 && ((f.payload[0] << 8) | f.payload[1]) == 1009) code1009 = true;
    CHECK(code1009);
  }
}

TEST(ws_connection_round_trip_and_close) {
  TcpListener l;
  CHECK(l.listen("127.0.0.1", 0, nullptr));
  std::thread server([&] {
    WebSocket ws;
    if (!ws.accept(l.accept(3000), 3000, nullptr)) return;
    WsMessage m;
    while (ws.receive(m, nullptr)) {
      if (m.binary) ws.sendBinary(m.data.data(), m.data.size());
      else ws.sendText("echo:" + m.text());
    }
  });
  WebSocket ws;
  std::string err;
  CHECK(ws.connect("127.0.0.1", l.port(), "/", 3000, &err));
  CHECK(ws.sendText("abc"));
  WsMessage m;
  CHECK(ws.receive(m, nullptr) && !m.binary && m.text() == "echo:abc");
  std::vector<uint8_t> big(300000);
  for (std::size_t i = 0; i < big.size(); ++i) big[i] = static_cast<uint8_t>(i * 7);
  CHECK(ws.sendBinary(big.data(), big.size()));
  CHECK(ws.receive(m, nullptr) && m.binary && m.data == big);
  ws.sendClose();
  WsMessage none;
  CHECK(!ws.receive(none, nullptr));  // the server answers the close
  server.join();
}

// --- JSON -----------------------------------------------------------------

TEST(json_reads_sdrconnect_messages) {
  std::vector<JsonField> f;
  CHECK(parseJsonObject(R"({"event_type":"property_changed","property":"device_center_frequency",)"
                        R"("device":"primary","value":"100000000"})",
                        f, nullptr));
  CHECK(f.size() == 4);
  CHECK(jsonField(f, "event_type") == "property_changed");
  CHECK(jsonField(f, "value") == "100000000");
  CHECK(jsonField(f, "missing").empty());
  // Whitespace, escapes, non-string values kept as text.
  CHECK(parseJsonObject(" { \"a\" : \"x\\\"y\\\\z\\n\\u00e9\\ud83d\\ude00\" , \"n\": -1.5e3, \"t\": true,"
                        " \"o\": {\"k\": [1, 2, {\"z\": null}]} } ",
                        f, nullptr));
  CHECK(jsonField(f, "a") == "x\"y\\z\n\xC3\xA9\xF0\x9F\x98\x80");
  CHECK(jsonField(f, "n") == "-1.5e3" && !f[1].isString);
  CHECK(jsonField(f, "t") == "true");
  CHECK(jsonField(f, "o") == "{\"k\": [1, 2, {\"z\": null}]}");
  std::string err;
  CHECK(!parseJsonObject("{\"a\":\"b\",}", f, &err) && !err.empty());
  CHECK(!parseJsonObject("{\"a\":\"b\"} x", f, nullptr));
  CHECK(!parseJsonObject("{\"a\":\"b", f, nullptr));
  CHECK(!parseJsonObject("[1]", f, nullptr));
  CHECK(parseJsonObject("{}", f, nullptr) && f.empty());
}

TEST(json_writer_output_reads_back) {
  JsonWriter j;
  j.beginObject();
  j.key("s").value("a\"b\x01");
  j.key("rate").value(250000.0);
  j.key("frac").value(0.1);
  j.key("big").value(uint64_t(1) << 40);
  j.key("neg").value(-3);
  j.key("ok").value(true);
  j.key("list").beginArray().value(1).beginObject().key("x").value("y").endObject().endArray();
  j.key("empty").beginArray().endArray();
  j.endObject();
  std::vector<JsonField> f;
  CHECK(parseJsonObject(j.str(), f, nullptr));
  CHECK(jsonField(f, "s") == "a\"b\x01");
  CHECK(jsonField(f, "rate") == "250000");
  CHECK(jsonField(f, "frac") == "0.1");
  CHECK(jsonField(f, "big") == "1099511627776");
  CHECK(jsonField(f, "neg") == "-3");
  CHECK(jsonField(f, "ok") == "true");
  CHECK(jsonField(f, "empty") == "[]");
}

TEST(sdrconnect_messages_round_trip) {
  namespace sc = sdrconnect;
  const sc::Message m{"set_property", "device_center_frequency", "101000000", "secondary"};
  const std::string text = sc::encodeMessage(m);
  sc::Message back;
  CHECK(sc::decodeMessage(text, back, nullptr));
  CHECK(back.eventType == m.eventType && back.property == m.property && back.value == m.value &&
        back.device == m.device);
  // No device field when none is given (the API then means the primary).
  CHECK(sc::encodeMessage({"iq_stream_enable", "", "true", ""}).find("device") == std::string::npos);
  sc::Device d;
  CHECK(sc::parseDevice("", d) && d == sc::Device::Primary);
  CHECK(sc::parseDevice("secondary", d) && d == sc::Device::Secondary);
  CHECK(!sc::parseDevice("tertiary", d));
  CHECK(!sc::decodeMessage("{\"value\":\"1\"}", back, nullptr));
}

// --- IQ clock -------------------------------------------------------------

namespace {

// Feeds an IqClock blocks of `rate`/20 samples, arriving 20 times a second
// with latency and jitter. `lostAt`/`lostSeconds`: samples missing from the
// stream from that time on. `stallAt`: the reader stops for 1 s there and
// then takes the queued blocks at once (nothing lost).
struct ClockRun {
  IqClock clock;
  std::vector<IqClock::Stamp> stamps;
  std::vector<double> times;
};

ClockRun runClock(double rate, double seconds, double lostAt, double lostSeconds, double stallAt,
                  double stallSeconds = 1.0) {
  ClockRun r;
  r.clock.setNominalRate(rate, 0.0);
  std::mt19937 rng(5);
  std::uniform_real_distribution<double> jitter(0.0, 0.03);
  const double block = 0.05;
  const std::size_t n = static_cast<std::size_t>(rate * block);
  const double utc0 = 1.8e9;
  for (double t = 0.0; t < seconds; t += block) {
    if (lostAt >= 0 && t >= lostAt && t < lostAt + lostSeconds) continue;  // never sent
    double arrival = t + block + 0.02 + jitter(rng);
    // A stall: everything made during it arrives in a burst when it ends.
    if (stallAt >= 0 && t >= stallAt && t < stallAt + stallSeconds)
      arrival = stallAt + stallSeconds + (t - stallAt) * 0.01;
    r.stamps.push_back(r.clock.onBlock(n, 100.0 + arrival, utc0 + arrival));
    r.times.push_back(t);
  }
  return r;
}

}  // namespace

TEST(iq_clock_numbers_and_dates_a_steady_stream) {
  const ClockRun r = runClock(250000, 20.0, -1, 0, -1);
  const auto st = r.clock.stats();
  CHECK(st.gaps == 0);
  CHECK_NEAR(st.measuredRate, 250000, 2500);
  // Every block is dated within 60 ms of when its first sample was made
  // (latency 20 ms + jitter <= 30 ms).
  double worst = 0;
  for (std::size_t i = 0; i < r.stamps.size(); ++i) {
    CHECK(r.stamps[i].index == i * 12500);
    worst = std::max(worst, std::fabs(r.stamps[i].utc - (1.8e9 + r.times[i])));
  }
  CHECK(worst < 0.06);
  CHECK(!r.stamps[5].discontinuity);
}

TEST(iq_clock_detects_lost_samples) {
  const ClockRun r = runClock(250000, 10.0, 4.0, 0.6, -1);
  const auto st = r.clock.stats();
  CHECK(st.gaps == 1);
  CHECK_NEAR(st.lostSeconds, 0.6, 0.1);
  // Flagged within the detection window after the gap, and dated right again.
  std::size_t flagged = 0;
  for (std::size_t i = 0; i < r.stamps.size(); ++i)
    if (r.stamps[i].discontinuity) flagged = i;
  CHECK(r.times[flagged] >= 4.6 && r.times[flagged] <= 6.0);
  // The gap is placed where the samples went missing: at the first block
  // made after the 0.6 s that were never sent.
  std::size_t after = 1;
  while (after < r.times.size() && r.times[after] - r.times[after - 1] < 0.1) ++after;
  CHECK(r.stamps[flagged].gap);
  CHECK(r.stamps[flagged].gapIndex == r.stamps[after].index);
  CHECK_NEAR(r.stamps[flagged].gapSeconds, 0.6, 0.1);
  const std::size_t last = r.stamps.size() - 1;
  CHECK_NEAR(r.stamps[last].utc, 1.8e9 + r.times[last], 0.06);
}

TEST(iq_clock_does_not_mistake_a_reader_stall_for_a_gap) {
  // Stalls up to SDRconnect's ~2.5 s of buffering, shorter and longer than
  // the 1 s detection window: no gap, and the dates stay right.
  for (double stall : {0.5, 1.0, 1.2, 1.5, 2.0, 2.4}) {
    const ClockRun r = runClock(250000, 12.0, -1, 0, 4.0, stall);
    CHECK(r.clock.stats().gaps == 0);
    const std::size_t last = r.stamps.size() - 1;
    CHECK_NEAR(r.stamps[last].utc, 1.8e9 + r.times[last], 0.06);
  }
}

TEST(iq_clock_finds_samples_lost_during_a_stall) {
  // A 3 s stall of which SDRconnect could only buffer 2.5 s: the first
  // 0.5 s are lost, the rest arrives in a burst.
  const ClockRun r = runClock(250000, 12.0, 4.0, 0.5, 4.5, 2.5);
  CHECK(r.clock.stats().gaps == 1);
  CHECK_NEAR(r.clock.stats().lostSeconds, 0.5, 0.1);
  const std::size_t last = r.stamps.size() - 1;
  CHECK_NEAR(r.stamps[last].utc, 1.8e9 + r.times[last], 0.06);
}

TEST(iq_clock_flags_rate_changes_and_retunes) {
  IqClock c;
  c.setNominalRate(250000, 0.0);
  c.onBlock(12500, 0.05, 1.0);
  c.onBlock(12500, 0.10, 1.05);
  c.setNominalRate(500000, 0.12);
  auto s = c.onBlock(25000, 0.15, 1.15);
  CHECK(s.discontinuity && s.settling);
  s = c.onBlock(25000, 0.30, 1.30);
  CHECK(!s.discontinuity && !s.settling);
  c.markRetune(0.31);
  s = c.onBlock(25000, 0.35, 1.35);
  CHECK(s.retuned && s.settling && !s.discontinuity);
  s = c.onBlock(25000, 0.45, 1.45);
  CHECK(!s.retuned && !s.settling);
}

// --- SigMF ----------------------------------------------------------------

TEST(sigmf_writer_writes_data_captures_and_annotations) {
  SigmfWriter w;
  SigmfWriter::Global g;
  g.sampleRate = 250000;
  g.hardware = "test";
  const std::string base = "hfc_test_sigmf";
  CHECK(w.open(base, g, nullptr));
  std::vector<int16_t> iq = {1, -1, 32767, -32768, 256, -256};
  CHECK(w.write(iq.data(), 3, false, 7000000, 1.8e9));
  CHECK(w.write(iq.data(), 3, false, 7000000, 1.8e9));  // contiguous: same capture
  w.annotate(6, 0, "gap", "lost");
  CHECK(w.write(iq.data(), 3, true, 7100000, 1.8e9 + 1.25));
  // A gap found late, inside what is already written: kept in order.
  w.addCapture(4, 7000000, 1.8e9 + 0.5);
  w.annotate(4, 0, "gap", "found late");
  CHECK(w.close(nullptr));
  std::FILE* f = std::fopen((base + ".sigmf-data").c_str(), "rb");
  CHECK(f);
  if (f) {
    std::vector<uint8_t> bytes(100);
    const std::size_t n = std::fread(bytes.data(), 1, bytes.size(), f);
    std::fclose(f);
    CHECK(n == 9 * 4);
    CHECK(bytes[0] == 1 && bytes[1] == 0 && bytes[2] == 0xFF && bytes[3] == 0xFF);  // little endian
    CHECK(bytes[4] == 0xFF && bytes[5] == 0x7F && bytes[6] == 0x00 && bytes[7] == 0x80);
  }
  f = std::fopen((base + ".sigmf-meta").c_str(), "rb");
  std::string meta;
  if (f) {
    char buf[4096];
    meta.assign(buf, std::fread(buf, 1, sizeof(buf), f));
    std::fclose(f);
  }
  std::vector<JsonField> top;
  CHECK(parseJsonObject(meta, top, nullptr));
  CHECK(jsonField(top, "global").find("\"core:datatype\": \"ci16_le\"") != std::string::npos);
  CHECK(jsonField(top, "global").find("\"core:sample_rate\": 250000") != std::string::npos);
  const std::string caps = jsonField(top, "captures");
  CHECK(caps.find("\"core:sample_start\": 0") != std::string::npos);
  CHECK(caps.find("\"core:sample_start\": 6") != std::string::npos);
  CHECK(caps.find("\"core:sample_start\": 3") == std::string::npos);
  CHECK(caps.find("\"core:sample_start\": 4") < caps.find("\"core:sample_start\": 6"));
  const std::string notes = jsonField(top, "annotations");
  CHECK(notes.find("found late") < notes.find("\"lost\""));
  CHECK(caps.find("2027-01-15T08:00:01.250000Z") != std::string::npos);
  CHECK(jsonField(top, "annotations").find("\"core:label\": \"gap\"") != std::string::npos);
  std::remove((base + ".sigmf-data").c_str());
  std::remove((base + ".sigmf-meta").c_str());
}

TEST(sigmf_reader_reads_back_what_the_writer_wrote) {
  SigmfWriter w;
  SigmfWriter::Global g;
  g.sampleRate = 100;
  const std::string base = "hfc_test_sigmf_rw";
  CHECK(w.open(base, g, nullptr));
  std::vector<int16_t> a(2 * 150), b(2 * 50);
  for (std::size_t i = 0; i < a.size(); ++i) a[i] = static_cast<int16_t>(i * 7 - 1000);
  for (std::size_t i = 0; i < b.size(); ++i) b[i] = static_cast<int16_t>(-static_cast<int>(i) * 11);
  CHECK(w.write(a.data(), 150, true, 7000000, 1.8e9 + 0.25));
  CHECK(w.write(b.data(), 50, true, 7100000, 1.8e9 + 10.5));
  CHECK(w.close(nullptr));

  SigmfReader r;
  std::string err;
  CHECK(r.open(base + ".sigmf-meta", &err));
  CHECK(r.sampleRate() == 100 && r.datatype() == "ci16_le" && r.samples() == 200);
  CHECK(r.captures().size() == 2);
  // Reads stop at the start of the second capture.
  std::vector<int16_t> buf(2 * 1000);
  SigmfReader::Capture cap;
  double utc = 0;
  std::size_t n = r.read(buf.data(), 1000, cap, utc);
  CHECK(n == 150 && cap.centerHz == 7000000);
  CHECK_NEAR(utc, 1.8e9 + 0.25, 1e-6);
  CHECK(std::equal(a.begin(), a.end(), buf.begin()));
  n = r.read(buf.data(), 30, cap, utc);
  CHECK(n == 30 && cap.centerHz == 7100000);
  CHECK_NEAR(utc, 1.8e9 + 10.5, 1e-6);
  CHECK(std::equal(b.begin(), b.begin() + 60, buf.begin()));
  n = r.read(buf.data(), 30, cap, utc);
  CHECK(n == 20);
  CHECK_NEAR(utc, 1.8e9 + 10.5 + 0.3, 1e-6);  // continues inside the capture
  CHECK(r.read(buf.data(), 30, cap, utc) == 0);
  r.close();
  std::remove((base + ".sigmf-data").c_str());
  std::remove((base + ".sigmf-meta").c_str());
}

TEST(sigmf_reader_converts_other_datatypes) {
  struct Case {
    const char* type;
    std::vector<uint8_t> bytes;  // two IQ pairs
    std::vector<int16_t> expect;
  };
  float f[4] = {0.5f, -1.0f, 2.0f, std::nanf("")};
  std::vector<uint8_t> cf(16);
  std::memcpy(cf.data(), f, 16);
  const Case cases[] = {
      {"ci8", {0x7F, 0x80, 0x00, 0x01}, {127 * 256, -128 * 256, 0, 256}},
      {"cu8", {255, 0, 128, 129}, {127 * 256, -128 * 256, 0, 256}},
      {"cf32_le", cf, {16384, -32767, 32767, 0}},  // clipped beyond full scale; not-a-number is 0
  };
  for (const auto& c : cases) {
    const std::string base = std::string("hfc_test_sigmf_") + c.type;
    std::FILE* d = std::fopen((base + ".sigmf-data").c_str(), "wb");
    std::fwrite(c.bytes.data(), 1, c.bytes.size(), d);
    std::fclose(d);
    std::FILE* m = std::fopen((base + ".sigmf-meta").c_str(), "wb");
    const std::string meta = std::string("{\"global\": {\"core:datatype\": \"") + c.type +
                             "\", \"core:sample_rate\": 48000, \"core:version\": \"1.2.0\"}, \"captures\": "
                             "[{\"core:sample_start\": 0}], \"annotations\": []}";
    std::fwrite(meta.data(), 1, meta.size(), m);
    std::fclose(m);
    SigmfReader r;
    std::string err;
    CHECK(r.open(base, &err));
    std::vector<int16_t> buf(4);
    SigmfReader::Capture cap;
    double utc = -1;
    CHECK(r.read(buf.data(), 2, cap, utc) == 2);
    CHECK(buf == c.expect);
    CHECK(utc == 0);  // no time in the recording
    r.close();
    std::remove((base + ".sigmf-data").c_str());
    std::remove((base + ".sigmf-meta").c_str());
  }
}

TEST(iso_utc_parsing_is_strict) {
  CHECK_NEAR(parseIsoUtc("1970-01-01T00:00:00Z"), 0.0, 1e-9);
  CHECK_NEAR(parseIsoUtc("2026-10-19T12:34:56.123456Z"), 1792413296.123456, 1e-5);
  CHECK_NEAR(parseIsoUtc("2024-02-29T00:00:00+00:00"), 1709164800.0, 1e-9);
  CHECK(parseIsoUtc(isoUtc(1.8e9 + 0.5)) == 1.8e9 + 0.5);
  for (const char* bad : {"2026-10-19T12:34:inf", "2026-10-19T99:99:99Z", "2026-02-31T00:00:00Z",
                          "2025-02-29T00:00:00Z", "2026-10-19T12:34:56Zjunk", "2026-10-19T12:34:56+02:00",
                          "2026-10-19 12:34:56Z", "", "1969-12-31T23:59:59Z", "2026-10-19T12:34:56.Z"})
    CHECK(parseIsoUtc(bad) < 0);
  // Out-of-range times format as the epoch instead of overflowing.
  CHECK(isoUtc(std::nan("")) == "1970-01-01T00:00:00.000000Z");
  CHECK(isoUtc(1e300) == "1970-01-01T00:00:00.000000Z");
}

TEST(json_arrays_and_url_decoding) {
  std::vector<JsonField> el;
  CHECK(parseJsonArray("[1, \"a\\\"b\", {\"k\": [2, 3]}, [], null]", el, nullptr));
  CHECK(el.size() == 5);
  if (el.size() == 5) {
    CHECK(el[0].value == "1" && !el[0].isString);
    CHECK(el[1].value == "a\"b" && el[1].isString);
    CHECK(el[2].value == "{\"k\": [2, 3]}");
    CHECK(el[3].value == "[]" && el[4].value == "null");
  }
  CHECK(parseJsonArray("[]", el, nullptr) && el.empty());
  CHECK(!parseJsonArray("[1,]", el, nullptr));
  CHECK(!parseJsonArray("[1] x", el, nullptr));
  CHECK(urlDecode("a%20b+c%2Fd", true) == "a b c/d");
  CHECK(urlDecode("a+b%zz%4", false) == "a+b%zz%4");  // malformed escapes stay as they are
}

TEST(iso_utc_formats_microseconds) {
  CHECK(isoUtc(0.0) == "1970-01-01T00:00:00.000000Z");
  CHECK(isoUtc(1.8e9 + 0.9999996) == "2027-01-15T08:00:01.000000Z");
}

// --- the client against the fake SDRconnect -------------------------------

namespace {

testing::FakeConfig fastFake() {
  testing::FakeConfig cfg;
  cfg.reportDelaySeconds = 0.05;
  cfg.sampleRate = 250000;
  cfg.tones.push_back({30000, -20, 0, 0});
  return cfg;
}

}  // namespace

TEST(client_reads_and_sets_properties) {
  namespace sc = sdrconnect;
  testing::FakeSdrconnect fake(fastFake());
  CHECK(fake.start(nullptr));
  sc::Client c;
  std::string err;
  CHECK(c.connect("127.0.0.1", fake.port(), 3000, &err));
  CHECK(c.query(sc::prop::kApiVersion, sc::Device::Primary, 2000).value_or("") == "1.0.3");
  // connect() asked for the rate and centre frequency.
  CHECK(waitFor([&] { return c.cached(sc::prop::kSampleRate).has_value(); }, 2000));
  CHECK(c.cached(sc::prop::kSampleRate).value_or("") == "250000");
  CHECK(c.cached(sc::prop::kCenterFrequency).value_or("") == "7000000");
  // Accepted: reported by property_changed.
  CHECK(c.setAndConfirm(sc::prop::kSampleRate, "500000", sc::Device::Primary, 2000).value_or("") == "500000");
  // Refused: no answer at all; setAndConfirm then asks and gets the old value.
  CHECK(c.setAndConfirm(sc::prop::kSampleRate, "3000000", sc::Device::Primary, 300).value_or("") == "500000");
  // Unchanged: no property_changed either.
  CHECK(c.setAndConfirm(sc::prop::kSampleRate, "500000", sc::Device::Primary, 300).value_or("") == "500000");
  // Read-only properties are not changed.
  CHECK(c.setAndConfirm(sc::prop::kApiVersion, "9.9.9", sc::Device::Primary, 300).value_or("") == "1.0.3");
  // Unknown properties get no answer.
  CHECK(!c.query("no_such_property", sc::Device::Primary, 300).has_value());
  // No secondary device outside dual-tuner mode.
  CHECK(!c.query(sc::prop::kSampleRate, sc::Device::Secondary, 300).has_value());
  c.disconnect();
  fake.stop();
}

TEST(client_receives_iq_at_the_reported_rate) {
  namespace sc = sdrconnect;
  testing::FakeSdrconnect fake(fastFake());
  CHECK(fake.start(nullptr));
  std::mutex mu;
  uint64_t pairs = 0, firstIndex = 99, blocks = 0;
  double rate = 0, centre = 0, maxAbs = 0;
  sc::Client::Handlers h;
  h.onIq = [&](sc::Device d, const sc::IqBlock& b) {
    std::lock_guard<std::mutex> lock(mu);
    if (d != sc::Device::Primary) return;
    if (blocks++ == 0) firstIndex = b.index;
    pairs += b.pairs;
    rate = b.sampleRate;
    centre = b.centerHz;
    for (std::size_t i = 0; i < 2 * b.pairs; ++i) maxAbs = std::max(maxAbs, std::fabs(double(b.iq[i])));
  };
  sc::Client c(h);
  CHECK(c.connect("127.0.0.1", fake.port(), 3000, nullptr));
  CHECK(waitFor([&] { return c.cached(sc::prop::kSampleRate).has_value(); }, 2000));
  CHECK(c.enableIq(true));
  sleepMs(1500);
  c.resetIqStats(sc::Device::Primary);
  sleepMs(2000);
  const auto st = c.iqStats(sc::Device::Primary);
  c.disconnect();
  fake.stop();
  std::lock_guard<std::mutex> lock(mu);
  CHECK(firstIndex == 0);
  CHECK(rate == 250000 && centre == 7000000);
  CHECK(maxAbs > 1000);  // the -20 dBFS tone is there
  CHECK_NEAR(st.measuredRate, 250000, 250000 * 0.15);
  CHECK(st.gaps == 0);
  CHECK(c.binaryMessages(sc::BinaryType::IqPrimary) >= 50);
}

TEST(client_detects_iq_the_server_never_sent) {
  namespace sc = sdrconnect;
  testing::FakeConfig cfg = fastFake();
  cfg.dropAfterSeconds = 1.5;
  cfg.dropSeconds = 0.6;
  testing::FakeSdrconnect fake(cfg);
  CHECK(fake.start(nullptr));
  std::atomic<int> flagged{0};
  sc::Client::Handlers h;
  h.onIq = [&](sc::Device, const sc::IqBlock& b) {
    if (b.discontinuity) ++flagged;
  };
  sc::Client c(h);
  CHECK(c.connect("127.0.0.1", fake.port(), 3000, nullptr));
  CHECK(waitFor([&] { return c.cached(sc::prop::kSampleRate).has_value(); }, 2000));
  c.enableIq(true);
  sleepMs(4500);
  const auto st = c.iqStats(sc::Device::Primary);
  c.disconnect();
  fake.stop();
  CHECK(fake.pairsDropped(0) > 0);
  CHECK(st.gaps == 1);
  CHECK_NEAR(st.lostSeconds, 0.6, 0.25);
  CHECK(flagged >= 1);
}

TEST(client_dual_tuner_streams_both_devices) {
  namespace sc = sdrconnect;
  testing::FakeSdrconnect fake(fastFake());
  CHECK(fake.start(nullptr));
  std::atomic<uint64_t> pairs[2] = {};
  sc::Client::Handlers h;
  h.onIq = [&](sc::Device d, const sc::IqBlock& b) { pairs[d == sc::Device::Primary ? 0 : 1] += b.pairs; };
  sc::Client c(h);
  CHECK(c.connect("127.0.0.1", fake.port(), 3000, nullptr));
  // As in SDRplay's MinimalDuoExample.
  c.selectDeviceSerial("1806030000:Dual");
  c.enableDevice(sc::Device::Primary, true);
  c.enableDevice(sc::Device::Secondary, true);
  c.enableIq(true);
  CHECK(waitFor([&] { return fake.dualMode(); }, 2000));
  CHECK(c.query(sc::prop::kSampleRate, sc::Device::Secondary, 2000).value_or("") == "2000000");
  // The two tuners are tuned independently.
  CHECK(c.setAndConfirm(sc::prop::kCenterFrequency, "9000000", sc::Device::Secondary, 2000).value_or("") ==
        "9000000");
  CHECK(c.query(sc::prop::kCenterFrequency, sc::Device::Primary, 2000).value_or("") == "7000000");
  CHECK(waitFor([&] { return pairs[0] > 200000 && pairs[1] > 200000; }, 4000));
  // Each stream is clocked separately.
  CHECK(c.iqStats(sc::Device::Secondary).nominalRate == 2000000);
  // Filtering out the secondary device stops its IQ.
  c.enableDevice(sc::Device::Secondary, false);
  sleepMs(300);
  const uint64_t before = pairs[1];
  sleepMs(500);
  CHECK(pairs[1] == before);
  c.disconnect();
  fake.stop();
}

TEST(client_cannot_change_a_read_only_sdrconnect) {
  namespace sc = sdrconnect;
  testing::FakeConfig cfg = fastFake();
  cfg.canControl = false;
  testing::FakeSdrconnect fake(cfg);
  CHECK(fake.start(nullptr));
  sc::Client c;
  CHECK(c.connect("127.0.0.1", fake.port(), 3000, nullptr));
  CHECK(c.query(sc::prop::kCanControl, sc::Device::Primary, 2000).value_or("") == "false");
  CHECK(c.setAndConfirm(sc::prop::kCenterFrequency, "8000000", sc::Device::Primary, 300).value_or("") == "7000000");
  c.disconnect();
  fake.stop();
}

TEST(client_reports_a_server_that_goes_away) {
  namespace sc = sdrconnect;
  auto fake = std::make_unique<testing::FakeSdrconnect>(fastFake());
  CHECK(fake->start(nullptr));
  std::atomic<bool> gone{false};
  sc::Client::Handlers h;
  h.onDisconnected = [&](const std::string&) { gone = true; };
  sc::Client c(h);
  CHECK(c.connect("127.0.0.1", fake->port(), 3000, nullptr));
  fake->stop();
  CHECK(waitFor([&] { return gone.load(); }, 3000));
  CHECK(!c.connected());
  // Nothing listens any more.
  CHECK(!c.connect("127.0.0.1", 1, 500, nullptr));
}

int main(int argc, char** argv) {
  // Line by line, also into a file or pipe: a test that hangs (and is
  // killed by a timeout) is then the one after the last line printed.
  std::setvbuf(stdout, nullptr, _IOLBF, 1 << 12);
  const char* filter = argc > 1 ? argv[1] : nullptr;
  int run = 0;
  for (const auto& t : hfc::test::registry()) {
    if (filter && !std::strstr(t.name, filter)) continue;
    const int before = hfc::test::g_failures;
    const auto t0 = std::chrono::steady_clock::now();
    t.fn();
    const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::printf("%s %s (%.2f s)\n", hfc::test::g_failures == before ? "ok  " : "FAIL", t.name, s);
    ++run;
  }
  std::printf("%d tests, %d failed checks\n", run, hfc::test::g_failures);
  return hfc::test::g_failures == 0 ? 0 : 1;
}
