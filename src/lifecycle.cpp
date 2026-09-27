// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "dccp/physical_location_registry/lifecycle.hpp"

#include <string>

#include "dccp/physical_location_registry/text.hpp"

namespace dccp::physical_location_registry {
namespace {

struct StateName {
  LifecycleState state;
  std::string_view name;
};

constexpr StateName kStateNames[] = {
    {LifecycleState::Active, "active"},
    {LifecycleState::Retired, "retired"},
    {LifecycleState::Replaced, "replaced"},
};

/// The single lifecycle legality table.
///
/// Active   - Retire, Replace
/// Retired  - Reactivate, Replace
/// Replaced - terminal, nothing
constexpr bool kLegalTransitions[3][3] = {
    //        Retire Reactivate Replace
    /*Active  */ {true, false, true},
    /*Retired */ {false, true, true},
    /*Replaced*/ {false, false, false},
};

constexpr std::size_t state_index(LifecycleState state) noexcept {
  return static_cast<std::size_t>(static_cast<unsigned>(state));
}

constexpr std::size_t transition_index(LifecycleTransition transition) noexcept {
  return static_cast<std::size_t>(static_cast<unsigned>(transition));
}

}  // namespace

std::string_view lifecycle_state_name(LifecycleState state) noexcept {
  for (const StateName& entry : kStateNames) {
    if (entry.state == state) {
      return entry.name;
    }
  }
  return "unrecognized";
}

Result<LifecycleState> parse_lifecycle_state(std::string_view raw) {
  for (const StateName& entry : kStateNames) {
    if (ascii_case_insensitive_equal(raw, entry.name)) {
      return entry.state;
    }
  }
  return Error(ErrorCode::UnknownEnumToken, "unknown lifecycle state; expected active, retired or replaced")
      .with_subject(std::string(raw.substr(0, 64)));
}

std::string_view lifecycle_transition_name(LifecycleTransition transition) noexcept {
  switch (transition) {
    case LifecycleTransition::Retire:
      return "retire";
    case LifecycleTransition::Reactivate:
      return "reactivate";
    case LifecycleTransition::Replace:
      return "replace";
  }
  return "unrecognized";
}

bool is_legal_lifecycle_transition(LifecycleState from, LifecycleTransition transition) noexcept {
  const std::size_t from_index = state_index(from);
  const std::size_t verb_index = transition_index(transition);
  if (from_index >= 3 || verb_index >= 3) {
    return false;
  }
  return kLegalTransitions[from_index][verb_index];
}

std::vector<LifecycleTransition> legal_lifecycle_transitions(LifecycleState from) {
  std::vector<LifecycleTransition> transitions;
  for (std::size_t index = 0; index < 3; ++index) {
    if (is_legal_lifecycle_transition(from, static_cast<LifecycleTransition>(index))) {
      transitions.push_back(static_cast<LifecycleTransition>(index));
    }
  }
  return transitions;
}

bool lifecycle_state_is_current(LifecycleState state) noexcept { return state == LifecycleState::Active; }

bool lifecycle_state_is_terminal(LifecycleState state) noexcept {
  return state == LifecycleState::Replaced;
}

}  // namespace dccp::physical_location_registry
