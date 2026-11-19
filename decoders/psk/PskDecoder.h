#pragma once

#include "Filters.h"
#include "QpskCode.h"
#include "Spectrum.h"
#include "Varicode.h"

#include <complex>
#include <deque>
#include <cstddef>
#include <functional>
#include <vector>

namespace psk {

// The values are stored in the settings: new modes are appended.
enum class Mode
{
	Bpsk31 = 0,
	Bpsk63 = 1,
	Bpsk125 = 2,
	Bpsk250 = 3,
	Bpsk500 = 4,
	Qpsk31 = 5,
	Qpsk63 = 6,
	Qpsk125 = 7,
	Qpsk250 = 8,
	Qpsk500 = 9,
};

constexpr int kModeCount = 10;

double BaudRate(Mode mode);
const char* ModeName(Mode mode);
// QPSK modes send each Varicode bit as one of four phase changes, through a
// rate 1/2 convolutional code (QpskCode.h).
bool IsQpsk(Mode mode);

// BPSK and QPSK (31 to 500 baud) receiver for a real audio signal (e.g. an
// SSB receiver's audio output).
//
// Pipeline: low-pass + decimate to ~8 kHz -> mix the selected carrier to
// baseband -> low-pass + decimate to ~16 samples/symbol -> matched filter ->
// Gardner symbol timing recovery -> differential phase detection -> (QPSK:
// Viterbi decoder) -> Varicode. A two-stage AFC (spectrum centroid for
// pull-in, symbol phase error for tracking) follows the carrier, and a
// quality metric drives the squelch.
//
// QPSK is decoded in both senses of rotation at once (the sense is reversed
// on the other sideband, and by fldigi's "Reverse" option), and the sense
// whose symbols form a valid code sequence is used.
class Decoder
{
public:
	using CharCallback = std::function<void(char)>;
	using SpectrumCallback = std::function<void(const std::vector<float>& db, double binHz)>;
	using SquelchCallback = std::function<void(bool open)>;

	Decoder();

	// (Re)initialises all filters. Must be called before Process().
	void Configure(double sampleRate, Mode mode);

	// Moves the receive frequency (keeps averaging state; used by the AFC).
	void SetCarrier(double hz);
	// Tunes to a new signal: sets the frequency and resets the AFC, quality
	// and Varicode state.
	void Tune(double hz);
	double Carrier() const { return m_carrierHz; }

	void SetAfc(bool enabled) { m_afc = enabled; }
	bool Afc() const { return m_afc; }

	// Squelch threshold on the quality metric, 0..1 (0 disables the squelch).
	// The squelch opens at the threshold, closes a little below it, and
	// closes at once when the signal level drops away.
	void SetSquelch(float threshold) { m_squelch = threshold; }
	float Squelch() const { return m_squelch; }

	// Signal quality 0..1: consistency of the symbol-to-symbol phase changes.
	// Noise stays below ~0.3, a clean signal reads close to 1.
	float Quality() const { return m_quality; }
	bool SquelchOpen() const { return m_squelchOpen; }
	// QPSK: whether the signal rotates the opposite way to fldigi's normal
	// sense (received on the other sideband, or sent with "Reverse").
	bool QpskReversed() const { return m_sense == 1; }

	Mode GetMode() const { return m_mode; }
	double SampleRate() const { return m_sampleRate; }
	// Highest carrier frequency the decoder can be tuned to.
	double MaxCarrier() const;

	void SetCharCallback(CharCallback cb) { m_onChar = std::move(cb); }
	// Called when the squelch opens (a transmission starts) or closes.
	void SetSquelchCallback(SquelchCallback cb) { m_onSquelch = std::move(cb); }
	// Receives spectra of the (decimated) input audio for a waterfall.
	void SetSpectrumCallback(SpectrumCallback cb) { m_onSpectrum = std::move(cb); }

