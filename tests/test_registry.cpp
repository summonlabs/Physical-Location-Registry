// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Registry creation, containment schema, address resolution, deterministic
// listings and the structural rejections the address space depends on.

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

Result<MutationReceipt> create_room(Registry& registry, const Fixture& fixture,
                                    std::string_view id, std::string_view parent,
                                    std::string_view component) {
  auto request = CreateLocationRequest::make(id, LocationKind::Room, parent, component, "",
                                             fixture.context());
  if (!request.has_value()) {
    return request.error();
  }
  return registry.create_location(request.value());
}

}  // namespace

PLR_TEST(registry, creation_builds_a_canonical_hierarchy) {
  TempDir dir;
  PLR_EXPECT_OK(registry, Registry::create(dir.path(), writer_options()));
  Fixture fixture(registry, 7);

  PLR_EXPECT_OK(facility, fixture.facility("FAC1", "Facility one"));
  PLR_EXPECT_OK(building, fixture.add(facility, LocationKind::Building, "BLDG-A", "Building A"));
  PLR_EXPECT_OK(room, fixture.add(building, LocationKind::Room, "ROOM-101", "Room 101"));
  PLR_EXPECT_OK(row, fixture.add(room, LocationKind::Row, "ROW-3", "Row 3"));
  PLR_EXPECT_OK(rack, fixture.add_rack(row, "RACK-07", 48, "Rack 07"));
  PLR_EXPECT_OK(unit, fixture.add_unit(rack, 12));

  PLR_EXPECT_OK(rack_path, registry->path_of(rack));
  PLR_EXPECT_EQ(rack_path.to_string(), std::string("/FAC1/BLDG-A/ROOM-101/ROW-3/RACK-07"));
  PLR_EXPECT_OK(unit_path, registry->path_of(unit));
  PLR_EXPECT_EQ(unit_path.to_string(),
                std::string("/FAC1/BLDG-A/ROOM-101/ROW-3/RACK-07/U12"));

  PLR_EXPECT_OK(view, registry->find(unit));
  PLR_EXPECT_EQ(view.kind(), LocationKind::RackUnit);
  PLR_EXPECT_EQ(view.generation().value(), 1U);
  PLR_EXPECT(view.is_current());
  PLR_EXPECT(view.unit().has_value());
  PLR_EXPECT_EQ(view.unit()->value(), 12U);
  PLR_EXPECT_EQ(view.component().value(), std::string_view("U12"));
  PLR_EXPECT(view.provenance().created_by == fixture.actor());
  PLR_EXPECT_EQ(view.provenance().source, std::string("synthetic-fixture"));
  PLR_EXPECT(view.provenance().created_revision.value() > 0U);
  PLR_EXPECT(view.moves().empty());
  PLR_EXPECT(view.aliases().empty());
  const std::string summary = view.summary();
  PLR_EXPECT(summary.find("kind=rack-unit") != std::string::npos);
  PLR_EXPECT(summary.find("generation=1") != std::string::npos);

  PLR_EXPECT_EQ(registry->revision().value(), 6U);
  PLR_EXPECT_EQ(registry->statistics().locations, 6U);
  PLR_EXPECT_EQ(registry->statistics().roots, 1U);
  PLR_EXPECT_EQ(registry->statistics().max_depth_observed, 6U);
  PLR_EXPECT_EQ(registry->statistics().by_kind[static_cast<std::size_t>(LocationKind::Rack)], 1U);
  PLR_EXPECT_EQ(registry->statistics().by_kind[static_cast<std::size_t>(LocationKind::Room)], 1U);
  PLR_EXPECT(registry->is_writer());
  PLR_REQUIRE(registry->authority().has_value());
  PLR_EXPECT(registry->authority()->store_id == registry->store_id());
  PLR_EXPECT_EQ(registry->authority()->writer_epoch.value(), registry->epoch().value());
  PLR_EXPECT(!registry->state_digest().empty());
  PLR_EXPECT(registry->state_bytes() > 0U);
  PLR_EXPECT_EQ(registry->state_file_name(), std::string("state.7.plr"));
  PLR_EXPECT(!format_statistics(registry->statistics()).empty());
}

