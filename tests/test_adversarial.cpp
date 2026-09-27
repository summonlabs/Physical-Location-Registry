// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Adversarial hardening. These cases try to make the runtime accept something it
// should not: store directories that are not stores, publications that are
// internally contradictory yet carry a valid digest, a lock file that is
// replaced under a writer, and command-line typos that would otherwise be
// ignored.

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "dccp/physical_location_registry/physical_location_registry.hpp"
#include "support/synthetic.hpp"
#include "support/test_harness.hpp"
#include "support/test_process.hpp"

using namespace dccp::physical_location_registry;
using plr_test::Fixture;
using plr_test::TempDir;

namespace {

RegistryOpenOptions writer_options() {
  RegistryOpenOptions options;
  options.mode = OpenMode::ReadWrite;
  options.create_if_missing = true;
  return options;
}

RegistryOpenOptions reader_options() {
  RegistryOpenOptions options;
  options.mode = OpenMode::ReadOnly;
  return options;
}

std::string read_bytes(const std::filesystem::path& path) {
  std::ifstream stream(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
}

void write_bytes(const std::filesystem::path& path, const std::string& bytes) {
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

/// Recomputes the integrity trailer so that a byte-surgery payload is internally
/// consistent: the digest can no longer be the thing that rejects it, which is
/// exactly what these tests want to exercise.
void reframe(std::string& bytes) {
  if (bytes.size() < kSha256Bytes) {
    return;
  }
  Sha256 hasher;
  hasher.update(std::string_view(bytes).substr(0, bytes.size() - kSha256Bytes));
  std::uint8_t digest[kSha256Bytes];
  hasher.finish(digest);
  for (std::size_t index = 0; index < kSha256Bytes; ++index) {
    bytes[bytes.size() - kSha256Bytes + index] = static_cast<char>(digest[index]);
  }
}

/// Replaces every occurrence of one equal-length identity text with another.
bool replace_all(std::string& bytes, std::string_view from, std::string_view to) {
  if (from.size() != to.size()) {
    return false;
  }
  bool replaced = false;
  std::size_t position = bytes.find(from);
  while (position != std::string::npos) {
    bytes.replace(position, from.size(), to);
    replaced = true;
    position = bytes.find(from, position + to.size());
  }
  return replaced;
}

struct SeededStore {
  std::filesystem::path path;
  std::string bytes;
};

/// A store whose state contains ids of equal length so that byte surgery can
/// rewrite one into another without disturbing the framing.
Result<SeededStore> seed_surgical_store(TempDir& dir) {
  RegistryOpenOptions options = writer_options();
  PLR_TRY(registry, Registry::create(dir.path(), options));
  const auto actor = ActorId::parse("hardening");
  if (!actor.has_value()) {
    return actor.error();
  }
  struct Step {
    const char* id;
    LocationKind kind;
    const char* parent;
    const char* component;
  };
  const Step plan[] = {
      {"loc-aaa", LocationKind::Facility, "", "FAC1"},
      {"loc-bbb", LocationKind::Room, "loc-aaa", "ROOM-1"},
      {"loc-ccc", LocationKind::Row, "loc-bbb", "ROW-1"},
  };
  std::int64_t clock = 1767225600;
  for (const auto& step : plan) {
    MutationContext context;
    context.actor = actor.value();
    const auto at = Timestamp::from_unix_seconds(clock++);
    if (!at.has_value()) {
      return at.error();
    }
    context.at = at.value();
    auto request = CreateLocationRequest::make(step.id, step.kind, step.parent, step.component, "",
                                               context);
    if (!request.has_value()) {
      return request.error();
    }
    const auto receipt = registry->create_location(request.value());
    if (!receipt.has_value()) {
      return receipt.error();
    }
  }
  const auto bytes = registry->encode_current_state();
  if (!bytes.has_value()) {
    return bytes.error();
  }
  SeededStore seeded;
  seeded.path = dir.path();
  seeded.bytes = bytes.value();
  PLR_CHECK(registry->close());
  return seeded;
}

}  // namespace

PLR_TEST(adversarial, a_path_that_is_not_a_store_is_never_treated_as_one) {
  TempDir dir;

  // A regular file where a directory is expected.
  const std::filesystem::path file_path = dir.file("not-a-store");
  write_bytes(file_path, "PLRSTAT\0 not really");
  PLR_EXPECT_ERR(Registry::open(file_path, reader_options()), ErrorCode::StoreNotFound);
  // With creation requested the refusal is an I/O refusal: the path is occupied
  // by something that is not a directory.
  PLR_EXPECT_ERR(Registry::open(file_path, writer_options()), ErrorCode::IoError);

  // A directory that is empty is not a store either, unless creation is asked
  // for explicitly.
  TempDir empty;
  PLR_EXPECT_ERR(Registry::open(empty.path(), reader_options()), ErrorCode::StoreNotFound);
  PLR_EXPECT_OK(created, Registry::open(empty.path(), writer_options()));
  PLR_EXPECT(created->close().has_value());

  // Unrelated files in a real store directory are ignored, not interpreted.
  TempDir populated;
  {
    PLR_EXPECT_OK(registry, Registry::create(populated.path(), writer_options()));
    Fixture fixture(registry, 900);
    PLR_EXPECT_OK(all, fixture.standard_facility("FAC1", 1, 1, 1, 1));
    (void)all;
    PLR_EXPECT(registry->close().has_value());
  }
  write_bytes(populated.file("notes.txt"), "operator notes\n");
  write_bytes(populated.file("head.backup"), "PLRHEAD1 seq=99 store=store-x rev=9 epoch=9 "
                                             "file=state.99.plr\n");
  write_bytes(populated.file("state.notanumber.plr"), "junk");
  PLR_EXPECT_OK(reader, Registry::open(populated.path(), reader_options()));
  PLR_EXPECT_EQ(reader->statistics().locations, 6U);
  PLR_EXPECT(reader->recovery().clean());
  PLR_EXPECT(reader->verify_storage().has_value());
}

PLR_TEST(adversarial, a_publication_whose_name_disagrees_with_its_content_is_not_adopted) {
  TempDir dir;
  std::string digest;
  {
    PLR_EXPECT_OK(registry, Registry::create(dir.path(), writer_options()));
    Fixture fixture(registry, 903);
    PLR_EXPECT_OK(all, fixture.standard_facility("FAC1", 1, 1, 1, 1));
    (void)all;
    digest = registry->state_digest();
    PLR_EXPECT(registry->close().has_value());
  }

  // Copy the valid newest publication to a much higher sequence number and
  // remove the head: recovery must not adopt the renamed copy, because its own
  // sequence disagrees with its name.
  const std::string newest = read_bytes(dir.path() / "state.7.plr");
  write_bytes(dir.path() / "state.99.plr", newest);
  std::filesystem::remove(dir.path() / "head");

  PLR_EXPECT_OK(reader, Registry::open(dir.path(), reader_options()));
  PLR_EXPECT(reader->recovery().recovered_older_publication);
  PLR_EXPECT_EQ(reader->recovery().recovered_sequence.value(), 7U);
  PLR_EXPECT_EQ(reader->state_digest(), digest);
  PLR_EXPECT(reader->recovery().invalid_publications_skipped >= 1U);
}

PLR_TEST(adversarial, a_directory_swapped_with_another_store_is_refused) {
  TempDir first;
  TempDir second;
  {
    PLR_EXPECT_OK(registry, Registry::create(first.path(), writer_options()));
    Fixture fixture(registry, 907);
    PLR_EXPECT_OK(all, fixture.standard_facility("FAC1", 1, 1, 1, 1));
    (void)all;
    PLR_EXPECT(registry->close().has_value());
  }
  {
    PLR_EXPECT_OK(registry, Registry::create(second.path(), writer_options()));
    Fixture fixture(registry, 907);
    PLR_EXPECT_OK(all, fixture.standard_facility("FAC2", 1, 1, 1, 1));
    (void)all;
    PLR_EXPECT(registry->close().has_value());
  }

  // Keep the first store's identity anchor but take the second store's
  // publications: the two disagree, so nothing is accepted.
  write_bytes(first.file("store.id"), read_bytes(second.file("store.id")));
  PLR_EXPECT_ERR(Registry::open(first.path(), reader_options()), ErrorCode::StoreMismatch);
  PLR_EXPECT_ERR(Registry::open(first.path(), writer_options()), ErrorCode::StoreMismatch);

  // A malformed anchor is refused rather than ignored.
  write_bytes(first.file("store.id"), "PLRID1 not a canonical identity!\n");
  PLR_EXPECT_ERR(Registry::open(first.path(), reader_options()), ErrorCode::MalformedIdentifier);
  write_bytes(first.file("store.id"), std::string(600, (char)0x78));
  PLR_EXPECT_ERR(Registry::open(first.path(), reader_options()), ErrorCode::StoreCorrupt);
}

PLR_TEST(adversarial, digest_valid_but_contradictory_publications_are_rejected) {
  TempDir dir;
  PLR_EXPECT_OK(seeded, seed_surgical_store(dir));
  const std::string canonical = seeded.bytes;
  PLR_EXPECT_OK(decoded, decode_snapshot(canonical));
  PLR_EXPECT_EQ(decoded.location_count(), 3U);


  // 1. A location that is its own parent: a cycle that the structural
  //    validation must reject even though the digest verifies.
  std::string cycle = canonical;
  PLR_REQUIRE(replace_all(cycle, "loc-aaa", "loc-bbb"));
  reframe(cycle);
  auto cycled = decode_snapshot(cycle);
  PLR_EXPECT(!cycled.has_value());

  // 2. Two records claiming the same identity.
  std::string duplicate = canonical;
  PLR_REQUIRE(replace_all(duplicate, "loc-ccc", "loc-bbb"));
  reframe(duplicate);
  auto duplicated = decode_snapshot(duplicate);
  PLR_EXPECT(!duplicated.has_value());
  PLR_EXPECT_EQ(duplicated.error().code(), ErrorCode::IdentityConflict);

  // 3. A tampered payload without a refreshed digest is caught by the digest.
  std::string tampered = canonical;
  PLR_REQUIRE(replace_all(tampered, "loc-ccc", "loc-bbb"));
  auto undigested = decode_snapshot(tampered);
  PLR_EXPECT(!undigested.has_value());
  PLR_EXPECT_EQ(undigested.error().code(), ErrorCode::DigestMismatch);

  // 4. A payload whose declared location count is inflated.
  std::string inflated = canonical;
  const std::size_t count_offset = 20U + 8U + 8U + 8U + 8U + 8U + 18U * 4U + 8U;
  PLR_REQUIRE(inflated.size() > count_offset + 4U);
  inflated[count_offset] = static_cast<char>(0xFF);
  inflated[count_offset + 1] = static_cast<char>(0xFF);
  inflated[count_offset + 2] = static_cast<char>(0xFF);
  inflated[count_offset + 3] = static_cast<char>(0x7F);
  reframe(inflated);
  auto inflated_result = decode_snapshot(inflated);
  PLR_EXPECT(!inflated_result.has_value());
  PLR_EXPECT(inflated_result.error().code() == ErrorCode::CountMismatch ||
             inflated_result.error().code() == ErrorCode::LimitExceeded ||
             inflated_result.error().code() == ErrorCode::TruncatedInput);

  // The canonical bytes still decode: the surgery above did not touch them.
  PLR_EXPECT_OK(again, decode_snapshot(canonical));
  PLR_EXPECT_EQ(again.location_count(), 3U);
}

PLR_TEST(adversarial, a_replaced_lock_file_cannot_produce_two_writers) {
  TempDir dir;
  PLR_EXPECT_OK(registry, Registry::create(dir.path(), writer_options()));
  Fixture fixture(registry, 911);
  PLR_EXPECT_OK(created, fixture.facility("FAC1"));
  (void)created;

  // Removing or replacing the lock file while a writer holds it must not let a
  // second writer in. On this platform the file cannot be deleted while it is
  // open; the check is that the attempt fails or leaves exclusivity intact.
  std::error_code error;
  std::filesystem::remove(dir.file("store.lock"), error);
  const std::filesystem::path lock_path = dir.file("store.lock");
  if (!std::filesystem::exists(lock_path)) {
    write_bytes(lock_path, "");
  }
  PLR_EXPECT_ERR(Registry::open(dir.path(), writer_options()), ErrorCode::StoreLocked);

  // A read-only session is unaffected and still sees the committed state.
  PLR_EXPECT_OK(reader, Registry::open(dir.path(), reader_options()));
  PLR_EXPECT_EQ(reader->statistics().locations, 1U);
  PLR_EXPECT(reader->close().has_value());

  // Replacing the lock file with junk does not change who owns the store.
  write_bytes(lock_path, "not a lock\n");
  PLR_EXPECT_ERR(Registry::open(dir.path(), writer_options()), ErrorCode::StoreLocked);

  // Once the real writer closes, the next writer can take over.
  PLR_EXPECT(registry->close().has_value());
  PLR_EXPECT_OK(next, Registry::open(dir.path(), writer_options()));
  PLR_EXPECT_EQ(next->statistics().locations, 1U);
  PLR_EXPECT(next->close().has_value());
}

PLR_TEST(adversarial, publications_of_the_wrong_shape_are_skipped_in_recovery) {
  TempDir dir;
  std::string digest;
  {
    PLR_EXPECT_OK(registry, Registry::create(dir.path(), writer_options()));
    Fixture fixture(registry, 919);
    PLR_EXPECT_OK(all, fixture.standard_facility("FAC1", 1, 1, 1, 1));
    (void)all;
    digest = registry->state_digest();
    PLR_EXPECT(registry->close().has_value());
  }

  // A publication name that is a directory, a file that is empty, a file that is
  // huge, and a file with a hostile name: none of them may be adopted.
  std::error_code error;
  std::filesystem::create_directories(dir.file("state.50.plr"), error);
  write_bytes(dir.file("state.51.plr"), "");
  write_bytes(dir.file("state.52.plr"), std::string(4096, 'Z'));
  write_bytes(dir.file("state.53.plr"), "PLRSTAT\0" + std::string(64, '\x01'));
  write_bytes(dir.file("state.0000000000000000000000009.plr"), "junk");
  std::filesystem::remove(dir.path() / "head");

  PLR_EXPECT_OK(reader, Registry::open(dir.path(), reader_options()));
  PLR_EXPECT(reader->recovery().recovered_older_publication);
  PLR_EXPECT_EQ(reader->recovery().recovered_sequence.value(), 7U);
  PLR_EXPECT_EQ(reader->state_digest(), digest);
  // One candidate is a directory, so it is not even a candidate file; the other
  // three are files that fail verification.
  PLR_EXPECT(reader->recovery().invalid_publications_skipped >= 3U);
  PLR_EXPECT(reader->statistics().locations == 6U);
}

PLR_TEST(adversarial, advisory_lock_paths_and_messages_stay_inside_the_store) {
  TempDir dir;
  PLR_EXPECT_OK(registry, Registry::create(dir.path(), writer_options()));

  // Hostile components cannot escape the store directory. Path-like text is
  // rejected outright by the address grammar; reserved device names are ordinary
  // ASCII tokens here, and they are accepted precisely because an address is
  // never used as a file name.
  Fixture fixture(registry, 929);
  PLR_EXPECT_OK(facility, fixture.facility("FAC1"));
  const std::vector<std::string> rejected = {"..", ".", "../../etc", "a/../..", "..\\..",
                                             "a/../../b", std::string("%2e%2e"), "C:"};
  for (const std::string& component : rejected) {
    auto request = CreateLocationRequest::make("loc-" + std::to_string(component.size()),
                                               LocationKind::Room, facility.str(), component, "",
                                               fixture.context());
    if (request.has_value()) {
      PLR_EXPECT(!registry->create_location(request.value()).has_value());
    }
  }
  const std::vector<std::string> accepted = {"CON", "NUL", "LPT1"};
  for (const std::string& component : accepted) {
    PLR_EXPECT_OK(room, fixture.add(facility, LocationKind::Room, component));
    const auto path = registry->path_of(room);
    PLR_REQUIRE(path.has_value());
    PLR_EXPECT_EQ(path.value().to_string(), std::string("/FAC1/") + component);
  }
  PLR_EXPECT_EQ(registry->statistics().locations, 4U);

  // Nothing hostile was written into the store directory: it holds only the
  // files the registry owns, and none of them is named after a component.
  std::error_code error;
  std::vector<std::string> names;
  for (const auto& entry : std::filesystem::directory_iterator(dir.path(), error)) {
    names.push_back(entry.path().filename().string());
  }
  for (const std::string& name : names) {
    PLR_EXPECT(name == "head" || name == "store.lock" || name == "store.id" ||
               name.rfind("state.", 0) == 0);
  }
  PLR_EXPECT(registry->close().has_value());
}

PLR_TEST(adversarial, zero_width_and_bidi_labels_are_stored_exactly_and_flagged) {
  TempDir dir;
  PLR_EXPECT_OK(registry, Registry::create(dir.path(), writer_options()));
  Fixture fixture(registry, 931);
  PLR_EXPECT_OK(facility, fixture.facility("FAC1"));

  // A label containing a right-to-left override and a zero-width joiner is valid
  // UTF-8 without control characters, so it is accepted and stored byte for
  // byte. This repository performs no Unicode normalization, which is why it is
  // recorded here as behaviour rather than silently "cleaned up".
  const std::string tricky = "rack \xE2\x80\xAE" "gnp\xE2\x80\x8D" "01";
  auto request = CreateLocationRequest::make("loc-tricky", LocationKind::Room, facility.str(),
                                             "ROOM-1", tricky, fixture.context());
  PLR_REQUIRE(request.has_value());
  PLR_EXPECT_OK(receipt, registry->create_location(request.value()));
  PLR_EXPECT_EQ(receipt.generation.value(), 1U);

  PLR_EXPECT_OK(view, registry->find(LocationId::parse("loc-tricky").value()));
  PLR_EXPECT_EQ(view.label(), tricky);
  // The address is unaffected: labels are not addresses.
  PLR_EXPECT_EQ(view.path().to_string(), std::string("/FAC1/ROOM-1"));
  const Limits limits = registry->limits();
  PLR_EXPECT_OK(address, LocationPath::parse("/FAC1/ROOM-1", limits));
  PLR_EXPECT_OK(resolved, registry->resolve(address));
  PLR_EXPECT_EQ(resolved.id.str(), std::string("loc-tricky"));

  // The same bytes survive a durable round trip.
  PLR_EXPECT_OK(bytes, registry->encode_current_state());
  PLR_EXPECT_OK(snapshot, decode_snapshot(bytes));
  PLR_EXPECT_OK(reloaded, snapshot.find(LocationId::parse("loc-tricky").value()));
  PLR_EXPECT_EQ(reloaded.label(), tricky);
  PLR_EXPECT(registry->close().has_value());
}

PLR_TEST(adversarial, command_line_typos_and_hostile_arguments_are_rejected) {
  const std::string* cli_option = plr_test::test_option("--cli");
  if (cli_option == nullptr) {
    PLR_FAIL("the inspection tool path was not provided with --cli");
    return;
  }
  const std::filesystem::path cli(*cli_option);

  TempDir dir;
  {
    PLR_EXPECT_OK(registry, Registry::create(dir.path(), writer_options()));
    Fixture fixture(registry, 937);
    PLR_EXPECT_OK(created, fixture.facility("FAC1"));
    (void)created;
    PLR_EXPECT(registry->close().has_value());
  }

  std::string output;
  const auto run = [&](const std::vector<std::string>& args) {
    return plr_test::run_process(cli, args, &output);
  };

  // A misspelled option must not be ignored: it is a usage error.
  const auto typo = run({"--store", dir.path().string(), "create", "--id", "loc-x", "--kind",
                         "room", "--parent", "loc-item-1", "--compnent", "ROOM-9", "--actor",
                         "ops"});
  PLR_REQUIRE(typo.has_value());
  PLR_EXPECT_EQ(typo.value(), 2);
  PLR_EXPECT(output.find("unknown option") != std::string::npos);

  // An option that belongs to a different command is refused too.
  const auto misplaced = run({"--store", dir.path().string(), "list", "--unit", "4"});
  PLR_REQUIRE(misplaced.has_value());
  PLR_EXPECT_EQ(misplaced.value(), 2);

  // An oversized label never reaches the registry.
  const auto oversized = run({"--store", dir.path().string(), "create", "--id", "loc-big",
                              "--kind", "room", "--parent", "loc-item-1", "--component", "ROOM-2",
                              "--label", std::string(4096, (char)0x4C), "--actor", "ops"});
  PLR_REQUIRE(oversized.has_value());
  PLR_EXPECT(oversized.value() == 2 || oversized.value() == 5);

  // A store path that is a file is reported as a missing store.
  const std::filesystem::path file_path = dir.file("plain-file");
  write_bytes(file_path, "not a store");
  const auto wrong_store = run({"--store", file_path.string(), "status"});
  PLR_REQUIRE(wrong_store.has_value());
  PLR_EXPECT_EQ(wrong_store.value(), 3);

  // The store is untouched by all of that.
  PLR_EXPECT_OK(reader, Registry::open(dir.path(), reader_options()));
  PLR_EXPECT_EQ(reader->statistics().locations, 1U);
  PLR_EXPECT_EQ(reader->revision().value(), 1U);
  PLR_EXPECT(reader->verify_storage().has_value());
}
