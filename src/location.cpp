// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "dccp/physical_location_registry/location.hpp"

#include <algorithm>
#include <string>

namespace dccp::physical_location_registry {

LocationView::LocationView(LocationId id,
                           LocationKind kind,
                           std::optional<LocationId> parent,
                           AddressComponent component,
                           LocationPath path,
                           std::string label,
                           LifecycleState lifecycle,
                           std::optional<RackUnitCoordinate> unit,
                           std::optional<RackEnvelope> envelope,
                           LocationGeneration generation,
                           std::optional<LocationId> replaces,
                           std::optional<LocationId> replaced_by,
                           ProvenanceRecord provenance,
                           std::vector<MoveRecord> moves,
                           std::vector<std::string> aliases)
    : id_(std::move(id)),
      kind_(kind),
      parent_(std::move(parent)),
      component_(std::move(component)),
      path_(std::move(path)),
      label_(std::move(label)),
      lifecycle_(lifecycle),
      unit_(unit),
      envelope_(envelope),
      generation_(generation),
      replaces_(std::move(replaces)),
      replaced_by_(std::move(replaced_by)),
      provenance_(std::move(provenance)),
      moves_(std::move(moves)),
      aliases_(std::move(aliases)) {}

std::string LocationView::summary() const {
  std::string text;
  text.append(path_.to_string());
  text.append(" id=");
  text.append(id_.value());
  text.append(" kind=");
  text.append(location_kind_name(kind_));
  text.append(" lifecycle=");
  text.append(lifecycle_state_name(lifecycle_));
  text.append(" generation=");
  text.append(std::to_string(generation_.value()));
  if (!label_.empty()) {
    text.append(" label=\"");
    text.append(label_);
    text.push_back('"');
  }
  if (unit_.has_value()) {
    text.append(" unit=");
    text.append(unit_->to_string());
  }
  if (envelope_.has_value()) {
    text.append(" envelope=");
    text.append(envelope_->to_string());
  }
  if (replaces_.has_value()) {
    text.append(" replaces=");
    text.append(replaces_->value());
  }
  if (replaced_by_.has_value()) {
    text.append(" replaced-by=");
    text.append(replaced_by_->value());
  }
  if (!aliases_.empty()) {
    text.append(" aliases=");
    text.append(std::to_string(aliases_.size()));
  }
  return text;
}

std::string format_child_entry(const ChildEntry& entry) {
  std::string text;
  text.append(entry.path);
  text.append(" id=");
  text.append(entry.id.value());
  text.append(" kind=");
  text.append(location_kind_name(entry.kind));
  text.append(" lifecycle=");
  text.append(lifecycle_state_name(entry.lifecycle));
  text.append(" generation=");
  text.append(std::to_string(entry.generation.value()));
  if (!entry.label.empty()) {
    text.append(" label=\"");
    text.append(entry.label);
    text.push_back('"');
  }
  return text;
}

std::string_view resolution_kind_name(ResolutionKind kind) noexcept {
  switch (kind) {
    case ResolutionKind::CanonicalAddress:
      return "canonical-address";
    case ResolutionKind::Alias:
      return "alias";
  }
  return "unrecognized";
}

std::string format_statistics(const LocationStatistics& statistics) {
  std::string text;
  const auto line = [&text](std::string_view key, std::uint64_t value) {
    text.append(key);
    text.push_back('=');
    text.append(std::to_string(value));
    text.push_back('\n');
  };
  line("locations", statistics.locations);
  line("active", statistics.active);
  line("retired", statistics.retired);
  line("replaced", statistics.replaced);
  line("roots", statistics.roots);
  line("aliases", statistics.aliases);
  line("move-records", statistics.moves);
  line("replacement-records", statistics.replacements);
  line("max-depth-observed", statistics.max_depth_observed);
  line("operation-receipts", statistics.operation_receipts);
  for (std::size_t index = 0; index < kLocationKindCount; ++index) {
    text.append("kind.");
    text.append(location_kind_name(static_cast<LocationKind>(index)));
    text.push_back('=');
    text.append(std::to_string(statistics.by_kind[index]));
    text.push_back('\n');
  }
  return text;
}

}  // namespace dccp::physical_location_registry
