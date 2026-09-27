// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "dccp/physical_location_registry/limits.hpp"

#include "dccp/physical_location_registry/text.hpp"

namespace dccp::physical_location_registry {
namespace {

Result<void> require_at_least(std::uint64_t value, std::uint64_t minimum, const char* field) {
  if (value < minimum) {
    return Error(ErrorCode::LimitExceeded,
                 std::string(field) + " must be at least " + std::to_string(minimum))
        .with_subject(std::to_string(value));
  }
  return ok();
}

Result<void> require_at_most(std::uint64_t value, std::uint64_t maximum, const char* field) {
  if (value > maximum) {
    return Error(ErrorCode::LimitExceeded,
                 std::string(field) + " must be at most " + std::to_string(maximum))
        .with_subject(std::to_string(value));
  }
  return ok();
}

Result<void> require_between(std::uint64_t value, std::uint64_t minimum, std::uint64_t maximum,
                             const char* field) {
  PLR_CHECK(require_at_least(value, minimum, field));
  return require_at_most(value, maximum, field);
}

}  // namespace

Result<void> Limits::validate() const {
  PLR_CHECK(require_between(max_locations, 1, kHardMaxLocations, "max_locations"));
  PLR_CHECK(require_between(max_depth, 1, kHardMaxDepth, "max_depth"));
  PLR_CHECK(require_between(max_children_per_location, 1, kHardMaxChildrenPerLocation,
                             "max_children_per_location"));
  PLR_CHECK(require_between(max_address_component_bytes, 1, kMaxAddressComponentBytes,
                             "max_address_component_bytes"));
  PLR_CHECK(require_between(max_label_bytes, 1, kMaxLabelBytes, "max_label_bytes"));
  PLR_CHECK(require_between(max_reason_bytes, 1, kMaxReasonBytes, "max_reason_bytes"));
  PLR_CHECK(require_between(max_source_bytes, 1, kMaxSourceBytes, "max_source_bytes"));
  PLR_CHECK(require_between(max_path_bytes, 2, kHardMaxPathBytes, "max_path_bytes"));
  PLR_CHECK(require_between(max_aliases_per_location, 1, kHardMaxAliasesPerLocation,
                             "max_aliases_per_location"));
  PLR_CHECK(require_between(max_total_aliases, 1, kHardMaxTotalAliases, "max_total_aliases"));
  PLR_CHECK(require_between(max_moves_per_location, 1, kHardMaxMovesPerLocation,
                             "max_moves_per_location"));
  PLR_CHECK(require_between(max_total_moves, 1, kHardMaxTotalMoves, "max_total_moves"));
  PLR_CHECK(require_between(max_total_replacements, 1, kHardMaxTotalReplacements,
                             "max_total_replacements"));
  PLR_CHECK(require_between(max_operation_receipts, 1, kHardMaxOperationReceipts,
                             "max_operation_receipts"));
  PLR_CHECK(require_between(max_retained_revisions, 1, kHardMaxRetainedRevisions,
                             "max_retained_revisions"));
  PLR_CHECK(require_between(max_publications_retained, 2, kHardMaxPublicationsRetained,
                             "max_publications_retained"));
  PLR_CHECK(require_between(max_traversal_nodes, 1, kHardMaxTraversalNodes, "max_traversal_nodes"));
  PLR_CHECK(require_between(max_state_bytes, 4096, kHardMaxStateBytes, "max_state_bytes"));

  if (max_path_bytes <= max_address_component_bytes) {
    return Error(ErrorCode::LimitExceeded,
                 "max_path_bytes must exceed max_address_component_bytes by at least one separator")
        .with_subject(std::to_string(max_path_bytes));
  }
  if (max_total_aliases < max_aliases_per_location) {
    return Error(ErrorCode::LimitExceeded,
                 "max_total_aliases must be at least max_aliases_per_location")
        .with_subject(std::to_string(max_total_aliases));
  }
  if (max_total_moves < max_moves_per_location) {
    return Error(ErrorCode::LimitExceeded,
                 "max_total_moves must be at least max_moves_per_location")
        .with_subject(std::to_string(max_total_moves));
  }
  if (max_children_per_location > max_locations) {
    return Error(ErrorCode::LimitExceeded,
                 "max_children_per_location must not exceed max_locations")
        .with_subject(std::to_string(max_children_per_location));
  }
  return ok();
}

std::string Limits::to_string() const {
  std::string text;
  const auto append = [&text](const char* key, std::uint64_t value) {
    if (!text.empty()) {
      text.push_back(';');
    }
    text.append(key);
    text.push_back('=');
    text.append(std::to_string(value));
  };
  append("max_locations", max_locations);
  append("max_depth", max_depth);
  append("max_children_per_location", max_children_per_location);
  append("max_address_component_bytes", max_address_component_bytes);
  append("max_label_bytes", max_label_bytes);
  append("max_reason_bytes", max_reason_bytes);
  append("max_source_bytes", max_source_bytes);
  append("max_path_bytes", max_path_bytes);
  append("max_aliases_per_location", max_aliases_per_location);
  append("max_total_aliases", max_total_aliases);
  append("max_moves_per_location", max_moves_per_location);
  append("max_total_moves", max_total_moves);
  append("max_total_replacements", max_total_replacements);
  append("max_operation_receipts", max_operation_receipts);
  append("max_retained_revisions", max_retained_revisions);
  append("max_publications_retained", max_publications_retained);
  append("max_traversal_nodes", max_traversal_nodes);
  append("max_state_bytes", max_state_bytes);
  return text;
}

}  // namespace dccp::physical_location_registry