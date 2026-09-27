// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Example 05: ask before you act. explain_* runs the same structural rules a
// mutation would, without changing anything, and every rejection carries a
// stable machine-readable code.

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

void show(const Explanation& explanation) {
  std::cout << "  code=" << error_code_name(explanation.code)
            << " category=" << error_category_name(explanation.category)
            << " ok=" << (explanation.ok() ? "true" : "false") << std::endl;
  std::cout << "  summary: " << explanation.summary << std::endl;
  for (const std::string& hint : explanation.hints) {
    std::cout << "  hint: " << hint << std::endl;
  }
}

}  // namespace

int main() {
  ScratchStore scratch("05");
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
  };
  std::int64_t clock = 1767225600;
  for (const auto& step : plan) {
    auto request = CreateLocationRequest::make(step.id, step.kind, step.parent, step.component, "",
                                               context_for("installer", clock++));
    const auto receipt = registry->create_location(request.value());
    if (!receipt.has_value()) {
      std::cerr << receipt.error().to_string() << std::endl;
      return 1;
    }
  }

  std::cout << "would this create be accepted?" << std::endl;
  auto good = CreateLocationRequest::make("loc-row-2", LocationKind::Row, "loc-room", "ROW-04", "",
                                          context_for("planner", clock++));
  show(registry->explain_create(good.value()));

  std::cout << "what about a component that is already used?" << std::endl;
  auto clash = CreateLocationRequest::make("loc-row-3", LocationKind::Row, "loc-room", "ROW-03", "",
                                           context_for("planner", clock++));
  show(registry->explain_create(clash.value()));

  std::cout << "what about a kind the parent cannot contain?" << std::endl;
  auto wrong_kind = CreateLocationRequest::make("loc-unit", LocationKind::RackUnit, "loc-facility",
                                                "U1", "", context_for("planner", clock++));
  wrong_kind.value().unit = RackUnitCoordinate::parse(1).value();
  show(registry->explain_create(wrong_kind.value()));

  std::cout << "would moving the row under the facility be accepted?" << std::endl;
  auto bad_move = MoveLocationRequest::make("loc-row", "loc-facility",
                                            context_for("planner", clock++));
  show(registry->explain_move(bad_move.value()));

  std::cout << "which is legal: the row under the room, or the room under the row?" << std::endl;
  auto cyc = MoveLocationRequest::make("loc-room", "loc-row", context_for("planner", clock++));
  show(registry->explain_move(cyc.value()));

  const Limits limits = registry->limits();
  std::cout << "why does this address not resolve?" << std::endl;
  const auto missing = LocationPath::parse("/FAC1/ROOM-101/ROW-99", limits);
  show(registry->explain_resolve(missing.value()));

  std::cout << "and this one?" << std::endl;
  const auto wrong_root = LocationPath::parse("/FAC9/ROOM-101", limits);
  show(registry->explain_resolve(wrong_root.value()));

  // Explanations never mutate: the registry is exactly where it was.
  std::cout << "state after all of that: revision=" << registry->revision().value()
            << " locations=" << registry->statistics().locations << std::endl;

  if (!registry->close().has_value()) {
    std::cerr << "close failed" << std::endl;
    return 1;
  }
  return 0;
}
