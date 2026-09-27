// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Authority, generations and idempotency: stale preconditions must be rejected
// rather than applied, a retry must not apply a mutation twice, and a superseded
// writer session must not be able to mutate state it no longer owns.

#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "dccp/physical_location_registry/physical_location_registry.hpp"
#include "support/synthetic.hpp"
#include "support/test_harness.hpp"

using namespace dccp::physical_location_registry;
using plr_test::Fixture;
using plr_test::Rng;
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

}  // namespace

PLR_TEST(authority, stale_generation_is_rejected) {
  TempDir dir;
  PLR_EXPECT_OK(registry, Registry::create(dir.path(), writer_options()));
  Fixture fixture(registry, 201);
  PLR_EXPECT_OK(facility, fixture.facility("FAC1"));
  PLR_EXPECT_OK(room, fixture.add(facility, LocationKind::Room, "ROOM-1"));

  PLR_EXPECT_OK(view, registry->find(room));
  const LocationGeneration original = view.generation();
  PLR_REQUIRE(original.value() == 1U);

  // A mutation that assumed generation 1 succeeds...
  auto first = RelabelRequest::make(room.str(), "First rename",
                                    fixture.context_with(original));
  PLR_REQUIRE(first.has_value());
  PLR_EXPECT_OK(first_receipt, registry->relabel(first.value()));
  (void)first_receipt;

  // ...and the same assumption is stale afterwards.
  auto second = RelabelRequest::make(room.str(), "Second rename",
                                     fixture.context_with(original));
  PLR_REQUIRE(second.has_value());
  PLR_EXPECT_ERR(registry->relabel(second.value()), ErrorCode::StaleGeneration);

  PLR_EXPECT_OK(after, registry->find(room));
  PLR_EXPECT_EQ(after.generation().value(), 2U);
  PLR_EXPECT_EQ(after.label(), std::string("First rename"));

  // The current generation is accepted.
  auto third = RelabelRequest::make(room.str(), "Second rename",
                                    fixture.context_with(after.generation()));
  PLR_REQUIRE(third.has_value());
  PLR_EXPECT_OK(third_receipt, registry->relabel(third.value()));
  (void)third_receipt;

  // A generation that was never published is rejected too.
  auto future = RelabelRequest::make(room.str(), "Third rename",
                                     fixture.context_with(LocationGeneration(99)));
  PLR_REQUIRE(future.has_value());
  PLR_EXPECT_ERR(registry->relabel(future.value()), ErrorCode::StaleGeneration);
}

PLR_TEST(authority, stale_revision_is_rejected) {
  TempDir dir;
  PLR_EXPECT_OK(registry, Registry::create(dir.path(), writer_options()));
  Fixture fixture(registry, 203);
  PLR_EXPECT_OK(facility, fixture.facility("FAC1"));

  const LocationRevision opening = registry->revision();
  PLR_EXPECT_OK(room, fixture.add(facility, LocationKind::Room, "ROOM-1"));

  auto stale = CreateLocationRequest::make("loc-new", LocationKind::Room, facility.str(), "ROOM-2",
                                           "", fixture.context());
  PLR_REQUIRE(stale.has_value());
  stale.value().context.expected_revision = opening;
  PLR_EXPECT_ERR(registry->create_location(stale.value()), ErrorCode::StaleRevision);

  stale.value().context.expected_revision = registry->revision();
  PLR_EXPECT_OK(receipt, registry->create_location(stale.value()));
  (void)receipt;
  PLR_EXPECT_OK(room_path, registry->path_of(room));
  PLR_EXPECT_EQ(room_path.to_string(), std::string("/FAC1/ROOM-1"));
}