PLR_TEST(registry, duplicate_identity_is_rejected) {
  TempDir dir;
  PLR_EXPECT_OK(registry, Registry::create(dir.path(), writer_options()));
  Fixture fixture(registry, 11);

  PLR_EXPECT_OK(facility, fixture.facility("FAC1"));
  auto first = CreateLocationRequest::make("loc-same", LocationKind::Room, facility.str(), "ROOM-1",
                                           "", fixture.context());
  PLR_REQUIRE(first.has_value());
  PLR_EXPECT(registry->create_location(first.value()).has_value());

  auto again = CreateLocationRequest::make("loc-same", LocationKind::Room, facility.str(), "ROOM-2",
                                           "", fixture.context());
  PLR_REQUIRE(again.has_value());
  PLR_EXPECT_ERR(registry->create_location(again.value()), ErrorCode::AlreadyPresent);

  // The state is untouched by the rejection.
  PLR_EXPECT_EQ(registry->revision().value(), 2U);
  PLR_EXPECT_EQ(registry->statistics().locations, 2U);
}

PLR_TEST(registry, containment_schema_is_enforced_on_create) {
  TempDir dir;
  PLR_EXPECT_OK(registry, Registry::create(dir.path(), writer_options()));
  Fixture fixture(registry, 13);

  PLR_EXPECT_OK(facility, fixture.facility("FAC1"));

  auto contained_facility = CreateLocationRequest::make("loc-fac2", LocationKind::Facility,
                                                        facility.str(), "FAC2", "",
                                                        fixture.context());
  PLR_REQUIRE(contained_facility.has_value());
  PLR_EXPECT_ERR(registry->create_location(contained_facility.value()),
                 ErrorCode::KindMustNotHaveParent);

  auto orphan = CreateLocationRequest::make("loc-orphan", LocationKind::Room, "", "ROOM-X", "",
                                            fixture.context());
  PLR_REQUIRE(orphan.has_value());
  PLR_EXPECT_ERR(registry->create_location(orphan.value()), ErrorCode::KindMustHaveParent);

  auto rack_under_facility = CreateLocationRequest::make(
      "loc-rack1", LocationKind::Rack, facility.str(), "RACK-1", "", fixture.context());
  PLR_REQUIRE(rack_under_facility.has_value());
  PLR_EXPECT_ERR(registry->create_location(rack_under_facility.value()),
                 ErrorCode::InvalidKindForParent);

  PLR_EXPECT_OK(room, fixture.add(facility, LocationKind::Room, "ROOM-1"));
  auto unit_under_room = CreateLocationRequest::make("loc-unit1", LocationKind::RackUnit, room.str(),
                                                     "U1", "", fixture.context());
  PLR_REQUIRE(unit_under_room.has_value());
  PLR_EXPECT_ERR(registry->create_location(unit_under_room.value()),
                 ErrorCode::InvalidKindForParent);

  auto missing_parent = CreateLocationRequest::make("loc-missing", LocationKind::Room, "loc-nope",
                                                    "ROOM-9", "", fixture.context());
  PLR_REQUIRE(missing_parent.has_value());
  PLR_EXPECT_ERR(registry->create_location(missing_parent.value()), ErrorCode::NotFound);

  PLR_EXPECT_OK(hall, fixture.add(facility, LocationKind::Hall, "HALL-1"));
  PLR_EXPECT_OK(cage, fixture.add(room, LocationKind::Cage, "CAGE-1"));
  PLR_EXPECT_OK(rack, fixture.add_rack(cage, "RACK-2", 42));
  PLR_EXPECT_OK(zone, fixture.add(hall, LocationKind::Zone, "ZONE-1"));
  PLR_EXPECT_OK(zoned_rack, fixture.add_rack(zone, "RACK-3", 42));
  PLR_EXPECT_OK(zone_path, registry->path_of(zoned_rack));
  PLR_EXPECT_EQ(zone_path.to_string(), std::string("/FAC1/HALL-1/ZONE-1/RACK-3"));
  PLR_EXPECT_OK(rack_path, registry->path_of(rack));
  PLR_EXPECT_EQ(rack_path.to_string(), std::string("/FAC1/ROOM-1/CAGE-1/RACK-2"));
}

