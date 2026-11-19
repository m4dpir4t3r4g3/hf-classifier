#pragma once

#include "PskDecoder.h"
#include "QpskCode.h"

#include <complex>
#include <string>
#include <vector>

namespace psk {

// Generates standard BPSK and QPSK audio, 31 to 500 baud, as fldigi sends it:
// the carrier's complex amplitude moves from one symbol's phase to the next
// with a raised-cosine shape. Used for tests and for producing reference
// recordings.
class Modulator
{
public:
	// `reversed`: QPSK with the opposite sense of rotation, as fldigi sends
	// with "Reverse" on (or as the signal arrives on the other sideband).
	Modulator(double sampleRate, Mode mode, double carrierHz, double startPhase = 0.0, bool reversed = false);

	// Appends `symbols` idle zero bits: phase reversals (BPSK, and QPSK
	// once the convolutional code holds only zeros). The standard preamble.
	void Idle(int symbols, std::vector<float>& out);
	// Appends unmodulated carrier (the BPSK postamble).
	void Carrier(int symbols, std::vector<float>& out);
	// Appends Varicode text, each character followed by the "00" gap.
	void Text(const std::string& text, std::vector<float>& out);
	// Appends one data bit. BPSK: 0 = phase reversal, 1 = no change. QPSK:
	// the bit's code symbol (QpskCode.h).
	void Bit(int bit, std::vector<float>& out);
	// Appends one symbol that turns the phase by `quarterTurns` x 90 degrees
	// (anticlockwise in the complex baseband).
	void Symbol(int quarterTurns, std::vector<float>& out);

	// Convenience: preamble + text + postamble (BPSK: carrier; QPSK: idle
	// zeros, which flush the convolutional code, as fldigi ends QPSK).
	std::vector<float> Transmission(const std::string& text, int preamble = 64, int postamble = 32);

private:
	double m_sampleRate;
	double m_baud;
	double m_carrierHz;
	double m_phase;
	bool m_qpsk;
	bool m_reversed;
	double m_symbolClock = 0.0;
	std::complex<double> m_vector{ 1.0, 0.0 };
	QpskEncoder m_encoder;
	float m_amplitude = 0.5f;
};

} // namespace psk
