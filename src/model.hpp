// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Internal authoritative model. Not installed, not part of the public API: the
// public surface exposes immutable views and mutation commands, never raw
// records.

#ifndef DCCP_PHYSICAL_LOCATION_REGISTRY_SRC_MODEL_HPP
#define DCCP_PHYSICAL_LOCATION_REGISTRY_SRC_MODEL_HPP

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "dccp/physical_location_registry/address.hpp"
#include "dccp/physical_location_registry/diff.hpp"
#include "dccp/physical_location_registry/kind.hpp"
#include "dccp/physical_location_registry/lifecycle.hpp"
#include "dccp/physical_location_registry/limits.hpp"
#include "dccp/physical_location_registry/location.hpp"
#include "dccp/physical_location_registry/provenance.hpp"
#include "dccp/physical_location_registry/rack_unit.hpp"
#include "dccp/physical_location_registry/requests.hpp"
#include "dccp/physical_location_registry/result.hpp"
#include "dccp/physical_location_registry/snapshot.hpp"
#include "dccp/physical_location_registry/strong_id.hpp"

namespace dccp::physical_location_registry {

/// The authoritative record of one location.
///
/// Address components are stored per node and paths are derived by walking
/// parents. That is what makes a subtree move a single write to the moved node
/// plus generation bumps for the descendants that followed it, with no stored
/// path anywhere that could drift out of agreement with the hierarchy.
struct LocationRecord {
  LocationRecord(LocationId record_id, AddressComponent record_component)
      : id(std::move(record_id)), component(std::move(record_component)) {}

  LocationId id;
  LocationKind kind = LocationKind::Facility;
  std::optional<LocationId> parent;
  AddressComponent component;
  std::string label;
  LifecycleState lifecycle = LifecycleState::Active;
  std::optional<RackUnitCoordinate> unit;
  std::optional<RackEnvelope> envelope;
  LocationGeneration generation;
  std::optional<LocationId> replaces;
  std::optional<LocationId> replaced_by;
  ProvenanceRecord provenance;
  std::vector<MoveRecord> moves;
  std::vector<std::string> aliases;
};

/// The children of one parent, indexed for exact and look-alike lookup.
struct ChildSet {
  /// Exact component -> child, ordered byte-wise; this order is the canonical
  /// child listing order.
  std::map<std::string, LocationId, std::less<>> by_component;

  /// ASCII-folded component -> child, used only to reject look-alike siblings.
  std::map<std::string, LocationId, std::less<>> by_folded_component;

  bool empty() const noexcept { return by_component.empty(); }
  std::size_t size() const noexcept { return by_component.size(); }
};

/// Authoritative state of one committed generation, and the storage behind the
/// public Snapshot value.
struct Snapshot::Impl {
  StoreId store_id;
  LocationRevision revision;
  StateSequence sequence;
  WriterEpoch epoch;
  Limits limits;

  std::map<LocationId, LocationRecord, std::less<>> records;
  std::map<LocationId, ChildSet, std::less<>> children;

  /// Parentless locations, indexed the same way as any other child set.
  ChildSet roots;

  /// Alias text -> location, ordered byte-wise.
  std::map<std::string, LocationId, std::less<>> alias_index;

  /// ASCII-folded alias text -> canonical alias text.
  std::map<std::string, std::string, std::less<>> alias_folded_index;

  /// Replacement lineage, ordered by (predecessor, successor).
  std::vector<ReplacementRecord> replacements;

  /// Idempotency receipts, oldest first; bounded by limits.max_operation_receipts.
  std::vector<OperationReceipt> receipts;

  /// Receipt lookup by operation identity, mirroring the ring above.
  std::map<OperationId, OperationReceipt, std::less<>> receipt_index;

  /// Running totals, maintained by the mutation path and recomputed by
  /// rebuild_indexes(), so that the publication gate is O(1).
  std::uint64_t total_moves = 0;
  std::uint32_t total_aliases = 0;

  void rebuild_indexes();

  const LocationRecord* find(const LocationId& id) const;
  LocationRecord* find(const LocationId& id);

