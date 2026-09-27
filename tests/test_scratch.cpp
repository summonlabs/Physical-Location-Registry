// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Scratch diagnostic (removed before release): dumps a store directory after a
// couple of committed mutations.

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include "dccp/physical_location_registry/physical_location_registry.hpp"
#include "support/test_harness.hpp"

using namespace dccp::physical_location_registry;
using plr_test::TempDir;

namespace {

void dump(const std::filesystem::path& dir) {
  std::cout << "--- directory " << dir.string() << std::endl;
  for (const auto& entry : std::filesystem::directory_iterator(dir)) {
    std::cout << "    " << entry.path().filename().string() << " size="
              << std::filesystem::file_size(entry.path()) << std::endl;
  }
  const auto head_path = dir / "head";
  if (std::filesystem::exists(head_path)) {
    std::ifstream stream(head_path, std::ios::binary);
    std::string line;
    std::getline(stream, line);
    std::cout << "    head: " << line << std::endl;
  }
}

}  // namespace

PLR_TEST(scratch, dump_store_after_mutations) {
  TempDir dir;
  auto options = RegistryOpenOptions{};
  options.mode = OpenMode::ReadWrite;
  options.create_if_missing = true;
  auto created = Registry::create(dir.path(), options);
  PLR_REQUIRE(created.has_value());
  std::shared_ptr<Registry> registry = created.value();
  std::cout << "after create: revision=" << registry->revision().value()
            << " sequence=" << registry->sequence().value() << " digest=" << registry->state_digest()
            << std::endl;
  dump(dir.path());

  MutationContext context;
  const auto actor = ActorId::parse("actor-1");
  PLR_REQUIRE(actor.has_value());
  context.actor = actor.value();
  const auto at = Timestamp::from_unix_seconds(1700000000);
  PLR_REQUIRE(at.has_value());
  context.at = at.value();

  auto facility = CreateLocationRequest::make("loc-fac", LocationKind::Facility, "", "FAC1", "",
                                              context);
  PLR_REQUIRE(facility.has_value());
  auto receipt = registry->create_location(facility.value());
  PLR_REQUIRE(receipt.has_value());
  std::cout << "after mutation 1: revision=" << registry->revision().value()
            << " sequence=" << registry->sequence().value() << " digest=" << registry->state_digest()
            << std::endl;

  auto room = CreateLocationRequest::make("loc-room", LocationKind::Room, "loc-fac", "ROOM-1", "",
                                          context);
  PLR_REQUIRE(room.has_value());
  auto receipt2 = registry->create_location(room.value());
  PLR_REQUIRE(receipt2.has_value());
  std::cout << "after mutation 2: revision=" << registry->revision().value()
            << " sequence=" << registry->sequence().value() << " digest=" << registry->state_digest()
            << std::endl;
  dump(dir.path());

  PLR_REQUIRE(registry->close().has_value());
  registry.reset();

  auto reader_options = RegistryOpenOptions{};
  reader_options.mode = OpenMode::ReadOnly;
  auto reopened = Registry::open(dir.path(), reader_options);
  if (!reopened.has_value()) {
    std::cout << "reopen failed: " << reopened.error().to_string() << std::endl;
    PLR_FAIL("reopen failed");
    return;
  }
  std::cout << "reopened: revision=" << reopened.value()->revision().value()
            << " sequence=" << reopened.value()->sequence().value()
            << " locations=" << reopened.value()->statistics().locations
            << " digest=" << reopened.value()->state_digest() << std::endl;
  std::cout << "recovery: " << reopened.value()->recovery().to_string() << std::endl;
  const auto verify = reopened.value()->verify_storage();
  if (!verify.has_value()) {
    std::cout << "verify failed: " << verify.error().to_string() << std::endl;
  }
}
