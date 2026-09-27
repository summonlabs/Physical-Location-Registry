// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "model.hpp"

#include <algorithm>
#include <utility>

#include "dccp/physical_location_registry/digest.hpp"
#include "dccp/physical_location_registry/text.hpp"

namespace dccp::physical_location_registry {
namespace {

/// Depth of the ancestor chain, bounded so that a cyclic record set cannot make
/// validation loop forever.
constexpr std::uint32_t kAncestorWalkBound = kHardMaxDepth + 4U;

}  // namespace

// ---------------------------------------------------------------------------
// Snapshot value semantics
// ---------------------------------------------------------------------------

Snapshot::Snapshot(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

Snapshot::Snapshot(Snapshot&&) noexcept = default;

Snapshot& Snapshot::operator=(Snapshot&&) noexcept = default;

Snapshot::~Snapshot() = default;

Snapshot::Impl& SnapshotAccess::get(Snapshot& snapshot) noexcept { return *snapshot.impl_; }

const Snapshot::Impl& SnapshotAccess::get(const Snapshot& snapshot) noexcept { return *snapshot.impl_; }

Snapshot SnapshotAccess::make(std::unique_ptr<Snapshot::Impl> impl) { return Snapshot(std::move(impl)); }

Snapshot SnapshotAccess::clone(const Snapshot& snapshot) {
  auto copy = std::make_unique<Snapshot::Impl>(*snapshot.impl_);
  return Snapshot(std::move(copy));
}

// ---------------------------------------------------------------------------
// Model indexes and lookups
// ---------------------------------------------------------------------------

void Snapshot::Impl::rebuild_indexes() {
  children.clear();
  roots = ChildSet{};
  alias_index.clear();
  alias_folded_index.clear();
  receipt_index.clear();
  total_moves = 0;
  total_aliases = 0;

  for (const auto& entry : records) {
    const LocationId& id = entry.first;
    const LocationRecord& record = entry.second;
    total_moves += record.moves.size();
    total_aliases += static_cast<std::uint32_t>(record.aliases.size());
    for (const std::string& alias : record.aliases) {
      alias_index.emplace(alias, id);
      alias_folded_index.emplace(ascii_fold(alias), alias);
    }

    // A replaced location is retained for lineage but is not addressable: its
    // successor holds the address, so it must not appear in the child indexes.
    if (record.lifecycle == LifecycleState::Replaced) {
      continue;
    }
    ChildSet& set = record.parent.has_value() ? children[*record.parent] : roots;
    set.by_component.emplace(record.component.str(), id);
    set.by_folded_component.emplace(ascii_fold(record.component.value()), id);
  }

  for (const OperationReceipt& receipt : receipts) {
    receipt_index.emplace(receipt.operation_id, receipt);
  }

  for (auto iterator = children.begin(); iterator != children.end();) {
    if (iterator->second.empty()) {
      iterator = children.erase(iterator);
    } else {
      ++iterator;
    }
  }
}

const LocationRecord* Snapshot::Impl::find(const LocationId& id) const {
  const auto iterator = records.find(id);
  return iterator == records.end() ? nullptr : &iterator->second;
}

LocationRecord* Snapshot::Impl::find(const LocationId& id) {
  const auto iterator = records.find(id);
  return iterator == records.end() ? nullptr : &iterator->second;
}

const ChildSet* Snapshot::Impl::child_set(const std::optional<LocationId>& parent) const {
  if (!parent.has_value()) {
    return roots.empty() ? nullptr : &roots;
  }
  const auto iterator = children.find(*parent);
  return iterator == children.end() ? nullptr : &iterator->second;
}

std::optional<LocationId> Snapshot::Impl::child_by_component(const LocationId& parent,
                                                             std::string_view component) const {
  const ChildSet* set = child_set(parent);
  if (set == nullptr) {
    return std::nullopt;
  }
  const auto iterator = set->by_component.find(component);
  if (iterator == set->by_component.end()) {
    return std::nullopt;
  }
  return iterator->second;
}

std::optional<LocationId> Snapshot::Impl::child_by_folded_component(const LocationId& parent,
                                                                   std::string_view folded) const {
  const ChildSet* set = child_set(parent);
  if (set == nullptr) {
    return std::nullopt;
  }
  const auto iterator = set->by_folded_component.find(folded);
  if (iterator == set->by_folded_component.end()) {
    return std::nullopt;
  }
  return iterator->second;
}

// ---------------------------------------------------------------------------
// Path construction and resolution
// ---------------------------------------------------------------------------

Result<LocationPath> Snapshot::Impl::path_of(const LocationRecord& record) const {
  std::vector<AddressComponent> components;
  const LocationRecord* current = &record;
  for (std::uint32_t step = 0; step <= kAncestorWalkBound; ++step) {
    components.push_back(current->component);
    if (!current->parent.has_value()) {
      std::reverse(components.begin(), components.end());
      return LocationPath::from_components(std::move(components), limits);
    }
    const LocationRecord* parent = find(*current->parent);
    if (parent == nullptr) {
      return Error(ErrorCode::MalformedRecord, "location references a parent that does not exist")
          .with_subject(current->parent->str());
    }
    current = parent;
  }
  return Error(ErrorCode::TraversalDepthExceeded,
               "ancestor chain exceeds the maximum representable depth")
      .with_subject(record.id.str());
}

Result<LocationPath> Snapshot::Impl::path_of(const LocationId& id) const {
  const LocationRecord* record = find(id);
  if (record == nullptr) {
    return Error(ErrorCode::NotFound, "no location with this identity exists").with_subject(id.str());
  }
  return path_of(*record);
}

Result<LocationId> Snapshot::Impl::resolve_canonical(const LocationPath& path) const {
  if (path.empty()) {
    return Error(ErrorCode::NotFound, "the empty root path is not the address of any location")
        .with_subject("/");
  }
  const auto root = roots.by_component.find(path.component(0).str());
  if (root == roots.by_component.end()) {
    return Error(ErrorCode::NotFound, "no location is addressed by this facility component")
        .with_subject(path.to_string());
  }
  LocationId current = root->second;
  for (std::size_t index = 1; index < path.depth(); ++index) {
    const ChildSet* set = child_set(current);
    if (set == nullptr) {
      return Error(ErrorCode::NotFound,
                   "the path continues past a location that has no children at this depth")
          .with_subject(path.to_string());
    }
    const auto child = set->by_component.find(path.component(index).str());
    if (child == set->by_component.end()) {
      return Error(ErrorCode::NotFound, "no child of the addressed location has this component")
          .with_subject(path.to_string());
    }
    current = child->second;
  }
  return current;
}

Result<std::vector<LocationId>> Snapshot::Impl::subtree_ids(const LocationId& root_id,
                                                            std::uint32_t max_depth) const {
  if (find(root_id) == nullptr) {
    return Error(ErrorCode::NotFound, "no location with this identity exists").with_subject(root_id.str());
  }
  std::vector<LocationId> ordered;
  struct Frame {
    LocationId id;
    std::uint32_t depth;
  };
  std::vector<Frame> stack;
  stack.push_back(Frame{root_id, 0});
  while (!stack.empty()) {
    const Frame frame = stack.back();
    stack.pop_back();
    ordered.push_back(frame.id);
    if (ordered.size() > static_cast<std::size_t>(limits.max_traversal_nodes)) {
      return Error(ErrorCode::LimitExceeded,
                   "traversal exceeds the configured maximum of " +
                       std::to_string(limits.max_traversal_nodes) + " locations")
          .with_subject(root_id.str());
    }
    if (frame.depth >= max_depth) {
      continue;
    }
    const ChildSet* set = child_set(frame.id);
    if (set == nullptr) {
      continue;
    }
    for (auto iterator = set->by_component.rbegin(); iterator != set->by_component.rend(); ++iterator) {
      stack.push_back(Frame{iterator->second, frame.depth + 1U});
    }
  }
  return ordered;
}

std::uint32_t Snapshot::Impl::move_record_count() const {
  return total_moves > UINT32_MAX ? UINT32_MAX : static_cast<std::uint32_t>(total_moves);
}

std::uint32_t Snapshot::Impl::alias_count() const { return total_aliases; }

std::uint32_t Snapshot::Impl::max_depth_observed() const {
  std::uint32_t maximum = 0;
  for (const auto& entry : records) {
    std::uint32_t depth = 0;
    const LocationRecord* current = &entry.second;
    for (std::uint32_t step = 0; step <= kAncestorWalkBound; ++step) {
      ++depth;
      if (!current->parent.has_value()) {
        break;
      }
      const LocationRecord* parent = find(*current->parent);
      if (parent == nullptr) {
        break;
      }
      current = parent;
    }
    maximum = depth > maximum ? depth : maximum;
  }
  return maximum;
}

std::uint64_t Snapshot::Impl::text_bytes() const {
  std::uint64_t bytes = 0;
  for (const auto& entry : records) {
    const LocationRecord& record = entry.second;
    bytes += record.id.value().size() + record.component.value().size() + record.label.size() +
             record.provenance.source.size();
    for (const MoveRecord& move : record.moves) {
      bytes += move.from_path.size() + move.to_path.size() + move.from_component.size() +
               move.to_component.size() + move.reason.size();
    }
    for (const std::string& alias : record.aliases) {
      bytes += alias.size();
    }
  }
  for (const OperationReceipt& receipt : receipts) {
    bytes += receipt.operation_id.value().size() + receipt.fingerprint.size();
  }
  return bytes;
}

// ---------------------------------------------------------------------------
// Validation
// ---------------------------------------------------------------------------

Result<void> Snapshot::Impl::validate_state_bounds() const {
  if (records.size() > static_cast<std::size_t>(limits.max_locations)) {
    return Error(ErrorCode::LimitExceeded,
                 "location count exceeds the configured maximum of " +
                     std::to_string(limits.max_locations))
        .with_subject(std::to_string(records.size()));
  }
  if (total_aliases != alias_index.size()) {
    return Error(ErrorCode::InternalError, "alias index and alias bindings disagree")
        .with_subject(std::to_string(alias_index.size()) + " vs " + std::to_string(total_aliases));
  }
  if (alias_folded_index.size() != alias_index.size()) {
    return Error(ErrorCode::AddressLookAlike,
                 "two alias addresses differ only by ASCII letter case")
        .with_subject(std::to_string(alias_index.size()) + " vs " +
                      std::to_string(alias_folded_index.size()));
  }
  if (roots.by_component.size() != roots.by_folded_component.size()) {
    return Error(ErrorCode::AddressLookAlike,
                 "two facility addresses differ only by ASCII letter case");
  }
  if (alias_index.size() > static_cast<std::size_t>(limits.max_total_aliases)) {
    return Error(ErrorCode::LimitExceeded,
                 "alias count exceeds the configured maximum of " +
                     std::to_string(limits.max_total_aliases))
        .with_subject(std::to_string(alias_index.size()));
  }
  if (total_moves > static_cast<std::uint64_t>(limits.max_total_moves)) {
    return Error(ErrorCode::LimitExceeded,
                 "move record count exceeds the configured maximum of " +
                     std::to_string(limits.max_total_moves))
        .with_subject(std::to_string(total_moves));
  }
  if (replacements.size() > static_cast<std::size_t>(limits.max_total_replacements)) {
    return Error(ErrorCode::LimitExceeded,
                 "replacement record count exceeds the configured maximum of " +
                     std::to_string(limits.max_total_replacements))
        .with_subject(std::to_string(replacements.size()));
  }
  if (receipts.size() > static_cast<std::size_t>(limits.max_operation_receipts)) {
    return Error(ErrorCode::LimitExceeded,
                 "operation receipt count exceeds the configured maximum of " +
                     std::to_string(limits.max_operation_receipts))
        .with_subject(std::to_string(receipts.size()));
  }
  if (receipt_index.size() != receipts.size()) {
    return Error(ErrorCode::InternalError, "operation receipt index disagrees with the receipt ring")
        .with_subject(std::to_string(receipt_index.size()) + " vs " + std::to_string(receipts.size()));
  }
  if (revision.value() == 0 && !records.empty()) {
    return Error(ErrorCode::InternalError, "a non-empty state cannot sit at revision zero");
  }
  return ok();
}

Result<void> Snapshot::Impl::validate_shape() const {
  PLR_CHECK(limits.validate());
  if (store_id.empty()) {
    return Error(ErrorCode::MalformedRecord, "store identity is missing");
  }
  if (sequence.value() < StateSequence::kFirstPublication) {
    return Error(ErrorCode::MalformedRecord, "state sequence must be at least 1")
        .with_subject(std::to_string(sequence.value()));
  }
  PLR_CHECK(validate_state_bounds());

  std::size_t indexed_children = roots.by_component.size();
  for (const auto& entry : children) {
    indexed_children += entry.second.by_component.size();
  }
  std::size_t replaced_count = 0;
  for (const auto& entry : records) {
    if (entry.second.lifecycle == LifecycleState::Replaced) {
      ++replaced_count;
    }
  }
  if (indexed_children + replaced_count != records.size()) {
    return Error(ErrorCode::InternalError,
                 "child index does not cover every addressable location")
        .with_subject(std::to_string(indexed_children) + "+" + std::to_string(replaced_count) +
                      " vs " + std::to_string(records.size()));
  }

  for (const auto& entry : records) {
    const LocationId& id = entry.first;
    const LocationRecord& record = entry.second;

    if (record.id != id) {
      return Error(ErrorCode::InternalError, "record key does not match its identity")
          .with_subject(id.str());
    }
    if (!record.generation.published()) {
      return Error(ErrorCode::MalformedRecord, "stored location has no published generation")
          .with_subject(id.str());
    }
    if (record.component.value().size() >
        static_cast<std::size_t>(limits.max_address_component_bytes)) {
      return Error(ErrorCode::AddressComponentTooLong,
                   "stored address component exceeds the configured maximum")
          .with_subject(id.str());
    }
    if (!is_valid_address_component(record.component.value())) {
      return Error(ErrorCode::MalformedAddressComponent, "stored address component is malformed")
          .with_subject(id.str());
    }
    if (record.label.size() > static_cast<std::size_t>(limits.max_label_bytes) ||
        !is_valid_label(record.label)) {
      return Error(ErrorCode::MalformedLabel, "stored label is malformed or too long")
          .with_subject(id.str());
    }
    if (record.unit.has_value() && !kind_may_carry_unit_coordinate(record.kind)) {
      return Error(ErrorCode::RackUnitNotAllowed, "this kind of location cannot carry a unit coordinate")
          .with_subject(id.str());
    }
    if (record.envelope.has_value() && !kind_may_carry_envelope(record.kind)) {
      return Error(ErrorCode::RackEnvelopeInvalid, "this kind of location cannot carry a rack envelope")
          .with_subject(id.str());
    }
    if (record.provenance.created_by.empty()) {
      return Error(ErrorCode::MalformedRecord, "stored location has no creating actor")
          .with_subject(id.str());
    }
    if (record.provenance.source.size() > static_cast<std::size_t>(limits.max_source_bytes) ||
        !is_valid_source(record.provenance.source)) {
      return Error(ErrorCode::MalformedText, "stored provenance source is malformed or too long")
          .with_subject(id.str());
    }
    if (record.provenance.created_revision > revision ||
        record.provenance.last_modified_revision > revision ||
        record.provenance.last_modified_revision < record.provenance.created_revision) {
      return Error(ErrorCode::MalformedRecord, "stored provenance revision is inconsistent")
          .with_subject(id.str());
    }
    if (record.moves.size() > static_cast<std::size_t>(limits.max_moves_per_location)) {
      return Error(ErrorCode::LimitExceeded, "stored move history exceeds the configured maximum")
          .with_subject(id.str());
    }
    for (const MoveRecord& move : record.moves) {
      if (move.revision > revision) {
        return Error(ErrorCode::MalformedRecord, "stored move record cites a future revision")
            .with_subject(id.str());
      }
      if (move.generation_before.published() && move.generation_after <= move.generation_before) {
        return Error(ErrorCode::MalformedRecord, "stored move record does not advance the generation")
            .with_subject(id.str());
      }
      if (move.actor.empty()) {
        return Error(ErrorCode::MalformedRecord, "stored move record has no actor").with_subject(id.str());
      }
      if (move.reason.size() > static_cast<std::size_t>(limits.max_reason_bytes) ||
          !is_valid_reason(move.reason)) {
        return Error(ErrorCode::MalformedText, "stored move reason is malformed or too long")
            .with_subject(id.str());
      }
      PLR_TRY(from_path, internal::parse_path_text(move.from_path, limits, ErrorCode::MalformedPath));
      PLR_TRY(to_path, internal::parse_path_text(move.to_path, limits, ErrorCode::MalformedPath));
      if (from_path.empty() || to_path.empty()) {
        return Error(ErrorCode::MalformedPath, "stored move record cites an empty address")
            .with_subject(id.str());
      }
    }

    std::string previous_alias;
    for (const std::string& alias : record.aliases) {
      if (!previous_alias.empty() && !(previous_alias < alias)) {
        return Error(ErrorCode::MalformedRecord, "stored aliases are not in canonical order")
            .with_subject(id.str());
      }
      previous_alias = alias;
      PLR_TRY(alias_path, internal::parse_path_text(alias, limits, ErrorCode::MalformedPath));
      if (alias_path.empty()) {
        return Error(ErrorCode::MalformedPath, "an alias cannot be the empty root path")
            .with_subject(id.str());
      }
      const auto binding = alias_index.find(alias);
      if (binding == alias_index.end() || binding->second != id) {
        return Error(ErrorCode::InternalError, "alias index disagrees with stored aliases")
            .with_subject(alias);
      }
      const auto canonical_owner = resolve_canonical(alias_path);
      if (canonical_owner.has_value()) {
        if (canonical_owner.value() == id) {
          return Error(ErrorCode::AliasRedundant,
                       "an alias duplicates the location's own canonical address")
              .with_subject(alias);
        }
        return Error(ErrorCode::AliasConflictsWithAddress,
                     "an alias is also the current address of another location")
            .with_subject(alias);
      }
    }

    PLR_TRY(path, path_of(record));
    if (path.depth() > static_cast<std::size_t>(limits.max_depth)) {
      return Error(ErrorCode::PathTooDeep, "stored location is deeper than the configured maximum")
          .with_subject(id.str());
    }
    if (path.byte_length() > static_cast<std::size_t>(limits.max_path_bytes)) {
      return Error(ErrorCode::PathTooLong, "stored location address exceeds the configured maximum")
          .with_subject(id.str());
    }

    if (record.parent.has_value()) {
      const LocationRecord* parent = find(*record.parent);
      if (parent == nullptr) {
        return Error(ErrorCode::MalformedRecord, "stored location references a missing parent")
            .with_subject(id.str());
      }
      if (!is_legal_child_kind(parent->kind, record.kind)) {
        return Error(ErrorCode::InvalidKindForParent,
                     std::string("stored containment is not legal: ") +
                         std::string(location_kind_name(record.kind)) + " under " +
                         std::string(location_kind_name(parent->kind)))
            .with_subject(id.str());
      }
      // A replaced location is a retained historical record: it keeps its place
      // in the hierarchy for lineage, but it is not addressable and it does not
      // constrain the lifecycle of its parent or of its own parent chain.
      if (record.lifecycle != LifecycleState::Replaced) {
        if (parent->lifecycle == LifecycleState::Replaced) {
          return Error(ErrorCode::ParentNotActive, "stored location sits under a replaced parent")
              .with_subject(id.str());
        }
        if (record.lifecycle == LifecycleState::Active &&
            parent->lifecycle != LifecycleState::Active) {
          return Error(ErrorCode::ParentNotActive, "an active location sits under a non-active parent")
              .with_subject(id.str());
        }
      }
    } else if (!kind_may_be_root(record.kind)) {
      return Error(ErrorCode::KindMustHaveParent,
                   std::string("a ") + std::string(location_kind_name(record.kind)) +
                       " must be contained by a parent")
          .with_subject(id.str());
    }

    if (record.lifecycle == LifecycleState::Replaced) {
      if (!record.replaced_by.has_value()) {
        return Error(ErrorCode::MalformedRecord, "a replaced location must name its successor")
            .with_subject(id.str());
      }
      const LocationRecord* successor = find(*record.replaced_by);
      if (successor == nullptr || successor->replaces != id) {
        return Error(ErrorCode::MalformedRecord, "replacement lineage is not symmetric")
            .with_subject(id.str());
      }
    } else if (record.replaced_by.has_value()) {
      return Error(ErrorCode::MalformedRecord, "only a replaced location may name a successor")
          .with_subject(id.str());
    }
    if (record.replaces.has_value()) {
      const LocationRecord* predecessor = find(*record.replaces);
      if (predecessor == nullptr || predecessor->replaced_by != id) {
        return Error(ErrorCode::MalformedRecord, "replacement lineage is not symmetric")
            .with_subject(id.str());
      }
      // Walk the lineage forward with a bound: a cycle would otherwise make the
      // chain unrepresentable.
      const LocationRecord* cursor = &record;
      for (std::uint32_t step = 0; step <= kAncestorWalkBound; ++step) {
        if (!cursor->replaced_by.has_value()) {
          break;
        }
        const LocationRecord* next = find(*cursor->replaced_by);
        if (next == nullptr) {
          return Error(ErrorCode::MalformedRecord, "replacement lineage references a missing location")
              .with_subject(id.str());
        }
        if (next->id == record.id) {
          return Error(ErrorCode::ReplacementCycle, "replacement lineage is cyclic").with_subject(id.str());
        }
        cursor = next;
        if (step == kAncestorWalkBound) {
          return Error(ErrorCode::ReplacementCycle, "replacement lineage exceeds the representable length")
              .with_subject(id.str());
        }
      }
    }
  }

  for (const ReplacementRecord& replacement : replacements) {
    const LocationRecord* predecessor = find(replacement.predecessor);
    const LocationRecord* successor = find(replacement.successor);
    if (predecessor == nullptr || successor == nullptr) {
      return Error(ErrorCode::MalformedRecord, "replacement record references a missing location")
          .with_subject(replacement.predecessor.str());
    }
    if (predecessor->replaced_by != successor->id || successor->replaces != predecessor->id) {
      return Error(ErrorCode::MalformedRecord, "replacement record does not match the lineage")
          .with_subject(replacement.predecessor.str());
    }
    if (replacement.revision > revision) {
      return Error(ErrorCode::MalformedRecord, "replacement record cites a future revision")
          .with_subject(replacement.predecessor.str());
    }
    if (replacement.actor.empty()) {
      return Error(ErrorCode::MalformedRecord, "replacement record has no actor")
          .with_subject(replacement.predecessor.str());
    }
  }

  LocationRevision previous_receipt_revision;
  bool first_receipt = true;
  for (const OperationReceipt& receipt : receipts) {
    if (receipt.operation_id.empty()) {
      return Error(ErrorCode::MalformedRecord, "operation receipt has no identity");
    }
    if (!is_lower_hex_sha256(receipt.fingerprint)) {
      return Error(ErrorCode::MalformedRecord, "operation receipt fingerprint is malformed")
          .with_subject(receipt.operation_id.str());
    }
    if (receipt.revision > revision) {
      return Error(ErrorCode::MalformedRecord, "operation receipt cites a future revision")
          .with_subject(receipt.operation_id.str());
    }
    if (!first_receipt && receipt.revision < previous_receipt_revision) {
      return Error(ErrorCode::MalformedRecord, "operation receipts are not in commit order")
          .with_subject(receipt.operation_id.str());
    }
    previous_receipt_revision = receipt.revision;
    first_receipt = false;
  }

  std::vector<std::string> keys;
  keys.reserve(alias_index.size());
  for (const auto& entry : alias_index) {
    keys.push_back(entry.first);
  }
  for (std::size_t index = 1; index < keys.size(); ++index) {
    if (!(keys[index - 1] < keys[index])) {
      return Error(ErrorCode::AliasConflict, "alias index is not strictly ordered")
          .with_subject(keys[index]);
    }
  }
  return ok();
}

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

namespace internal {

LocationView make_view(const LocationRecord& record, const LocationPath& path) {
  return LocationView(record.id, record.kind, record.parent, record.component, path, record.label,
                      record.lifecycle, record.unit, record.envelope, record.generation,
                      record.replaces, record.replaced_by, record.provenance, record.moves,
                      record.aliases);
}

ChildEntry make_child_entry(const LocationRecord& record, const LocationPath& path) {
  ChildEntry entry;
  entry.id = record.id;
  entry.kind = record.kind;
  entry.component = record.component.str();
  entry.label = record.label;
  entry.lifecycle = record.lifecycle;
  entry.generation = record.generation;
  entry.path = path.to_string();
  return entry;
}

Result<LocationPath> parse_path_text(std::string_view text, const Limits& limits, ErrorCode failure_code) {
  auto parsed = LocationPath::parse(text, limits);
  if (parsed.has_value()) {
    return parsed;
  }
  return Error(failure_code, std::string("stored address text is not a canonical absolute path: ") +
                                 std::string(parsed.error().message()))
      .with_subject(std::string(text.substr(0, 160)));
}

Result<std::uint32_t> count_descendants(const Snapshot::Impl& model,
                                        const LocationId& root_id,
                                        std::uint32_t max_depth) {
  PLR_TRY(ids, model.subtree_ids(root_id, max_depth));
  return static_cast<std::uint32_t>(ids.size() - 1);
}

Result<void> validate_text_field(std::string_view text,
                                 std::uint32_t limit,
                                 bool allow_empty,
                                 ErrorCode too_long_code,
                                 ErrorCode malformed_code,
                                 const char* field_name,
                                 std::string_view syntax_help) {
  if (text.empty()) {
    if (allow_empty) {
      return ok();
    }
    return Error(malformed_code, std::string(field_name) + " must not be empty");
  }
  if (text.size() > static_cast<std::size_t>(limit)) {
    return Error(too_long_code, std::string(field_name) + " is longer than the configured maximum of " +
                                    std::to_string(limit) + " bytes")
        .with_subject(std::string(text.substr(0, 160)));
  }
  if (!is_valid_label(text)) {
    return Error(malformed_code, std::string(field_name) + " is malformed: " + std::string(syntax_help))
        .with_subject(std::string(text.substr(0, 160)));
  }
  return ok();
}

void index_alias(Snapshot::Impl& model, std::string_view alias, const LocationId& id) {
  model.alias_index.emplace(std::string(alias), id);
  model.alias_folded_index.emplace(ascii_fold(alias), std::string(alias));
}

void unindex_alias(Snapshot::Impl& model, std::string_view alias) {
  model.alias_index.erase(std::string(alias));
  model.alias_folded_index.erase(ascii_fold(alias));
}

}  // namespace internal

// ---------------------------------------------------------------------------
// Public queries
// ---------------------------------------------------------------------------

const StoreId& Snapshot::store_id() const noexcept { return impl_->store_id; }
LocationRevision Snapshot::revision() const noexcept { return impl_->revision; }
StateSequence Snapshot::sequence() const noexcept { return impl_->sequence; }
WriterEpoch Snapshot::epoch() const noexcept { return impl_->epoch; }
const Limits& Snapshot::limits() const noexcept { return impl_->limits; }

std::uint32_t Snapshot::location_count() const noexcept {
  return impl_->records.size() > UINT32_MAX ? UINT32_MAX
                                            : static_cast<std::uint32_t>(impl_->records.size());
}

bool Snapshot::empty() const noexcept { return impl_->records.empty(); }

Result<LocationView> Snapshot::find(const LocationId& id) const {
  const LocationRecord* record = impl_->find(id);
  if (record == nullptr) {
    return Error(ErrorCode::NotFound, "no location with this identity exists").with_subject(id.str());
  }
  PLR_TRY(path, impl_->path_of(*record));
  return internal::make_view(*record, path);
}

Result<ResolutionResult> Snapshot::resolve(const LocationPath& path, ResolutionMode mode) const {
  ResolutionResult result;
  const LocationRecord* record = nullptr;
  const auto canonical = impl_->resolve_canonical(path);
  if (canonical.has_value()) {
    record = impl_->find(canonical.value());
    result.id = canonical.value();
    result.kind = ResolutionKind::CanonicalAddress;
  } else {
    const auto binding = impl_->alias_index.find(path.to_string());
    if (binding == impl_->alias_index.end()) {
      return canonical.error();
    }
    record = impl_->find(binding->second);
    if (record == nullptr) {
      return Error(ErrorCode::InternalError, "alias index references a location that does not exist")
          .with_subject(path.to_string());
    }
    result.id = binding->second;
    result.kind = ResolutionKind::Alias;
  }

  if (record->lifecycle == LifecycleState::Replaced) {
    return Error(ErrorCode::NotFound,
                 std::string(result.kind == ResolutionKind::Alias ? "alias" : "address") +
                     " belongs to a location that was replaced; resolve the successor instead")
        .with_subject(path.to_string());
  }
  if (record->lifecycle == LifecycleState::Retired && mode == ResolutionMode::CurrentOnly) {
    return Error(ErrorCode::NotFound,
                 std::string(result.kind == ResolutionKind::Alias ? "alias" : "address") +
                     " belongs to a retired location; ask for retired locations explicitly")
        .with_subject(path.to_string());
  }

  PLR_TRY(current, impl_->path_of(*record));
  result.canonical_path = current;
  result.lifecycle = record->lifecycle;
  result.generation = record->generation;
  return result;
}

Result<LocationPath> Snapshot::path_of(const LocationId& id) const { return impl_->path_of(id); }

Result<std::vector<ChildEntry>> Snapshot::children(const LocationId& id) const {
  if (impl_->find(id) == nullptr) {
    return Error(ErrorCode::NotFound, "no location with this identity exists").with_subject(id.str());
  }
  std::vector<ChildEntry> entries;
  const ChildSet* set = impl_->child_set(id);
  if (set == nullptr) {
    return entries;
  }
  entries.reserve(set->by_component.size());
  for (const auto& entry : set->by_component) {
    const LocationRecord* child = impl_->find(entry.second);
    if (child == nullptr) {
      return Error(ErrorCode::InternalError, "child index references a location that does not exist")
          .with_subject(entry.first);
    }
    PLR_TRY(path, impl_->path_of(*child));
    entries.push_back(internal::make_child_entry(*child, path));
  }
  return entries;
}

Result<std::vector<ChildEntry>> Snapshot::descendants(const LocationId& id,
                                                      std::uint32_t max_depth) const {
  PLR_TRY(ids, impl_->subtree_ids(id, max_depth));
  std::vector<ChildEntry> entries;
  entries.reserve(ids.size() > 0 ? ids.size() - 1 : 0);
  for (std::size_t index = 1; index < ids.size(); ++index) {
    const LocationRecord* record = impl_->find(ids[index]);
    if (record == nullptr) {
      return Error(ErrorCode::InternalError, "subtree walk returned an unknown location")
          .with_subject(ids[index].str());
    }
    PLR_TRY(path, impl_->path_of(*record));
    entries.push_back(internal::make_child_entry(*record, path));
  }
  return entries;
}

Result<std::vector<ChildEntry>> Snapshot::roots() const {
  std::vector<ChildEntry> entries;
  entries.reserve(impl_->roots.by_component.size());
  for (const auto& entry : impl_->roots.by_component) {
    const LocationRecord* record = impl_->find(entry.second);
    if (record == nullptr) {
      return Error(ErrorCode::InternalError, "root index references a location that does not exist")
          .with_subject(entry.first);
    }
    PLR_TRY(path, impl_->path_of(*record));
    entries.push_back(internal::make_child_entry(*record, path));
  }
  return entries;
}

Result<std::vector<AliasBinding>> Snapshot::aliases() const {
  std::vector<AliasBinding> bindings;
  bindings.reserve(impl_->alias_index.size());
  for (const auto& entry : impl_->alias_index) {
    const LocationRecord* record = impl_->find(entry.second);
    if (record == nullptr) {
      return Error(ErrorCode::InternalError, "alias index references a location that does not exist")
          .with_subject(entry.first);
    }
    PLR_TRY(path, impl_->path_of(*record));
    AliasBinding binding;
    binding.alias = entry.first;
    binding.id = entry.second;
    binding.canonical_path = path;
    binding.lifecycle = record->lifecycle;
    binding.generation = record->generation;
    bindings.push_back(std::move(binding));
  }
  return bindings;
}

Result<std::vector<LocationView>> Snapshot::list(const ListOptions& options) const {
  std::vector<std::pair<std::string, LocationView>> ordered;
  const std::size_t bound = static_cast<std::size_t>(options.max_nodes);

  const auto consider = [&](const LocationRecord& record) -> Result<void> {
    if (options.kind.has_value() && record.kind != *options.kind) {
      return ok();
    }
    if (options.lifecycle.has_value() && record.lifecycle != *options.lifecycle) {
      return ok();
    }
    PLR_TRY(path, impl_->path_of(record));
    if (ordered.size() >= bound) {
      return Error(ErrorCode::LimitExceeded,
                   "listing exceeds the requested maximum of " + std::to_string(bound) + " entries")
          .with_subject(record.id.str());
    }
    ordered.emplace_back(path.to_string(), internal::make_view(record, path));
    return ok();
  };

  if (options.root.has_value()) {
    const LocationId& root_id = *options.root;
    PLR_TRY(ids, impl_->subtree_ids(root_id, impl_->limits.max_depth));
    for (const LocationId& id : ids) {
      const LocationRecord* record = impl_->find(id);
      if (record == nullptr) {
        return Error(ErrorCode::InternalError, "subtree walk returned an unknown location")
            .with_subject(id.str());
      }
      PLR_CHECK(consider(*record));
    }
  } else {
    for (const auto& entry : impl_->records) {
      PLR_CHECK(consider(entry.second));
    }
  }

  std::sort(ordered.begin(), ordered.end(),
            [](const auto& left, const auto& right) { return left.first < right.first; });

  std::vector<LocationView> views;
  views.reserve(ordered.size());
  for (auto& entry : ordered) {
    views.push_back(std::move(entry.second));
  }
  return views;
}

LocationStatistics Snapshot::statistics() const {
  LocationStatistics statistics;
  statistics.locations = location_count();
  for (const auto& entry : impl_->records) {
    const LocationRecord& record = entry.second;
    switch (record.lifecycle) {
      case LifecycleState::Active:
        ++statistics.active;
        break;
      case LifecycleState::Retired:
        ++statistics.retired;
        break;
      case LifecycleState::Replaced:
        ++statistics.replaced;
        break;
    }
    const std::size_t kind_index = static_cast<std::size_t>(static_cast<unsigned>(record.kind));
    if (kind_index < kLocationKindCount) {
      ++statistics.by_kind[kind_index];
    }
    if (!record.parent.has_value()) {
      ++statistics.roots;
    }
  }
  statistics.aliases = impl_->alias_count();
  statistics.moves = impl_->move_record_count();
  statistics.replacements = static_cast<std::uint32_t>(impl_->replacements.size());
  statistics.max_depth_observed = impl_->max_depth_observed();
  statistics.operation_receipts = static_cast<std::uint32_t>(impl_->receipts.size());
  return statistics;
}

const std::vector<ReplacementRecord>& Snapshot::replacements() const { return impl_->replacements; }

const std::vector<OperationReceipt>& Snapshot::operation_receipts() const { return impl_->receipts; }

Result<std::string> Snapshot::canonical_digest() const {
  PLR_TRY(bytes, encode_snapshot(*this));
  return Sha256::hex(bytes);
}

}  // namespace dccp::physical_location_registry