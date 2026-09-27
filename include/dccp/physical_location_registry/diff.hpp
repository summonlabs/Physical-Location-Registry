// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#ifndef DCCP_PHYSICAL_LOCATION_REGISTRY_DIFF_HPP
#define DCCP_PHYSICAL_LOCATION_REGISTRY_DIFF_HPP

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "dccp/physical_location_registry/address.hpp"
#include "dccp/physical_location_registry/export.hpp"
#include "dccp/physical_location_registry/lifecycle.hpp"
#include "dccp/physical_location_registry/location.hpp"
#include "dccp/physical_location_registry/provenance.hpp"
#include "dccp/physical_location_registry/rack_unit.hpp"
#include "dccp/physical_location_registry/result.hpp"
#include "dccp/physical_location_registry/snapshot.hpp"
#include "dccp/physical_location_registry/strong_id.hpp"

namespace dccp::physical_location_registry {

/// Aspect bits describing what changed about one location.
///
/// Locations are never deleted by a mutation: a location leaves currency by
/// being Retired or Replaced, and both are recorded as lifecycle changes. The
/// removed and kind-changed aspects can therefore only appear when two unrelated
/// snapshots are compared, and they are labelled as such rather than hidden.
using ChangeFlags = std::uint16_t;

inline constexpr ChangeFlags kChangeCreated = 0x0001;
inline constexpr ChangeFlags kChangeReparented = 0x0002;
inline constexpr ChangeFlags kChangeReaddressed = 0x0004;
inline constexpr ChangeFlags kChangeRelabeled = 0x0008;
inline constexpr ChangeFlags kChangeLifecycle = 0x0010;
inline constexpr ChangeFlags kChangeRackGeometry = 0x0020;
inline constexpr ChangeFlags kChangeAliasAdded = 0x0040;
inline constexpr ChangeFlags kChangeAliasRemoved = 0x0080;
inline constexpr ChangeFlags kChangeReplacementLineage = 0x0100;
inline constexpr ChangeFlags kChangeRemoved = 0x0200;
inline constexpr ChangeFlags kChangeKindChanged = 0x0400;

/// Bound on the per-location detail recorded for one revision in the in-memory
/// journal. Counters stay exact when the bound truncates detail.
inline constexpr std::uint32_t kMaxJournalChangesPerEntry = 4096;

/// Stable textual name of one aspect bit ("created", "reparented", ...).
PLR_API std::string_view change_flag_name(ChangeFlags flag) noexcept;

/// Comma-joined aspect names in canonical bit order.
PLR_API std::string format_change_flags(ChangeFlags flags);

/// What changed about one location between two revisions.
struct PLR_API LocationChange {
  LocationId id;
  LocationKind kind = LocationKind::Facility;
  ChangeFlags flags = 0;

  std::optional<LocationPath> path_before;
  std::optional<LocationPath> path_after;
  std::optional<LocationId> parent_before;
  std::optional<LocationId> parent_after;
  LocationGeneration generation_before;
  LocationGeneration generation_after;
  std::optional<std::string> label_before;
  std::optional<std::string> label_after;
  std::optional<LifecycleState> lifecycle_before;
  std::optional<LifecycleState> lifecycle_after;
  std::optional<RackUnitCoordinate> unit_before;
  std::optional<RackUnitCoordinate> unit_after;
  std::optional<RackEnvelope> envelope_before;
  std::optional<RackEnvelope> envelope_after;
  std::vector<std::string> aliases_added;
  std::vector<std::string> aliases_removed;

  bool has(ChangeFlags flag) const noexcept { return (flags & flag) != 0; }

  /// One-line canonical rendering.
  std::string to_string() const;
};

/// A generation diff between two committed revisions.
struct PLR_API RevisionDiff {
  LocationRevision from_revision;
  LocationRevision to_revision;

  /// Sorted by LocationId byte order, so a diff renders identically everywhere.
  std::vector<LocationChange> changes;

  /// Exact counters, even when per-location detail was truncated.
  std::uint32_t created = 0;
  std::uint32_t changed = 0;
  std::uint32_t removed = 0;

  /// True when the diff had to stop recording individual locations for a
  /// revision whose change set exceeded the configured journal bound. The
  /// counters remain exact; only the per-location detail is incomplete.
  bool truncated = false;

  bool empty() const noexcept { return changes.empty(); }

  std::string to_string() const;
};

/// Full comparison of two snapshots; works for any pair of decoded state files.
PLR_API RevisionDiff diff_snapshots(const Snapshot& from, const Snapshot& to);

/// Multi-line deterministic rendering of a diff.
PLR_API std::string format_revision_diff(const RevisionDiff& diff);

}  // namespace dccp::physical_location_registry

#endif  // DCCP_PHYSICAL_LOCATION_REGISTRY_DIFF_HPP
