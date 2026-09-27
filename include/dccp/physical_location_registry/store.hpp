// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#ifndef DCCP_PHYSICAL_LOCATION_REGISTRY_STORE_HPP
#define DCCP_PHYSICAL_LOCATION_REGISTRY_STORE_HPP

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "dccp/physical_location_registry/export.hpp"
#include "dccp/physical_location_registry/limits.hpp"
#include "dccp/physical_location_registry/result.hpp"
#include "dccp/physical_location_registry/snapshot.hpp"
#include "dccp/physical_location_registry/strong_id.hpp"

namespace dccp::physical_location_registry {

namespace internal {
struct LockHandle;
}  // namespace internal

/// How a store directory is opened.
enum class OpenMode : std::uint8_t {
  /// Loads the published state. Takes no lock: readers never block writers and
  /// always observe a complete published generation, because publication is an
  /// atomic replacement of the head pointer.
  ReadOnly = 0,
  /// Takes the exclusive writer lock, advances the durable writer epoch, and
  /// publishes that authority before returning.
  ReadWrite = 1,
};

/// Canonical lower-case name ("read-only", "read-write").
PLR_API std::string_view open_mode_name(OpenMode mode) noexcept;

/// Publication steps at which a durability test may inject a failure or an
/// abrupt process exit.
///
/// This exists so that crash and partial-write behavior can be proven by test
/// rather than asserted by comment. The commit point is the head switch: a fault
/// before it leaves the previously published generation authoritative, and a
/// fault after it leaves the new generation authoritative and complete.
enum class PublishStage : std::uint8_t {
  None = 0,
  BeforeStateWrite = 1,  ///< before the new publication file is created
  AfterStateWrite = 2,   ///< the temporary publication exists; nothing is published
  BeforeStateRename = 3,  ///< the temporary file is durable but not yet named
  BeforeHeadPublish = 4,  ///< the publication has its final name; head is untouched
  AfterHeadPublish = 5,   ///< the head switch happened: the commit has taken effect
  BeforeRetire = 6,       ///< committed; superseded publications not yet retired
};

/// Canonical lower-case name ("before-state-write", ...).
PLR_API std::string_view publish_stage_name(PublishStage stage) noexcept;

/// What a fault plan does when it triggers.
enum class FaultAction : std::uint8_t {
  None = 0,
  /// Report a failure at the stage. Valid only before the commit point: a
  /// failure reported after the commit point would be a lie, so a plan that
  /// asks for one is rejected when the store is opened.
  Fail = 1,
  /// Terminate the process immediately without unwinding or cleaning up: the
  /// exact behavior of a crash at that instant.
  Crash = 2,
};

/// Canonical lower-case name ("fail", "crash").
PLR_API std::string_view fault_action_name(FaultAction action) noexcept;

/// Deterministic fault injection plan.
struct PLR_API FaultPlan {
  PublishStage stage = PublishStage::None;
  FaultAction action = FaultAction::None;

  /// 1-based publication ordinal to trigger on; 0 means every publication.
  std::uint32_t publication_ordinal = 0;

  friend bool operator==(const FaultPlan&, const FaultPlan&) noexcept = default;
};

/// Options for creating or opening a store directory.
struct PLR_API StoreOptions {
  OpenMode mode = OpenMode::ReadOnly;
  bool create_if_missing = false;

  /// Flush the publication file to stable storage before the head switch.
  bool fsync_state_before_publish = true;

  /// Flush the directory entry after the head switch, where the platform
  /// supports it.
  bool fsync_directory_after_rename = true;

  /// Read the new publication back and verify its integrity digest before the
  /// head switch. Turning this off removes a full read of the published bytes
  /// but also removes the proof that those bytes reached the disk intact.
  bool verify_written_state = true;

  /// Limits for a store that is being created; on an existing store the
  /// persisted limits govern and a mismatch here is rejected.
  std::optional<Limits> requested_limits;

  /// Identity for a store that is being created; generated when unset.
  std::optional<StoreId> store_id;

  /// Failure injection for durability tests. Default: no injection.
  FaultPlan faults;
};

/// What recovery did while opening a store.
///
/// Recovery is conservative: it never invents state, never repairs a payload in
/// place, and refuses to open rather than presenting unverified bytes.
struct PLR_API RecoveryReport {
  bool head_missing = false;
  bool head_unreadable = false;
  bool head_invalid = false;
  bool recovered_older_publication = false;
  StateSequence head_sequence;
  StateSequence recovered_sequence;
  std::uint32_t invalid_publications_skipped = 0;
  bool republished_head = false;

