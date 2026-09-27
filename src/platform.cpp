// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "platform.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <system_error>

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
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace dccp::physical_location_registry::internal {
namespace {

Error io_error(std::string_view what, const std::filesystem::path& path, std::string detail) {
  std::string message(what);
  message.append(" failed");
  if (!detail.empty()) {
    message.append(": ");
    message.append(detail);
  }
  return Error(ErrorCode::IoError, std::move(message)).with_subject(path.string());
}

#if defined(_WIN32)

std::string last_error_text() {
  const DWORD code = GetLastError();
  LPWSTR buffer = nullptr;
  const DWORD length = FormatMessageW(
      FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
      nullptr, code, 0, reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);
  std::string text = "win32 error " + std::to_string(code);
  if (length != 0 && buffer != nullptr) {
    std::wstring wide(buffer, length);
    while (!wide.empty() && (wide.back() == L'\r' || wide.back() == L'\n' || wide.back() == L' ')) {
      wide.pop_back();
    }
    text.append(" (");
    for (const wchar_t character : wide) {
      text.push_back(character < 128 ? static_cast<char>(character) : '?');
    }
    text.push_back(')');
  }
  if (buffer != nullptr) {
    LocalFree(buffer);
  }
  return text;
}

bool is_missing_error(DWORD code) {
  return code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND ||
         code == ERROR_INVALID_NAME;
}

#else

std::string last_error_text() { return std::strerror(errno); }

bool is_missing_error(int code) { return code == ENOENT || code == ENOTDIR; }

#endif

}  // namespace

LockHandle::~LockHandle() {
  if (!held) {
    return;
  }
#if defined(_WIN32)
  if (file_handle != nullptr) {
    OVERLAPPED* overlapped_ptr = static_cast<OVERLAPPED*>(overlapped);
    UnlockFile(static_cast<HANDLE>(file_handle), 0, 0, 1, 0);
    CloseHandle(static_cast<HANDLE>(file_handle));
    delete overlapped_ptr;
  }
#else
  if (descriptor >= 0) {
    ::flock(descriptor, LOCK_UN);
    ::close(descriptor);
  }
#endif
  file_handle = nullptr;
  overlapped = nullptr;
  descriptor = -1;
  held = false;
}

Result<void> ensure_directory(const std::filesystem::path& directory) {
  std::error_code error;
  if (std::filesystem::exists(directory, error)) {
    if (std::filesystem::is_directory(directory, error)) {
      return ok();
    }
    return Error(ErrorCode::StoreExists, "path exists and is not a directory")
        .with_subject(directory.string());
  }
  std::filesystem::create_directories(directory, error);
  if (error) {
    return io_error("create_directories", directory, error.message());
  }
  return ok();
}

Result<bool> directory_exists(const std::filesystem::path& directory) {
  std::error_code error;
  const bool present = std::filesystem::exists(directory, error);
  if (error) {
    return io_error("exists", directory, error.message());
  }
  if (!present) {
    return false;
  }
  return std::filesystem::is_directory(directory, error) && !error;
}

Result<bool> regular_file_exists(const std::filesystem::path& path) {
  std::error_code error;
  const bool present = std::filesystem::exists(path, error);
  if (error) {
    return io_error("exists", path, error.message());
  }
  if (!present) {
    return false;
  }
  return std::filesystem::is_regular_file(path, error) && !error;
}

Result<std::uint64_t> file_size(const std::filesystem::path& path) {
  std::error_code error;
  const auto size = std::filesystem::file_size(path, error);
  if (error) {
    return io_error("file_size", path, error.message());
  }
  return static_cast<std::uint64_t>(size);
}

Result<std::string> read_file_bounded(const std::filesystem::path& path, std::uint64_t max_bytes) {
  PLR_TRY(exists, regular_file_exists(path));
  if (!exists) {
    return Error(ErrorCode::StoreNotFound, "file does not exist").with_subject(path.string());
  }
  PLR_TRY(size, internal::file_size(path));
  if (size > max_bytes) {
    return Error(ErrorCode::LimitExceeded,
                 "file is larger than the permitted maximum of " + std::to_string(max_bytes) + " bytes")
        .with_subject(path.string());
  }

#if defined(_WIN32)
  HANDLE handle = CreateFileW(path.wstring().c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    const DWORD code = GetLastError();
    if (is_missing_error(code)) {
      return Error(ErrorCode::StoreNotFound, "file does not exist").with_subject(path.string());
    }
    return io_error("CreateFile", path, last_error_text());
  }
  std::string content;
  content.resize(static_cast<std::size_t>(size));
  std::size_t offset = 0;
  while (offset < content.size()) {
    const DWORD chunk = static_cast<DWORD>(
        std::min<std::size_t>(content.size() - offset, 1U << 20U));
    DWORD read = 0;
    if (ReadFile(handle, content.data() + offset, chunk, &read, nullptr) == 0) {
      CloseHandle(handle);
      return io_error("ReadFile", path, last_error_text());
    }
    if (read == 0) {
      break;
    }
    offset += read;
  }
  CloseHandle(handle);
  if (offset != content.size()) {
    return Error(ErrorCode::TruncatedInput, "file shrank while it was being read")
        .with_subject(path.string());
  }
  return content;
#else
  const int descriptor = ::open(path.c_str(), O_RDONLY);
  if (descriptor < 0) {
    if (is_missing_error(errno)) {
      return Error(ErrorCode::StoreNotFound, "file does not exist").with_subject(path.string());
    }
    return io_error("open", path, last_error_text());
  }
  std::string content;
  content.resize(static_cast<std::size_t>(size));
  std::size_t offset = 0;
  while (offset < content.size()) {
    const ssize_t read = ::read(descriptor, content.data() + offset, content.size() - offset);
    if (read < 0) {
      if (errno == EINTR) {
        continue;
      }
      ::close(descriptor);
      return io_error("read", path, last_error_text());
    }
    if (read == 0) {
      break;
    }
    offset += static_cast<std::size_t>(read);
  }
  ::close(descriptor);
  if (offset != content.size()) {
    return Error(ErrorCode::TruncatedInput, "file shrank while it was being read")
        .with_subject(path.string());
  }
  return content;
#endif
}

Result<void> write_file(const std::filesystem::path& path, std::string_view bytes, bool durable) {
#if defined(_WIN32)
  HANDLE handle = CreateFileW(path.wstring().c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    return io_error("CreateFile", path, last_error_text());
  }
  std::size_t offset = 0;
  while (offset < bytes.size()) {
    const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(bytes.size() - offset, 1U << 20U));
    DWORD written = 0;
    if (WriteFile(handle, bytes.data() + offset, chunk, &written, nullptr) == 0) {
      CloseHandle(handle);
      return io_error("WriteFile", path, last_error_text());
    }
    offset += written;
  }
  if (durable && FlushFileBuffers(handle) == 0) {
    CloseHandle(handle);
    return io_error("FlushFileBuffers", path, last_error_text());
  }
  CloseHandle(handle);
  return ok();
#else
  const int descriptor = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (descriptor < 0) {
    return io_error("open", path, last_error_text());
  }
  std::size_t offset = 0;
  while (offset < bytes.size()) {
    const ssize_t written = ::write(descriptor, bytes.data() + offset, bytes.size() - offset);
    if (written < 0) {
      if (errno == EINTR) {
        continue;
      }
      ::close(descriptor);
      return io_error("write", path, last_error_text());
    }
    offset += static_cast<std::size_t>(written);
  }
  if (durable && ::fsync(descriptor) != 0) {
    ::close(descriptor);
    return io_error("fsync", path, last_error_text());
  }
  if (::close(descriptor) != 0) {
    return io_error("close", path, last_error_text());
  }
  return ok();
#endif
}

