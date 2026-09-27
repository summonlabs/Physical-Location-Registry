// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Request factories. These parse and validate the identity-bearing text fields
// so that a request cannot even be constructed from malformed input; the
// registry still applies every structural rule, because a request is a value and
// the registry is the authority.

#include <string>
#include <utility>

#include "dccp/physical_location_registry/requests.hpp"
#include "dccp/physical_location_registry/text.hpp"

namespace dccp::physical_location_registry {

std::string_view subtree_mode_name(SubtreeMode mode) noexcept {
  switch (mode) {
    case SubtreeMode::LocationOnly:
      return "location-only";
    case SubtreeMode::Subtree:
      return "subtree";
  }
  return "unrecognized";
}

Result<CreateLocationRequest> CreateLocationRequest::make(std::string_view id,
                                                          LocationKind kind,
                                                          std::string_view parent,
                                                          std::string_view component,
                                                          std::string_view label,
                                                          const MutationContext& context) {
  PLR_TRY(parsed_id, LocationId::parse(id));
  PLR_TRY(parsed_component, AddressComponent::parse(component));
  if (!is_valid_label(label)) {
    return Error(ErrorCode::MalformedLabel,
                 std::string("label rejected: ") + std::string(label_syntax_help()))
        .with_subject(std::string(label.substr(0, 160)));
  }
  CreateLocationRequest request{parsed_id,
                                kind,
                                std::nullopt,
                                parsed_component,
                                std::string(label),
                                std::nullopt,
                                std::nullopt,
                                std::string(),
                                context};
  if (!parent.empty()) {
    PLR_TRY(parsed_parent, LocationId::parse(parent));
    request.parent = parsed_parent;
  }
  return request;
}

Result<ReaddressRequest> ReaddressRequest::make(std::string_view id,
                                                std::string_view component,
                                                const MutationContext& context) {
  PLR_TRY(parsed_id, LocationId::parse(id));
  PLR_TRY(parsed_component, AddressComponent::parse(component));
  return ReaddressRequest(parsed_id, parsed_component, context);
}

Result<RelabelRequest> RelabelRequest::make(std::string_view id,
                                            std::string_view label,
                                            const MutationContext& context) {
  PLR_TRY(parsed_id, LocationId::parse(id));
  if (!is_valid_label(label)) {
    return Error(ErrorCode::MalformedLabel,
                 std::string("label rejected: ") + std::string(label_syntax_help()))
        .with_subject(std::string(label.substr(0, 160)));
  }
  return RelabelRequest(parsed_id, std::string(label), context);
}

Result<MoveLocationRequest> MoveLocationRequest::make(std::string_view id,
                                                      std::string_view new_parent,
                                                      const MutationContext& context) {
  PLR_TRY(parsed_id, LocationId::parse(id));
  PLR_TRY(parsed_parent, LocationId::parse(new_parent));
  return MoveLocationRequest(parsed_id, parsed_parent, context);
}

Result<RetireLocationRequest> RetireLocationRequest::make(std::string_view id,
                                                          SubtreeMode mode,
                                                          const MutationContext& context) {
  PLR_TRY(parsed_id, LocationId::parse(id));
  return RetireLocationRequest(parsed_id, mode, context);
}

Result<ReactivateLocationRequest> ReactivateLocationRequest::make(std::string_view id,
                                                                  SubtreeMode mode,
                                                                  const MutationContext& context) {
  PLR_TRY(parsed_id, LocationId::parse(id));
  return ReactivateLocationRequest(parsed_id, mode, context);
}

Result<ReplaceLocationRequest> ReplaceLocationRequest::make(std::string_view predecessor,
                                                             std::string_view successor_id,
                                                             std::string_view successor_label,
                                                             const MutationContext& context) {
  PLR_TRY(parsed_predecessor, LocationId::parse(predecessor));
  PLR_TRY(parsed_successor, LocationId::parse(successor_id));
  if (!is_valid_label(successor_label)) {
    return Error(ErrorCode::MalformedLabel,
                 std::string("label rejected: ") + std::string(label_syntax_help()))
        .with_subject(std::string(successor_label.substr(0, 160)));
  }
  ReplaceLocationRequest request{parsed_predecessor, parsed_successor,
                                 std::string(successor_label), std::nullopt, std::nullopt,
                                 std::string(), context};
  return request;
}

Result<AddAliasRequest> AddAliasRequest::make(std::string_view id,
                                              std::string_view alias,
                                              const Limits& limits,
                                              const MutationContext& context) {
  PLR_TRY(parsed_id, LocationId::parse(id));
  PLR_TRY(parsed_alias, LocationPath::parse(alias, limits));
  if (parsed_alias.empty()) {
    return Error(ErrorCode::MalformedPath, "an alias cannot be the empty root path");
  }
  return AddAliasRequest(parsed_id, parsed_alias, context);
}

Result<RemoveAliasRequest> RemoveAliasRequest::make(std::string_view id,
                                                    std::string_view alias,
                                                    const Limits& limits,
                                                    const MutationContext& context) {
  PLR_TRY(parsed_id, LocationId::parse(id));
  PLR_TRY(parsed_alias, LocationPath::parse(alias, limits));
  if (parsed_alias.empty()) {
    return Error(ErrorCode::MalformedPath, "an alias cannot be the empty root path");
  }
  return RemoveAliasRequest(parsed_id, parsed_alias, context);
}

Result<SetRackGeometryRequest> SetRackGeometryRequest::make(std::string_view id,
                                                            const MutationContext& context) {
  PLR_TRY(parsed_id, LocationId::parse(id));
  return SetRackGeometryRequest(parsed_id, std::nullopt, std::nullopt, context);
}

}  // namespace dccp::physical_location_registry
