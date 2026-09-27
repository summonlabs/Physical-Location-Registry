// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#ifndef DCCP_PHYSICAL_LOCATION_REGISTRY_STRONG_ID_HPP
#define DCCP_PHYSICAL_LOCATION_REGISTRY_STRONG_ID_HPP

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

#include "dccp/physical_location_registry/result.hpp"

namespace dccp::physical_location_registry {

/// Maximum length, in bytes, of any identifier accepted by the library.
inline constexpr std::size_t kMaxIdentifierBytes = 128;

/// Identifier syntax (canonical form):
///   - 1..128 bytes;
///   - first and last byte are ASCII alphanumeric;
///   - interior bytes are ASCII alphanumeric or one of '.', ':', '-'.
///
/// The grammar is deliberately free of whitespace, quoting, path separators and
/// non-ASCII bytes so that canonical serialization is escape-free and identity
/// strings cannot smuggle paths, control characters or look-alikes.
PLR_API bool is_valid_identifier_syntax(std::string_view raw) noexcept;

/// Explains the identifier grammar; used in rejection messages.
PLR_API std::string_view identifier_syntax_help() noexcept;

/// Tag types selecting a distinct StrongId instantiation. Unrelated identities
/// are distinct types and cannot be implicitly converted into one another.
struct LocationIdTag {
  static constexpr std::string_view kind_name = "location";
};
struct ActorIdTag {
  static constexpr std::string_view kind_name = "actor";
};
struct StoreIdTag {
  static constexpr std::string_view kind_name = "store";
};
struct OperationIdTag {
  static constexpr std::string_view kind_name = "operation";
};

/// A validated, strongly typed identifier.
///
/// Construction only succeeds through parse(); the default-constructed value is
/// empty and only exists so that identifiers can live in containers. An empty
/// identifier is never written to durable state and is rejected by every public
/// entry point that requires one.
template <class Tag>
class StrongId {
 public:
  using tag_type = Tag;

  StrongId() noexcept = default;

  /// Parses and validates untrusted text.
  static Result<StrongId> parse(std::string_view raw) {
    if (!is_valid_identifier_syntax(raw)) {
      return Error(ErrorCode::MalformedIdentifier,
                   "identifier does not match the canonical grammar; " +
                       std::string(identifier_syntax_help()))
          .with_subject(std::string(raw.substr(0, 160)));
    }
    return StrongId(std::string(raw));
  }

  bool empty() const noexcept { return value_.empty(); }
  std::string_view value() const noexcept { return value_; }
  const std::string& str() const noexcept { return value_; }

  friend bool operator==(const StrongId& lhs, const StrongId& rhs) noexcept = default;

  /// Ordering is byte-wise over the canonical identifier, so iteration order is
  /// identical on every platform and in every locale.
  friend std::strong_ordering operator<=>(const StrongId& lhs, const StrongId& rhs) noexcept {
    const int cmp = lhs.value_.compare(rhs.value_);
    return cmp < 0 ? std::strong_ordering::less
                   : (cmp > 0 ? std::strong_ordering::greater : std::strong_ordering::equal);
  }

 private:
  explicit StrongId(std::string value) : value_(std::move(value)) {}

  std::string value_;
};

using LocationId = StrongId<LocationIdTag>;
using ActorId = StrongId<ActorIdTag>;
using StoreId = StrongId<StoreIdTag>;
using OperationId = StrongId<OperationIdTag>;

/// Monotonic per-location generation of authoritative location state.
///
/// A location is created at generation 1 and every accepted mutation of that
/// location (including a mutation driven by a move of an ancestor) advances it.
/// A caller that holds a generation has a token that becomes stale the moment
/// the location changes, which is what makes stale mutations rejectable rather
/// than silently applied. Generation 0 means "no location", never a live one.
class PLR_API LocationGeneration {
 public:
  static constexpr std::uint64_t kFirstPublished = 1;

  constexpr LocationGeneration() noexcept = default;
  explicit constexpr LocationGeneration(std::uint64_t value) noexcept : value_(value) {}

  constexpr std::uint64_t value() const noexcept { return value_; }
  constexpr bool published() const noexcept { return value_ != 0; }

  /// Strictly increasing successor. Overflow is reported, never wrapped.
  Result<LocationGeneration> next() const {
    if (value_ == UINT64_MAX) {
      return Error(ErrorCode::GenerationOverflow, "location generation counter exhausted");
    }
    return LocationGeneration(value_ + 1);
  }

  friend constexpr bool operator==(const LocationGeneration&, const LocationGeneration&) noexcept = default;
  friend constexpr std::strong_ordering operator<=>(const LocationGeneration& lhs,
                                                    const LocationGeneration& rhs) noexcept = default;

