// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#ifndef DCCP_PHYSICAL_LOCATION_REGISTRY_VERSION_HPP
#define DCCP_PHYSICAL_LOCATION_REGISTRY_VERSION_HPP

#include <cstdint>
#include <string_view>

#include "dccp/physical_location_registry/export.hpp"

namespace dccp::physical_location_registry {

inline constexpr int kVersionMajor = 1;
inline constexpr int kVersionMinor = 0;
inline constexpr int kVersionPatch = 0;

/// Semantic version of the library ("1.0.0").
PLR_API std::string_view version_string() noexcept;

/// Canonical library name ("physical_location_registry").
PLR_API std::string_view library_name() noexcept;

/// Canonical snapshot format identity ("PLRSNAP/1").
PLR_API std::string_view snapshot_format_name() noexcept;

}  // namespace dccp::physical_location_registry

#endif  // DCCP_PHYSICAL_LOCATION_REGISTRY_VERSION_HPP
