// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Internal registry plumbing: the bounded revision journal and the helpers the
// mutation engine shares. Not installed and not part of the public API.

#ifndef DCCP_PHYSICAL_LOCATION_REGISTRY_SRC_REGISTRY_INTERNAL_HPP
#define DCCP_PHYSICAL_LOCATION_REGISTRY_SRC_REGISTRY_INTERNAL_HPP

#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <vector>

#include "dccp/physical_location_registry/diff.hpp"
#include "dccp/physical_location_registry/registry.hpp"
#include "byte_codec.hpp"
#include "model.hpp"

namespace dccp::physical_location_registry {

/// In-memory, bounded record of what each committed revision changed.
///
/// The journal is what makes diff() between recent revisions possible without
/// retaining whole past states. It is deliberately not persisted: durable
/// comparison of arbitrary revisions is done by comparing published state files
/// with decode_snapshot() and diff_snapshots(). Its size is bounded by
/// Limits::max_retained_revisions.
struct Registry::Journal {
  struct Entry {
    LocationRevision revision;
    Timestamp at;
    ActorId actor;
    std::string reason;
    bool truncated = false;

    /// Exact counts of locations created and changed by this revision, kept even
    /// when the per-location detail below was truncated.
    std::uint32_t created = 0;
    std::uint32_t changed = 0;

    /// Per-location detail, sorted by LocationId; at most
    /// kMaxJournalChangesPerEntry entries.
    std::vector<LocationChange> changes;
  };

  std::deque<Entry> entries;

  /// Appends an entry, dropping the oldest so that at most max_entries remain.
  void append(Entry entry, std::uint32_t max_entries);

  /// Merges the entries in (from, to] into one diff.
  Result<RevisionDiff> diff(LocationRevision from, LocationRevision to) const;

  /// Revisions currently diffable.
  std::vector<LocationRevision> retained_revisions() const;

  bool empty() const noexcept { return entries.empty(); }
  void clear() { entries.clear(); }
};

namespace internal {

/// Validates the context fields every mutation requires (actor, reason,
/// timestamp). Authority and revision preconditions are checked separately so
/// that an idempotent replay is not blocked by a precondition that the original
/// request already satisfied.
Result<void> validate_context(const MutationContext& context, const Limits& limits);

/// Verifies caller-supplied writer authority against the store's own identity
/// and current epoch.
Result<void> validate_authority(const MutationContext& context,
                                const StoreId& store_id,
                                WriterEpoch epoch);

/// Builds a LocationChange describing a lifecycle change of one record.
LocationChange lifecycle_change(const LocationRecord& record,
                                const LocationPath& path,
                                LifecycleState before,
                                LifecycleState after);

}  // namespace internal
}  // namespace dccp::physical_location_registry

#endif  // DCCP_PHYSICAL_LOCATION_REGISTRY_SRC_REGISTRY_INTERNAL_HPP
