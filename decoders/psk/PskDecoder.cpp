#include "PskDecoder.h"

#include <algorithm>
#include <cmath>

namespace psk {

namespace {

// Rate the input audio is first reduced to.
constexpr double kTargetRateA = 8000.0;
// Upper edge of the audio band that survives stage A.
constexpr double kAudioBandHz = 3500.0;
// Samples per symbol aimed for after stage B.
constexpr double kTargetSps = 16.0;
// Length of the timing-recovery history, in stage-B samples.
constexpr size_t kHistory = 256;
// Gardner loop gain (fraction of a symbol per unit of normalised error).
constexpr double kTimingGain = 0.01;
// AFC loop gain per symbol.
constexpr double kAfcGain = 0.03;
// AFC only runs above this quality, so it does not wander on noise.
constexpr float kAfcMinQuality = 0.4f;
// QPSK's quality (the fourth power) reads much lower on a weak signal than
// BPSK's, though the code still decodes it. There the AFC runs above a lower
// quality while the symbols also form code sequences (the Viterbi decoder's
// recent path-metric growth, 1 at best, about 0.72 on noise), and a signal is
// taken as lost, or held a quarter of a baud off (about 0.73), when they do
// not.
constexpr float kQpskAfcMinQuality = 0.15f;
constexpr float kQpskLockedScore = 0.85f;
constexpr float kQpskLostScore = 0.8f;
// QPSK idles (and starts) with phase reversals, as BPSK does: there the
// squared phase change measures the frequency error over BPSK's wider range
// (+/- baud/4). Averaged over about 8 symbols, short enough to catch
// QPSK31's one-second preamble; idle above this level.
constexpr float kIdleAlpha = 1.0f / 8.0f;
constexpr float kIdleQuality = 0.6f;
// QPSK's AFC acts only once its conditions have held for this many symbols
// in a row: noise meets either now and then, a signal keeps meeting them.
constexpr int kQpskAfcRunSymbols = 16;
// Coarse (spectrum-based) AFC: frames to average before the first estimate
// after tuning and between later corrections, and the offset (in baud)
// beyond which it re-centres the carrier.
constexpr int kAcquireFrames = 10;
// Above 125 baud fewer frames are averaged after tuning (but at least this
// many: fewer leave noise looking like a signal somewhere in the wide band
// searched), so the faster modes are not decoded for hundreds of symbols on
// the wrong frequency.
constexpr int kMinAcquireFrames = 5;
constexpr int kCoarseAfcFrames = 6;
// Frames (about 0.8 s) of low quality before a lost signal is searched for.
constexpr int kReacquireFrames = 12;
// It must stay inside the phase AFC's pull-in range (BPSK: +/- baud/4; from
// further away it locks half a baud off, where every symbol looks like the
// opposite one). QPSK's phase AFC pulls in from only +/- baud/8 (and locks a
// quarter of a baud off from further away).
constexpr double kCoarseAfcThreshold = 0.2;
constexpr double kQpskCoarseAfcThreshold = 0.1;
// How far (dB) a signal must stand above the noise floor for the coarse AFC
// to move to it: right after tuning, and when re-acquiring a lost signal.
constexpr float kAcquirePeakDb = 8.0f;
constexpr float kReacquirePeakDb = 12.0f;
// QPSK: a signal this far (dB) above the noise floor gives a spectrum
// estimate good enough to act on without other evidence.
constexpr float kStrongPeakDb = 15.0f;
// The squelch closes this far below its opening threshold, so it does not
// flicker when the quality hovers around the threshold.
constexpr float kSquelchHysteresis = 0.1f;
// Decoded characters are held back this many symbols, long enough for the
// signal-loss detector to catch the end of a transmission first.
constexpr long long kHoldSymbols = 10;
// Symbols the quality must stay above the threshold before the squelch opens.
constexpr int kSquelchOpenSymbols = 8;
// Signal-loss detection: fast symbol-power average (a few symbols), the slow
// average that learns the signal level while the squelch is open, and the
// drop (about -8 dB) that counts as the signal having gone.
constexpr float kFastPowerAlpha = 0.3f;
constexpr float kSignalPowerAlpha = 0.05f;
constexpr float kSignalLostRatio = 0.15f;
// Quality averaging length in symbols. Noise false-alarm behaviour depends on
// the number of symbols averaged, so a fixed count keeps the squelch equally
// safe in every mode while opening faster on the faster modes.
constexpr double kQualitySymbols = 32.0;
// QPSK sense of rotation: averaging length (symbols) of each Viterbi
// decoder's path-metric growth, the lead the other sense needs, and the
// symbols in a row it must keep it (while reading as a code sequence,
// kQpskLockedScore) before it is switched to. On noise either sense leads
// now and then, briefly: without the run, the sense flipped there 1 to 30
// times a minute, and the next transmission started in the wrong one.
constexpr float kSenseAlpha = 1.0f / 16.0f;
constexpr float kSenseMargin = 0.1f;
constexpr int kSenseRunSymbols = 16;
// QPSK soft decisions: the phase change is scaled by its average size, and a
// burst of noise may count at most this many average symbols.
constexpr float kMaxSoftMagnitude = 2.0f;
// The raised-cosine pulses, through the matched filter, leak this fraction of
// each symbol into its neighbours at the sampling instants. Harmless to BPSK
// (the leak is in phase or opposite), it turns QPSK phases by up to 37
// degrees; a three-tap equaliser takes it out (QPSK only).
constexpr float kNeighbourLeak = 1.0f / 6.0f;

// The spectrum stage moves the carrier at all only when something stands this
// far (dB) above the noise floor within a baud of it, in the averaged
// spectrum: noise always has some peak for it to land on otherwise. (In
// audio band-limited to a channel, noise moved it in up to 38 of 40 tries.)
constexpr float kPresenceDb = 8.0f;

int OddTaps(double n, int minTaps, int maxTaps)
{
	int taps = static_cast<int>(std::lround(n)) | 1;
	return std::clamp(taps, minTaps | 1, maxTaps | 1);
}

} // namespace

double BaudRate(Mode mode)
{
	switch (mode)
	{
	case Mode::Bpsk63:
	case Mode::Qpsk63:
		return 62.5;
	case Mode::Bpsk125:
	case Mode::Qpsk125:
		return 125.0;
	case Mode::Bpsk250:
	case Mode::Qpsk250:
		return 250.0;
	case Mode::Bpsk500:
	case Mode::Qpsk500:
		return 500.0;
	case Mode::Bpsk31:
	case Mode::Qpsk31:
	default:
		return 31.25;
	}
}

const char* ModeName(Mode mode)
{
	switch (mode)
	{
	case Mode::Bpsk63:
		return "BPSK63";
	case Mode::Bpsk125:
		return "BPSK125";
	case Mode::Bpsk250:
		return "BPSK250";
	case Mode::Bpsk500:
		return "BPSK500";
	case Mode::Qpsk31:
		return "QPSK31";
	case Mode::Qpsk63:
		return "QPSK63";
	case Mode::Qpsk125:
		return "QPSK125";
	case Mode::Qpsk250:
		return "QPSK250";
	case Mode::Qpsk500:
		return "QPSK500";
	case Mode::Bpsk31:
	default:
		return "BPSK31";
	}
}

bool IsQpsk(Mode mode)
{
	switch (mode)
	{
	case Mode::Qpsk31:
	case Mode::Qpsk63:
	case Mode::Qpsk125:
	case Mode::Qpsk250:
	case Mode::Qpsk500:
		return true;
	default:
		return false;
	}
}

Decoder::Decoder()
{
	Configure(48000.0, Mode::Bpsk31);
}

void Decoder::Configure(double sampleRate, Mode mode)
{
	m_sampleRate = sampleRate;
	m_mode = mode;
	m_baud = BaudRate(mode);
	m_qpsk = IsQpsk(mode);

	// Stage A: keep 0..~3.5 kHz and drop to ~8 kHz.
	m_factorA = std::max(1, static_cast<int>(std::floor(sampleRate / kTargetRateA)));
	m_rateA = sampleRate / m_factorA;
	if (m_factorA > 1)
	{
		const double stopHz = m_rateA - kAudioBandHz;
		const double transition = std::max(200.0, stopHz - kAudioBandHz);
		const int taps = OddTaps(5.5 * sampleRate / transition, 31, 2047);
		m_decimA.Init(DesignLowpass(taps, 0.5 * (kAudioBandHz + stopHz), sampleRate), m_factorA);
	}

	const size_t fftSize = m_rateA >= 6000.0 ? 1024 : 512;
	m_spectrum.Init(fftSize, m_rateA);

	// Stage B: complex low-pass around the carrier, down to ~16 samples/symbol.
	const int factorB = std::max(1, static_cast<int>(std::floor(m_rateA / (kTargetSps * m_baud))));
	m_rateB = m_rateA / factorB;
	m_sps = m_rateB / m_baud;
	{
		const double cutoff = 2.0 * m_baud;
		const int taps = OddTaps(5.5 * m_rateA / (1.5 * m_baud), 31, 2047);
		m_decimB.Init(DesignLowpass(taps, cutoff, m_rateA), factorB);
	}

	// Matched filter: PSK31 symbols (and those of the other modes, QPSK
	// included) are raised-cosine pulses two symbols long, so the matched
	// filter is a Hann window of the same length.
	{
		const int taps = OddTaps(2.0 * m_sps, 3, 1023);
		std::vector<float> h(static_cast<size_t>(taps));
		double sum = 0.0;
		for (int i = 0; i < taps; ++i)
		{
			h[static_cast<size_t>(i)] = static_cast<float>(0.5 - 0.5 * std::cos(2.0 * kPi * (i + 1) / (taps + 1)));
			sum += h[static_cast<size_t>(i)];
		}
		for (auto& v : h)
		{
			v = static_cast<float>(v / sum);
		}
		m_matched.Init(std::move(h), 1);
	}

	m_history.assign(kHistory, std::complex<float>{});
	m_sampleIndex = -1;
	m_nextStrobe = m_sps;
	m_prevSymbol = {};
	m_prevStrobe = {};
	m_strobes[0] = m_strobes[1] = {};
	m_symbolPower = 1e-12f;
	m_zmAvg = {};
	m_afcAvg = {};
	m_idleAvg = {};
	m_quality = 0.0f;
	m_fastPower = 0.0f;
	m_signalPower = 0.0f;
	m_symbolsAbove = 0;
	m_held.clear();
	SetSquelchState(m_squelch <= 0.0f);
	m_varicode.Reset();
	ResetQpsk();
	m_sense = 0;
	m_avgPower.clear();
	m_coarseHoldoff = 0;
	m_lowQualityFrames = 0;
	m_acquiring = true;
	m_carrierHz = std::clamp(m_carrierHz, m_baud, MaxCarrier());
}

double Decoder::MaxCarrier() const
{
	return std::min(kAudioBandHz, 0.5 * m_rateA - 2.0 * m_baud);
}

void Decoder::SetCarrier(double hz)
{
	m_carrierHz = std::clamp(hz, m_baud, MaxCarrier());
}

void Decoder::Tune(double hz)
{
	SetCarrier(hz);
	// Give the new signal a fresh start: forget the old spectrum average and
	// quality history.
	m_avgPower.clear();
	m_coarseHoldoff = 0;
	m_lowQualityFrames = 0;
	m_acquiring = true;
	m_zmAvg = {};
	m_afcAvg = {};
	m_idleAvg = {};
	m_quality = 0.0f;
	m_fastPower = 0.0f;
	m_signalPower = 0.0f;
	m_symbolsAbove = 0;
	m_held.clear();
	SetSquelchState(m_squelch <= 0.0f);
	m_varicode.Reset();
	ResetQpsk();
}

double Decoder::QpskAcquireTarget(double centre) const
{
	const double threshold = kQpskCoarseAfcThreshold * m_baud;
	// The spectrum's estimate where the signal stands well clear of the
	// noise (on the preamble's two tones good to about baud/10).
	const double strong = FindSignalCentre(m_avgDb, m_spectrum.BinHz(), m_carrierHz, 1.5 * m_baud, m_baud, kStrongPeakDb);
	// Idle reversals (the preamble) measure the error precisely, but only
	// modulo half a baud; a clear spectrum picks which.
	if (std::abs(m_idleAvg) >= kIdleQuality)
	{
		double error = 0.5 * std::arg(m_idleAvg) * m_baud / (2.0 * kPi);
		if (strong != m_carrierHz)
		{
			error += 0.5 * m_baud * std::round((strong - m_carrierHz - error) / (0.5 * m_baud));
		}
		return std::fabs(error) > threshold ? m_carrierHz + error : m_carrierHz;
	}
	// On data the clear spectrum's estimate is acted on alone; a noisier
	// one only where the symbols do not form code sequences at the present
	// frequency (on a weak signal that decodes, it would only move the
	// carrier off it) and a signal does stand out there in the averaged
	// spectrum: noise forms no code sequences either, and its spectrum
	// always has some peak.
	if (std::fabs(strong - m_carrierHz) > kCoarseAfcThreshold * m_baud)
	{
		return strong;
	}
	if (std::fabs(centre - m_carrierHz) > threshold && QpskScore() < kQpskLockedScore
		&& SignalNearCarrier(m_avgDb, kPresenceDb))
	{
		return centre;
	}
	return m_carrierHz;
}

float Decoder::QpskScore() const
{
	return std::max(m_senseScore[0], m_senseScore[1]);
}

void Decoder::ResetQpsk()
{
	m_viterbi[0].Reset();
	m_viterbi[1].Reset();
	m_senseScore[0] = m_senseScore[1] = 0.0f;
	m_senseLeadRun = 0;
	m_zMagnitude = 0.0f;
	m_idleRun = 0;
	m_lockedRun = 0;
	m_qpskSkipBits = 0;
}

void Decoder::Process(const float* interleaved, size_t frames, int channels)
{
	if (channels < 1)
	{
		return;
	}
	const float norm = 1.0f / static_cast<float>(channels);
	for (size_t f = 0; f < frames; ++f)
	{
		float x = 0.0f;
		for (int c = 0; c < channels; ++c)
		{
			x += interleaved[f * static_cast<size_t>(channels) + static_cast<size_t>(c)];
		}
		x *= norm;

		if (m_factorA == 1)
		{
			ProcessDecimated(x);
		}
		else
		{
			float y;
			if (m_decimA.Push(x, y))
			{
				ProcessDecimated(y);
			}
		}
	}
}

void Decoder::ProcessDecimated(float x)
{
	if (m_spectrum.Push(x, m_spectrumFrame))
	{
		CoarseAfc(m_spectrumFrame);
		if (m_onSpectrum)
		{
			m_onSpectrum(m_spectrumFrame, m_spectrum.BinHz());
		}
	}

	const std::complex<float> lo(static_cast<float>(std::cos(m_ncoPhase)), static_cast<float>(-std::sin(m_ncoPhase)));
	m_ncoPhase += 2.0 * kPi * m_carrierHz / m_rateA;
	if (m_ncoPhase > 2.0 * kPi)
	{
		m_ncoPhase -= 2.0 * kPi;
	}

	std::complex<float> bb;
	if (!m_decimB.Push(x * lo, bb))
	{
		return;
	}
	std::complex<float> filtered;
	m_matched.Push(bb, filtered);
	ProcessBaseband(filtered);
}

void Decoder::CoarseAfc(const std::vector<float>& frameDb)
{
	// The phase-based AFC only pulls in from within +/- baud/4, and can lock
	// half a baud off (where every symbol looks like a reversal). The
	// averaged spectrum is used to put the carrier on the signal's centre
	// right after tuning, and again whenever the signal has been lost.
	if (m_avgPower.size() != frameDb.size())
	{
		m_avgPower.assign(frameDb.size(), 0.0f);
		m_avgDb.assign(frameDb.size(), 0.0f);
		m_avgFrames = 0;
	}
	const float alpha = m_avgFrames == 0 ? 1.0f : 0.25f;
	for (size_t i = 0; i < frameDb.size(); ++i)
	{
		m_avgPower[i] += alpha * (std::pow(10.0f, frameDb[i] / 10.0f) - m_avgPower[i]);
	}
	++m_avgFrames;
	if (m_coarseHoldoff > 0)
	{
		--m_coarseHoldoff;
	}
	if (!m_afc)
	{
		m_acquiring = false;
		return;
	}
	// Re-acquisition is for a signal that is still there but has drifted
	// away, so it waits until a signal has been visible near the carrier
	// with low quality for a while. (At the start of every transmission the
	// quality is low for a moment too, while the spectrum average still
	// mostly holds the noise from before.)
	const bool signalNear = SignalNearCarrier(frameDb, kReacquirePeakDb);
	const bool lowQuality = m_qpsk ? QpskScore() < kQpskLostScore : m_quality < kAfcMinQuality;
	m_lowQualityFrames = (signalNear && lowQuality) ? m_lowQualityFrames + 1 : 0;
	const int acquireFrames = m_baud > 125.0
		? std::max(kMinAcquireFrames, static_cast<int>(std::lround(kAcquireFrames * 125.0 / m_baud)))
		: kAcquireFrames;
	if (m_avgFrames < (m_acquiring ? acquireFrames : kCoarseAfcFrames) || m_coarseHoldoff > 0
		|| (!m_acquiring && m_lowQualityFrames < kReacquireFrames))
	{
		return;
	}

	for (size_t i = 0; i < m_avgPower.size(); ++i)
	{
		m_avgDb[i] = 10.0f * std::log10(m_avgPower[i] + 1e-20f);
	}
	const double binHz = m_spectrum.BinHz();
	const double threshold = (m_qpsk ? kQpskCoarseAfcThreshold : kCoarseAfcThreshold) * m_baud;
	if (m_acquiring)
	{
		// Small corrections are left to the phase AFC, which is more precise
		// than the spectrum estimate on a noisy signal. QPSK's phase AFC
		// reaches less far (QpskAcquireTarget).
		const double centre = FindSignalCentre(m_avgDb, binHz, m_carrierHz, 1.5 * m_baud, m_baud, kAcquirePeakDb);
		double target = m_carrierHz;
		if (!m_qpsk)
		{
			if (std::fabs(centre - m_carrierHz) > kCoarseAfcThreshold * m_baud && SignalNearCarrier(m_avgDb, kPresenceDb))
			{
				target = centre;
			}
		}
		else
		{
			target = QpskAcquireTarget(centre);
		}
		if (target != m_carrierHz)
		{
			// The jump garbles the symbol in progress.
			SetCarrier(target);
			m_varicode.Resync();
			m_afcAvg = {};
			m_idleAvg = {};
		}
		m_acquiring = false;
		return;
	}

	// Signal lost: look for it nearby, but only for a clear signal so the
	// carrier does not wander on noise.
	const double centre = FindSignalCentre(m_avgDb, binHz, m_carrierHz, m_baud, m_baud, kReacquirePeakDb);
	if (std::fabs(centre - m_carrierHz) > threshold)
	{
		SetCarrier(centre);
		m_varicode.Resync();
		m_afcAvg = {};
		m_idleAvg = {};
		m_coarseHoldoff = kCoarseAfcFrames;
	}
}

void Decoder::UpdateSquelch(float symbolPower)
{
	m_fastPower += kFastPowerAlpha * (symbolPower - m_fastPower);
	if (m_squelch <= 0.0f)
	{
		SetSquelchState(true);
		return;
	}
	if (!m_squelchOpen)
	{
		// Noise can briefly push the quality over the threshold; a real
		// signal stays there.
		m_symbolsAbove = m_quality >= m_squelch ? m_symbolsAbove + 1 : 0;
		if (m_symbolsAbove >= kSquelchOpenSymbols)
		{
			m_signalPower = m_fastPower;
			// Any character in progress started before the signal was
			// trusted. QPSK: the Viterbi decoder's bits lag by its path
			// memory, so the next that many came before the signal was
			// trusted, some perhaps before it began.
			m_varicode.Resync();
			m_qpskSkipBits = m_qpsk ? QpskViterbi::kDepth : 0;
			SetSquelchState(true);
		}
		return;
	}

	// While open, learn the signal level so the end of a transmission can
	// be spotted within a few symbols, long before the quality average
	// decays. Without this, a second or so of noise after every
	// transmission is decoded as random characters.
	m_signalPower += kSignalPowerAlpha * (m_fastPower - m_signalPower);
	if (m_fastPower < kSignalLostRatio * m_signalPower)
	{
		// Make the quality be earned again from scratch, and drop the
		// partial character and those decoded since the signal went.
		m_zmAvg = {};
		m_afcAvg = {};
		m_idleAvg = {};
		m_quality = 0.0f;
		m_varicode.Reset();
		if (m_qpsk)
		{
			// The Viterbi decoder decides each bit QpskViterbi::kDepth
			// symbols after it arrived, longer ago than the signal has been
			// gone: the characters held were sent before it went. What is
			// still in the decoder's path memory is dropped.
			ReleaseHeld(true);
			ResetQpsk();
		}
		else
		{
			m_held.clear();
		}
		SetSquelchState(false);
	}
	else if (m_quality < m_squelch - kSquelchHysteresis)
	{
		ReleaseHeld(true);
		SetSquelchState(false);
	}
}

void Decoder::SetSquelchState(bool open)
{
	if (open != m_squelchOpen)
	{
		m_squelchOpen = open;
		if (m_onSquelch)
		{
			m_onSquelch(open);
		}
	}
}

bool Decoder::SignalNearCarrier(const std::vector<float>& frameDb, float marginDb) const
{
	const double binHz = m_spectrum.BinHz();
	const int last = static_cast<int>(frameDb.size()) - 1;
	auto toBin = [&](double hz) { return std::clamp(static_cast<int>(std::lround(hz / binHz)), 0, last); };
	std::vector<float> wide(frameDb.begin() + toBin(m_carrierHz - 8.0 * m_baud),
		frameDb.begin() + toBin(m_carrierHz + 8.0 * m_baud) + 1);
	if (wide.size() < 3)
	{
		return false;
	}
	const float floorDb = NoiseFloorDb(std::move(wide));
	// The peak within +/- one baud, over about 8 values in every mode: in
	// BPSK31 single bins, at the faster rates averages of neighbouring bins.
	// The highest of many single bins of noise would stand out by itself.
	const int lo = toBin(m_carrierHz - m_baud);
	const int hi = toBin(m_carrierHz + m_baud);
	const int width = std::max(1, (hi - lo + 1) / 8);
	float peak = -1e30f;
	for (int i = lo; i + width - 1 <= hi; ++i)
	{
		double sum = 0.0;
		for (int k = 0; k < width; ++k)
		{
			sum += std::pow(10.0, frameDb[static_cast<size_t>(i + k)] / 10.0);
		}
		peak = std::max(peak, static_cast<float>(10.0 * std::log10(sum / width + 1e-30)));
	}
	return peak >= floorDb + marginDb;
}

std::complex<float> Decoder::Interpolate(double t) const
{
	const double base = std::floor(t);
	const float frac = static_cast<float>(t - base);
	const auto i = static_cast<long long>(base);
	const auto& a = m_history[static_cast<size_t>(i) % kHistory];
	const auto& b = m_history[static_cast<size_t>(i + 1) % kHistory];
	return a * (1.0f - frac) + b * frac;
}

void Decoder::ProcessBaseband(std::complex<float> x)
{
	++m_sampleIndex;
	m_history[static_cast<size_t>(m_sampleIndex) % kHistory] = x;

	// Interpolating at t needs samples floor(t) and floor(t) + 1.
	while (m_nextStrobe < static_cast<double>(m_sampleIndex))
	{
		const double t = m_nextStrobe;
		const std::complex<float> y = Interpolate(t);
		const std::complex<float> mid = Interpolate(t - 0.5 * m_sps);

		// Gardner timing error detector, normalised by the symbol power.
		m_symbolPower += 0.05f * (std::norm(y) - m_symbolPower);
		float err = std::real((m_prevStrobe - y) * std::conj(mid)) / (m_symbolPower + 1e-20f);
		err = std::clamp(err, -1.0f, 1.0f);
		m_prevStrobe = y;

		if (m_qpsk)
		{
			// The symbol before this one, equalised (one symbol's delay).
			const std::complex<float> eq = m_strobes[1] - kNeighbourLeak * (m_strobes[0] + y);
			m_strobes[0] = m_strobes[1];
			m_strobes[1] = y;
			ProcessSymbol(eq);
		}
		else
		{
			ProcessSymbol(y);
		}

		m_nextStrobe += m_sps * (1.0 + kTimingGain * err);
	}
}

void Decoder::ProcessSymbol(std::complex<float> y)
{
	const std::complex<float> z = y * std::conj(m_prevSymbol);
	m_prevSymbol = y;

	// Squaring removes the BPSK data, leaving twice the phase error; QPSK
	// needs the fourth power, which leaves four times the phase error.
	const std::complex<float> z2 = z * z;
	const std::complex<float> zm = m_qpsk ? z2 * z2 : z2;
	const double order = m_qpsk ? 4.0 : 2.0;
	const float alpha = static_cast<float>(1.0 / kQualitySymbols);
	// Averaging unit vectors (rather than weighting by power) keeps noise
	// bursts from dominating, so noise reads consistently low (and alike in
	// both cases: the phase of noise raised to any power is still random).
	const float mag = std::abs(zm);
	if (mag > 0.0f)
	{
		m_zmAvg += alpha * (zm / mag - m_zmAvg);
	}
	m_quality = std::min(1.0f, std::abs(m_zmAvg));
	UpdateSquelch(std::norm(y));

	if (m_qpsk)
	{
		// The same average, restarted whenever the carrier jumps, and the
		// squared phase change's, for the idle reversals.
		if (mag > 0.0f)
		{
			m_afcAvg += alpha * (zm / mag - m_afcAvg);
			m_idleAvg += kIdleAlpha * (z2 / std::abs(z2) - m_idleAvg);
		}
		const bool idle = std::abs(m_idleAvg) >= kIdleQuality;
		const bool locked = m_quality >= kQpskAfcMinQuality && QpskScore() >= kQpskLockedScore;
		m_idleRun = idle ? m_idleRun + 1 : 0;
		m_lockedRun = locked ? m_lockedRun + 1 : 0;
		if (m_afc && !m_acquiring)
		{
			double phaseErr = 0.0;
			if (m_idleRun >= kQpskAfcRunSymbols)
			{
				phaseErr = 0.5 * std::arg(m_idleAvg);
			}
			else if (!idle && m_lockedRun >= kQpskAfcRunSymbols && std::norm(m_afcAvg) > 0.0f)
			{
				// Averaged, the phase error stays usable on a weak signal,
				// where the fourth power of one symbol's is nearly random.
				phaseErr = std::arg(m_afcAvg) / order;
			}
			SetCarrier(m_carrierHz + kAfcGain * phaseErr * m_baud / (2.0 * kPi));
		}
	}
	else if (m_afc && !m_acquiring && m_quality >= kAfcMinQuality && mag > 0.0f)
	{
		// Phase error per symbol (within +/- pi/2) -> frequency error in Hz.
		const double phaseErr = std::arg(zm) / order;
		const double freqErr = phaseErr * m_baud / (2.0 * kPi);
		SetCarrier(m_carrierHz + kAfcGain * freqErr);
	}

	// BPSK: no phase change is a one, a reversal a zero. QPSK: the bit the
	// Viterbi decoder has decided (none while its path memory fills).
	const int bit = m_qpsk ? QpskBit(z) : (std::real(z) > 0.0f ? 1 : 0);
	++m_symbolCount;
	if (bit >= 0)
	{
		PushBit(bit);
	}
	ReleaseHeld(false);
}

void Decoder::PushBit(int bit)
{
	const int c = m_varicode.PushBit(bit);
	if (m_qpskSkipBits > 0)
	{
		if (--m_qpskSkipBits == 0)
		{
			m_varicode.Resync();
		}
	}
	else if (c >= 0 && m_squelchOpen)
	{
		m_held.push_back({ m_symbolCount, static_cast<char>(c) });
	}
}

int Decoder::QpskBit(std::complex<float> z)
{
	// Soft decisions: the phase change scaled by its average size, so a
	// faded or noisy symbol counts less.
	m_zMagnitude += 0.05f * (std::abs(z) - m_zMagnitude);
	std::complex<float> r = z / (m_zMagnitude + 1e-20f);
	const float mag = std::abs(r);
	if (mag > kMaxSoftMagnitude)
	{
		r *= kMaxSoftMagnitude / mag;
	}
	// Correlation with the phase change of each code symbol. In fldigi's
	// normal sense, code symbol 0 is a reversal, 2 no change, 1 a quarter
	// turn clockwise and 3 anticlockwise (as seen in this receiver's
	// baseband, which mixes with exp(-j w t)); reversed, 1 and 3 swap.
	const float re = r.real();
	const float im = r.imag();
	const float normal[4] = { -re, -im, re, im };
	const float reversed[4] = { -re, im, re, -im };
	const int bits[2] = { m_viterbi[0].Push(normal), m_viterbi[1].Push(reversed) };

	// The idle zeros look the same either way; text only forms a valid code
	// sequence in the right sense, where the best path's metric grows by
	// close to the full score every symbol.
	for (int s = 0; s < 2; ++s)
	{
		m_senseScore[s] += kSenseAlpha * (m_viterbi[s].LastIncrement() - m_senseScore[s]);
	}
	const int other = 1 - m_sense;
	const bool leads = m_senseScore[other] >= kQpskLockedScore
		&& m_senseScore[other] > m_senseScore[m_sense] + kSenseMargin;
	m_senseLeadRun = leads ? m_senseLeadRun + 1 : 0;
	if (m_senseLeadRun >= kSenseRunSymbols)
	{
		m_sense = other;
		m_senseLeadRun = 0;
		// The character in progress was decoded in the wrong sense.
		m_varicode.Resync();
	}
	return bits[m_sense];
}

void Decoder::Flush()
{
	if (m_qpsk)
	{
		for (int bit : m_viterbi[m_sense].Flush())
		{
			PushBit(bit);
		}
		ResetQpsk();
	}
	ReleaseHeld(true);
}

void Decoder::ReleaseHeld(bool all)
{
	// Characters are held back for a few symbols so that, if the signal
	// turns out to have ended, the noise decoded in the meantime can still be
	// discarded (see UpdateSquelch).
	while (!m_held.empty() && (all || m_symbolCount - m_held.front().symbol >= kHoldSymbols))
	{
		const char c = m_held.front().c;
		m_held.pop_front();
		if (m_onChar)
		{
			m_onChar(c);
		}
	}
}

} // namespace psk