PLR_TEST(registry, sibling_addresses_must_not_collide_or_look_alike) {
  TempDir dir;
  PLR_EXPECT_OK(registry, Registry::create(dir.path(), writer_options()));
  Fixture fixture(registry, 17);

  PLR_EXPECT_OK(facility, fixture.facility("FAC1"));
  PLR_EXPECT_OK(room, fixture.add(facility, LocationKind::Room, "ROOM-1"));

  auto exact = CreateLocationRequest::make("loc-a", LocationKind::Row, room.str(), "ROW-1", "",
                                           fixture.context());
  PLR_REQUIRE(exact.has_value());
  PLR_EXPECT(registry->create_location(exact.value()).has_value());

  auto duplicate = CreateLocationRequest::make("loc-b", LocationKind::Row, room.str(), "ROW-1", "",
                                               fixture.context());
  PLR_REQUIRE(duplicate.has_value());
  PLR_EXPECT_ERR(registry->create_location(duplicate.value()), ErrorCode::AddressInUse);

  auto look_alike = CreateLocationRequest::make("loc-c", LocationKind::Row, room.str(), "row-1", "",
                                                fixture.context());
  PLR_REQUIRE(look_alike.has_value());
  PLR_EXPECT_ERR(registry->create_location(look_alike.value()), ErrorCode::AddressLookAlike);

  // A sibling facility component must be unique and look-alike free too.
  auto second_facility = CreateLocationRequest::make("loc-fac2", LocationKind::Facility, "", "fac1",
                                                     "", fixture.context());
  PLR_REQUIRE(second_facility.has_value());
  PLR_EXPECT_ERR(registry->create_location(second_facility.value()), ErrorCode::AddressLookAlike);

  PLR_EXPECT_ERR(registry->find(LocationId()), ErrorCode::NotFound);

  // The same component under a different parent is a different address.
  PLR_EXPECT_OK(other_room, fixture.add(facility, LocationKind::Room, "ROOM-2"));
  PLR_EXPECT_OK(other_row, fixture.add(other_room, LocationKind::Row, "ROW-1"));
  PLR_EXPECT_OK(other_path, registry->path_of(other_row));
  PLR_EXPECT_EQ(other_path.to_string(), std::string("/FAC1/ROOM-2/ROW-1"));
}

PLR_TEST(registry, rack_geometry_rules) {
  TempDir dir;
  PLR_EXPECT_OK(registry, Registry::create(dir.path(), writer_options()));
  Fixture fixture(registry, 19);

  PLR_EXPECT_OK(facility, fixture.facility("FAC1"));
  PLR_EXPECT_OK(room, fixture.add(facility, LocationKind::Room, "ROOM-1"));
  PLR_EXPECT_OK(rack, fixture.add_rack(room, "RACK-1", 8));

  MutationContext context = fixture.context();
  auto outside = CreateLocationRequest::make("loc-u9", LocationKind::RackUnit, rack.str(), "U9", "",
                                             context);
  PLR_REQUIRE(outside.has_value());
  auto coordinate = RackUnitCoordinate::parse(9);
  PLR_REQUIRE(coordinate.has_value());
  outside.value().unit = coordinate.value();
  PLR_EXPECT_ERR(registry->create_location(outside.value()), ErrorCode::RackUnitOutOfEnvelope);

  auto no_coordinate = CreateLocationRequest::make("loc-u0", LocationKind::RackUnit, rack.str(),
                                                   "U1", "", fixture.context());
  PLR_REQUIRE(no_coordinate.has_value());
  PLR_EXPECT_ERR(registry->create_location(no_coordinate.value()), ErrorCode::InvalidArgument);

  PLR_EXPECT(fixture.add_unit(rack, 3).has_value());
  auto taken = CreateLocationRequest::make("loc-u3b", LocationKind::RackUnit, rack.str(), "U3-B", "",
                                           fixture.context());
  PLR_REQUIRE(taken.has_value());
  auto third = RackUnitCoordinate::parse(3);
  PLR_REQUIRE(third.has_value());
  taken.value().unit = third.value();
  PLR_EXPECT_ERR(registry->create_location(taken.value()), ErrorCode::RackUnitConflict);

  auto room_envelope = CreateLocationRequest::make("loc-roe", LocationKind::Room, facility.str(),
                                                   "ROOM-9", "", fixture.context());
  PLR_REQUIRE(room_envelope.has_value());
  auto envelope = RackEnvelope::with_height(48);
  PLR_REQUIRE(envelope.has_value());
  room_envelope.value().envelope = envelope.value();
  PLR_EXPECT_ERR(registry->create_location(room_envelope.value()), ErrorCode::RackEnvelopeInvalid);

  PLR_EXPECT_OK(row, fixture.add(room, LocationKind::Row, "ROW-1"));
  auto unit_under_row = CreateLocationRequest::make("loc-ur", LocationKind::RackUnit, row.str(),
                                                    "U1", "", fixture.context());
  PLR_REQUIRE(unit_under_row.has_value());
  auto first = RackUnitCoordinate::parse(1);
  PLR_REQUIRE(first.has_value());
  unit_under_row.value().unit = first.value();
  PLR_EXPECT_ERR(registry->create_location(unit_under_row.value()),
                 ErrorCode::InvalidKindForParent);

  auto row_with_unit = CreateLocationRequest::make("loc-row-unit", LocationKind::Row, room.str(),
                                                   "ROW-2", "", fixture.context());
  PLR_REQUIRE(row_with_unit.has_value());
  row_with_unit.value().unit = first.value();
  PLR_EXPECT_ERR(registry->create_location(row_with_unit.value()), ErrorCode::RackUnitNotAllowed);
}

