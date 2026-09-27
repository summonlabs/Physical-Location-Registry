// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Durable store behaviour: atomic publication, integrity verification,
// conservative recovery, retention, failure injection at every publication
// stage, and canonical byte-for-byte determinism.

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "dccp/physical_location_registry/physical_location_registry.hpp"
#include "support/synthetic.hpp"
#include "support/test_harness.hpp"

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

std::vector<std::string> file_names(const std::filesystem::path& dir) {
  std::vector<std::string> names;
  std::error_code error;
  for (const auto& entry : std::filesystem::directory_iterator(dir, error)) {
    names.push_back(entry.path().filename().string());
  }
  std::sort(names.begin(), names.end());
  return names;
}

std::string read_text(const std::filesystem::path& path) {
  std::ifstream stream(path, std::ios::binary);
  std::string content;
  std::getline(stream, content);
  return content;
}

void write_text(const std::filesystem::path& path, const std::string& content) {
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  stream << content;
}

bool contains(const std::vector<std::string>& names, std::string_view needle) {
  for (const std::string& name : names) {
    if (name.find(needle) != std::string::npos) {
      return true;
    }
  }
  return false;
}

}  // namespace

PLR_TEST(persistence, create_publishes_an_empty_initial_state) {
  TempDir dir;
  PLR_EXPECT_OK(registry, Registry::create(dir.path(), writer_options()));
  PLR_EXPECT_EQ(registry->revision().value(), 0U);
  PLR_EXPECT_EQ(registry->sequence().value(), StateSequence::kFirstPublication);
  PLR_EXPECT_EQ(registry->epoch().value(), 1U);
  PLR_EXPECT_EQ(registry->state_file_name(), std::string("state.1.plr"));
  PLR_EXPECT(registry->state_bytes() > 0U);
  PLR_EXPECT(is_lower_hex_sha256(registry->state_digest()));
  PLR_EXPECT_EQ(registry->recovery().head_missing, false);
  PLR_EXPECT(registry->recovery().clean());

  const std::vector<std::string> names = file_names(dir.path());
  PLR_EXPECT(contains(names, "head"));
  PLR_EXPECT(contains(names, "state.1.plr"));
  PLR_EXPECT(contains(names, "store.lock"));

  PLR_EXPECT(registry->verify_storage().has_value());
  PLR_EXPECT_OK(state, registry->encode_current_state());
  PLR_EXPECT_EQ(state.size(), static_cast<std::size_t>(registry->state_bytes()));
}

PLR_TEST(persistence, canonical_bytes_are_deterministic) {
  TempDir first_dir;
  TempDir second_dir;
  StoreId fixed_id;
  std::string first_bytes;
  std::string second_bytes;
  std::string third_bytes;

  // Two stores built from the same sequence with the same identity must produce
  // identical canonical bytes.
  {
    PLR_EXPECT_OK(store_id, StoreId::parse("store-deterministic-000000000001"));
    fixed_id = store_id;
    RegistryOpenOptions options = writer_options();
    options.store_id = fixed_id;
    PLR_EXPECT_OK(registry, Registry::create(first_dir.path(), options));
    Fixture fixture(registry, 301);
    PLR_EXPECT_OK(all, fixture.standard_facility("FAC1", 1, 1, 1, 1));
    (void)all;
    PLR_EXPECT_OK(bytes, registry->encode_current_state());
    first_bytes = bytes;
    PLR_EXPECT(registry->close().has_value());
  }
  {
    RegistryOpenOptions options = writer_options();
    options.store_id = fixed_id;
    PLR_EXPECT_OK(registry, Registry::create(second_dir.path(), options));
    Fixture fixture(registry, 301);
    PLR_EXPECT_OK(all, fixture.standard_facility("FAC1", 1, 1, 1, 1));
    PLR_EXPECT_OK(bytes, registry->encode_current_state());
    second_bytes = bytes;
    // One extra location must produce different bytes.
    PLR_EXPECT_OK(extra_rack, fixture.add(all[3], LocationKind::Rack, "RACK-2"));
    (void)extra_rack;
    PLR_EXPECT_OK(bytes_after, registry->encode_current_state());
    third_bytes = bytes_after;
    PLR_EXPECT(registry->close().has_value());
  }

  PLR_EXPECT_EQ(first_bytes.size(), second_bytes.size());
  PLR_EXPECT(first_bytes == second_bytes);
  PLR_EXPECT(third_bytes != first_bytes);

  // Decoding and re-encoding preserves the bytes exactly.
  PLR_EXPECT_OK(decoded, decode_snapshot(first_bytes));
  PLR_EXPECT_OK(reencoded, encode_snapshot(decoded));
  PLR_EXPECT(reencoded == first_bytes);
  PLR_EXPECT_EQ(decoded.location_count(), 6U);

  // Different content produces different bytes and therefore a different digest.
  PLR_EXPECT_OK(larger_snapshot, decode_snapshot(third_bytes));
  PLR_EXPECT_EQ(larger_snapshot.location_count(), 7U);
  PLR_EXPECT_OK(first_digest, decoded.canonical_digest());
  PLR_EXPECT_OK(second_digest, larger_snapshot.canonical_digest());
  PLR_EXPECT_NE(first_digest, second_digest);
}

