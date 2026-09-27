// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#ifndef DCCP_PHYSICAL_LOCATION_REGISTRY_RESULT_HPP
#define DCCP_PHYSICAL_LOCATION_REGISTRY_RESULT_HPP

#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "dccp/physical_location_registry/export.hpp"

namespace dccp::physical_location_registry {

/// Stable, machine-readable outcome codes.
///
/// These codes are part of the public contract: values are appended to, never
/// renumbered or repurposed, and the textual name returned by error_code_name()
/// is equally stable. Consumers may branch on the code; the accompanying
/// message is human-facing and may be improved without notice.
enum class ErrorCode : std::uint16_t {
  Ok = 0,

  // Input shape and encoding (untrusted data).
  InvalidArgument,
  MalformedIdentifier,
  IdentifierTooLong,
  MalformedAddressComponent,
  AddressComponentTooLong,
  MalformedPath,
  PathTooDeep,
  PathTooLong,
  InvalidUtf8,
  MalformedLabel,
  LabelTooLong,
  MalformedText,
  UnknownEnumToken,
  MalformedRecord,
  UnsupportedSchemaVersion,
  UnsupportedFormatFlag,
  TruncatedInput,
  CountMismatch,
  DigestMismatch,
  LimitExceeded,
  LimitsMismatch,

  // Structure and semantics of the location hierarchy.
  NotFound,
  AlreadyPresent,
  IdentityConflict,
  InvalidKindForParent,
  KindMustNotHaveParent,
  KindMustHaveParent,
  ContainmentCycle,
  LocationHasChildren,
  LocationHasActiveChildren,
  ParentNotActive,
  AddressInUse,
  AddressLookAlike,
  AliasConflict,
  AliasConflictsWithAddress,
  AliasRedundant,
  AliasNotFound,
  RackUnitNotAllowed,
  RackUnitConflict,
  RackUnitOutOfEnvelope,
  RackEnvelopeInvalid,
  SelfMove,
  MoveIntoDescendant,
  LifecycleTransitionIllegal,
  ReplacementCycle,
  ReplacementTargetExists,
  PredecessorAlreadyReplaced,

  // Authority, generations and idempotency.
  StaleGeneration,
  StaleRevision,
  StaleAuthorityEpoch,
  StoreMismatch,
  SessionReadOnly,
  SessionClosed,
  GenerationOverflow,
  RevisionOverflow,
  SequenceOverflow,
  EpochOverflow,
  OperationIdConflict,
  RevisionNotRetained,

  // Persistence, integrity and recovery.
  StoreNotFound,
  StoreExists,
  StoreNotEmpty,
  StoreLocked,
  StoreCorrupt,
  HeadMissing,
  HeadCorrupt,
  RecoveryRequired,
  RecoveryUnavailable,
  RecoveryNotNeeded,
  IntegrityFailure,
  IoError,
  InjectedFault,

  // Execution.
  TraversalDepthExceeded,
  Cancelled,
  InternalError,

  // A request that describes no change at all. Appended rather than inserted so
  // that every code already published keeps its numeric value.
  NoOpMutation,
};

/// Coarse classification of an ErrorCode, for callers that branch on the kind
/// of failure rather than the exact code.
enum class ErrorCategory : std::uint8_t {
  Ok = 0,
  Argument,     // caller-supplied or untrusted input was rejected
  Structure,    // the location hierarchy would become invalid
  Authority,    // generation, revision or writer-epoch precondition failed
  Persistence,  // durable state is missing, corrupt, locked or unwritable
  Lifecycle,    // a documented lifecycle transition was not legal
  Limit,        // a configured bound would be exceeded
  Cancelled,    // the operation was cancelled before publication
  Internal,     // defect in the library
};

/// Stable textual name of an error code (upper snake case).
PLR_API std::string_view error_code_name(ErrorCode code) noexcept;

/// Category of an error code.
PLR_API ErrorCategory error_category(ErrorCode code) noexcept;

/// Human-readable name of a category.
PLR_API std::string_view error_category_name(ErrorCategory category) noexcept;

/// True when re-issuing the exact same operation after a refresh can succeed.
///
/// Retryable codes describe a precondition that a well-behaved caller can
/// repair (refreshing a stale generation or revision, waiting for another
/// writer to release the store lock). A retryable code never means "the
/// operation may have been applied": non-publication is the whole point of
/// these outcomes, and every committed mutation reports success.
PLR_API bool error_code_is_retryable(ErrorCode code) noexcept;

/// An error value: stable code, human explanation, and optional subject.
class PLR_API Error {
 public:
  Error() noexcept = default;

