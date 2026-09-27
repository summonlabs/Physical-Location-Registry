// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "support/test_process.hpp"

#include <chrono>
#include <cstring>
#include <fstream>
#include <sstream>
#include <thread>
#include <utility>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace plr_test {
namespace {

using dccp::physical_location_registry::Error;
using dccp::physical_location_registry::ErrorCode;
using dccp::physical_location_registry::make_error;

#if defined(_WIN32)

std::string quote_argument(const std::string& argument) {
  std::string quoted = "\"";
  for (const char character : argument) {
    if (character == '"') {
      quoted.append("\\\"");
    } else {
      quoted.push_back(character);
    }
  }
  quoted.push_back('"');
  return quoted;
}

std::string last_error_text() {
  const DWORD code = GetLastError();
  return "win32 error " + std::to_string(code);
}

#else

std::string last_error_text() { return std::strerror(errno); }

#endif

}  // namespace

struct ChildProcess::Impl {
  std::string collected;
#if defined(_WIN32)
  HANDLE process = nullptr;
  HANDLE stdin_write = nullptr;
  HANDLE stdout_read = nullptr;
#else
  int pid = -1;
  int stdin_write = -1;
  int stdout_read = -1;
#endif
  bool waited = false;
  int exit_code = -1;
};

ChildProcess::ChildProcess() : impl_(std::make_unique<Impl>()) {}

ChildProcess::~ChildProcess() {
  if (running_) {
    (void)terminate();
  }
#if defined(_WIN32)
  if (impl_->stdin_write != nullptr) {
    CloseHandle(impl_->stdin_write);
  }
  if (impl_->stdout_read != nullptr) {
    CloseHandle(impl_->stdout_read);
  }
  if (impl_->process != nullptr) {
    CloseHandle(impl_->process);
  }
#else
  if (impl_->stdin_write >= 0) {
    ::close(impl_->stdin_write);
  }
  if (impl_->stdout_read >= 0) {
    ::close(impl_->stdout_read);
  }
#endif
}

Result<std::unique_ptr<ChildProcess>> ChildProcess::start(const std::filesystem::path& program,
                                                          const std::vector<std::string>& arguments) {
  auto child = std::unique_ptr<ChildProcess>(new ChildProcess());

#if defined(_WIN32)
  SECURITY_ATTRIBUTES attributes{};
  attributes.nLength = sizeof(SECURITY_ATTRIBUTES);
  attributes.bInheritHandle = TRUE;
  attributes.lpSecurityDescriptor = nullptr;

  HANDLE child_stdin_read = nullptr;
  HANDLE child_stdout_write = nullptr;
  if (CreatePipe(&child_stdin_read, &child->impl_->stdin_write, &attributes, 0) == 0) {
    return make_error(ErrorCode::IoError, "CreatePipe failed: " + last_error_text());
  }
  if (CreatePipe(&child->impl_->stdout_read, &child_stdout_write, &attributes, 0) == 0) {
    return make_error(ErrorCode::IoError, "CreatePipe failed: " + last_error_text());
  }
  SetHandleInformation(child->impl_->stdin_write, HANDLE_FLAG_INHERIT, 0);
  SetHandleInformation(child->impl_->stdout_read, HANDLE_FLAG_INHERIT, 0);

  std::string command_line = quote_argument(program.string());
  for (const std::string& argument : arguments) {
    command_line.push_back(' ');
    command_line.append(quote_argument(argument));
  }

  STARTUPINFOW startup{};
  startup.cb = sizeof(STARTUPINFOW);
  startup.dwFlags = STARTF_USESTDHANDLES;
  startup.hStdInput = child_stdin_read;
  startup.hStdOutput = child_stdout_write;
  startup.hStdError = child_stdout_write;

  PROCESS_INFORMATION process_info{};
  std::wstring wide_command(command_line.begin(), command_line.end());
  const BOOL started = CreateProcessW(nullptr, wide_command.data(), nullptr, nullptr, TRUE, 0,
                                      nullptr, nullptr, &startup, &process_info);
  CloseHandle(child_stdin_read);
  CloseHandle(child_stdout_write);
  if (started == 0) {
    return make_error(ErrorCode::IoError, "CreateProcess failed: " + last_error_text())
        .with_subject(program.string());
  }
  CloseHandle(process_info.hThread);
  child->impl_->process = process_info.hProcess;
  child->running_ = true;
  return child;
#else
  int stdin_pipe[2] = {-1, -1};
  int stdout_pipe[2] = {-1, -1};
  if (pipe(stdin_pipe) != 0 || pipe(stdout_pipe) != 0) {
    return make_error(ErrorCode::IoError, "pipe failed");
  }
  const pid_t pid = fork();
  if (pid < 0) {
    return make_error(ErrorCode::IoError, "fork failed");
  }
  if (pid == 0) {
    dup2(stdin_pipe[0], STDIN_FILENO);
    dup2(stdout_pipe[1], STDOUT_FILENO);
    dup2(stdout_pipe[1], STDERR_FILENO);
    close(stdin_pipe[0]);
    close(stdin_pipe[1]);
    close(stdout_pipe[0]);
    close(stdout_pipe[1]);
    std::vector<std::string> owned;
    owned.push_back(program.string());
    for (const std::string& argument : arguments) {
      owned.push_back(argument);
    }
    std::vector<char*> argv;
    for (std::string& item : owned) {
      argv.push_back(item.data());
    }
    argv.push_back(nullptr);
    execv(program.c_str(), argv.data());
    _exit(127);
  }
  close(stdin_pipe[0]);
  close(stdout_pipe[1]);
  child->impl_->pid = pid;
  child->impl_->stdin_write = stdin_pipe[1];
  child->impl_->stdout_read = stdout_pipe[0];
  child->running_ = true;
  return child;
#endif
}

