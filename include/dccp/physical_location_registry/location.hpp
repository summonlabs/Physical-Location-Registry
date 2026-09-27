// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#ifndef DCCP_PHYSICAL_LOCATION_REGISTRY_LOCATION_HPP
#define DCCP_PHYSICAL_LOCATION_REGISTRY_LOCATION_HPP

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "dccp/physical_location_registry/address.hpp"
#include "dccp/physical_location_registry/export.hpp"
#include "dccp/physical_location_registry/kind.hpp"
#include "dccp/physical_location_registry/lifecycle.hpp"
#include "dccp/physical_location_registry/provenance.hpp"
#include "dccp/physical_location_registry/rack_unit.hpp"
#include "dccp/physical_location_registry/result.hpp"
#include "dccp/physical_location_registry/strong_id.hpp"

namespace dccp::physical_location_registry {

/// A detached, immutable view of one location at one generation.
///
/// A view is a value: reading it never touches registry state and never holds a
/// lock. It is produced by the registry (or by a decoded snapshot) and is safe
/// to keep, but it is a point-in-time answer - a later mutation makes the
/// generation stale, which is exactly how a caller detects that its cached
/// address may have changed.
class PLR_API LocationView {
 public:
  LocationView(LocationId id,
               LocationKind kind,
               std::optional<LocationId> parent,
               AddressComponent component,
               LocationPath path,
               std::string label,
               LifecycleState lifecycle,
               std::optional<RackUnitCoordinate> unit,
               std::optional<RackEnvelope> envelope,
               LocationGeneration generation,
               std::optional<LocationId> replaces,
               std::optional<LocationId> replaced_by,
               ProvenanceRecord provenance,
               std::vector<MoveRecord> moves,
               std::vector<std::string> aliases);

  const LocationId& id() const noexcept { return id_; }
  LocationKind kind() const noexcept { return kind_; }
  const std::optional<LocationId>& parent() const noexcept { return parent_; }
  const AddressComponent& component() const noexcept { return component_; }

  /// Canonical absolute address of this location at this generation.
  const LocationPath& path() const noexcept { return path_; }

  const std::string& label() const noexcept { return label_; }
  LifecycleState lifecycle() const noexcept { return lifecycle_; }
  const std::optional<RackUnitCoordinate>& unit() const noexcept { return unit_; }
  const std::optional<RackEnvelope>& envelope() const noexcept { return envelope_; }
  LocationGeneration generation() const noexcept { return generation_; }

  /// Predecessor location this one replaced, if any.
  const std::optional<LocationId>& replaces() const noexcept { return replaces_; }

  /// Successor location that replaced this one, if any.
  const std::optional<LocationId>& replaced_by() const noexcept { return replaced_by_; }

  const ProvenanceRecord& provenance() const noexcept { return provenance_; }

  /// Address changes, oldest first.
  const std::vector<MoveRecord>& moves() const noexcept { return moves_; }

  /// Alias addresses bound to this location, sorted byte-wise.
  const std::vector<std::string>& aliases() const noexcept { return aliases_; }

  bool is_current() const noexcept { return lifecycle_state_is_current(lifecycle_); }

  /// One-line canonical rendering used by the inspection tooling.
  std::string summary() const;

 private:
  LocationId id_;
  LocationKind kind_;
  std::optional<LocationId> parent_;
  AddressComponent component_;
  LocationPath path_;
  std::string label_;
  LifecycleState lifecycle_;
  std::optional<RackUnitCoordinate> unit_;
  std::optional<RackEnvelope> envelope_;
  LocationGeneration generation_;
  std::optional<LocationId> replaces_;
  std::optional<LocationId> replaced_by_;
  ProvenanceRecord provenance_;
  std::vector<MoveRecord> moves_;
  std::vector<std::string> aliases_;
};

/// One entry of a deterministic child listing.
struct PLR_API ChildEntry {
  LocationId id;
  LocationKind kind = LocationKind::Facility;
  std::string component;
  std::string label;
  LifecycleState lifecycle = LifecycleState::Active;
  LocationGeneration generation;
  std::string path;  ///< canonical absolute path of the child
};

/// Compact multi-line rendering of one child entry.
PLR_API std::string format_child_entry(const ChildEntry& entry);

/// How a path resolved to a location.
enum class ResolutionKind : std::uint8_t {
  CanonicalAddress = 0,  ///< the path is the location's current address
  Alias = 1,             ///< the path is an alias bound to the location
};

/// Canonical lower-case name ("canonical-address", "alias").
PLR_API std::string_view resolution_kind_name(ResolutionKind kind) noexcept;

/// What a path resolution found.
struct PLR_API ResolutionResult {
  LocationId id;
  ResolutionKind kind = ResolutionKind::CanonicalAddress;
  LocationPath canonical_path;  ///< the address the id currently resolves to
  LifecycleState lifecycle = LifecycleState::Active;
  LocationGeneration generation;
};

/// Which lifecycle states a resolution may return.
enum class ResolutionMode : std::uint8_t {
  /// Only Active locations. A retired location never resolves silently.
  CurrentOnly = 0,
  /// Active and Retired locations; terminal Replaced locations are never
  /// returned, because their successor is the current answer.
  IncludeRetired = 1,
};

/// An alias address and the location it is bound to.
struct PLR_API AliasBinding {
  std::string alias;  ///< canonical absolute path text
  LocationId id;
  LocationPath canonical_path;
  LifecycleState lifecycle = LifecycleState::Active;
  LocationGeneration generation;
};

/// Aggregate counts of a snapshot or a live registry.
struct PLR_API LocationStatistics {
  std::uint32_t locations = 0;
  std::uint32_t active = 0;
  std::uint32_t retired = 0;
  std::uint32_t replaced = 0;
  std::uint32_t roots = 0;
  std::uint32_t aliases = 0;
  std::uint32_t moves = 0;
  std::uint32_t replacements = 0;
  std::uint32_t max_depth_observed = 0;
  std::uint32_t operation_receipts = 0;
  std::uint32_t by_kind[kLocationKindCount] = {};
};

/// Multi-line deterministic rendering of statistics.
PLR_API std::string format_statistics(const LocationStatistics& statistics);

}  // namespace dccp::physical_location_registry

#endif  // DCCP_PHYSICAL_LOCATION_REGISTRY_LOCATION_HPP
