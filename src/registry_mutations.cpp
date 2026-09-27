// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// The mutation engine. Every mutation follows the same shape:
//
//   session check -> context check -> authority check -> idempotent replay
//   -> structural validation (const, no side effects) -> revision precondition
//   -> cancellation check -> apply -> bounds check -> durable publication
//   -> rollback on failure, journal entry on success
//
// Validation completes before the first byte of authoritative state changes, and
// applied changes are kept only when the durable publication succeeded.

#include <algorithm>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "byte_codec.hpp"
#include "dccp/physical_location_registry/digest.hpp"
#include "dccp/physical_location_registry/text.hpp"
#include "registry_internal.hpp"

namespace dccp::physical_location_registry {
namespace {

using internal::ByteWriter;

// ---------------------------------------------------------------------------
// Undo journal
// ---------------------------------------------------------------------------

/// Captures just enough state to put the model back the way it was if the
/// durable publication fails. Rollback is rare, so it may rebuild the derived
/// indexes from the records.
class MutationUndo {
 public:
  explicit MutationUndo(Snapshot::Impl& model)
      : model_(model),
        revision_(model.revision),
        sequence_(model.sequence),
        receipts_(model.receipts),
        replacements_(model.replacements),
        total_moves_(model.total_moves),
        total_aliases_(model.total_aliases) {}

  /// Records the current value of a location before it is first modified.
  void capture(const LocationId& id) {
    if (captured_.find(id) != captured_.end()) {
      return;
    }
    const LocationRecord* existing = model_.find(id);
    captured_.emplace(id, existing == nullptr ? std::optional<LocationRecord>()
                                              : std::optional<LocationRecord>(*existing));
  }

  void rollback() {
    if (committed_) {
      return;
    }
    for (auto& entry : captured_) {
      if (entry.second.has_value()) {
        const auto iterator = model_.records.find(entry.first);
        if (iterator != model_.records.end()) {
          iterator->second = entry.second.value();
        } else {
          model_.records.emplace(entry.first, entry.second.value());
        }
      } else {
        model_.records.erase(entry.first);
      }
    }
    model_.revision = revision_;
    model_.sequence = sequence_;
    model_.receipts = receipts_;
    model_.replacements = replacements_;
    model_.total_moves = total_moves_;
    model_.total_aliases = total_aliases_;
    model_.rebuild_indexes();
  }

  void commit() noexcept { committed_ = true; }

