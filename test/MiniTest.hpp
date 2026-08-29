#pragma once

#include <string>
#include <vector>

namespace minitest {

using TestFunction = void (*)();

struct TestCase {
  std::string name;
  TestFunction function;
};

std::vector<TestCase> &registry();

class Registrar {
public:
  Registrar(const char *name, TestFunction function);
};

void require(bool condition, const char *expression, const char *file,
             int line);

} // namespace minitest

#define MINITEST_CONCAT_IMPL(left, right) left##right
#define MINITEST_CONCAT(left, right) MINITEST_CONCAT_IMPL(left, right)

#define TEST_CASE(name, tags)                                                  \
  static void MINITEST_CONCAT(minitest_case_, __LINE__)();                     \
  static const ::minitest::Registrar MINITEST_CONCAT(minitest_registrar_,      \
                                                     __LINE__)(                \
      name, &MINITEST_CONCAT(minitest_case_, __LINE__));                       \
  static void MINITEST_CONCAT(minitest_case_, __LINE__)()

#define REQUIRE(expression)                                                    \
  ::minitest::require(static_cast<bool>(expression), #expression, __FILE__,    \
                      __LINE__)

#define REQUIRE_FALSE(expression) REQUIRE(!(expression))
