// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#ifndef DCCP_PHYSICAL_LOCATION_REGISTRY_REQUESTS_HPP
#define DCCP_PHYSICAL_LOCATION_REGISTRY_REQUESTS_HPP

#include <cstdint>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

#include "dccp/physical_location_registry/address.hpp"
#include "dccp/physical_location_registry/export.hpp"
#include "dccp/physical_location_registry/kind.hpp"
#include "dccp/physical_location_registry/limits.hpp"
#include "dccp/physical_location_registry/provenance.hpp"
#include "dccp/physical_location_registry/rack_unit.hpp"
#include "dccp/physical_location_registry/result.hpp"
#include "dccp/physical_location_registry/strong_id.hpp"

namespace dccp::physical_location_registry {

/// The writer authority a mutation was authorized under.
///
/// A session obtains one at open time and stamps it on every mutation. An
/// authority whose store does not match, or whose writer epoch is older than the
/// store's current epoch, is rejected: a caller cannot keep mutating state that a
/// later writer incarnation now owns.
struct PLR_API MutationAuthority {
  StoreId store_id;
  WriterEpoch writer_epoch;

  friend bool operator==(const MutationAuthority&, const MutationAuthority&) noexcept = default;
};

/// Everything a mutation needs besides its own subject matter.
struct PLR_API MutationContext {
  ActorId actor;  ///< required: provenance attribution

  /// Caller-supplied instant recorded in provenance. The library never reads
  /// the system clock.
  Timestamp at;

  /// When present, verified against the session before anything else happens.
  std::optional<MutationAuthority> authority;

  /// Optimistic concurrency: reject unless the committed revision matches.
  std::optional<LocationRevision> expected_revision;

  /// Optimistic concurrency for single-location mutations: reject unless the
  /// target's generation matches.
  std::optional<LocationGeneration> expected_generation;

  /// Idempotency key. When set, a second submission of the same logical
  /// operation returns the stored receipt instead of applying the mutation
  /// again; a different request under the same key is rejected.
  std::optional<OperationId> operation_id;

  /// Free-form audit text recorded in the mutation record.
  std::string reason;

  /// Cooperative cancellation. Checked before publication; once the durable
  /// publication begins it runs to completion and the mutation reports success.
  std::stop_token stop;
};

/// A request to create one location.
struct PLR_API CreateLocationRequest {
  LocationId id;
  LocationKind kind = LocationKind::Facility;
  std::optional<LocationId> parent;
  AddressComponent component;
  std::string label;
  std::optional<RackUnitCoordinate> unit;
  std::optional<RackEnvelope> envelope;
  std::string source;
  MutationContext context;

  /// Parses and validates the textual form of the identity-bearing fields.
  static Result<CreateLocationRequest> make(std::string_view id,
                                            LocationKind kind,
                                            std::string_view parent,
                                            std::string_view component,
                                            std::string_view label,
                                            const MutationContext& context);
};

/// Change a location's own address component; its subtree follows.
struct PLR_API ReaddressRequest {
  LocationId id;
  AddressComponent component;
  MutationContext context;

  static Result<ReaddressRequest> make(std::string_view id,
                                       std::string_view component,
                                       const MutationContext& context);
};

/// Change a location's human-readable label. Address and identity are unchanged.
struct PLR_API RelabelRequest {
  LocationId id;
  std::string label;
  MutationContext context;

  static Result<RelabelRequest> make(std::string_view id,
                                     std::string_view label,
                                     const MutationContext& context);
};

/// Move a location (and its subtree) under a different parent.
struct PLR_API MoveLocationRequest {
  LocationId id;
  LocationId new_parent;
  MutationContext context;

  static Result<MoveLocationRequest> make(std::string_view id,
                                          std::string_view new_parent,
                                          const MutationContext& context);
};

/// How much of a subtree a lifecycle mutation applies to.
enum class SubtreeMode : std::uint8_t {
  /// Only the named location; it must have no Active descendants when retiring,
  /// and no non-active ancestors when reactivating.
  LocationOnly = 0,
  /// The named location and every descendant, applied atomically.
  Subtree = 1,
};

/// Canonical lower-case name ("location-only", "subtree").
PLR_API std::string_view subtree_mode_name(SubtreeMode mode) noexcept;

/// Retire a location, or a whole subtree bottom-up.
struct PLR_API RetireLocationRequest {
  LocationId id;
  SubtreeMode mode = SubtreeMode::LocationOnly;
  MutationContext context;

  static Result<RetireLocationRequest> make(std::string_view id,
                                            SubtreeMode mode,
                                            const MutationContext& context);
};

/// Reactivate a retired location, or a whole retired subtree top-down.
struct PLR_API ReactivateLocationRequest {
  LocationId id;
  SubtreeMode mode = SubtreeMode::LocationOnly;
  MutationContext context;

  static Result<ReactivateLocationRequest> make(std::string_view id,
                                                SubtreeMode mode,
                                                const MutationContext& context);
};

/// Replace a location: the predecessor ceases to exist at its address and a new
/// successor location is created at the same parent and component.
struct PLR_API ReplaceLocationRequest {
  LocationId predecessor;
  LocationId successor_id;
  std::string successor_label;
  std::optional<RackUnitCoordinate> successor_unit;
  std::optional<RackEnvelope> successor_envelope;
  std::string source;
  MutationContext context;

  static Result<ReplaceLocationRequest> make(std::string_view predecessor,
                                             std::string_view successor_id,
                                             std::string_view successor_label,
                                             const MutationContext& context);
};

/// Bind an additional address to a location.
struct PLR_API AddAliasRequest {
  LocationId id;
  LocationPath alias;
  MutationContext context;

  static Result<AddAliasRequest> make(std::string_view id,
                                      std::string_view alias,
                                      const Limits& limits,
                                      const MutationContext& context);
};

/// Remove an alias binding from a location.
struct PLR_API RemoveAliasRequest {
  LocationId id;
  LocationPath alias;
  MutationContext context;

  static Result<RemoveAliasRequest> make(std::string_view id,
                                         std::string_view alias,
                                         const Limits& limits,
                                         const MutationContext& context);
};

/// Set or clear the rack geometry a location carries.
struct PLR_API SetRackGeometryRequest {
  LocationId id;
  std::optional<RackUnitCoordinate> unit;
  std::optional<RackEnvelope> envelope;
  MutationContext context;

  static Result<SetRackGeometryRequest> make(std::string_view id,
                                             const MutationContext& context);
};

/// What a committed mutation did, and at which revision.
struct PLR_API MutationReceipt {
  LocationRevision revision;  ///< committed revision
  StateSequence sequence;     ///< published state generation
  LocationGeneration generation;  ///< generation of the primary target afterwards
  std::optional<LocationGeneration> successor_generation;  ///< replacement only
  std::uint32_t affected_locations = 0;   ///< locations whose generation advanced
  std::uint32_t affected_descendants = 0;  ///< descendants that followed a moved node
  bool replayed = false;  ///< true when an idempotent replay returned a stored receipt
};

/// The durable record that makes idempotent replay possible after a restart.
struct PLR_API OperationReceipt {
  OperationId operation_id;
  std::string fingerprint;  ///< lowercase hex SHA-256 of the canonical request
  LocationRevision revision;
  StateSequence sequence;
  LocationGeneration generation;
  std::optional<LocationGeneration> successor_generation;
  std::uint32_t affected_locations = 0;
  std::uint32_t affected_descendants = 0;
  Timestamp at;
  ActorId actor;
};

}  // namespace dccp::physical_location_registry

#endif  // DCCP_PHYSICAL_LOCATION_REGISTRY_REQUESTS_HPP
