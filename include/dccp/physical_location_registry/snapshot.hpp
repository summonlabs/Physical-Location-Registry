// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#ifndef DCCP_PHYSICAL_LOCATION_REGISTRY_SNAPSHOT_HPP
#define DCCP_PHYSICAL_LOCATION_REGISTRY_SNAPSHOT_HPP

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "dccp/physical_location_registry/address.hpp"
#include "dccp/physical_location_registry/export.hpp"
#include "dccp/physical_location_registry/limits.hpp"
#include "dccp/physical_location_registry/location.hpp"
#include "dccp/physical_location_registry/provenance.hpp"
#include "dccp/physical_location_registry/requests.hpp"
#include "dccp/physical_location_registry/result.hpp"
#include "dccp/physical_location_registry/strong_id.hpp"

namespace dccp::physical_location_registry {

/// Canonical snapshot format version understood by this build.
inline constexpr std::uint16_t kSnapshotFormatVersion = 1;

/// Restrict and bound a listing.
struct PLR_API ListOptions {
  std::optional<LocationKind> kind;
  std::optional<LifecycleState> lifecycle;

  /// Restrict the listing to this location and its descendants.
  std::optional<LocationId> root;

  /// Maximum number of entries to return. Exceeding it is a LimitExceeded
  /// rejection, never a silent truncation.
  std::uint32_t max_nodes = 1000;
};

/// An immutable, self-contained view of committed location state.
///
/// A snapshot owns everything it answers from: queries never touch a registry,
/// never take a lock and never observe a partially applied mutation. It is
/// produced by Registry::copy_snapshot() or by decoding canonical state bytes,
/// and it is what the inspection tooling diffs, lists and verifies.
class PLR_API Snapshot {
 public:
  /// Opaque storage type of a snapshot. Declared here so that the library's own
  /// translation units can share it; it is incomplete in this header and defined
  /// only inside the library, so a consumer can neither name its contents nor
  /// reach authoritative records except through the queries below.
  struct Impl;

  Snapshot(const Snapshot&) = delete;
  Snapshot& operator=(const Snapshot&) = delete;
  Snapshot(Snapshot&&) noexcept;
  Snapshot& operator=(Snapshot&&) noexcept;
  ~Snapshot();

  const StoreId& store_id() const noexcept;
  LocationRevision revision() const noexcept;
  StateSequence sequence() const noexcept;
  WriterEpoch epoch() const noexcept;
  const Limits& limits() const noexcept;

  std::uint32_t location_count() const noexcept;
  bool empty() const noexcept;

  Result<LocationView> find(const LocationId& id) const;

  /// Resolves a canonical address, or an alias bound to one. A retired
  /// location resolves only when the caller asks for it; a replaced location
  /// never resolves, because its successor is the current answer.
  Result<ResolutionResult> resolve(const LocationPath& path,
                                   ResolutionMode mode = ResolutionMode::CurrentOnly) const;

  /// Current canonical address of a location id.
  Result<LocationPath> path_of(const LocationId& id) const;

  /// Direct children in canonical address order.
  Result<std::vector<ChildEntry>> children(const LocationId& id) const;

  /// Depth-first descendants in canonical order, at most max_depth levels below
  /// the given location, bounded by limits.max_traversal_nodes.
  Result<std::vector<ChildEntry>> descendants(const LocationId& id, std::uint32_t max_depth) const;

  /// Parentless locations in canonical address order.
  Result<std::vector<ChildEntry>> roots() const;

  /// Every alias binding, ordered by alias text.
  Result<std::vector<AliasBinding>> aliases() const;

  /// Locations in canonical address order, filtered and bounded by options.
  Result<std::vector<LocationView>> list(const ListOptions& options) const;

  LocationStatistics statistics() const;

  /// Replacement lineage records, ordered by predecessor id.
  const std::vector<ReplacementRecord>& replacements() const;

  /// Idempotency receipts, oldest first.
  const std::vector<OperationReceipt>& operation_receipts() const;

  /// SHA-256 over the canonical encoding of this snapshot, lowercase hex.
  Result<std::string> canonical_digest() const;

 private:
  friend class Registry;
  friend struct SnapshotAccess;
  explicit Snapshot(std::unique_ptr<Impl> impl);

  std::unique_ptr<Impl> impl_;
};

/// Canonical, deterministic encoding of a snapshot, including its integrity
/// trailer. Equal snapshots encode to equal bytes on every platform.
PLR_API Result<std::string> encode_snapshot(const Snapshot& snapshot);

/// Decodes and fully validates canonical snapshot bytes. Every length, count,
/// enumeration value and identity is checked against the declared limits before
/// anything is allocated, and the integrity digest is verified before the
/// result is returned.
PLR_API Result<Snapshot> decode_snapshot(std::string_view bytes);

}  // namespace dccp::physical_location_registry

#endif  // DCCP_PHYSICAL_LOCATION_REGISTRY_SNAPSHOT_HPP
