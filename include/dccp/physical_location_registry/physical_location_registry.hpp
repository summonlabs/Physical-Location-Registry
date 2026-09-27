// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#ifndef DCCP_PHYSICAL_LOCATION_REGISTRY_PHYSICAL_LOCATION_REGISTRY_HPP
#define DCCP_PHYSICAL_LOCATION_REGISTRY_PHYSICAL_LOCATION_REGISTRY_HPP

/// Umbrella header for the Physical Location Registry library.
///
/// Physical Location Registry owns stable physical-location identities and
/// hierarchical addressing semantics for the DCCP facility control plane: what
/// places exist, where they are in the address hierarchy, which generation of
/// each place is current, and how a place moves, is renamed, is retired or is
/// replaced.
///
/// It does not own rack occupancy, asset inventory, facility topology edges,
/// placement planning, geospatial mapping, capacity reservations, power or
/// cooling control, or multi-site federation. Those systems consume the typed
/// identities, generations and addresses defined here.

#include "dccp/physical_location_registry/address.hpp"
#include "dccp/physical_location_registry/diff.hpp"
#include "dccp/physical_location_registry/digest.hpp"
#include "dccp/physical_location_registry/explanation.hpp"
#include "dccp/physical_location_registry/kind.hpp"
#include "dccp/physical_location_registry/lifecycle.hpp"
#include "dccp/physical_location_registry/limits.hpp"
#include "dccp/physical_location_registry/location.hpp"
#include "dccp/physical_location_registry/provenance.hpp"
#include "dccp/physical_location_registry/rack_unit.hpp"
#include "dccp/physical_location_registry/registry.hpp"
#include "dccp/physical_location_registry/requests.hpp"
#include "dccp/physical_location_registry/result.hpp"
#include "dccp/physical_location_registry/snapshot.hpp"
#include "dccp/physical_location_registry/store.hpp"
#include "dccp/physical_location_registry/strong_id.hpp"
#include "dccp/physical_location_registry/text.hpp"
#include "dccp/physical_location_registry/version.hpp"

#endif  // DCCP_PHYSICAL_LOCATION_REGISTRY_PHYSICAL_LOCATION_REGISTRY_HPP
