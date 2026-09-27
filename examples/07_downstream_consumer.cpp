// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Example 07: consuming the registry from another DCCP layer.
//
// A placement or inventory system does not own physical addresses; it consumes
// them. This shows the contract such a consumer uses: hold a stable LocationId
// and the generation you observed, ask for the current address when you need it,
// and detect staleness rather than trusting a cached string.

#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "dccp/physical_location_registry/physical_location_registry.hpp"

namespace {

using namespace dccp::physical_location_registry;

/// A cached reference held by a downstream system, exactly as it would be
/// persisted in that system's own state.
struct CachedPlacement {
  std::string asset_reference;  // this repository never stores assets; the consumer does
  LocationId location;
  LocationGeneration observed_generation;
  std::string last_known_address;
};

class ScratchStore {
 public:
  explicit ScratchStore(std::string_view name) {
    std::error_code error;
    const auto base = std::filesystem::temp_directory_path(error);
    path_ = (error ? std::filesystem::path(".") : base) / ("plr-example-" + std::string(name));
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
  return context;
}

}  // namespace

int main() {
  ScratchStore scratch("07");
  RegistryOpenOptions options;
  options.mode = OpenMode::ReadWrite;
  options.create_if_missing = true;

  auto created = Registry::create(scratch.path(), options);
  if (!created.has_value()) {
    std::cerr << created.error().to_string() << std::endl;
    return 1;
  }
  std::shared_ptr<Registry> registry = created.value();

  struct Step {
    const char* id;
    LocationKind kind;
    const char* parent;
    const char* component;
  };
  const Step plan[] = {
      {"loc-facility", LocationKind::Facility, "", "FAC1"},
      {"loc-room", LocationKind::Room, "loc-facility", "ROOM-101"},
      {"loc-row", LocationKind::Row, "loc-room", "ROW-03"},
      {"loc-row-2", LocationKind::Row, "loc-room", "ROW-04"},
      {"loc-rack", LocationKind::Rack, "loc-row", "RACK-07"},
  };
  std::int64_t clock = 1767225600;
  for (const auto& step : plan) {
    auto request = CreateLocationRequest::make(step.id, step.kind, step.parent, step.component, "",
                                               context_for("installer", clock++));
    if (std::string_view(step.component) == "RACK-07") {
      request.value().envelope = RackEnvelope::with_height(48).value();
    }
    const auto receipt = registry->create_location(request.value());
    if (!receipt.has_value()) {
      std::cerr << receipt.error().to_string() << std::endl;
      return 1;
    }
  }

  // The consumer records a placement using the typed identity and the observed
  // generation. It never invents an address of its own.
  const LocationId rack = LocationId::parse("loc-rack").value();
  const auto observed = registry->find(rack);
  CachedPlacement placement;
  placement.asset_reference = "asi://accelerator/serial-8821";
  placement.location = rack;
  placement.observed_generation = observed.value().generation();
  placement.last_known_address = observed.value().path().to_string();
  std::cout << "consumer cached " << placement.asset_reference << " at "
            << placement.last_known_address << " generation "
            << placement.observed_generation.value() << std::endl;

  // The room is rearranged: the rack keeps its identity, its address moves to
  // the other row.
  auto move = MoveLocationRequest::make(rack.str(), "loc-row-2",
                                        context_for("operator", clock++));
  const auto moved = registry->move_location(move.value());
  if (!moved.has_value()) {
    std::cerr << moved.error().to_string() << std::endl;
    return 1;
  }
  std::cout << "rack moved: descendants-that-followed=" << moved.value().affected_descendants
            << std::endl;

  // The consumer detects the change by comparing generations, then re-reads.
  const auto now = registry->find(placement.location);
  const bool stale = now.value().generation() != placement.observed_generation;
  std::cout << "consumer's cache is " << (stale ? "stale" : "current") << std::endl;
  if (stale) {
    std::cout << "  cached address:  " << placement.last_known_address << std::endl;
    std::cout << "  current address: " << now.value().path().to_string() << std::endl;
    placement.last_known_address = now.value().path().to_string();
    placement.observed_generation = now.value().generation();
  }

  // The typed address of a retired or replaced location is never silently
  // current, so a consumer that resolves by address must handle the rejection.
  auto retire = RetireLocationRequest::make(rack.str(), SubtreeMode::Subtree,
                                            context_for("operator", clock++));
  const auto retired = registry->retire_location(retire.value());
  if (!retired.has_value()) {
    std::cerr << retired.error().to_string() << std::endl;
    return 1;
  }
  const auto address = LocationPath::parse(placement.last_known_address, registry->limits());
  const auto resolution = registry->resolve(address.value());
  std::cout << "resolving a retired address: " << error_code_name(resolution.error().code())
            << std::endl;

  // What the consumer may rely on instead: the identity still resolves, with its
  // lifecycle visible, and the generation tells it something changed.
  const auto retired_view = registry->find(placement.location);
  std::cout << "by identity: path=" << retired_view.value().path().to_string()
            << " lifecycle=" << lifecycle_state_name(retired_view.value().lifecycle())
            << " generation=" << retired_view.value().generation().value() << std::endl;

  // The registry never stores the consumer's asset reference: that stays in the
  // consumer's own state.
  const std::shared_ptr<const Snapshot> snapshot = registry->copy_snapshot();
  const LocationStatistics statistics = snapshot->statistics();
  std::cout << "registry holds " << statistics.locations
            << " locations and no asset records: retired=" << statistics.retired
            << " active=" << statistics.active << std::endl;

  if (!registry->close().has_value()) {
    std::cerr << "close failed" << std::endl;
    return 1;
  }
  return 0;
}
