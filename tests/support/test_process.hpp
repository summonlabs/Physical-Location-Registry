// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Real child-process control for the multi-process and crash tests. Nothing
// here uses a timeout: a child is either signalled by the parent through its
// stdin, terminated deliberately to simulate a crash, or waited for until it
// exits on its own.

#ifndef PLR_TESTS_SUPPORT_TEST_PROCESS_HPP
#define PLR_TESTS_SUPPORT_TEST_PROCESS_HPP

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "dccp/physical_location_registry/result.hpp"

namespace plr_test {

using dccp::physical_location_registry::Result;

/// A running child process with piped standard input and output.
class ChildProcess {
 public:
  ChildProcess();
  ~ChildProcess();

  ChildProcess(const ChildProcess&) = delete;
  ChildProcess& operator=(const ChildProcess&) = delete;

  /// Starts a program. The program path must exist.
  static Result<std::unique_ptr<ChildProcess>> start(const std::filesystem::path& program,
                                                     const std::vector<std::string>& arguments);

  /// Writes one line to the child's standard input.
  Result<void> write_line(const std::string& line);

  /// Closes the child's standard input, signalling "no more input".
  Result<void> close_stdin();

  /// Waits for the child to exit and returns its exit code.
  Result<int> wait();

  /// Reads everything the child has written so far plus what it writes before
  /// exiting; call after wait().
  Result<std::string> output() const;

  /// Terminates the child immediately (used only to simulate a crash).
  Result<void> terminate();

  bool running() const noexcept { return running_; }

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  bool running_ = false;
};

/// Runs a program to completion and returns its exit code and output.
Result<int> run_process(const std::filesystem::path& program,
                        const std::vector<std::string>& arguments,
                        std::string* output);

/// Waits until a file exists, polling with a bounded number of short sleeps.
/// Returns false when the bound is exhausted; the caller decides what that
/// means (it is never treated as a passing test).
bool wait_for_file(const std::filesystem::path& path, unsigned attempts = 2000);

/// Waits until a file exists and contains a non-empty first line.
bool wait_for_file_content(const std::filesystem::path& path, std::string* content,
                           unsigned attempts = 2000);

}  // namespace plr_test

#endif  // PLR_TESTS_SUPPORT_TEST_PROCESS_HPP