 private:
  std::uint64_t value_ = 0;
};

/// Monotonic revision of the committed registry state.
///
/// Revision 0 is the state of a store immediately after creation (no locations
/// yet). Every committed mutation advances the revision by exactly one and is
/// published durably before the mutation reports success. Independent of
/// LocationGeneration: a revision identifies a whole committed state, a
/// generation identifies one location inside it.
class PLR_API LocationRevision {
 public:
  constexpr LocationRevision() noexcept = default;
  explicit constexpr LocationRevision(std::uint64_t value) noexcept : value_(value) {}

  constexpr std::uint64_t value() const noexcept { return value_; }

  Result<LocationRevision> next() const {
    if (value_ == UINT64_MAX) {
      return Error(ErrorCode::RevisionOverflow, "registry revision counter exhausted");
    }
    return LocationRevision(value_ + 1);
  }

  friend constexpr bool operator==(const LocationRevision&, const LocationRevision&) noexcept = default;
  friend constexpr std::strong_ordering operator<=>(const LocationRevision& lhs,
                                                    const LocationRevision& rhs) noexcept = default;

 private:
  std::uint64_t value_ = 0;
};

/// Monotonic counter of published state generations in a store.
///
/// Advances on every durable publication, including publications that only
/// change writer authority (a writer incarnation) and not the location state.
/// Revision answers "what is the authoritative content", state sequence answers
/// "which publication is this"; they are intentionally different counters.
class PLR_API StateSequence {
 public:
  static constexpr std::uint64_t kFirstPublication = 1;

  constexpr StateSequence() noexcept = default;
  explicit constexpr StateSequence(std::uint64_t value) noexcept : value_(value) {}

  constexpr std::uint64_t value() const noexcept { return value_; }

  Result<StateSequence> next() const {
    if (value_ == UINT64_MAX) {
      return Error(ErrorCode::SequenceOverflow, "state sequence counter exhausted");
    }
    return StateSequence(value_ + 1);
  }

  friend constexpr bool operator==(const StateSequence&, const StateSequence&) noexcept = default;
  friend constexpr std::strong_ordering operator<=>(const StateSequence& lhs,
                                                    const StateSequence& rhs) noexcept = default;

 private:
  std::uint64_t value_ = 0;
};

/// Durable mutation-authority epoch of a writer incarnation.
///
/// Every store session that opens for mutation advances the epoch and publishes
/// it before any mutation is authorized. An operation whose authority carries an
/// older epoch is rejected instead of being applied to state the caller no
/// longer owns. Epoch 0 means "no writer has ever opened this store".
class PLR_API WriterEpoch {
 public:
  constexpr WriterEpoch() noexcept = default;
  explicit constexpr WriterEpoch(std::uint64_t value) noexcept : value_(value) {}

  constexpr std::uint64_t value() const noexcept { return value_; }
  constexpr bool valid() const noexcept { return value_ != 0; }

  Result<WriterEpoch> next() const {
    if (value_ == UINT64_MAX) {
      return Error(ErrorCode::EpochOverflow, "writer epoch counter exhausted");
    }
    return WriterEpoch(value_ + 1);
  }

  friend constexpr bool operator==(const WriterEpoch&, const WriterEpoch&) noexcept = default;
  friend constexpr std::strong_ordering operator<=>(const WriterEpoch& lhs,
                                                    const WriterEpoch& rhs) noexcept = default;

 private:
  std::uint64_t value_ = 0;
};

}  // namespace dccp::physical_location_registry

namespace std {
template <class Tag>
struct hash<dccp::physical_location_registry::StrongId<Tag>> {
  std::size_t operator()(const dccp::physical_location_registry::StrongId<Tag>& id) const noexcept {
    return std::hash<std::string_view>{}(id.value());
  }
};
template <>
struct hash<dccp::physical_location_registry::LocationGeneration> {
  std::size_t operator()(const dccp::physical_location_registry::LocationGeneration& value) const noexcept {
    return std::hash<std::uint64_t>{}(value.value());
  }
};
template <>
struct hash<dccp::physical_location_registry::LocationRevision> {
  std::size_t operator()(const dccp::physical_location_registry::LocationRevision& value) const noexcept {
    return std::hash<std::uint64_t>{}(value.value());
  }
};
template <>
struct hash<dccp::physical_location_registry::StateSequence> {
  std::size_t operator()(const dccp::physical_location_registry::StateSequence& value) const noexcept {
    return std::hash<std::uint64_t>{}(value.value());
  }
};
template <>
struct hash<dccp::physical_location_registry::WriterEpoch> {
  std::size_t operator()(const dccp::physical_location_registry::WriterEpoch& value) const noexcept {
    return std::hash<std::uint64_t>{}(value.value());
  }
};
}  // namespace std

#endif  // DCCP_PHYSICAL_LOCATION_REGISTRY_STRONG_ID_HPP