PLR_TEST(registry, resolution_by_address_and_alias) {
  TempDir dir;
  PLR_EXPECT_OK(registry, Registry::create(dir.path(), writer_options()));
  Fixture fixture(registry, 23);

  PLR_EXPECT_OK(facility, fixture.facility("FAC1"));
  PLR_EXPECT_OK(room, fixture.add(facility, LocationKind::Room, "ROOM-101"));
  PLR_EXPECT_OK(row, fixture.add(room, LocationKind::Row, "ROW-3"));

  const Limits limits = registry->limits();
  PLR_EXPECT_OK(address, LocationPath::parse("/FAC1/ROOM-101/ROW-3", limits));
  PLR_EXPECT_OK(resolved, registry->resolve(address));
  PLR_EXPECT(resolved.id == row);
  PLR_EXPECT_EQ(resolved.kind, ResolutionKind::CanonicalAddress);
  PLR_EXPECT_EQ(resolved.canonical_path.to_string(), std::string("/FAC1/ROOM-101/ROW-3"));
  PLR_EXPECT_EQ(resolved.generation.value(), 1U);
  PLR_EXPECT_EQ(resolution_kind_name(resolved.kind), std::string_view("canonical-address"));

  PLR_EXPECT_OK(missing, LocationPath::parse("/FAC1/ROOM-999", limits));
  PLR_EXPECT_ERR(registry->resolve(missing), ErrorCode::NotFound);

  PLR_EXPECT_OK(deeper, LocationPath::parse("/FAC1/ROOM-101/ROW-3/U1", limits));
  PLR_EXPECT_ERR(registry->resolve(deeper), ErrorCode::NotFound);

  PLR_EXPECT_OK(other_facility, LocationPath::parse("/FAC2/ROOM-101", limits));
  PLR_EXPECT_ERR(registry->resolve(other_facility), ErrorCode::NotFound);

  PLR_EXPECT_OK(empty_path, LocationPath::parse("/", limits));
  PLR_EXPECT_ERR(registry->resolve(empty_path), ErrorCode::NotFound);

  PLR_EXPECT_OK(legacy, LocationPath::parse("/SITE-1/ROOM-101/ROW-3", limits));
  auto alias_request = AddAliasRequest::make(row.str(), legacy.to_string(), limits,
                                             fixture.context());
  PLR_REQUIRE(alias_request.has_value());
  PLR_EXPECT_OK(alias_receipt, registry->add_alias(alias_request.value()));
  PLR_EXPECT_EQ(alias_receipt.generation.value(), 2U);

  PLR_EXPECT_OK(alias_resolution, registry->resolve(legacy));
  PLR_EXPECT(alias_resolution.id == row);
  PLR_EXPECT_EQ(alias_resolution.kind, ResolutionKind::Alias);
  PLR_EXPECT_EQ(alias_resolution.canonical_path.to_string(), std::string("/FAC1/ROOM-101/ROW-3"));

  PLR_EXPECT_OK(binding, registry->aliases());
  PLR_REQUIRE(binding.size() == 1);
  PLR_EXPECT_EQ(binding[0].alias, std::string("/SITE-1/ROOM-101/ROW-3"));
  PLR_EXPECT(binding[0].id == row);

  PLR_EXPECT_OK(view, registry->find(row));
  PLR_REQUIRE(view.aliases().size() == 1);
  PLR_EXPECT_EQ(view.aliases()[0], std::string("/SITE-1/ROOM-101/ROW-3"));
}

