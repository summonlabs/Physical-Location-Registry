// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "support/synthetic.hpp"

#include <string>
#include <utility>

namespace plr_test {
namespace {

std::string decimal(std::uint64_t value) { return std::to_string(value); }

}  // namespace

Result<CreateLocationRequest> make_create(std::string_view id, LocationKind kind,
                                          std::string_view parent, std::string_view component,
                                          std::string_view label, const MutationContext& context) {
  return CreateLocationRequest::make(id, kind, parent, component, label, context);
}

Fixture::Fixture(std::shared_ptr<Registry> registry, std::uint64_t seed)
    : registry_(std::move(registry)), seed_(seed) {
  const std::string actor_text = "actor-" + decimal(seed % 1000U);
  const auto parsed = ActorId::parse(actor_text);
  actor_ = parsed.value();
}

MutationContext Fixture::context() const {
  MutationContext context;
  context.actor = actor_;
  const auto at = Timestamp::from_unix_seconds(static_cast<std::int64_t>(1700000000ULL + tick_));
  ++tick_;
  context.at = at.value();
  return context;
}

MutationContext Fixture::context_with(std::optional<LocationGeneration> generation,
                                      std::optional<LocationRevision> revision) const {
  MutationContext context = this->context();
  context.expected_generation = generation;
  context.expected_revision = revision;
  return context;
}

MutationContext Fixture::context_with_operation(std::string_view operation_id) const {
  MutationContext context = this->context();
  const auto parsed = OperationId::parse(operation_id);
  context.operation_id = parsed.value();
  return context;
}

std::string Fixture::next_component(std::string_view prefix) {
  ++counter_;
  std::string text(prefix);
  text.push_back('-');
  text.append(decimal(counter_));
  return text;
}

Result<LocationId> Fixture::facility(std::string_view component, std::string_view label) {
  const std::string id_text = "loc-" + next_component("facility");
  auto request = CreateLocationRequest::make(id_text, LocationKind::Facility, std::string(),
                                             component, label, context());
  if (!request.has_value()) {
    return request.error();
  }
  request.value().source = "synthetic-fixture";
  auto receipt = registry_->create_location(request.value());
  if (!receipt.has_value()) {
    return receipt.error();
  }
  created_.push_back(request.value().id);
  return request.value().id;
}

Result<LocationId> Fixture::add(const LocationId& parent, LocationKind kind,
                                std::string_view component, std::string_view label) {
  const std::string id_text = "loc-" + next_component("item");
  auto request = CreateLocationRequest::make(id_text, kind, parent.str(), component, label, context());
  if (!request.has_value()) {
    return request.error();
  }
  request.value().source = "synthetic-fixture";
  auto receipt = registry_->create_location(request.value());
  if (!receipt.has_value()) {
    return receipt.error();
  }
  created_.push_back(request.value().id);
  return request.value().id;
}

Result<LocationId> Fixture::add_rack(const LocationId& parent, std::string_view component,
                                     std::uint32_t height, std::string_view label) {
  const std::string id_text = "loc-" + next_component("rack");
  auto request = CreateLocationRequest::make(id_text, LocationKind::Rack, parent.str(), component,
                                             label, context());
  if (!request.has_value()) {
    return request.error();
  }
  auto envelope = RackEnvelope::with_height(height);
  if (!envelope.has_value()) {
    return envelope.error();
  }
  request.value().envelope = envelope.value();
  request.value().source = "synthetic-fixture";
  auto receipt = registry_->create_location(request.value());
  if (!receipt.has_value()) {
    return receipt.error();
  }
  created_.push_back(request.value().id);
  return request.value().id;
}

Result<LocationId> Fixture::add_unit(const LocationId& rack, std::uint32_t unit,
                                     std::string_view label) {
  const std::string id_text = "loc-" + next_component("unit");
  const std::string component = "U" + decimal(unit);
  auto request = CreateLocationRequest::make(id_text, LocationKind::RackUnit, rack.str(), component,
                                             label, context());
  if (!request.has_value()) {
    return request.error();
  }
  auto coordinate = RackUnitCoordinate::parse(unit);
  if (!coordinate.has_value()) {
    return coordinate.error();
  }
  request.value().unit = coordinate.value();
  request.value().source = "synthetic-fixture";
  auto receipt = registry_->create_location(request.value());
  if (!receipt.has_value()) {
    return receipt.error();
  }
  created_.push_back(request.value().id);
  return request.value().id;
}

Result<std::vector<LocationId>> Fixture::standard_facility(std::string_view facility_component,
                                                           std::uint32_t rooms, std::uint32_t rows,
                                                           std::uint32_t racks,
                                                           std::uint32_t units) {
  std::vector<LocationId> all;
  PLR_TRY(facility_id, facility(facility_component, "Facility " + std::string(facility_component)));
  all.push_back(facility_id);
  PLR_TRY(building, add(facility_id, LocationKind::Building, "BLDG-1", "Building 1"));
  all.push_back(building);
  for (std::uint32_t room_index = 0; room_index < rooms; ++room_index) {
    PLR_TRY(room, add(building, LocationKind::Room, "ROOM-" + decimal(room_index + 1), "Room"));
    all.push_back(room);
    for (std::uint32_t row_index = 0; row_index < rows; ++row_index) {
      PLR_TRY(row, add(room, LocationKind::Row, "ROW-" + decimal(row_index + 1), "Row"));
      all.push_back(row);
      for (std::uint32_t rack_index = 0; rack_index < racks; ++rack_index) {
        PLR_TRY(rack, add_rack(row, "RACK-" + decimal(rack_index + 1), 48, "Rack"));
        all.push_back(rack);
        for (std::uint32_t unit_index = 0; unit_index < units; ++unit_index) {
          PLR_TRY(unit, add_unit(rack, unit_index + 1, "Unit"));
          all.push_back(unit);
        }
      }
    }
  }
  return all;
}

}  // namespace plr_test
