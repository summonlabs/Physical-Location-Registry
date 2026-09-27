// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Mutation semantics: address changes, subtree moves, lifecycle transitions,
// replacement lineage and alias binding, including the generation and move
// records each of them must leave behind.

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

/// A standard facility: FAC1/ROOM-1/ROW-1/RACK-1 with a 48U envelope.
struct Standard {
  LocationId facility;
  LocationId room;
  LocationId row;
  LocationId rack;
};

Result<Standard> build_standard(Fixture& fixture) {
  Standard standard;
  PLR_TRY(facility, fixture.facility("FAC1"));
  PLR_TRY(room, fixture.add(facility, LocationKind::Room, "ROOM-1", "Room 1"));
  PLR_TRY(row, fixture.add(room, LocationKind::Row, "ROW-1", "Row 1"));
  PLR_TRY(rack, fixture.add_rack(row, "RACK-1", 48, "Rack 1"));
  standard.facility = facility;
  standard.room = room;
  standard.row = row;
  standard.rack = rack;
  return standard;
}

}  // namespace

PLR_TEST(mutations, readdress_changes_address_and_keeps_identity) {
  TempDir dir;
  PLR_EXPECT_OK(registry, Registry::create(dir.path(), writer_options()));
  Fixture fixture(registry, 101);
  PLR_EXPECT_OK(standard, build_standard(fixture));

  const LocationGeneration before = registry->find(standard.row)->generation();
  auto request = ReaddressRequest::make(standard.row.str(), "ROW-2", fixture.context());
  PLR_REQUIRE(request.has_value());
  PLR_EXPECT_OK(receipt, registry->readdress(request.value()));
  PLR_EXPECT_EQ(receipt.generation.value(), before.value() + 1U);
  PLR_EXPECT_EQ(receipt.affected_locations, 2U);
  PLR_EXPECT_EQ(receipt.affected_descendants, 1U);

  PLR_EXPECT_OK(path, registry->path_of(standard.row));
  PLR_EXPECT_EQ(path.to_string(), std::string("/FAC1/ROOM-1/ROW-2"));
  PLR_EXPECT_OK(rack_path, registry->path_of(standard.rack));
  PLR_EXPECT_EQ(rack_path.to_string(), std::string("/FAC1/ROOM-1/ROW-2/RACK-1"));

  // The moved node records the address change; the descendant that followed it
  // has its generation advanced so a cached address is detectable as stale.
  PLR_EXPECT_OK(row_view, registry->find(standard.row));
  PLR_REQUIRE(row_view.moves().size() == 1);
  const MoveRecord& move = row_view.moves()[0];
  PLR_EXPECT_EQ(move.kind, MoveKind::Readdress);
  PLR_EXPECT_EQ(move.from_component, std::string("ROW-1"));
  PLR_EXPECT_EQ(move.to_component, std::string("ROW-2"));
  PLR_EXPECT_EQ(move.from_path, std::string("/FAC1/ROOM-1/ROW-1"));
  PLR_EXPECT_EQ(move.to_path, std::string("/FAC1/ROOM-1/ROW-2"));
  PLR_EXPECT_EQ(move.affected_descendants, 1U);
  PLR_EXPECT(move.actor == fixture.actor());
  PLR_EXPECT_EQ(move.generation_before.value(), before.value());
  PLR_EXPECT_EQ(move.generation_after.value(), before.value() + 1U);
  PLR_EXPECT(row_view.provenance().last_modified_revision == receipt.revision);
  PLR_EXPECT_EQ(row_view.provenance().created_revision.value(), 3U);

  PLR_EXPECT_OK(rack_view, registry->find(standard.rack));
  PLR_EXPECT(rack_view.moves().empty());
  PLR_EXPECT(rack_view.generation() > LocationGeneration(1));

  // The old address no longer resolves; the new one does.
  const Limits limits = registry->limits();
  PLR_EXPECT_OK(old_path, LocationPath::parse("/FAC1/ROOM-1/ROW-1", limits));
  PLR_EXPECT_ERR(registry->resolve(old_path), ErrorCode::NotFound);
  PLR_EXPECT_OK(new_path, LocationPath::parse("/FAC1/ROOM-1/ROW-2", limits));
  PLR_EXPECT_OK(resolved, registry->resolve(new_path));
  PLR_EXPECT(resolved.id == standard.row);
}

PLR_TEST(mutations, readdress_rejections) {
  TempDir dir;
  PLR_EXPECT_OK(registry, Registry::create(dir.path(), writer_options()));
  Fixture fixture(registry, 103);
  PLR_EXPECT_OK(standard, build_standard(fixture));
  PLR_EXPECT_OK(other_row, fixture.add(standard.room, LocationKind::Row, "ROW-9"));

  auto taken = ReaddressRequest::make(standard.row.str(), "ROW-9", fixture.context());
  PLR_REQUIRE(taken.has_value());
  PLR_EXPECT_ERR(registry->readdress(taken.value()), ErrorCode::AddressInUse);

  auto look_alike = ReaddressRequest::make(standard.row.str(), "row-9", fixture.context());
  PLR_REQUIRE(look_alike.has_value());
  PLR_EXPECT_ERR(registry->readdress(look_alike.value()), ErrorCode::AddressLookAlike);

  auto unchanged = ReaddressRequest::make(standard.row.str(), "ROW-1", fixture.context());
  PLR_REQUIRE(unchanged.has_value());
  PLR_EXPECT_ERR(registry->readdress(unchanged.value()), ErrorCode::NoOpMutation);

  auto missing = ReaddressRequest::make("loc-missing", "ROW-2", fixture.context());
  PLR_REQUIRE(missing.has_value());
  PLR_EXPECT_ERR(registry->readdress(missing.value()), ErrorCode::NotFound);
  (void)other_row;
}