PLR_TEST(registry, child_and_descendant_listings_are_deterministic) {
  TempDir dir;
  PLR_EXPECT_OK(registry, Registry::create(dir.path(), writer_options()));
  Fixture fixture(registry, 29);

  PLR_EXPECT_OK(facility, fixture.facility("FAC1"));
  PLR_EXPECT_OK(room, fixture.add(facility, LocationKind::Room, "ROOM-1"));
  // Byte order, not numeric order: "ROW-10" sorts before "ROW-2".
  PLR_EXPECT_OK(row_ten, fixture.add(room, LocationKind::Row, "ROW-10"));
  PLR_EXPECT_OK(row_two, fixture.add(room, LocationKind::Row, "ROW-2"));
  PLR_EXPECT_OK(row_one, fixture.add(room, LocationKind::Row, "ROW-1"));
  PLR_EXPECT_OK(rack, fixture.add_rack(row_two, "RACK-1", 4));
  PLR_EXPECT_OK(unit, fixture.add_unit(rack, 2));

  PLR_EXPECT_OK(children, registry->children(room));
  PLR_REQUIRE(children.size() == 3);
  PLR_EXPECT_EQ(children[0].component, std::string("ROW-1"));
  PLR_EXPECT_EQ(children[1].component, std::string("ROW-10"));
  PLR_EXPECT_EQ(children[2].component, std::string("ROW-2"));
  PLR_EXPECT_EQ(children[0].path, std::string("/FAC1/ROOM-1/ROW-1"));
  PLR_EXPECT(children[0].id == row_one);
  PLR_EXPECT(children[0].lifecycle == LifecycleState::Active);
  PLR_EXPECT_EQ(children[0].generation.value(), 1U);
  PLR_EXPECT(!format_child_entry(children[0]).empty());

  PLR_EXPECT_OK(again, registry->children(room));
  PLR_REQUIRE(again.size() == children.size());
  for (std::size_t index = 0; index < children.size(); ++index) {
    PLR_EXPECT(again[index].id == children[index].id);
  }

  PLR_EXPECT_OK(deep, registry->descendants(room, 8));
  PLR_REQUIRE(deep.size() == 5);
  PLR_EXPECT(deep[0].id == row_one);
  PLR_EXPECT(deep[deep.size() - 1].id == unit);

  PLR_EXPECT_OK(one_level, registry->descendants(room, 1));
  PLR_EXPECT_EQ(one_level.size(), std::size_t{3});

  PLR_EXPECT_OK(roots, registry->roots());
  PLR_REQUIRE(roots.size() == 1);
  PLR_EXPECT(roots[0].id == facility);
  (void)row_ten;

  PLR_EXPECT_OK(all, registry->list(ListOptions{}));
  PLR_EXPECT_EQ(all.size(), std::size_t{7});
  PLR_EXPECT_EQ(all[0].path().to_string(), std::string("/FAC1"));
  PLR_EXPECT_EQ(all[all.size() - 1].path().to_string(),
                std::string("/FAC1/ROOM-1/ROW-2/RACK-1/U2"));

  ListOptions racks_only;
  racks_only.kind = LocationKind::Rack;
  PLR_EXPECT_OK(racks, registry->list(racks_only));
  PLR_EXPECT_EQ(racks.size(), std::size_t{1});
  PLR_EXPECT(racks[0].id() == rack);

  ListOptions subtree;
  subtree.root = row_two;
  PLR_EXPECT_OK(subtree_list, registry->list(subtree));
  PLR_EXPECT_EQ(subtree_list.size(), std::size_t{3});

  ListOptions bounded;
  bounded.max_nodes = 2;
  PLR_EXPECT_ERR(registry->list(bounded), ErrorCode::LimitExceeded);

  PLR_EXPECT_ERR(registry->children(LocationId()), ErrorCode::NotFound);
  PLR_EXPECT_ERR(registry->path_of(LocationId()), ErrorCode::NotFound);
  PLR_EXPECT_ERR(registry->find(LocationId()), ErrorCode::NotFound);
  PLR_EXPECT_ERR(registry->descendants(LocationId(), 3), ErrorCode::NotFound);
}

