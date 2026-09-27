// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Minimal deterministic test harness. No timeouts, no watchdogs, no
// process-kill-as-pass: a test either completes and reports, or it is a defect
// to diagnose.

#ifndef PLR_TESTS_SUPPORT_TEST_HARNESS_HPP
#define PLR_TESTS_SUPPORT_TEST_HARNESS_HPP

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include "dccp/physical_location_registry/result.hpp"

namespace plr_test {

using dccp::physical_location_registry::Error;
using dccp::physical_location_registry::ErrorCode;
using dccp::physical_location_registry::Result;

/// Thrown by PLR_REQUIRE to stop the current test case immediately.
class RequirementFailed {
 public:
  explicit RequirementFailed(std::string message) : message_(std::move(message)) {}
  const std::string& message() const noexcept { return message_; }

 private:
  std::string message_;
};

struct TestCase {
  std::string suite;
  std::string name;
  void (*function)();
};

/// Registration point used by the PLR_TEST macro.
class TestRegistry {
 public:
  static TestRegistry& instance();
  void add(TestCase test_case);
  const std::vector<TestCase>& cases() const noexcept { return cases_; }

 private:
  std::vector<TestCase> cases_;
};

/// Records one assertion failure against the running test case.
void report_failure(const char* file, int line, const std::string& message);

/// Number of failures recorded for the running test case.
std::size_t current_failure_count();

struct RunOptions {
  /// Optional substring filter; empty runs everything.
  std::string filter;
  /// Print one line per test case instead of only failures and the summary.
  bool verbose = true;
};

/// Runs the registered tests and returns a process exit code.
int run_all_tests(int argc, char** argv, const RunOptions& options = {});

/// Argument access for tests that need tool paths (for example the
/// multi-process suite).
const std::vector<std::string>& test_arguments();
const std::string* test_option(std::string_view name);

}  // namespace plr_test

#define PLR_TEST(suite_name, test_name)                                                   \
  static void suite_name##_##test_name##_body();                                          \
  namespace {                                                                             \
  const bool suite_name##_##test_name##_registered = []() {                               \
    ::plr_test::TestRegistry::instance().add(                                             \
        ::plr_test::TestCase{#suite_name, #test_name, &suite_name##_##test_name##_body}); \
    return true;                                                                          \
  }();                                                                                    \
  }                                                                                       \
  static void suite_name##_##test_name##_body()

#define PLR_FAIL(message) \
  ::plr_test::report_failure(__FILE__, __LINE__, (message))

#define PLR_EXPECT(condition)                                                          \
  do {                                                                                 \
    if (!(condition)) {                                                                \
      ::plr_test::report_failure(__FILE__, __LINE__, "expected: " #condition);         \
    }                                                                                  \
  } while (false)

#define PLR_EXPECT_MSG(condition, message)                                    \
  do {                                                                        \
    if (!(condition)) {                                                       \
      ::plr_test::report_failure(__FILE__, __LINE__,                          \
                                 std::string("expected: " #condition " - ") + \
                                     std::string(message));                   \
    }                                                                         \
  } while (false)

#define PLR_REQUIRE(condition)                                                        \
  do {                                                                                \
    if (!(condition)) {                                                               \
      ::plr_test::report_failure(__FILE__, __LINE__, "required: " #condition);        \
      throw ::plr_test::RequirementFailed("required: " #condition);                   \
    }                                                                                 \
  } while (false)

namespace plr_test {
namespace detail {

template <class T>
std::string render(const T& value) {
  if constexpr (std::is_convertible_v<T, std::string_view>) {
    return std::string(std::string_view(value));
  } else if constexpr (std::is_enum_v<T>) {
    return std::to_string(static_cast<long long>(value));
  } else if constexpr (std::is_arithmetic_v<T>) {
    return std::to_string(value);
  } else {
    return "<value>";
  }
}

}  // namespace detail
}  // namespace plr_test

#define PLR_EXPECT_EQ(actual, expected)                                                     \
  do {                                                                                      \
    const auto& plr_actual = (actual);                                                      \
    const auto& plr_expected = (expected);                                                  \
    if (!(plr_actual == plr_expected)) {                                                    \
      ::plr_test::report_failure(__FILE__, __LINE__,                                        \
                                 std::string(#actual " == " #expected " (got ") +           \
                                     ::plr_test::detail::render(plr_actual) + " vs " +      \
                                     ::plr_test::detail::render(plr_expected) + ")");       \
    }                                                                                       \
  } while (false)

#define PLR_EXPECT_NE(actual, unexpected)                                          \
  do {                                                                             \
    const auto& plr_actual = (actual);                                             \
    const auto& plr_unexpected = (unexpected);                                     \
    if (plr_actual == plr_unexpected) {                                            \
      ::plr_test::report_failure(__FILE__, __LINE__,                               \
                                 std::string(#actual " != " #unexpected " (both ") + \
                                     ::plr_test::detail::render(plr_actual) + ")");  \
    }                                                                              \
  } while (false)

/// Asserts that an expression yields a value and binds it to a name.
#define PLR_EXPECT_OK(value_name, expression)                                       \
  auto value_name##_plr_test_result = (expression);                                 \
  if (!value_name##_plr_test_result.has_value()) {                                  \
    ::plr_test::report_failure(__FILE__, __LINE__,                                  \
                               std::string("expected success from " #expression     \
                                           " but got ") +                           \
                                   value_name##_plr_test_result.error().to_string()); \
    throw ::plr_test::RequirementFailed("expected success from " #expression);      \
  }                                                                                 \
  auto& value_name = *value_name##_plr_test_result

/// Asserts that an expression fails with a specific code.
#define PLR_EXPECT_ERR(expression, expected_code)                                          \
  do {                                                                                     \
    auto plr_error_result = (expression);                                                  \
    if (plr_error_result.has_value()) {                                                    \
      ::plr_test::report_failure(__FILE__, __LINE__,                                       \
                                 std::string("expected " #expected_code " from " #expression \
                                             " but the operation succeeded"));             \
    } else if (plr_error_result.error().code() != (expected_code)) {                       \
      ::plr_test::report_failure(__FILE__, __LINE__,                                       \
                                 std::string("expected " #expected_code " from " #expression \
                                             " but got ") +                                \
                                     plr_error_result.error().to_string());                \
    }                                                                                      \
  } while (false)

namespace plr_test {

/// A unique temporary directory, removed when the object goes out of scope.
class TempDir {
 public:
  TempDir();
  ~TempDir();

  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;

  const std::filesystem::path& path() const noexcept { return path_; }
  std::filesystem::path file(std::string_view name) const;
  std::filesystem::path operator/(std::string_view name) const { return file(name); }

 private:
  std::filesystem::path path_;
};

/// Deterministic pseudo-random generator (splitmix64), so every property test
/// is reproducible from its seed.
class Rng {
 public:
  explicit Rng(std::uint64_t seed) : state_(seed) {}

  std::uint64_t next_u64();
  std::uint32_t below(std::uint32_t bound);
  bool chance(unsigned percent);
  std::string token(std::size_t length);
  std::string component(std::string_view prefix);

  std::uint64_t seed() const noexcept { return seed_; }

 private:
  std::uint64_t state_;
  std::uint64_t seed_ = 0;
};

}  // namespace plr_test

#endif  // PLR_TESTS_SUPPORT_TEST_HARNESS_HPP
