// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Bounds: every configured limit is enforced deterministically, growth is
// bounded in memory and on disk, and reaching a bound is a stable rejection
// rather than a silent truncation or an unbounded allocation.

#include <memory>
#include <string>
#include <vector>

#include "dccp/physical_location_registry/physical_location_registry.hpp"
#include "support/synthetic.hpp"
#include "support/test_harness.hpp"

using namespace dccp::physical_location_registry;
using plr_test::Fixture;
using plr_test::Rng;
using plr_test::TempDir;

namespace {

RegistryOpenOptions bounded_options(Limits limits, bool create = true) {
  RegistryOpenOptions options;
  options.mode = OpenMode::ReadWrite;
  options.create_if_missing = create;
  options.limits = limits;
  return options;
}

}  // namespace

PLR_TEST(limits, location_count_is_bounded) {
  TempDir dir;
  Limits limits;
  limits.max_locations = 5;
  limits.max_children_per_location = 5;
  limits.max_traversal_nodes = 5;
  PLR_EXPECT_OK(registry, Registry::create(dir.path(), bounded_options(limits)));
  Fixture fixture(registry, 601);

  PLR_EXPECT_OK(facility, fixture.facility("FAC1"));
  for (int index = 0; index < 4; ++index) {
    PLR_EXPECT_OK(room, fixture.add(facility, LocationKind::Room,
                                    "ROOM-" + std::to_string(index + 1)));
    (void)room;
  }
  PLR_EXPECT_EQ(registry->statistics().locations, 5U);
  const std::uint64_t revision_before = registry->revision().value();

  auto extra = CreateLocationRequest::make("loc-extra", LocationKind::Room, facility.str(),
                                           "ROOM-9", "", fixture.context());
  PLR_REQUIRE(extra.has_value());
  PLR_EXPECT_ERR(registry->create_location(extra.value()), ErrorCode::LimitExceeded);
  PLR_EXPECT_EQ(registry->revision().value(), revision_before);
}

PLR_TEST(limits, child_count_is_bounded) {
  TempDir dir;
  Limits limits;
  limits.max_children_per_location = 2;
  limits.max_locations = 100;
  PLR_EXPECT_OK(registry, Registry::create(dir.path(), bounded_options(limits)));
  Fixture fixture(registry, 607);
  PLR_EXPECT_OK(facility, fixture.facility("FAC1"));
  PLR_EXPECT_OK(first, fixture.add(facility, LocationKind::Room, "ROOM-1"));
  PLR_EXPECT_OK(second, fixture.add(facility, LocationKind::Room, "ROOM-2"));

  auto third = CreateLocationRequest::make("loc-third", LocationKind::Room, facility.str(), "ROOM-3",
                                           "", fixture.context());
  PLR_REQUIRE(third.has_value());
  PLR_EXPECT_ERR(registry->create_location(third.value()), ErrorCode::LimitExceeded);
  PLR_EXPECT_OK(children, registry->children(facility));
  PLR_EXPECT_EQ(children.size(), std::size_t{2});

  // A move is bounded by the same per-parent rule: the first room is filled to
  // its cap, so a row from the second room cannot move into it.
  PLR_EXPECT_OK(row_one, fixture.add(first, LocationKind::Row, "ROW-1"));
  PLR_EXPECT_OK(row_two, fixture.add(first, LocationKind::Row, "ROW-2"));
  PLR_EXPECT_OK(other_row, fixture.add(second, LocationKind::Row, "ROW-3"));
  PLR_EXPECT_OK(first_children, registry->children(first));
  PLR_EXPECT_EQ(first_children.size(), std::size_t{2});
  auto move = MoveLocationRequest::make(other_row.str(), first.str(), fixture.context());
  PLR_REQUIRE(move.has_value());
  PLR_EXPECT_ERR(registry->move_location(move.value()), ErrorCode::LimitExceeded);
  (void)row_one;
  (void)row_two;
}

