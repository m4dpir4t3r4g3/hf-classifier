#pragma once

#include <complex>
#include <cstddef>
#include <vector>

namespace psk {

// In-place radix-2 FFT. `data.size()` must be a power of two.
void Fft(std::vector<std::complex<float>>& data);

// Produces power spectra (dB) of a real signal using a Hann window and 50%
// overlap. Each completed frame holds Size()/2 bins covering 0..fs/2.
class SpectrumAnalyzer
{
public:
	void Init(size_t fftSize, double sampleRate);

	// Returns true when `frame` has been filled with a new spectrum.
	bool Push(float x, std::vector<float>& frame);

	size_t Size() const { return m_size; }
	double SampleRate() const { return m_sampleRate; }
	double BinHz() const { return m_sampleRate / static_cast<double>(m_size); }

private:
	size_t m_size = 0;
	double m_sampleRate = 0.0;
	std::vector<float> m_window;
	std::vector<float> m_input;
	size_t m_fill = 0;
	std::vector<std::complex<float>> m_work;
};

// Bins more than this far (dB) below the strongest tenth of a spectrum are
// taken as empty, not noise, and of the rest those within kFlatDb of their
// median as the flat part where the noise floor is (NoiseFloorDb).
constexpr float kEmptyBelowDb = 50.0f;
constexpr float kFlatDb = 10.0f;

// Noise floor (dB) of a stretch of spectrum: the 20th percentile of its flat
// part, so that neither the empty part of a band-limited input (a narrow
// receiver filter, or a channel cut out of IQ) nor the sloping edges of its
// band count as noise. For audio with noise across the band this is the
// plain 20th percentile, or close to it.
float NoiseFloorDb(std::vector<float> binsDb);

// Estimates the carrier frequency of a PSK signal near `approxHz` from a
// power spectrum in dB (as produced by SpectrumAnalyzer). Candidates within
// +/- `searchHz` are scored by how much power above the noise floor is
// mirror-symmetric around them, over a width set by the signal's `baud`.
// This finds the carrier even when an idling signal shows two tones at
// +/- baud/2. Returns `approxHz` unchanged when nothing within the search
// range stands `minPeakDb` above the floor.
double FindSignalCentre(const std::vector<float>& spectrumDb, double binHz, double approxHz, double searchHz,
	double baud, float minPeakDb = 6.0f);

} // namespace psk
