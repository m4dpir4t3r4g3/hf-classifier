#include "navtex/ale.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "navtex/ale2g.hpp"
#include "navtex/ale_frontend.hpp"
#include "navtex/ale_psk.hpp"

namespace navtex {

using namespace ale;

struct AleDecoder::Impl : Ale2gReceiver::Listener, Ale2gAssembler::Listener, AlePskReceiver::Listener {
  explicit Impl(AleDecoder& d) : dec(d) {
    rx2g.setListener(this);
    assembler.setListener(this);
    psk.setListener(this);
  }

  void onWord(const Word2g& w) override {
    ++dec.status_.words2g;
    if (dec.cfg_.showWords) {
      AleEvent e;
      e.kind = AleEvent::Word2g;
      e.generation = AleGeneration::G2;
      e.time = w.time;
      e.word = w.word;
      e.quality = w.quality;
      e.offsetHz = w.offsetHz;
      std::snprintf(e.waveform, sizeof e.waveform, "2G");
      std::snprintf(e.text, sizeof e.text, "%s  (%d/48 votes unanimous, %d corrected)",
                    Ale2gAssembler::describeWord(w.word).c_str(), w.unanimous, w.corrected);
      dec.emit(e);
    }
    assembler.onWord(w);
  }

  void onFrame(const Ale2gAssembler::Frame& f) override {
    ++dec.status_.frames2g;
    AleEvent e;
    e.kind = AleEvent::Frame2g;
    e.generation = AleGeneration::G2;
    e.time = f.time;
    e.quality = f.quality;
    std::snprintf(e.waveform, sizeof e.waveform, "2G");
    std::snprintf(e.text, sizeof e.text, "%s", f.text.c_str());
    dec.emit(e);
  }

  void onBurst(const PskBurst& b) override {
    const bool g3 = b.waveform == PskWaveform::Bw0 || b.waveform == PskWaveform::Bw1;
    if (b.ok) ++(g3 ? dec.status_.pdus3g : dec.status_.pdus4g);
    else ++dec.status_.failed;
    if (!b.ok && !dec.cfg_.showFailed) return;
    AleEvent e;
    e.kind = b.ok ? AleEvent::Pdu : AleEvent::Failed;
    e.generation = g3 ? AleGeneration::G3 : AleGeneration::G4;
    e.time = b.time;
    e.quality = b.quality;
    e.intact = b.intact;
    e.snrDb = b.snrDb;
    e.offsetHz = b.offsetHz;
    std::snprintf(e.waveform, sizeof e.waveform, "%s", waveformName(b.waveform));
    std::snprintf(e.text, sizeof e.text, "%s", b.description.c_str());
    if (g3) {
      e.bits = static_cast<int>(b.bits.size());
      for (std::size_t i = 0; i < b.bits.size() && i / 8 < sizeof e.octets; ++i)
        if (b.bits[i]) e.octets[i / 8] |= static_cast<std::uint8_t>(0x80u >> (i % 8));
    } else {
      e.bits = 96;
      std::memcpy(e.octets, b.pdu.data(), 12);
    }
    dec.emit(e);
  }

