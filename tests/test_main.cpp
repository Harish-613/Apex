#include <cstdio>
#include <cstring>

#include "test_framework.hpp"

int main(int argc, char** argv) {
  const char* filter = argc > 1 ? argv[1] : nullptr;
  int run = 0;
  for (const auto& t : apex_test::registry()) {
    if (filter && !std::strstr(t.name, filter)) continue;
    apex_test::current_failed() = false;
    t.fn();
    ++run;
    if (apex_test::current_failed()) {
      ++apex_test::failures();
      std::printf("FAIL %s\n", t.name);
    } else {
      std::printf("ok   %s\n", t.name);
    }
  }
  std::printf("\n%d tests, %d failed\n", run, apex_test::failures());
  return apex_test::failures() == 0 ? 0 : 1;
}