PLR_TEST(persistence, reopen_after_each_mutation_sees_the_committed_revision) {
  TempDir dir;
  std::vector<LocationRevision> revisions;
  {
    PLR_EXPECT_OK(registry, Registry::create(dir.path(), writer_options()));
    Fixture fixture(registry, 307);
    PLR_EXPECT_OK(all, fixture.standard_facility("FAC1", 2, 2, 1, 1));
    (void)all;
    PLR_EXPECT(registry->close().has_value());
  }

  PLR_EXPECT_OK(reader, Registry::open(dir.path(), reader_options()));
  PLR_EXPECT_EQ(reader->revision().value(), 16U);
  PLR_EXPECT_EQ(reader->statistics().locations, 16U);
  PLR_EXPECT_OK(listed, reader->list(ListOptions{}));
  PLR_EXPECT_EQ(listed.size(), std::size_t{16});
  // The listing is ordered by canonical address.
  for (std::size_t index = 1; index < listed.size(); ++index) {
    PLR_EXPECT(listed[index - 1].path() < listed[index].path());
  }
  PLR_EXPECT_OK(digest, reader->copy_snapshot()->canonical_digest());
  PLR_EXPECT_EQ(digest, reader->state_digest());
  (void)revisions;
}

PLR_TEST(persistence, publication_retires_superseded_generations) {
  TempDir dir;
  RegistryOpenOptions options = writer_options();
  Limits limits;
  limits.max_publications_retained = 2;
  options.limits = limits;
  PLR_EXPECT_OK(registry, Registry::create(dir.path(), options));
  Fixture fixture(registry, 311);
  PLR_EXPECT_OK(facility, fixture.facility("FAC1"));
  for (int index = 0; index < 5; ++index) {
    PLR_EXPECT_OK(room, fixture.add(facility, LocationKind::Room,
                                    "ROOM-" + std::to_string(index + 1)));
    (void)room;
  }
  const std::vector<std::string> names = file_names(dir.path());
  std::vector<std::uint64_t> sequences;
  for (const std::string& name : names) {
    if (name.rfind("state.", 0) == 0) {
      const std::string digits = name.substr(6, name.size() - 10);
      sequences.push_back(std::stoull(digits));
    }
  }
  std::sort(sequences.begin(), sequences.end());
  // The initial publication plus each mutation, minus the retired ones.
  PLR_EXPECT_EQ(sequences.size(), std::size_t{2});
  PLR_EXPECT_EQ(sequences[0] + 1U, sequences[1]);
  PLR_EXPECT(registry->verify_storage().has_value());
  PLR_EXPECT(registry->close().has_value());
  PLR_EXPECT_OK(reader, Registry::open(dir.path(), reader_options()));
  PLR_EXPECT_EQ(reader->statistics().locations, 6U);
}