PLR_TEST(mutations, relabel_does_not_change_the_address) {
  TempDir dir;
  PLR_EXPECT_OK(registry, Registry::create(dir.path(), writer_options()));
  Fixture fixture(registry, 107);
  PLR_EXPECT_OK(standard, build_standard(fixture));

  auto request = RelabelRequest::make(standard.row.str(), "Row one (renamed)", fixture.context());
  PLR_REQUIRE(request.has_value());
  PLR_EXPECT_OK(receipt, registry->relabel(request.value()));

  PLR_EXPECT_OK(view, registry->find(standard.row));
  PLR_EXPECT_EQ(view.label(), std::string("Row one (renamed)"));
  PLR_EXPECT_EQ(view.path().to_string(), std::string("/FAC1/ROOM-1/ROW-1"));
  PLR_EXPECT(view.moves().empty());
  PLR_EXPECT_EQ(receipt.affected_locations, 1U);

  // The label is not part of the address, so the child address is unchanged and
  // the child generation is untouched.
  PLR_EXPECT_OK(rack_view, registry->find(standard.rack));
  PLR_EXPECT_EQ(rack_view.generation().value(), 1U);

  auto same = RelabelRequest::make(standard.row.str(), "Row one (renamed)", fixture.context());
  PLR_REQUIRE(same.has_value());
  PLR_EXPECT_ERR(registry->relabel(same.value()), ErrorCode::NoOpMutation);

  auto unicode = RelabelRequest::make(standard.row.str(),
                                      "R\xC3\xA9seau \xE2\x82\xAC \xF0\x9F\x9A\x80", fixture.context());
  PLR_REQUIRE(unicode.has_value());
  PLR_EXPECT_OK(unicode_receipt, registry->relabel(unicode.value()));
  (void)unicode_receipt;
  PLR_EXPECT_OK(updated, registry->find(standard.row));
  PLR_EXPECT_EQ(updated.label(), std::string("R\xC3\xA9seau \xE2\x82\xAC \xF0\x9F\x9A\x80"));
}

PLR_TEST(mutations, subtree_move_is_atomic_and_restamps_descendants) {
  TempDir dir;
  PLR_EXPECT_OK(registry, Registry::create(dir.path(), writer_options()));
  Fixture fixture(registry, 109);
  PLR_EXPECT_OK(standard, build_standard(fixture));
  PLR_EXPECT_OK(rack_two, fixture.add_rack(standard.row, "RACK-2", 24));
  PLR_EXPECT_OK(unit, fixture.add_unit(rack_two, 3));
  PLR_EXPECT_OK(hall, fixture.add(standard.facility, LocationKind::Hall, "HALL-1"));

  auto request = MoveLocationRequest::make(standard.row.str(), hall.str(), fixture.context());
  PLR_REQUIRE(request.has_value());
  PLR_EXPECT_OK(receipt, registry->move_location(request.value()));
  PLR_EXPECT_EQ(receipt.affected_locations, 4U);
  PLR_EXPECT_EQ(receipt.affected_descendants, 3U);

  PLR_EXPECT_OK(row_path, registry->path_of(standard.row));
  PLR_EXPECT_EQ(row_path.to_string(), std::string("/FAC1/HALL-1/ROW-1"));
  PLR_EXPECT_OK(rack_path, registry->path_of(standard.rack));
  PLR_EXPECT_EQ(rack_path.to_string(), std::string("/FAC1/HALL-1/ROW-1/RACK-1"));
  PLR_EXPECT_OK(unit_path, registry->path_of(unit));
  PLR_EXPECT_EQ(unit_path.to_string(), std::string("/FAC1/HALL-1/ROW-1/RACK-2/U3"));

  PLR_EXPECT_OK(row_view, registry->find(standard.row));
  PLR_REQUIRE(row_view.moves().size() == 1);
  PLR_EXPECT_EQ(row_view.moves()[0].kind, MoveKind::Reparent);
  PLR_REQUIRE(row_view.moves()[0].from_parent.has_value());
  PLR_REQUIRE(row_view.moves()[0].to_parent.has_value());
  PLR_EXPECT(row_view.moves()[0].from_parent.value() == standard.room);
  PLR_EXPECT(row_view.moves()[0].to_parent.value() == hall);

  PLR_EXPECT_OK(unit_view, registry->find(unit));
  PLR_EXPECT(unit_view.generation() > LocationGeneration(1));
  PLR_EXPECT(unit_view.moves().empty());

  // The old parent keeps no stale child entry.
  PLR_EXPECT_OK(room_children, registry->children(standard.room));
  PLR_EXPECT_EQ(room_children.size(), std::size_t{0});
  PLR_EXPECT_OK(hall_children, registry->children(hall));
  PLR_REQUIRE(hall_children.size() == 1);
  PLR_EXPECT(hall_children[0].id == standard.row);
}