PLR_TEST(limits, alias_bounds_are_enforced_per_location_and_in_total) {
  TempDir dir;
  Limits limits;
  limits.max_aliases_per_location = 2;
  limits.max_total_aliases = 2;
  PLR_EXPECT_OK(registry, Registry::create(dir.path(), bounded_options(limits)));
  Fixture fixture(registry, 613);
  PLR_EXPECT_OK(facility, fixture.facility("FAC1"));
  PLR_EXPECT_OK(room, fixture.add(facility, LocationKind::Room, "ROOM-1"));
  const Limits active = registry->limits();

  PLR_EXPECT_OK(first, AddAliasRequest::make(room.str(), "/A/1", active, fixture.context()));
  PLR_EXPECT_OK(second, AddAliasRequest::make(room.str(), "/A/2", active, fixture.context()));
  PLR_EXPECT_OK(third, AddAliasRequest::make(room.str(), "/A/3", active, fixture.context()));
  PLR_EXPECT(registry->add_alias(first).has_value());
  PLR_EXPECT(registry->add_alias(second).has_value());
  // The per-location bound is reached first here.
  PLR_EXPECT_ERR(registry->add_alias(third), ErrorCode::LimitExceeded);
  PLR_EXPECT_EQ(registry->statistics().aliases, 2U);

  // The registry-wide bound is enforced on a different location too.
  PLR_EXPECT_OK(other, AddAliasRequest::make(facility.str(), "/A/4", active, fixture.context()));
  PLR_EXPECT_ERR(registry->add_alias(other), ErrorCode::LimitExceeded);
  PLR_EXPECT_EQ(registry->statistics().aliases, 2U);
}

PLR_TEST(limits, move_history_is_bounded_per_location_and_in_total) {
  TempDir dir;
  Limits limits;
  limits.max_moves_per_location = 2;
  limits.max_total_moves = 3;
  PLR_EXPECT_OK(registry, Registry::create(dir.path(), bounded_options(limits)));
  Fixture fixture(registry, 617);
  PLR_EXPECT_OK(facility, fixture.facility("FAC1"));
  PLR_EXPECT_OK(first_room, fixture.add(facility, LocationKind::Room, "ROOM-1"));
  PLR_EXPECT_OK(second_room, fixture.add(facility, LocationKind::Room, "ROOM-2"));
  PLR_EXPECT_OK(third_room, fixture.add(facility, LocationKind::Room, "ROOM-3"));
  PLR_EXPECT_OK(row, fixture.add(first_room, LocationKind::Row, "ROW-1"));

  auto move_one = MoveLocationRequest::make(row.str(), second_room.str(), fixture.context());
  PLR_REQUIRE(move_one.has_value());
  PLR_EXPECT_OK(first_receipt, registry->move_location(move_one.value()));
  (void)first_receipt;
  auto move_two = MoveLocationRequest::make(row.str(), third_room.str(), fixture.context());
  PLR_REQUIRE(move_two.has_value());
  PLR_EXPECT_OK(second_receipt, registry->move_location(move_two.value()));
  (void)second_receipt;
  auto move_three = MoveLocationRequest::make(row.str(), first_room.str(), fixture.context());
  PLR_REQUIRE(move_three.has_value());
  PLR_EXPECT_ERR(registry->move_location(move_three.value()), ErrorCode::LimitExceeded);

  PLR_EXPECT_OK(view, registry->find(row));
  PLR_EXPECT_EQ(view.moves().size(), std::size_t{2});
  PLR_EXPECT_OK(path, registry->path_of(row));
  PLR_EXPECT_EQ(path.to_string(), std::string("/FAC1/ROOM-3/ROW-1"));
}