PLR_TEST(authority, superseded_writer_epoch_is_rejected) {
  TempDir dir;
  LocationId room;
  MutationAuthority stale_authority;
  {
    PLR_EXPECT_OK(registry, Registry::create(dir.path(), writer_options()));
    Fixture fixture(registry, 207);
    PLR_EXPECT_OK(facility, fixture.facility("FAC1"));
    PLR_EXPECT_OK(created, fixture.add(facility, LocationKind::Room, "ROOM-1"));
    room = created;
    PLR_REQUIRE(registry->authority().has_value());
    stale_authority = registry->authority().value();
    PLR_EXPECT_EQ(stale_authority.writer_epoch.value(), 1U);
    PLR_EXPECT(registry->close().has_value());
  }

  PLR_EXPECT_OK(reopened, Registry::open(dir.path(), writer_options()));
  PLR_EXPECT_EQ(reopened->epoch().value(), 2U);

  MutationContext context;
  const auto actor = ActorId::parse("actor-1");
  PLR_REQUIRE(actor.has_value());
  context.actor = actor.value();
  const auto at = Timestamp::from_unix_seconds(1700000000);
  PLR_REQUIRE(at.has_value());
  context.at = at.value();
  context.authority = stale_authority;

  auto request = RelabelRequest::make(room.str(), "Renamed", context);
  PLR_REQUIRE(request.has_value());
  PLR_EXPECT_ERR(reopened->relabel(request.value()), ErrorCode::StaleAuthorityEpoch);

  // The authority of the current session is accepted.
  auto current = reopened->authority();
  PLR_REQUIRE(current.has_value());
  request.value().context.authority = current.value();
  PLR_EXPECT_OK(receipt, reopened->relabel(request.value()));
  (void)receipt;

  // An authority from a different store is rejected outright.
  auto other_store = StoreId::parse("store-somewhere-else");
  PLR_REQUIRE(other_store.has_value());
  MutationAuthority foreign;
  foreign.store_id = other_store.value();
  foreign.writer_epoch = current.value().writer_epoch;
  request.value().context.authority = foreign;
  PLR_EXPECT_ERR(reopened->relabel(request.value()), ErrorCode::StoreMismatch);
}

PLR_TEST(authority, second_writer_cannot_open_the_same_store) {
  TempDir dir;
  PLR_EXPECT_OK(registry, Registry::create(dir.path(), writer_options()));
  Fixture fixture(registry, 211);
  PLR_EXPECT_OK(facility, fixture.facility("FAC1"));

  PLR_EXPECT_ERR(Registry::open(dir.path(), writer_options()), ErrorCode::StoreLocked);

  // A reader may still open the store and see the committed state.
  PLR_EXPECT_OK(reader, Registry::open(dir.path(), reader_options()));
  PLR_EXPECT_EQ(reader->revision().value(), registry->revision().value());
  PLR_EXPECT_OK(view, reader->find(facility));
  PLR_EXPECT_EQ(view.path().to_string(), std::string("/FAC1"));

  // Closing the writer releases the lock.
  PLR_EXPECT(registry->close().has_value());
  PLR_EXPECT_OK(second_writer, Registry::open(dir.path(), writer_options()));
  PLR_EXPECT_EQ(second_writer->epoch().value(), 2U);
  PLR_EXPECT(second_writer->close().has_value());
}

PLR_TEST(authority, mutations_require_an_actor) {
  TempDir dir;
  PLR_EXPECT_OK(registry, Registry::create(dir.path(), writer_options()));
  Fixture fixture(registry, 213);

  MutationContext context = fixture.context();
  context.actor = ActorId();
  auto request = CreateLocationRequest::make("loc-fac", LocationKind::Facility, "", "FAC1", "",
                                             context);
  PLR_REQUIRE(request.has_value());
  PLR_EXPECT_ERR(registry->create_location(request.value()), ErrorCode::InvalidArgument);

  MutationContext bad_reason = fixture.context();
  bad_reason.reason = std::string("control\x01character");
  auto reason_request = CreateLocationRequest::make("loc-fac", LocationKind::Facility, "", "FAC1",
                                                    "", bad_reason);
  PLR_REQUIRE(reason_request.has_value());
  PLR_EXPECT_ERR(registry->create_location(reason_request.value()), ErrorCode::MalformedText);

  MutationContext long_reason = fixture.context();
  long_reason.reason = std::string(registry->limits().max_reason_bytes + 1U, 'r');
  auto long_request = CreateLocationRequest::make("loc-fac", LocationKind::Facility, "", "FAC1", "",
                                                  long_reason);
  PLR_REQUIRE(long_request.has_value());
  PLR_EXPECT_ERR(registry->create_location(long_request.value()), ErrorCode::MalformedText);
}