PLR_TEST(registry, repeated_open_reads_the_committed_state) {
  TempDir dir;
  StoreId store_id;
  std::string state_digest;
  {
    PLR_EXPECT_OK(registry, Registry::create(dir.path(), writer_options()));
    store_id = registry->store_id();
    Fixture fixture(registry, 31);
    PLR_EXPECT_OK(facility, fixture.facility("FAC1"));
    PLR_EXPECT(fixture.add(facility, LocationKind::Room, "ROOM-1").has_value());
    PLR_EXPECT_EQ(registry->revision().value(), 2U);
    state_digest = registry->state_digest();
    PLR_EXPECT(registry->close().has_value());
    PLR_EXPECT(registry->closed());
  }
  {
    PLR_EXPECT_OK(reopened, Registry::open(dir.path(), reader_options()));
    PLR_EXPECT(reopened->store_id() == store_id);
    PLR_EXPECT_EQ(reopened->revision().value(), 2U);
    PLR_EXPECT_EQ(reopened->statistics().locations, 2U);
    PLR_EXPECT(!reopened->is_writer());
    PLR_EXPECT(!reopened->authority().has_value());
    PLR_EXPECT_EQ(reopened->state_digest(), state_digest);
    PLR_EXPECT(reopened->verify_storage().has_value());
    PLR_EXPECT_OK(root, reopened->roots());
    PLR_EXPECT_EQ(root.size(), std::size_t{1});
    PLR_EXPECT_ERR(reopened->find(LocationId()), ErrorCode::NotFound);
    PLR_EXPECT_OK(state_bytes, reopened->encode_current_state());
    PLR_EXPECT_EQ(state_bytes.size(), static_cast<std::size_t>(reopened->state_bytes()));
  }
}

PLR_TEST(registry, read_only_sessions_cannot_mutate) {
  TempDir dir;
  PLR_EXPECT_OK(registry, Registry::create(dir.path(), writer_options()));
  Fixture fixture(registry, 37);
  PLR_EXPECT_OK(facility, fixture.facility("FAC1"));
  PLR_EXPECT(registry->close().has_value());

  PLR_EXPECT_OK(reader, Registry::open(dir.path(), reader_options()));
  auto request = CreateLocationRequest::make("loc-x", LocationKind::Room, facility.str(), "ROOM-X",
                                             "", fixture.context());
  PLR_REQUIRE(request.has_value());
  PLR_EXPECT_ERR(reader->create_location(request.value()), ErrorCode::SessionReadOnly);
  PLR_EXPECT(reader->close().has_value());
  PLR_EXPECT(reader->closed());
  PLR_EXPECT_ERR(reader->find(facility), ErrorCode::SessionClosed);
  }

PLR_TEST(registry, explanations_describe_acceptance_and_rejection) {
  TempDir dir;
  PLR_EXPECT_OK(registry, Registry::create(dir.path(), writer_options()));
  Fixture fixture(registry, 41);
  PLR_EXPECT_OK(facility, fixture.facility("FAC1"));
  PLR_EXPECT_OK(room, fixture.add(facility, LocationKind::Room, "ROOM-1"));
  PLR_EXPECT_OK(row, fixture.add(room, LocationKind::Row, "ROW-1"));

  auto good = CreateLocationRequest::make("loc-new", LocationKind::Row, room.str(), "ROW-2", "",
                                          fixture.context());
  PLR_REQUIRE(good.has_value());
  const Explanation accepted = registry->explain_create(good.value());
  PLR_EXPECT(accepted.ok());
  PLR_EXPECT(accepted.summary.find("would be accepted") != std::string::npos);
  PLR_REQUIRE(!accepted.details.empty());
  PLR_EXPECT_EQ(accepted.details[0], std::string("address: /FAC1/ROOM-1/ROW-2"));

  auto bad = CreateLocationRequest::make("loc-bad", LocationKind::Row, room.str(), "ROW-1", "",
                                         fixture.context());
  PLR_REQUIRE(bad.has_value());
  const Explanation rejected = registry->explain_create(bad.value());
  PLR_EXPECT(!rejected.ok());
  PLR_EXPECT_EQ(rejected.code, ErrorCode::AddressInUse);
  PLR_EXPECT(!rejected.hints.empty());
  PLR_EXPECT(rejected.to_string().find("hint:") != std::string::npos);

  auto illegal_move = MoveLocationRequest::make(row.str(), facility.str(), fixture.context());
  PLR_REQUIRE(illegal_move.has_value());
  const Explanation move_rejected = registry->explain_move(illegal_move.value());
  PLR_EXPECT(!move_rejected.ok());
  PLR_EXPECT_EQ(move_rejected.code, ErrorCode::InvalidKindForParent);

  PLR_EXPECT_OK(hall, fixture.add(facility, LocationKind::Hall, "HALL-1"));
  auto allowed = MoveLocationRequest::make(row.str(), hall.str(), fixture.context());
  PLR_REQUIRE(allowed.has_value());
  const Explanation move_ok = registry->explain_move(allowed.value());
  PLR_EXPECT(move_ok.ok());
  PLR_REQUIRE(!move_ok.details.empty());
  PLR_EXPECT(move_ok.details[0].find("/FAC1/HALL-1/ROW-1") != std::string::npos);

  const Limits limits = registry->limits();
  PLR_EXPECT_OK(unknown, LocationPath::parse("/FAC1/NOPE", limits));
  const Explanation unresolved = registry->explain_resolve(unknown);
  PLR_EXPECT(!unresolved.ok());
  PLR_EXPECT_EQ(unresolved.code, ErrorCode::NotFound);
  PLR_EXPECT(unresolved.to_string().find("NOT_FOUND") != std::string::npos);

  PLR_EXPECT_OK(known, LocationPath::parse("/FAC1/ROOM-1/ROW-1", limits));
  const Explanation resolved = registry->explain_resolve(known);
  PLR_EXPECT(resolved.ok());
  PLR_EXPECT(resolved.summary.find("resolves to") != std::string::npos);
  PLR_EXPECT_EQ(resolved.details[1], std::string("lifecycle: active"));
}