Result<void> replace_file(const std::filesystem::path& from,
                          const std::filesystem::path& to,
                          bool durable) {
#if defined(_WIN32)
  DWORD flags = MOVEFILE_REPLACE_EXISTING;
  if (durable) {
    flags |= MOVEFILE_WRITE_THROUGH;
  }
  if (MoveFileExW(from.wstring().c_str(), to.wstring().c_str(), flags) == 0) {
    return io_error("MoveFileEx", to, last_error_text());
  }
  return ok();
#else
  (void)durable;
  if (::rename(from.c_str(), to.c_str()) != 0) {
    return io_error("rename", to, last_error_text());
  }
  return ok();
#endif
}

Result<void> sync_directory(const std::filesystem::path& directory, bool durable) {
  if (!durable) {
    return ok();
  }
#if defined(_WIN32)
  // MoveFileEx(MOVEFILE_WRITE_THROUGH) already flushes the directory metadata on
  // Windows; there is no directory handle to fsync. Report success explicitly
  // rather than pretending to do more than the platform supports.
  (void)directory;
  return ok();
#else
  const int descriptor = ::open(directory.c_str(), O_RDONLY);
  if (descriptor < 0) {
    return io_error("open directory", directory, last_error_text());
  }
  const int result = ::fsync(descriptor);
  ::close(descriptor);
  if (result != 0) {
    return io_error("fsync directory", directory, last_error_text());
  }
  return ok();
#endif
}

