#pragma once
// Minimal test harness (no external dependencies, so the repo builds offline).

#include <cstdio>
#include <functional>
#include <sstream>
#include <string>
#include <vector>

namespace apex_test {

struct TestCase {
  const char* name;
  void (*fn)();
};

inline std::vector<TestCase>& registry() {
  static std::vector<TestCase> r;
  return r;
}

inline int& failures() {
  static int f = 0;
  return f;
}

inline bool& current_failed() {
  static bool f = false;
  return f;
}

struct Registrar {
  Registrar(const char* name, void (*fn)()) { registry().push_back({name, fn}); }
};

inline void report(const char* file, int line, const std::string& msg) {
  std::fprintf(stderr, "    %s:%d: %s\n", file, line, msg.c_str());
  current_failed() = true;
}

}  // namespace apex_test

#define TEST(name)                                                  \
  static void name();                                               \
  static ::apex_test::Registrar registrar_##name(#name, &name);     \
  static void name()

#define CHECK(cond)                                                       \
  do {                                                                    \
    if (!(cond)) {                                                        \
      ::apex_test::report(__FILE__, __LINE__, "CHECK(" #cond ") failed"); \
      return;                                                             \
    }                                                                     \
  } while (0)

#define CHECK_EQ(a, b)                                                     \
  do {                                                                     \
    const auto& va_ = (a);                                                 \
    const auto& vb_ = (b);                                                 \
    if (!(va_ == vb_)) {                                                   \
      std::ostringstream os_;                                              \
      os_ << "CHECK_EQ(" #a ", " #b ") failed: " << va_ << " vs " << vb_;  \
      ::apex_test::report(__FILE__, __LINE__, os_.str());                  \
      return;                                                              \
    }                                                                      \
  } while (0)
