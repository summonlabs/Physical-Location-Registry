// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#ifndef DCCP_PHYSICAL_LOCATION_REGISTRY_RACK_UNIT_HPP
#define DCCP_PHYSICAL_LOCATION_REGISTRY_RACK_UNIT_HPP

#include <cstdint>
#include <string>
#include <string_view>

#include "dccp/physical_location_registry/export.hpp"
#include "dccp/physical_location_registry/result.hpp"

namespace dccp::physical_location_registry {

/// Hard cap on a rack-unit coordinate and on a rack envelope height.
///
/// A rack taller than this is not a rack; the cap keeps every coordinate in a
/// range where arithmetic on it cannot overflow and bounds the coordinate space
/// that untrusted durable state may describe.
inline constexpr std::uint32_t kMaxRackUnitCoordinate = 512;

/// A validated rack-unit coordinate: 1-based, at most kMaxRackUnitCoordinate.
class PLR_API RackUnitCoordinate {
 public:
  RackUnitCoordinate() = delete;

  static Result<RackUnitCoordinate> parse(std::uint64_t unit);

  /// Accepts "12", "U12" or "12U"; used by the inspection tooling.
  static Result<RackUnitCoordinate> parse_text(std::string_view raw);

  std::uint32_t value() const noexcept { return value_; }

  /// Canonical form: "U12".
  std::string to_string() const;

  friend constexpr bool operator==(const RackUnitCoordinate&, const RackUnitCoordinate&) noexcept = default;
  friend constexpr std::strong_ordering operator<=>(const RackUnitCoordinate& lhs,
                                                    const RackUnitCoordinate& rhs) noexcept = default;

 private:
  friend class RackEnvelope;

  explicit constexpr RackUnitCoordinate(std::uint32_t value) noexcept : value_(value) {}

  std::uint32_t value_;
};

/// The U extent a rack offers: a first coordinate and a height in units.
///
/// This describes the rack as a place, not what is mounted in it. Mounting,
/// occupancy and reservations belong to other systems and are not modelled.
class PLR_API RackEnvelope {
 public:
  RackEnvelope() = delete;

  /// First coordinate plus height; the resulting last coordinate must not
  /// exceed kMaxRackUnitCoordinate.
  static Result<RackEnvelope> make(RackUnitCoordinate first, std::uint32_t height);

  /// Convenience for the common case of a rack starting at U1.
  static Result<RackEnvelope> with_height(std::uint32_t height);

  /// Accepts "48", "48U", "1-48", "1-48U"; used by the inspection tooling.
  static Result<RackEnvelope> parse_text(std::string_view raw);

  RackUnitCoordinate first() const noexcept { return first_; }
  std::uint32_t height() const noexcept { return height_; }
  RackUnitCoordinate last() const noexcept {
    return RackUnitCoordinate(static_cast<std::uint32_t>(first_.value() + height_ - 1U));
  }

  bool contains(RackUnitCoordinate coordinate) const noexcept {
    return coordinate.value() >= first_.value() && coordinate.value() <= last().value();
  }

  /// Canonical form: "1-48U".
  std::string to_string() const;

  friend constexpr bool operator==(const RackEnvelope&, const RackEnvelope&) noexcept = default;

 private:
  constexpr RackEnvelope(RackUnitCoordinate first, std::uint32_t height) noexcept
      : first_(first), height_(height) {}

  RackUnitCoordinate first_;
  std::uint32_t height_;
};

}  // namespace dccp::physical_location_registry

namespace std {
template <>
struct hash<dccp::physical_location_registry::RackUnitCoordinate> {
  std::size_t operator()(const dccp::physical_location_registry::RackUnitCoordinate& value) const noexcept {
    return std::hash<std::uint32_t>{}(value.value());
  }
};
}  // namespace std

#endif  // DCCP_PHYSICAL_LOCATION_REGISTRY_RACK_UNIT_HPP
