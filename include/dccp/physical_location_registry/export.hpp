// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#ifndef DCCP_PHYSICAL_LOCATION_REGISTRY_EXPORT_HPP
#define DCCP_PHYSICAL_LOCATION_REGISTRY_EXPORT_HPP

// Shared-library visibility decoration. Static builds see an empty macro, so
// the same source builds both ways without a conditional compilation maze.

#if defined(_WIN32) && defined(PHYSICAL_LOCATION_REGISTRY_USE_SHARED)
#define PLR_API __declspec(dllimport)
#elif defined(_WIN32) && defined(PHYSICAL_LOCATION_REGISTRY_BUILD_SHARED)
#define PLR_API __declspec(dllexport)
#else
#define PLR_API
#endif

#endif  // DCCP_PHYSICAL_LOCATION_REGISTRY_EXPORT_HPP