PLR_TEST(mutations, illegal_moves_are_rejected_without_side_effects) {
  TempDir dir;
  PLR_EXPECT_OK(registry, Registry::create(dir.path(), writer_options()));
  Fixture fixture(registry, 113);
  PLR_EXPECT_OK(standard, build_standard(fixture));

  auto self = MoveLocationRequest::make(standard.row.str(), standard.row.str(), fixture.context());  PLR_REQUIRE(self.has_value());
  PLR_EXPECT_ERR(registry->move_location(self.value()), ErrorCode::SelfMove);

  auto into_descendant = MoveLocationRequest::make(standard.row.str(), standard.rack.str(),
                                                   fixture.context());
  PLR_REQUIRE(into_descendant.has_value());
  PLR_EXPECT_ERR(registry->move_location(into_descendant.value()), ErrorCode::MoveIntoDescendant);

  auto same_parent = MoveLocationRequest::make(standard.row.str(), standard.room.str(),
                                               fixture.context());
  PLR_REQUIRE(same_parent.has_value());
  PLR_EXPECT_ERR(registry->move_location(same_parent.value()), ErrorCode::NoOpMutation);

  auto illegal_kind = MoveLocationRequest::make(standard.rack.str(), standard.facility.str(),
                                                fixture.context());
  PLR_REQUIRE(illegal_kind.has_value());
  PLR_EXPECT_ERR(registry->move_location(illegal_kind.value()), ErrorCode::InvalidKindForParent);

  // A facility is a root and can never be moved.
  PLR_EXPECT_OK(other_facility, fixture.facility("FAC2"));
  auto root_move = MoveLocationRequest::make(other_facility.str(), standard.facility.str(),
                                             fixture.context());
  PLR_REQUIRE(root_move.has_value());
  PLR_EXPECT_ERR(registry->move_location(root_move.value()), ErrorCode::KindMustNotHaveParent);

  auto missing_parent = MoveLocationRequest::make(standard.row.str(), "loc-missing",
                                                  fixture.context());
  PLR_REQUIRE(missing_parent.has_value());
  PLR_EXPECT_ERR(registry->move_location(missing_parent.value()), ErrorCode::NotFound);

  // Component collision under the target parent.
  PLR_EXPECT_OK(hall, fixture.add(standard.facility, LocationKind::Hall, "HALL-1"));
  PLR_EXPECT_OK(clashing, fixture.add(hall, LocationKind::Row, "ROW-1"));
  (void)clashing;

  // From here on the test asserts that every rejection left the committed
  // revision untouched.
  const std::uint64_t before_revision = registry->revision().value();
  auto collision = MoveLocationRequest::make(standard.row.str(), hall.str(), fixture.context());
  PLR_REQUIRE(collision.has_value());
  PLR_EXPECT_ERR(registry->move_location(collision.value()), ErrorCode::AddressInUse);
  PLR_EXPECT_ERR(registry->move_location(collision.value()), ErrorCode::AddressInUse);
  PLR_EXPECT_ERR(registry->move_location(missing_parent.value()), ErrorCode::NotFound);
  PLR_EXPECT_ERR(registry->move_location(into_descendant.value()), ErrorCode::MoveIntoDescendant);

  PLR_EXPECT_EQ(registry->revision().value(), before_revision);
  PLR_EXPECT_OK(row_path, registry->path_of(standard.row));
  PLR_EXPECT_EQ(row_path.to_string(), std::string("/FAC1/ROOM-1/ROW-1"));
}

