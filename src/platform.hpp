// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Internal operating-system layer: durable file writes, atomic replacement,
// bounded reads, directory listing and the advisory writer lock. Kept behind a
// narrow interface so the store logic contains no platform branches.

#ifndef DCCP_PHYSICAL_LOCATION_REGISTRY_SRC_PLATFORM_HPP
#define DCCP_PHYSICAL_LOCATION_REGISTRY_SRC_PLATFORM_HPP

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "dccp/physical_location_registry/result.hpp"

namespace dccp::physical_location_registry::internal {

/// An exclusive advisory lock on a lock file, held for the lifetime of a writer
/// session. The operating system releases it if the process dies, which is what
/// makes writer fencing survive a crash.
struct LockHandle {
  void* file_handle = nullptr;   ///< HANDLE on Windows, unused on POSIX
  void* overlapped = nullptr;    ///< OVERLAPPED* on Windows, unused on POSIX
  int descriptor = -1;           ///< file descriptor on POSIX
  std::filesystem::path path;
  bool held = false;

  ~LockHandle();
};

/// Creates the directory (and parents) when it does not exist.
Result<void> ensure_directory(const std::filesystem::path& directory);

/// True when the path exists and is a directory.
Result<bool> directory_exists(const std::filesystem::path& directory);

/// True when the path exists and is a regular file.
Result<bool> regular_file_exists(const std::filesystem::path& path);

/// Reads a whole file, refusing to read more than max_bytes.
Result<std::string> read_file_bounded(const std::filesystem::path& path, std::uint64_t max_bytes);

/// Writes bytes to path, creating or truncating it. When durable is set the
/// contents are flushed to stable storage before the call returns.
Result<void> write_file(const std::filesystem::path& path, std::string_view bytes, bool durable);

/// Atomically replaces "to" with "from" (same volume). When durable is set the
/// replacement is flushed through to stable storage where the platform supports
/// it.
Result<void> replace_file(const std::filesystem::path& from,
                          const std::filesystem::path& to,
                          bool durable);

/// Flushes a directory entry to stable storage where the platform supports it.
/// Windows reports success without doing anything: MoveFileEx with
/// MOVEFILE_WRITE_THROUGH already covers the metadata update.
Result<void> sync_directory(const std::filesystem::path& directory, bool durable);

/// Removes a file. Missing files are not an error.
Result<void> remove_file(const std::filesystem::path& path);

/// File names (not paths) of the regular files directly inside a directory,
/// sorted byte-wise.
Result<std::vector<std::string>> list_file_names(const std::filesystem::path& directory);

/// Size of a regular file in bytes.
Result<std::uint64_t> file_size(const std::filesystem::path& path);

/// Acquires the exclusive writer lock, or reports StoreLocked when another
/// holder owns it. Non-blocking: never waits.
Result<std::shared_ptr<LockHandle>> acquire_exclusive_lock(const std::filesystem::path& lock_path);

/// Releases a lock. Idempotent.
Result<void> release_lock(const std::shared_ptr<LockHandle>& lock);

/// Exit code used by the deliberate abrupt-exit path that proves crash
/// behavior. Documented and stable so tests can assert on it.
inline constexpr int kInjectedCrashExitCode = 70;

/// Terminates the process immediately without unwinding: no destructors, no
/// buffered flush, no lock release by this code. Used only by the documented
/// fault-injection path.
[[noreturn]] void abrupt_exit(int code);

}  // namespace dccp::physical_location_registry::internal

#endif  // DCCP_PHYSICAL_LOCATION_REGISTRY_SRC_PLATFORM_HPP
