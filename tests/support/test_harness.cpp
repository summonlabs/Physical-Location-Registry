// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "support/test_harness.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <utility>

namespace plr_test {
namespace {

std::size_t g_failures = 0;
std::string g_current_test;
std::vector<std::string> g_arguments;
std::vector<std::pair<std::string, std::string>> g_options;

std::string unique_suffix() {
  static std::uint64_t counter = 0;
  ++counter;
  const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
  std::random_device device;
  std::ostringstream stream;
  stream << static_cast<unsigned long long>(now) << '-' << counter << '-'
         << static_cast<unsigned long long>(device());
  return stream.str();
}

}  // namespace

TestRegistry& TestRegistry::instance() {
  static TestRegistry registry;
  return registry;
}

void TestRegistry::add(TestCase test_case) { cases_.push_back(std::move(test_case)); }

void report_failure(const char* file, int line, const std::string& message) {
  ++g_failures;
  std::cout << "FAIL " << g_current_test << " (" << file << ":" << line << "): " << message
            << std::endl;
}

std::size_t current_failure_count() { return g_failures; }

const std::vector<std::string>& test_arguments() { return g_arguments; }

const std::string* test_option(std::string_view name) {
  for (const auto& option : g_options) {
    if (option.first == name) {
      return &option.second;
    }
  }
  return nullptr;
}

TempDir::TempDir() {
  std::error_code error;
  const auto base = std::filesystem::temp_directory_path(error);
  const auto root = error ? std::filesystem::path(".") : base;
  path_ = root / ("plr-test-" + unique_suffix());
  std::filesystem::create_directories(path_, error);
  if (error) {
    std::cout << "WARN could not create temporary directory " << path_.string() << ": "
              << error.message() << std::endl;
  }
}

TempDir::~TempDir() {
  std::error_code error;
  std::filesystem::remove_all(path_, error);
}

std::filesystem::path TempDir::file(std::string_view name) const {
  std::filesystem::path result = path_;
  result /= std::string(name);
  return result;
}

std::uint64_t Rng::next_u64() {
  // splitmix64
  state_ += 0x9E3779B97F4A7C15ULL;
  std::uint64_t value = state_;
  value = (value ^ (value >> 30U)) * 0xBF58476D1CE4E5B9ULL;
  value = (value ^ (value >> 27U)) * 0x94D049BB133111EBULL;
  return value ^ (value >> 31U);
}

std::uint32_t Rng::below(std::uint32_t bound) {
  if (bound == 0) {
    return 0;
  }
  return static_cast<std::uint32_t>(next_u64() % bound);
}

bool Rng::chance(unsigned percent) { return below(100) < percent; }

std::string Rng::token(std::size_t length) {
  static constexpr char kAlphabet[] = "abcdefghijklmnopqrstuvwxyz0123456789";
  std::string text;
  text.reserve(length);
  for (std::size_t index = 0; index < length; ++index) {
    text.push_back(kAlphabet[below(36)]);
  }
  return text;
}

std::string Rng::component(std::string_view prefix) {
  std::string text(prefix);
  text.push_back('-');
  text.append(token(4));
  return text;
}

int run_all_tests(int argc, char** argv, const RunOptions& options) {
  for (int index = 1; index < argc; ++index) {
    const std::string argument = argv[index];
    if (argument.rfind("--", 0) == 0) {
      const std::size_t equals = argument.find('=');
      if (equals != std::string::npos) {
        g_options.emplace_back(argument.substr(0, equals), argument.substr(equals + 1));
      } else if (index + 1 < argc && argv[index + 1][0] != '-') {
        g_options.emplace_back(argument, argv[index + 1]);
        ++index;
      } else {
        g_options.emplace_back(argument, std::string());
      }
      continue;
    }
    g_arguments.push_back(argument);
  }

  std::size_t executed = 0;
  std::size_t failed_cases = 0;
  for (const TestCase& test_case : TestRegistry::instance().cases()) {
    const std::string full_name = test_case.suite + "." + test_case.name;
    if (!options.filter.empty() && full_name.find(options.filter) == std::string::npos) {
      continue;
    }
    g_current_test = full_name;
    const std::size_t before = g_failures;
    ++executed;
    try {
      test_case.function();
    } catch (const RequirementFailed& failure) {
      std::cout << "ABORTED " << full_name << ": " << failure.message() << std::endl;
    } catch (const std::exception& error) {
      report_failure(__FILE__, __LINE__,
                     std::string("unexpected exception: ") + error.what());
    } catch (...) {
      report_failure(__FILE__, __LINE__, "unexpected non-standard exception");
    }
    if (g_failures != before) {
      ++failed_cases;
    } else if (options.verbose) {
      std::cout << "ok   " << full_name << std::endl;
    }
  }

  std::cout << "----" << std::endl;
  std::cout << "tests=" << executed << " failed-cases=" << failed_cases
            << " failures=" << g_failures << std::endl;
  return g_failures == 0 ? 0 : 1;
}

}  // namespace plr_test