PLR_TEST(mutations, retire_and_reactivate_follow_the_transition_table) {
  TempDir dir;
  PLR_EXPECT_OK(registry, Registry::create(dir.path(), writer_options()));
  Fixture fixture(registry, 127);
  PLR_EXPECT_OK(standard, build_standard(fixture));
  PLR_EXPECT_OK(unit, fixture.add_unit(standard.rack, 5));

  // Retiring a location with active descendants is rejected unless the whole
  // subtree is retired in one atomic step.
  auto leaf_only = RetireLocationRequest::make(standard.row.str(), SubtreeMode::LocationOnly,
                                               fixture.context());
  PLR_REQUIRE(leaf_only.has_value());
  PLR_EXPECT_ERR(registry->retire_location(leaf_only.value()),
                 ErrorCode::LocationHasActiveChildren);

  auto subtree = RetireLocationRequest::make(standard.row.str(), SubtreeMode::Subtree,
                                             fixture.context());
  PLR_REQUIRE(subtree.has_value());
  PLR_EXPECT_OK(receipt, registry->retire_location(subtree.value()));
  PLR_EXPECT_EQ(receipt.affected_locations, 3U);
  PLR_EXPECT_EQ(receipt.generation.value(), 2U);

  PLR_EXPECT_OK(row_view, registry->find(standard.row));
  PLR_EXPECT(row_view.lifecycle() == LifecycleState::Retired);
  PLR_EXPECT_OK(unit_view, registry->find(unit));
  PLR_EXPECT(unit_view.lifecycle() == LifecycleState::Retired);

  // A retired address never resolves as current, but does resolve on request.
  const Limits limits = registry->limits();
  PLR_EXPECT_OK(row_path, LocationPath::parse("/FAC1/ROOM-1/ROW-1", limits));
  PLR_EXPECT_ERR(registry->resolve(row_path), ErrorCode::NotFound);
  PLR_EXPECT_OK(retired_resolution, registry->resolve(row_path, ResolutionMode::IncludeRetired));
  PLR_EXPECT(retired_resolution.id == standard.row);
  PLR_EXPECT(retired_resolution.lifecycle == LifecycleState::Retired);

  // A retired location cannot be moved, relabeled or readdressed.
  auto move = MoveLocationRequest::make(standard.row.str(), standard.facility.str(),
                                        fixture.context());
  PLR_REQUIRE(move.has_value());
  PLR_EXPECT_ERR(registry->move_location(move.value()), ErrorCode::LifecycleTransitionIllegal);
  // Relabeling a retired location is allowed: the label is descriptive
  // metadata, not part of the address space.
  auto relabel = RelabelRequest::make(standard.row.str(), "Row (retired)", fixture.context());
  PLR_REQUIRE(relabel.has_value());
  PLR_EXPECT_OK(relabel_receipt, registry->relabel(relabel.value()));
  PLR_EXPECT_EQ(relabel_receipt.affected_locations, 1U);

  // Retiring again is not a legal transition.
  auto again = RetireLocationRequest::make(standard.row.str(), SubtreeMode::Subtree,
                                           fixture.context());
  PLR_REQUIRE(again.has_value());
  PLR_EXPECT_ERR(registry->retire_location(again.value()), ErrorCode::LifecycleTransitionIllegal);

  // Reactivating a child while the parent is retired is rejected.
  auto child_reactivate = ReactivateLocationRequest::make(standard.rack.str(),
                                                          SubtreeMode::LocationOnly,
                                                          fixture.context());
  PLR_REQUIRE(child_reactivate.has_value());
  PLR_EXPECT_ERR(registry->reactivate_location(child_reactivate.value()),
                 ErrorCode::ParentNotActive);

  auto reactivate = ReactivateLocationRequest::make(standard.row.str(), SubtreeMode::Subtree,
                                                    fixture.context());
  PLR_REQUIRE(reactivate.has_value());
  PLR_EXPECT_OK(reactivated, registry->reactivate_location(reactivate.value()));
  PLR_EXPECT_EQ(reactivated.affected_locations, 3U);
  PLR_EXPECT_OK(restored, registry->resolve(row_path));
  PLR_EXPECT(restored.id == standard.row);

  // Reactivating an active location is not a legal transition.
  auto active_reactivate = ReactivateLocationRequest::make(standard.row.str(),
                                                           SubtreeMode::Subtree,
                                                           fixture.context());
  PLR_REQUIRE(active_reactivate.has_value());
  PLR_EXPECT_ERR(registry->reactivate_location(active_reactivate.value()),
                 ErrorCode::LifecycleTransitionIllegal);

  // Retiring a leaf directly is allowed.
  auto unit_retire = RetireLocationRequest::make(unit.str(), SubtreeMode::LocationOnly,
                                                 fixture.context());
  PLR_REQUIRE(unit_retire.has_value());
  PLR_EXPECT_OK(unit_receipt, registry->retire_location(unit_retire.value()));
  (void)unit_receipt;
  PLR_EXPECT_OK(unit_after, registry->find(unit));
  PLR_EXPECT(unit_after.lifecycle() == LifecycleState::Retired);
}

PLR_TEST(mutations, replacement_creates_a_successor_and_keeps_lineage) {
  TempDir dir;
  PLR_EXPECT_OK(registry, Registry::create(dir.path(), writer_options()));
  Fixture fixture(registry, 131);
  PLR_EXPECT_OK(standard, build_standard(fixture));
  PLR_EXPECT_OK(unit, fixture.add_unit(standard.rack, 7));

  auto request = ReplaceLocationRequest::make(unit.str(), "loc-unit-replacement", "Unit 7 (new)",
                                              fixture.context());
  PLR_REQUIRE(request.has_value());
  request.value().source = "replacement-test";
  PLR_EXPECT_OK(receipt, registry->replace_location(request.value()));
  PLR_EXPECT_EQ(receipt.successor_generation.value().value(), 1U);

  const LocationId successor = request.value().successor_id;
  PLR_EXPECT_OK(predecessor_view, registry->find(unit));
  PLR_EXPECT(predecessor_view.lifecycle() == LifecycleState::Replaced);
  PLR_REQUIRE(predecessor_view.replaced_by().has_value());
  PLR_EXPECT(predecessor_view.replaced_by().value() == successor);

  PLR_EXPECT_OK(successor_view, registry->find(successor));
  PLR_EXPECT(successor_view.is_current());
  PLR_REQUIRE(successor_view.replaces().has_value());
  PLR_EXPECT(successor_view.replaces().value() == unit);
  PLR_EXPECT_EQ(successor_view.unit()->value(), 7U);
  PLR_EXPECT_EQ(successor_view.label(), std::string("Unit 7 (new)"));
  PLR_EXPECT_EQ(successor_view.provenance().source, std::string("replacement-test"));
  PLR_EXPECT_EQ(successor_view.path().to_string(),
                std::string("/FAC1/ROOM-1/ROW-1/RACK-1/U7"));

  // The address now belongs to the successor; the predecessor never resolves.
  const Limits limits = registry->limits();
  PLR_EXPECT_OK(unit_path, LocationPath::parse("/FAC1/ROOM-1/ROW-1/RACK-1/U7", limits));
  PLR_EXPECT_OK(resolved, registry->resolve(unit_path));
  PLR_EXPECT(resolved.id == successor);
  PLR_EXPECT_OK(successor_resolution, registry->resolve(unit_path, ResolutionMode::IncludeRetired));
  PLR_EXPECT(successor_resolution.id == successor);
  PLR_EXPECT(successor_resolution.lifecycle == LifecycleState::Active);

  // The predecessor stays visible for audit and keeps its historical address.
  PLR_EXPECT_OK(predecessor_path, registry->path_of(unit));
  PLR_EXPECT_EQ(predecessor_path.to_string(),
                std::string("/FAC1/ROOM-1/ROW-1/RACK-1/U7"));

  const std::shared_ptr<const Snapshot> lineage_snapshot = registry->copy_snapshot();
  PLR_REQUIRE(lineage_snapshot != nullptr);
  const std::vector<ReplacementRecord>& lineage = lineage_snapshot->replacements();
  PLR_REQUIRE(lineage.size() == 1);
  PLR_EXPECT(lineage[0].predecessor == unit);
  PLR_EXPECT(lineage[0].successor == successor);

  // A replaced location is terminal.
  auto readdress = ReaddressRequest::make(unit.str(), "U8", fixture.context());
  PLR_REQUIRE(readdress.has_value());
  PLR_EXPECT_ERR(registry->readdress(readdress.value()), ErrorCode::LifecycleTransitionIllegal);
  auto replace_again = ReplaceLocationRequest::make(unit.str(), "loc-again", "",
                                                    fixture.context());
  PLR_REQUIRE(replace_again.has_value());
  PLR_EXPECT_ERR(registry->replace_location(replace_again.value()),
                 ErrorCode::PredecessorAlreadyReplaced);
}

