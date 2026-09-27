// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "dccp/physical_location_registry/version.hpp"

namespace dccp::physical_location_registry {

std::string_view version_string() noexcept { return "1.0.0"; }

std::string_view library_name() noexcept { return "physical_location_registry"; }

std::string_view snapshot_format_name() noexcept { return "PLRSNAP/1"; }

}  // namespace dccp::physical_location_registry
