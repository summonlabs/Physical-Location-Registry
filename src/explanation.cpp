// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "dccp/physical_location_registry/explanation.hpp"

#include <string>

namespace dccp::physical_location_registry {
namespace {

struct CodeHelp {
  std::string_view summary;
  std::string_view hint;
};

CodeHelp help_for(ErrorCode code) {
  switch (code) {
    case ErrorCode::Ok:
      return {"the operation is accepted", ""};
    case ErrorCode::InvalidArgument:
      return {"an argument was rejected because it cannot describe a valid operation",
              "check the argument against the documented grammar and range"};
    case ErrorCode::MalformedIdentifier:
      return {"an identity does not match the canonical identifier grammar",
              "identities are 1..128 bytes of ASCII letters, digits, '.', ':' and '-', starting and "
              "ending with a letter or digit"};
    case ErrorCode::IdentifierTooLong:
      return {"an identity is longer than the permitted maximum",
              "shorten the identity to at most 128 bytes"};
    case ErrorCode::MalformedAddressComponent:
      return {"an address component does not match the canonical grammar",
              "components are 1..64 bytes, start and end with an ASCII letter or digit, and contain "
              "only letters, digits, '.', '_' and '-'"};
    case ErrorCode::AddressComponentTooLong:
      return {"an address component is longer than the configured maximum",
              "shorten the component or raise max_address_component_bytes when creating the store"};
    case ErrorCode::MalformedPath:
      return {"an address is not a canonical absolute path",
              "addresses are absolute: a leading '/' followed by components joined with single '/'"};
    case ErrorCode::PathTooDeep:
      return {"the address hierarchy would be deeper than the configured maximum",
              "raise max_depth when creating the store, or restructure the hierarchy"};
    case ErrorCode::PathTooLong:
      return {"the canonical address would be longer than the configured maximum",
              "raise max_path_bytes when creating the store, or shorten component names"};
    case ErrorCode::InvalidUtf8:
      return {"text is not valid UTF-8", "re-encode the text as UTF-8 without overlong forms"};
    case ErrorCode::MalformedLabel:
      return {"a label is not acceptable display text",
              "labels must be valid UTF-8 without control characters or noncharacters"};
    case ErrorCode::LabelTooLong:
      return {"a label is longer than the configured maximum",
              "shorten the label or raise max_label_bytes when creating the store"};
    case ErrorCode::MalformedText:
      return {"recorded text is malformed or too long", "supply valid UTF-8 without control characters"};
    case ErrorCode::UnknownEnumToken:
      return {"an enumerated token is not part of the domain", "use one of the documented names"};
    case ErrorCode::MalformedRecord:
      return {"stored state is not structurally valid", "restore the store from a verified publication"};
    case ErrorCode::UnsupportedSchemaVersion:
      return {"the state was written by an incompatible version of this library",
              "use a build whose snapshot format version matches, or re-export from the writer"};
    case ErrorCode::UnsupportedFormatFlag:
      return {"the state sets format flags this build does not understand",
              "use a build that understands those flags"};
    case ErrorCode::TruncatedInput:
      return {"the input ends before the declared structure does",
              "the publication is incomplete; restore it from a verified copy"};
    case ErrorCode::CountMismatch:
      return {"declared counts and actual contents disagree", "the publication is corrupt"};
    case ErrorCode::DigestMismatch:
      return {"the integrity digest does not match the contents",
              "the publication is corrupt and must not be trusted"};
    case ErrorCode::LimitExceeded:
      return {"a configured bound would be exceeded",
              "raise the bound when creating the store, or reduce the request"};
    case ErrorCode::LimitsMismatch:
      return {"the requested limits differ from the limits the store was created with",
              "open the store without requesting different limits"};
    case ErrorCode::NotFound:
      return {"the referenced location or binding does not exist",
              "check the identity or address, and note that retired locations resolve only when "
              "asked for explicitly"};
    case ErrorCode::AlreadyPresent:
      return {"the referenced identity or value already exists",
              "use the existing location, or choose a different identity"};
    case ErrorCode::IdentityConflict:
      return {"an identity is already used by a different location",
              "identities are stable and unique: choose a new one"};
    case ErrorCode::InvalidKindForParent:
      return {"the containment schema does not allow this kind under that parent",
              "consult the containment table; for example a rack unit belongs under a rack"};
    case ErrorCode::KindMustNotHaveParent:
      return {"this kind of location cannot be contained", "a facility is created without a parent"};
    case ErrorCode::KindMustHaveParent:
      return {"this kind of location must be contained by a parent", "name an existing parent"};
    case ErrorCode::ContainmentCycle:
      return {"the operation would create a containment cycle",
              "a location cannot contain itself directly or indirectly"};
    case ErrorCode::LocationHasChildren:
      return {"the location still contains other locations",
              "retire, move or replace the children first"};
    case ErrorCode::LocationHasActiveChildren:
      return {"the location still has active descendants",
              "retire the subtree, or retire children before their parent"};
    case ErrorCode::ParentNotActive:
      return {"the parent is not active", "reactivate the parent, then repeat the operation"};
    case ErrorCode::AddressInUse:
      return {"another location already holds this address",
              "choose a different component, or move the existing location first"};
    case ErrorCode::AddressLookAlike:
      return {"another address differs from this one only by ASCII letter case",
              "addresses must not be case-insensitive look-alikes"};
    case ErrorCode::AliasConflict:
      return {"the alias is already bound to another location",
              "remove the existing binding first, or choose a different alias"};
    case ErrorCode::AliasConflictsWithAddress:
      return {"the alias is the current address of another location, or a new address is claimed by "
              "an existing alias",
              "aliases must never be ambiguous with a current address"};
    case ErrorCode::AliasRedundant:
      return {"the alias duplicates the location's own canonical address",
              "an alias is only useful when it differs from the current address"};
    case ErrorCode::AliasNotFound:
      return {"the location has no such alias bound", "list the aliases before removing one"};
    case ErrorCode::RackUnitNotAllowed:
      return {"this kind of location cannot carry a rack-unit coordinate",
              "only rack-unit locations carry a unit coordinate"};
    case ErrorCode::RackUnitConflict:
      return {"another rack unit under the same rack already holds this coordinate",
              "each rack-unit coordinate is one addressable place"};
    case ErrorCode::RackUnitOutOfEnvelope:
      return {"the rack-unit coordinate is outside the rack's declared envelope",
              "widen the rack envelope, or choose a coordinate inside it"};
    case ErrorCode::RackEnvelopeInvalid:
      return {"the rack envelope is not a valid coordinate range",
              "an envelope starts at a unit coordinate and has a positive height within the maximum"};
    case ErrorCode::SelfMove:
      return {"a location cannot be moved under itself", "choose a different parent"};
    case ErrorCode::MoveIntoDescendant:
      return {"a location cannot be moved into its own subtree", "choose a parent outside the subtree"};
    case ErrorCode::LifecycleTransitionIllegal:
      return {"the requested lifecycle transition is not legal from the current state",
              "active locations can be retired or replaced, retired locations reactivated or "
              "replaced, and replaced locations are terminal"};
    case ErrorCode::ReplacementCycle:
      return {"replacement lineage would become cyclic",
              "a successor is always a new location; lineage only ever points forward"};
    case ErrorCode::ReplacementTargetExists:
      return {"the successor identity already exists",
              "a replacement creates a new location; choose an unused identity"};
    case ErrorCode::PredecessorAlreadyReplaced:
      return {"the predecessor has already been replaced", "a location can be replaced only once"};
    case ErrorCode::StaleGeneration:
      return {"the expected generation does not match the location's current generation",
              "re-read the location and retry with the current generation"};
    case ErrorCode::StaleRevision:
      return {"the expected revision does not match the committed revision",
              "re-read the registry and retry with the current revision"};
    case ErrorCode::StaleAuthorityEpoch:
      return {"the mutation carries authority from a superseded writer session",
              "reopen the store and use the authority of the current session"};
    case ErrorCode::StoreMismatch:
      return {"the value belongs to a different store", "use the identity of the open store"};
    case ErrorCode::SessionReadOnly:
      return {"this session was opened read-only", "reopen the store for writing"};
    case ErrorCode::SessionClosed:
      return {"this session has been closed", "reopen the store to continue"};
    case ErrorCode::GenerationOverflow:
      return {"the location generation counter is exhausted",
              "replace the location rather than mutating it again"};
    case ErrorCode::RevisionOverflow:
      return {"the registry revision counter is exhausted", "create a new store"};
    case ErrorCode::SequenceOverflow:
      return {"the publication sequence counter is exhausted", "create a new store"};
    case ErrorCode::EpochOverflow:
      return {"the writer epoch counter is exhausted", "create a new store"};
    case ErrorCode::OperationIdConflict:
      return {"this operation id was already used for a different request",
              "operation ids identify one logical operation; use a new id for a new request"};
    case ErrorCode::RevisionNotRetained:
      return {"the requested revision is outside the retained diff window",
              "compare published state files with the snapshot decode and diff operations instead"};
    case ErrorCode::StoreNotFound:
      return {"no store exists at this path", "create the store, or check the directory"};
    case ErrorCode::StoreExists:
      return {"a store already exists at this path", "open it instead of creating it"};
    case ErrorCode::StoreNotEmpty:
      return {"the directory is not empty enough to host a new store",
              "choose an empty directory"};
    case ErrorCode::StoreLocked:
      return {"another writer session owns this store",
              "retry once the other writer closes, or open read-only"};
    case ErrorCode::StoreCorrupt:
      return {"the store contents are corrupt", "recover from a verified publication"};
    case ErrorCode::HeadMissing:
      return {"the publication pointer is missing", "recover the store to republish it"};
    case ErrorCode::HeadCorrupt:
      return {"the publication pointer is unusable", "recover the store to republish it"};
    case ErrorCode::RecoveryRequired:
      return {"the store needs recovery before it can be used", "run recovery on a writer session"};
    case ErrorCode::RecoveryUnavailable:
      return {"no publication in the store passed verification",
              "restore the store from a backup; unverified state is never presented as authoritative"};
    case ErrorCode::RecoveryNotNeeded:
      return {"the store did not need recovery", "no action required"};
    case ErrorCode::IntegrityFailure:
      return {"a publication failed integrity verification",
              "the previous known-good publication remains authoritative"};
    case ErrorCode::IoError:
      return {"an operating-system file operation failed", "check permissions, free space and paths"};
    case ErrorCode::InjectedFault:
      return {"a durability fault was injected at the configured publication stage",
              "this outcome is produced only by an explicit fault plan"};
    case ErrorCode::TraversalDepthExceeded:
      return {"a hierarchy walk exceeded its bound",
              "the state contains a cycle or a chain deeper than the configured maximum"};
    case ErrorCode::Cancelled:
      return {"the operation was cancelled before publication", "nothing was published"};
    case ErrorCode::NoOpMutation:
      return {"the request describes no change to the committed state",
              "the committed state already matches the request, so nothing was published"};

    case ErrorCode::InternalError:
      return {"the library detected an internal inconsistency",
              "this is a defect: report it with the accompanying detail"};
  }
  return {"unrecognized outcome", ""};
}

}  // namespace

Explanation explain_error_code(ErrorCode code) {
  const CodeHelp help = help_for(code);
  Explanation explanation;
  explanation.code = code;
  explanation.category = error_category(code);
  explanation.summary = std::string(help.summary);
  if (!help.hint.empty()) {
    explanation.hints.emplace_back(help.hint);
  }
  return explanation;
}

Explanation explain_error(const Error& error) {
  Explanation explanation = explain_error_code(error.code());
  explanation.summary = error.to_string();
  if (!error.subject().empty()) {
    explanation.details.push_back("subject: " + error.subject());
  }
  return explanation;
}

std::string Explanation::to_string() const {
  std::string text(error_code_name(code));
  text.append(" (");
  text.append(error_category_name(category));
  text.append("): ");
  text.append(summary);
  for (const std::string& detail : details) {
    text.append("\ndetail: ");
    text.append(detail);
  }
  for (const std::string& hint : hints) {
    text.append("\nhint: ");
    text.append(hint);
  }
  return text;
}

}  // namespace dccp::physical_location_registry
