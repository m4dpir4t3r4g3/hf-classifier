#pragma once

#include <cmath>
#include <complex>
#include <cstddef>
#include <vector>

namespace psk {

constexpr double kPi = 3.14159265358979323846;

// Windowed-sinc (Blackman) low-pass FIR with unity DC gain.
inline std::vector<float> DesignLowpass(int taps, double cutoffHz, double sampleRate)
{
	std::vector<float> h(static_cast<size_t>(taps));
	const double fc = cutoffHz / sampleRate;
	const double mid = 0.5 * (taps - 1);
	double sum = 0.0;
	for (int i = 0; i < taps; ++i)
	{
		const double x = i - mid;
		const double sinc = (x == 0.0) ? 2.0 * fc : std::sin(2.0 * kPi * fc * x) / (kPi * x);
		const double w = taps > 1
			? 0.42 - 0.5 * std::cos(2.0 * kPi * i / (taps - 1)) + 0.08 * std::cos(4.0 * kPi * i / (taps - 1))
			: 1.0;
		h[static_cast<size_t>(i)] = static_cast<float>(sinc * w);
		sum += sinc * w;
	}
	for (auto& v : h)
	{
		v = static_cast<float>(v / sum);
	}
	return h;
}

// FIR filter that keeps one output out of every `factor` inputs and only
// computes the outputs it keeps.
template <typename T>
class FirDecimator
{
public:
	void Init(std::vector<float> taps, int factor)
	{
		m_taps.assign(taps.rbegin(), taps.rend());
		m_n = m_taps.size();
		m_history.assign(2 * m_n, T{});
		m_pos = 0;
		m_factor = factor < 1 ? 1 : factor;
		m_phase = 0;
	}

	// Returns true and writes `out` when this input produces an output.
	bool Push(T x, T& out)
	{
		m_history[m_pos] = x;
		m_history[m_pos + m_n] = x;
		m_pos = (m_pos + 1) % m_n;
		if (++m_phase < m_factor)
		{
			return false;
		}
		m_phase = 0;
		// m_history[m_pos .. m_pos + n) holds the samples oldest to newest.
		const T* p = &m_history[m_pos];
		T acc{};
		for (size_t i = 0; i < m_n; ++i)
		{
			acc += p[i] * m_taps[i];
		}
		out = acc;
		return true;
	}

	int Factor() const { return m_factor; }

private:
	std::vector<float> m_taps;
	std::vector<T> m_history;
	size_t m_n = 0;
	size_t m_pos = 0;
	int m_factor = 1;
	int m_phase = 0;
};

} // namespace psk
