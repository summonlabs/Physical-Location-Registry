// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Real multi-process proof: writer fencing, reader isolation, crash release of
// the writer lock, epoch fencing after a restart, and crash behaviour at every
// publication stage. Children are real operating-system processes; nothing here
// is simulated in-process.

#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "dccp/physical_location_registry/physical_location_registry.hpp"
#include "support/synthetic.hpp"
#include "support/test_harness.hpp"
#include "support/test_process.hpp"

using namespace dccp::physical_location_registry;
using plr_test::ChildProcess;
using plr_test::Fixture;
using plr_test::run_process;
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

const std::filesystem::path* agent_path() {
  static const std::filesystem::path path = []() {
    const std::string* option = plr_test::test_option("--agent");
    return option == nullptr ? std::filesystem::path() : std::filesystem::path(*option);
  }();
  return path.empty() ? nullptr : &path;
}

const std::filesystem::path* cli_path() {
  static const std::filesystem::path path = []() {
    const std::string* option = plr_test::test_option("--cli");
    return option == nullptr ? std::filesystem::path() : std::filesystem::path(*option);
  }();
  return path.empty() ? nullptr : &path;
}

void write_text(const std::filesystem::path& path, const std::string& text) {
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  stream << text;
}

/// Seeds a store with one facility and returns its identity.
Result<LocationId> seed_store(const std::filesystem::path& dir, std::uint64_t fixture_seed) {
  PLR_TRY(registry, Registry::create(dir, writer_options()));
  Fixture fixture(registry, fixture_seed);
  PLR_TRY(facility, fixture.facility("FAC1"));
  PLR_EXPECT(registry->close().has_value());
  return facility;
}

}  // namespace

PLR_TEST(multiprocess, writer_lock_is_exclusive_across_processes) {
  if (agent_path() == nullptr) {
    PLR_FAIL("the store agent path was not provided with --agent");
    return;
  }
  TempDir dir;
  PLR_EXPECT_OK(facility, seed_store(dir.path(), 801));
  (void)facility;

  PLR_EXPECT_OK(registry, Registry::open(dir.path(), writer_options()));
  PLR_EXPECT_OK(roots, registry->roots());
  PLR_REQUIRE(roots.size() == 1);
  const LocationId parent = roots[0].id;

  // While this process owns the writer lock, another process cannot write.
  std::string output;
  PLR_EXPECT_EQ(run_process(*agent_path(),
                            {"write", "--store", dir.path().string(), "--id", "loc-child",
                             "--component", "CHILD1", "--kind", "room", "--parent", parent.str()},
                            &output).value(), 3);
  PLR_EXPECT(output.find("STORE_LOCKED") != std::string::npos);

  // A read-only process may open the same store and sees the committed state.
  output.clear();
  PLR_EXPECT_EQ(run_process(*agent_path(),
                            {"read", "--store", dir.path().string()}, &output).value(), 0);
  PLR_EXPECT(output.find("locations=1") != std::string::npos);
  PLR_EXPECT(output.find("revision=1") != std::string::npos);

  // The reader also resolves by address from its own process.
  output.clear();
  PLR_EXPECT_EQ(run_process(*agent_path(),
                            {"read", "--store", dir.path().string(), "--path", "/FAC1"}, &output).value(), 0);
  PLR_EXPECT(output.find("path=/FAC1") != std::string::npos);

  // After this process closes, the child can take the writer lock and publish.
  PLR_EXPECT(registry->close().has_value());
  output.clear();
  PLR_EXPECT_EQ(run_process(*agent_path(),
                            {"write", "--store", dir.path().string(), "--id", "loc-child",
                             "--component", "CHILD1", "--kind", "room", "--parent", parent.str()},
                            &output).value(), 0);
  PLR_EXPECT(output.find("revision=2") != std::string::npos);

  // The write is durable and visible to a fresh session in this process.
  PLR_EXPECT_OK(reader, Registry::open(dir.path(), reader_options()));
  PLR_EXPECT_EQ(reader->revision().value(), 2U);
  PLR_EXPECT_EQ(reader->statistics().locations, 2U);
  const Limits limits = reader->limits();
  PLR_EXPECT_OK(address, LocationPath::parse("/FAC1/CHILD1", limits));
  PLR_EXPECT_OK(resolved, reader->resolve(address));
  PLR_EXPECT_EQ(resolved.id.str(), std::string("loc-child"));
}

