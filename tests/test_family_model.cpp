// The learned family model: its text format, inference, and the embedded
// model on synthetic signals.
#include <cmath>
#include <string>
#include <vector>

#include "hfc/family_model.hpp"
#include "hfc/features.hpp"
#include "test_framework.hpp"

using namespace hfc;

TEST(family_model_text_format_and_inference) {
  // Two trees over two features, two families.
  const std::string text =
      "hfc-family-forest 1\nversion test\nfamilies A|B\nfeatures 2\n"
      "tree 3\n0 0.5 1 2\n-1 1 0\n-1 0.25 0.75\n"
      "tree 5\n1 2 1 2\n-1 0.5 0.5\n0 0.5 3 4\n-1 1 0\n-1 0 1\n";
  FamilyModel m;
  std::string err;
  CHECK(m.load(text, &err) && m.loaded() && m.version() == "test" && m.families().size() == 2 && m.features() == 2);
  FamilyPrediction p = m.predict({0.2f, 1.0f});  // tree 1: A 1.0; tree 2: 0.5 / 0.5
  CHECK(p.family == "A" && std::fabs(p.confidence - 0.75) < 1e-6 && p.sure);
  p = m.predict({0.9f, 3.0f});  // tree 1: B 0.75; tree 2: B 1.0
  CHECK(p.family == "B" && std::fabs(p.confidence - 0.875) < 1e-6);
  p = m.predict({0.9f, 1.0f});  // tree 1: B 0.75; tree 2: 0.5: B 0.625
  CHECK(p.family == "B" && std::fabs(p.confidence - 0.625) < 1e-6 && std::fabs(p.probabilities[0] - 0.375) < 1e-6);
  // The wrong number of features: no opinion.
  CHECK(m.predict({1.0f}).family.empty());
  // Damaged files are refused, and leave no model.
  for (const char* bad : {"", "something else\n", "hfc-family-forest 1\nfamilies A|B\nfeatures 2\ntree 3\n0 0.5 1 2\n-1 1 0\n",
                          "hfc-family-forest 1\nfamilies A|B\nfeatures 2\ntree 2\n0 0.5 0 1\n-1 1 0\n",
                          "hfc-family-forest 1\nfamilies A|B\nfeatures 2\ntree 2\n5 0.5 1 1\n-1 1 0\n",
                          "hfc-family-forest 1\nfamilies A|B\nfeatures 2\ntree 3\n0 0.5 1 7\n-1 1 0\n-1 0 1\n"}) {
    FamilyModel b;
    CHECK(!b.load(bad, &err) && !b.loaded() && b.predict({0.f, 0.f}).family.empty());
  }
}

TEST(family_model_embedded) {
  const FamilyModel& m = FamilyModel::builtin();
  CHECK(m.loaded() && m.features() == kFeatureVector && m.families().size() == 7);
  CHECK(m.version().rfind("family-forest-", 0) == 0);
  // Probabilities sum to one, whatever the input.
  std::vector<float> x(kFeatureVector, 0.3f);
  const FamilyPrediction p = m.predict(x);
  double sum = 0;
  for (double v : p.probabilities) sum += v;
  CHECK(std::fabs(sum - 1.0) < 0.02 && !p.family.empty() && p.confidence > 0 && p.confidence <= 1.0);
}