PLR_TEST(persistence, injected_failures_before_the_commit_point_publish_nothing) {
  const PublishStage stages[] = {PublishStage::BeforeStateWrite, PublishStage::AfterStateWrite,
                                 PublishStage::BeforeStateRename, PublishStage::BeforeHeadPublish};
  for (const PublishStage stage : stages) {
    TempDir dir;
    RegistryOpenOptions options = writer_options();
    PLR_EXPECT_OK(seed, Registry::create(dir.path(), options));
    Fixture fixture(seed, 313);
    PLR_EXPECT_OK(facility, fixture.facility("FAC1"));
    PLR_EXPECT(seed->close().has_value());

    // Reopen with a fault plan that fails the publication after the one that
    // carries the reopening writer epoch.
    RegistryOpenOptions faulty = writer_options();
    faulty.faults.stage = stage;
    faulty.faults.action = FaultAction::Fail;
    faulty.faults.publication_ordinal = 2;
    PLR_EXPECT_OK(writer, Registry::open(dir.path(), faulty));
    const std::string baseline_digest = writer->state_digest();
    const StateSequence baseline_sequence = writer->sequence();
    const std::string baseline_head = read_text(dir.path() / "head");
    PLR_EXPECT_OK(before, writer->find(facility));

    auto request = RelabelRequest::make(facility.str(), "label after injected fault",
                                        fixture.context());
    PLR_REQUIRE(request.has_value());
    PLR_EXPECT_ERR(writer->relabel(request.value()), ErrorCode::InjectedFault);

    // Nothing authoritative moved: same revision, same published generation,
    // same bytes, same head pointer, and the in-memory state was rolled back.
    PLR_EXPECT_EQ(writer->state_digest(), baseline_digest);
    PLR_EXPECT(writer->sequence() == baseline_sequence);
    PLR_EXPECT_EQ(read_text(dir.path() / "head"), baseline_head);
    PLR_EXPECT_OK(unchanged, writer->find(facility));
    PLR_EXPECT_EQ(unchanged.label(), before.label());
    PLR_EXPECT(unchanged.generation() == before.generation());
    PLR_EXPECT(writer->verify_storage().has_value());
    PLR_EXPECT(writer->close().has_value());

    // A reader sees exactly the state that was committed before the fault.
    PLR_EXPECT_OK(reader, Registry::open(dir.path(), reader_options()));
    PLR_EXPECT_EQ(reader->state_digest(), baseline_digest);
    PLR_EXPECT_OK(reloaded, reader->find(facility));
    PLR_EXPECT_EQ(reloaded.label(), before.label());
    PLR_EXPECT(reader->recovery().clean());
  }
}