PLR_TEST(registry, creating_over_an_existing_store_is_rejected) {
  TempDir dir;
  PLR_EXPECT_OK(registry, Registry::create(dir.path(), writer_options()));
  PLR_EXPECT(registry->close().has_value());
  PLR_EXPECT_ERR(Registry::create(dir.path(), writer_options()), ErrorCode::StoreExists);

  TempDir empty;
  PLR_EXPECT_ERR(Registry::open(empty.path(), reader_options()), ErrorCode::StoreNotFound);
  PLR_EXPECT_ERR(Registry::open(empty.path() / "missing", reader_options()),
                 ErrorCode::StoreNotFound);

  RegistryOpenOptions mismatched = reader_options();
  Limits other;
  other.max_locations = 10;
  mismatched.limits = other;
  PLR_EXPECT_ERR(Registry::open(dir.path(), mismatched), ErrorCode::LimitsMismatch);
}

PLR_TEST(registry, custom_limits_are_enforced_from_creation) {
  TempDir dir;
  RegistryOpenOptions options = writer_options();
  Limits limits;
  limits.max_locations = 6;
  limits.max_depth = 3;
  limits.max_children_per_location = 2;
  limits.max_traversal_nodes = 6;
  options.limits = limits;
  PLR_EXPECT_OK(registry, Registry::create(dir.path(), options));
  PLR_EXPECT(registry->limits() == limits);
  Fixture fixture(registry, 43);

  PLR_EXPECT_OK(facility, fixture.facility("FAC1"));
  PLR_EXPECT_OK(room, fixture.add(facility, LocationKind::Room, "ROOM-1"));
  PLR_EXPECT_OK(row, fixture.add(room, LocationKind::Row, "ROW-1"));
  // Depth 4 would exceed max_depth = 3.
  auto too_deep = CreateLocationRequest::make("loc-deep", LocationKind::Rack, row.str(), "RACK-1",
                                              "", fixture.context());
  PLR_REQUIRE(too_deep.has_value());
  PLR_EXPECT_ERR(registry->create_location(too_deep.value()), ErrorCode::PathTooDeep);

  // A second child of the facility is legal; a third exceeds the per-parent cap.
  PLR_EXPECT_OK(second_room, fixture.add(facility, LocationKind::Room, "ROOM-2"));
  auto third_room = CreateLocationRequest::make("loc-room3", LocationKind::Room, facility.str(),
                                                "ROOM-3", "", fixture.context());
  PLR_REQUIRE(third_room.has_value());
  PLR_EXPECT_ERR(registry->create_location(third_room.value()), ErrorCode::LimitExceeded);

  // A listing bound is reported, never applied silently.
  ListOptions bounded;
  bounded.max_nodes = 2;
  PLR_EXPECT_ERR(registry->list(bounded), ErrorCode::LimitExceeded);
  ListOptions generous;
  generous.max_nodes = 6;
  PLR_EXPECT_OK(listed, registry->list(generous));
  PLR_EXPECT_EQ(listed.size(), std::size_t{4});
  (void)second_room;
}