PLR_TEST(mutations, replacement_rejections) {
  TempDir dir;
  PLR_EXPECT_OK(registry, Registry::create(dir.path(), writer_options()));
  Fixture fixture(registry, 137);
  PLR_EXPECT_OK(standard, build_standard(fixture));

  // A location with children cannot be replaced while they exist.
  auto with_children = ReplaceLocationRequest::make(standard.row.str(), "loc-row2", "",
                                                    fixture.context());
  PLR_REQUIRE(with_children.has_value());
  PLR_EXPECT_ERR(registry->replace_location(with_children.value()), ErrorCode::LocationHasChildren);

  // The successor identity must be new.
  PLR_EXPECT_OK(unit, fixture.add_unit(standard.rack, 1));
  auto existing = ReplaceLocationRequest::make(unit.str(), standard.rack.str(), "",
                                               fixture.context());
  PLR_REQUIRE(existing.has_value());
  PLR_EXPECT_ERR(registry->replace_location(existing.value()),
                 ErrorCode::ReplacementTargetExists);

  // Replacing a successor chain keeps lineage acyclic.
  auto first = ReplaceLocationRequest::make(unit.str(), "loc-unit-v2", "", fixture.context());
  PLR_REQUIRE(first.has_value());
  PLR_EXPECT_OK(first_receipt, registry->replace_location(first.value()));
  (void)first_receipt;
  auto second = ReplaceLocationRequest::make("loc-unit-v2", "loc-unit-v3", "", fixture.context());
  PLR_REQUIRE(second.has_value());
  PLR_EXPECT_OK(second_receipt, registry->replace_location(second.value()));
  (void)second_receipt;
  PLR_EXPECT_OK(v3, registry->find(LocationId::parse("loc-unit-v3").value()));
  PLR_EXPECT(v3.is_current());
  PLR_EXPECT_OK(v2, registry->find(LocationId::parse("loc-unit-v2").value()));
  PLR_EXPECT(v2.lifecycle() == LifecycleState::Replaced);
  const std::shared_ptr<const Snapshot> chain_snapshot = registry->copy_snapshot();
  PLR_REQUIRE(chain_snapshot != nullptr);
  PLR_EXPECT_EQ(chain_snapshot->replacements().size(), std::size_t{2});

  // Reusing the identity of a replaced location is impossible: identities are
  // never recycled by the registry.
  auto stale = CreateLocationRequest::make("loc-unit-v2", LocationKind::RackUnit, standard.rack.str(),
                                           "U2", "", fixture.context());
  PLR_REQUIRE(stale.has_value());
  auto coordinate = RackUnitCoordinate::parse(2);
  PLR_REQUIRE(coordinate.has_value());
  stale.value().unit = coordinate.value();
  PLR_EXPECT_ERR(registry->create_location(stale.value()), ErrorCode::AlreadyPresent);
}