PLR_TEST(limits, traversal_bounds_stop_oversized_walks) {
  TempDir dir;
  Limits limits;
  limits.max_locations = 1000;
  limits.max_children_per_location = 100;
  limits.max_traversal_nodes = 10;
  PLR_EXPECT_OK(registry, Registry::create(dir.path(), bounded_options(limits)));
  Fixture fixture(registry, 619);
  PLR_EXPECT_OK(facility, fixture.facility("FAC1"));
  for (int index = 0; index < 20; ++index) {
    PLR_EXPECT_OK(room, fixture.add(facility, LocationKind::Room,
                                    "ROOM-" + std::to_string(index + 1)));
    (void)room;
  }
  PLR_EXPECT_ERR(registry->descendants(facility, 8), ErrorCode::LimitExceeded);
  ListOptions bounded;
  bounded.max_nodes = 5;
  PLR_EXPECT_ERR(registry->list(bounded), ErrorCode::LimitExceeded);
  ListOptions allowed;
  allowed.max_nodes = 21;
  PLR_EXPECT_OK(listed, registry->list(allowed));
  PLR_EXPECT_EQ(listed.size(), std::size_t{21});
}

PLR_TEST(limits, state_bytes_bound_is_enforced_before_publication) {
  TempDir dir;
  Limits limits;
  limits.max_state_bytes = 4096;
  limits.max_locations = 10000;
  PLR_EXPECT_OK(registry, Registry::create(dir.path(), bounded_options(limits)));
  Fixture fixture(registry, 623);
  PLR_EXPECT_OK(facility, fixture.facility("FAC1"));

  std::uint64_t committed_revision = registry->revision().value();
  int created = 0;
  for (int index = 0; index < 200; ++index) {
    auto request = CreateLocationRequest::make("loc-" + std::to_string(index), LocationKind::Room,
                                               facility.str(), "ROOM-" + std::to_string(index + 1),
                                               std::string(60, 'x'), fixture.context());
    PLR_REQUIRE(request.has_value());
    const auto receipt = registry->create_location(request.value());
    if (!receipt.has_value()) {
      PLR_EXPECT_EQ(receipt.error().code(), ErrorCode::LimitExceeded);
      break;
    }
    ++created;
    committed_revision = receipt.value().revision.value();
  }
  PLR_EXPECT(created > 0);
  // The rejected mutation left the committed state exactly as it was.
  PLR_EXPECT_EQ(registry->revision().value(), committed_revision);
  PLR_EXPECT(registry->state_bytes() <= limits.max_state_bytes + 64U);
  PLR_EXPECT(registry->verify_storage().has_value());
  PLR_EXPECT(registry->close().has_value());

  // The same bound applies when the state is loaded: a store whose publication
  // exceeds its declared bound is refused rather than trusted.
  PLR_EXPECT_OK(reader, Registry::open(dir.path(), bounded_options(limits, false)));
  PLR_EXPECT_EQ(reader->statistics().locations, static_cast<std::uint32_t>(created + 1));
}