Result<void> ChildProcess::write_line(const std::string& line) {
  const std::string text = line + "\n";
#if defined(_WIN32)
  DWORD written = 0;
  if (WriteFile(impl_->stdin_write, text.data(), static_cast<DWORD>(text.size()), &written, nullptr) ==
      0) {
    return make_error(ErrorCode::IoError, "writing to the child failed: " + last_error_text());
  }
#else
  if (::write(impl_->stdin_write, text.data(), text.size()) < 0) {
    return make_error(ErrorCode::IoError, "writing to the child failed");
  }
#endif
  return dccp::physical_location_registry::ok();
}

Result<void> ChildProcess::close_stdin() {
#if defined(_WIN32)
  if (impl_->stdin_write != nullptr) {
    CloseHandle(impl_->stdin_write);
    impl_->stdin_write = nullptr;
  }
#else
  if (impl_->stdin_write >= 0) {
    ::close(impl_->stdin_write);
    impl_->stdin_write = -1;
  }
#endif
  return dccp::physical_location_registry::ok();
}

Result<int> ChildProcess::wait() {
#if defined(_WIN32)
  if (impl_->process == nullptr) {
    return make_error(ErrorCode::InvalidArgument, "child was never started");
  }
  (void)WaitForSingleObject(impl_->process, INFINITE);
  DWORD code = 0;
  if (GetExitCodeProcess(impl_->process, &code) == 0) {
    return make_error(ErrorCode::IoError, "GetExitCodeProcess failed: " + last_error_text());
  }
  impl_->exit_code = static_cast<int>(code);
  impl_->waited = true;
  running_ = false;
  return impl_->exit_code;
#else
  int status = 0;
  while (::waitpid(impl_->pid, &status, 0) < 0) {
    return make_error(ErrorCode::IoError, "waitpid failed");
  }
  impl_->exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -(WTERMSIG(status));
  impl_->waited = true;
  running_ = false;
  return impl_->exit_code;
#endif
}

Result<std::string> ChildProcess::output() const {
  std::string collected = impl_->collected;
#if defined(_WIN32)
  if (impl_->stdout_read != nullptr) {
    char buffer[4096];
    DWORD read = 0;
    while (ReadFile(impl_->stdout_read, buffer, sizeof(buffer), &read, nullptr) != 0 && read > 0) {
      collected.append(buffer, read);
    }
  }
#else
  if (impl_->stdout_read >= 0) {
    char buffer[4096];
    const ssize_t read = ::read(impl_->stdout_read, buffer, sizeof(buffer));
    if (read > 0) {
      collected.append(buffer, static_cast<std::size_t>(read));
    }
  }
#endif
  return collected;
}

Result<void> ChildProcess::terminate() {
#if defined(_WIN32)
  if (impl_->process != nullptr && running_) {
    (void)TerminateProcess(impl_->process, 1);
    (void)WaitForSingleObject(impl_->process, INFINITE);
    DWORD code = 0;
    (void)GetExitCodeProcess(impl_->process, &code);
    impl_->exit_code = static_cast<int>(code);
    running_ = false;
  }
#else
  if (impl_->pid > 0 && running_) {
    (void)::kill(impl_->pid, SIGKILL);
    int status = 0;
    (void)::waitpid(impl_->pid, &status, 0);
    impl_->exit_code = -1;
    running_ = false;
  }
#endif
  return dccp::physical_location_registry::ok();
}

Result<int> run_process(const std::filesystem::path& program,
                        const std::vector<std::string>& arguments,
                        std::string* output) {
  auto started = ChildProcess::start(program, arguments);
  if (!started.has_value()) {
    return started.error();
  }
  std::unique_ptr<ChildProcess> child = std::move(started.value());
  (void)child->close_stdin();
  const auto code = child->wait();
  if (!code.has_value()) {
    return code.error();
  }
  if (output != nullptr) {
    const auto text = child->output();
    if (text.has_value()) {
      *output = text.value();
    }
  }
  return code.value();
}

bool wait_for_file(const std::filesystem::path& path, unsigned attempts) {
  std::error_code error;
  for (unsigned attempt = 0; attempt < attempts; ++attempt) {
    if (std::filesystem::exists(path, error) && !error) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return false;
}

bool wait_for_file_content(const std::filesystem::path& path, std::string* content,
                           unsigned attempts) {
  for (unsigned attempt = 0; attempt < attempts; ++attempt) {
    std::error_code error;
    if (std::filesystem::exists(path, error) && !error) {
      std::ifstream stream(path, std::ios::binary);
      std::string line;
      if (std::getline(stream, line) && !line.empty()) {
        if (content != nullptr) {
          *content = line;
        }
        return true;
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return false;
}

}  // namespace plr_test
