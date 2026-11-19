#include "hfc/family_model.hpp"

#include <cstdio>
#include <cstdlib>
#include <sstream>

namespace hfc {

const FamilyModel& FamilyModel::builtin() {
  static const FamilyModel m = [] {
    FamilyModel k;
    std::string err;
    if (!k.load(familyModelText(), &err)) std::fprintf(stderr, "family model: %s\n", err.c_str());
    return k;
  }();
  return m;
}

bool FamilyModel::load(const std::string& text, std::string* error) {
  auto fail = [&](const std::string& why) {
    trees_.clear();
    values_.clear();
    families_.clear();
    features_ = 0;
    if (error) *error = why;
    return false;
  };
  trees_.clear();
  values_.clear();
  families_.clear();
  version_.clear();
  features_ = 0;
  std::istringstream in(text);
  std::string line;
  if (!std::getline(in, line) || line.rfind("hfc-family-forest 1", 0) != 0) return fail("not a family model");
  std::size_t pending = 0;  // nodes still to read of the current tree
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty()) continue;
    if (pending == 0) {
      if (line.rfind("version ", 0) == 0) {
        version_ = line.substr(8);
      } else if (line.rfind("families ", 0) == 0) {
        std::string name;
        for (char c : line.substr(9) + "|") {
          if (c == '|') {
            if (!name.empty()) families_.push_back(name);
            name.clear();
          } else {
            name += c;
          }
        }
      } else if (line.rfind("features ", 0) == 0) {
        features_ = static_cast<std::size_t>(std::atol(line.c_str() + 9));
      } else if (line.rfind("tree ", 0) == 0) {
        pending = static_cast<std::size_t>(std::atol(line.c_str() + 5));
        if (pending == 0 || pending > 1000000 || families_.empty() || features_ == 0) return fail("bad tree header");
        trees_.emplace_back();
        trees_.back().reserve(pending);
      } else {
        return fail("unexpected line: " + line.substr(0, 40));
      }
      continue;
    }
    Node n;
    char* p = nullptr;
    n.feature = static_cast<int>(std::strtol(line.c_str(), &p, 10));
    if (n.feature < 0) {
      n.value = values_.size();
      for (std::size_t k = 0; k < families_.size(); ++k) values_.push_back(std::strtof(p, &p));
    } else {
      if (static_cast<std::size_t>(n.feature) >= features_) return fail("feature out of range");
      n.threshold = std::strtof(p, &p);
      n.left = static_cast<int>(std::strtol(p, &p, 10));
      n.right = static_cast<int>(std::strtol(p, &p, 10));
    }
    trees_.back().push_back(n);
    --pending;
  }
  if (pending != 0 || trees_.empty()) return fail("truncated");
  // Every child must exist and lie after its parent (no loops).
  for (const auto& t : trees_)
    for (std::size_t i = 0; i < t.size(); ++i)
      if (t[i].feature >= 0 && (t[i].left <= static_cast<int>(i) || t[i].right <= static_cast<int>(i) ||
                                static_cast<std::size_t>(t[i].left) >= t.size() || static_cast<std::size_t>(t[i].right) >= t.size()))
        return fail("bad child index");
  return true;
}

FamilyPrediction FamilyModel::predict(const std::vector<float>& x) const {
  FamilyPrediction out;
  if (trees_.empty() || x.size() != features_) return out;
  out.probabilities.assign(families_.size(), 0.0);
  for (const auto& t : trees_) {
    std::size_t i = 0;
    while (t[i].feature >= 0) i = static_cast<std::size_t>(x[static_cast<std::size_t>(t[i].feature)] <= t[i].threshold ? t[i].left : t[i].right);
    for (std::size_t k = 0; k < families_.size(); ++k) out.probabilities[k] += values_[t[i].value + k];
  }
  std::size_t best = 0;
  for (std::size_t k = 0; k < families_.size(); ++k) {
    out.probabilities[k] /= static_cast<double>(trees_.size());
    if (out.probabilities[k] > out.probabilities[best]) best = k;
  }
  out.family = families_[best];
  out.confidence = out.probabilities[best];
  out.sure = out.confidence >= kSure;
  return out;
}

}  // namespace hfc