 private:
  Snapshot::Impl& model_;
  std::map<LocationId, std::optional<LocationRecord>, std::less<>> captured_;
  LocationRevision revision_;
  StateSequence sequence_;
  std::vector<OperationReceipt> receipts_;
  std::vector<ReplacementRecord> replacements_;
  std::uint64_t total_moves_;
  std::uint32_t total_aliases_;
  bool committed_ = false;
};

// ---------------------------------------------------------------------------
// Shared checks
// ---------------------------------------------------------------------------

bool is_replaced(const LocationRecord& record) {
  return record.lifecycle == LifecycleState::Replaced;
}

Result<void> check_cancelled(const std::stop_token& stop) {
  if (stop.stop_requested()) {
    return Error(ErrorCode::Cancelled,
                 "the operation was cancelled before publication; nothing was published");
  }
  return ok();
}

Result<void> require_writable(bool closed, bool writer) {
  if (closed) {
    return Error(ErrorCode::SessionClosed, "this registry session is closed");
  }
  if (!writer) {
    return Error(ErrorCode::SessionReadOnly,
                 "this session was opened read-only and cannot mutate the registry");
  }
  return ok();
}

Result<void> check_expected_revision(const MutationContext& context, LocationRevision current) {
  if (context.expected_revision.has_value() && context.expected_revision.value() != current) {
    return Error(ErrorCode::StaleRevision,
                 "expected revision " +
                     std::to_string(context.expected_revision.value().value()) +
                     " does not match the committed revision " + std::to_string(current.value()))
        .with_subject(std::to_string(context.expected_revision.value().value()));
  }
  return ok();
}

Result<void> check_expected_generation(const MutationContext& context,
                                       const LocationRecord& record) {
  if (context.expected_generation.has_value() &&
      context.expected_generation.value() != record.generation) {
    return Error(ErrorCode::StaleGeneration,
                 "expected generation " +
                     std::to_string(context.expected_generation.value().value()) +
                     " does not match the current generation " +
                     std::to_string(record.generation.value()))
        .with_subject(record.id.str());
  }
  return ok();
}

Result<void> check_text_field(std::string_view text,
                              std::uint32_t limit,
                              ErrorCode too_long_code,
                              ErrorCode malformed_code,
                              const char* field_name,
                              std::string_view syntax_help) {
  return internal::validate_text_field(text, limit, true, too_long_code, malformed_code, field_name,
                                       syntax_help);
}

/// Verifies that a parent is usable for a new child of the given kind.
Result<void> check_parent_usable(const Snapshot::Impl& model,
                                 const std::optional<LocationId>& parent,
                                 LocationKind child_kind) {
  if (!parent.has_value()) {
    if (kind_requires_parent(child_kind)) {
      return Error(ErrorCode::KindMustHaveParent,
                   std::string("a ") + std::string(location_kind_name(child_kind)) +
                       " must be contained by a parent")
          .with_subject(std::string(location_kind_name(child_kind)));
    }
    return ok();
  }
  if (kind_may_be_root(child_kind)) {
    return Error(ErrorCode::KindMustNotHaveParent,
                 std::string("a ") + std::string(location_kind_name(child_kind)) +
                     " is a top-level location and cannot be contained")
        .with_subject(std::string(location_kind_name(child_kind)));
  }
  const LocationRecord* parent_record = model.find(parent.value());
  if (parent_record == nullptr) {
    return Error(ErrorCode::NotFound, "the requested parent does not exist").with_subject(parent->str());
  }
  if (!is_legal_child_kind(parent_record->kind, child_kind)) {
    return Error(ErrorCode::InvalidKindForParent,
                 std::string("a ") + std::string(location_kind_name(child_kind)) +
                     " cannot be contained by a " +
                     std::string(location_kind_name(parent_record->kind)))
        .with_subject(parent_record->id.str());
  }
  if (parent_record->lifecycle != LifecycleState::Active) {
    return Error(ErrorCode::ParentNotActive, "the parent is not active")
        .with_subject(parent_record->id.str());
  }
  return ok();
}

Result<void> check_rack_geometry(const Snapshot::Impl& model,
                                 const std::optional<LocationId>& parent,
                                 LocationKind kind,
                                 const std::optional<RackUnitCoordinate>& unit,
                                 const std::optional<RackEnvelope>& envelope,
                                 const LocationId& owner) {
  if (unit.has_value() && !kind_may_carry_unit_coordinate(kind)) {
    return Error(ErrorCode::RackUnitNotAllowed,
                 std::string("a ") + std::string(location_kind_name(kind)) +
                     " cannot carry a rack-unit coordinate")
        .with_subject(owner.str());
  }
  if (envelope.has_value() && !kind_may_carry_envelope(kind)) {
    return Error(ErrorCode::RackEnvelopeInvalid,
                 std::string("a ") + std::string(location_kind_name(kind)) +
                     " cannot carry a rack envelope")
        .with_subject(owner.str());
  }
  if (kind_may_carry_unit_coordinate(kind) && !unit.has_value()) {
    return Error(ErrorCode::InvalidArgument, "a rack-unit location must carry a unit coordinate")
        .with_subject(owner.str());
  }
  if (!unit.has_value() || !parent.has_value()) {
    return ok();
  }

  const LocationRecord* parent_record = model.find(parent.value());
  if (parent_record != nullptr && parent_record->envelope.has_value() &&
      !parent_record->envelope->contains(unit.value())) {
    return Error(ErrorCode::RackUnitOutOfEnvelope,
                 "unit " + unit->to_string() + " is outside the rack envelope " +
                     parent_record->envelope->to_string())
        .with_subject(owner.str());
  }
  const ChildSet* set = model.child_set(parent);
  if (set != nullptr) {
    for (const auto& entry : set->by_component) {
      if (entry.second == owner) {
        continue;
      }
      const LocationRecord* sibling = model.find(entry.second);
      if (sibling != nullptr && sibling->unit.has_value() &&
          sibling->unit.value() == unit.value()) {
        return Error(ErrorCode::RackUnitConflict,
                     "unit " + unit->to_string() + " is already the address of location " +
                         sibling->id.str())
            .with_subject(owner.str());
      }
    }
  }
  return ok();
}

/// Sibling component availability: exact collision, look-alike collision and the
/// child-count bound.
Result<void> check_component_available(const Snapshot::Impl& model,
                                       const std::optional<LocationId>& parent,
                                       const AddressComponent& component,
                                       const LocationId& owner) {
  const ChildSet* set = model.child_set(parent);
  if (set == nullptr) {
    return ok();
  }
  const auto exact = set->by_component.find(component.value());
  if (exact != set->by_component.end() && exact->second != owner) {
    return Error(ErrorCode::AddressInUse,
                 "the address component is already used by location " + exact->second.str())
        .with_subject(component.str());
  }
  const auto folded = set->by_folded_component.find(ascii_fold(component.value()));
  if (folded != set->by_folded_component.end() && folded->second != owner) {
    return Error(ErrorCode::AddressLookAlike,
                 "the address component differs only by ASCII letter case from location " +
                     folded->second.str())
        .with_subject(component.str());
  }
  if (parent.has_value() &&
      set->size() >= static_cast<std::size_t>(model.limits.max_children_per_location) &&
      exact == set->by_component.end()) {
    return Error(ErrorCode::LimitExceeded,
                 "the parent already holds the configured maximum of " +
                     std::to_string(model.limits.max_children_per_location) + " children")
        .with_subject(parent->str());
  }
  return ok();
}

/// An address must never be claimed by an alias bound to a different location,
/// and an alias must never duplicate the owner's own current address.
Result<void> check_alias_claims(const Snapshot::Impl& model,
                                const LocationPath& path,
                                const LocationId& owner) {
  const std::string text = path.to_string();
  const auto exact = model.alias_index.find(text);
  if (exact != model.alias_index.end()) {
    if (exact->second == owner) {
      return Error(ErrorCode::AliasRedundant,
                   "this address is already bound as an alias of this location")
          .with_subject(text);
    }
    return Error(ErrorCode::AliasConflictsWithAddress,
                 "this address is bound as an alias of location " + exact->second.str())
        .with_subject(text);
  }
  const auto folded = model.alias_folded_index.find(ascii_fold(text));
  if (folded != model.alias_folded_index.end()) {
    return Error(ErrorCode::AddressLookAlike,
                 "the address differs only by ASCII letter case from alias " + folded->second)
        .with_subject(text);
  }
  return ok();
}

/// Subtree ids that are addressable (a replaced location is not).
Result<std::vector<LocationId>> addressable_subtree(const Snapshot::Impl& model,
                                                    const LocationId& root_id) {
  std::vector<LocationId> addressable;
  PLR_TRY(ids, model.subtree_ids(root_id, model.limits.max_depth));
  addressable.reserve(ids.size());
  for (const LocationId& id : ids) {
    const LocationRecord* record = model.find(id);
    if (record == nullptr) {
      return Error(ErrorCode::InternalError, "subtree walk returned an unknown location")
          .with_subject(id.str());
    }
    if (is_replaced(*record)) {
      continue;
    }
    addressable.push_back(id);
  }
  return addressable;
}

/// Height of the subtree rooted at a location, where a leaf has height 1.
Result<std::uint32_t> subtree_height_of(const Snapshot::Impl& model, const LocationId& root_id) {
  PLR_TRY(ids, addressable_subtree(model, root_id));
  std::uint32_t height = 0;
  for (const LocationId& id : ids) {
    std::uint32_t depth = 0;
    const LocationRecord* cursor = model.find(id);
    while (cursor != nullptr) {
      ++depth;
      if (cursor->id == root_id || !cursor->parent.has_value()) {
        break;
      }
      cursor = model.find(cursor->parent.value());
    }
    height = std::max(height, depth);
  }
  return height;
}

/// Longest canonical address that would exist under a record addressed as
/// "root_path", in bytes.
Result<std::size_t> deepest_path_bytes_of(const Snapshot::Impl& model,
                                          const LocationId& root_id,
                                          const LocationPath& root_path) {
  PLR_TRY(ids, addressable_subtree(model, root_id));
  std::size_t deepest = root_path.byte_length();
  for (const LocationId& id : ids) {
    if (id == root_id) {
      continue;
    }
    const LocationRecord* cursor = model.find(id);
    std::size_t bytes = root_path.byte_length();
    while (cursor != nullptr && cursor->id != root_id) {
      bytes += cursor->component.value().size() + 1U;
      if (!cursor->parent.has_value()) {
        break;
      }
      cursor = model.find(cursor->parent.value());
    }
    deepest = std::max(deepest, bytes);
  }
  return deepest;
}

/// Refreshes one record's generation and provenance after a change.
void touch(LocationRecord& record,
           const ActorId& actor,
           const Timestamp& at,
           LocationRevision revision) {
  const auto next = record.generation.next();
  record.generation = next.has_value() ? next.value() : LocationGeneration(UINT64_MAX);
  record.provenance.last_modified_by = actor;
  record.provenance.last_modified_at = at;
  record.provenance.last_modified_revision = revision;
}

/// Bumps generation and provenance for every addressable node in a subtree.
Result<std::uint32_t> restamp_subtree(Snapshot::Impl& model,
                                      MutationUndo& undo,
                                      const LocationId& root_id,
                                      const ActorId& actor,
                                      const Timestamp& at,
                                      LocationRevision revision) {
  PLR_TRY(ids, addressable_subtree(model, root_id));
  std::uint32_t descendants = 0;
  for (const LocationId& id : ids) {
    undo.capture(id);
    LocationRecord* record = model.find(id);
    if (record == nullptr) {
      return Error(ErrorCode::InternalError, "subtree walk returned an unknown location")
          .with_subject(id.str());
    }
    touch(*record, actor, at, revision);
    if (id != root_id) {
      ++descendants;
    }
  }
  return descendants;
}

// ---------------------------------------------------------------------------
// Idempotency
// ---------------------------------------------------------------------------

MutationReceipt receipt_from(const OperationReceipt& receipt, bool replayed) {
  MutationReceipt result;
  result.revision = receipt.revision;
  result.sequence = receipt.sequence;
  result.generation = receipt.generation;
  result.successor_generation = receipt.successor_generation;
  result.affected_locations = receipt.affected_locations;
  result.affected_descendants = receipt.affected_descendants;
  result.replayed = replayed;
  return result;
}

Result<MutationReceipt> replay_if_recorded(const Snapshot::Impl& model,
                                           const MutationContext& context,
                                           const std::string& fingerprint) {
  MutationReceipt replay;
  if (!context.operation_id.has_value()) {
    return Error(ErrorCode::NotFound, "no operation id was supplied");
  }
  const auto iterator = model.receipt_index.find(context.operation_id.value());
  if (iterator == model.receipt_index.end()) {
    return Error(ErrorCode::NotFound, "this operation has not been applied");
  }
  if (iterator->second.fingerprint != fingerprint) {
    return Error(ErrorCode::OperationIdConflict,
                 "this operation id was already applied to a different request")
        .with_subject(context.operation_id.value().str());
  }
  return receipt_from(iterator->second, true);
}

void remember_receipt(Snapshot::Impl& model,
                      const MutationContext& context,
                      const std::string& fingerprint,
                      const MutationReceipt& receipt) {
  if (!context.operation_id.has_value()) {
    return;
  }
  OperationReceipt stored;
  stored.operation_id = context.operation_id.value();
  stored.fingerprint = fingerprint;
  stored.revision = receipt.revision;
  stored.sequence = receipt.sequence;
  stored.generation = receipt.generation;
  stored.successor_generation = receipt.successor_generation;
  stored.affected_locations = receipt.affected_locations;
  stored.affected_descendants = receipt.affected_descendants;
  stored.at = context.at;
  stored.actor = context.actor;

  model.receipts.push_back(stored);
  model.receipt_index.erase(stored.operation_id);
  model.receipt_index.emplace(stored.operation_id, stored);
  while (model.receipts.size() > static_cast<std::size_t>(model.limits.max_operation_receipts)) {
    model.receipt_index.erase(model.receipts.front().operation_id);
    model.receipts.erase(model.receipts.begin());
  }
}

// ---------------------------------------------------------------------------
// Fingerprints
// ---------------------------------------------------------------------------

std::string finish_fingerprint(ByteWriter& writer) {
  return Sha256::hex(writer.take());
}

void write_context(ByteWriter& writer, const MutationContext& context) {
  writer.bytes(context.actor.value());
  writer.i64(context.at.unix_seconds());
  writer.u32(context.at.nanos());
  writer.u8(context.authority.has_value() ? 1U : 0U);
  if (context.authority.has_value()) {
    writer.bytes(context.authority->store_id.value());
    writer.u64(context.authority->writer_epoch.value());
  }
  writer.u8(context.expected_revision.has_value() ? 1U : 0U);
  if (context.expected_revision.has_value()) {
    writer.u64(context.expected_revision->value());
  }
  writer.u8(context.expected_generation.has_value() ? 1U : 0U);
  if (context.expected_generation.has_value()) {
    writer.u64(context.expected_generation->value());
  }
  writer.bytes(context.reason);
}

void write_optional_id(ByteWriter& writer, const std::optional<LocationId>& id) {
  writer.u8(id.has_value() ? 1U : 0U);
  writer.bytes(id.has_value() ? std::string_view(id->value()) : std::string_view());
}

void write_optional_unit(ByteWriter& writer, const std::optional<RackUnitCoordinate>& unit) {
  writer.u8(unit.has_value() ? 1U : 0U);
  writer.u32(unit.has_value() ? unit->value() : 0U);
}

void write_optional_envelope(ByteWriter& writer, const std::optional<RackEnvelope>& envelope) {
  writer.u8(envelope.has_value() ? 1U : 0U);
  writer.u32(envelope.has_value() ? envelope->first().value() : 0U);
  writer.u32(envelope.has_value() ? envelope->height() : 0U);
}

// ---------------------------------------------------------------------------
// Phase 1: pure validation
// ---------------------------------------------------------------------------

Result<LocationPath> path_for_child(const Snapshot::Impl& model,
                                    const std::optional<LocationId>& parent,
                                    const AddressComponent& component) {
  if (!parent.has_value()) {
    std::vector<AddressComponent> single;
    single.push_back(component);
    return LocationPath::from_components(std::move(single), model.limits);
  }
  PLR_TRY(parent_path, model.path_of(parent.value()));
  return parent_path.child(component, model.limits);
}

Result<void> validate_create(const Snapshot::Impl& model, const CreateLocationRequest& request) {
  if (model.find(request.id) != nullptr) {
    return Error(ErrorCode::AlreadyPresent, "a location with this identity already exists")
        .with_subject(request.id.str());
  }
  PLR_CHECK(check_text_field(request.component.value(),
                                         model.limits.max_address_component_bytes,
                                         ErrorCode::AddressComponentTooLong,
                                         ErrorCode::MalformedAddressComponent, "address component",
                                         address_component_syntax_help()));
  PLR_CHECK(check_text_field(request.label, model.limits.max_label_bytes,
                                     ErrorCode::LabelTooLong, ErrorCode::MalformedLabel, "label",
                                     label_syntax_help()));
  PLR_CHECK(check_text_field(request.source, model.limits.max_source_bytes,
                                      ErrorCode::MalformedText, ErrorCode::MalformedText, "source",
                                      "valid UTF-8 without control characters"));
  PLR_CHECK(check_parent_usable(model, request.parent, request.kind));
  PLR_CHECK(check_rack_geometry(model, request.parent, request.kind, request.unit,
                                       request.envelope, request.id));
  if (model.records.size() >= static_cast<std::size_t>(model.limits.max_locations)) {
    return Error(ErrorCode::LimitExceeded,
                 "the registry already holds the configured maximum of " +
                     std::to_string(model.limits.max_locations) + " locations");
  }
  PLR_TRY(path, path_for_child(model, request.parent, request.component));
  if (path.depth() > static_cast<std::size_t>(model.limits.max_depth)) {
    return Error(ErrorCode::PathTooDeep,
                 "the new location would be deeper than the configured maximum of " +
                     std::to_string(model.limits.max_depth))
        .with_subject(request.id.str());
  }
  if (path.byte_length() > static_cast<std::size_t>(model.limits.max_path_bytes)) {
    return Error(ErrorCode::PathTooLong,
                 "the new address would be longer than the configured maximum of " +
                     std::to_string(model.limits.max_path_bytes) + " bytes")
        .with_subject(path.to_string());
  }
  PLR_CHECK(check_component_available(model, request.parent, request.component,
                                                    request.id));
  PLR_CHECK(check_alias_claims(model, path, request.id));
  return ok();
}

Result<LocationPath> require_active_target(Snapshot::Impl& model,
                                           const LocationId& id,
                                           const MutationContext& context,
                                           LocationRecord*& record_out) {
  LocationRecord* record = model.find(id);
  if (record == nullptr) {
    return Error(ErrorCode::NotFound, "no location with this identity exists").with_subject(id.str());
  }
  PLR_CHECK(check_expected_generation(context, *record));
  if (record->lifecycle == LifecycleState::Replaced) {
    return Error(ErrorCode::LifecycleTransitionIllegal,
                 "a replaced location is terminal and cannot be mutated")
        .with_subject(id.str());
  }
  if (record->lifecycle != LifecycleState::Active) {
    return Error(ErrorCode::LifecycleTransitionIllegal,
                 "only an active location can be changed; reactivate it first")
        .with_subject(id.str());
  }
  record_out = record;
  return model.path_of(*record);
}

}  // namespace

// ---------------------------------------------------------------------------
// Mutations
// ---------------------------------------------------------------------------

Result<MutationReceipt> Registry::create_location(const CreateLocationRequest& request) {
  std::unique_lock lock(mutex_);
  PLR_CHECK(require_writable(closed_, is_writer()));
  Snapshot::Impl& model = SnapshotAccess::get(*model_);
  PLR_CHECK(internal::validate_context(request.context, model.limits));
  PLR_CHECK(internal::validate_authority(request.context, model.store_id, model.epoch));

  ByteWriter writer;
  writer.raw("create");
  writer.bytes(request.id.value());
  writer.u8(static_cast<std::uint8_t>(request.kind));
  write_optional_id(writer, request.parent);
  writer.bytes(request.component.value());
  writer.bytes(request.label);
  write_optional_unit(writer, request.unit);
  write_optional_envelope(writer, request.envelope);
  writer.bytes(request.source);
  write_context(writer, request.context);
  const std::string fingerprint = finish_fingerprint(writer);

  if (request.context.operation_id.has_value()) {
    auto replay = replay_if_recorded(model, request.context, fingerprint);
    if (replay.has_value()) {
      return replay.value();
    }
    if (replay.error().code() != ErrorCode::NotFound) {
      return replay.error();
    }
  }

  PLR_CHECK(check_expected_revision(request.context, model.revision));
  PLR_CHECK(validate_create(model, request));
  PLR_CHECK(check_cancelled(request.context.stop));

  PLR_TRY(path, path_for_child(model, request.parent, request.component));
  PLR_TRY(next_revision, model.revision.next());
  const LocationGeneration first_generation(LocationGeneration::kFirstPublished);

  MutationUndo undo(model);
  undo.capture(request.id);

  LocationRecord record(request.id, request.component);
  record.kind = request.kind;
  record.parent = request.parent;
  record.label = request.label;
  record.lifecycle = LifecycleState::Active;
  record.unit = request.unit;
  record.envelope = request.envelope;
  record.generation = first_generation;
  record.provenance.created_by = request.context.actor;
  record.provenance.created_at = request.context.at;
  record.provenance.created_revision = next_revision;
  record.provenance.source = request.source;
  record.provenance.last_modified_by = request.context.actor;
  record.provenance.last_modified_at = request.context.at;
  record.provenance.last_modified_revision = next_revision;
  model.records.emplace(request.id, std::move(record));

  ChildSet& set = request.parent.has_value() ? model.children[*request.parent] : model.roots;
  set.by_component.emplace(request.component.str(), request.id);
  set.by_folded_component.emplace(ascii_fold(request.component.value()), request.id);

  model.revision = next_revision;
  PLR_TRY(next_sequence, store_->next_sequence());
  model.sequence = next_sequence;

  MutationReceipt receipt;
  receipt.revision = next_revision;
  receipt.sequence = next_sequence;
  receipt.generation = first_generation;
  receipt.affected_locations = 1;
  remember_receipt(model, request.context, fingerprint, receipt);

  PLR_CHECK(model.validate_state_bounds());
  auto published = store_->publish(*model_);
  if (!published.has_value()) {
    undo.rollback();
    return published.error();
  }
  undo.commit();

  LocationChange change;
  change.id = request.id;
  change.kind = request.kind;
  change.flags = kChangeCreated;
  change.path_after = path;
  change.generation_after = first_generation;
  change.lifecycle_after = LifecycleState::Active;
  change.label_after = request.label;
  change.parent_after = request.parent;
  record_journal(next_revision, request.context, {std::move(change)}, 1, 0);
  return receipt;
}

Result<MutationReceipt> Registry::readdress(const ReaddressRequest& request) {
  std::unique_lock lock(mutex_);
  PLR_CHECK(require_writable(closed_, is_writer()));
  Snapshot::Impl& model = SnapshotAccess::get(*model_);
  PLR_CHECK(internal::validate_context(request.context, model.limits));
  PLR_CHECK(internal::validate_authority(request.context, model.store_id, model.epoch));

  ByteWriter writer;
  writer.raw("readdress");
  writer.bytes(request.id.value());
  writer.bytes(request.component.value());
  write_context(writer, request.context);
  const std::string fingerprint = finish_fingerprint(writer);

  if (request.context.operation_id.has_value()) {
    auto replay = replay_if_recorded(model, request.context, fingerprint);
    if (replay.has_value()) {
      return replay.value();
    }
    if (replay.error().code() != ErrorCode::NotFound) {
      return replay.error();
    }
  }

  PLR_CHECK(check_expected_revision(request.context, model.revision));

  LocationRecord* record = nullptr;
  PLR_TRY(from_path, require_active_target(model, request.id, request.context, record));
  if (record->component == request.component) {
    return Error(ErrorCode::NoOpMutation,
                 "the requested address component is already the current one")
        .with_subject(request.id.str());
  }
  PLR_CHECK(check_text_field(request.component.value(),
                                         model.limits.max_address_component_bytes,
                                         ErrorCode::AddressComponentTooLong,
                                         ErrorCode::MalformedAddressComponent, "address component",
                                         address_component_syntax_help()));
  PLR_CHECK(check_component_available(model, record->parent, request.component, request.id));

  PLR_TRY(to_path, path_for_child(model, record->parent, request.component));
  PLR_TRY(deepest, deepest_path_bytes_of(model, request.id, to_path));
  if (to_path.depth() + 0 > static_cast<std::size_t>(model.limits.max_depth) ||
      deepest > static_cast<std::size_t>(model.limits.max_path_bytes)) {
    return Error(ErrorCode::PathTooLong,
                 "the subtree address would exceed the configured maximum path length of " +
                     std::to_string(model.limits.max_path_bytes) + " bytes")
        .with_subject(to_path.to_string());
  }
  if (to_path.depth() > static_cast<std::size_t>(model.limits.max_depth)) {
    return Error(ErrorCode::PathTooDeep, "the new address is deeper than the configured maximum")
        .with_subject(to_path.to_string());
  }

  PLR_TRY(ids, addressable_subtree(model, request.id));
  for (const LocationId& id : ids) {
    const LocationRecord* member = model.find(id);
    if (member == nullptr) {
      return Error(ErrorCode::InternalError, "subtree walk returned an unknown location")
          .with_subject(id.str());
    }
    LocationPath member_path = to_path;
    if (id != request.id) {
      PLR_TRY(member_current, model.path_of(*member));
      std::vector<AddressComponent> components = to_path.components();
      const std::size_t shared = from_path.depth();
      for (std::size_t index = shared; index < member_current.depth(); ++index) {
        components.push_back(member_current.component(index));
      }
      PLR_TRY(member_new, LocationPath::from_components(std::move(components), model.limits));
      member_path = member_new;
    }
    PLR_CHECK(check_alias_claims(model, member_path, id));
  }

  if (record->moves.size() >= static_cast<std::size_t>(model.limits.max_moves_per_location)) {
    return Error(ErrorCode::LimitExceeded,
                 "this location already holds the configured maximum of " +
                     std::to_string(model.limits.max_moves_per_location) + " move records")
        .with_subject(request.id.str());
  }
  if (model.total_moves >= static_cast<std::uint64_t>(model.limits.max_total_moves)) {
    return Error(ErrorCode::LimitExceeded,
                 "the registry already holds the configured maximum of " +
                     std::to_string(model.limits.max_total_moves) + " move records");
  }
  PLR_CHECK(check_cancelled(request.context.stop));

  PLR_TRY(next_revision, model.revision.next());
  MutationUndo undo(model);
  undo.capture(request.id);
  const LocationGeneration generation_before = record->generation;
  PLR_TRY(descendants, restamp_subtree(model, undo, request.id, request.context.actor,
                                       request.context.at, next_revision));

  // Re-index the renamed node under its parent.
  ChildSet& set = record->parent.has_value() ? model.children[*record->parent] : model.roots;
  set.by_component.erase(record->component.str());
  set.by_folded_component.erase(ascii_fold(record->component.value()));
  set.by_component.emplace(request.component.str(), request.id);
  set.by_folded_component.emplace(ascii_fold(request.component.value()), request.id);

  MoveRecord move;
  move.kind = MoveKind::Readdress;
  move.revision = next_revision;
  move.generation_before = generation_before;
  move.generation_after = record->generation;
  move.from_parent = record->parent;
  move.to_parent = record->parent;
  move.from_component = record->component.str();
  move.to_component = request.component.str();
  move.from_path = from_path.to_string();
  move.to_path = to_path.to_string();
  move.affected_descendants = descendants;
  move.at = request.context.at;
  move.actor = request.context.actor;
  move.reason = request.context.reason;
  record->component = request.component;
  record->moves.push_back(move);
  ++model.total_moves;

  model.revision = next_revision;
  PLR_TRY(next_sequence, store_->next_sequence());
  model.sequence = next_sequence;

  MutationReceipt receipt;
  receipt.revision = next_revision;
  receipt.sequence = next_sequence;
  receipt.generation = record->generation;
  receipt.affected_locations = descendants + 1U;
  receipt.affected_descendants = descendants;
  remember_receipt(model, request.context, fingerprint, receipt);

  PLR_CHECK(model.validate_state_bounds());
  auto published = store_->publish(*model_);
  if (!published.has_value()) {
    undo.rollback();
    return published.error();
  }
  undo.commit();

  LocationChange change;
  change.id = request.id;
  change.kind = record->kind;
  change.flags = kChangeReaddressed;
  change.path_before = from_path;
  change.path_after = to_path;
  change.generation_before = generation_before;
  change.generation_after = record->generation;
  record_journal(next_revision, request.context, {std::move(change)}, 0, 1);
  return receipt;
}

Result<MutationReceipt> Registry::relabel(const RelabelRequest& request) {
  std::unique_lock lock(mutex_);
  PLR_CHECK(require_writable(closed_, is_writer()));
  Snapshot::Impl& model = SnapshotAccess::get(*model_);
  PLR_CHECK(internal::validate_context(request.context, model.limits));
  PLR_CHECK(internal::validate_authority(request.context, model.store_id, model.epoch));

  ByteWriter writer;
  writer.raw("relabel");
  writer.bytes(request.id.value());
  writer.bytes(request.label);
  write_context(writer, request.context);
  const std::string fingerprint = finish_fingerprint(writer);

  if (request.context.operation_id.has_value()) {
    auto replay = replay_if_recorded(model, request.context, fingerprint);
    if (replay.has_value()) {
      return replay.value();
    }
    if (replay.error().code() != ErrorCode::NotFound) {
      return replay.error();
    }
  }

  PLR_CHECK(check_expected_revision(request.context, model.revision));

  LocationRecord* record = model.find(request.id);
  if (record == nullptr) {
    return Error(ErrorCode::NotFound, "no location with this identity exists")
        .with_subject(request.id.str());
  }
  PLR_CHECK(check_expected_generation(request.context, *record));
  if (record->lifecycle == LifecycleState::Replaced) {
    return Error(ErrorCode::LifecycleTransitionIllegal,
                 "a replaced location is terminal and cannot be relabeled")
        .with_subject(request.id.str());
  }
  PLR_CHECK(check_text_field(request.label, model.limits.max_label_bytes,
                                     ErrorCode::LabelTooLong, ErrorCode::MalformedLabel, "label",
                                     label_syntax_help()));
  if (record->label == request.label) {
    return Error(ErrorCode::NoOpMutation, "the requested label is already the current label")
        .with_subject(request.id.str());
  }
  PLR_CHECK(check_cancelled(request.context.stop));

  const std::string previous_label = record->label;
  PLR_TRY(next_revision, model.revision.next());
  MutationUndo undo(model);
  undo.capture(request.id);
  const LocationGeneration generation_before = record->generation;
  record->label = request.label;
  touch(*record, request.context.actor, request.context.at, next_revision);

  model.revision = next_revision;
  PLR_TRY(next_sequence, store_->next_sequence());
  model.sequence = next_sequence;

  MutationReceipt receipt;
  receipt.revision = next_revision;
  receipt.sequence = next_sequence;
  receipt.generation = record->generation;
  receipt.affected_locations = 1;
  remember_receipt(model, request.context, fingerprint, receipt);

  PLR_CHECK(model.validate_state_bounds());
  auto published = store_->publish(*model_);
  if (!published.has_value()) {
    undo.rollback();
    return published.error();
  }
  undo.commit();

  LocationChange change;
  change.id = request.id;
  change.kind = record->kind;
  change.flags = kChangeRelabeled;
  change.generation_before = generation_before;
  change.generation_after = record->generation;
  change.label_before = previous_label;
  change.label_after = request.label;
  record_journal(next_revision, request.context, {std::move(change)}, 0, 1);
  return receipt;
}

Result<MutationReceipt> Registry::move_location(const MoveLocationRequest& request) {
  std::unique_lock lock(mutex_);
  PLR_CHECK(require_writable(closed_, is_writer()));
  Snapshot::Impl& model = SnapshotAccess::get(*model_);
  PLR_CHECK(internal::validate_context(request.context, model.limits));
  PLR_CHECK(internal::validate_authority(request.context, model.store_id, model.epoch));

  ByteWriter writer;
  writer.raw("move");
  writer.bytes(request.id.value());
  writer.bytes(request.new_parent.value());
  write_context(writer, request.context);
  const std::string fingerprint = finish_fingerprint(writer);

  if (request.context.operation_id.has_value()) {
    auto replay = replay_if_recorded(model, request.context, fingerprint);
    if (replay.has_value()) {
      return replay.value();
    }
    if (replay.error().code() != ErrorCode::NotFound) {
      return replay.error();
    }
  }

  PLR_CHECK(check_expected_revision(request.context, model.revision));

  LocationRecord* record = nullptr;
  PLR_TRY(from_path, require_active_target(model, request.id, request.context, record));
  if (kind_may_be_root(record->kind)) {
    return Error(ErrorCode::KindMustNotHaveParent,
                 "a facility is a top-level location and cannot be moved under a parent")
        .with_subject(request.id.str());
  }
  if (request.id == request.new_parent) {
    return Error(ErrorCode::SelfMove, "a location cannot be moved under itself")
        .with_subject(request.id.str());
  }
  if (record->parent.has_value() && record->parent.value() == request.new_parent) {
    return Error(ErrorCode::NoOpMutation, "the location is already under this parent")
        .with_subject(request.id.str());
  }
  PLR_CHECK(check_parent_usable(model, request.new_parent, record->kind));

  PLR_TRY(ancestors, model.subtree_ids(request.id, model.limits.max_depth));
  for (const LocationId& member : ancestors) {
    if (member == request.new_parent) {
      return Error(ErrorCode::MoveIntoDescendant,
                   "the new parent is inside the subtree being moved")
          .with_subject(request.new_parent.str());
    }
  }

  const std::optional<LocationId> new_parent = request.new_parent;
  PLR_CHECK(check_component_available(model, new_parent, record->component, request.id));
  PLR_TRY(to_path, path_for_child(model, new_parent, record->component));
  PLR_TRY(height, subtree_height_of(model, request.id));
  PLR_TRY(parent_path, model.path_of(request.new_parent));
  if (parent_path.depth() + height > static_cast<std::size_t>(model.limits.max_depth)) {
    return Error(ErrorCode::PathTooDeep,
                 "the moved subtree would be deeper than the configured maximum of " +
                     std::to_string(model.limits.max_depth))
        .with_subject(request.id.str());
  }
  PLR_TRY(deepest, deepest_path_bytes_of(model, request.id, to_path));
  if (deepest > static_cast<std::size_t>(model.limits.max_path_bytes)) {
    return Error(ErrorCode::PathTooLong,
                 "the moved subtree address would exceed the configured maximum of " +
                     std::to_string(model.limits.max_path_bytes) + " bytes")
        .with_subject(to_path.to_string());
  }

  PLR_TRY(ids, addressable_subtree(model, request.id));
  for (const LocationId& id : ids) {
    const LocationRecord* member = model.find(id);
    if (member == nullptr) {
      return Error(ErrorCode::InternalError, "subtree walk returned an unknown location")
          .with_subject(id.str());
    }
    LocationPath member_path = to_path;
    if (id != request.id) {
      PLR_TRY(member_current, model.path_of(*member));
      std::vector<AddressComponent> components = to_path.components();
      for (std::size_t index = from_path.depth(); index < member_current.depth(); ++index) {
        components.push_back(member_current.component(index));
      }
      PLR_TRY(member_new, LocationPath::from_components(std::move(components), model.limits));
      member_path = member_new;
    }
    PLR_CHECK(check_alias_claims(model, member_path, id));
  }

  if (record->moves.size() >= static_cast<std::size_t>(model.limits.max_moves_per_location)) {
    return Error(ErrorCode::LimitExceeded,
                 "this location already holds the configured maximum of " +
                     std::to_string(model.limits.max_moves_per_location) + " move records")
        .with_subject(request.id.str());
  }
  if (model.total_moves >= static_cast<std::uint64_t>(model.limits.max_total_moves)) {
    return Error(ErrorCode::LimitExceeded,
                 "the registry already holds the configured maximum of " +
                     std::to_string(model.limits.max_total_moves) + " move records");
  }
  PLR_CHECK(check_cancelled(request.context.stop));

  PLR_TRY(next_revision, model.revision.next());
  MutationUndo undo(model);
  undo.capture(request.id);
  const std::optional<LocationId> previous_parent = record->parent;
  const LocationGeneration generation_before = record->generation;
  PLR_TRY(descendants, restamp_subtree(model, undo, request.id, request.context.actor,
                                       request.context.at, next_revision));

  ChildSet& old_set = previous_parent.has_value() ? model.children[*previous_parent] : model.roots;
  old_set.by_component.erase(record->component.str());
  old_set.by_folded_component.erase(ascii_fold(record->component.value()));
  ChildSet& new_set = model.children[request.new_parent];
  new_set.by_component.emplace(record->component.str(), request.id);
  new_set.by_folded_component.emplace(ascii_fold(record->component.value()), request.id);

  MoveRecord move;
  move.kind = MoveKind::Reparent;
  move.revision = next_revision;
  move.generation_before = generation_before;
  move.generation_after = record->generation;
  move.from_parent = previous_parent;
  move.to_parent = request.new_parent;
  move.from_component = record->component.str();
  move.to_component = record->component.str();
  move.from_path = from_path.to_string();
  move.to_path = to_path.to_string();
  move.affected_descendants = descendants;
  move.at = request.context.at;
  move.actor = request.context.actor;
  move.reason = request.context.reason;
  record->parent = request.new_parent;
  record->moves.push_back(move);
  ++model.total_moves;

  model.revision = next_revision;
  PLR_TRY(next_sequence, store_->next_sequence());
  model.sequence = next_sequence;

  MutationReceipt receipt;
  receipt.revision = next_revision;
  receipt.sequence = next_sequence;
  receipt.generation = record->generation;
  receipt.affected_locations = descendants + 1U;
  receipt.affected_descendants = descendants;
  remember_receipt(model, request.context, fingerprint, receipt);

  PLR_CHECK(model.validate_state_bounds());
  auto published = store_->publish(*model_);
  if (!published.has_value()) {
    undo.rollback();
    return published.error();
  }
  undo.commit();

  LocationChange change;
  change.id = request.id;
  change.kind = record->kind;
  change.flags = kChangeReparented;
  change.path_before = from_path;
  change.path_after = to_path;
  change.parent_before = previous_parent;
  change.parent_after = request.new_parent;
  change.generation_before = generation_before;
  change.generation_after = record->generation;
  record_journal(next_revision, request.context, {std::move(change)}, 0, 1);
  return receipt;
}

Result<MutationReceipt> Registry::retire_location(const RetireLocationRequest& request) {
  std::unique_lock lock(mutex_);
  PLR_CHECK(require_writable(closed_, is_writer()));
  Snapshot::Impl& model = SnapshotAccess::get(*model_);
  PLR_CHECK(internal::validate_context(request.context, model.limits));
  PLR_CHECK(internal::validate_authority(request.context, model.store_id, model.epoch));

  ByteWriter writer;
  writer.raw("retire");
  writer.bytes(request.id.value());
  writer.u8(static_cast<std::uint8_t>(request.mode));
  write_context(writer, request.context);
  const std::string fingerprint = finish_fingerprint(writer);

  if (request.context.operation_id.has_value()) {
    auto replay = replay_if_recorded(model, request.context, fingerprint);
    if (replay.has_value()) {
      return replay.value();
    }
    if (replay.error().code() != ErrorCode::NotFound) {
      return replay.error();
    }
  }

  PLR_CHECK(check_expected_revision(request.context, model.revision));

  LocationRecord* record = model.find(request.id);
  if (record == nullptr) {
    return Error(ErrorCode::NotFound, "no location with this identity exists")
        .with_subject(request.id.str());
  }
  PLR_CHECK(check_expected_generation(request.context, *record));
  if (!is_legal_lifecycle_transition(record->lifecycle, LifecycleTransition::Retire)) {
    return Error(ErrorCode::LifecycleTransitionIllegal,
                 std::string("a ") + std::string(lifecycle_state_name(record->lifecycle)) +
                     " location cannot be retired")
        .with_subject(request.id.str());
  }

  PLR_TRY(scope, addressable_subtree(model, request.id));
  if (request.mode == SubtreeMode::LocationOnly && scope.size() > 1) {
    return Error(ErrorCode::LocationHasActiveChildren,
                 "this location still has descendants; retire the subtree or retire descendants "
                 "first")
        .with_subject(request.id.str());
  }
  std::vector<LocationId> retirable;
  for (const LocationId& id : scope) {
    const LocationRecord* member = model.find(id);
    if (member == nullptr) {
      return Error(ErrorCode::InternalError, "subtree walk returned an unknown location")
          .with_subject(id.str());
    }
    if (member->lifecycle == LifecycleState::Active) {
      retirable.push_back(id);
    }
  }
  if (retirable.empty()) {
    return Error(ErrorCode::NoOpMutation, "no location in scope is active, so nothing would change")
        .with_subject(request.id.str());
  }
  PLR_CHECK(check_cancelled(request.context.stop));

  PLR_TRY(next_revision, model.revision.next());
  MutationUndo undo(model);
  std::vector<LocationChange> changes;
  changes.reserve(retirable.size());
  for (const LocationId& id : retirable) {
    undo.capture(id);
    LocationRecord* member = model.find(id);
    PLR_TRY(member_path, model.path_of(*member));
    const LifecycleState before = member->lifecycle;
    const LocationGeneration generation_before = member->generation;
    member->lifecycle = LifecycleState::Retired;
    touch(*member, request.context.actor, request.context.at, next_revision);
    changes.push_back(internal::lifecycle_change(*member, member_path, before, LifecycleState::Retired));
    changes.back().generation_before = generation_before;
  }

  model.revision = next_revision;
  PLR_TRY(next_sequence, store_->next_sequence());
  model.sequence = next_sequence;

  MutationReceipt receipt;
  receipt.revision = next_revision;
  receipt.sequence = next_sequence;
  receipt.generation = model.find(request.id)->generation;
  receipt.affected_locations = static_cast<std::uint32_t>(retirable.size());
  remember_receipt(model, request.context, fingerprint, receipt);

  PLR_CHECK(model.validate_state_bounds());
  auto published = store_->publish(*model_);
  if (!published.has_value()) {
    undo.rollback();
    return published.error();
  }
  undo.commit();

  const std::uint32_t changed = static_cast<std::uint32_t>(changes.size());
  record_journal(next_revision, request.context, std::move(changes), 0, changed);
  return receipt;
}

Result<MutationReceipt> Registry::reactivate_location(const ReactivateLocationRequest& request) {
  std::unique_lock lock(mutex_);
  PLR_CHECK(require_writable(closed_, is_writer()));
  Snapshot::Impl& model = SnapshotAccess::get(*model_);
  PLR_CHECK(internal::validate_context(request.context, model.limits));
  PLR_CHECK(internal::validate_authority(request.context, model.store_id, model.epoch));

  ByteWriter writer;
  writer.raw("reactivate");
  writer.bytes(request.id.value());
  writer.u8(static_cast<std::uint8_t>(request.mode));
  write_context(writer, request.context);
  const std::string fingerprint = finish_fingerprint(writer);

  if (request.context.operation_id.has_value()) {
    auto replay = replay_if_recorded(model, request.context, fingerprint);
    if (replay.has_value()) {
      return replay.value();
    }
    if (replay.error().code() != ErrorCode::NotFound) {
      return replay.error();
    }
  }

  PLR_CHECK(check_expected_revision(request.context, model.revision));

  LocationRecord* record = model.find(request.id);
  if (record == nullptr) {
    return Error(ErrorCode::NotFound, "no location with this identity exists")
        .with_subject(request.id.str());
  }
  PLR_CHECK(check_expected_generation(request.context, *record));
  if (!is_legal_lifecycle_transition(record->lifecycle, LifecycleTransition::Reactivate)) {
    return Error(ErrorCode::LifecycleTransitionIllegal,
                 std::string("a ") + std::string(lifecycle_state_name(record->lifecycle)) +
                     " location cannot be reactivated")
        .with_subject(request.id.str());
  }
  if (record->parent.has_value()) {
    const LocationRecord* parent = model.find(record->parent.value());
    if (parent == nullptr || parent->lifecycle != LifecycleState::Active) {
      return Error(ErrorCode::ParentNotActive,
                   "the parent must be active before a location can be reactivated")
          .with_subject(request.id.str());
    }
  }

  PLR_TRY(scope, addressable_subtree(model, request.id));
  std::vector<LocationId> reactivatable;
  for (const LocationId& id : scope) {
    const LocationRecord* member = model.find(id);
    if (member == nullptr) {
      return Error(ErrorCode::InternalError, "subtree walk returned an unknown location")
          .with_subject(id.str());
    }
    if (member->lifecycle == LifecycleState::Retired) {
      reactivatable.push_back(id);
      continue;
    }
    if (id != request.id && request.mode == SubtreeMode::Subtree) {
      return Error(ErrorCode::LifecycleTransitionIllegal,
                   "the subtree contains a location that is not retired")
          .with_subject(id.str());
    }
  }
  if (reactivatable.empty()) {
    return Error(ErrorCode::NoOpMutation, "no location in scope is retired, so nothing would change")
        .with_subject(request.id.str());
  }
  PLR_CHECK(check_cancelled(request.context.stop));

  PLR_TRY(next_revision, model.revision.next());
  MutationUndo undo(model);
  std::vector<LocationChange> changes;
  changes.reserve(reactivatable.size());
  for (const LocationId& id : reactivatable) {
    undo.capture(id);
    LocationRecord* member = model.find(id);
    PLR_TRY(member_path, model.path_of(*member));
    const LifecycleState before = member->lifecycle;
    const LocationGeneration generation_before = member->generation;
    member->lifecycle = LifecycleState::Active;
    touch(*member, request.context.actor, request.context.at, next_revision);
    changes.push_back(internal::lifecycle_change(*member, member_path, before, LifecycleState::Active));
    changes.back().generation_before = generation_before;
  }

  model.revision = next_revision;
  PLR_TRY(next_sequence, store_->next_sequence());
  model.sequence = next_sequence;

  MutationReceipt receipt;
  receipt.revision = next_revision;
  receipt.sequence = next_sequence;
  receipt.generation = model.find(request.id)->generation;
  receipt.affected_locations = static_cast<std::uint32_t>(reactivatable.size());
  remember_receipt(model, request.context, fingerprint, receipt);

  PLR_CHECK(model.validate_state_bounds());
  auto published = store_->publish(*model_);
  if (!published.has_value()) {
    undo.rollback();
    return published.error();
  }
  undo.commit();

  const std::uint32_t changed = static_cast<std::uint32_t>(changes.size());
  record_journal(next_revision, request.context, std::move(changes), 0, changed);
  return receipt;
}

Result<MutationReceipt> Registry::replace_location(const ReplaceLocationRequest& request) {
  std::unique_lock lock(mutex_);
  PLR_CHECK(require_writable(closed_, is_writer()));
  Snapshot::Impl& model = SnapshotAccess::get(*model_);
  PLR_CHECK(internal::validate_context(request.context, model.limits));
  PLR_CHECK(internal::validate_authority(request.context, model.store_id, model.epoch));

  ByteWriter writer;
  writer.raw("replace");
  writer.bytes(request.predecessor.value());
  writer.bytes(request.successor_id.value());
  writer.bytes(request.successor_label);
  write_optional_unit(writer, request.successor_unit);
  write_optional_envelope(writer, request.successor_envelope);
  writer.bytes(request.source);
  write_context(writer, request.context);
  const std::string fingerprint = finish_fingerprint(writer);

  if (request.context.operation_id.has_value()) {
    auto replay = replay_if_recorded(model, request.context, fingerprint);
    if (replay.has_value()) {
      return replay.value();
    }
    if (replay.error().code() != ErrorCode::NotFound) {
      return replay.error();
    }
  }

  PLR_CHECK(check_expected_revision(request.context, model.revision));

  LocationRecord* predecessor = model.find(request.predecessor);
  if (predecessor == nullptr) {
    return Error(ErrorCode::NotFound, "the predecessor does not exist")
        .with_subject(request.predecessor.str());
  }
  PLR_CHECK(check_expected_generation(request.context, *predecessor));
  if (predecessor->lifecycle == LifecycleState::Replaced) {
    return Error(ErrorCode::PredecessorAlreadyReplaced,
                 "this location has already been replaced by " +
                     (predecessor->replaced_by.has_value() ? predecessor->replaced_by->str()
                                                           : std::string("<unknown>")))
        .with_subject(request.predecessor.str());
  }
  if (model.find(request.successor_id) != nullptr) {
    return Error(ErrorCode::ReplacementTargetExists,
                 "the successor identity is already in use")
        .with_subject(request.successor_id.str());
  }
  const ChildSet* existing_children = model.child_set(request.predecessor);
  if (existing_children != nullptr && !existing_children->empty()) {
    return Error(ErrorCode::LocationHasChildren,
                 "a location with children cannot be replaced; move or replace them first")
        .with_subject(request.predecessor.str());
  }
  PLR_CHECK(check_text_field(request.successor_label, model.limits.max_label_bytes,
                                     ErrorCode::LabelTooLong, ErrorCode::MalformedLabel, "label",
                                     label_syntax_help()));
  PLR_CHECK(check_text_field(request.source, model.limits.max_source_bytes,
                                      ErrorCode::MalformedText, ErrorCode::MalformedText, "source",
                                      "valid UTF-8 with no control characters"));
  if (model.records.size() >= static_cast<std::size_t>(model.limits.max_locations)) {
    return Error(ErrorCode::LimitExceeded,
                 "the registry already holds the configured maximum of " +
                     std::to_string(model.limits.max_locations) + " locations");
  }
  if (model.replacements.size() >= static_cast<std::size_t>(model.limits.max_total_replacements)) {
    return Error(ErrorCode::LimitExceeded,
                 "the registry already holds the configured maximum of " +
                     std::to_string(model.limits.max_total_replacements) + " replacement records");
  }
  if (predecessor->parent.has_value()) {
    const LocationRecord* parent = model.find(predecessor->parent.value());
    if (parent == nullptr || parent->lifecycle != LifecycleState::Active) {
      return Error(ErrorCode::ParentNotActive,
                   "the parent of the predecessor must be active for a successor to exist")
          .with_subject(request.predecessor.str());
    }
  }

  // The successor occupies the predecessor's place and coordinate.
  const std::optional<RackUnitCoordinate> successor_unit =
      request.successor_unit.has_value() ? request.successor_unit : predecessor->unit;
  const std::optional<RackEnvelope> successor_envelope =
      request.successor_envelope.has_value() ? request.successor_envelope : predecessor->envelope;
  PLR_CHECK(check_rack_geometry(model, predecessor->parent, predecessor->kind, successor_unit,
                                       successor_envelope, request.successor_id));
  PLR_CHECK(check_alias_claims(model, model.path_of(*predecessor).value(),
                                         request.successor_id));
  PLR_CHECK(check_cancelled(request.context.stop));

  PLR_TRY(next_revision, model.revision.next());
  MutationUndo undo(model);
  undo.capture(request.predecessor);
  undo.capture(request.successor_id);

  PLR_TRY(predecessor_path, model.path_of(*predecessor));
  const LocationGeneration predecessor_generation_before = predecessor->generation;

  LocationRecord successor(request.successor_id, predecessor->component);
  successor.kind = predecessor->kind;
  successor.parent = predecessor->parent;
  successor.label = request.successor_label;
  successor.lifecycle = LifecycleState::Active;
  successor.unit = successor_unit;
  successor.envelope = successor_envelope;
  successor.generation = LocationGeneration(LocationGeneration::kFirstPublished);
  successor.replaces = request.predecessor;
  successor.provenance.created_by = request.context.actor;
  successor.provenance.created_at = request.context.at;
  successor.provenance.created_revision = next_revision;
  successor.provenance.source = request.source;
  successor.provenance.last_modified_by = request.context.actor;
  successor.provenance.last_modified_at = request.context.at;
  successor.provenance.last_modified_revision = next_revision;
  model.records.emplace(request.successor_id, std::move(successor));

  // The predecessor keeps its place in the hierarchy for lineage but leaves the
  // address space: the successor now holds that address.
  predecessor->lifecycle = LifecycleState::Replaced;
  predecessor->replaced_by = request.successor_id;
  touch(*predecessor, request.context.actor, request.context.at, next_revision);

  ChildSet& set =
      predecessor->parent.has_value() ? model.children[*predecessor->parent] : model.roots;
  set.by_component.erase(predecessor->component.str());
  set.by_folded_component.erase(ascii_fold(predecessor->component.value()));
  set.by_component.emplace(predecessor->component.str(), request.successor_id);
  set.by_folded_component.emplace(ascii_fold(predecessor->component.value()),
                                  request.successor_id);

  ReplacementRecord replacement;
  replacement.predecessor = request.predecessor;
  replacement.successor = request.successor_id;
  replacement.revision = next_revision;
  replacement.at = request.context.at;
  replacement.actor = request.context.actor;
  replacement.reason = request.context.reason;
  model.replacements.push_back(replacement);

  model.revision = next_revision;
  PLR_TRY(next_sequence, store_->next_sequence());
  model.sequence = next_sequence;

  const LocationRecord* successor_record = model.find(request.successor_id);
  MutationReceipt receipt;
  receipt.revision = next_revision;
  receipt.sequence = next_sequence;
  receipt.generation = predecessor->generation;
  receipt.successor_generation = successor_record->generation;
  receipt.affected_locations = 2;
  remember_receipt(model, request.context, fingerprint, receipt);

  PLR_CHECK(model.validate_state_bounds());
  auto published = store_->publish(*model_);
  if (!published.has_value()) {
    undo.rollback();
    return published.error();
  }
  undo.commit();

  LocationChange predecessor_change;
  predecessor_change.id = request.predecessor;
  predecessor_change.kind = model.find(request.predecessor)->kind;
  predecessor_change.flags = kChangeLifecycle | kChangeReplacementLineage;
  predecessor_change.path_before = predecessor_path;
  predecessor_change.path_after = predecessor_path;
  predecessor_change.generation_before = predecessor_generation_before;
  predecessor_change.generation_after = model.find(request.predecessor)->generation;
  predecessor_change.lifecycle_before = LifecycleState::Active;
  predecessor_change.lifecycle_after = LifecycleState::Replaced;

  LocationChange successor_change;
  successor_change.id = request.successor_id;
  successor_change.kind = successor_record->kind;
  successor_change.flags = kChangeCreated | kChangeReplacementLineage;
  successor_change.path_after = predecessor_path;
  successor_change.generation_after = successor_record->generation;
  successor_change.lifecycle_after = LifecycleState::Active;
  successor_change.label_after = request.successor_label;
  successor_change.parent_after = successor_record->parent;

  record_journal(next_revision, request.context,
                 {std::move(predecessor_change), std::move(successor_change)}, 1, 1);
  return receipt;
}

Result<MutationReceipt> Registry::add_alias(const AddAliasRequest& request) {
  std::unique_lock lock(mutex_);
  PLR_CHECK(require_writable(closed_, is_writer()));
  Snapshot::Impl& model = SnapshotAccess::get(*model_);
  PLR_CHECK(internal::validate_context(request.context, model.limits));
  PLR_CHECK(internal::validate_authority(request.context, model.store_id, model.epoch));

  ByteWriter writer;
  writer.raw("add-alias");
  writer.bytes(request.id.value());
  writer.bytes(request.alias.to_string());
  write_context(writer, request.context);
  const std::string fingerprint = finish_fingerprint(writer);

  if (request.context.operation_id.has_value()) {
    auto replay = replay_if_recorded(model, request.context, fingerprint);
    if (replay.has_value()) {
      return replay.value();
    }
    if (replay.error().code() != ErrorCode::NotFound) {
      return replay.error();
    }
  }

  PLR_CHECK(check_expected_revision(request.context, model.revision));

  LocationRecord* record = nullptr;
  PLR_TRY(current_path, require_active_target(model, request.id, request.context, record));
  if (request.alias.empty()) {
    return Error(ErrorCode::MalformedPath, "an alias cannot be the empty root path");
  }
  const std::string alias_text = request.alias.to_string();
  if (alias_text.size() > static_cast<std::size_t>(model.limits.max_path_bytes)) {
    return Error(ErrorCode::PathTooLong, "the alias is longer than the configured maximum")
        .with_subject(alias_text);
  }
  if (record->aliases.size() >= static_cast<std::size_t>(model.limits.max_aliases_per_location)) {
    return Error(ErrorCode::LimitExceeded,
                 "this location already holds the configured maximum of " +
                     std::to_string(model.limits.max_aliases_per_location) + " aliases")
        .with_subject(request.id.str());
  }
  if (model.alias_index.size() >= static_cast<std::size_t>(model.limits.max_total_aliases)) {
    return Error(ErrorCode::LimitExceeded,
                 "the registry already holds the configured maximum of " +
                     std::to_string(model.limits.max_total_aliases) + " aliases");
  }
  if (alias_text == current_path.to_string()) {
    return Error(ErrorCode::AliasRedundant,
                 "the alias duplicates the location's own canonical address")
        .with_subject(alias_text);
  }
  const auto existing = model.alias_index.find(alias_text);
  if (existing != model.alias_index.end()) {
    if (existing->second == request.id) {
      return Error(ErrorCode::AlreadyPresent, "this alias is already bound to this location")
          .with_subject(alias_text);
    }
    return Error(ErrorCode::AliasConflict,
                 "this alias is already bound to location " + existing->second.str())
        .with_subject(alias_text);
  }
  const auto folded = model.alias_folded_index.find(ascii_fold(alias_text));
  if (folded != model.alias_folded_index.end()) {
    return Error(ErrorCode::AddressLookAlike,
                 "an alias differs from this one only by ASCII letter case: " + folded->second)
        .with_subject(alias_text);
  }
  auto canonical = model.resolve_canonical(request.alias);
  if (canonical.has_value()) {
    return Error(ErrorCode::AliasConflictsWithAddress,
                 "the alias is the current address of location " + canonical.value().str())
        .with_subject(alias_text);
  }
  PLR_CHECK(check_cancelled(request.context.stop));

  PLR_TRY(next_revision, model.revision.next());
  MutationUndo undo(model);
  undo.capture(request.id);
  const LocationGeneration generation_before = record->generation;
  record->aliases.push_back(alias_text);
  std::sort(record->aliases.begin(), record->aliases.end());
  internal::index_alias(model, alias_text, request.id);
  ++model.total_aliases;
  touch(*record, request.context.actor, request.context.at, next_revision);

  model.revision = next_revision;
  PLR_TRY(next_sequence, store_->next_sequence());
  model.sequence = next_sequence;

  MutationReceipt receipt;
  receipt.revision = next_revision;
  receipt.sequence = next_sequence;
  receipt.generation = record->generation;
  receipt.affected_locations = 1;
  remember_receipt(model, request.context, fingerprint, receipt);

  PLR_CHECK(model.validate_state_bounds());
  auto published = store_->publish(*model_);
  if (!published.has_value()) {
    undo.rollback();
    return published.error();
  }
  undo.commit();

  LocationChange change;
  change.id = request.id;
  change.kind = record->kind;
  change.flags = kChangeAliasAdded;
  change.generation_before = generation_before;
  change.generation_after = record->generation;
  change.aliases_added.push_back(alias_text);
  record_journal(next_revision, request.context, {std::move(change)}, 0, 1);
  return receipt;
}

Result<MutationReceipt> Registry::remove_alias(const RemoveAliasRequest& request) {
  std::unique_lock lock(mutex_);
  PLR_CHECK(require_writable(closed_, is_writer()));
  Snapshot::Impl& model = SnapshotAccess::get(*model_);
  PLR_CHECK(internal::validate_context(request.context, model.limits));
  PLR_CHECK(internal::validate_authority(request.context, model.store_id, model.epoch));

  ByteWriter writer;
  writer.raw("remove-alias");
  writer.bytes(request.id.value());
  writer.bytes(request.alias.to_string());
  write_context(writer, request.context);
  const std::string fingerprint = finish_fingerprint(writer);

  if (request.context.operation_id.has_value()) {
    auto replay = replay_if_recorded(model, request.context, fingerprint);
    if (replay.has_value()) {
      return replay.value();
    }
    if (replay.error().code() != ErrorCode::NotFound) {
      return replay.error();
    }
  }

  PLR_CHECK(check_expected_revision(request.context, model.revision));

  LocationRecord* record = model.find(request.id);
  if (record == nullptr) {
    return Error(ErrorCode::NotFound, "no location with this identity exists")
        .with_subject(request.id.str());
  }
  PLR_CHECK(check_expected_generation(request.context, *record));
  if (record->lifecycle == LifecycleState::Replaced) {
    return Error(ErrorCode::LifecycleTransitionIllegal,
                 "a replaced location is terminal and its aliases cannot change")
        .with_subject(request.id.str());
  }
  const std::string alias_text = request.alias.to_string();
  const auto binding = model.alias_index.find(alias_text);
  if (binding == model.alias_index.end()) {
    return Error(ErrorCode::AliasNotFound, "no location has this alias bound")
        .with_subject(alias_text);
  }
  if (binding->second != request.id) {
    return Error(ErrorCode::AliasConflict,
                 "this alias is bound to location " + binding->second.str() + ", not to this one")
        .with_subject(alias_text);
  }
  PLR_CHECK(check_cancelled(request.context.stop));

  PLR_TRY(next_revision, model.revision.next());
  MutationUndo undo(model);
  undo.capture(request.id);
  const LocationGeneration generation_before = record->generation;
  record->aliases.erase(std::remove(record->aliases.begin(), record->aliases.end(), alias_text),
                        record->aliases.end());
  internal::unindex_alias(model, alias_text);
  if (model.total_aliases > 0) {
    --model.total_aliases;
  }
  touch(*record, request.context.actor, request.context.at, next_revision);

  model.revision = next_revision;
  PLR_TRY(next_sequence, store_->next_sequence());
  model.sequence = next_sequence;

  MutationReceipt receipt;
  receipt.revision = next_revision;
  receipt.sequence = next_sequence;
  receipt.generation = record->generation;
  receipt.affected_locations = 1;
  remember_receipt(model, request.context, fingerprint, receipt);

  PLR_CHECK(model.validate_state_bounds());
  auto published = store_->publish(*model_);
  if (!published.has_value()) {
    undo.rollback();
    return published.error();
  }
  undo.commit();

  LocationChange change;
  change.id = request.id;
  change.kind = record->kind;
  change.flags = kChangeAliasRemoved;
  change.generation_before = generation_before;
  change.generation_after = record->generation;
  change.aliases_removed.push_back(alias_text);
  record_journal(next_revision, request.context, {std::move(change)}, 0, 1);
  return receipt;
}

Result<MutationReceipt> Registry::set_rack_geometry(const SetRackGeometryRequest& request) {
  std::unique_lock lock(mutex_);
  PLR_CHECK(require_writable(closed_, is_writer()));
  Snapshot::Impl& model = SnapshotAccess::get(*model_);
  PLR_CHECK(internal::validate_context(request.context, model.limits));
  PLR_CHECK(internal::validate_authority(request.context, model.store_id, model.epoch));

  ByteWriter writer;
  writer.raw("set-rack-geometry");
  writer.bytes(request.id.value());
  write_optional_unit(writer, request.unit);
  write_optional_envelope(writer, request.envelope);
  write_context(writer, request.context);
  const std::string fingerprint = finish_fingerprint(writer);

  if (request.context.operation_id.has_value()) {
    auto replay = replay_if_recorded(model, request.context, fingerprint);
    if (replay.has_value()) {
      return replay.value();
    }
    if (replay.error().code() != ErrorCode::NotFound) {
      return replay.error();
    }
  }

  PLR_CHECK(check_expected_revision(request.context, model.revision));

  LocationRecord* record = nullptr;
  PLR_CHECK(require_active_target(model, request.id, request.context, record));
  PLR_CHECK(check_rack_geometry(model, record->parent, record->kind, request.unit,
                                       request.envelope, request.id));
  if (record->unit == request.unit && record->envelope == request.envelope) {
    return Error(ErrorCode::NoOpMutation, "the requested rack geometry is already in place")
        .with_subject(request.id.str());
  }
  PLR_CHECK(check_cancelled(request.context.stop));

  PLR_TRY(next_revision, model.revision.next());
  MutationUndo undo(model);
  undo.capture(request.id);
  const std::optional<RackUnitCoordinate> previous_unit = record->unit;
  const std::optional<RackEnvelope> previous_envelope = record->envelope;
  const LocationGeneration generation_before = record->generation;
  record->unit = request.unit;
  record->envelope = request.envelope;
  touch(*record, request.context.actor, request.context.at, next_revision);

  model.revision = next_revision;
  PLR_TRY(next_sequence, store_->next_sequence());
  model.sequence = next_sequence;

  MutationReceipt receipt;
  receipt.revision = next_revision;
  receipt.sequence = next_sequence;
  receipt.generation = record->generation;
  receipt.affected_locations = 1;
  remember_receipt(model, request.context, fingerprint, receipt);

  PLR_CHECK(model.validate_state_bounds());
  auto published = store_->publish(*model_);
  if (!published.has_value()) {
    undo.rollback();
    return published.error();
  }
  undo.commit();

  LocationChange change;
  change.id = request.id;
  change.kind = record->kind;
  change.flags = kChangeRackGeometry;
  change.generation_before = generation_before;
  change.generation_after = record->generation;
  change.unit_before = previous_unit;
  change.unit_after = request.unit;
  change.envelope_before = previous_envelope;
  change.envelope_after = request.envelope;
  record_journal(next_revision, request.context, {std::move(change)}, 0, 1);
  return receipt;
}

// ---------------------------------------------------------------------------
// Explanations
// ---------------------------------------------------------------------------

Explanation Registry::explain_create(const CreateLocationRequest& request) const {
  std::shared_lock lock(mutex_);
  if (closed_) {
    return explain_error_code(ErrorCode::SessionClosed);
  }
  const Snapshot::Impl& model = SnapshotAccess::get(*model_);

  auto context_ok = internal::validate_context(request.context, model.limits);
  if (!context_ok.has_value()) {
    return explain_error(context_ok.error());
  }
  auto valid = validate_create(model, request);
  if (!valid.has_value()) {
    Explanation explanation = explain_error(valid.error());
    explanation.details.push_back(std::string("requested kind: ") +
                                  std::string(location_kind_name(request.kind)));
    if (request.parent.has_value()) {
      explanation.details.push_back("requested parent: " + request.parent->str());
    } else {
      explanation.details.push_back("requested parent: none (top level)");
    }
    explanation.details.push_back("requested component: " + request.component.str());
    explanation.details.push_back(
        "note: this verdict is structural; generation, revision and authority preconditions are "
        "evaluated when the mutation is submitted");
    return explanation;
  }

  Explanation explanation;
  explanation.code = ErrorCode::Ok;
  explanation.category = ErrorCategory::Ok;
  explanation.summary = "the create request would be accepted";
  if (request.parent.has_value()) {
    const auto parent_path = model.path_of(request.parent.value());
    if (parent_path.has_value()) {
      explanation.details.push_back("address: " +
                                    parent_path.value().child(request.component, model.limits)
                                        .value()
                                        .to_string());
    }
  } else {
    explanation.details.push_back("address: /" + request.component.str());
  }
  explanation.details.push_back(
      "note: this verdict is structural; generation, revision and authority preconditions are "
      "evaluated when the mutation is submitted");
  return explanation;
}

Explanation Registry::explain_move(const MoveLocationRequest& request) const {
  std::shared_lock lock(mutex_);
  if (closed_) {
    return explain_error_code(ErrorCode::SessionClosed);
  }
  const Snapshot::Impl& model = SnapshotAccess::get(*model_);

  auto context_ok = internal::validate_context(request.context, model.limits);
  if (!context_ok.has_value()) {
    return explain_error(context_ok.error());
  }

  const auto fail = [&](const Error& error) {
    Explanation explanation = explain_error(error);
    explanation.details.push_back("requested location: " + request.id.str());
    explanation.details.push_back("requested parent: " + request.new_parent.str());
    explanation.details.push_back(
        "note: this verdict is structural; generation, revision and authority preconditions are "
        "evaluated when the mutation is submitted");
    return explanation;
  };

  const LocationRecord* record = model.find(request.id);
  if (record == nullptr) {
    return fail(Error(ErrorCode::NotFound, "no location with this identity exists")
                    .with_subject(request.id.str()));
  }
  if (kind_may_be_root(record->kind)) {
    return fail(Error(ErrorCode::KindMustNotHaveParent,
                      "a facility is a top-level location and cannot be moved under a parent")
                    .with_subject(request.id.str()));
  }
  if (request.id == request.new_parent) {
    return fail(Error(ErrorCode::SelfMove, "a location cannot be moved under itself")
                    .with_subject(request.id.str()));
  }
  if (record->parent.has_value() && record->parent.value() == request.new_parent) {
    return fail(Error(ErrorCode::NoOpMutation, "the location is already under this parent")
                    .with_subject(request.id.str()));
  }
  if (record->lifecycle != LifecycleState::Active) {
    return fail(Error(ErrorCode::LifecycleTransitionIllegal,
                      "only an active location can be moved")
                    .with_subject(request.id.str()));
  }
  auto parent_ok = check_parent_usable(model, request.new_parent, record->kind);
  if (!parent_ok.has_value()) {
    return fail(parent_ok.error());
  }
  auto subtree = model.subtree_ids(request.id, model.limits.max_depth);
  if (!subtree.has_value()) {
    return fail(subtree.error());
  }
  for (const LocationId& member : subtree.value()) {
    if (member == request.new_parent) {
      return fail(Error(ErrorCode::MoveIntoDescendant,
                        "the new parent is inside the subtree being moved")
                      .with_subject(request.new_parent.str()));
    }
  }
  auto component_free = check_component_available(model, request.new_parent, record->component,
                                                 request.id);
  if (!component_free.has_value()) {
    return fail(component_free.error());
  }
  const auto parent_path = model.path_of(request.new_parent);
  const auto height = subtree_height_of(model, request.id);
  if (parent_path.has_value() && height.has_value() &&
      parent_path.value().depth() + height.value() >
          static_cast<std::size_t>(model.limits.max_depth)) {
    return fail(Error(ErrorCode::PathTooDeep,
                      "the moved subtree would be deeper than the configured maximum of " +
                          std::to_string(model.limits.max_depth))
                    .with_subject(request.id.str()));
  }

  Explanation explanation;
  explanation.code = ErrorCode::Ok;
  explanation.category = ErrorCategory::Ok;
  explanation.summary = "the move request would be accepted";
  if (parent_path.has_value()) {
    const auto target = parent_path.value().child(record->component, model.limits);
    if (target.has_value()) {
      explanation.details.push_back("new address: " + target.value().to_string());
    }
  }
  if (height.has_value()) {
    explanation.details.push_back("subtree height including the moved location: " +
                                  std::to_string(height.value()));
  }
  explanation.details.push_back(
      "note: this verdict is structural; generation, revision and authority preconditions are "
      "evaluated when the mutation is submitted");
  return explanation;
}

}  // namespace dccp::physical_location_registry
