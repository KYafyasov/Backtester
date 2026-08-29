#include "MiniTest.hpp"

#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>

namespace minitest {

std::vector<TestCase> &registry() {
  static std::vector<TestCase> tests;
  return tests;
}

Registrar::Registrar(const char *name, TestFunction function) {
  registry().push_back(TestCase{name, function});
}

void require(bool condition, const char *expression, const char *file,
             int line) {
  if (!condition) {
    throw std::runtime_error(std::string(file) + ":" + std::to_string(line) +
                             ": requirement failed: " + expression);
  }
}

} // namespace minitest

int main() {
  int failures = 0;

  for (const auto &test : minitest::registry()) {
    try {
      test.function();
      std::cout << "[PASS] " << test.name << '\n';
    } catch (const std::exception &error) {
      ++failures;
      std::cerr << "[FAIL] " << test.name << ": " << error.what() << '\n';
    } catch (...) {
      ++failures;
      std::cerr << "[FAIL] " << test.name << ": unknown exception\n";
    }
  }

  std::cout << minitest::registry().size() << " test(s), " << failures
            << " failure(s)\n";
  return failures == 0 ? 0 : 1;
}