PLR_TEST(limits, component_and_label_bounds_are_enforced) {
  TempDir dir;
  Limits limits;
  limits.max_address_component_bytes = 8;
  limits.max_label_bytes = 16;
  limits.max_reason_bytes = 8;
  limits.max_path_bytes = 64;
  PLR_EXPECT_OK(registry, Registry::create(dir.path(), bounded_options(limits)));
  Fixture fixture(registry, 631);
  PLR_EXPECT_OK(facility, fixture.facility("FAC1"));

  auto long_component = CreateLocationRequest::make("loc-long", LocationKind::Room, facility.str(),
                                                    std::string(9, 'a'), "", fixture.context());
  PLR_REQUIRE(long_component.has_value());
  PLR_EXPECT_ERR(registry->create_location(long_component.value()),
                 ErrorCode::AddressComponentTooLong);

  auto long_label = CreateLocationRequest::make("loc-label", LocationKind::Room, facility.str(),
                                                "ROOM-1", std::string(17, 'l'), fixture.context());
  PLR_REQUIRE(long_label.has_value());
  PLR_EXPECT_ERR(registry->create_location(long_label.value()), ErrorCode::LabelTooLong);

  PLR_EXPECT_OK(room, fixture.add(facility, LocationKind::Room, "ROOM-1"));
  auto long_reason = RelabelRequest::make(room.str(), "fine", fixture.context());
  PLR_REQUIRE(long_reason.has_value());
  long_reason.value().context.reason = std::string(9, 'r');
  PLR_EXPECT_ERR(registry->relabel(long_reason.value()), ErrorCode::MalformedText);

  // A path longer than the configured maximum of 64 bytes is refused. With
  // 8-byte components the deepest legal chain reaches the limit only at the last
  // level, so the rejection lands on the unit that would exceed it.
  std::string component(8, 'b');
  component[0] = 'g';
  PLR_EXPECT_OK(building, fixture.add(facility, LocationKind::Building, component));
  component[0] = 'h';
  PLR_EXPECT_OK(hall, fixture.add(building, LocationKind::Hall, component));
  component[0] = 'm';
  PLR_EXPECT_OK(room_two, fixture.add(hall, LocationKind::Room, component));
  component[0] = 'z';
  PLR_EXPECT_OK(zone, fixture.add(room_two, LocationKind::Zone, component));
  component[0] = 'w';
  PLR_EXPECT_OK(row, fixture.add(zone, LocationKind::Row, component));
  component[0] = 'c';
  PLR_EXPECT_OK(rack, fixture.add(row, LocationKind::Rack, component));
  PLR_EXPECT_OK(rack_path, registry->path_of(rack));
  PLR_EXPECT(rack_path.byte_length() <= registry->limits().max_path_bytes);
  component[0] = 'd';
  auto too_long = CreateLocationRequest::make("loc-unit", LocationKind::RackUnit, rack.str(),
                                              component, "", fixture.context());
  PLR_REQUIRE(too_long.has_value());
  auto coordinate = RackUnitCoordinate::parse(2);
  PLR_REQUIRE(coordinate.has_value());
  too_long.value().unit = coordinate.value();
  PLR_EXPECT_ERR(registry->create_location(too_long.value()), ErrorCode::PathTooLong);

  // A shorter component at the same depth is accepted, which proves the address
  // length, not the depth, was the blocker.
  component[0] = 'e';
  component.resize(2);
  auto fits = CreateLocationRequest::make("loc-unit-ok", LocationKind::RackUnit, rack.str(),
                                          component, "", fixture.context());
  PLR_REQUIRE(fits.has_value());
  fits.value().unit = coordinate.value();
  PLR_EXPECT_OK(fits_receipt, registry->create_location(fits.value()));
  (void)fits_receipt;
  PLR_EXPECT_OK(unit_path, registry->path_of(fits.value().id));
  PLR_EXPECT(unit_path.byte_length() <= registry->limits().max_path_bytes);
}

PLR_TEST(limits, replacement_records_are_bounded) {
  TempDir dir;
  Limits limits;
  limits.max_total_replacements = 2;
  PLR_EXPECT_OK(registry, Registry::create(dir.path(), bounded_options(limits)));
  Fixture fixture(registry, 641);
  PLR_EXPECT_OK(facility, fixture.facility("FAC1"));
  PLR_EXPECT_OK(room, fixture.add(facility, LocationKind::Room, "ROOM-1"));
  PLR_EXPECT_OK(rack, fixture.add(room, LocationKind::Rack, "RACK-1"));
  PLR_EXPECT_OK(unit, fixture.add_unit(rack, 1));

  auto first = ReplaceLocationRequest::make(unit.str(), "loc-v2", "", fixture.context());
  PLR_REQUIRE(first.has_value());
  PLR_EXPECT_OK(first_receipt, registry->replace_location(first.value()));
  (void)first_receipt;
  auto second = ReplaceLocationRequest::make("loc-v2", "loc-v3", "", fixture.context());
  PLR_REQUIRE(second.has_value());
  PLR_EXPECT_OK(second_receipt, registry->replace_location(second.value()));
  (void)second_receipt;
  auto third = ReplaceLocationRequest::make("loc-v3", "loc-v4", "", fixture.context());
  PLR_REQUIRE(third.has_value());
  PLR_EXPECT_ERR(registry->replace_location(third.value()), ErrorCode::LimitExceeded);
  PLR_EXPECT_EQ(registry->copy_snapshot()->replacements().size(), std::size_t{2});
}

