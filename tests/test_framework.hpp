// The test framework: TEST(name) registers a test, CHECK and CHECK_NEAR
// count failures without stopping the test. Self-contained, no dependency.
#pragma once

#include <cmath>
#include <cstdio>
#include <functional>
#include <vector>

namespace hfc::test {

struct TestCase {
  const char* name;
  std::function<void()> fn;
};
inline std::vector<TestCase>& registry() {
  static std::vector<TestCase> r;
  return r;
}
struct Registrar {
  Registrar(const char* n, std::function<void()> f) { registry().push_back({n, std::move(f)}); }
};
inline int g_failures = 0;

#define TEST(name)                                     \
  static void name();                                  \
  static hfc::test::Registrar reg_##name(#name, name); \
  static void name()

#define CHECK(cond)                                                   \
  do {                                                                \
    if (!(cond)) {                                                    \
      std::printf("    FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      ++hfc::test::g_failures;                                        \
    }                                                                 \
  } while (0)

#define CHECK_NEAR(a, b, tol)                                                                                  \
  do {                                                                                                         \
    const double va_ = (a), vb_ = (b);                                                                         \
    if (!(std::fabs(va_ - vb_) <= (tol))) {                                                                    \
      std::printf("    FAIL %s:%d: %s = %g, expected %g +- %g\n", __FILE__, __LINE__, #a, va_, vb_, double(tol)); \
      ++hfc::test::g_failures;                                                                                 \
    }                                                                                                          \
  } while (0)

}  // namespace hfc::test