Result<void> remove_file(const std::filesystem::path& path) {
  PLR_TRY(exists, regular_file_exists(path));
  if (!exists) {
    return ok();
  }
  std::error_code error;
  if (!std::filesystem::remove(path, error)) {
    if (error) {
      return io_error("remove", path, error.message());
    }
  }
  return ok();
}

Result<std::vector<std::string>> list_file_names(const std::filesystem::path& directory) {
  std::error_code error;
  std::vector<std::string> names;
  std::filesystem::directory_iterator iterator(directory, error);
  if (error) {
    return io_error("directory_iterator", directory, error.message());
  }
  for (const auto& entry : iterator) {
    std::error_code type_error;
    if (!entry.is_regular_file(type_error)) {
      continue;
    }
    names.push_back(entry.path().filename().string());
  }
  std::sort(names.begin(), names.end());
  return names;
}

Result<std::shared_ptr<LockHandle>> acquire_exclusive_lock(const std::filesystem::path& lock_path) {
  auto handle = std::make_shared<LockHandle>();
  handle->path = lock_path;

#if defined(_WIN32)
  HANDLE file = CreateFileW(lock_path.wstring().c_str(), GENERIC_READ | GENERIC_WRITE,
                            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    return io_error("CreateFile", lock_path, last_error_text());
  }
  auto* overlapped = new OVERLAPPED();
  std::memset(overlapped, 0, sizeof(OVERLAPPED));
  if (LockFileEx(file, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, overlapped) == 0) {
    const DWORD code = GetLastError();
    CloseHandle(file);
    delete overlapped;
    if (code == ERROR_LOCK_VIOLATION || code == ERROR_IO_PENDING) {
      return Error(ErrorCode::StoreLocked,
                   "another writer holds this store; only one writer may own a store directory")
          .with_subject(lock_path.string());
    }
    return io_error("LockFileEx", lock_path, last_error_text());
  }
  handle->file_handle = file;
  handle->overlapped = overlapped;
  handle->held = true;
  return handle;
#else
  const int descriptor = ::open(lock_path.c_str(), O_RDWR | O_CREAT, 0644);
  if (descriptor < 0) {
    return io_error("open lock", lock_path, last_error_text());
  }
  if (::flock(descriptor, LOCK_EX | LOCK_NB) != 0) {
    const int code = errno;
    ::close(descriptor);
    if (code == EWOULDBLOCK || code == EAGAIN) {
      return Error(ErrorCode::StoreLocked,
                   "another writer holds this store; only one writer may own a store directory")
          .with_subject(lock_path.string());
    }
    return io_error("flock", lock_path, last_error_text());
  }
  handle->descriptor = descriptor;
  handle->held = true;
  return handle;
#endif
}

Result<void> release_lock(const std::shared_ptr<LockHandle>& lock) {
  if (!lock || !lock->held) {
    return ok();
  }
#if defined(_WIN32)
  OVERLAPPED* overlapped_ptr = static_cast<OVERLAPPED*>(lock->overlapped);
  HANDLE file = static_cast<HANDLE>(lock->file_handle);
  UnlockFile(file, 0, 0, 1, 0);
  CloseHandle(file);
  delete overlapped_ptr;
  lock->file_handle = nullptr;
  lock->overlapped = nullptr;
#else
  ::flock(lock->descriptor, LOCK_UN);
  ::close(lock->descriptor);
  lock->descriptor = -1;
#endif
  lock->held = false;
  return ok();
}

void abrupt_exit(int code) { std::_Exit(code); }

}  // namespace dccp::physical_location_registry::internal
