// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Example 06: retries, idempotency, generations and writer authority in one
// short workflow. This is the shape a control-plane consumer should follow:
// stamp authority, retry with an operation id, refresh before retrying a
// generation, and reopen when the session loses its authority.

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
  ScratchStore scratch("06");
  RegistryOpenOptions options;
  options.mode = OpenMode::ReadWrite;
  options.create_if_missing = true;

  auto created = Registry::create(scratch.path(), options);
  if (!created.has_value()) {
    std::cerr << created.error().to_string() << std::endl;
    return 1;
  }
  std::shared_ptr<Registry> registry = created.value();

  auto facility = CreateLocationRequest::make("loc-facility", LocationKind::Facility, "", "FAC1",
                                              "Facility", context_for("agent", 1767225600));
  const auto facility_receipt = registry->create_location(facility.value());
  if (!facility_receipt.has_value()) {
    std::cerr << facility_receipt.error().to_string() << std::endl;
    return 1;
  }

  // ---- idempotent retry ----------------------------------------------------
  MutationContext context = context_for("agent", 1767225601);
  auto parsed_operation = OperationId::parse("install-room-101");
  context.operation_id = parsed_operation.value();
  context.authority = registry->authority();
  context.reason = "rack installation";
  auto install = CreateLocationRequest::make("loc-room", LocationKind::Room, "loc-facility",
                                             "ROOM-101", "Room 101", context);
  const auto first = registry->create_location(install.value());
  const auto retry = registry->create_location(install.value());
  if (!first.has_value() || !retry.has_value()) {
    std::cerr << "idempotent create failed" << std::endl;
    return 1;
  }
  std::cout << "first attempt:   revision=" << first.value().revision.value()
            << " replayed=" << (first.value().replayed ? "true" : "false") << std::endl;
  std::cout << "retried attempt: revision=" << retry.value().revision.value()
            << " replayed=" << (retry.value().replayed ? "true" : "false")
            << " (no second publication)" << std::endl;

  // The same operation id with a different request is a conflict, not an apply.
  auto other = CreateLocationRequest::make("loc-room-102", LocationKind::Room, "loc-facility",
                                           "ROOM-102", "Room 102", context);
  const auto conflict = registry->create_location(other.value());
  std::cout << "same id, different request: " << error_code_name(conflict.error().code())
            << std::endl;

  // ---- generation fencing --------------------------------------------------
  const LocationId room = LocationId::parse("loc-room").value();
  const auto before = registry->find(room);
  MutationContext generation_context = context_for("agent", 1767225602);
  generation_context.authority = registry->authority();
  generation_context.expected_generation = before.value().generation();
  auto rename = RelabelRequest::make(room.str(), "Room 101 (renamed)", generation_context);
  const auto renamed = registry->relabel(rename.value());
  std::cout << "relabel at generation " << before.value().generation().value()
            << ": revision=" << renamed.value().revision.value()
            << " generation=" << renamed.value().generation.value() << std::endl;

  auto stale_rename = RelabelRequest::make(room.str(), "Room 101 (stale)",
                                           rename.value().context);
  const auto stale = registry->relabel(stale_rename.value());
  std::cout << "same generation again: " << error_code_name(stale.error().code()) << " ("
            << stale.error().message() << ")" << std::endl;

  auto refreshed = registry->find(room);
  MutationContext fresh_context = context_for("agent", 1767225603);
  fresh_context.authority = registry->authority();
  fresh_context.expected_generation = refreshed.value().generation();
  fresh_context.expected_revision = registry->revision();
  auto accepted_rename = RelabelRequest::make(room.str(), "Room 101 (final)", fresh_context);
  const auto final_receipt = registry->relabel(accepted_rename.value());
  std::cout << "after refreshing: revision=" << final_receipt.value().revision.value()
            << " generation=" << final_receipt.value().generation.value() << std::endl;

  // ---- authority from a superseded session ---------------------------------
  const MutationAuthority old_authority = registry->authority().value();
  const WriterEpoch old_epoch = registry->epoch();
  if (!registry->close().has_value()) {
    std::cerr << "close failed" << std::endl;
    return 1;
  }

  auto reopened = Registry::open(scratch.path(), options);
  if (!reopened.has_value()) {
    std::cerr << reopened.error().to_string() << std::endl;
    return 1;
  }
  registry = reopened.value();
  std::cout << "reopened: epoch " << old_epoch.value() << " -> " << registry->epoch().value()
            << std::endl;

  MutationContext stale_authority = context_for("agent", 1767225604);
  stale_authority.authority = old_authority;
  auto late = RelabelRequest::make(room.str(), "too late", stale_authority);
  const auto rejected = registry->relabel(late.value());
  std::cout << "mutation under the old epoch: " << error_code_name(rejected.error().code()) << " ("
            << rejected.error().message() << ")" << std::endl;

  MutationContext current_authority = context_for("agent", 1767225605);
  current_authority.authority = registry->authority();
  auto accepted = RelabelRequest::make(room.str(), "accepted after reopen", current_authority);
  const auto accepted_receipt = registry->relabel(accepted.value());
  std::cout << "mutation under the current epoch: revision="
            << accepted_receipt.value().revision.value()
            << " epoch=" << registry->epoch().value() << std::endl;

  if (!registry->close().has_value()) {
    std::cerr << "close failed" << std::endl;
    return 1;
  }
  return 0;
}