PLR_TEST(persistence, an_interrupted_publication_leaves_the_previous_head_in_place) {
  const PublishStage stages[] = {PublishStage::BeforeStateWrite, PublishStage::AfterStateWrite,
                                 PublishStage::BeforeStateRename, PublishStage::BeforeHeadPublish};
  for (const PublishStage stage : stages) {
    TempDir dir;
    {
      PLR_EXPECT_OK(seed, Registry::create(dir.path(), writer_options()));
      Fixture fixture(seed, 317);
      PLR_EXPECT_OK(created, fixture.facility("FAC1"));
      (void)created;
      PLR_EXPECT(seed->close().has_value());
    }
    const std::string committed_head = read_text(dir.path() / "head");

    RegistryOpenOptions faulty = writer_options();
    faulty.faults.stage = stage;
    faulty.faults.action = FaultAction::Fail;
    faulty.faults.publication_ordinal = 2;
    PLR_EXPECT_OK(writer, Registry::open(dir.path(), faulty));
    const std::string baseline_digest = writer->state_digest();
    const std::string baseline_head = read_text(dir.path() / "head");
    PLR_EXPECT_OK(roots, writer->roots());
    PLR_REQUIRE(roots.size() == 1);

    MutationContext context;
    const auto actor = ActorId::parse("actor-interrupted");
    PLR_REQUIRE(actor.has_value());
    context.actor = actor.value();
    const auto at = Timestamp::from_unix_seconds(1700000000);
    PLR_REQUIRE(at.has_value());
    context.at = at.value();
    auto request = RelabelRequest::make(roots[0].id.str(), "interrupted", context);
    PLR_REQUIRE(request.has_value());
    PLR_EXPECT_ERR(writer->relabel(request.value()), ErrorCode::InjectedFault);
    PLR_EXPECT(writer->close().has_value());

    // The head pointer still names the generation published before the
    // interruption; the interrupted generation was never referenced by it.
    PLR_EXPECT_EQ(read_text(dir.path() / "head"), baseline_head);
    PLR_EXPECT(baseline_head != committed_head);
    PLR_EXPECT(baseline_head.find("rev=1") != std::string::npos);
    PLR_EXPECT(baseline_head.find("epoch=2") != std::string::npos);

    PLR_EXPECT_OK(reader, Registry::open(dir.path(), reader_options()));
    PLR_EXPECT_EQ(reader->state_digest(), baseline_digest);
    PLR_EXPECT_EQ(reader->revision().value(), 1U);
    PLR_EXPECT_EQ(reader->statistics().locations, 1U);
    PLR_EXPECT(reader->recovery().clean());
  }
}

PLR_TEST(persistence, a_failure_after_the_commit_point_is_not_representable_as_a_failure) {
  TempDir dir;
  for (const PublishStage stage : {PublishStage::AfterHeadPublish, PublishStage::BeforeRetire}) {
    RegistryOpenOptions options = writer_options();
    options.create_if_missing = true;
    options.faults.stage = stage;
    options.faults.action = FaultAction::Fail;
    PLR_EXPECT_ERR(Registry::open(dir.path(), options), ErrorCode::InvalidArgument);
    PLR_EXPECT_ERR(Registry::create(dir.path(), options), ErrorCode::InvalidArgument);
  }
  // A crash action at those stages is a legitimate plan. It is armed for a
  // publication ordinal that never arrives, because an in-process crash would
  // end this test run: the real proof runs the crash in a child process.
  RegistryOpenOptions crash_plan = writer_options();
  crash_plan.faults.stage = PublishStage::AfterHeadPublish;
  crash_plan.faults.action = FaultAction::Crash;
  crash_plan.faults.publication_ordinal = 99;
  PLR_EXPECT_OK(registry, Registry::create(dir.path(), crash_plan));
  PLR_EXPECT_EQ(registry->revision().value(), 0U);
  PLR_EXPECT(registry->close().has_value());
}

