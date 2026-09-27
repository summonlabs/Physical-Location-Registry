// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Example 02: change an address under generation control, and keep old
// references working through an explicit alias.

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

MutationContext context_for(std::string_view actor, std::int64_t seconds,
                            std::optional<LocationGeneration> expected = std::nullopt) {
  MutationContext context;
  context.actor = ActorId::parse(actor).value();
  context.at = Timestamp::from_unix_seconds(seconds).value();
  context.expected_generation = expected;
  return context;
}

}  // namespace

int main() {
  ScratchStore scratch("02");
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
  };
  std::int64_t clock = 1767225600;
  for (const auto& step : plan) {
    auto request = CreateLocationRequest::make(step.id, step.kind, step.parent, step.component, "",
                                               context_for("planner", clock++));
    if (!request.has_value()) {
      std::cerr << request.error().to_string() << std::endl;
      return 1;
    }
    if (std::string_view(step.component) == "RACK-07") {
      request.value().envelope = RackEnvelope::with_height(48).value();
    }
    const auto receipt = registry->create_location(request.value());
    if (!receipt.has_value()) {
      std::cerr << receipt.error().to_string() << std::endl;
      return 1;
    }
  }

  const LocationId row = LocationId::parse("loc-row").value();
  const auto before = registry->find(row);
  std::cout << "before: " << before.value().path().to_string()
            << " generation=" << before.value().generation().value() << std::endl;

  // A legacy address from the previous addressing scheme stays resolvable
  // through an explicit alias. Binding an address that is already the location's
  // own canonical address would be redundant and is refused.
  const Limits limits = registry->limits();
  auto redundant = AddAliasRequest::make(row.str(), "/FAC1/ROOM-101/ROW-03", limits,
                                         context_for("mover", clock++));
  if (redundant.has_value()) {
    const auto refused = registry->add_alias(redundant.value());
    std::cout << "redundant alias: " << error_code_name(refused.error().code()) << " ("
              << refused.error().message() << ")" << std::endl;
  }

  auto alias = AddAliasRequest::make(row.str(), "/SITE-1/HALL-101/ROW-03", limits,
                                     context_for("mover", clock++));
  if (!alias.has_value()) {
    std::cerr << alias.error().to_string() << std::endl;
    return 1;
  }
  const auto alias_receipt = registry->add_alias(alias.value());
  if (!alias_receipt.has_value()) {
    std::cerr << alias_receipt.error().to_string() << std::endl;
    return 1;
  }
  std::cout << "bound legacy alias /SITE-1/HALL-101/ROW-03 at revision "
            << alias_receipt.value().revision.value() << std::endl;

  // A caller that cached generation 1 cannot readdress silently: the stale
  // generation is rejected.
  auto stale = ReaddressRequest::make(row.str(), "ROW-04",
                                      context_for("mover", clock++, LocationGeneration(1)));
  if (!stale.has_value()) {
    std::cerr << stale.error().to_string() << std::endl;
    return 1;
  }
  const auto stale_receipt = registry->readdress(stale.value());
  std::cout << "stale attempt: " << error_code_name(stale_receipt.error().code()) << " ("
            << stale_receipt.error().message() << ")" << std::endl;

  // With the current generation the readdress is accepted; the whole subtree
  // follows the row.
  const auto current = registry->find(row);
  auto fresh = ReaddressRequest::make(row.str(), "ROW-04",
                                      context_for("mover", clock++, current.value().generation()));
  if (!fresh.has_value()) {
    std::cerr << fresh.error().to_string() << std::endl;
    return 1;
  }
  const auto receipt = registry->readdress(fresh.value());
  if (!receipt.has_value()) {
    std::cerr << receipt.error().to_string() << std::endl;
    return 1;
  }
  std::cout << "readdress: revision=" << receipt.value().revision.value()
            << " generation=" << receipt.value().generation.value()
            << " descendants-that-followed=" << receipt.value().affected_descendants << std::endl;

  const auto rack = registry->find(LocationId::parse("loc-rack").value());
  std::cout << "rack now: " << rack.value().path().to_string()
            << " generation=" << rack.value().generation().value() << std::endl;

  const auto old_address = LocationPath::parse("/SITE-1/HALL-101/ROW-03", limits);
  const auto via_alias = registry->resolve(old_address.value());
  std::cout << "legacy address resolves via " << resolution_kind_name(via_alias.value().kind)
            << " to " << via_alias.value().id.str()
            << " whose current address is " << via_alias.value().canonical_path.to_string()
            << std::endl;

  const auto new_address = LocationPath::parse("/FAC1/ROOM-101/ROW-04", limits);
  const auto direct = registry->resolve(new_address.value());
  std::cout << "new address resolves via " << resolution_kind_name(direct.value().kind)
            << " to " << direct.value().id.str() << std::endl;

  if (!registry->close().has_value()) {
    std::cerr << "close failed" << std::endl;
    return 1;
  }
  return 0;
}
