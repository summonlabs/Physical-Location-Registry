// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#ifndef DCCP_PHYSICAL_LOCATION_REGISTRY_ADDRESS_HPP
#define DCCP_PHYSICAL_LOCATION_REGISTRY_ADDRESS_HPP

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "dccp/physical_location_registry/export.hpp"
#include "dccp/physical_location_registry/limits.hpp"
#include "dccp/physical_location_registry/result.hpp"

namespace dccp::physical_location_registry {

/// One validated component of a canonical location address.
///
/// Not default constructible: an AddressComponent always holds a validated
/// token, so "no component" is expressed with std::optional rather than an
/// empty sentinel.
class PLR_API AddressComponent {
 public:
  AddressComponent() = delete;

  static Result<AddressComponent> parse(std::string_view raw);

  std::string_view value() const noexcept { return value_; }
  const std::string& str() const noexcept { return value_; }

  friend bool operator==(const AddressComponent& lhs, const AddressComponent& rhs) noexcept {
    return lhs.value_ == rhs.value_;
  }
  friend std::strong_ordering operator<=>(const AddressComponent& lhs,
                                          const AddressComponent& rhs) noexcept {
    const int cmp = lhs.value_.compare(rhs.value_);
    return cmp < 0 ? std::strong_ordering::less
                   : (cmp > 0 ? std::strong_ordering::greater : std::strong_ordering::equal);
  }

 private:
  explicit AddressComponent(std::string value) : value_(std::move(value)) {}

  std::string value_;
};

/// A canonical absolute address: the components from the facility root down to
/// one location, e.g. "/FAC1/BLDG-A/ROOM101/ROW-3/RACK-07/U12".
///
/// The empty path (depth 0) denotes "no location" and is what a parentless
/// address resolves to; it is never the address of a stored location, because
/// every stored location has at least a facility component.
///
/// Canonical construction is the only construction: components are validated,
/// joined with a single '/', and a path therefore always prints the same way
/// for the same address.
class PLR_API LocationPath {
 public:
  LocationPath() noexcept = default;

  /// Parses an absolute canonical path. Rejects a missing leading '/', empty
  /// components, "//", trailing '/', invalid component syntax, a depth above
  /// limits.max_depth and a byte length above limits.max_path_bytes.
  static Result<LocationPath> parse(std::string_view raw, const Limits& limits);

  /// Builds a path from already validated components, enforcing depth, byte
  /// length and the non-empty rule.
  static Result<LocationPath> from_components(std::vector<AddressComponent> components, const Limits& limits);

  bool empty() const noexcept { return components_.empty(); }
  std::size_t depth() const noexcept { return components_.size(); }

  /// Component at index; index must be below depth().
  const AddressComponent& component(std::size_t index) const { return components_.at(index); }

  const std::vector<AddressComponent>& components() const noexcept { return components_; }

  /// Parent path, or nullopt when this path is already at the root (depth <= 1).
  std::optional<LocationPath> parent() const;

  /// True when this path is a strict ancestor of "other".
  bool is_ancestor_of(const LocationPath& other) const noexcept;

  /// Appends one component, enforcing the limits.
  Result<LocationPath> child(const AddressComponent& component, const Limits& limits) const;

  /// Canonical textual form: "/" followed by components joined with '/'.
  std::string to_string() const;

  /// Byte length of the canonical textual form.
  std::size_t byte_length() const noexcept;

  friend bool operator==(const LocationPath& lhs, const LocationPath& rhs) noexcept {
    return lhs.components_ == rhs.components_;
  }
  friend std::strong_ordering operator<=>(const LocationPath& lhs, const LocationPath& rhs) noexcept {
    return std::lexicographical_compare_three_way(lhs.components_.begin(), lhs.components_.end(),
                                                  rhs.components_.begin(), rhs.components_.end());
  }

 private:
  std::vector<AddressComponent> components_;
};

}  // namespace dccp::physical_location_registry

namespace std {
template <>
struct hash<dccp::physical_location_registry::AddressComponent> {
  std::size_t operator()(const dccp::physical_location_registry::AddressComponent& value) const noexcept {
    return std::hash<std::string_view>{}(value.value());
  }
};
template <>
struct hash<dccp::physical_location_registry::LocationPath> {
  std::size_t operator()(const dccp::physical_location_registry::LocationPath& value) const noexcept {
    std::size_t seed = 1469598103934665603ULL;
    for (const auto& component : value.components()) {
      seed ^= std::hash<std::string_view>{}(component.value());
      seed *= 1099511628211ULL;
    }
    return seed;
  }
};
}  // namespace std

#endif  // DCCP_PHYSICAL_LOCATION_REGISTRY_ADDRESS_HPP
