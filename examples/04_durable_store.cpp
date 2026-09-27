// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Example 04: durable state. Commit, close, reopen read-only, verify the
// published generation against its integrity digest, diff two revisions and
// compare exported snapshot files.

#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

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

bool write_file(const std::filesystem::path& path, const std::string& bytes) {
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  return static_cast<bool>(stream);
}

std::string read_file(const std::filesystem::path& path) {
  std::ifstream stream(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
}

}  // namespace

int main() {
  ScratchStore scratch("04");
  std::string first_snapshot;
  std::string second_snapshot;
  LocationRevision first_revision;
  LocationRevision second_revision;
  std::string published_digest;

  // ---- writer session: two committed revisions ------------------------------
  {
    RegistryOpenOptions options;
    options.mode = OpenMode::ReadWrite;
    options.create_if_missing = true;
    Limits limits;
    limits.max_publications_retained = 4;  // keep enough generations to diff them
    options.limits = limits;

    auto created = Registry::create(scratch.path(), options);
    if (!created.has_value()) {
      std::cerr << created.error().to_string() << std::endl;
      return 1;
    }
    std::shared_ptr<Registry> registry = created.value();
    std::cout << "store=" << registry->store_id().str()
              << " state-file=" << registry->state_file_name() << std::endl;

    struct Step {
      const char* id;
      LocationKind kind;
      const char* parent;
      const char* component;
    };
    const Step plan[] = {
        {"loc-facility", LocationKind::Facility, "", "FAC1"},
        {"loc-building", LocationKind::Building, "loc-facility", "BLDG-A"},
        {"loc-room", LocationKind::Room, "loc-building", "ROOM-101"},
        {"loc-row", LocationKind::Row, "loc-room", "ROW-03"},
    };
    std::int64_t clock = 1767225600;
    for (const auto& step : plan) {
      auto request = CreateLocationRequest::make(step.id, step.kind, step.parent, step.component, "",
                                                 context_for("installer", clock++));
      if (!request.has_value()) {
        std::cerr << request.error().to_string() << std::endl;
        return 1;
      }
      const auto receipt = registry->create_location(request.value());
      if (!receipt.has_value()) {
        std::cerr << receipt.error().to_string() << std::endl;
        return 1;
      }
    }
    first_revision = registry->revision();
    const auto first_bytes = registry->encode_current_state();
    if (!first_bytes.has_value()) {
      std::cerr << first_bytes.error().to_string() << std::endl;
      return 1;
    }
    first_snapshot = first_bytes.value();

    // A rejected operation leaves the committed revision exactly as it was.
    auto duplicate = CreateLocationRequest::make("loc-row2", LocationKind::Row, "loc-room", "ROW-03",
                                                 "", context_for("installer", clock++));
    const auto rejected = registry->create_location(duplicate.value());
    std::cout << "rejected duplicate: " << error_code_name(rejected.error().code())
              << " revision-still=" << registry->revision().value() << std::endl;

    auto relabel = RelabelRequest::make("loc-room", "Room 101 (renamed)",
                                        context_for("operator", clock++));
    const auto relabel_receipt = registry->relabel(relabel.value());
    if (!relabel_receipt.has_value()) {
      std::cerr << relabel_receipt.error().to_string() << std::endl;
      return 1;
    }
    second_revision = registry->revision();
    const auto second_bytes = registry->encode_current_state();
    if (!second_bytes.has_value()) {
      std::cerr << second_bytes.error().to_string() << std::endl;
      return 1;
    }
    second_snapshot = second_bytes.value();
    published_digest = registry->state_digest();
    std::cout << "committed revisions " << first_revision.value() << " and "
              << second_revision.value() << " digest=" << published_digest << std::endl;

    if (!write_file(scratch.path() / "export-a.plrsnap", first_snapshot) ||
        !write_file(scratch.path() / "export-b.plrsnap", second_snapshot)) {
      std::cerr << "could not write snapshot files" << std::endl;
      return 1;
    }
    std::cout << "session revision history (in memory): ";
    for (const LocationRevision revision : registry->retained_revisions()) {
      std::cout << revision.value() << " ";
    }
    std::cout << std::endl;

    if (!registry->close().has_value()) {
      std::cerr << "close failed" << std::endl;
      return 1;
    }
  }

  // ---- reader session: verify what is on disk ------------------------------
  {
    RegistryOpenOptions options;
    options.mode = OpenMode::ReadOnly;
    auto reopened = Registry::open(scratch.path(), options);
    if (!reopened.has_value()) {
      std::cerr << reopened.error().to_string() << std::endl;
      return 1;
    }
    std::shared_ptr<Registry> registry = reopened.value();
    const auto verified = registry->verify_storage();
    std::cout << "reopened revision=" << registry->revision().value()
              << " digest=" << registry->state_digest()
              << " verified=" << (verified.has_value() ? "true" : "false")
              << " recovery-clean=" << (registry->recovery().clean() ? "true" : "false")
              << std::endl;
    const auto room = registry->find(LocationId::parse("loc-room").value());
    std::cout << "room label after reopen: \"" << room.value().label() << "\"" << std::endl;
    if (registry->state_digest() != published_digest) {
      std::cerr << "digest changed across reopen" << std::endl;
      return 1;
    }
    if (!registry->close().has_value()) {
      std::cerr << "close failed" << std::endl;
      return 1;
    }
  }

  // ---- file-to-file comparison: arbitrary revisions, any time --------------
  {
    const std::string bytes_a = read_file(scratch.path() / "export-a.plrsnap");
    const std::string bytes_b = read_file(scratch.path() / "export-b.plrsnap");
    auto snapshot_a = decode_snapshot(bytes_a);
    auto snapshot_b = decode_snapshot(bytes_b);
    if (!snapshot_a.has_value() || !snapshot_b.has_value()) {
      std::cerr << "snapshot files did not decode" << std::endl;
      return 1;
    }
    std::cout << "snapshot A: revision=" << snapshot_a.value().revision().value()
              << " locations=" << snapshot_a.value().location_count()
              << " digest=" << snapshot_a.value().canonical_digest().value() << std::endl;
    std::cout << "snapshot B: revision=" << snapshot_b.value().revision().value()
              << " locations=" << snapshot_b.value().location_count() << std::endl;
    const RevisionDiff diff = diff_snapshots(snapshot_a.value(), snapshot_b.value());
    std::cout << "diff changed=" << diff.changed << " created=" << diff.created << std::endl;
    for (const LocationChange& change : diff.changes) {
      std::cout << "  " << change.to_string() << std::endl;
    }
  }

  // ---- a damaged publication is refused, never trusted ---------------------
  {
    std::string bytes = read_file(scratch.path() / "export-b.plrsnap");
    if (bytes.size() > 40) {
      bytes[40] = static_cast<char>(bytes[40] ^ 0x01);
      const auto damaged = decode_snapshot(bytes);
      std::cout << "damaged snapshot: " << error_code_name(damaged.error().code()) << " ("
                << damaged.error().message() << ")" << std::endl;
    }
  }

  return 0;
}