PLR_TEST(authority, malformed_request_text_is_rejected_at_construction) {
  MutationContext context;
  const auto actor = ActorId::parse("actor-1");
  PLR_REQUIRE(actor.has_value());
  context.actor = actor.value();

  PLR_EXPECT_ERR(CreateLocationRequest::make("bad id", LocationKind::Facility, "", "FAC1", "",
                                             context),
                 ErrorCode::MalformedIdentifier);
  PLR_EXPECT_ERR(CreateLocationRequest::make("loc-1", LocationKind::Facility, "", "bad/component",
                                             "", context),
                 ErrorCode::MalformedAddressComponent);
  PLR_EXPECT_ERR(CreateLocationRequest::make("loc-1", LocationKind::Facility, "", "FAC1",
                                             std::string("bad\x7Flabel"), context),
                 ErrorCode::MalformedLabel);
  PLR_EXPECT_ERR(CreateLocationRequest::make("loc-1", LocationKind::Facility, "bad parent", "FAC1",
                                             "", context),
                 ErrorCode::MalformedIdentifier);
  PLR_EXPECT_ERR(RelabelRequest::make("loc-1", std::string("line\nbreak"), context),
                 ErrorCode::MalformedLabel);
  PLR_EXPECT_ERR(MoveLocationRequest::make("loc-1", "", context), ErrorCode::MalformedIdentifier);
  PLR_EXPECT_ERR(ReplaceLocationRequest::make("loc-1", "bad id", "", context),
                 ErrorCode::MalformedIdentifier);

  Limits limits;
  PLR_EXPECT_ERR(AddAliasRequest::make("loc-1", "relative/path", limits, context),
                 ErrorCode::MalformedPath);
  PLR_EXPECT_ERR(AddAliasRequest::make("loc-1", "/", limits, context), ErrorCode::MalformedPath);
  PLR_EXPECT_ERR(AddAliasRequest::make("loc-1", "/FAC/../etc", limits, context),
                 ErrorCode::MalformedAddressComponent);
}

PLR_TEST(authority, idempotent_replay_returns_the_recorded_outcome) {
  TempDir dir;
  PLR_EXPECT_OK(registry, Registry::create(dir.path(), writer_options()));
  Fixture fixture(registry, 217);
  PLR_EXPECT_OK(facility, fixture.facility("FAC1"));

  MutationContext context = fixture.context_with_operation("op-create-room");
  auto request = CreateLocationRequest::make("loc-room", LocationKind::Room, facility.str(),
                                             "ROOM-1", "", context);
  PLR_REQUIRE(request.has_value());
  PLR_EXPECT_OK(first, registry->create_location(request.value()));
  PLR_EXPECT(!first.replayed);
  const std::uint64_t revision_after_first = registry->revision().value();

  // The identical retry replays the receipt and changes nothing.
  PLR_EXPECT_OK(second, registry->create_location(request.value()));
  PLR_EXPECT(second.replayed);
  PLR_EXPECT(second.revision == first.revision);
  PLR_EXPECT_EQ(second.generation.value(), first.generation.value());
  PLR_EXPECT_EQ(second.affected_locations, first.affected_locations);
  PLR_EXPECT_EQ(registry->revision().value(), revision_after_first);
  PLR_EXPECT_EQ(registry->statistics().locations, 2U);

  // A different request under the same operation id is a conflict.
  auto conflicting = CreateLocationRequest::make("loc-room2", LocationKind::Room, facility.str(),
                                                 "ROOM-2", "", context);
  PLR_REQUIRE(conflicting.has_value());
  PLR_EXPECT_ERR(registry->create_location(conflicting.value()), ErrorCode::OperationIdConflict);
  PLR_EXPECT_EQ(registry->revision().value(), revision_after_first);

  // A replay is not blocked by a precondition the original request satisfied.
  MutationContext stale_context = fixture.context_with_operation("op-relabel");
  auto relabel = RelabelRequest::make(request.value().id.str(), "Renamed", stale_context);
  PLR_REQUIRE(relabel.has_value());
  PLR_EXPECT_OK(relabel_first, registry->relabel(relabel.value()));
  PLR_EXPECT_OK(relabel_second, registry->relabel(relabel.value()));
  PLR_EXPECT(relabel_second.replayed);
  PLR_EXPECT(relabel_second.revision == relabel_first.revision);
}