PLR_TEST(multiprocess, a_crashed_writer_releases_the_lock_and_advances_the_epoch) {
  if (agent_path() == nullptr) {
    PLR_FAIL("the store agent path was not provided with --agent");
    return;
  }
  TempDir dir;
  PLR_EXPECT_OK(facility, seed_store(dir.path(), 809));
  (void)facility;

  // A child takes the writer lock and dies without unwinding.
  const std::filesystem::path ready = dir.file("ready.txt");
  auto started = ChildProcess::start(*agent_path(), {"crash-hold", "--store", dir.path().string(),
                                                     "--ready-file", ready.string()});
  PLR_REQUIRE(started.has_value());
  std::unique_ptr<ChildProcess> child = std::move(started.value());
  PLR_REQUIRE(plr_test::wait_for_file(ready));
  PLR_EXPECT_EQ(child->wait().value(), 70);  // the documented abrupt-exit code
  const std::string child_output = child->output().value();
  // The child is the second writer incarnation: the seeding session was epoch 1.
  PLR_EXPECT(child_output.find("crashing epoch=2") != std::string::npos);

  // The operating system released the lock, so a new writer takes over and the
  // durable epoch advances past the crashed incarnation.
  PLR_EXPECT_OK(writer, Registry::open(dir.path(), writer_options()));
  PLR_EXPECT_EQ(writer->epoch().value(), 3U);
  PLR_EXPECT_EQ(writer->revision().value(), 1U);
  PLR_EXPECT(writer->verify_storage().has_value());

  // A mutation authorized under the crashed epoch is refused here.
  MutationContext context;
  const auto actor = ActorId::parse("actor-1");
  PLR_REQUIRE(actor.has_value());
  context.actor = actor.value();
  const auto at = Timestamp::from_unix_seconds(1700000000);
  PLR_REQUIRE(at.has_value());
  context.at = at.value();
  MutationAuthority stale;
  stale.store_id = writer->store_id();
  stale.writer_epoch = WriterEpoch(2);  // the epoch the crashed child held
  context.authority = stale;
  PLR_EXPECT_OK(roots, writer->roots());
  PLR_REQUIRE(roots.size() == 1);
  auto request = RelabelRequest::make(roots[0].id.str(), "after crash", context);
  PLR_REQUIRE(request.has_value());
  PLR_EXPECT_ERR(writer->relabel(request.value()), ErrorCode::StaleAuthorityEpoch);

  // The same request now carries the authority of the live session and succeeds.
  request.value().context.authority = writer->authority();
  PLR_EXPECT_OK(receipt, writer->relabel(request.value()));
  PLR_EXPECT_EQ(receipt.revision.value(), 2U);
  PLR_EXPECT(writer->close().has_value());
}

PLR_TEST(multiprocess, crash_during_publication_has_exactly_two_outcomes) {
  if (agent_path() == nullptr) {
    PLR_FAIL("the store agent path was not provided with --agent");
    return;
  }
  struct Stage {
    const char* name;
    bool committed;  // true when the crash happens after the head switch
  };
  const Stage stages[] = {
      {"before-state-write", false}, {"after-state-write", false},
      {"before-state-rename", false}, {"before-head-publish", false},
      {"after-head-publish", true},  {"before-retire", true},
  };

  for (const Stage& stage : stages) {
    TempDir dir;
    PLR_EXPECT_OK(facility, seed_store(dir.path(), 811));
    (void)facility;

    std::string output;
    const int code =
        run_process(*agent_path(),
                    {"crash-publish", "--store", dir.path().string(), "--stage", stage.name,
                     "--id", "loc-crash", "--component", "CRASH1", "--kind", "facility"},
                    &output)
            .value();
    PLR_EXPECT_EQ(code, 70);
    PLR_EXPECT(output.find("publishing epoch=2") != std::string::npos);

    // Whatever the stage, the store opens, verifies, and presents one of the two
    // complete states: never a torn or partially applied one.
    PLR_EXPECT_OK(reader, Registry::open(dir.path(), reader_options()));
    PLR_EXPECT(reader->verify_storage().has_value());
    if (stage.committed) {
      PLR_EXPECT_EQ(reader->revision().value(), 2U);
      PLR_EXPECT_EQ(reader->statistics().locations, 2U);
      PLR_EXPECT_OK(crash_view, reader->find(LocationId::parse("loc-crash").value()));
      PLR_EXPECT_EQ(crash_view.path().to_string(), std::string("/CRASH1"));
    } else {
      PLR_EXPECT_EQ(reader->revision().value(), 1U);
      PLR_EXPECT_EQ(reader->statistics().locations, 1U);
      PLR_EXPECT_ERR(reader->find(LocationId::parse("loc-crash").value()), ErrorCode::NotFound);
    }

    // A writer can always take over from the crashed incarnation and continue.
    PLR_EXPECT_OK(writer, Registry::open(dir.path(), writer_options()));
    PLR_EXPECT_EQ(writer->epoch().value(), 3U);
    PLR_EXPECT(writer->verify_storage().has_value());
    MutationContext context;
    const auto actor = ActorId::parse("actor-recovery");
    PLR_REQUIRE(actor.has_value());
    context.actor = actor.value();
    auto request = CreateLocationRequest::make("loc-after-crash", LocationKind::Facility, "",
                                               "AFTER1", "", context);
    PLR_REQUIRE(request.has_value());
    PLR_EXPECT_OK(receipt, writer->create_location(request.value()));
    PLR_EXPECT(receipt.revision.value() > 0U);
    PLR_EXPECT(writer->close().has_value());
  }
}