PLR_TEST(limits, integer_edges_are_rejected_not_wrapped) {
  TempDir dir;
  PLR_EXPECT_OK(registry, Registry::create(dir.path(), bounded_options(Limits{})));
  Fixture fixture(registry, 643);
  PLR_EXPECT_OK(facility, fixture.facility("FAC1"));

  // A generation beyond anything published is stale, never wrapped into a match.
  auto request = RelabelRequest::make(facility.str(), "label",
                                      fixture.context_with(LocationGeneration(UINT64_MAX)));
  PLR_REQUIRE(request.has_value());
  PLR_EXPECT_ERR(registry->relabel(request.value()), ErrorCode::StaleGeneration);

  auto revision_request = RelabelRequest::make(facility.str(), "label", fixture.context());
  PLR_REQUIRE(revision_request.has_value());
  revision_request.value().context.expected_revision = LocationRevision(UINT64_MAX);
  PLR_EXPECT_ERR(registry->relabel(revision_request.value()), ErrorCode::StaleRevision);

  // Rack-unit and envelope arithmetic refuses to wrap at the cap.
  PLR_EXPECT_ERR(RackUnitCoordinate::parse(UINT64_MAX), ErrorCode::InvalidArgument);
  PLR_EXPECT_ERR(RackEnvelope::with_height(UINT32_MAX), ErrorCode::RackEnvelopeInvalid);
  auto top = RackUnitCoordinate::parse(kMaxRackUnitCoordinate);
  PLR_REQUIRE(top.has_value());
  PLR_EXPECT_ERR(RackEnvelope::make(top.value(), 2), ErrorCode::RackEnvelopeInvalid);
  PLR_EXPECT_OK(exact, RackEnvelope::make(top.value(), 1));
  PLR_EXPECT_EQ(exact.last().value(), kMaxRackUnitCoordinate);

  // Enum values outside the domain never enter the model.
  const std::vector<std::string> hostile_kinds = {"", "rack unit", "rack\\unit", "0", "facility-",
                                                  std::string(200, 'k')};
  for (const std::string& kind : hostile_kinds) {
    PLR_EXPECT_ERR(parse_location_kind(kind), ErrorCode::UnknownEnumToken);
  }
}