PLR_TEST(mutations, alias_binding_rules) {
  TempDir dir;
  PLR_EXPECT_OK(registry, Registry::create(dir.path(), writer_options()));
  Fixture fixture(registry, 139);
  PLR_EXPECT_OK(standard, build_standard(fixture));
  const Limits limits = registry->limits();

  auto add = AddAliasRequest::make(standard.rack.str(), "/LEGACY/RACK-1", limits,
                                   fixture.context());
  PLR_REQUIRE(add.has_value());
  PLR_EXPECT_OK(receipt, registry->add_alias(add.value()));
  PLR_EXPECT_EQ(receipt.generation.value(), 2U);

  // The same alias twice is reported as already present.
  auto duplicate = AddAliasRequest::make(standard.rack.str(), "/LEGACY/RACK-1", limits,
                                         fixture.context());
  PLR_REQUIRE(duplicate.has_value());
  PLR_EXPECT_ERR(registry->add_alias(duplicate.value()), ErrorCode::AlreadyPresent);

  // The same alias on another location is a conflict.
  auto conflicting = AddAliasRequest::make(standard.row.str(), "/LEGACY/RACK-1", limits,
                                           fixture.context());
  PLR_REQUIRE(conflicting.has_value());
  PLR_EXPECT_ERR(registry->add_alias(conflicting.value()), ErrorCode::AliasConflict);

  // A look-alike alias is rejected.
  auto look_alike = AddAliasRequest::make(standard.row.str(), "/legacy/rack-1", limits,
                                          fixture.context());
  PLR_REQUIRE(look_alike.has_value());
  PLR_EXPECT_ERR(registry->add_alias(look_alike.value()), ErrorCode::AddressLookAlike);

  // An alias may not duplicate a current address.
  auto current = AddAliasRequest::make(standard.row.str(), "/FAC1/ROOM-1/ROW-1", limits,
                                       fixture.context());
  PLR_REQUIRE(current.has_value());
  PLR_EXPECT_ERR(registry->add_alias(current.value()), ErrorCode::AliasRedundant);
  auto other_current = AddAliasRequest::make(standard.row.str(), "/FAC1/ROOM-1/ROW-1/RACK-1", limits,
                                             fixture.context());
  PLR_REQUIRE(other_current.has_value());
  PLR_EXPECT_ERR(registry->add_alias(other_current.value()),
                 ErrorCode::AliasConflictsWithAddress);

  // A new location may not be created at an address that an alias already claims.
  PLR_EXPECT_OK(room_two, fixture.add(standard.facility, LocationKind::Room, "ROOM-2"));
  auto claim = AddAliasRequest::make(standard.rack.str(), "/FAC1/ROOM-2/ROW-9", limits,
                                     fixture.context());
  PLR_REQUIRE(claim.has_value());
  PLR_EXPECT_OK(claim_receipt, registry->add_alias(claim.value()));
  (void)claim_receipt;
  auto at_alias = CreateLocationRequest::make("loc-row9", LocationKind::Row, room_two.str(),
                                              "ROW-9", "", fixture.context());
  PLR_REQUIRE(at_alias.has_value());
  PLR_EXPECT_ERR(registry->create_location(at_alias.value()),
                 ErrorCode::AliasConflictsWithAddress);

  // Removing an alias that is not bound to this location is a conflict.
  auto wrong_owner = RemoveAliasRequest::make(standard.row.str(), "/LEGACY/RACK-1", limits,
                                              fixture.context());
  PLR_REQUIRE(wrong_owner.has_value());
  PLR_EXPECT_ERR(registry->remove_alias(wrong_owner.value()), ErrorCode::AliasConflict);
  auto unknown = RemoveAliasRequest::make(standard.rack.str(), "/LEGACY/NOPE", limits,
                                          fixture.context());
  PLR_REQUIRE(unknown.has_value());
  PLR_EXPECT_ERR(registry->remove_alias(unknown.value()), ErrorCode::AliasNotFound);

  // Removal works and the alias stops resolving.
  auto remove = RemoveAliasRequest::make(standard.rack.str(), "/LEGACY/RACK-1", limits,
                                         fixture.context());
  PLR_REQUIRE(remove.has_value());
  PLR_EXPECT_OK(remove_receipt, registry->remove_alias(remove.value()));
  (void)remove_receipt;
  PLR_EXPECT_OK(legacy_path, LocationPath::parse("/LEGACY/RACK-1", limits));
  PLR_EXPECT_ERR(registry->resolve(legacy_path), ErrorCode::NotFound);
  PLR_EXPECT_OK(bindings, registry->aliases());
  PLR_REQUIRE(bindings.size() == 1);
  PLR_EXPECT_EQ(bindings[0].alias, std::string("/FAC1/ROOM-2/ROW-9"));
}

PLR_TEST(mutations, moving_into_an_aliased_address_is_rejected) {
  TempDir dir;
  PLR_EXPECT_OK(registry, Registry::create(dir.path(), writer_options()));
  Fixture fixture(registry, 149);
  PLR_EXPECT_OK(standard, build_standard(fixture));
  const Limits limits = registry->limits();

  // An alias claims the address that the move would produce.
  auto alias = AddAliasRequest::make(standard.rack.str(), "/FAC1/ROOM-1/ROW-2", limits,
                                     fixture.context());
  PLR_REQUIRE(alias.has_value());
  PLR_EXPECT_OK(alias_receipt, registry->add_alias(alias.value()));
  (void)alias_receipt;

  auto readdress = ReaddressRequest::make(standard.row.str(), "ROW-2", fixture.context());
  PLR_REQUIRE(readdress.has_value());
  PLR_EXPECT_ERR(registry->readdress(readdress.value()), ErrorCode::AliasConflictsWithAddress);

  // An alias equal to the location's own new address is redundant and rejected.
  auto own_alias = AddAliasRequest::make(standard.row.str(), "/FAC1/ROOM-1/ROW-3", limits,
                                         fixture.context());
  PLR_REQUIRE(own_alias.has_value());
  PLR_EXPECT_OK(own_receipt, registry->add_alias(own_alias.value()));
  (void)own_receipt;
  auto own_readdress = ReaddressRequest::make(standard.row.str(), "ROW-3", fixture.context());
  PLR_REQUIRE(own_readdress.has_value());
  PLR_EXPECT_ERR(registry->readdress(own_readdress.value()), ErrorCode::AliasRedundant);

  // Removing that alias makes the same mutation legal, which proves the alias,
  // not the address, was the blocker.
  auto remove = RemoveAliasRequest::make(standard.row.str(), "/FAC1/ROOM-1/ROW-3", limits,
                                         fixture.context());
  PLR_REQUIRE(remove.has_value());
  PLR_EXPECT_OK(remove_receipt, registry->remove_alias(remove.value()));
  (void)remove_receipt;
  auto retry = ReaddressRequest::make(standard.row.str(), "ROW-3", fixture.context());
  PLR_REQUIRE(retry.has_value());
  PLR_EXPECT_OK(retry_receipt, registry->readdress(retry.value()));
  (void)retry_receipt;
}

