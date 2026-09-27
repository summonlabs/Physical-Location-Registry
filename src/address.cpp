// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "dccp/physical_location_registry/address.hpp"

#include <algorithm>

#include "dccp/physical_location_registry/text.hpp"

namespace dccp::physical_location_registry {

Result<AddressComponent> AddressComponent::parse(std::string_view raw) {
  if (!is_valid_address_component(raw)) {
    return Error(ErrorCode::MalformedAddressComponent,
                 "address component rejected: " + std::string(address_component_syntax_help()))
        .with_subject(std::string(raw.substr(0, 160)));
  }
  return AddressComponent(std::string(raw));
}

Result<LocationPath> LocationPath::parse(std::string_view raw, const Limits& limits) {
  const std::size_t max_bytes = limits.max_path_bytes;
  if (raw.empty() || raw.front() != '/') {
    return Error(ErrorCode::MalformedPath, "absolute path must begin with '/'")
        .with_subject(std::string(raw.substr(0, 160)));
  }
  if (raw.size() > max_bytes) {
    return Error(ErrorCode::PathTooLong,
                 "path is longer than the configured maximum of " + std::to_string(max_bytes) + " bytes")
        .with_subject(std::string(raw.substr(0, 160)));
  }
  if (raw.size() == 1) {
    // "/" is the empty path: no location. It is a legal value for the root of
    // the hierarchy but never the address of a stored location.
    return LocationPath();
  }
  if (raw.back() == '/') {
    return Error(ErrorCode::MalformedPath, "path must not end with '/'")
        .with_subject(std::string(raw));
  }

  std::vector<AddressComponent> components;
  const std::size_t estimated = std::count(raw.begin(), raw.end(), '/');
  if (estimated > static_cast<std::size_t>(limits.max_depth)) {
    return Error(ErrorCode::PathTooDeep,
                 "path has more than the configured maximum of " + std::to_string(limits.max_depth) +
                     " components")
        .with_subject(std::string(raw.substr(0, 160)));
  }
  components.reserve(estimated);

  std::size_t start = 1;
  while (start <= raw.size()) {
    const std::size_t separator = raw.find('/', start);
    const std::size_t end = separator == std::string_view::npos ? raw.size() : separator;
    const std::string_view component = raw.substr(start, end - start);
    if (component.empty()) {
      return Error(ErrorCode::MalformedPath, "path contains an empty component")
          .with_subject(std::string(raw));
    }
    auto parsed = AddressComponent::parse(component);
    if (!parsed.has_value()) {
      return parsed.error();
    }
    components.push_back(std::move(parsed).value());
    if (separator == std::string_view::npos) {
      break;
    }
    start = separator + 1;
  }

  if (components.size() > static_cast<std::size_t>(limits.max_depth)) {
    return Error(ErrorCode::PathTooDeep,
                 "path has more than the configured maximum of " + std::to_string(limits.max_depth) +
                     " components")
        .with_subject(std::string(raw.substr(0, 160)));
  }

  LocationPath path;
  path.components_ = std::move(components);
  return path;
}

Result<LocationPath> LocationPath::from_components(std::vector<AddressComponent> components,
                                                   const Limits& limits) {
  if (components.empty()) {
    return LocationPath();
  }
  if (components.size() > static_cast<std::size_t>(limits.max_depth)) {
    return Error(ErrorCode::PathTooDeep,
                 "path has more than the configured maximum of " + std::to_string(limits.max_depth) +
                     " components");
  }
  std::size_t bytes = 1 + (components.size() - 1);
  for (const AddressComponent& component : components) {
    bytes += component.value().size();
  }
  if (bytes > limits.max_path_bytes) {
    return Error(ErrorCode::PathTooLong,
                 "path is longer than the configured maximum of " +
                     std::to_string(limits.max_path_bytes) + " bytes");
  }
  LocationPath path;
  path.components_ = std::move(components);
  return path;
}

std::optional<LocationPath> LocationPath::parent() const {
  if (components_.size() <= 1) {
    return std::nullopt;
  }
  LocationPath parent_path;
  parent_path.components_.assign(components_.begin(), components_.end() - 1);
  return parent_path;
}

bool LocationPath::is_ancestor_of(const LocationPath& other) const noexcept {
  if (components_.size() >= other.components_.size()) {
    return false;
  }
  return std::equal(components_.begin(), components_.end(), other.components_.begin());
}

Result<LocationPath> LocationPath::child(const AddressComponent& component, const Limits& limits) const {
  std::vector<AddressComponent> components = components_;
  components.push_back(component);
  return from_components(std::move(components), limits);
}

std::string LocationPath::to_string() const {
  if (components_.empty()) {
    return "/";
  }
  std::size_t bytes = 0;
  for (const AddressComponent& component : components_) {
    bytes += component.value().size() + 1;
  }
  std::string text;
  text.reserve(bytes);
  for (const AddressComponent& component : components_) {
    text.push_back('/');
    text.append(component.value());
  }
  return text;
}

std::size_t LocationPath::byte_length() const noexcept {
  if (components_.empty()) {
    return 1;
  }
  std::size_t bytes = 0;
  for (const AddressComponent& component : components_) {
    bytes += component.value().size() + 1;
  }
  return bytes;
}

}  // namespace dccp::physical_location_registry
