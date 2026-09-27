// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#ifndef DCCP_PHYSICAL_LOCATION_REGISTRY_PROVENANCE_HPP
#define DCCP_PHYSICAL_LOCATION_REGISTRY_PROVENANCE_HPP

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "dccp/physical_location_registry/export.hpp"
#include "dccp/physical_location_registry/result.hpp"
#include "dccp/physical_location_registry/strong_id.hpp"

namespace dccp::physical_location_registry {

/// Largest accepted Unix timestamp: 9999-12-31T23:59:59Z.
inline constexpr std::int64_t kMaxUnixSeconds = 253402300799LL;

/// A UTC instant supplied by the caller.
///
/// The library never reads the system clock: every timestamp in authoritative
/// state is caller-supplied, validated here, and stored verbatim. That is what
/// makes deterministic serialization of a mutation sequence achievable, and
/// what lets a durability test assert exact bytes.
class PLR_API Timestamp {
 public:
  /// Unknown instant (Unix epoch 0). Provenance may record it; lifecycle rules
  /// never depend on time.
  constexpr Timestamp() noexcept = default;

  static Result<Timestamp> make(std::int64_t unix_seconds, std::uint32_t nanos);

  static Result<Timestamp> from_unix_seconds(std::int64_t unix_seconds);

  /// Parses "YYYY-MM-DDTHH:MM:SS[.fffffffff]Z" or a bare Unix second count.
  static Result<Timestamp> parse_text(std::string_view raw);

  constexpr std::int64_t unix_seconds() const noexcept { return unix_seconds_; }
  constexpr std::uint32_t nanos() const noexcept { return nanos_; }
  constexpr bool known() const noexcept { return unix_seconds_ != 0 || nanos_ != 0; }

  /// Canonical form: "YYYY-MM-DDTHH:MM:SSZ" when nanos are zero, otherwise
  /// "YYYY-MM-DDTHH:MM:SS.fffffffffZ".
  std::string to_string() const;

  friend constexpr bool operator==(const Timestamp&, const Timestamp&) noexcept = default;
  friend constexpr std::strong_ordering operator<=>(const Timestamp& lhs, const Timestamp& rhs) noexcept {
    if (lhs.unix_seconds_ != rhs.unix_seconds_) {
      return lhs.unix_seconds_ <=> rhs.unix_seconds_;
    }
    return lhs.nanos_ <=> rhs.nanos_;
  }

 private:
  std::int64_t unix_seconds_ = 0;
  std::uint32_t nanos_ = 0;
};

/// The verb that changed a location's address.
enum class MoveKind : std::uint8_t {
  Reparent = 0,   ///< the location moved under a different parent
  Readdress = 1,  ///< the location kept its parent and changed its own component
};

/// Canonical lower-case name ("reparent", "readdress").
PLR_API std::string_view move_kind_name(MoveKind kind) noexcept;

/// Parses a canonical move kind name (ASCII case-insensitive).
PLR_API Result<MoveKind> parse_move_kind(std::string_view raw);

/// Audit record of one address change.
///
/// A subtree move is recorded once, at the node that was moved; the descendants
/// that followed it are counted in affected_descendants and have their own
/// generations advanced so that a stale cached address is detectable.
struct PLR_API MoveRecord {
  MoveKind kind = MoveKind::Reparent;
  LocationRevision revision;  ///< committed revision produced by this move
  LocationGeneration generation_before;
  LocationGeneration generation_after;
  std::optional<LocationId> from_parent;
  std::optional<LocationId> to_parent;
  std::string from_component;
  std::string to_component;
  std::string from_path;  ///< canonical absolute path before the move
  std::string to_path;    ///< canonical absolute path after the move
  std::uint32_t affected_descendants = 0;
  Timestamp at;
  ActorId actor;
  std::string reason;

  friend bool operator==(const MoveRecord&, const MoveRecord&) noexcept = default;
};

/// Audit record of one replacement: a location ceased to exist and a successor
/// location was created at the same time.
struct PLR_API ReplacementRecord {
  LocationId predecessor;
  LocationId successor;
  LocationRevision revision;
  Timestamp at;
  ActorId actor;
  std::string reason;

  friend bool operator==(const ReplacementRecord&, const ReplacementRecord&) noexcept = default;
};

/// Who created a location and who last changed it.
///
/// A reader can always answer "where did this authoritative fact come from"
/// without consulting a separate audit store.
struct PLR_API ProvenanceRecord {
  ActorId created_by;
  Timestamp created_at;
  LocationRevision created_revision;
  std::string source;  ///< free-form system of record; validated text
  ActorId last_modified_by;
  Timestamp last_modified_at;
  LocationRevision last_modified_revision;

  friend bool operator==(const ProvenanceRecord&, const ProvenanceRecord&) noexcept = default;
};

/// Canonical textual form of a batch of timestamps, used by listing output.
PLR_API std::string format_timestamp(std::int64_t unix_seconds, std::uint32_t nanos);

}  // namespace dccp::physical_location_registry

#endif  // DCCP_PHYSICAL_LOCATION_REGISTRY_PROVENANCE_HPP
