// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#ifndef DCCP_PHYSICAL_LOCATION_REGISTRY_REGISTRY_HPP
#define DCCP_PHYSICAL_LOCATION_REGISTRY_REGISTRY_HPP

#include <filesystem>
#include <memory>
#include <optional>
#include <shared_mutex>
#include <string>
#include <vector>

#include "dccp/physical_location_registry/address.hpp"
#include "dccp/physical_location_registry/diff.hpp"
#include "dccp/physical_location_registry/explanation.hpp"
#include "dccp/physical_location_registry/export.hpp"
#include "dccp/physical_location_registry/limits.hpp"
#include "dccp/physical_location_registry/location.hpp"
#include "dccp/physical_location_registry/requests.hpp"
#include "dccp/physical_location_registry/result.hpp"
#include "dccp/physical_location_registry/snapshot.hpp"
#include "dccp/physical_location_registry/store.hpp"
#include "dccp/physical_location_registry/strong_id.hpp"

namespace dccp::physical_location_registry {

/// Options for creating or opening a registry session.
struct PLR_API RegistryOpenOptions {
  OpenMode mode = OpenMode::ReadWrite;
  bool create_if_missing = false;
  bool fsync_state_before_publish = true;
  bool fsync_directory_after_rename = true;
  std::optional<Limits> limits;
  std::optional<StoreId> store_id;
  FaultPlan faults;
};

/// The authoritative in-memory model of one durable store, plus its mutation
/// API.
///
/// Concurrency model (explicit, and the only one supported):
///   - one Registry instance may be used from many threads;
///   - readers take a shared lock and see one committed revision;
///   - a mutation takes the exclusive lock, validates, applies, publishes
///     durably, and only then reports success;
///   - no callback is ever invoked, no background thread is created, and no lock
///     is ever held across a call back into the registry;
///   - the durable writer lock is acquired at open, before the in-memory lock is
///     ever needed, and released at close, so file lock and memory lock are
///     never nested in both orders.
///
/// Two writer sessions cannot own the same store directory, in this process or
/// in any other: the second open is rejected with StoreLocked.
class PLR_API Registry {
 public:
  /// Creates a new store and returns a read-write session over it.
  static Result<std::shared_ptr<Registry>> create(const std::filesystem::path& directory,
                                                  const RegistryOpenOptions& options = {});

  /// Opens an existing store. Read-write sessions take the writer lock and
  /// advance the durable writer epoch before returning.
  static Result<std::shared_ptr<Registry>> open(const std::filesystem::path& directory,
                                                const RegistryOpenOptions& options = {});

  ~Registry();

  Registry(const Registry&) = delete;
  Registry& operator=(const Registry&) = delete;
  Registry(Registry&&) = delete;
  Registry& operator=(Registry&&) = delete;

  // ----- session facts -----------------------------------------------------

  bool is_writer() const noexcept;
  bool closed() const noexcept;

  StoreId store_id() const;
  LocationRevision revision() const;
  StateSequence sequence() const;
  WriterEpoch epoch() const;
  Limits limits() const;
  RecoveryReport recovery() const;

  /// Noexcept-free accessor for the limits of this session.
  const std::filesystem::path& directory() const noexcept { return directory_; }

  /// The authority to stamp on mutations, or nullopt for a read-only session.
  std::optional<MutationAuthority> authority() const;

  /// On-disk state facts: file name, byte size and digest of the publication
  /// this session last loaded or wrote.
  std::string state_file_name() const;
  std::uint64_t state_bytes() const;
  std::string state_digest() const;

  // ----- queries -----------------------------------------------------------

  Result<LocationView> find(const LocationId& id) const;
  Result<ResolutionResult> resolve(const LocationPath& path,
                                   ResolutionMode mode = ResolutionMode::CurrentOnly) const;
  Result<LocationPath> path_of(const LocationId& id) const;
  Result<std::vector<ChildEntry>> children(const LocationId& id) const;
  Result<std::vector<ChildEntry>> descendants(const LocationId& id, std::uint32_t max_depth) const;
  Result<std::vector<LocationView>> list(const ListOptions& options = {}) const;
  Result<std::vector<AliasBinding>> aliases() const;
  LocationStatistics statistics() const;

  /// Generation diff between two revisions that are still inside the in-memory
  /// journal window. Older pairs are diffed from published state files with
  /// decode_snapshot() plus diff_snapshots().
  Result<RevisionDiff> diff(LocationRevision from, LocationRevision to) const;

  /// Revisions currently diffable through diff().
  std::vector<LocationRevision> retained_revisions() const;

  /// Deep copy of the committed state, safe to query without holding any lock.
  std::shared_ptr<const Snapshot> copy_snapshot() const;

  /// Canonical, digest-checked bytes of the committed state.
  Result<std::string> encode_current_state() const;

  /// Verifies the current publication on disk against its integrity digest.
  Result<void> verify_storage() const;

  // ----- explanations (no mutation) ---------------------------------------

  Explanation explain_resolve(const LocationPath& path,
                              ResolutionMode mode = ResolutionMode::CurrentOnly) const;
  Explanation explain_create(const CreateLocationRequest& request) const;
  Explanation explain_move(const MoveLocationRequest& request) const;

  // ----- mutations (writer sessions only) ----------------------------------

  Result<MutationReceipt> create_location(const CreateLocationRequest& request);
  Result<MutationReceipt> readdress(const ReaddressRequest& request);
  Result<MutationReceipt> relabel(const RelabelRequest& request);
  Result<MutationReceipt> move_location(const MoveLocationRequest& request);
  Result<MutationReceipt> retire_location(const RetireLocationRequest& request);
  Result<MutationReceipt> reactivate_location(const ReactivateLocationRequest& request);
  Result<MutationReceipt> replace_location(const ReplaceLocationRequest& request);
  Result<MutationReceipt> add_alias(const AddAliasRequest& request);
  Result<MutationReceipt> remove_alias(const RemoveAliasRequest& request);
  Result<MutationReceipt> set_rack_geometry(const SetRackGeometryRequest& request);

  /// Stops accepting new work, waits for in-flight mutations to finish, and
  /// releases the writer lock. Mutations that already crossed the publication
  /// point report success; nothing is left half-applied.
  Result<void> close();

 private:
  Registry() = default;

  /// In-memory, bounded revision journal enabling diff() between recent
  /// revisions. Defined in the implementation; never part of the ABI surface.
  struct Journal;

  /// Wraps an already-open store in a session and loads its committed state.
  static Result<std::shared_ptr<Registry>> adopt(std::shared_ptr<Store> store,
                                                 const std::filesystem::path& directory);

  /// Appends one revision to the bounded in-memory journal. Called only after a
  /// mutation has been durably published.
  void record_journal(LocationRevision revision,
                      const MutationContext& context,
                      std::vector<LocationChange> changes,
                      std::uint32_t created,
                      std::uint32_t changed);

  std::shared_ptr<Store> store_;
  std::filesystem::path directory_;
  std::unique_ptr<Snapshot> model_;  ///< committed state; guarded by mutex_
  std::unique_ptr<Journal> journal_;
  mutable std::shared_mutex mutex_;
  bool closed_ = false;
};

}  // namespace dccp::physical_location_registry

#endif  // DCCP_PHYSICAL_LOCATION_REGISTRY_REGISTRY_HPP
