// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#ifndef DCCP_PHYSICAL_LOCATION_REGISTRY_KIND_HPP
#define DCCP_PHYSICAL_LOCATION_REGISTRY_KIND_HPP

#include <cstdint>
#include <string_view>
#include <vector>

#include "dccp/physical_location_registry/export.hpp"
#include "dccp/physical_location_registry/result.hpp"

namespace dccp::physical_location_registry {

/// The kinds of place this registry addresses.
///
/// The enumeration order is the canonical order used by every deterministic
/// listing; the numeric values are part of the canonical serialization and are
/// never renumbered.
enum class LocationKind : std::uint8_t {
  Facility = 0,  ///< A site. The only kind that may exist without a parent.
  Building = 1,
  Hall = 2,
  Room = 3,
  Row = 4,
  Rack = 5,
  Cage = 6,
  RackUnit = 7,
  Zone = 8,  ///< A containment node used to address a group of rows/racks/cages.
};

/// Number of distinct LocationKind values.
inline constexpr std::size_t kLocationKindCount = 9;

/// Canonical lower-case name ("facility", "rack-unit", ...).
PLR_API std::string_view location_kind_name(LocationKind kind) noexcept;

/// Parses a canonical kind name (case-insensitive ASCII, '-' and '_' accepted).
PLR_API Result<LocationKind> parse_location_kind(std::string_view raw);

/// True for LocationKind::Facility, the only kind allowed at the top.
PLR_API bool kind_may_be_root(LocationKind kind) noexcept;

/// True for every kind that must have a parent.
PLR_API bool kind_requires_parent(LocationKind kind) noexcept;

/// True when "child" may be created directly under "parent".
PLR_API bool is_legal_child_kind(LocationKind parent, LocationKind child) noexcept;

/// A zone is a hierarchical containment node in this registry. Overlapping
/// membership sets are a different system boundary and are not modelled here.
PLR_API bool kind_is_zone(LocationKind kind) noexcept;

/// True for kinds that may carry a rack-unit coordinate.
PLR_API bool kind_may_carry_unit_coordinate(LocationKind kind) noexcept;

/// True for kinds that may carry a rack envelope (the U extent the rack offers).
PLR_API bool kind_may_carry_envelope(LocationKind kind) noexcept;

/// Legal child kinds of "parent", in canonical enumeration order.
PLR_API std::vector<LocationKind> allowed_child_kinds(LocationKind parent);

/// All kinds in canonical enumeration order.
PLR_API std::vector<LocationKind> all_location_kinds();

}  // namespace dccp::physical_location_registry

#endif  // DCCP_PHYSICAL_LOCATION_REGISTRY_KIND_HPP
