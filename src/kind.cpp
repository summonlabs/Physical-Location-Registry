// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "dccp/physical_location_registry/kind.hpp"

#include <cstddef>

#include "dccp/physical_location_registry/text.hpp"

namespace dccp::physical_location_registry {
namespace {

constexpr std::size_t index_of(LocationKind kind) noexcept {
  return static_cast<std::size_t>(static_cast<unsigned>(kind));
}

/// Containment schema. Rows are parents, columns are children, and the order of
/// the enumeration is the canonical order used by allowed_child_kinds().
constexpr bool kContainment[kLocationKindCount][kLocationKindCount] = {
    //        FAC    BLD    HALL   ROOM   ROW    RACK   CAGE   UNIT   ZONE
    /*FAC */ {false, true,  true,  true,  false, false, true,  false, true},
    /*BLD */ {false, false, true,  true,  false, false, true,  false, true},
    /*HALL*/ {false, false, false, true,  true,  false, true,  false, true},
    /*ROOM*/ {false, false, false, false, true,  true,  true,  false, true},
    /*ROW */ {false, false, false, false, false, true,  true,  false, false},
    /*RACK*/ {false, false, false, false, false, false, false, true,  false},
    /*CAGE*/ {false, false, false, false, false, true,  false, false, false},
    /*UNIT*/ {false, false, false, false, false, false, false, false, false},
    /*ZONE*/ {false, false, false, false, true,  true,  true,  false, false},
};

struct KindName {
  LocationKind kind;
  std::string_view name;
};

constexpr KindName kKindNames[] = {
    {LocationKind::Facility, "facility"}, {LocationKind::Building, "building"},
    {LocationKind::Hall, "hall"},         {LocationKind::Room, "room"},
    {LocationKind::Row, "row"},           {LocationKind::Rack, "rack"},
    {LocationKind::Cage, "cage"},         {LocationKind::RackUnit, "rack-unit"},
    {LocationKind::Zone, "zone"},
};

}  // namespace

std::string_view location_kind_name(LocationKind kind) noexcept {
  for (const KindName& entry : kKindNames) {
    if (entry.kind == kind) {
      return entry.name;
    }
  }
  return "unrecognized";
}

Result<LocationKind> parse_location_kind(std::string_view raw) {
  for (const KindName& entry : kKindNames) {
    if (ascii_case_insensitive_equal(raw, entry.name)) {
      return entry.kind;
    }
    // Accept the underscore spelling as well as the canonical hyphen.
    std::string alternate(entry.name);
    for (char& character : alternate) {
      if (character == '-') {
        character = '_';
      }
    }
    if (ascii_case_insensitive_equal(raw, alternate)) {
      return entry.kind;
    }
  }
  return Error(ErrorCode::UnknownEnumToken,
               "unknown location kind; expected one of facility, building, hall, room, row, rack, "
               "cage, rack-unit, zone")
      .with_subject(std::string(raw.substr(0, 64)));
}

bool kind_may_be_root(LocationKind kind) noexcept { return kind == LocationKind::Facility; }

bool kind_requires_parent(LocationKind kind) noexcept { return kind != LocationKind::Facility; }

bool is_legal_child_kind(LocationKind parent, LocationKind child) noexcept {
  const std::size_t parent_index = index_of(parent);
  const std::size_t child_index = index_of(child);
  if (parent_index >= kLocationKindCount || child_index >= kLocationKindCount) {
    return false;
  }
  return kContainment[parent_index][child_index];
}

bool kind_is_zone(LocationKind kind) noexcept { return kind == LocationKind::Zone; }

bool kind_may_carry_unit_coordinate(LocationKind kind) noexcept {
  return kind == LocationKind::RackUnit;
}

bool kind_may_carry_envelope(LocationKind kind) noexcept { return kind == LocationKind::Rack; }

std::vector<LocationKind> allowed_child_kinds(LocationKind parent) {
  std::vector<LocationKind> allowed;
  const std::size_t parent_index = index_of(parent);
  if (parent_index >= kLocationKindCount) {
    return allowed;
  }
  for (std::size_t child_index = 0; child_index < kLocationKindCount; ++child_index) {
    if (kContainment[parent_index][child_index]) {
      allowed.push_back(static_cast<LocationKind>(child_index));
    }
  }
  return allowed;
}

std::vector<LocationKind> all_location_kinds() {
  std::vector<LocationKind> kinds;
  kinds.reserve(kLocationKindCount);
  for (std::size_t index = 0; index < kLocationKindCount; ++index) {
    kinds.push_back(static_cast<LocationKind>(index));
  }
  return kinds;
}

}  // namespace dccp::physical_location_registry
