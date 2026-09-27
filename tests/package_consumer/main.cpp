// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Independent downstream consumer. It is configured and built outside the
// registry's own source tree, against the installed package, and exercises the
// public API only: create a store, commit locations, address them, detect a
// stale generation, and reopen the state from a fresh session.

#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include <dccp/physical_location_registry/physical_location_registry.hpp>

namespace {

using namespace dccp::physical_location_registry;

/// The consumer brings its own scratch directory and never reaches into the
/// registry's persistence layout.
class ScratchArea {
 public:
  ScratchArea() {
    std::error_code error;
    const auto base = std::filesystem::temp_directory_path(error);
    path_ = (error ? std::filesystem::path(".") : base) / "plr-package-consumer";
    std::filesystem::remove_all(path_, error);
  }
  ~ScratchArea() {
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

int fail(const std::string& message) {
  std::cerr << "consumer failure: " << message << std::endl;
  return 1;
}

}  // namespace

int main() {
  ScratchArea scratch;

  RegistryOpenOptions options;
  options.mode = OpenMode::ReadWrite;
  options.create_if_missing = true;

  auto created = Registry::create(scratch.path(), options);
  if (!created.has_value()) {
    return fail(created.error().to_string());
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
      {"loc-rack", LocationKind::Rack, "loc-row", "RACK-07"},
      {"loc-unit", LocationKind::RackUnit, "loc-rack", "U12"},
  };
  std::int64_t clock = 1767225600;
  for (const auto& step : plan) {
    auto request = CreateLocationRequest::make(step.id, step.kind, step.parent, step.component, "",
                                               context_for("consumer", clock++));
    if (!request.has_value()) {
      return fail(request.error().to_string());
    }
    if (std::string_view(step.component) == "RACK-07") {
      request.value().envelope = RackEnvelope::with_height(48).value();
    }
    if (std::string_view(step.component) == "U12") {
      request.value().unit = RackUnitCoordinate::parse(12).value();
    }
    const auto receipt = registry->create_location(request.value());
    if (!receipt.has_value()) {
      return fail(receipt.error().to_string());
    }
  }

  const Limits limits = registry->limits();
  const auto path = LocationPath::parse("/FAC1/ROOM-101/ROW-03/RACK-07/U12", limits);
  if (!path.has_value()) {
    return fail(path.error().to_string());
  }
  const auto resolved = registry->resolve(path.value());
  if (!resolved.has_value()) {
    return fail(resolved.error().to_string());
  }
  std::cout << "resolved " << path.value().to_string() << " to " << resolved.value().id.str()
            << " generation " << resolved.value().generation.value() << std::endl;

  // Generation fencing across the package boundary.
  const LocationId unit = resolved.value().id;
  MutationContext stale = context_for("consumer", clock++);
  stale.expected_generation = LocationGeneration(99);
  auto rejected = RelabelRequest::make(unit.str(), "stale label", stale);
  const auto rejected_receipt = registry->relabel(rejected.value());
  if (rejected_receipt.has_value()) {
    return fail("a stale generation was accepted");
  }
  std::cout << "stale generation rejected: " << error_code_name(rejected_receipt.error().code())
            << std::endl;

  MutationContext fresh = context_for("consumer", clock++);
  fresh.expected_generation = resolved.value().generation;
  auto accepted = RelabelRequest::make(unit.str(), "Unit 12 (verified)", fresh);
  const auto accepted_receipt = registry->relabel(accepted.value());
  if (!accepted_receipt.has_value()) {
    return fail(accepted_receipt.error().to_string());
  }
  const std::uint64_t revision = accepted_receipt.value().revision.value();
  const std::string digest = registry->state_digest();
  std::cout << "committed revision " << revision << " digest " << digest << std::endl;

  if (!registry->close().has_value()) {
    return fail("close failed");
  }

  RegistryOpenOptions read_options;
  read_options.mode = OpenMode::ReadOnly;
  auto reopened = Registry::open(scratch.path(), read_options);
  if (!reopened.has_value()) {
    return fail(reopened.error().to_string());
  }
  std::shared_ptr<Registry> reader = reopened.value();
  if (!reader->verify_storage().has_value()) {
    return fail("reopened store failed verification");
  }
  if (reader->revision().value() != revision || reader->state_digest() != digest) {
    return fail("reopened state does not match the committed revision");
  }
  const auto view = reader->find(unit);
  if (!view.has_value() || view.value().label() != "Unit 12 (verified)") {
    return fail("reopened state lost the committed label");
  }
  const LocationStatistics statistics = reader->statistics();
  std::cout << "reopened: revision=" << reader->revision().value()
            << " locations=" << statistics.locations << " verified=true" << std::endl;
  if (!reader->close().has_value()) {
    return fail("close failed");
  }
  std::cout << "consumer ok" << std::endl;
  return 0;
}