  AleDecoder& dec;
  AleFrontEnd frontEnd;
  RrcFilter rrc;
  AleSpectrum spectrum;
  SpectrumFrame frame;
  Ale2gReceiver rx2g;
  Ale2gAssembler assembler;
  AlePskReceiver psk;
  long long basebandSamples = 0;
  long long inputSamples = 0;
  double levelSum = 0.0;
  long long levelCount = 0;
};

AleDecoder::AleDecoder(const AleConfig& config) : impl_(std::make_unique<Impl>(*this)) {
  cfg_.sampleRate = 0;  // force the front end's set-up
  configure(config);
}

AleDecoder::~AleDecoder() = default;

void AleDecoder::configure(const AleConfig& config) {
  const bool newRate = config.sampleRate != cfg_.sampleRate;
  cfg_ = config;
  if (cfg_.channels < 1) cfg_.channels = 1;
  if (newRate) {
    impl_->frontEnd.configure(cfg_.sampleRate);
    reset();
  }
  impl_->psk.enable(cfg_.enable3g, cfg_.enable4g);
  if (!cfg_.enable2g) impl_->assembler.flush();
}

void AleDecoder::reset() {
  impl_->assembler.flush();
  impl_->frontEnd.reset();
  impl_->rrc.reset();
  impl_->spectrum.reset();
  impl_->rx2g.reset();
  impl_->assembler.reset();
  impl_->psk.reset();
  impl_->psk.enable(cfg_.enable3g, cfg_.enable4g);
  impl_->basebandSamples = 0;
  impl_->inputSamples = 0;
  status_ = AleStatus();
}

void AleDecoder::process(const float* samples, std::size_t frames) {
  Impl& m = *impl_;
  const int ch = cfg_.channels;
  cf out[4];
  for (std::size_t f = 0; f < frames; ++f) {
    float x = 0.0f;
    for (int c = 0; c < ch; ++c) x += samples[f * static_cast<std::size_t>(ch) + static_cast<std::size_t>(c)];
    x /= static_cast<float>(ch);
    m.levelSum += static_cast<double>(x) * x;
    if (++m.levelCount >= 4800) {
      double p = m.levelSum / static_cast<double>(m.levelCount);
      status_.levelDb = p > 1e-20 ? 10.0 * std::log10(p) : -200.0;
      m.levelSum = 0.0;
      m.levelCount = 0;
    }
    ++m.inputSamples;
    const int n = m.frontEnd.push(x, out);
    for (int i = 0; i < n; ++i) {
      const double t = static_cast<double>(m.basebandSamples++) / kBasebandRate;
      if (cfg_.enable2g) m.rx2g.process(out[i], t);
      if (cfg_.enable3g || cfg_.enable4g) m.psk.process(m.rrc.process(out[i]), t);
      if (m.spectrum.push(out[i], m.frame) && sink_) sink_->onSpectrum(m.frame);
      if ((m.basebandSamples & 127) == 0) {
        m.assembler.tick(t);
        status_.streamTime = t;
        const double last = std::max({m.rx2g.lastWordTime(), m.psk.lastDetection(), status_.lastTime});
        status_.signalPresent = t - last < 3.0;
      }
    }
  }
}

void AleDecoder::flush() { impl_->assembler.flush(); }

AleStatus AleDecoder::status() const { return status_; }

double AleDecoder::streamTime() const { return static_cast<double>(impl_->basebandSamples) / kBasebandRate; }

void formatAleLine(const AleEvent& e, char* line, std::size_t size) {
  const char* gen = e.generation == AleGeneration::G2 ? "" : e.generation == AleGeneration::G3 ? "3G " : "4G ";
  if (e.kind == AleEvent::Pdu || e.kind == AleEvent::Failed)
    std::snprintf(line, size, "%s%s  %s  [SNR %.0f dB, %+.0f Hz]", gen, e.waveform, e.text, e.snrDb, e.offsetHz);
  else
    std::snprintf(line, size, "%s%s  %s", e.waveform, e.kind == AleEvent::Word2g ? " word" : "", e.text);
}

void AleDecoder::emit(const AleEvent& e) {
  status_.lastTime = e.time;
  std::snprintf(status_.last, sizeof status_.last, "%.15s  %.78s", e.waveform, e.text);
  if (aleSink_) aleSink_->onAle(e);
  if (!sink_) return;
  char line[400];
  formatAleLine(e, line, sizeof line);
  DecodedChar c;
  c.time = e.time;
  c.confidence = e.kind == AleEvent::Failed ? 0.0f : std::max(0.5f, e.quality);
  c.corrected = e.kind == AleEvent::Failed;
  for (const char* p = line; *p; ++p) {
    c.text[0] = *p;
    sink_->onCharacter(c);
  }
  c.text[0] = '\n';
  sink_->onCharacter(c);
}

}  // namespace navtex
