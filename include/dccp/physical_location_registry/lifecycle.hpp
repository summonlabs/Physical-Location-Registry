// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#ifndef DCCP_PHYSICAL_LOCATION_REGISTRY_LIFECYCLE_HPP
#define DCCP_PHYSICAL_LOCATION_REGISTRY_LIFECYCLE_HPP

#include <cstdint>
#include <string_view>
#include <vector>

#include "dccp/physical_location_registry/export.hpp"
#include "dccp/physical_location_registry/result.hpp"

namespace dccp::physical_location_registry {

/// Lifecycle state of a location.
///
/// Active    - the location exists now; it is the current answer when its
///             address or an alias resolves.
/// Retired   - the location no longer describes a current place. It is kept for
///             provenance and may be reactivated; it never resolves as current
///             unless the caller explicitly asks for retired locations.
/// Replaced  - terminal. A successor location exists and the predecessor is
///             kept only as replacement lineage; it can never be mutated again.
enum class LifecycleState : std::uint8_t {
  Active = 0,
  Retired = 1,
  Replaced = 2,
};

/// The mutation verbs that change lifecycle state.
enum class LifecycleTransition : std::uint8_t {
  Retire = 0,      ///< Active -> Retired
  Reactivate = 1,  ///< Retired -> Active
  Replace = 2,     ///< Active -> Replaced, Retired -> Replaced
};

/// Canonical lower-case name ("active", "retired", "replaced").
PLR_API std::string_view lifecycle_state_name(LifecycleState state) noexcept;

/// Parses a canonical lifecycle name (ASCII case-insensitive).
PLR_API Result<LifecycleState> parse_lifecycle_state(std::string_view raw);

/// Canonical lower-case name of a transition verb.
PLR_API std::string_view lifecycle_transition_name(LifecycleTransition transition) noexcept;

/// True when the transition is legal from the given state.
///
/// This table is the single authority for lifecycle legality and is what the
/// mutation path consults; there is no second, informal rule anywhere else.
PLR_API bool is_legal_lifecycle_transition(LifecycleState from, LifecycleTransition transition) noexcept;

/// Legal transitions from a state, in canonical enumeration order.
PLR_API std::vector<LifecycleTransition> legal_lifecycle_transitions(LifecycleState from);

/// True for Active: the single state that answers resolution as current.
PLR_API bool lifecycle_state_is_current(LifecycleState state) noexcept;

/// True for Replaced: the terminal state that accepts no further mutation.
PLR_API bool lifecycle_state_is_terminal(LifecycleState state) noexcept;

}  // namespace dccp::physical_location_registry

#endif  // DCCP_PHYSICAL_LOCATION_REGISTRY_LIFECYCLE_HPP
