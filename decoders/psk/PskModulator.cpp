#include "PskModulator.h"

#include <cmath>

namespace psk {

Modulator::Modulator(double sampleRate, Mode mode, double carrierHz, double startPhase, bool reversed) :
	m_sampleRate(sampleRate),
	m_baud(BaudRate(mode)),
	m_carrierHz(carrierHz),
	m_phase(startPhase),
	m_qpsk(IsQpsk(mode)),
	m_reversed(reversed)
{
}

void Modulator::Symbol(int quarterTurns, std::vector<float>& out)
{
	const double samplesPerSymbol = m_sampleRate / m_baud;
	// Carry the fractional part so symbol timing stays exact at any rate.
	m_symbolClock += samplesPerSymbol;
	const int n = static_cast<int>(m_symbolClock);
	m_symbolClock -= n;

	// Quarter turns are exact multiplications by j.
	std::complex<double> next = m_vector;
	for (int k = ((quarterTurns % 4) + 4) % 4; k > 0; --k)
	{
		next = { -next.imag(), next.real() };
	}

	const double dphi = 2.0 * kPi * m_carrierHz / m_sampleRate;
	for (int i = 0; i < n; ++i)
	{
		// Raised-cosine move from the old phase to the new one: a reversal
		// passes through zero amplitude, a quarter turn through 0.7.
		const double a = 0.5 + 0.5 * std::cos(kPi * (i + 0.5) / n);
		const std::complex<double> v = a * m_vector + (1.0 - a) * next;
		out.push_back(static_cast<float>(m_amplitude * (v.real() * std::cos(m_phase) - v.imag() * std::sin(m_phase))));
		m_phase += dphi;
		if (m_phase > 2.0 * kPi)
		{
			m_phase -= 2.0 * kPi;
		}
	}
	m_vector = next;
}

void Modulator::Bit(int bit, std::vector<float>& out)
{
	if (!m_qpsk)
	{
		Symbol(bit ? 0 : 2, out);
		return;
	}
	// fldigi's normal sense, in a baseband mixed with exp(-j w t): code
	// symbol 0 is a reversal, 2 no change, 1 a quarter turn clockwise and 3
	// anticlockwise. Reversed, 1 and 3 swap.
	static const int kNormal[4] = { 2, -1, 0, 1 };
	static const int kReversed[4] = { 2, 1, 0, -1 };
	const int code = m_encoder.Encode(bit);
	Symbol(m_reversed ? kReversed[code] : kNormal[code], out);
}

void Modulator::Idle(int symbols, std::vector<float>& out)
{
	for (int i = 0; i < symbols; ++i)
	{
		Bit(0, out);
	}
}

void Modulator::Carrier(int symbols, std::vector<float>& out)
{
	for (int i = 0; i < symbols; ++i)
	{
		Symbol(0, out);
	}
}

void Modulator::Text(const std::string& text, std::vector<float>& out)
{
	for (unsigned char c : text)
	{
		const char* code = Varicode::Encode(c);
		if (!code)
		{
			continue;
		}
		for (const char* p = code; *p; ++p)
		{
			Bit(*p == '1' ? 1 : 0, out);
		}
		Bit(0, out);
		Bit(0, out);
	}
}

std::vector<float> Modulator::Transmission(const std::string& text, int preamble, int postamble)
{
	std::vector<float> out;
	Idle(preamble, out);
	Text(text, out);
	if (m_qpsk)
	{
		Idle(postamble, out);
	}
	else
	{
		Carrier(postamble, out);
	}
	return out;
}

} // namespace psk