  /// Ordered, deterministic notes explaining exactly what was observed.
  std::vector<std::string> notes;

  bool clean() const noexcept {
    return !head_missing && !head_unreadable && !head_invalid && !recovered_older_publication;
  }

  std::string to_string() const;
};

/// A durable location state store: one directory, one writer at a time.
///
/// Layout inside the directory:
///   store.lock          writer lock file (advisory, operating-system lock)
///   head                current publication pointer, replaced atomically
///   state.<sequence>.plr immutable canonical state publications
///
/// Publication is plan -> write new generation -> verify -> atomic head switch
/// -> retire superseded generations. A crash before the head switch leaves the
/// previous generation authoritative; a crash after it leaves the new one
/// authoritative and complete, because the new file was durable and verified
/// before the switch.
class PLR_API Store {
 public:
  /// Creates a new store and publishes its initial (empty) state.
  static Result<std::shared_ptr<Store>> create(const std::filesystem::path& directory,
                                               const StoreOptions& options);

  /// Opens an existing store, recovering conservatively when head is unusable.
  static Result<std::shared_ptr<Store>> open(const std::filesystem::path& directory,
                                             const StoreOptions& options);

  ~Store();

  Store(const Store&) = delete;
  Store& operator=(const Store&) = delete;
  Store(Store&&) = delete;
  Store& operator=(Store&&) = delete;

  const std::filesystem::path& directory() const noexcept { return directory_; }
  OpenMode mode() const noexcept { return mode_; }
  bool is_writer() const noexcept { return mode_ == OpenMode::ReadWrite; }

  const StoreId& store_id() const noexcept { return store_id_; }
  LocationRevision revision() const noexcept { return revision_; }
  StateSequence sequence() const noexcept { return sequence_; }
  WriterEpoch epoch() const noexcept { return epoch_; }
  const Limits& limits() const noexcept { return limits_; }
  const RecoveryReport& recovery() const noexcept { return recovery_; }

  /// File name of the current publication inside the store directory.
  std::string state_file_name() const;
  std::uint64_t state_bytes() const noexcept { return state_bytes_; }

  /// Lowercase hex SHA-256 of the currently published canonical bytes.
  const std::string& state_digest() const noexcept { return state_digest_; }

  /// Number of successful publications performed through this handle.
  std::uint64_t publications() const noexcept { return publications_; }

  /// Sequence the next publication would carry, or an overflow error.
  Result<StateSequence> next_sequence() const;

  /// Loads and fully validates the current publication.
  Result<Snapshot> load() const;

  /// Canonical bytes of the current publication.
  Result<std::string> read_published_state() const;

  /// Verifies that the current publication matches the head pointer and passes
  /// its integrity check. Never mutates anything.
  Result<void> verify() const;

  /// Publishes the next state generation. The snapshot must belong to this
  /// store, must not lower the revision, and must satisfy the persisted limits.
  /// On success the store's revision, sequence and digest are updated; on any
  /// failure the previously published generation remains authoritative.
  Result<StateSequence> publish(const Snapshot& snapshot);

  /// Verifies the store and republishes the head pointer from the newest valid
  /// publication. Writer sessions only.
  Result<RecoveryReport> recover_and_republish();

  /// Releases the writer lock. Idempotent; further mutations are rejected.
  Result<void> close();

  bool closed() const noexcept { return closed_; }

 private:
  Store() = default;

  friend struct StoreAccess;

  std::filesystem::path directory_;
  OpenMode mode_ = OpenMode::ReadOnly;
  std::shared_ptr<internal::LockHandle> lock_;
  std::shared_ptr<const StoreOptions> options_;
  StoreId store_id_;
  LocationRevision revision_;
  StateSequence sequence_;
  WriterEpoch epoch_;
  Limits limits_;
  RecoveryReport recovery_;
  std::string state_digest_;
  std::uint64_t state_bytes_ = 0;
  std::uint64_t publications_ = 0;
  bool closed_ = false;
};

}  // namespace dccp::physical_location_registry

#endif  // DCCP_PHYSICAL_LOCATION_REGISTRY_STORE_HPP