PLR_TEST(persistence, recovery_rebuilds_head_from_the_newest_valid_publication) {
  TempDir dir;
  std::string committed_digest;
  {
    PLR_EXPECT_OK(registry, Registry::create(dir.path(), writer_options()));
    Fixture fixture(registry, 331);
    PLR_EXPECT_OK(all, fixture.standard_facility("FAC1", 1, 1, 1, 1));
    (void)all;
    committed_digest = registry->state_digest();
    PLR_EXPECT(registry->close().has_value());
  }

  // A reader whose head is damaged recovers the newest publication that still
  // verifies, and reports that it did so.
  write_text(dir.path() / "head", "PLRHEAD1 seq=1 store=store-x rev=0 epoch=0 file=state.1.plr\n");
  {
    PLR_EXPECT_OK(reader, Registry::open(dir.path(), reader_options()));
    PLR_EXPECT(reader->recovery().head_invalid);
    PLR_EXPECT(reader->recovery().recovered_older_publication);
    PLR_EXPECT_EQ(reader->recovery().recovered_sequence.value(), 7U);
    PLR_EXPECT_EQ(reader->statistics().locations, 6U);
    PLR_EXPECT_EQ(reader->state_digest(), committed_digest);
    PLR_EXPECT(reader->close().has_value());
  }

  // A writer heals the head and republishes the recovered state.
  {
    PLR_EXPECT_OK(writer, Registry::open(dir.path(), writer_options()));
    PLR_EXPECT(writer->recovery().head_invalid);
    PLR_EXPECT(writer->recovery().recovered_older_publication);
    PLR_EXPECT(writer->recovery().republished_head);
    PLR_EXPECT(writer->verify_storage().has_value());
    PLR_EXPECT(writer->close().has_value());
  }
  {
    PLR_EXPECT_OK(reader, Registry::open(dir.path(), reader_options()));
    PLR_EXPECT(reader->recovery().clean());
    PLR_EXPECT_EQ(reader->statistics().locations, 6U);
    PLR_EXPECT(reader->verify_storage().has_value());
  }
}