PLR_TEST(authority, idempotency_survives_a_restart) {
  TempDir dir;
  MutationContext context;
  {
    PLR_EXPECT_OK(registry, Registry::create(dir.path(), writer_options()));
    Fixture fixture(registry, 219);
    PLR_EXPECT_OK(facility, fixture.facility("FAC1"));
    context = fixture.context_with_operation("op-create-room");
    auto request = CreateLocationRequest::make("loc-room", LocationKind::Room, facility.str(),
                                               "ROOM-1", "", context);
    PLR_REQUIRE(request.has_value());
    PLR_EXPECT_OK(receipt, registry->create_location(request.value()));
    (void)receipt;
    PLR_EXPECT(registry->close().has_value());
  }
  {
    PLR_EXPECT_OK(reopened, Registry::open(dir.path(), writer_options()));
    const std::uint64_t before = reopened->revision().value();
    auto request = CreateLocationRequest::make("loc-room", LocationKind::Room,
                                               reopened->copy_snapshot()->roots().value()[0].id.str(),
                                               "ROOM-1", "", context);
    PLR_REQUIRE(request.has_value());
    auto replayed = reopened->create_location(request.value());
    if (replayed.has_value()) {
      PLR_EXPECT(replayed.value().replayed);
      PLR_EXPECT_EQ(reopened->revision().value(), before);
    } else {
      // A replay whose fingerprint differs is a conflict, never a second apply.
      PLR_EXPECT_EQ(replayed.error().code(), ErrorCode::OperationIdConflict);
    }
    PLR_EXPECT_EQ(reopened->statistics().locations, 2U);
    const std::shared_ptr<const Snapshot> restarted = reopened->copy_snapshot();
    PLR_REQUIRE(restarted != nullptr);
    PLR_EXPECT_EQ(restarted->operation_receipts().size(), std::size_t{1});
  }
}

PLR_TEST(authority, receipts_are_bounded_and_evict_oldest_first) {
  TempDir dir;
  RegistryOpenOptions options = writer_options();
  Limits limits;
  limits.max_operation_receipts = 3;
  options.limits = limits;
  PLR_EXPECT_OK(registry, Registry::create(dir.path(), options));
  Fixture fixture(registry, 223);
  PLR_EXPECT_OK(facility, fixture.facility("FAC1"));

  for (int index = 0; index < 6; ++index) {
    const std::string id = "op-" + std::to_string(index);
    MutationContext context = fixture.context_with_operation(id);
    const std::string location_id = "loc-" + std::to_string(index);
    const std::string component = "ROOM-" + std::to_string(index + 1);
    auto request = CreateLocationRequest::make(location_id, LocationKind::Room, facility.str(),
                                               component, "", context);
    PLR_REQUIRE(request.has_value());
    PLR_EXPECT_OK(receipt, registry->create_location(request.value()));
    PLR_EXPECT(!receipt.replayed);
  }

  const std::shared_ptr<const Snapshot> snapshot = registry->copy_snapshot();
  PLR_REQUIRE(snapshot != nullptr);
  PLR_EXPECT_EQ(snapshot->operation_receipts().size(), std::size_t{3});
  PLR_EXPECT_EQ(snapshot->statistics().operation_receipts, 3U);
  // The oldest receipts were evicted, so replaying one applies the mutation
  // again: that is exactly the documented bound, not a silent replay.
  MutationContext evicted = fixture.context_with_operation("op-0");
  auto request = CreateLocationRequest::make("loc-0", LocationKind::Room, facility.str(), "ROOM-1",
                                             "", evicted);
  PLR_REQUIRE(request.has_value());
  PLR_EXPECT_ERR(registry->create_location(request.value()), ErrorCode::AlreadyPresent);
}