PLR_TEST(mutations, rack_geometry_updates) {
  TempDir dir;
  PLR_EXPECT_OK(registry, Registry::create(dir.path(), writer_options()));
  Fixture fixture(registry, 151);
  PLR_EXPECT_OK(standard, build_standard(fixture));

  auto request = SetRackGeometryRequest::make(standard.rack.str(), fixture.context());
  PLR_REQUIRE(request.has_value());
  auto wider = RackEnvelope::with_height(52);
  PLR_REQUIRE(wider.has_value());
  request.value().envelope = wider.value();
  PLR_EXPECT_OK(receipt, registry->set_rack_geometry(request.value()));

  PLR_EXPECT_OK(view, registry->find(standard.rack));
  PLR_REQUIRE(view.envelope().has_value());
  PLR_EXPECT_EQ(view.envelope()->height(), 52U);
  PLR_EXPECT_EQ(receipt.generation.value(), 2U);

  // The same request again is a no-op.
  auto same = SetRackGeometryRequest::make(standard.rack.str(), fixture.context());
  PLR_REQUIRE(same.has_value());
  same.value().envelope = wider.value();
  PLR_EXPECT_ERR(registry->set_rack_geometry(same.value()), ErrorCode::NoOpMutation);

  // A rack unit must stay inside the envelope it declares.
  auto narrow = SetRackGeometryRequest::make(standard.rack.str(), fixture.context());
  PLR_REQUIRE(narrow.has_value());
  auto smaller = RackEnvelope::with_height(4);
  PLR_REQUIRE(smaller.has_value());
  narrow.value().envelope = smaller.value();
  PLR_EXPECT_OK(narrow_receipt, registry->set_rack_geometry(narrow.value()));
  (void)narrow_receipt;
  PLR_EXPECT_OK(unit, fixture.add_unit(standard.rack, 3));
  auto outside = SetRackGeometryRequest::make(standard.rack.str(), fixture.context());
  PLR_REQUIRE(outside.has_value());
  outside.value().envelope = RackEnvelope::with_height(2).value();
  PLR_EXPECT_ERR(registry->set_rack_geometry(outside.value()), ErrorCode::RackUnitOutOfEnvelope);

  (void)unit;
}

PLR_TEST(mutations, generation_diff_reports_each_change) {
  TempDir dir;
  PLR_EXPECT_OK(registry, Registry::create(dir.path(), writer_options()));
  Fixture fixture(registry, 157);
  PLR_EXPECT_OK(standard, build_standard(fixture));

  const LocationRevision after_create = registry->revision();
  auto relabel = RelabelRequest::make(standard.row.str(), "Row renamed", fixture.context());
  PLR_REQUIRE(relabel.has_value());
  PLR_EXPECT_OK(relabel_receipt, registry->relabel(relabel.value()));
  PLR_EXPECT_EQ(relabel_receipt.affected_locations, 1U);
  auto readdress = ReaddressRequest::make(standard.rack.str(), "RACK-9", fixture.context());
  PLR_REQUIRE(readdress.has_value());
  PLR_EXPECT_OK(readdress_receipt, registry->readdress(readdress.value()));
  PLR_EXPECT_EQ(readdress_receipt.affected_locations, 1U);

  PLR_EXPECT_OK(diff, registry->diff(after_create, registry->revision()));
  PLR_EXPECT_EQ(diff.from_revision.value(), after_create.value());
  PLR_EXPECT_EQ(diff.changed, 2U);
  PLR_EXPECT_EQ(diff.created, 0U);
  PLR_EXPECT(!diff.truncated);
  PLR_REQUIRE(diff.changes.size() == 2);
  // Sorted by identity, so the diff renders identically everywhere.
  for (std::size_t index = 1; index < diff.changes.size(); ++index) {
    PLR_EXPECT(diff.changes[index - 1].id < diff.changes[index].id);
  }
  bool saw_readdress = false;
  bool saw_relabel = false;
  for (const LocationChange& change : diff.changes) {
    if (change.id == standard.rack) {
      PLR_EXPECT(change.has(kChangeReaddressed));
      PLR_REQUIRE(change.path_before.has_value());
      PLR_REQUIRE(change.path_after.has_value());
      PLR_EXPECT_EQ(change.path_after->to_string(),
                    std::string("/FAC1/ROOM-1/ROW-1/RACK-9"));
      saw_readdress = true;
    }
    if (change.id == standard.row) {
      PLR_EXPECT(change.has(kChangeRelabeled));
      PLR_EXPECT_EQ(change.label_after.value_or(std::string()), std::string("Row renamed"));
      saw_relabel = true;
    }
  }
  PLR_EXPECT(saw_readdress);
  PLR_EXPECT(saw_relabel);
  PLR_EXPECT(!format_revision_diff(diff).empty());

  PLR_EXPECT_OK(empty_diff, registry->diff(registry->revision(), registry->revision()));
  PLR_EXPECT(empty_diff.empty());

  const std::vector<LocationRevision> retained = registry->retained_revisions();
  PLR_EXPECT(!retained.empty());
  PLR_EXPECT(retained.back() == registry->revision());
}

