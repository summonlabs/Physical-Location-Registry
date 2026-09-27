// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Example 03: lifecycle. Retire a subtree, reactivate it, and replace a rack
// unit so that identity is age-tracked while the address stays current.

#include <filesystem>
#include <iostream>
#include <memory>
#include <string>

#include "dccp/physical_location_registry/physical_location_registry.hpp"

namespace {

using namespace dccp::physical_location_registry;

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
  ScratchStore scratch("03");
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
      {"loc-rack", LocationKind::Rack, "loc-row", "RACK-07"},
      {"loc-unit", LocationKind::RackUnit, "loc-rack", "U12"},
  };
  std::int64_t clock = 1767225600;
  for (const auto& step : plan) {
    auto request = CreateLocationRequest::make(step.id, step.kind, step.parent, step.component, "",
                                               context_for("installer", clock++));
    if (!request.has_value()) {
      std::cerr << request.error().to_string() << std::endl;
      return 1;
    }
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
  }

  const LocationId row = LocationId::parse("loc-row").value();
  const Limits limits = registry->limits();
  const auto row_path = LocationPath::parse("/FAC1/ROOM-101/ROW-03", limits);

  // Retiring only the row is refused while its rack is still active: the
  // hierarchy may never contain an active location under a retired one.
  auto partial = RetireLocationRequest::make(row.str(), SubtreeMode::LocationOnly,
                                             context_for("operator", clock++));
  const auto partial_receipt = registry->retire_location(partial.value());
  std::cout << "retire row only: " << error_code_name(partial_receipt.error().code()) << " ("
            << partial_receipt.error().message() << ")" << std::endl;

  // Retiring the subtree is atomic and reports how much it covered.
  auto whole = RetireLocationRequest::make(row.str(), SubtreeMode::Subtree,
                                           context_for("operator", clock++));
  const auto whole_receipt = registry->retire_location(whole.value());
  if (!whole_receipt.has_value()) {
    std::cerr << whole_receipt.error().to_string() << std::endl;
    return 1;
  }
  std::cout << "retired subtree: locations=" << whole_receipt.value().affected_locations
            << " revision=" << whole_receipt.value().revision.value() << std::endl;

  // A retired address is not the current answer...
  const auto hidden = registry->resolve(row_path.value());
  std::cout << "resolution while retired: " << error_code_name(hidden.error().code()) << " ("
            << hidden.error().message() << ")" << std::endl;
  // ...unless the caller asks for retired locations on purpose.
  const auto shown = registry->resolve(row_path.value(), ResolutionMode::IncludeRetired);
  std::cout << "with retired included: " << shown.value().id.str()
            << " lifecycle=" << lifecycle_state_name(shown.value().lifecycle) << std::endl;

  // Reactivation restores the whole subtree.
  auto reactivate = ReactivateLocationRequest::make(row.str(), SubtreeMode::Subtree,
                                                    context_for("operator", clock++));
  const auto reactivated = registry->reactivate_location(reactivate.value());
  if (!reactivated.has_value()) {
    std::cerr << reactivated.error().to_string() << std::endl;
    return 1;
  }
  std::cout << "reactivated subtree: locations=" << reactivated.value().affected_locations
            << std::endl;

  // Replace the rack unit: a new identity at the same coordinate, with lineage.
  auto replace = ReplaceLocationRequest::make("loc-unit", "loc-unit-replacement", "Unit 12 (new)",
                                              context_for("installer", clock++));
  if (!replace.has_value()) {
    std::cerr << replace.error().to_string() << std::endl;
    return 1;
  }
  replace.value().source = "field-replacement";
  const auto replacement = registry->replace_location(replace.value());
  if (!replacement.has_value()) {
    std::cerr << replacement.error().to_string() << std::endl;
    return 1;
  }
  std::cout << "replaced unit: predecessor-generation=" << replacement.value().generation.value()
            << " successor-generation=" << replacement.value().successor_generation.value().value()
            << std::endl;

  const auto predecessor = registry->find(LocationId::parse("loc-unit").value());
  const auto successor = registry->find(LocationId::parse("loc-unit-replacement").value());
  std::cout << "predecessor lifecycle=" << lifecycle_state_name(predecessor.value().lifecycle())
            << " replaced-by=" << predecessor.value().replaced_by().value().str() << std::endl;
  std::cout << "successor lifecycle=" << lifecycle_state_name(successor.value().lifecycle())
            << " replaces=" << successor.value().replaces().value().str()
            << " address=" << successor.value().path().to_string() << std::endl;

  const auto address = LocationPath::parse("/FAC1/ROOM-101/ROW-03/RACK-07/U12", limits);
  const auto current = registry->resolve(address.value());
  std::cout << "the address now belongs to " << current.value().id.str() << std::endl;

  const std::shared_ptr<const Snapshot> snapshot = registry->copy_snapshot();
  std::cout << "replacement records=" << snapshot->replacements().size()
            << " active=" << snapshot->statistics().active
            << " replaced=" << snapshot->statistics().replaced << std::endl;

  if (!registry->close().has_value()) {
    std::cerr << "close failed" << std::endl;
    return 1;
  }
  return 0;
}