PLR_TEST(multiprocess, a_held_lock_can_be_released_by_a_signal_file) {
  if (agent_path() == nullptr) {
    PLR_FAIL("the store agent path was not provided with --agent");
    return;
  }
  TempDir dir;
  PLR_EXPECT_OK(facility, seed_store(dir.path(), 823));
  (void)facility;

  const std::filesystem::path ready = dir.file("ready.txt");
  const std::filesystem::path release = dir.file("release.txt");
  auto started = ChildProcess::start(*agent_path(),
                                     {"hold-lock", "--store", dir.path().string(), "--ready-file",
                                      ready.string(), "--release-file", release.string()});
  PLR_REQUIRE(started.has_value());
  std::unique_ptr<ChildProcess> child = std::move(started.value());
  PLR_REQUIRE(plr_test::wait_for_file(ready));

  // The lock is held by the child: this process cannot take it.
  PLR_EXPECT_ERR(Registry::open(dir.path(), writer_options()), ErrorCode::StoreLocked);
  // A reader still works, and sees the committed revision rather than a torn one.
  PLR_EXPECT_OK(reader, Registry::open(dir.path(), reader_options()));
  PLR_EXPECT_EQ(reader->revision().value(), 1U);
  PLR_EXPECT(reader->verify_storage().has_value());

  write_text(release, "go\n");
  PLR_EXPECT_EQ(child->wait().value(), 0);
  // The seeding session was epoch 1, so the child holds epoch 2.
  PLR_EXPECT(child->output().value().find("epoch=2") != std::string::npos);

  // Once released, this process can take the writer lock and the epoch advances.
  PLR_EXPECT_OK(writer, Registry::open(dir.path(), writer_options()));
  PLR_EXPECT_EQ(writer->epoch().value(), 3U);
  PLR_EXPECT(writer->close().has_value());
}