PLR_TEST(persistence, recovery_skips_corrupt_publications_and_refuses_when_none_verify) {
  TempDir dir;
  {
    PLR_EXPECT_OK(registry, Registry::create(dir.path(), writer_options()));
    Fixture fixture(registry, 337);
    PLR_EXPECT_OK(all, fixture.standard_facility("FAC1", 1, 1, 1, 1));
    (void)all;
    PLR_EXPECT(registry->close().has_value());
  }

  // Corrupt the newest publication and remove the head pointer.
  const std::string newest = "state.7.plr";
  {
    std::string bytes;
    {
      std::ifstream stream(dir.path() / newest, std::ios::binary);
      bytes.assign(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
    }
    PLR_REQUIRE(bytes.size() > 40);
    bytes[bytes.size() / 2] = static_cast<char>(bytes[bytes.size() / 2] ^ 0x5A);
    {
      std::ofstream stream(dir.path() / newest, std::ios::binary | std::ios::trunc);
      stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    }
    std::filesystem::remove(dir.path() / "head");
  }

  {
    PLR_EXPECT_OK(reader, Registry::open(dir.path(), reader_options()));
    PLR_EXPECT(reader->recovery().head_missing);
    PLR_EXPECT(reader->recovery().recovered_older_publication);
    PLR_EXPECT(reader->recovery().invalid_publications_skipped >= 1U);
    // The newest *valid* publication is the one before the last mutation.
    PLR_EXPECT_EQ(reader->statistics().locations, 5U);
  }

  // With every publication corrupt, opening refuses instead of presenting
  // unverified bytes as authoritative.
  for (const std::string& name : file_names(dir.path())) {
    if (name.rfind("state.", 0) == 0) {
      write_text(dir.path() / name, "not a publication at all");
    }
  }
  std::filesystem::remove(dir.path() / "head");
  PLR_EXPECT_ERR(Registry::open(dir.path(), reader_options()), ErrorCode::RecoveryUnavailable);
  PLR_EXPECT_ERR(Registry::open(dir.path(), writer_options()), ErrorCode::RecoveryUnavailable);
}

PLR_TEST(persistence, snapshot_files_can_be_compared_and_imported) {
  TempDir first;
  TempDir second;
  std::string first_bytes;
  std::string second_bytes;
  std::string first_digest;
  {
    PLR_EXPECT_OK(registry, Registry::create(first.path(), writer_options()));
    Fixture fixture(registry, 347);
    PLR_EXPECT_OK(all, fixture.standard_facility("FAC1", 1, 1, 1, 1));
    (void)all;
    PLR_EXPECT_OK(bytes, registry->encode_current_state());
    first_bytes = bytes;
    first_digest = registry->state_digest();
    PLR_EXPECT(registry->close().has_value());
  }
  {
    PLR_EXPECT_OK(registry, Registry::create(second.path(), writer_options()));
    Fixture fixture(registry, 347);
    PLR_EXPECT_OK(all, fixture.standard_facility("FAC1", 1, 1, 1, 1));
    (void)all;
    auto relabel = RelabelRequest::make(all[1].str(), "second store label", fixture.context());
    PLR_REQUIRE(relabel.has_value());
    PLR_EXPECT_OK(receipt, registry->relabel(relabel.value()));
    (void)receipt;
    PLR_EXPECT_OK(bytes, registry->encode_current_state());
    second_bytes = bytes;
    PLR_EXPECT(registry->close().has_value());
  }

  PLR_EXPECT_OK(before, decode_snapshot(first_bytes));
  PLR_EXPECT_OK(after, decode_snapshot(second_bytes));
  const RevisionDiff diff = diff_snapshots(before, after);
  PLR_EXPECT_EQ(diff.changed, 1U);
  PLR_EXPECT_EQ(diff.created, 0U);
  PLR_EXPECT_EQ(diff.removed, 0U);
  PLR_REQUIRE(diff.changes.size() == 1);
  PLR_EXPECT(diff.changes[0].has(kChangeRelabeled));
  PLR_EXPECT_EQ(diff.changes[0].label_after.value_or(std::string()),
                std::string("second store label"));

  // A decoded snapshot is a first-class view: it resolves, lists and digests.
  const Limits limits = before.limits();
  PLR_EXPECT_OK(path, LocationPath::parse("/FAC1/BLDG-1/ROOM-1/ROW-1/RACK-1/U1", limits));
  PLR_EXPECT_OK(resolved, before.resolve(path));
  PLR_EXPECT_EQ(resolved.canonical_path.to_string(), path.to_string());
  PLR_EXPECT_OK(digest, before.canonical_digest());
  PLR_EXPECT_EQ(digest, first_digest);
  PLR_EXPECT_EQ(before.statistics().locations, 6U);
  PLR_EXPECT_OK(aliases, before.aliases());
  PLR_EXPECT(aliases.empty());
}

PLR_TEST(persistence, reopen_write_read_cycle_is_repeatable) {
  TempDir dir;
  for (int round = 0; round < 4; ++round) {
    PLR_EXPECT_OK(registry, Registry::open(dir.path(), writer_options()));
    Fixture fixture(registry, static_cast<std::uint64_t>(400 + round));
    if (round == 0) {
      PLR_EXPECT_OK(facility, fixture.facility("FAC1"));
      (void)facility;
    } else {
      PLR_EXPECT_OK(roots, registry->roots());
      PLR_REQUIRE(roots.size() == 1);
      // Identities are stable and never recycled, so each round names its own.
      auto request = CreateLocationRequest::make("loc-round-" + std::to_string(round),
                                                 LocationKind::Room, roots[0].id.str(),
                                                 "ROOM-" + std::to_string(round), "",
                                                 fixture.context());
      PLR_REQUIRE(request.has_value());
      PLR_EXPECT_OK(created, registry->create_location(request.value()));
      (void)created;
    }
    PLR_EXPECT_EQ(registry->statistics().locations, static_cast<std::uint32_t>(round + 1));
    PLR_EXPECT_EQ(registry->epoch().value(), static_cast<std::uint64_t>(round + 1));
    PLR_EXPECT(registry->verify_storage().has_value());
    PLR_EXPECT(registry->close().has_value());
  }
  PLR_EXPECT_OK(reader, Registry::open(dir.path(), reader_options()));
  PLR_EXPECT_EQ(reader->statistics().locations, 4U);
  PLR_EXPECT_EQ(reader->revision().value(), 4U);
  PLR_EXPECT_EQ(reader->epoch().value(), 4U);
}
