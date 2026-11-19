#pragma once

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <vector>

namespace psk {

// The convolutional code of the QPSK31/63/125/250/500 modes (G3PLX's QPSK31,
// as fldigi implements it): rate 1/2, constraint length 5, generator
// polynomials 0x17 and 0x19. Each data bit becomes one 2-bit code symbol,
// sent as one of four phase changes.

// Code symbol for a 5-bit shift register holding the newest data bit in bit
// 0: bit 0 of the result is the parity of the register masked by 0x17, bit 1
// that masked by 0x19.
inline int QpskCodeSymbol(unsigned reg)
{
	auto parity = [](unsigned v) {
		v ^= v >> 4;
		v ^= v >> 2;
		v ^= v >> 1;
		return static_cast<int>(v & 1u);
	};
	return parity(reg & 0x17u) | (parity(reg & 0x19u) << 1);
}

class QpskEncoder
{
public:
	// Shifts in one data bit and returns its code symbol (0..3).
	int Encode(int bit)
	{
		m_reg = ((m_reg << 1) | (bit ? 1u : 0u)) & 31u;
		return QpskCodeSymbol(m_reg);
	}
	void Reset() { m_reg = 0; }

private:
	unsigned m_reg = 0;
};

// Soft-decision Viterbi decoder for the code above.
class QpskViterbi
{
public:
	static constexpr int kStates = 16;
	// Each bit is decided this many symbols after it was received: about six
	// constraint lengths, past which a longer path memory no longer helps.
	static constexpr int kDepth = 32;

	QpskViterbi() { Reset(); }

	void Reset()
	{
		std::fill(std::begin(m_metric), std::end(m_metric), 0.0f);
		m_pos = 0;
		m_filled = 0;
		m_increment = 0.0f;
		m_bestState = 0;
	}

	// `score[s]`: how well the received symbol matches code symbol s (higher
	// is better, e.g. the correlation with the phase change it stands for).
	// Returns the data bit decided kDepth symbols ago, or -1 while the path
	// memory fills.
	int Push(const float score[4])
	{
		// A state is the last four data bits, the newest in bit 0. State ns
		// is reached from (ns >> 1) and (ns >> 1) | 8 by the data bit ns & 1.
		float next[kStates];
		std::uint16_t decisions = 0;
		float best = -1e30f;
		int bestState = 0;
		for (int ns = 0; ns < kStates; ++ns)
		{
			const int p0 = ns >> 1;
			const int p1 = p0 | 8;
			const float m0 = m_metric[p0] + score[QpskCodeSymbol(static_cast<unsigned>(ns))];
			const float m1 = m_metric[p1] + score[QpskCodeSymbol(static_cast<unsigned>(ns | 16))];
			if (m1 > m0)
			{
				next[ns] = m1;
				decisions = static_cast<std::uint16_t>(decisions | (1u << ns));
			}
			else
			{
				next[ns] = m0;
			}
			if (next[ns] > best)
			{
				best = next[ns];
				bestState = ns;
			}
		}
		// The previous best metric was normalised to 0.
		m_increment = best;
		m_bestState = bestState;
		for (int s = 0; s < kStates; ++s)
		{
			m_metric[s] = next[s] - best;
		}
		m_decisions[m_pos] = decisions;
		m_pos = (m_pos + 1) % kDepth;
		if (m_filled < kDepth)
		{
			++m_filled;
			if (m_filled < kDepth)
			{
				return -1;
			}
		}
		// Follow the best path back to the oldest step in the memory.
		int s = bestState;
		int bit = 0;
		for (int k = 1; k <= kDepth; ++k)
		{
			const std::uint16_t d = m_decisions[(m_pos - k + kDepth) % kDepth];
			bit = s & 1;
			s = (s >> 1) | (((d >> s) & 1) << 3);
		}
		return bit;
	}

	// The bits not yet returned by Push (up to kDepth - 1, oldest first),
	// decided on what has been received; then starts afresh. For the end of
	// a recording.
	std::vector<int> Flush()
	{
		const int count = m_filled < kDepth ? m_filled : kDepth - 1;
		std::vector<int> bits(static_cast<size_t>(count));
		int s = m_bestState;
		for (int k = 1; k <= count; ++k)
		{
			const std::uint16_t d = m_decisions[(m_pos - k + kDepth) % kDepth];
			bits[static_cast<size_t>(count - k)] = s & 1;
			s = (s >> 1) | (((d >> s) & 1) << 3);
		}
		Reset();
		return bits;
	}

	// How much the best path's metric grew with the last symbol: close to the
	// best score a symbol can get while the received symbols form a valid
	// code sequence, lower when they do not.
	float LastIncrement() const { return m_increment; }

private:
	float m_metric[kStates];
	std::uint16_t m_decisions[kDepth] = {};
	int m_pos = 0;
	int m_filled = 0;
	float m_increment = 0.0f;
	int m_bestState = 0;
};

} // namespace psk
