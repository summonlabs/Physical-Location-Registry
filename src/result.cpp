// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "dccp/physical_location_registry/result.hpp"

#include <cstddef>

namespace dccp::physical_location_registry {
namespace {

struct ErrorCodeEntry {
  ErrorCode code;
  std::string_view name;
};

// The single authority mapping codes to their stable textual names. Codes are
// appended, never renumbered, so this table is append-only as well.
constexpr ErrorCodeEntry kErrorCodeNames[] = {
    {ErrorCode::Ok, "OK"},
    {ErrorCode::InvalidArgument, "INVALID_ARGUMENT"},
    {ErrorCode::MalformedIdentifier, "MALFORMED_IDENTIFIER"},
    {ErrorCode::IdentifierTooLong, "IDENTIFIER_TOO_LONG"},
    {ErrorCode::MalformedAddressComponent, "MALFORMED_ADDRESS_COMPONENT"},
    {ErrorCode::AddressComponentTooLong, "ADDRESS_COMPONENT_TOO_LONG"},
    {ErrorCode::MalformedPath, "MALFORMED_PATH"},
    {ErrorCode::PathTooDeep, "PATH_TOO_DEEP"},
    {ErrorCode::PathTooLong, "PATH_TOO_LONG"},
    {ErrorCode::InvalidUtf8, "INVALID_UTF8"},
    {ErrorCode::MalformedLabel, "MALFORMED_LABEL"},
    {ErrorCode::LabelTooLong, "LABEL_TOO_LONG"},
    {ErrorCode::MalformedText, "MALFORMED_TEXT"},
    {ErrorCode::UnknownEnumToken, "UNKNOWN_ENUM_TOKEN"},
    {ErrorCode::MalformedRecord, "MALFORMED_RECORD"},
    {ErrorCode::UnsupportedSchemaVersion, "UNSUPPORTED_SCHEMA_VERSION"},
    {ErrorCode::UnsupportedFormatFlag, "UNSUPPORTED_FORMAT_FLAG"},
    {ErrorCode::TruncatedInput, "TRUNCATED_INPUT"},
    {ErrorCode::CountMismatch, "COUNT_MISMATCH"},
    {ErrorCode::DigestMismatch, "DIGEST_MISMATCH"},
    {ErrorCode::LimitExceeded, "LIMIT_EXCEEDED"},
    {ErrorCode::LimitsMismatch, "LIMITS_MISMATCH"},
    {ErrorCode::NotFound, "NOT_FOUND"},
    {ErrorCode::AlreadyPresent, "ALREADY_PRESENT"},
    {ErrorCode::IdentityConflict, "IDENTITY_CONFLICT"},
    {ErrorCode::InvalidKindForParent, "INVALID_KIND_FOR_PARENT"},
    {ErrorCode::KindMustNotHaveParent, "KIND_MUST_NOT_HAVE_PARENT"},
    {ErrorCode::KindMustHaveParent, "KIND_MUST_HAVE_PARENT"},
    {ErrorCode::ContainmentCycle, "CONTAINMENT_CYCLE"},
    {ErrorCode::LocationHasChildren, "LOCATION_HAS_CHILDREN"},
    {ErrorCode::LocationHasActiveChildren, "LOCATION_HAS_ACTIVE_CHILDREN"},
    {ErrorCode::ParentNotActive, "PARENT_NOT_ACTIVE"},
    {ErrorCode::AddressInUse, "ADDRESS_IN_USE"},
    {ErrorCode::AddressLookAlike, "ADDRESS_LOOK_ALIKE"},
    {ErrorCode::AliasConflict, "ALIAS_CONFLICT"},
    {ErrorCode::AliasConflictsWithAddress, "ALIAS_CONFLICTS_WITH_ADDRESS"},
    {ErrorCode::AliasRedundant, "ALIAS_REDUNDANT"},
    {ErrorCode::AliasNotFound, "ALIAS_NOT_FOUND"},
    {ErrorCode::RackUnitNotAllowed, "RACK_UNIT_NOT_ALLOWED"},
    {ErrorCode::RackUnitConflict, "RACK_UNIT_CONFLICT"},
    {ErrorCode::RackUnitOutOfEnvelope, "RACK_UNIT_OUT_OF_ENVELOPE"},
    {ErrorCode::RackEnvelopeInvalid, "RACK_ENVELOPE_INVALID"},
    {ErrorCode::SelfMove, "SELF_MOVE"},
    {ErrorCode::MoveIntoDescendant, "MOVE_INTO_DESCENDANT"},
    {ErrorCode::LifecycleTransitionIllegal, "LIFECYCLE_TRANSITION_ILLEGAL"},
    {ErrorCode::ReplacementCycle, "REPLACEMENT_CYCLE"},
    {ErrorCode::ReplacementTargetExists, "REPLACEMENT_TARGET_EXISTS"},
    {ErrorCode::PredecessorAlreadyReplaced, "PREDECESSOR_ALREADY_REPLACED"},
    {ErrorCode::StaleGeneration, "STALE_GENERATION"},
    {ErrorCode::StaleRevision, "STALE_REVISION"},
    {ErrorCode::StaleAuthorityEpoch, "STALE_AUTHORITY_EPOCH"},
    {ErrorCode::StoreMismatch, "STORE_MISMATCH"},
    {ErrorCode::SessionReadOnly, "SESSION_READ_ONLY"},
    {ErrorCode::SessionClosed, "SESSION_CLOSED"},
    {ErrorCode::GenerationOverflow, "GENERATION_OVERFLOW"},
    {ErrorCode::RevisionOverflow, "REVISION_OVERFLOW"},
    {ErrorCode::SequenceOverflow, "SEQUENCE_OVERFLOW"},
    {ErrorCode::EpochOverflow, "EPOCH_OVERFLOW"},
    {ErrorCode::OperationIdConflict, "OPERATION_ID_CONFLICT"},
    {ErrorCode::RevisionNotRetained, "REVISION_NOT_RETAINED"},
    {ErrorCode::StoreNotFound, "STORE_NOT_FOUND"},
    {ErrorCode::StoreExists, "STORE_EXISTS"},
    {ErrorCode::StoreNotEmpty, "STORE_NOT_EMPTY"},
    {ErrorCode::StoreLocked, "STORE_LOCKED"},
    {ErrorCode::StoreCorrupt, "STORE_CORRUPT"},
    {ErrorCode::HeadMissing, "HEAD_MISSING"},
    {ErrorCode::HeadCorrupt, "HEAD_CORRUPT"},
    {ErrorCode::RecoveryRequired, "RECOVERY_REQUIRED"},
    {ErrorCode::RecoveryUnavailable, "RECOVERY_UNAVAILABLE"},
    {ErrorCode::RecoveryNotNeeded, "RECOVERY_NOT_NEEDED"},
    {ErrorCode::IntegrityFailure, "INTEGRITY_FAILURE"},
    {ErrorCode::IoError, "IO_ERROR"},
    {ErrorCode::InjectedFault, "INJECTED_FAULT"},
    {ErrorCode::TraversalDepthExceeded, "TRAVERSAL_DEPTH_EXCEEDED"},
    {ErrorCode::Cancelled, "CANCELLED"},
    {ErrorCode::InternalError, "INTERNAL_ERROR"},
    {ErrorCode::NoOpMutation, "NO_OP_MUTATION"},
};

std::string_view name_for(ErrorCode code) noexcept {
  for (const ErrorCodeEntry& entry : kErrorCodeNames) {
    if (entry.code == code) {
      return entry.name;
    }
  }
  return "UNRECOGNIZED_CODE";
}

}  // namespace

std::string_view error_code_name(ErrorCode code) noexcept { return name_for(code); }

ErrorCategory error_category(ErrorCode code) noexcept {
  switch (code) {
    case ErrorCode::Ok:
      return ErrorCategory::Ok;

    case ErrorCode::InvalidArgument:
    case ErrorCode::MalformedIdentifier:
    case ErrorCode::IdentifierTooLong:
    case ErrorCode::MalformedAddressComponent:
    case ErrorCode::AddressComponentTooLong:
    case ErrorCode::MalformedPath:
    case ErrorCode::PathTooDeep:
    case ErrorCode::PathTooLong:
    case ErrorCode::InvalidUtf8:
    case ErrorCode::MalformedLabel:
    case ErrorCode::LabelTooLong:
    case ErrorCode::MalformedText:
    case ErrorCode::UnknownEnumToken:
    case ErrorCode::MalformedRecord:
    case ErrorCode::UnsupportedSchemaVersion:
    case ErrorCode::UnsupportedFormatFlag:
    case ErrorCode::TruncatedInput:
    case ErrorCode::CountMismatch:
    case ErrorCode::DigestMismatch:
      return ErrorCategory::Argument;

    case ErrorCode::LimitExceeded:
    case ErrorCode::LimitsMismatch:
    case ErrorCode::TraversalDepthExceeded:
      return ErrorCategory::Limit;

    case ErrorCode::NotFound:
    case ErrorCode::AlreadyPresent:
    case ErrorCode::IdentityConflict:
    case ErrorCode::InvalidKindForParent:
    case ErrorCode::KindMustNotHaveParent:
    case ErrorCode::KindMustHaveParent:
    case ErrorCode::ContainmentCycle:
    case ErrorCode::LocationHasChildren:
    case ErrorCode::LocationHasActiveChildren:
    case ErrorCode::ParentNotActive:
    case ErrorCode::AddressInUse:
    case ErrorCode::AddressLookAlike:
    case ErrorCode::AliasConflict:
    case ErrorCode::AliasConflictsWithAddress:
    case ErrorCode::AliasRedundant:
    case ErrorCode::AliasNotFound:
    case ErrorCode::RackUnitNotAllowed:
    case ErrorCode::RackUnitConflict:
    case ErrorCode::RackUnitOutOfEnvelope:
    case ErrorCode::RackEnvelopeInvalid:
    case ErrorCode::SelfMove:
    case ErrorCode::MoveIntoDescendant:
    case ErrorCode::ReplacementCycle:
    case ErrorCode::ReplacementTargetExists:
    case ErrorCode::PredecessorAlreadyReplaced:
    case ErrorCode::NoOpMutation:
      return ErrorCategory::Structure;

    case ErrorCode::LifecycleTransitionIllegal:
    case ErrorCode::SessionReadOnly:
    case ErrorCode::SessionClosed:
      return ErrorCategory::Lifecycle;

    case ErrorCode::StaleGeneration:
    case ErrorCode::StaleRevision:
    case ErrorCode::StaleAuthorityEpoch:
    case ErrorCode::StoreMismatch:
    case ErrorCode::GenerationOverflow:
    case ErrorCode::RevisionOverflow:
    case ErrorCode::SequenceOverflow:
    case ErrorCode::EpochOverflow:
    case ErrorCode::OperationIdConflict:
    case ErrorCode::RevisionNotRetained:
      return ErrorCategory::Authority;

    case ErrorCode::StoreNotFound:
    case ErrorCode::StoreExists:
    case ErrorCode::StoreNotEmpty:
    case ErrorCode::StoreLocked:
    case ErrorCode::StoreCorrupt:
    case ErrorCode::HeadMissing:
    case ErrorCode::HeadCorrupt:
    case ErrorCode::RecoveryRequired:
    case ErrorCode::RecoveryUnavailable:
    case ErrorCode::RecoveryNotNeeded:
    case ErrorCode::IntegrityFailure:
    case ErrorCode::IoError:
    case ErrorCode::InjectedFault:
      return ErrorCategory::Persistence;

    case ErrorCode::Cancelled:
      return ErrorCategory::Cancelled;

    case ErrorCode::InternalError:
      return ErrorCategory::Internal;
  }
  return ErrorCategory::Internal;
}

std::string_view error_category_name(ErrorCategory category) noexcept {
  switch (category) {
    case ErrorCategory::Ok:
      return "ok";
    case ErrorCategory::Argument:
      return "argument";
    case ErrorCategory::Structure:
      return "structure";
    case ErrorCategory::Authority:
      return "authority";
    case ErrorCategory::Persistence:
      return "persistence";
    case ErrorCategory::Lifecycle:
      return "lifecycle";
    case ErrorCategory::Limit:
      return "limit";
    case ErrorCategory::Cancelled:
      return "cancelled";
    case ErrorCategory::Internal:
      return "internal";
  }
  return "internal";
}

bool error_code_is_retryable(ErrorCode code) noexcept {
  switch (code) {
    case ErrorCode::StaleGeneration:
    case ErrorCode::StaleRevision:
    case ErrorCode::StaleAuthorityEpoch:
    case ErrorCode::StoreLocked:
      return true;
    default:
      return false;
  }
}

std::string Error::to_string() const {
  std::string text(error_code_name(code_));
  text.append(": ");
  text.append(message_);
  if (!subject_.empty()) {
    text.append(" [subject=");
    text.append(subject_);
    text.append("]");
  }
  return text;
}

}  // namespace dccp::physical_location_registry
