#include "Spectrum.h"
#include "Filters.h"

#include <algorithm>
#include <cmath>

namespace psk {

void Fft(std::vector<std::complex<float>>& data)
{
	const size_t n = data.size();
	for (size_t i = 1, j = 0; i < n; ++i)
	{
		size_t bit = n >> 1;
		for (; j & bit; bit >>= 1)
		{
			j ^= bit;
		}
		j ^= bit;
		if (i < j)
		{
			std::swap(data[i], data[j]);
		}
	}
	for (size_t len = 2; len <= n; len <<= 1)
	{
		const double angle = -2.0 * kPi / static_cast<double>(len);
		const std::complex<float> wlen(static_cast<float>(std::cos(angle)), static_cast<float>(std::sin(angle)));
		for (size_t i = 0; i < n; i += len)
		{
			std::complex<float> w(1.0f, 0.0f);
			for (size_t k = 0; k < len / 2; ++k)
			{
				const std::complex<float> u = data[i + k];
				const std::complex<float> v = data[i + k + len / 2] * w;
				data[i + k] = u + v;
				data[i + k + len / 2] = u - v;
				w *= wlen;
			}
		}
	}
}

void SpectrumAnalyzer::Init(size_t fftSize, double sampleRate)
{
	m_size = fftSize;
	m_sampleRate = sampleRate;
	m_window.resize(fftSize);
	for (size_t i = 0; i < fftSize; ++i)
	{
		m_window[i] = static_cast<float>(0.5 - 0.5 * std::cos(2.0 * kPi * i / (fftSize - 1)));
	}
	m_input.assign(fftSize, 0.0f);
	m_fill = 0;
	m_work.resize(fftSize);
}

bool SpectrumAnalyzer::Push(float x, std::vector<float>& frame)
{
	if (m_size == 0)
	{
		return false;
	}
	m_input[m_fill++] = x;
	if (m_fill < m_size)
	{
		return false;
	}

	for (size_t i = 0; i < m_size; ++i)
	{
		m_work[i] = std::complex<float>(m_input[i] * m_window[i], 0.0f);
	}
	Fft(m_work);

	const size_t bins = m_size / 2;
	frame.resize(bins);
	// Normalise so a full-scale sine reads roughly 0 dB.
	const float scale = 4.0f / (static_cast<float>(m_size) * static_cast<float>(m_size));
	for (size_t i = 0; i < bins; ++i)
	{
		frame[i] = 10.0f * std::log10(std::norm(m_work[i]) * scale + 1e-20f);
	}

	// 50% overlap: keep the second half as the start of the next frame.
	std::copy(m_input.begin() + static_cast<std::ptrdiff_t>(bins), m_input.end(), m_input.begin());
	m_fill = m_size - bins;
	return true;
}

float NoiseFloorDb(std::vector<float> binsDb)
{
	if (binsDb.empty())
	{
		return -200.0f;
	}
	// Bins far below the strongest tenth hold no noise at all: the empty rest
	// of a band-limited input (a narrow receiver filter, or a channel cut out
	// of IQ, as the SDRconnect classifier feeds this decoder). Left in, they
	// put the floor so low that noise passes for a signal.
	const size_t top = binsDb.size() * 9 / 10;
	std::nth_element(binsDb.begin(), binsDb.begin() + static_cast<std::ptrdiff_t>(top), binsDb.end());
	const float limit = binsDb[top] - kEmptyBelowDb;
	std::vector<float> live;
	live.reserve(binsDb.size());
	for (float v : binsDb)
	{
		if (v >= limit)
		{
			live.push_back(v);
		}
	}
	std::vector<float>& use = live.size() >= 3 ? live : binsDb;
	// Nor do the sloping edges of such a band: the floor is taken from the
	// flat part, the bins within kFlatDb of the median (noise, mostly, as
	// long as signals cover less than half of the stretch).
	const size_t mid = use.size() / 2;
	std::nth_element(use.begin(), use.begin() + static_cast<std::ptrdiff_t>(mid), use.end());
	const float median = use[mid];
	std::vector<float> flat;
	flat.reserve(use.size());
	for (float v : use)
	{
		if (std::fabs(v - median) <= kFlatDb)
		{
			flat.push_back(v);
		}
	}
	std::vector<float>& from = flat.size() >= 3 ? flat : use;
	const size_t pct = from.size() / 5;
	std::nth_element(from.begin(), from.begin() + static_cast<std::ptrdiff_t>(pct), from.end());
	return from[pct];
}

double FindSignalCentre(const std::vector<float>& spectrumDb, double binHz, double approxHz, double searchHz,
	double baud, float minPeakDb)
{
	if (spectrumDb.empty() || binHz <= 0.0)
	{
		return approxHz;
	}
	const int size = static_cast<int>(spectrumDb.size());
	auto toBin = [&](double hz) {
		return std::clamp(static_cast<int>(std::lround(hz / binHz)), 0, size - 1);
	};

	// Noise floor: low percentile of a wider neighbourhood.
	std::vector<float> neighbourhood(spectrumDb.begin() + toBin(approxHz - 4.0 * (searchHz + baud)),
		spectrumDb.begin() + toBin(approxHz + 4.0 * (searchHz + baud)) + 1);
	if (neighbourhood.size() < 3)
	{
		return approxHz;
	}
	const float floorDb = NoiseFloorDb(std::move(neighbourhood));

	const int lo = toBin(approxHz - searchHz);
	const int hi = toBin(approxHz + searchHz);
	const float peakDb = *std::max_element(spectrumDb.begin() + lo, spectrumDb.begin() + hi + 1);
	if (peakDb < floorDb + minPeakDb)
	{
		return approxHz;
	}

	// Power above the noise floor, lightly smoothed so a tone that falls
	// between two bins still looks symmetric about its true frequency.
	const double floorLin = std::pow(10.0, floorDb / 10.0);
	auto rawExcess = [&](int i) {
		if (i < 0 || i >= size)
		{
			return 0.0;
		}
		return std::max(0.0, std::pow(10.0, spectrumDb[static_cast<size_t>(i)] / 10.0) - floorLin);
	};
	const int halfWidth = std::max(1, static_cast<int>(std::lround(0.75 * baud / binHz)));
	const int first = lo - halfWidth - 1;
	std::vector<double> smoothed(static_cast<size_t>(hi - lo + 2 * halfWidth + 3));
	for (size_t j = 0; j < smoothed.size(); ++j)
	{
		const int i = first + static_cast<int>(j);
		smoothed[j] = (rawExcess(i - 2) + 2.0 * rawExcess(i - 1) + 3.0 * rawExcess(i) + 2.0 * rawExcess(i + 1)
			+ rawExcess(i + 2)) / 9.0;
	}
	auto excess = [&](int i) {
		const int j = i - first;
		return (j >= 0 && j < static_cast<int>(smoothed.size())) ? smoothed[static_cast<size_t>(j)] : 0.0;
	};

	// Score each candidate centre (in half-bin steps) by how much power is
	// mirror-symmetric around it, minus how much is not. A PSK spectrum is
	// symmetric about its carrier, so this lands on the carrier rather than
	// on one of the idle tones, and a candidate that would pair our signal
	// with a neighbour's is penalised by the imbalance. +/- 0.75 baud covers
	// the idle tones (+/- baud/2) and most of the data spectrum without
	// reaching a neighbour a couple of baud away.
	auto pairScore = [&](int a, int b) {
		const double pa = excess(a);
		const double pb = excess(b);
		return 2.0 * std::min(pa, pb) - std::max(pa, pb);
	};
	std::vector<double> scores;
	for (int twice = 2 * lo; twice <= 2 * hi; ++twice)
	{
		const int left = twice / 2;          // bin at or below the centre
		const int right = (twice + 1) / 2;   // bin at or above the centre
		// A bin exactly on the centre would pair with itself, so it is left
		// out: otherwise one idle tone could outscore the symmetric pair.
		double score = left == right ? 0.0 : pairScore(left, right);
		for (int k = 1; k <= halfWidth; ++k)
		{
			score += pairScore(left - k, right + k);
		}
		scores.push_back(score);
	}
	const auto best = std::max_element(scores.begin(), scores.end());
	if (*best <= 0.0)
	{
		return approxHz;
	}
	const size_t idx = static_cast<size_t>(best - scores.begin());
	double offset = 0.0;
	if (idx > 0 && idx + 1 < scores.size())
	{
		// Parabolic interpolation between neighbouring candidates.
		const double a = scores[idx - 1];
		const double b = scores[idx];
		const double c = scores[idx + 1];
		const double denom = a - 2.0 * b + c;
		if (denom < 0.0)
		{
			offset = std::clamp(0.5 * (a - c) / denom, -0.5, 0.5);
		}
	}
	return (lo + 0.5 * (static_cast<double>(idx) + offset)) * binHz;
}

} // namespace psk
