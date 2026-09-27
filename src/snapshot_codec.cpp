// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include <cstdint>
#include <string>

#include "byte_codec.hpp"
#include "dccp/physical_location_registry/digest.hpp"
#include "dccp/physical_location_registry/snapshot.hpp"
#include "dccp/physical_location_registry/text.hpp"
#include "model.hpp"

namespace dccp::physical_location_registry {
namespace {

/// Magic, header sizes and the fixed field caps used while decoding.
constexpr char kMagic[8] = {'P', 'L', 'R', 'S', 'T', 'A', 'T', '\0'};
constexpr std::size_t kHeaderBytes = 20;
constexpr std::size_t kTrailerBytes = kSha256Bytes;

constexpr std::uint64_t kMaxPathTextBytes = kHardMaxPathBytes;
constexpr std::uint64_t kMaxComponentTextBytes = kMaxAddressComponentBytes;
constexpr std::uint64_t kMaxIdentifierTextBytes = kMaxIdentifierBytes;

void write_move(internal::ByteWriter& writer, const MoveRecord& move) {
  writer.u8(static_cast<std::uint8_t>(move.kind));
  writer.u64(move.revision.value());
  writer.u64(move.generation_before.value());
  writer.u64(move.generation_after.value());
  writer.u8(move.from_parent.has_value() ? 1U : 0U);
  writer.bytes(move.from_parent.has_value() ? std::string_view(move.from_parent->value())
                                            : std::string_view());
  writer.u8(move.to_parent.has_value() ? 1U : 0U);
  writer.bytes(move.to_parent.has_value() ? std::string_view(move.to_parent->value())
                                          : std::string_view());
  writer.bytes(move.from_component);
  writer.bytes(move.to_component);
  writer.bytes(move.from_path);
  writer.bytes(move.to_path);
  writer.u32(move.affected_descendants);
  writer.i64(move.at.unix_seconds());
  writer.u32(move.at.nanos());
  writer.bytes(move.actor.value());
  writer.bytes(move.reason);
}

void write_provenance(internal::ByteWriter& writer, const ProvenanceRecord& provenance) {
  writer.bytes(provenance.created_by.value());
  writer.i64(provenance.created_at.unix_seconds());
  writer.u32(provenance.created_at.nanos());
  writer.u64(provenance.created_revision.value());
  writer.bytes(provenance.source);
  writer.bytes(provenance.last_modified_by.value());
  writer.i64(provenance.last_modified_at.unix_seconds());
  writer.u32(provenance.last_modified_at.nanos());
  writer.u64(provenance.last_modified_revision.value());
}

void write_limits(internal::ByteWriter& writer, const Limits& limits) {
  writer.u32(limits.max_locations);
  writer.u32(limits.max_depth);
  writer.u32(limits.max_children_per_location);
  writer.u32(limits.max_address_component_bytes);
  writer.u32(limits.max_label_bytes);
  writer.u32(limits.max_reason_bytes);
  writer.u32(limits.max_source_bytes);
  writer.u32(limits.max_path_bytes);
  writer.u32(limits.max_aliases_per_location);
  writer.u32(limits.max_total_aliases);
  writer.u32(limits.max_moves_per_location);
  writer.u32(limits.max_total_moves);
  writer.u32(limits.max_total_replacements);
  writer.u32(limits.max_operation_receipts);
  writer.u32(limits.max_retained_revisions);
  writer.u32(limits.max_publications_retained);
  writer.u32(limits.max_traversal_nodes);
  writer.u64(limits.max_state_bytes);
}

Result<Limits> read_limits(internal::ByteReader& reader) {
  Limits limits;
  PLR_TRY(max_locations, reader.u32());
  PLR_TRY(max_depth, reader.u32());
  PLR_TRY(max_children_per_location, reader.u32());
  PLR_TRY(max_address_component_bytes, reader.u32());
  PLR_TRY(max_label_bytes, reader.u32());
  PLR_TRY(max_reason_bytes, reader.u32());
  PLR_TRY(max_source_bytes, reader.u32());
  PLR_TRY(max_path_bytes, reader.u32());
  PLR_TRY(max_aliases_per_location, reader.u32());
  PLR_TRY(max_total_aliases, reader.u32());
  PLR_TRY(max_moves_per_location, reader.u32());
  PLR_TRY(max_total_moves, reader.u32());
  PLR_TRY(max_total_replacements, reader.u32());
  PLR_TRY(max_operation_receipts, reader.u32());
  PLR_TRY(max_retained_revisions, reader.u32());
  PLR_TRY(max_publications_retained, reader.u32());
  PLR_TRY(max_traversal_nodes, reader.u32());
  PLR_TRY(max_state_bytes, reader.u64());

  limits.max_locations = max_locations;
  limits.max_depth = max_depth;
  limits.max_children_per_location = max_children_per_location;
  limits.max_address_component_bytes = max_address_component_bytes;
  limits.max_label_bytes = max_label_bytes;
  limits.max_reason_bytes = max_reason_bytes;
  limits.max_source_bytes = max_source_bytes;
  limits.max_path_bytes = max_path_bytes;
  limits.max_aliases_per_location = max_aliases_per_location;
  limits.max_total_aliases = max_total_aliases;
  limits.max_moves_per_location = max_moves_per_location;
  limits.max_total_moves = max_total_moves;
  limits.max_total_replacements = max_total_replacements;
  limits.max_operation_receipts = max_operation_receipts;
  limits.max_retained_revisions = max_retained_revisions;
  limits.max_publications_retained = max_publications_retained;
  limits.max_traversal_nodes = max_traversal_nodes;
  limits.max_state_bytes = max_state_bytes;

  PLR_CHECK(limits.validate());
  return limits;
}

Result<MoveRecord> read_move(internal::ByteReader& reader, const Limits& limits) {
  MoveRecord move;
  PLR_TRY(kind, reader.u8());
  if (kind > static_cast<std::uint8_t>(MoveKind::Readdress)) {
    return Error(ErrorCode::MalformedRecord, "move record has an unknown kind")
        .with_subject(std::to_string(kind));
  }
  move.kind = static_cast<MoveKind>(kind);

  PLR_TRY(revision, reader.u64());
  move.revision = LocationRevision(revision);
  PLR_TRY(generation_before, reader.u64());
  move.generation_before = LocationGeneration(generation_before);
  PLR_TRY(generation_after, reader.u64());
  move.generation_after = LocationGeneration(generation_after);

  PLR_TRY(has_from, reader.u8());
  PLR_TRY(from_parent, reader.text(kMaxIdentifierTextBytes, "move.from_parent"));
  if (has_from != 0) {
    PLR_TRY(parent_id, LocationId::parse(from_parent));
    move.from_parent = parent_id;
  } else if (!from_parent.empty()) {
    return Error(ErrorCode::MalformedRecord, "move record carries an unexpected parent value");
  }

  PLR_TRY(has_to, reader.u8());
  PLR_TRY(to_parent, reader.text(kMaxIdentifierTextBytes, "move.to_parent"));
  if (has_to != 0) {
    PLR_TRY(parent_id, LocationId::parse(to_parent));
    move.to_parent = parent_id;
  } else if (!to_parent.empty()) {
    return Error(ErrorCode::MalformedRecord, "move record carries an unexpected parent value");
  }

  PLR_TRY(from_component, reader.text(kMaxComponentTextBytes, "move.from_component"));
  PLR_TRY(to_component, reader.text(kMaxComponentTextBytes, "move.to_component"));
  PLR_TRY(from_path, reader.text(kMaxPathTextBytes, "move.from_path"));
  PLR_TRY(to_path, reader.text(kMaxPathTextBytes, "move.to_path"));
  move.from_component = std::move(from_component);
  move.to_component = std::move(to_component);
  move.from_path = std::move(from_path);
  move.to_path = std::move(to_path);
  PLR_TRY(affected, reader.u32());
  move.affected_descendants = affected;
  PLR_TRY(at_seconds, reader.i64());
  PLR_TRY(at_nanos, reader.u32());
  PLR_TRY(at, Timestamp::make(at_seconds, at_nanos));
  move.at = at;
  PLR_TRY(actor_text, reader.text(kMaxIdentifierTextBytes, "move.actor"));
  PLR_TRY(actor, ActorId::parse(actor_text));
  move.actor = actor;
  PLR_TRY(reason, reader.text(limits.max_reason_bytes, "move.reason"));
  move.reason = std::move(reason);
  return move;
}

Result<ProvenanceRecord> read_provenance(internal::ByteReader& reader, const Limits& limits) {
  ProvenanceRecord provenance;
  PLR_TRY(created_by_text, reader.text(kMaxIdentifierTextBytes, "provenance.created_by"));
  PLR_TRY(created_by, ActorId::parse(created_by_text));
  provenance.created_by = created_by;
  PLR_TRY(created_seconds, reader.i64());
  PLR_TRY(created_nanos, reader.u32());
  PLR_TRY(created_at, Timestamp::make(created_seconds, created_nanos));
  provenance.created_at = created_at;
  PLR_TRY(created_revision, reader.u64());
  provenance.created_revision = LocationRevision(created_revision);
  PLR_TRY(source, reader.text(limits.max_source_bytes, "provenance.source"));
  provenance.source = std::move(source);
  PLR_TRY(modified_by_text, reader.text(kMaxIdentifierTextBytes, "provenance.last_modified_by"));
  PLR_TRY(modified_by, ActorId::parse(modified_by_text));
  provenance.last_modified_by = modified_by;
  PLR_TRY(modified_seconds, reader.i64());
  PLR_TRY(modified_nanos, reader.u32());
  PLR_TRY(modified_at, Timestamp::make(modified_seconds, modified_nanos));
  provenance.last_modified_at = modified_at;
  PLR_TRY(modified_revision, reader.u64());
  provenance.last_modified_revision = LocationRevision(modified_revision);
  return provenance;
}

}  // namespace

namespace internal {

Result<std::string> encode_model(const Snapshot::Impl& model) {
  ByteWriter payload;
  payload.bytes(model.store_id.value());
  payload.u64(model.revision.value());
  payload.u64(model.sequence.value());
  payload.u64(model.epoch.value());
  write_limits(payload, model.limits);

  payload.u32(static_cast<std::uint32_t>(model.records.size()));
  for (const auto& entry : model.records) {
    const LocationRecord& record = entry.second;
    payload.bytes(record.id.value());
    payload.u8(static_cast<std::uint8_t>(record.kind));
    payload.u8(record.parent.has_value() ? 1U : 0U);
    payload.bytes(record.parent.has_value() ? std::string_view(record.parent->value())
                                            : std::string_view());
    payload.bytes(record.component.value());
    payload.bytes(record.label);
    payload.u8(static_cast<std::uint8_t>(record.lifecycle));
    payload.u8(record.unit.has_value() ? 1U : 0U);
    payload.u32(record.unit.has_value() ? record.unit->value() : 0U);
    payload.u8(record.envelope.has_value() ? 1U : 0U);
    payload.u32(record.envelope.has_value() ? record.envelope->first().value() : 0U);
    payload.u32(record.envelope.has_value() ? record.envelope->height() : 0U);
    payload.u64(record.generation.value());
    payload.u8(record.replaces.has_value() ? 1U : 0U);
    payload.bytes(record.replaces.has_value() ? std::string_view(record.replaces->value())
                                              : std::string_view());
    payload.u8(record.replaced_by.has_value() ? 1U : 0U);
    payload.bytes(record.replaced_by.has_value() ? std::string_view(record.replaced_by->value())
                                                 : std::string_view());
    write_provenance(payload, record.provenance);
    payload.u32(static_cast<std::uint32_t>(record.aliases.size()));
    for (const std::string& alias : record.aliases) {
      payload.bytes(alias);
    }
    payload.u32(static_cast<std::uint32_t>(record.moves.size()));
    for (const MoveRecord& move : record.moves) {
      write_move(payload, move);
    }
  }

  payload.u32(static_cast<std::uint32_t>(model.replacements.size()));
  for (const ReplacementRecord& replacement : model.replacements) {
    payload.bytes(replacement.predecessor.value());
    payload.bytes(replacement.successor.value());
    payload.u64(replacement.revision.value());
    payload.i64(replacement.at.unix_seconds());
    payload.u32(replacement.at.nanos());
    payload.bytes(replacement.actor.value());
    payload.bytes(replacement.reason);
  }

  payload.u32(static_cast<std::uint32_t>(model.receipts.size()));
  for (const OperationReceipt& receipt : model.receipts) {
    payload.bytes(receipt.operation_id.value());
    payload.bytes(receipt.fingerprint);
    payload.u64(receipt.revision.value());
    payload.u64(receipt.sequence.value());
    payload.u64(receipt.generation.value());
    payload.u8(receipt.successor_generation.has_value() ? 1U : 0U);
    payload.u64(receipt.successor_generation.has_value() ? receipt.successor_generation->value() : 0U);
    payload.u32(receipt.affected_locations);
    payload.u32(receipt.affected_descendants);
    payload.i64(receipt.at.unix_seconds());
    payload.u32(receipt.at.nanos());
    payload.bytes(receipt.actor.value());
  }

  const std::string body = payload.take();
  ByteWriter output;
  output.raw(std::string_view(kMagic, sizeof(kMagic)));
  output.u16(kSnapshotFormatVersion);
  output.u16(0U);
  output.u64(static_cast<std::uint64_t>(body.size()));
  output.raw(body);

  const std::string framed = output.take();
  Sha256 hasher;
  hasher.update(framed);
  std::uint8_t digest[kSha256Bytes];
  hasher.finish(digest);

  std::string encoded = framed;
  encoded.append(reinterpret_cast<const char*>(digest), kSha256Bytes);
  return encoded;
}

Result<Snapshot> decode_model_bytes(std::string_view bytes) {
  if (bytes.size() < kHeaderBytes + kTrailerBytes) {
    return Error(ErrorCode::TruncatedInput, "state is smaller than the minimum framed size")
        .with_subject(std::to_string(bytes.size()));
  }
  if (bytes.size() > static_cast<std::size_t>(kHardMaxStateBytes) + kHeaderBytes + kTrailerBytes) {
    return Error(ErrorCode::LimitExceeded, "state is larger than the absolute maximum of " +
                                               std::to_string(kHardMaxStateBytes) + " bytes")
        .with_subject(std::to_string(bytes.size()));
  }
  if (std::string_view(bytes.data(), sizeof(kMagic)) != std::string_view(kMagic, sizeof(kMagic))) {
    return Error(ErrorCode::MalformedRecord, "state does not begin with the canonical format magic");
  }

  ByteReader header(bytes.data(), kHeaderBytes);
  PLR_CHECK(header.take(sizeof(kMagic)));
  PLR_TRY(version, header.u16());
  if (version != kSnapshotFormatVersion) {
    return Error(ErrorCode::UnsupportedSchemaVersion,
                 "state was written with snapshot format version " + std::to_string(version) +
                     "; this build understands version " + std::to_string(kSnapshotFormatVersion))
        .with_subject(std::to_string(version));
  }
  PLR_TRY(flags, header.u16());
  if (flags != 0U) {
    return Error(ErrorCode::UnsupportedFormatFlag, "state sets format flags this build does not understand")
        .with_subject(std::to_string(flags));
  }
  PLR_TRY(payload_bytes, header.u64());
  const std::size_t expected_payload = bytes.size() - kHeaderBytes - kTrailerBytes;
  if (payload_bytes != static_cast<std::uint64_t>(expected_payload)) {
    return Error(ErrorCode::CountMismatch, "state declares a payload length that does not match the file")
        .with_subject(std::to_string(payload_bytes) + " vs " + std::to_string(expected_payload));
  }

  Sha256 hasher;
  hasher.update(bytes.substr(0, bytes.size() - kTrailerBytes));
  std::uint8_t computed[kSha256Bytes];
  hasher.finish(computed);
  const std::string_view stored(bytes.data() + bytes.size() - kTrailerBytes, kTrailerBytes);
  if (!digest_equal(stored, std::string_view(reinterpret_cast<const char*>(computed), kSha256Bytes))) {
    return Error(ErrorCode::DigestMismatch,
                 "state integrity digest does not match its contents; the publication is corrupt");
  }

  const std::string_view payload = bytes.substr(kHeaderBytes, expected_payload);
  ByteReader reader(payload.data(), payload.size());

  PLR_TRY(store_id_text, reader.text(kMaxIdentifierTextBytes, "store_id"));
  PLR_TRY(store_id, StoreId::parse(store_id_text));
  PLR_TRY(revision, reader.u64());
  PLR_TRY(sequence, reader.u64());
  PLR_TRY(epoch, reader.u64());
  PLR_TRY(limits, read_limits(reader));

  if (payload.size() > static_cast<std::size_t>(limits.max_state_bytes)) {
    return Error(ErrorCode::LimitExceeded,
                 "state is larger than the maximum this state declares for itself")
        .with_subject(std::to_string(payload.size()));
  }

  auto model = std::make_unique<Snapshot::Impl>();
  model->store_id = store_id;
  model->revision = LocationRevision(revision);
  model->sequence = StateSequence(sequence);
  model->epoch = WriterEpoch(epoch);
  model->limits = limits;

  PLR_TRY(location_count, reader.u32());
  if (location_count > limits.max_locations) {
    return Error(ErrorCode::LimitExceeded,
                 "state declares more locations than the configured maximum of " +
                     std::to_string(limits.max_locations))
        .with_subject(std::to_string(location_count));
  }
  // The smallest possible encoded location occupies far more than 8 bytes, so a
  // count that cannot fit in the remaining payload is rejected before the map is
  // sized from it.
  if (static_cast<std::uint64_t>(location_count) * 8ULL > static_cast<std::uint64_t>(reader.remaining())) {
    return Error(ErrorCode::CountMismatch,
                 "state declares more locations than the remaining payload could hold")
        .with_subject(std::to_string(location_count));
  }

  for (std::uint32_t index = 0; index < location_count; ++index) {
    PLR_TRY(id_text, reader.text(kMaxIdentifierTextBytes, "location.id"));
    PLR_TRY(id, LocationId::parse(id_text));
    PLR_TRY(kind_value, reader.u8());
    if (kind_value >= kLocationKindCount) {
      return Error(ErrorCode::MalformedRecord, "location has an unknown kind")
          .with_subject(std::to_string(kind_value));
    }

    // Field order here mirrors encode_model() exactly; the format is canonical,
    // so a decoder that disagreed with the encoder would misread every record.
    PLR_TRY(has_parent, reader.u8());
    PLR_TRY(parent_text, reader.text(kMaxIdentifierTextBytes, "location.parent"));
    std::optional<LocationId> parent;
    if (has_parent != 0) {
      PLR_TRY(parent_id, LocationId::parse(parent_text));
      parent = parent_id;
    } else if (!parent_text.empty()) {
      return Error(ErrorCode::MalformedRecord, "location carries an unexpected parent value")
          .with_subject(id.str());
    }

    PLR_TRY(component_text, reader.text(kMaxComponentTextBytes, "location.component"));
    PLR_TRY(component, AddressComponent::parse(component_text));

    if (model->records.find(id) != model->records.end()) {
      return Error(ErrorCode::IdentityConflict, "state contains a duplicate location identity")
          .with_subject(id.str());
    }
    auto inserted = model->records.emplace(id, LocationRecord(id, component));
    LocationRecord& record = inserted.first->second;
    record.kind = static_cast<LocationKind>(kind_value);
    record.parent = parent;

    PLR_TRY(label, reader.text(limits.max_label_bytes, "location.label"));
    record.label = std::move(label);
    PLR_TRY(lifecycle_value, reader.u8());
    if (lifecycle_value > static_cast<std::uint8_t>(LifecycleState::Replaced)) {
      return Error(ErrorCode::MalformedRecord, "location has an unknown lifecycle state")
          .with_subject(id.str());
    }
    record.lifecycle = static_cast<LifecycleState>(lifecycle_value);

    PLR_TRY(has_unit, reader.u8());
    PLR_TRY(unit_value, reader.u32());
    if (has_unit != 0) {
      PLR_TRY(unit, RackUnitCoordinate::parse(unit_value));
      record.unit = unit;
    } else if (unit_value != 0U) {
      return Error(ErrorCode::MalformedRecord, "location carries an unexpected unit coordinate")
          .with_subject(id.str());
    }

    PLR_TRY(has_envelope, reader.u8());
    PLR_TRY(envelope_first, reader.u32());
    PLR_TRY(envelope_height, reader.u32());
    if (has_envelope != 0) {
      PLR_TRY(first, RackUnitCoordinate::parse(envelope_first));
      PLR_TRY(envelope, RackEnvelope::make(first, envelope_height));
      record.envelope = envelope;
    } else if (envelope_first != 0U || envelope_height != 0U) {
      return Error(ErrorCode::MalformedRecord, "location carries an unexpected rack envelope")
          .with_subject(id.str());
    }

    PLR_TRY(generation, reader.u64());
    record.generation = LocationGeneration(generation);

    PLR_TRY(has_replaces, reader.u8());
    PLR_TRY(replaces_text, reader.text(kMaxIdentifierTextBytes, "location.replaces"));
    if (has_replaces != 0) {
      PLR_TRY(replaces_id, LocationId::parse(replaces_text));
      record.replaces = replaces_id;
    } else if (!replaces_text.empty()) {
      return Error(ErrorCode::MalformedRecord, "location carries an unexpected replacement value")
          .with_subject(id.str());
    }

    PLR_TRY(has_replaced_by, reader.u8());
    PLR_TRY(replaced_by_text, reader.text(kMaxIdentifierTextBytes, "location.replaced_by"));
    if (has_replaced_by != 0) {
      PLR_TRY(replaced_by_id, LocationId::parse(replaced_by_text));
      record.replaced_by = replaced_by_id;
    } else if (!replaced_by_text.empty()) {
      return Error(ErrorCode::MalformedRecord, "location carries an unexpected successor value")
          .with_subject(id.str());
    }

    PLR_TRY(provenance, read_provenance(reader, limits));
    record.provenance = provenance;

    PLR_TRY(alias_count, reader.u32());
    if (alias_count > limits.max_aliases_per_location) {
      return Error(ErrorCode::LimitExceeded,
                   "location declares more aliases than the configured maximum per location")
          .with_subject(id.str());
    }
    record.aliases.reserve(alias_count);
    for (std::uint32_t alias_index = 0; alias_index < alias_count; ++alias_index) {
      PLR_TRY(alias, reader.text(kMaxPathTextBytes, "location.alias"));
      record.aliases.push_back(std::move(alias));
    }

    PLR_TRY(move_count, reader.u32());
    if (move_count > limits.max_moves_per_location) {
      return Error(ErrorCode::LimitExceeded,
                   "location declares more move records than the configured maximum per location")
          .with_subject(id.str());
    }
    record.moves.reserve(move_count);
    for (std::uint32_t move_index = 0; move_index < move_count; ++move_index) {
      PLR_TRY(move, read_move(reader, limits));
      record.moves.push_back(std::move(move));
    }
  }

  PLR_TRY(replacement_count, reader.u32());
  if (replacement_count > limits.max_total_replacements) {
    return Error(ErrorCode::LimitExceeded,
                 "state declares more replacement records than the configured maximum")
        .with_subject(std::to_string(replacement_count));
  }
  model->replacements.reserve(replacement_count);
  for (std::uint32_t index = 0; index < replacement_count; ++index) {
    ReplacementRecord replacement;
    PLR_TRY(predecessor_text, reader.text(kMaxIdentifierTextBytes, "replacement.predecessor"));
    PLR_TRY(predecessor, LocationId::parse(predecessor_text));
    replacement.predecessor = predecessor;
    PLR_TRY(successor_text, reader.text(kMaxIdentifierTextBytes, "replacement.successor"));
    PLR_TRY(successor, LocationId::parse(successor_text));
    replacement.successor = successor;
    PLR_TRY(revision_value, reader.u64());
    replacement.revision = LocationRevision(revision_value);
    PLR_TRY(at_seconds, reader.i64());
    PLR_TRY(at_nanos, reader.u32());
    PLR_TRY(at, Timestamp::make(at_seconds, at_nanos));
    replacement.at = at;
    PLR_TRY(actor_text, reader.text(kMaxIdentifierTextBytes, "replacement.actor"));
    PLR_TRY(actor, ActorId::parse(actor_text));
    replacement.actor = actor;
    PLR_TRY(reason, reader.text(limits.max_reason_bytes, "replacement.reason"));
    replacement.reason = std::move(reason);
    model->replacements.push_back(std::move(replacement));
  }

  PLR_TRY(receipt_count, reader.u32());
  if (receipt_count > limits.max_operation_receipts) {
    return Error(ErrorCode::LimitExceeded,
                 "state declares more operation receipts than the configured maximum")
        .with_subject(std::to_string(receipt_count));
  }
  model->receipts.reserve(receipt_count);
  for (std::uint32_t index = 0; index < receipt_count; ++index) {
    OperationReceipt receipt;
    PLR_TRY(operation_text, reader.text(kMaxIdentifierTextBytes, "receipt.operation_id"));
    PLR_TRY(operation_id, OperationId::parse(operation_text));
    receipt.operation_id = operation_id;
    PLR_TRY(fingerprint, reader.text(kSha256Bytes * 2U, "receipt.fingerprint"));
    receipt.fingerprint = std::move(fingerprint);
    PLR_TRY(receipt_revision, reader.u64());
    receipt.revision = LocationRevision(receipt_revision);
    PLR_TRY(receipt_sequence, reader.u64());
    receipt.sequence = StateSequence(receipt_sequence);
    PLR_TRY(receipt_generation, reader.u64());
    receipt.generation = LocationGeneration(receipt_generation);
    PLR_TRY(has_successor, reader.u8());
    PLR_TRY(successor_generation, reader.u64());
    if (has_successor != 0) {
      receipt.successor_generation = LocationGeneration(successor_generation);
    } else if (successor_generation != 0U) {
      return Error(ErrorCode::MalformedRecord, "receipt carries an unexpected successor generation")
          .with_subject(operation_id.str());
    }
    PLR_TRY(affected_locations, reader.u32());
    receipt.affected_locations = affected_locations;
    PLR_TRY(affected_descendants, reader.u32());
    receipt.affected_descendants = affected_descendants;
    PLR_TRY(at_seconds, reader.i64());
    PLR_TRY(at_nanos, reader.u32());
    PLR_TRY(at, Timestamp::make(at_seconds, at_nanos));
    receipt.at = at;
    PLR_TRY(actor_text, reader.text(kMaxIdentifierTextBytes, "receipt.actor"));
    PLR_TRY(actor, ActorId::parse(actor_text));
    receipt.actor = actor;
    model->receipts.push_back(std::move(receipt));
  }

  if (!reader.exhausted()) {
    return Error(ErrorCode::CountMismatch, "state payload has trailing bytes after the last structure")
        .with_subject(std::to_string(reader.remaining()));
  }

  model->rebuild_indexes();
  PLR_CHECK(model->validate_shape());
  return SnapshotAccess::make(std::move(model));
}

}  // namespace internal

Result<std::string> encode_snapshot(const Snapshot& snapshot) {
  return internal::encode_model(SnapshotAccess::get(snapshot));
}

Result<Snapshot> decode_snapshot(std::string_view bytes) { return internal::decode_model_bytes(bytes); }

}  // namespace dccp::physical_location_registry