PLR_TEST(multiprocess, cli_changes_are_durable_and_rejected_operations_signal_their_category) {
  if (cli_path() == nullptr) {
    PLR_FAIL("the inspection tool path was not provided with --cli");
    return;
  }
  TempDir dir;
  const std::string store = dir.path().string();

  std::string output;
  PLR_EXPECT_EQ(run_process(*cli_path(), {"init", "--store", store}, &output).value(), 0);
  PLR_EXPECT(output.find("store=") != std::string::npos);

  PLR_EXPECT_EQ(run_process(*cli_path(),
                            {"--store", store, "create", "--id", "loc-fac", "--kind", "facility",
                             "--component", "FAC1", "--label", "Facility one", "--actor", "ops",
                             "--at", "2026-01-01T00:00:00Z"},
                            &output).value(), 0);
  PLR_EXPECT(output.find("revision=1") != std::string::npos);

  PLR_EXPECT_EQ(run_process(*cli_path(),
                            {"--store", store, "create", "--id", "loc-room", "--kind", "room",
                             "--parent", "loc-fac", "--component", "ROOM-1", "--actor", "ops"},
                            &output).value(), 0);
  PLR_EXPECT_EQ(run_process(*cli_path(),
                            {"--store", store, "create", "--id", "loc-rack", "--kind", "rack",
                             "--parent", "loc-room", "--component", "RACK-1", "--u-height", "48",
                             "--actor", "ops"},
                            &output).value(), 0);

  // Structure rejections exit with 5 and print the stable machine-readable code.
  PLR_EXPECT_EQ(run_process(*cli_path(),
                            {"--store", store, "create", "--id", "loc-rack2", "--kind", "rack",
                             "--parent", "loc-room", "--component", "RACK-1", "--actor", "ops"},
                            &output).value(), 5);
  PLR_EXPECT(output.find("ADDRESS_IN_USE") != std::string::npos);

  // Usage rejections exit with 2.
  PLR_EXPECT_EQ(run_process(*cli_path(),
                            {"--store", store, "create", "--id", "bad id", "--kind", "rack",
                             "--parent", "loc-room", "--component", "RACK-3", "--actor", "ops"},
                            &output).value(), 2);
  PLR_EXPECT(output.find("MALFORMED_IDENTIFIER") != std::string::npos);

  // Store problems exit with 3.
  PLR_EXPECT_EQ(run_process(*cli_path(),
                            {"--store", dir.file("absent").string(), "status"}, &output).value(), 3);
  PLR_EXPECT(output.find("STORE_NOT_FOUND") != std::string::npos);

  // A read-only inspection succeeds and reports what the writer committed.
  PLR_EXPECT_EQ(run_process(*cli_path(),
                            {"--store", store, "resolve", "--path", "/FAC1/ROOM-1/RACK-1"},
                            &output).value(), 0);
  PLR_EXPECT(output.find("id=loc-rack") != std::string::npos);
  PLR_EXPECT(output.find("generation=1") != std::string::npos);

  PLR_EXPECT_EQ(run_process(*cli_path(), {"--store", store, "verify"}, &output).value(), 0);
  PLR_EXPECT(output.find("verified") != std::string::npos);

  // The state the tool produced is a normal store this process can open.
  PLR_EXPECT_OK(reader, Registry::open(dir.path(), reader_options()));
  PLR_EXPECT_EQ(reader->statistics().locations, 3U);
  PLR_EXPECT_EQ(reader->revision().value(), 3U);
  PLR_EXPECT_OK(view, reader->find(LocationId::parse("loc-rack").value()));
  PLR_EXPECT_EQ(view.label(), std::string(""));

  // Damaging the published state makes verification fail with the integrity
  // exit code rather than reporting success.
  const std::filesystem::path state_file = dir.path() / reader->state_file_name();
  PLR_REQUIRE(std::filesystem::exists(state_file));
  std::string bytes;
  {
    std::ifstream stream(state_file, std::ios::binary);
    bytes.assign(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
  }
  PLR_REQUIRE(bytes.size() > 100);
  bytes[60] = static_cast<char>(bytes[60] ^ 0x33);
  write_text(state_file, bytes);
  PLR_EXPECT(reader->close().has_value());
  PLR_EXPECT_EQ(run_process(*cli_path(), {"--store", store, "verify"}, &output).value(), 4);
  PLR_EXPECT(output.find("DIGEST_MISMATCH") != std::string::npos);
}

PLR_TEST(multiprocess, sequential_writers_in_separate_processes_keep_the_chain) {
  if (agent_path() == nullptr) {
    PLR_FAIL("the store agent path was not provided with --agent");
    return;
  }
  TempDir dir;
  PLR_EXPECT_OK(facility, seed_store(dir.path(), 829));
  (void)facility;

  // Four sequential child writers, each in its own process, each taking and
  // releasing the writer lock; every one must see the previous revision.
  for (int round = 1; round <= 4; ++round) {
    std::string output;
    const std::string id = "loc-round-" + std::to_string(round);
    const std::string component = "ROOM-" + std::to_string(round);
    PLR_EXPECT_EQ(run_process(*agent_path(),
                              {"write", "--store", dir.path().string(), "--id", id, "--component",
                               component, "--kind", "room", "--parent", facility.str()},
                              &output).value(), 0);
    PLR_EXPECT(output.find("revision=" + std::to_string(round + 1)) != std::string::npos);
  }

  PLR_EXPECT_OK(reader, Registry::open(dir.path(), reader_options()));
  PLR_EXPECT_EQ(reader->revision().value(), 5U);
  PLR_EXPECT_EQ(reader->statistics().locations, 5U);
  PLR_EXPECT_EQ(reader->epoch().value(), 5U);
  PLR_EXPECT(reader->verify_storage().has_value());
  for (int round = 1; round <= 4; ++round) {
    PLR_EXPECT_OK(view, reader->find(LocationId::parse("loc-round-" + std::to_string(round)).value()));
    PLR_EXPECT_EQ(view.path().to_string(),
                  std::string("/FAC1/ROOM-" + std::to_string(round)));
  }
}