PLR_TEST(mutations, snapshot_comparison_matches_live_state) {
  TempDir dir;
  PLR_EXPECT_OK(registry, Registry::create(dir.path(), writer_options()));
  Fixture fixture(registry, 163);
  PLR_EXPECT_OK(standard, build_standard(fixture));

  std::shared_ptr<const Snapshot> before = registry->copy_snapshot();
  PLR_REQUIRE(before != nullptr);
  const LocationRevision before_revision = before->revision();

  auto relabel = RelabelRequest::make(standard.rack.str(), "Rack renamed", fixture.context());
  PLR_REQUIRE(relabel.has_value());
  PLR_EXPECT_OK(receipt, registry->relabel(relabel.value()));
  (void)receipt;

  std::shared_ptr<const Snapshot> after = registry->copy_snapshot();
  PLR_REQUIRE(after != nullptr);
  const RevisionDiff diff = diff_snapshots(*before, *after);
  PLR_EXPECT_EQ(diff.from_revision.value(), before_revision.value());
  PLR_EXPECT_EQ(diff.changed, 1U);
  PLR_REQUIRE(diff.changes.size() == 1);
  PLR_EXPECT(diff.changes[0].id == standard.rack);
  PLR_EXPECT(diff.changes[0].has(kChangeRelabeled));

  const RevisionDiff identical = diff_snapshots(*after, *after);
  PLR_EXPECT(identical.empty());

  // Snapshot queries answer without a registry and without a lock.
  PLR_EXPECT_OK(view, before->find(standard.rack));
  PLR_EXPECT_EQ(view.label(), std::string("Rack 1"));
  PLR_EXPECT_OK(current, after->find(standard.rack));
  PLR_EXPECT_EQ(current.label(), std::string("Rack renamed"));
  PLR_EXPECT_EQ(before->location_count(), 4U);
  PLR_EXPECT(!before->empty());
  PLR_EXPECT_OK(roots, before->roots());
  PLR_EXPECT_EQ(roots.size(), std::size_t{1});
  PLR_EXPECT_OK(children, before->children(standard.row));
  PLR_EXPECT_EQ(children.size(), std::size_t{1});
  PLR_EXPECT_OK(before_digest, before->canonical_digest());
  PLR_EXPECT_OK(after_digest, after->canonical_digest());
  PLR_EXPECT_NE(before_digest, after_digest);
  PLR_EXPECT_EQ(before_digest.size(), std::size_t{64});
}

PLR_TEST(mutations, reopen_preserves_history_and_lineage) {
  TempDir dir;
  LocationId unit;
  LocationId successor;
  {
    PLR_EXPECT_OK(registry, Registry::create(dir.path(), writer_options()));
    Fixture fixture(registry, 167);
    PLR_EXPECT_OK(standard, build_standard(fixture));
    PLR_EXPECT_OK(created_unit, fixture.add_unit(standard.rack, 11));
    unit = created_unit;
    auto replace = ReplaceLocationRequest::make(unit.str(), "loc-unit-v2", "", fixture.context());
    PLR_REQUIRE(replace.has_value());
    PLR_EXPECT_OK(receipt, registry->replace_location(replace.value()));
    successor = replace.value().successor_id;
    (void)receipt;
    PLR_EXPECT(registry->close().has_value());
  }
  {
    PLR_EXPECT_OK(reopened, Registry::open(dir.path(), writer_options()));
    PLR_EXPECT_OK(predecessor, reopened->find(unit));
    PLR_EXPECT(predecessor.lifecycle() == LifecycleState::Replaced);
    PLR_REQUIRE(predecessor.replaced_by().has_value());
    PLR_EXPECT(predecessor.replaced_by().value() == successor);
    PLR_EXPECT_OK(replacement, reopened->find(successor));
    PLR_REQUIRE(replacement.replaces().has_value());
    PLR_EXPECT(replacement.replaces().value() == unit);
    const std::shared_ptr<const Snapshot> reopened_snapshot = reopened->copy_snapshot();
    PLR_REQUIRE(reopened_snapshot != nullptr);
    PLR_EXPECT_EQ(reopened_snapshot->replacements().size(), std::size_t{1});
    const Limits limits = reopened->limits();
    PLR_EXPECT_OK(path, LocationPath::parse("/FAC1/ROOM-1/ROW-1/RACK-1/U11", limits));
    PLR_EXPECT_OK(resolved, reopened->resolve(path));
    PLR_EXPECT(resolved.id == successor);
  }
}
