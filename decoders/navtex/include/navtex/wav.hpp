// Minimal RIFF/WAVE reader and writer: PCM 8/16/24/32-bit and IEEE float.
#pragma once

#include <string>
#include <vector>

namespace navtex {

struct WavData {
  double sampleRate = 0;
  int channels = 0;
  std::vector<float> samples;  // interleaved, -1..1
};

// Returns false and sets `error` on failure.
bool readWav(const std::string& path, WavData& out, std::string& error);
// Writes 16-bit PCM.
bool writeWav(const std::string& path, const WavData& data, std::string& error);

}  // namespace navtex
