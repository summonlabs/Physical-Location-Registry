// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#ifndef DCCP_PHYSICAL_LOCATION_REGISTRY_LIMITS_HPP
#define DCCP_PHYSICAL_LOCATION_REGISTRY_LIMITS_HPP

#include <cstdint>
#include <string>

#include "dccp/physical_location_registry/export.hpp"
#include "dccp/physical_location_registry/result.hpp"

namespace dccp::physical_location_registry {

/// Absolute ceilings that a configured Limits may never exceed.
///
/// These exist so that persisted limits read from an untrusted store file can
/// be validated against a fixed, compile-time maximum before any structure is
/// sized from them.
inline constexpr std::uint32_t kHardMaxLocations = 4'000'000U;
inline constexpr std::uint32_t kHardMaxDepth = 128U;
inline constexpr std::uint32_t kHardMaxPathBytes = 65536U;
inline constexpr std::uint32_t kHardMaxChildrenPerLocation = 1'000'000U;
inline constexpr std::uint32_t kHardMaxAliasesPerLocation = 1024U;
inline constexpr std::uint32_t kHardMaxTotalAliases = 4'000'000U;
inline constexpr std::uint32_t kHardMaxMovesPerLocation = 4096U;
inline constexpr std::uint32_t kHardMaxTotalMoves = 16'000'000U;
inline constexpr std::uint32_t kHardMaxTotalReplacements = 4'000'000U;
inline constexpr std::uint32_t kHardMaxOperationReceipts = 1'000'000U;
inline constexpr std::uint32_t kHardMaxRetainedRevisions = 64U;
inline constexpr std::uint32_t kHardMaxPublicationsRetained = 64U;
inline constexpr std::uint64_t kHardMaxStateBytes = 1024ULL * 1024ULL * 1024ULL;
inline constexpr std::uint32_t kHardMaxTraversalNodes = 4'000'000U;

/// Bounds on every structure the registry will hold or read.
///
/// Limits are fixed when a store is created, are persisted inside the
/// authoritative state, and are validated against the hard ceilings above when
/// that state is read back. They bound memory, durable size and traversal cost;
/// exceeding one is a deterministic rejection carrying ErrorCode::LimitExceeded,
/// never a silent truncation.
struct PLR_API Limits {
  std::uint32_t max_locations = 100'000U;
  std::uint32_t max_depth = 32U;
  std::uint32_t max_children_per_location = 4096U;
  std::uint32_t max_address_component_bytes = 64U;
  std::uint32_t max_label_bytes = 256U;
  std::uint32_t max_reason_bytes = 256U;
  std::uint32_t max_source_bytes = 128U;
  std::uint32_t max_path_bytes = 4096U;
  std::uint32_t max_aliases_per_location = 8U;
  std::uint32_t max_total_aliases = 65'536U;
  std::uint32_t max_moves_per_location = 64U;
  std::uint32_t max_total_moves = 262'144U;
  std::uint32_t max_total_replacements = 65'536U;
  std::uint32_t max_operation_receipts = 1024U;
  std::uint32_t max_retained_revisions = 4U;
  std::uint32_t max_publications_retained = 3U;
  std::uint32_t max_traversal_nodes = 1'000'000U;
  std::uint64_t max_state_bytes = 64ULL * 1024ULL * 1024ULL;

  /// Validates every field and every relation between fields.
  Result<void> validate() const;

  /// Canonical, deterministic textual form ("key=value;key=value"), used by the
  /// inspection tooling and by diagnostics.
  std::string to_string() const;

  friend bool operator==(const Limits&, const Limits&) noexcept = default;
};

}  // namespace dccp::physical_location_registry

#endif  // DCCP_PHYSICAL_LOCATION_REGISTRY_LIMITS_HPP