PLR_TEST(authority, closed_sessions_reject_every_operation) {
  TempDir dir;
  PLR_EXPECT_OK(registry, Registry::create(dir.path(), writer_options()));
  Fixture fixture(registry, 227);
  PLR_EXPECT_OK(facility, fixture.facility("FAC1"));
  PLR_EXPECT(registry->close().has_value());
  PLR_EXPECT(registry->close().has_value());  // idempotent

  auto request = CreateLocationRequest::make("loc-room", LocationKind::Room, facility.str(),
                                             "ROOM-1", "", fixture.context());
  PLR_REQUIRE(request.has_value());
  PLR_EXPECT_ERR(registry->create_location(request.value()), ErrorCode::SessionClosed);
  PLR_EXPECT_ERR(registry->find(facility), ErrorCode::SessionClosed);
  PLR_EXPECT_ERR(registry->encode_current_state(), ErrorCode::SessionClosed);
  PLR_EXPECT_ERR(registry->verify_storage(), ErrorCode::SessionClosed);
  PLR_EXPECT(!registry->closed() == false);
}

PLR_TEST(authority, cancellation_before_publication_publishes_nothing) {
  TempDir dir;
  PLR_EXPECT_OK(registry, Registry::create(dir.path(), writer_options()));
  Fixture fixture(registry, 229);
  PLR_EXPECT_OK(facility, fixture.facility("FAC1"));
  const std::uint64_t before = registry->revision().value();

  std::stop_source source;
  source.request_stop();
  MutationContext context = fixture.context();
  context.stop = source.get_token();
  auto request = CreateLocationRequest::make("loc-room", LocationKind::Room, facility.str(),
                                             "ROOM-1", "", context);
  PLR_REQUIRE(request.has_value());
  PLR_EXPECT_ERR(registry->create_location(request.value()), ErrorCode::Cancelled);
  PLR_EXPECT_EQ(registry->revision().value(), before);
  PLR_EXPECT_EQ(registry->statistics().locations, 1U);

  // The same request without a cancelled token succeeds.
  auto allowed = CreateLocationRequest::make("loc-room", LocationKind::Room, facility.str(),
                                             "ROOM-1", "", fixture.context());
  PLR_REQUIRE(allowed.has_value());
  PLR_EXPECT_OK(receipt, registry->create_location(allowed.value()));
  (void)receipt;
  PLR_EXPECT_EQ(registry->statistics().locations, 2U);
}

PLR_TEST(authority, deterministic_accept_reject_matrix_over_a_seeded_sequence) {
  TempDir dir;
  PLR_EXPECT_OK(registry, Registry::create(dir.path(), writer_options()));
  Fixture fixture(registry, 233);
  PLR_EXPECT_OK(facility, fixture.facility("FAC1"));
  PLR_EXPECT_OK(room, fixture.add(facility, LocationKind::Room, "ROOM-1"));

  // The same sequence of stale and fresh expectations must produce the same
  // verdicts every time, with the same revision progression.
  auto initial = RelabelRequest::make(room.str(), "initial",
                                      fixture.context_with(LocationGeneration(1)));
  PLR_REQUIRE(initial.has_value());
  PLR_EXPECT_OK(initial_receipt, registry->relabel(initial.value()));
  PLR_EXPECT_EQ(initial_receipt.generation.value(), 2U);

  std::vector<std::uint64_t> revisions;
  for (int round = 0; round < 8; ++round) {
    auto stale = RelabelRequest::make(room.str(), "Round " + std::to_string(round),
                                      fixture.context_with(LocationGeneration(1)));
    PLR_REQUIRE(stale.has_value());
    PLR_EXPECT_ERR(registry->relabel(stale.value()), ErrorCode::StaleGeneration);

    PLR_EXPECT_OK(current, registry->find(room));
    auto fresh = RelabelRequest::make(room.str(), "Round " + std::to_string(round),
                                      fixture.context_with(current.generation()));
    PLR_REQUIRE(fresh.has_value());
    PLR_EXPECT_OK(receipt, registry->relabel(fresh.value()));
    revisions.push_back(receipt.revision.value());
  }
  for (std::size_t index = 1; index < revisions.size(); ++index) {
    PLR_EXPECT_EQ(revisions[index], revisions[index - 1] + 1U);
  }
  PLR_EXPECT_OK(final_view, registry->find(room));
  PLR_EXPECT_EQ(final_view.generation().value(), 10U);
}