	// Processes `frames` frames of interleaved audio with `channels` channels.
	// The channels are averaged.
	void Process(const float* interleaved, size_t frames, int channels = 1);
	// At the end of a recording: decodes what the decoder still holds (QPSK:
	// the last QpskViterbi::kDepth symbols) and passes on the characters held
	// back for the squelch.
	void Flush();

private:
	void ProcessDecimated(float x);
	void ProcessBaseband(std::complex<float> x);
	void ProcessSymbol(std::complex<float> y);
	void PushBit(int bit);
	int QpskBit(std::complex<float> z);
	float QpskScore() const;
	double QpskAcquireTarget(double centre) const;
	void ResetQpsk();
	void CoarseAfc(const std::vector<float>& frameDb);
	void UpdateSquelch(float symbolPower);
	void SetSquelchState(bool open);
	void ReleaseHeld(bool all);
	// Whether a signal stands `marginDb` above the noise floor within +/- one
	// baud of the carrier.
	bool SignalNearCarrier(const std::vector<float>& spectrumDb, float marginDb) const;
	std::complex<float> Interpolate(double t) const;

	double m_sampleRate = 0.0;
	Mode m_mode = Mode::Bpsk31;
	double m_baud = 31.25;
	bool m_qpsk = false;

	// Stage A: real decimation to ~8 kHz.
	FirDecimator<float> m_decimA;
	int m_factorA = 1;
	double m_rateA = 0.0;
	SpectrumAnalyzer m_spectrum;
	std::vector<float> m_spectrumFrame;
	std::vector<float> m_avgPower;
	std::vector<float> m_avgDb;
	int m_avgFrames = 0;
	int m_coarseHoldoff = 0;
	int m_lowQualityFrames = 0;
	bool m_acquiring = true;

	// Mixer.
	double m_carrierHz = 1000.0;
	double m_ncoPhase = 0.0;

	// Stage B: complex decimation to ~16 samples/symbol, then matched filter.
	FirDecimator<std::complex<float>> m_decimB;
	FirDecimator<std::complex<float>> m_matched;
	double m_rateB = 0.0;
	double m_sps = 16.0;

	// Timing recovery.
	std::vector<std::complex<float>> m_history;
	long long m_sampleIndex = -1;
	double m_nextStrobe = 0.0;
	std::complex<float> m_prevStrobe;
	std::complex<float> m_strobes[2];  // QPSK equaliser: the last two strobes
	std::complex<float> m_prevSymbol;  // for the differential detection
	float m_symbolPower = 1e-12f;

	// Detection, AFC and squelch. The quality averages the phase change
	// raised to the power of the number of phases (BPSK 2, QPSK 4), which
	// removes the data.
	std::complex<float> m_zmAvg;
	std::complex<float> m_afcAvg;   // QPSK AFC: the same, restarted on carrier jumps
	std::complex<float> m_idleAvg;  // QPSK: squared phase change, short average (idle reversals)
	float m_quality = 0.0f;
	bool m_afc = true;
	float m_squelch = 0.35f;
	bool m_squelchOpen = false;
	float m_fastPower = 0.0f;
	float m_signalPower = 0.0f;
	int m_symbolsAbove = 0;
	struct HeldChar
	{
		long long symbol;
		char c;
	};
	std::deque<HeldChar> m_held;
	long long m_symbolCount = 0;
	VaricodeDecoder m_varicode;

	// QPSK: one Viterbi decoder per sense of rotation (0 normal, 1 reversed),
	// how well each has matched the signal recently, and the one in use.
	QpskViterbi m_viterbi[2];
	float m_senseScore[2] = { 0.0f, 0.0f };
	float m_zMagnitude = 0.0f;
	int m_qpskSkipBits = 0;  // decoded bits not to print after the squelch opens
	int m_idleRun = 0;    // symbols in a row of idle reversals
	int m_lockedRun = 0;  // symbols in a row meeting the AFC's conditions
	int m_sense = 0;
	int m_senseLeadRun = 0;  // symbols in a row the other sense has led

	CharCallback m_onChar;
	SpectrumCallback m_onSpectrum;
	SquelchCallback m_onSquelch;
};

} // namespace psk