  const ChildSet* child_set(const std::optional<LocationId>& parent) const;

  /// Exact child lookup by component text.
  std::optional<LocationId> child_by_component(const LocationId& parent,
                                               std::string_view component) const;

  /// Look-alike child lookup by ASCII-folded component text.
  std::optional<LocationId> child_by_folded_component(const LocationId& parent,
                                                      std::string_view folded) const;

  /// Canonical path of a record by walking parents. Bounded by limits.max_depth;
  /// returns TraversalDepthExceeded when a cycle or an over-deep chain is found.
  Result<LocationPath> path_of(const LocationId& id) const;
  Result<LocationPath> path_of(const LocationRecord& record) const;

  /// Resolves a canonical path through the hierarchy, ignoring aliases.
  Result<LocationId> resolve_canonical(const LocationPath& path) const;

  /// Depth-first subtree ids in canonical order, including the root id.
  Result<std::vector<LocationId>> subtree_ids(const LocationId& root_id,
                                              std::uint32_t max_depth) const;

  std::uint32_t move_record_count() const;
  std::uint32_t alias_count() const;
  std::uint32_t max_depth_observed() const;

  /// Cheap publication gate: every top-level bound checked in constant time
  /// from the running counters. Run before every durable publication.
  Result<void> validate_state_bounds() const;

  /// Full structural validation: identity, containment schema, acyclicity,
  /// lifecycle/lineage consistency, alias uniqueness, index agreement and every
  /// configured bound. Called after each decode and by explicit verification, so
  /// an invalid state can neither be published nor loaded as authoritative.
  Result<void> validate_shape() const;

  /// Bytes of stored text, used to bound growth.
  std::uint64_t text_bytes() const;
};

/// Access to the private implementation of a public Snapshot value.
///
/// This is how the codec, the store and the registry reach the authoritative
/// records without exposing them in an installed header.
struct SnapshotAccess {
  static Snapshot::Impl& get(Snapshot& snapshot) noexcept;
  static const Snapshot::Impl& get(const Snapshot& snapshot) noexcept;
  static Snapshot make(std::unique_ptr<Snapshot::Impl> impl);
  static Snapshot clone(const Snapshot& snapshot);
};

namespace internal {

/// Builds a LocationView (public value) from a stored record plus its path.
LocationView make_view(const LocationRecord& record, const LocationPath& path);

/// Builds a ChildEntry from a stored record plus its path.
ChildEntry make_child_entry(const LocationRecord& record, const LocationPath& path);

/// Parses stored path text, mapping syntax failures onto a caller-chosen code.
Result<LocationPath> parse_path_text(std::string_view text,
                                     const Limits& limits,
                                     ErrorCode failure_code);

/// Canonical encoding of an authoritative model, including its integrity
/// trailer. Equal models encode to identical bytes.
Result<std::string> encode_model(const Snapshot::Impl& model);

/// Decodes canonical bytes into a validated snapshot. Every length, count,
/// enumeration and identity is validated against the payload's own limits
/// before allocation, and the integrity digest is checked first.
Result<Snapshot> decode_model_bytes(std::string_view bytes);

/// Number of locations in the subtree below root_id, excluding root_id itself.
Result<std::uint32_t> count_descendants(const Snapshot::Impl& model,
                                        const LocationId& root_id,
                                        std::uint32_t max_depth);

/// Validates a text payload against a limit and a syntax predicate, producing
/// the documented error code on rejection.
Result<void> validate_text_field(std::string_view text,
                                 std::uint32_t limit,
                                 bool allow_empty,
                                 ErrorCode too_long_code,
                                 ErrorCode malformed_code,
                                 const char* field_name,
                                 std::string_view syntax_help);

/// Adds or removes one alias binding in the model indexes.
void index_alias(Snapshot::Impl& model, std::string_view alias, const LocationId& id);
void unindex_alias(Snapshot::Impl& model, std::string_view alias);

}  // namespace internal
}  // namespace dccp::physical_location_registry

#endif  // DCCP_PHYSICAL_LOCATION_REGISTRY_SRC_MODEL_HPP
