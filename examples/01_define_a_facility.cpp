// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Example 01: define a facility hierarchy and read it back.
//
// Shows the create path, canonical address construction, deterministic child
// order and the aggregate view of the state.

#include <filesystem>
#include <iostream>
#include <memory>
#include <string>

#include "dccp/physical_location_registry/physical_location_registry.hpp"

namespace {

using namespace dccp::physical_location_registry;

/// Creates a throwaway store directory that is removed when the example exits.
class ScratchStore {
 public:
  ScratchStore() {
    std::error_code error;
    const auto base = std::filesystem::temp_directory_path(error);
    path_ = (error ? std::filesystem::path(".") : base) / "plr-example-01";
    std::filesystem::remove_all(path_, error);
  }
  ~ScratchStore() {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
  }
  const std::filesystem::path& path() const { return path_; }

 private:
  std::filesystem::path path_;
};

MutationContext context_for(std::string_view actor, std::int64_t seconds) {
  MutationContext context;
  context.actor = ActorId::parse(actor).value();
  context.at = Timestamp::from_unix_seconds(seconds).value();
  context.reason = "example 01";
  return context;
}

}  // namespace

int main() {
  ScratchStore scratch;
  RegistryOpenOptions options;
  options.mode = OpenMode::ReadWrite;
  options.create_if_missing = true;

  auto created = Registry::create(scratch.path(), options);
  if (!created.has_value()) {
    std::cerr << created.error().to_string() << std::endl;
    return 1;
  }
  std::shared_ptr<Registry> registry = created.value();

  const struct {
    const char* id;
    LocationKind kind;
    const char* parent;
    const char* component;
    const char* label;
  } plan[] = {
      {"loc-facility", LocationKind::Facility, "", "FAC1", "Ashburn facility"},
      {"loc-building", LocationKind::Building, "loc-facility", "BLDG-A", "Building A"},
      {"loc-room", LocationKind::Room, "loc-building", "ROOM-101", "Hall 101"},
      {"loc-row", LocationKind::Row, "loc-room", "ROW-03", "Row 3"},
      {"loc-rack", LocationKind::Rack, "loc-row", "RACK-07", "Rack 7"},
      {"loc-unit", LocationKind::RackUnit, "loc-rack", "U12", "Unit 12"},
  };

  std::int64_t clock = 1767225600;  // 2026-01-01T00:00:00Z
  for (const auto& step : plan) {
    auto request = CreateLocationRequest::make(step.id, step.kind, step.parent, step.component,
                                               step.label, context_for("planner", clock));
    if (!request.has_value()) {
      std::cerr << request.error().to_string() << std::endl;
      return 1;
    }
    request.value().source = "example-01";
    if (std::string_view(step.component) == "RACK-07") {
      request.value().envelope = RackEnvelope::with_height(48).value();
    }
    if (std::string_view(step.component) == "U12") {
      request.value().unit = RackUnitCoordinate::parse(12).value();
    }
    const auto receipt = registry->create_location(request.value());
    if (!receipt.has_value()) {
      std::cerr << receipt.error().to_string() << std::endl;
      return 1;
    }
    std::cout << "created " << step.id << " at revision " << receipt.value().revision.value()
              << " generation " << receipt.value().generation.value() << std::endl;
    ++clock;
  }

  const LocationId row = LocationId::parse("loc-row").value();
  const auto row_path = registry->path_of(row);
  std::cout << "row address: " << row_path.value().to_string() << std::endl;

  const auto room = LocationId::parse("loc-room").value();
  const auto children = registry->children(room);
  std::cout << "children of " << room.str() << " in canonical order:" << std::endl;
  for (const ChildEntry& entry : children.value()) {
    std::cout << "  " << entry.path << " kind=" << location_kind_name(entry.kind)
              << " label=\"" << entry.label << "\"" << std::endl;
  }

  const Limits limits = registry->limits();
  const auto deepest = LocationPath::parse("/FAC1/BLDG-A/ROOM-101/ROW-03/RACK-07/U12", limits);
  const auto resolved = registry->resolve(deepest.value());
  std::cout << "resolved " << deepest.value().to_string() << " to " << resolved.value().id.str()
            << " at generation " << resolved.value().generation.value() << std::endl;

  const LocationStatistics statistics = registry->statistics();
  std::cout << "locations=" << statistics.locations << " roots=" << statistics.roots
            << " max-depth=" << statistics.max_depth_observed
            << " revision=" << registry->revision().value() << std::endl;

  if (!registry->close().has_value()) {
    std::cerr << "close failed" << std::endl;
    return 1;
  }
  return 0;
}