  Error(ErrorCode code, std::string message) : code_(code), message_(std::move(message)) {}

  ErrorCode code() const noexcept { return code_; }
  ErrorCategory category() const noexcept { return error_category(code_); }
  const std::string& message() const noexcept { return message_; }

  /// Identifier, path or text the error is about, when one exists.
  const std::string& subject() const noexcept { return subject_; }

  Error& with_subject(std::string subject) {
    subject_ = std::move(subject);
    return *this;
  }

  bool ok() const noexcept { return code_ == ErrorCode::Ok; }

  /// "CODE: message" (plus " [subject=...]" when a subject is present).
  std::string to_string() const;

 private:
  ErrorCode code_ = ErrorCode::Ok;
  std::string message_;
  std::string subject_;
};

/// Result of an operation that yields a value of type T or an Error.
///
/// The library never uses exceptions for expected failure modes; Result is the
/// only channel for them. value() throws std::logic_error only on programmer
/// error (dereferencing a failed Result).
template <class T>
class Result {
 public:
  Result(T value) : value_(std::move(value)) {}
  Result(Error error) : error_(normalize(std::move(error))) {}

  bool has_value() const noexcept { return value_.has_value(); }
  explicit operator bool() const noexcept { return has_value(); }

  T& value() & {
    require_value();
    return *value_;
  }
  const T& value() const& {
    require_value();
    return *value_;
  }
  T&& value() && {
    require_value();
    return std::move(*value_);
  }

  T& operator*() & { return value(); }
  const T& operator*() const& { return value(); }
  T* operator->() { return &value(); }
  const T* operator->() const { return &value(); }

  const Error& error() const noexcept { return error_; }

 private:
  static Error normalize(Error error) {
    if (error.ok()) {
      return Error(ErrorCode::InternalError, "result constructed without a value or an error");
    }
    return error;
  }

  void require_value() const {
    if (!value_.has_value()) {
      throw std::logic_error("physical_location_registry: Result has no value: " + error_.to_string());
    }
  }

  std::optional<T> value_;
  Error error_;
};

/// Result specialization for operations that produce no value.
template <>
class Result<void> {
 public:
  Result() noexcept = default;
  Result(Error error) : error_(normalize(std::move(error))) {}

  static Result success() noexcept { return Result(); }

  bool has_value() const noexcept { return error_.ok(); }
  explicit operator bool() const noexcept { return has_value(); }

  const Error& error() const noexcept { return error_; }

 private:
  static Error normalize(Error error) {
    if (error.ok()) {
      return Error(ErrorCode::InternalError, "result constructed without a value or an error");
    }
    return error;
  }

  Error error_;
};

/// Convenience constructors.
inline Error make_error(ErrorCode code, std::string message) { return Error(code, std::move(message)); }

inline Result<void> ok() noexcept { return Result<void>(); }

}  // namespace dccp::physical_location_registry

/// Propagate a failed Result out of the current function.
///
/// PLR_TRY(name, expression) declares "name" bound to the successful value and
/// returns the error if the expression failed.
#define PLR_TRY(value_name, expression)         \
  auto value_name##_plr_result = (expression);  \
  if (!value_name##_plr_result.has_value()) {   \
    return value_name##_plr_result.error();     \
  }                                             \
  auto& value_name = *value_name##_plr_result

/// Propagate a failed Result<void> out of the current function.
#define PLR_CHECK(expression)                  \
  do {                                         \
    auto plr_check_result = (expression);      \
    if (!plr_check_result.has_value()) {       \
      return plr_check_result.error();         \
    }                                          \
  } while (false)

#endif  // DCCP_PHYSICAL_LOCATION_REGISTRY_RESULT_HPP
