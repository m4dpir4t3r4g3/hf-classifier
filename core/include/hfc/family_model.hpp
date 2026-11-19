// The learned signal-family model: a random forest over the feature meter's
// vector (EmissionFeatures::vector), trained offline (tools/ml/) on the
// Signal Identification Wiki's samples and embedded as text
// (catalogue/family_model.txt). It answers one question, which family a
// signal looks like:
//
//   FSK, MFSK, PSK (single carrier, QAM too), multi-carrier, OOK,
//   sweep/pulse (radars, sounders), analogue (AM, FM, SSB)
//
// with a confidence: the mean of its trees' leaf probabilities for that
// family. It is a prediction, not a measurement, and is shown as such; it
// names a family only at 0.5 or more (on wiki pages held out of training,
// such predictions were right 87 % of the time and were made for 44 % of
// the signals; tools/ml/README.md). Below that the signal is "not sure", which
// is also what something unlike the training data should get. It never
// names a system or a transmitter.
//
// The text format, one item per line:
//   hfc-family-forest 1
//   version <text>
//   families <name>|<name>|...
//   features <count>
//   tree <nodes>
//   <feature> <threshold> <left> <right>      a split: x[feature] <= threshold goes left
//   -1 <p0> <p1> ...                           a leaf: the families' probabilities
#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace hfc {

struct FamilyPrediction {
  std::string family;       // the most probable family ("" without a model or a vector)
  double confidence = 0;    // its probability, 0..1
  bool sure = false;        // confidence >= FamilyModel::kSure
  std::vector<double> probabilities;  // per FamilyModel::families()
};

class FamilyModel {
 public:
  static constexpr double kSure = 0.5;
  // The embedded model, parsed once.
  static const FamilyModel& builtin();

  bool load(const std::string& text, std::string* error);
  bool loaded() const { return !trees_.empty(); }
  const std::string& version() const { return version_; }
  const std::vector<std::string>& families() const { return families_; }
  std::size_t features() const { return features_; }
  FamilyPrediction predict(const std::vector<float>& vector) const;

 private:
  struct Node {
    int feature = -1;  // -1: a leaf
    float threshold = 0;
    int left = 0, right = 0;
    std::size_t value = 0;  // leaf: index into values_ of its first probability
  };
  std::string version_;
  std::vector<std::string> families_;
  std::size_t features_ = 0;
  std::vector<std::vector<Node>> trees_;
  std::vector<float> values_;
};

// catalogue/family_model.txt as embedded (cmake/embed.cmake).
const std::string& familyModelText();

}  // namespace hfc