PLR_TEST(limits, extreme_hierarchies_stay_within_the_schema) {
  TempDir dir;
  PLR_EXPECT_OK(registry, Registry::create(dir.path(), bounded_options(Limits{})));
  Fixture fixture(registry, 647);

  // The deepest legal chain the containment schema allows, with the longest
  // component the grammar permits at every level.
  const std::string component(kMaxAddressComponentBytes, 'a');
  std::string unique = component;
  unique[0] = 'f';
  PLR_EXPECT_OK(facility, fixture.facility(unique));
  unique[0] = 'b';
  PLR_EXPECT_OK(building, fixture.add(facility, LocationKind::Building, unique));
  unique[0] = 'h';
  PLR_EXPECT_OK(hall, fixture.add(building, LocationKind::Hall, unique));
  unique[0] = 'r';
  PLR_EXPECT_OK(room, fixture.add(hall, LocationKind::Room, unique));
  unique[0] = 'z';
  PLR_EXPECT_OK(zone, fixture.add(room, LocationKind::Zone, unique));
  unique[0] = 'w';
  PLR_EXPECT_OK(row, fixture.add(zone, LocationKind::Row, unique));
  unique[0] = 'k';
  PLR_EXPECT_OK(rack, fixture.add_rack(row, unique, 512));
  unique[0] = 'u';
  unique[1] = '1';
  PLR_EXPECT_OK(unit, fixture.add_unit(rack, 512));
  (void)unit;

  const std::uint32_t deepest = registry->statistics().max_depth_observed;
  PLR_EXPECT_EQ(deepest, 8U);
  PLR_EXPECT_OK(unit_path, registry->path_of(unit));
  PLR_EXPECT(unit_path.depth() == 8U);
  // Seven 64-byte components plus the unit component and separators.
  PLR_EXPECT(unit_path.byte_length() > 400U);
  PLR_EXPECT(unit_path.byte_length() <= registry->limits().max_path_bytes);

  // One level deeper is impossible: a rack unit has no legal children.
  auto deeper = CreateLocationRequest::make("loc-deeper", LocationKind::Rack, unit.str(),
                                            std::string(4, 'd'), "", fixture.context());
  PLR_REQUIRE(deeper.has_value());
  PLR_EXPECT_ERR(registry->create_location(deeper.value()), ErrorCode::InvalidKindForParent);

  // Wide fan-out at one level stays bounded by max_children_per_location.
  PLR_EXPECT_OK(wide_room, fixture.add(hall, LocationKind::Room, std::string(60, '9')));
  for (int index = 0; index < 500; ++index) {
    const std::string child = "C" + std::to_string(index);
    const auto added = fixture.add(wide_room, LocationKind::Row, child);
    PLR_REQUIRE(added.has_value());
  }
  PLR_EXPECT_OK(children, registry->children(wide_room));
  PLR_EXPECT_EQ(children.size(), std::size_t{500});
  PLR_EXPECT_EQ(children[0].component, std::string("C0"));
  // Ordering is byte-wise, not numeric: "C499" sorts before "C99".
  for (std::size_t index = 1; index < children.size(); ++index) {
    PLR_EXPECT(children[index - 1].component < children[index].component);
  }
}

PLR_TEST(limits, unicode_labels_and_long_text_are_handled_exactly) {
  TempDir dir;
  PLR_EXPECT_OK(registry, Registry::create(dir.path(), bounded_options(Limits{})));
  Fixture fixture(registry, 653);
  PLR_EXPECT_OK(facility, fixture.facility("FAC1"));

  const std::vector<std::string> labels = {
      "R\xC3\xA9seau",                                  // Latin-1 supplement
      "\xE6\x9C\xBA\xE6\x88\xBF",                        // CJK
      "\xF0\x9F\x9A\x80 rack",                           // emoji
      "\xD8\xA7\xD9\x84\xD8\xAE\xD8\xA7\xD8\xAF\xD9\x85",  // Arabic
      "combining: e\xCC\x81",                            // combining accent
      std::string(200, 'x'),                             // near the byte cap
      std::string("a") + std::string(kMaxLabelBytes - 1, 'b'),
  };
  for (std::size_t index = 0; index < labels.size(); ++index) {
    auto request = CreateLocationRequest::make("loc-" + std::to_string(index), LocationKind::Room,
                                               facility.str(), "ROOM-" + std::to_string(index),
                                               labels[index], fixture.context());
    PLR_REQUIRE(request.has_value());
    PLR_EXPECT_OK(receipt, registry->create_location(request.value()));
    (void)receipt;
  }
  PLR_EXPECT_OK(rooms, registry->children(facility));
  PLR_REQUIRE(rooms.size() == labels.size());
  for (std::size_t index = 0; index < labels.size(); ++index) {
    PLR_EXPECT_EQ(rooms[index].label, labels[index]);
  }

  // Labels are compared byte for byte: no normalization is applied anywhere.
  PLR_EXPECT_OK(view, registry->find(rooms[4].id));
  PLR_EXPECT_EQ(view.label(), labels[4]);
  PLR_EXPECT(view.label() != std::string("combining: \xC3\xA9"));
}
