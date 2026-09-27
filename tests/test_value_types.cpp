// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Value types: identities, text grammars, address construction, rack geometry,
// lifecycle table, limits and digests. These are the typed boundary every other
// guarantee rests on, so the rejections matter as much as the acceptances.

#include <string>
#include <vector>

#include "dccp/physical_location_registry/physical_location_registry.hpp"
#include "support/test_harness.hpp"

using namespace dccp::physical_location_registry;
using plr_test::Rng;

namespace {

Limits test_limits() {
  Limits limits;
  return limits;
}

std::string repeat(char character, std::size_t count) { return std::string(count, character); }

}  // namespace

PLR_TEST(ids, identifier_grammar) {
  PLR_EXPECT(is_valid_identifier_syntax("a"));
  PLR_EXPECT(is_valid_identifier_syntax("A1"));
  PLR_EXPECT(is_valid_identifier_syntax("rack-01.unit:7"));
  PLR_EXPECT(is_valid_identifier_syntax(repeat('a', kMaxIdentifierBytes)));

  PLR_EXPECT(!is_valid_identifier_syntax(""));
  PLR_EXPECT(!is_valid_identifier_syntax(repeat('a', kMaxIdentifierBytes + 1)));
  PLR_EXPECT(!is_valid_identifier_syntax(".leading"));
  PLR_EXPECT(!is_valid_identifier_syntax("trailing."));
  PLR_EXPECT(!is_valid_identifier_syntax("-leading"));
  PLR_EXPECT(!is_valid_identifier_syntax("has space"));
  PLR_EXPECT(!is_valid_identifier_syntax("has/slash"));
  PLR_EXPECT(!is_valid_identifier_syntax("has\\backslash"));
  PLR_EXPECT(!is_valid_identifier_syntax(".."));
  PLR_EXPECT(!is_valid_identifier_syntax("caf\xC3\xA9"));
  PLR_EXPECT(!is_valid_identifier_syntax(std::string("nul\0byte", 8)));

  PLR_EXPECT_OK(id, LocationId::parse("rack-07"));
  PLR_EXPECT_EQ(id.value(), std::string_view("rack-07"));
  PLR_EXPECT(!id.empty());
  PLR_EXPECT(LocationId().empty());

  PLR_EXPECT_ERR(LocationId::parse("bad id"), ErrorCode::MalformedIdentifier);
  PLR_EXPECT_ERR(ActorId::parse(""), ErrorCode::MalformedIdentifier);
  PLR_EXPECT_ERR(OperationId::parse("x/y"), ErrorCode::MalformedIdentifier);

  // Distinct identity types do not convert into one another.
  static_assert(!std::is_convertible_v<LocationId, ActorId>);
  static_assert(!std::is_convertible_v<LocationGeneration, LocationRevision>);
  static_assert(!std::is_convertible_v<WriterEpoch, StateSequence>);

  PLR_EXPECT_OK(a, LocationId::parse("rack-01"));
  PLR_EXPECT_OK(b, LocationId::parse("rack-02"));
  PLR_EXPECT(a < b);
  PLR_EXPECT(a != b);
}

PLR_TEST(ids, counter_types_are_monotonic_and_overflow_safe) {
  const LocationGeneration generation(7);
  PLR_EXPECT_EQ(generation.next().value().value(), 8U);
  PLR_EXPECT(generation.published());
  PLR_EXPECT(!LocationGeneration(0).published());
  PLR_EXPECT_EQ(LocationGeneration(UINT64_MAX).next().error().code(), ErrorCode::GenerationOverflow);

  PLR_EXPECT_EQ(LocationRevision(4).next().value().value(), 5U);
  PLR_EXPECT_EQ(LocationRevision(UINT64_MAX).next().error().code(), ErrorCode::RevisionOverflow);

  PLR_EXPECT_EQ(StateSequence(1).next().value().value(), 2U);
  PLR_EXPECT_EQ(StateSequence(UINT64_MAX).next().error().code(), ErrorCode::SequenceOverflow);

  PLR_EXPECT_EQ(WriterEpoch(2).next().value().value(), 3U);
  PLR_EXPECT_EQ(WriterEpoch(UINT64_MAX).next().error().code(), ErrorCode::EpochOverflow);
  PLR_EXPECT(!WriterEpoch(0).valid());
  PLR_EXPECT(WriterEpoch(1).valid());
}

PLR_TEST(text, utf8_validation_is_strict) {
  PLR_EXPECT(is_valid_utf8(""));
  PLR_EXPECT(is_valid_utf8("plain ascii"));
  PLR_EXPECT(is_valid_utf8("caf\xC3\xA9"));            // U+00E9
  PLR_EXPECT(is_valid_utf8("\xE2\x82\xAC"));            // U+20AC euro sign
  PLR_EXPECT(is_valid_utf8("\xF0\x9F\x9A\x80"));        // U+1F680 rocket
  PLR_EXPECT(is_valid_utf8("\xF4\x8F\xBF\xBF"));        // U+10FFFF, the last code point

  PLR_EXPECT(!is_valid_utf8("\xC0\x80"));          // overlong NUL
  PLR_EXPECT(!is_valid_utf8("\xC1\xBF"));          // overlong
  PLR_EXPECT(!is_valid_utf8("\xE0\x80\x80"));      // overlong three-byte
  PLR_EXPECT(!is_valid_utf8("\xF0\x80\x80\x80"));  // overlong four-byte
  PLR_EXPECT(!is_valid_utf8("\xED\xA0\x80"));      // surrogate U+D800
  PLR_EXPECT(!is_valid_utf8("\xED\xBF\xBF"));      // surrogate U+DFFF
  PLR_EXPECT(!is_valid_utf8("\xF4\x90\x80\x80"));  // beyond U+10FFFF
  PLR_EXPECT(!is_valid_utf8("\xF5\x80\x80\x80"));  // invalid lead byte
  PLR_EXPECT(!is_valid_utf8("\x80"));              // stray continuation
  PLR_EXPECT(!is_valid_utf8("\xE2\x82"));          // truncated
  PLR_EXPECT(!is_valid_utf8("abc\xC3"));           // truncated at the end
  // U+0000 is valid UTF-8; it is the display-text rules that reject it.
  PLR_EXPECT(is_valid_utf8(std::string("a\0b", 3)));
  PLR_EXPECT(!is_valid_label(std::string("a\0b", 3)));
}

PLR_TEST(text, label_grammar) {
  PLR_EXPECT(is_valid_label(""));
  PLR_EXPECT(is_valid_label("Rack 07 (north)"));
  PLR_EXPECT(is_valid_label("caf\xC3\xA9 \xE2\x82\xAC"));
  PLR_EXPECT(is_valid_label(repeat('x', kMaxLabelBytes)));

  PLR_EXPECT(!is_valid_label(repeat('x', kMaxLabelBytes + 1)));
  PLR_EXPECT(!is_valid_label("line\nbreak"));
  PLR_EXPECT(!is_valid_label("tab\tseparated"));
  PLR_EXPECT(!is_valid_label(std::string("bell\a", 5)));
  PLR_EXPECT(!is_valid_label("del\x7F"));
  PLR_EXPECT(!is_valid_label("\xC2\x85"));      // NEL, a C1 control
  PLR_EXPECT(!is_valid_label("\xEF\xB7\x90"));  // U+FDD0 noncharacter
  PLR_EXPECT(!is_valid_label("\xEF\xBF\xBE"));  // U+FFFE noncharacter
  PLR_EXPECT(!is_valid_label("\xC3\x28"));      // invalid UTF-8
  PLR_EXPECT(!is_valid_reason("with\nnewline"));
  PLR_EXPECT(!is_valid_source("with\rreturn"));
  PLR_EXPECT(is_valid_reason(""));
  PLR_EXPECT(is_valid_source(""));
  PLR_EXPECT(is_valid_reason("maintenance window"));
}

PLR_TEST(text, ascii_folding_and_byte_order) {
  PLR_EXPECT_EQ(ascii_fold("RaCk-07"), std::string("rack-07"));
  PLR_EXPECT_EQ(ascii_fold("caf\xC3\xA9"), std::string("caf\xC3\xA9"));
  PLR_EXPECT(ascii_case_insensitive_equal("ROOM", "room"));
  PLR_EXPECT(!ascii_case_insensitive_equal("ROOM", "rooms"));
  PLR_EXPECT_EQ(byte_compare("a", "b"), -1);
  PLR_EXPECT_EQ(byte_compare("b", "a"), 1);
  PLR_EXPECT_EQ(byte_compare("a", "a"), 0);
}

PLR_TEST(address, component_grammar_rejects_hostile_text) {
  PLR_EXPECT_OK(component, AddressComponent::parse("RACK-07"));
  PLR_EXPECT_EQ(component.value(), std::string_view("RACK-07"));
  PLR_EXPECT_OK(single, AddressComponent::parse("a"));
  PLR_EXPECT_EQ(single.value(), std::string_view("a"));
  PLR_EXPECT_OK(longest, AddressComponent::parse(repeat('a', kMaxAddressComponentBytes)));
  PLR_EXPECT_EQ(longest.value().size(), kMaxAddressComponentBytes);

  PLR_EXPECT_ERR(AddressComponent::parse(""), ErrorCode::MalformedAddressComponent);
  PLR_EXPECT_ERR(AddressComponent::parse(repeat('a', kMaxAddressComponentBytes + 1)),
                 ErrorCode::MalformedAddressComponent);
  PLR_EXPECT_ERR(AddressComponent::parse(".."), ErrorCode::MalformedAddressComponent);
  PLR_EXPECT_ERR(AddressComponent::parse("."), ErrorCode::MalformedAddressComponent);
  PLR_EXPECT_ERR(AddressComponent::parse("../etc"), ErrorCode::MalformedAddressComponent);
  PLR_EXPECT_ERR(AddressComponent::parse("a/b"), ErrorCode::MalformedAddressComponent);
  PLR_EXPECT_ERR(AddressComponent::parse("a\\b"), ErrorCode::MalformedAddressComponent);
  PLR_EXPECT_ERR(AddressComponent::parse("C:"), ErrorCode::MalformedAddressComponent);
  PLR_EXPECT_ERR(AddressComponent::parse("%2e%2e"), ErrorCode::MalformedAddressComponent);
  PLR_EXPECT_ERR(AddressComponent::parse("a b"), ErrorCode::MalformedAddressComponent);
  PLR_EXPECT_ERR(AddressComponent::parse("-a"), ErrorCode::MalformedAddressComponent);
  PLR_EXPECT_ERR(AddressComponent::parse("a-"), ErrorCode::MalformedAddressComponent);
  PLR_EXPECT_ERR(AddressComponent::parse("\xE2\x82\xAC"), ErrorCode::MalformedAddressComponent);
  PLR_EXPECT_ERR(AddressComponent::parse(std::string("a\0b", 3)), ErrorCode::MalformedAddressComponent);
}

PLR_TEST(address, canonical_path_construction_and_parsing) {
  const Limits limits = test_limits();
  PLR_EXPECT_OK(path, LocationPath::parse("/FAC1/BLDG-A/ROOM101", limits));
  PLR_EXPECT_EQ(path.depth(), std::size_t{3});
  PLR_EXPECT_EQ(path.to_string(), std::string("/FAC1/BLDG-A/ROOM101"));
  PLR_EXPECT_EQ(path.byte_length(), std::size_t{20});
  PLR_EXPECT_EQ(path.component(1).value(), std::string_view("BLDG-A"));

  PLR_REQUIRE(path.parent().has_value());
  const LocationPath parent = path.parent().value();
  PLR_EXPECT_EQ(parent.to_string(), std::string("/FAC1/BLDG-A"));
  PLR_EXPECT(parent.is_ancestor_of(path));
  PLR_EXPECT(!path.is_ancestor_of(parent));
  PLR_EXPECT(!path.is_ancestor_of(path));

  PLR_EXPECT_OK(root, LocationPath::parse("/FAC1", limits));
  PLR_EXPECT(!root.parent().has_value());
  PLR_EXPECT(root.is_ancestor_of(path));

  PLR_EXPECT_OK(empty, LocationPath::parse("/", limits));
  PLR_EXPECT(empty.empty());
  PLR_EXPECT_EQ(empty.to_string(), std::string("/"));
  PLR_EXPECT_EQ(empty.byte_length(), std::size_t{1});
  PLR_EXPECT(!empty.parent().has_value());

  PLR_EXPECT_OK(child, root.child(path.component(1), limits));
  PLR_EXPECT_EQ(child.to_string(), std::string("/FAC1/BLDG-A"));
}

PLR_TEST(address, hostile_and_malformed_paths_are_rejected) {
  const Limits limits = test_limits();
  PLR_EXPECT_ERR(LocationPath::parse("", limits), ErrorCode::MalformedPath);
  PLR_EXPECT_ERR(LocationPath::parse("FAC1", limits), ErrorCode::MalformedPath);
  PLR_EXPECT_ERR(LocationPath::parse("/FAC1/", limits), ErrorCode::MalformedPath);
  PLR_EXPECT_ERR(LocationPath::parse("//FAC1", limits), ErrorCode::MalformedPath);
  PLR_EXPECT_ERR(LocationPath::parse("/FAC1//ROOM", limits), ErrorCode::MalformedPath);
  PLR_EXPECT_ERR(LocationPath::parse("/..", limits), ErrorCode::MalformedAddressComponent);
  PLR_EXPECT_ERR(LocationPath::parse("/../..", limits), ErrorCode::MalformedAddressComponent);
  PLR_EXPECT_ERR(LocationPath::parse("/FAC/../../etc/passwd", limits),
                 ErrorCode::MalformedAddressComponent);
  PLR_EXPECT_ERR(LocationPath::parse("/FAC\\WINDOWS", limits), ErrorCode::MalformedAddressComponent);
  PLR_EXPECT_ERR(LocationPath::parse("/C:/Windows", limits), ErrorCode::MalformedAddressComponent);
  PLR_EXPECT_ERR(LocationPath::parse("/%2e%2e", limits), ErrorCode::MalformedAddressComponent);
  PLR_EXPECT_ERR(LocationPath::parse("/FAC/ROOM\x01", limits), ErrorCode::MalformedAddressComponent);

  Limits narrow = limits;
  narrow.max_depth = 2;
  PLR_EXPECT_ERR(LocationPath::parse("/A/B/C", narrow), ErrorCode::PathTooDeep);
  Limits short_path = limits;
  short_path.max_path_bytes = 6;
  PLR_EXPECT_ERR(LocationPath::parse("/AAAAAAAA", short_path), ErrorCode::PathTooLong);
  PLR_EXPECT_ERR(LocationPath::parse("/" + repeat('a', 5000), limits), ErrorCode::PathTooLong);
}

PLR_TEST(rack, unit_coordinates_and_envelopes) {
  PLR_EXPECT_OK(unit, RackUnitCoordinate::parse(1));
  PLR_EXPECT_EQ(unit.value(), 1U);
  PLR_EXPECT_EQ(unit.to_string(), std::string("U1"));
  PLR_EXPECT_ERR(RackUnitCoordinate::parse(0), ErrorCode::InvalidArgument);
  PLR_EXPECT_ERR(RackUnitCoordinate::parse(kMaxRackUnitCoordinate + 1U), ErrorCode::InvalidArgument);
  PLR_EXPECT_OK(highest, RackUnitCoordinate::parse(kMaxRackUnitCoordinate));
  PLR_EXPECT_EQ(highest.value(), kMaxRackUnitCoordinate);

  PLR_EXPECT_OK(textual, RackUnitCoordinate::parse_text("U12"));
  PLR_EXPECT_EQ(textual.value(), 12U);
  PLR_EXPECT_OK(suffixed, RackUnitCoordinate::parse_text("12U"));
  PLR_EXPECT_EQ(suffixed.value(), 12U);
  PLR_EXPECT_OK(bare, RackUnitCoordinate::parse_text("12"));
  PLR_EXPECT_EQ(bare.value(), 12U);
  PLR_EXPECT_ERR(RackUnitCoordinate::parse_text("U"), ErrorCode::InvalidArgument);
  PLR_EXPECT_ERR(RackUnitCoordinate::parse_text("12.5"), ErrorCode::InvalidArgument);
  PLR_EXPECT_ERR(RackUnitCoordinate::parse_text("99999999999999999999"), ErrorCode::InvalidArgument);

  PLR_EXPECT_OK(envelope, RackEnvelope::with_height(48));
  PLR_EXPECT_EQ(envelope.first().value(), 1U);
  PLR_EXPECT_EQ(envelope.height(), 48U);
  PLR_EXPECT_EQ(envelope.last().value(), 48U);
  PLR_EXPECT_EQ(envelope.to_string(), std::string("1-48U"));
  PLR_EXPECT(envelope.contains(unit));
  PLR_EXPECT(envelope.contains(envelope.last()));
  PLR_EXPECT(!envelope.contains(RackUnitCoordinate::parse(49).value()));
  PLR_EXPECT_ERR(RackEnvelope::with_height(0), ErrorCode::RackEnvelopeInvalid);
  PLR_EXPECT_ERR(RackEnvelope::make(RackUnitCoordinate::parse(500).value(), 40),
                 ErrorCode::RackEnvelopeInvalid);

  PLR_EXPECT_OK(range, RackEnvelope::parse_text("1-48"));
  PLR_EXPECT_EQ(range.height(), 48U);
  PLR_EXPECT_OK(single, RackEnvelope::parse_text("42U"));
  PLR_EXPECT_EQ(single.height(), 42U);
  PLR_EXPECT_OK(offset, RackEnvelope::parse_text("5-8U"));
  PLR_EXPECT_EQ(offset.first().value(), 5U);
  PLR_EXPECT_EQ(offset.height(), 4U);
  PLR_EXPECT_ERR(RackEnvelope::parse_text("8-5"), ErrorCode::RackEnvelopeInvalid);
  PLR_EXPECT_ERR(RackEnvelope::parse_text("0-5"), ErrorCode::InvalidArgument);
  PLR_EXPECT_ERR(RackEnvelope::parse_text("x-y"), ErrorCode::InvalidArgument);
}

PLR_TEST(kind, containment_schema) {
  PLR_EXPECT(kind_may_be_root(LocationKind::Facility));
  PLR_EXPECT(!kind_may_be_root(LocationKind::Room));
  PLR_EXPECT(kind_requires_parent(LocationKind::RackUnit));
  PLR_EXPECT(!kind_requires_parent(LocationKind::Facility));

  PLR_EXPECT(is_legal_child_kind(LocationKind::Facility, LocationKind::Building));
  PLR_EXPECT(is_legal_child_kind(LocationKind::Facility, LocationKind::Zone));
  PLR_EXPECT(!is_legal_child_kind(LocationKind::Facility, LocationKind::Facility));
  PLR_EXPECT(!is_legal_child_kind(LocationKind::Facility, LocationKind::RackUnit));
  PLR_EXPECT(is_legal_child_kind(LocationKind::Room, LocationKind::Row));
  PLR_EXPECT(is_legal_child_kind(LocationKind::Room, LocationKind::Rack));
  PLR_EXPECT(is_legal_child_kind(LocationKind::Row, LocationKind::Rack));
  PLR_EXPECT(!is_legal_child_kind(LocationKind::Row, LocationKind::Room));
  PLR_EXPECT(is_legal_child_kind(LocationKind::Rack, LocationKind::RackUnit));
  PLR_EXPECT(!is_legal_child_kind(LocationKind::Rack, LocationKind::Rack));
  PLR_EXPECT(is_legal_child_kind(LocationKind::Cage, LocationKind::Rack));
  PLR_EXPECT(!is_legal_child_kind(LocationKind::Cage, LocationKind::RackUnit));
  PLR_EXPECT(is_legal_child_kind(LocationKind::Zone, LocationKind::Row));
  PLR_EXPECT(!is_legal_child_kind(LocationKind::RackUnit, LocationKind::RackUnit));
  PLR_EXPECT(!is_legal_child_kind(LocationKind::Zone, LocationKind::Zone));

  const std::vector<LocationKind> facility_children = allowed_child_kinds(LocationKind::Facility);
  PLR_REQUIRE(facility_children.size() == 5);
  PLR_EXPECT_EQ(facility_children[0], LocationKind::Building);
  PLR_EXPECT_EQ(facility_children[1], LocationKind::Hall);

  PLR_EXPECT_EQ(all_location_kinds().size(), kLocationKindCount);
  PLR_EXPECT_EQ(location_kind_name(LocationKind::RackUnit), std::string_view("rack-unit"));
  PLR_EXPECT_EQ(parse_location_kind("RACK-UNIT").value(), LocationKind::RackUnit);
  PLR_EXPECT_EQ(parse_location_kind("rack_unit").value(), LocationKind::RackUnit);
  PLR_EXPECT_EQ(parse_location_kind("Zone").value(), LocationKind::Zone);
  PLR_EXPECT_ERR(parse_location_kind("floor"), ErrorCode::UnknownEnumToken);

  PLR_EXPECT(kind_may_carry_unit_coordinate(LocationKind::RackUnit));
  PLR_EXPECT(!kind_may_carry_unit_coordinate(LocationKind::Rack));
  PLR_EXPECT(kind_may_carry_envelope(LocationKind::Rack));
  PLR_EXPECT(!kind_may_carry_envelope(LocationKind::Row));
}

PLR_TEST(lifecycle, transition_table) {
  PLR_EXPECT(is_legal_lifecycle_transition(LifecycleState::Active, LifecycleTransition::Retire));
  PLR_EXPECT(is_legal_lifecycle_transition(LifecycleState::Active, LifecycleTransition::Replace));
  PLR_EXPECT(!is_legal_lifecycle_transition(LifecycleState::Active, LifecycleTransition::Reactivate));
  PLR_EXPECT(is_legal_lifecycle_transition(LifecycleState::Retired, LifecycleTransition::Reactivate));
  PLR_EXPECT(is_legal_lifecycle_transition(LifecycleState::Retired, LifecycleTransition::Replace));
  PLR_EXPECT(!is_legal_lifecycle_transition(LifecycleState::Retired, LifecycleTransition::Retire));
  PLR_EXPECT(!is_legal_lifecycle_transition(LifecycleState::Replaced, LifecycleTransition::Retire));
  PLR_EXPECT(!is_legal_lifecycle_transition(LifecycleState::Replaced, LifecycleTransition::Reactivate));
  PLR_EXPECT(!is_legal_lifecycle_transition(LifecycleState::Replaced, LifecycleTransition::Replace));

  PLR_EXPECT_EQ(legal_lifecycle_transitions(LifecycleState::Replaced).size(), std::size_t{0});
  PLR_EXPECT_EQ(legal_lifecycle_transitions(LifecycleState::Active).size(), std::size_t{2});
  PLR_EXPECT(lifecycle_state_is_current(LifecycleState::Active));
  PLR_EXPECT(!lifecycle_state_is_current(LifecycleState::Retired));
  PLR_EXPECT(lifecycle_state_is_terminal(LifecycleState::Replaced));
  PLR_EXPECT_EQ(lifecycle_state_name(LifecycleState::Replaced), std::string_view("replaced"));
  PLR_EXPECT_EQ(parse_lifecycle_state("ACTIVE").value(), LifecycleState::Active);
  PLR_EXPECT_ERR(parse_lifecycle_state("gone"), ErrorCode::UnknownEnumToken);
  PLR_EXPECT_EQ(lifecycle_transition_name(LifecycleTransition::Replace), std::string_view("replace"));
}

PLR_TEST(limits, validation_rejects_out_of_range_configuration) {
  Limits limits;
  PLR_EXPECT(limits.validate().has_value());

  Limits zero_locations = limits;
  zero_locations.max_locations = 0;
  PLR_EXPECT_ERR(zero_locations.validate(), ErrorCode::LimitExceeded);

  Limits huge_locations = limits;
  huge_locations.max_locations = kHardMaxLocations + 1U;
  PLR_EXPECT_ERR(huge_locations.validate(), ErrorCode::LimitExceeded);

  Limits huge_component = limits;
  huge_component.max_address_component_bytes = kMaxAddressComponentBytes + 1U;
  PLR_EXPECT_ERR(huge_component.validate(), ErrorCode::LimitExceeded);

  Limits huge_label = limits;
  huge_label.max_label_bytes = kMaxLabelBytes + 1U;
  PLR_EXPECT_ERR(huge_label.validate(), ErrorCode::LimitExceeded);

  Limits bad_relation = limits;
  bad_relation.max_path_bytes = bad_relation.max_address_component_bytes;
  PLR_EXPECT_ERR(bad_relation.validate(), ErrorCode::LimitExceeded);

  Limits bad_aliases = limits;
  bad_aliases.max_total_aliases = 1;
  bad_aliases.max_aliases_per_location = 2;
  PLR_EXPECT_ERR(bad_aliases.validate(), ErrorCode::LimitExceeded);

  Limits bad_moves = limits;
  bad_moves.max_total_moves = 1;
  bad_moves.max_moves_per_location = 2;
  PLR_EXPECT_ERR(bad_moves.validate(), ErrorCode::LimitExceeded);

  Limits bad_children = limits;
  bad_children.max_children_per_location = bad_children.max_locations + 1U;
  PLR_EXPECT_ERR(bad_children.validate(), ErrorCode::LimitExceeded);

  Limits bad_state = limits;
  bad_state.max_state_bytes = 16;
  PLR_EXPECT_ERR(bad_state.validate(), ErrorCode::LimitExceeded);

  Limits bad_publications = limits;
  bad_publications.max_publications_retained = 1;
  PLR_EXPECT_ERR(bad_publications.validate(), ErrorCode::LimitExceeded);

  const std::string rendered = limits.to_string();
  PLR_EXPECT(rendered.find("max_locations=") != std::string::npos);
  PLR_EXPECT(rendered.find("max_state_bytes=") != std::string::npos);
}

PLR_TEST(provenance, timestamp_validation_and_formatting) {
  PLR_EXPECT_OK(epoch, Timestamp::from_unix_seconds(0));
  PLR_EXPECT(!epoch.known());
  PLR_EXPECT_EQ(epoch.to_string(), std::string("1970-01-01T00:00:00Z"));

  PLR_EXPECT_OK(instant, Timestamp::make(1700000000, 123456789));
  PLR_EXPECT_EQ(instant.to_string(), std::string("2023-11-14T22:13:20.123456789Z"));
  PLR_EXPECT_OK(whole, Timestamp::make(1700000000, 0));
  PLR_EXPECT_EQ(whole.to_string(), std::string("2023-11-14T22:13:20Z"));

  PLR_EXPECT_OK(leap, Timestamp::parse_text("2024-02-29T00:00:00Z"));
  PLR_EXPECT_OK(leap_seconds, Timestamp::parse_text("2024-02-29T12:34:56.5Z"));
  PLR_EXPECT_EQ(Timestamp::parse_text("172800").value().to_string(),
                std::string("1970-01-03T00:00:00Z"));
  PLR_EXPECT_EQ(leap.to_string(), std::string("2024-02-29T00:00:00Z"));
  PLR_EXPECT_EQ(leap_seconds.nanos(), 500000000U);

  PLR_EXPECT_ERR(Timestamp::make(-1, 0), ErrorCode::InvalidArgument);
  PLR_EXPECT_ERR(Timestamp::make(0, 1000000000U), ErrorCode::InvalidArgument);
  PLR_EXPECT_ERR(Timestamp::from_unix_seconds(kMaxUnixSeconds + 1), ErrorCode::InvalidArgument);
  PLR_EXPECT_ERR(Timestamp::parse_text("2023-02-29T00:00:00Z"), ErrorCode::InvalidArgument);
  PLR_EXPECT_ERR(Timestamp::parse_text("2023-13-01T00:00:00Z"), ErrorCode::InvalidArgument);
  PLR_EXPECT_ERR(Timestamp::parse_text("2023-01-01T24:00:00Z"), ErrorCode::InvalidArgument);
  PLR_EXPECT_ERR(Timestamp::parse_text("2023-01-01 00:00:00Z"), ErrorCode::InvalidArgument);
  PLR_EXPECT_ERR(Timestamp::parse_text("1969-12-31T23:59:59Z"), ErrorCode::InvalidArgument);
  PLR_EXPECT_ERR(Timestamp::parse_text("2023-01-01T00:00:00"), ErrorCode::InvalidArgument);
  PLR_EXPECT_ERR(Timestamp::parse_text("not-a-time"), ErrorCode::InvalidArgument);
  PLR_EXPECT_ERR(Timestamp::parse_text(""), ErrorCode::InvalidArgument);
  PLR_EXPECT_ERR(Timestamp::parse_text("2023-01-01T00:00:00.0000000000Z"),
                 ErrorCode::InvalidArgument);

  // Round trip over a spread of instants, including leap days and year ends.
  const std::int64_t samples[] = {0,      1,        86399,   86400,   951782400,
                                  1709164800, 1735689599, 253402300799LL};
  for (const std::int64_t sample : samples) {
    const auto original = Timestamp::from_unix_seconds(sample);
    PLR_REQUIRE(original.has_value());
    const auto reparsed = Timestamp::parse_text(original.value().to_string());
    PLR_REQUIRE(reparsed.has_value());
    PLR_EXPECT_EQ(reparsed.value().unix_seconds(), sample);
  }

  PLR_EXPECT_EQ(move_kind_name(MoveKind::Reparent), std::string_view("reparent"));
  PLR_EXPECT_EQ(parse_move_kind("Readdress").value(), MoveKind::Readdress);
  PLR_EXPECT_ERR(parse_move_kind("teleport"), ErrorCode::UnknownEnumToken);
}

PLR_TEST(digest, sha256_matches_published_vectors) {
  PLR_EXPECT_EQ(Sha256::hex(""),
                std::string("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
  PLR_EXPECT_EQ(Sha256::hex("abc"),
                std::string("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
  PLR_EXPECT_EQ(
      Sha256::hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"),
      std::string("248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"));

  // Streaming in awkward chunk sizes must equal the one-shot digest.
  std::string payload;
  for (int index = 0; index < 1000; ++index) {
    payload.append("block-");
    payload.append(std::to_string(index));
    payload.push_back('\n');
  }
  Sha256 streaming;
  std::size_t offset = 0;
  std::size_t chunk = 1;
  while (offset < payload.size()) {
    const std::size_t take = std::min(chunk, payload.size() - offset);
    streaming.update(std::string_view(payload).substr(offset, take));
    offset += take;
    chunk = chunk * 3U + 1U;
  }
  PLR_EXPECT_EQ(streaming.finish_hex(), Sha256::hex(payload));

  // The padding boundary: 55, 56, 63, 64 and 65 byte messages.
  for (const std::size_t size : {std::size_t{55}, std::size_t{56}, std::size_t{63}, std::size_t{64},
                                 std::size_t{65}, std::size_t{119}, std::size_t{120}}) {
    const std::string message = repeat('a', size);
    Sha256 one_shot;
    one_shot.update(message);
    const std::string hex = one_shot.finish_hex();
    PLR_EXPECT_EQ(hex.size(), std::size_t{64});
    PLR_EXPECT(is_lower_hex_sha256(hex));
    PLR_EXPECT_EQ(hex, Sha256::hex(message));
  }

  PLR_EXPECT(!is_lower_hex_sha256("ABCDEF"));
  PLR_EXPECT(!is_lower_hex_sha256(repeat('a', 63)));
  PLR_EXPECT(is_lower_hex_sha256(repeat('a', 64)));
  PLR_EXPECT(!is_lower_hex_sha256(repeat('g', 64)));
  PLR_EXPECT(digest_equal("abc", "abc"));
  PLR_EXPECT(!digest_equal("abc", "abd"));
  PLR_EXPECT(!digest_equal("abc", "ab"));
}

PLR_TEST(result, codes_categories_and_retryability) {
  PLR_EXPECT_EQ(error_code_name(ErrorCode::Ok), std::string_view("OK"));
  PLR_EXPECT_EQ(error_code_name(ErrorCode::StaleGeneration), std::string_view("STALE_GENERATION"));
  PLR_EXPECT_EQ(error_code_name(ErrorCode::NoOpMutation), std::string_view("NO_OP_MUTATION"));
  PLR_EXPECT_EQ(error_category(ErrorCode::StaleGeneration), ErrorCategory::Authority);
  PLR_EXPECT_EQ(error_category(ErrorCode::AddressInUse), ErrorCategory::Structure);
  PLR_EXPECT_EQ(error_category(ErrorCode::DigestMismatch), ErrorCategory::Argument);
  PLR_EXPECT_EQ(error_category(ErrorCode::StoreLocked), ErrorCategory::Persistence);
  PLR_EXPECT_EQ(error_category(ErrorCode::LimitExceeded), ErrorCategory::Limit);
  PLR_EXPECT_EQ(error_category(ErrorCode::Cancelled), ErrorCategory::Cancelled);
  PLR_EXPECT_EQ(error_category_name(ErrorCategory::Structure), std::string_view("structure"));

  PLR_EXPECT(error_code_is_retryable(ErrorCode::StaleGeneration));
  PLR_EXPECT(error_code_is_retryable(ErrorCode::StaleRevision));
  PLR_EXPECT(error_code_is_retryable(ErrorCode::StoreLocked));
  PLR_EXPECT(!error_code_is_retryable(ErrorCode::AddressInUse));
  PLR_EXPECT(!error_code_is_retryable(ErrorCode::NotFound));

  const Error error(ErrorCode::AddressInUse, "component is taken");
  PLR_EXPECT_EQ(error.to_string(), std::string("ADDRESS_IN_USE: component is taken"));
  const Error with_subject =
      Error(ErrorCode::NotFound, "no such location").with_subject("loc-1");
  PLR_EXPECT_EQ(with_subject.to_string(),
                std::string("NOT_FOUND: no such location [subject=loc-1]"));
  PLR_EXPECT(with_subject.category() == ErrorCategory::Structure);

  const Result<int> failure(Error(ErrorCode::InternalError, "boom"));
  PLR_EXPECT(!failure.has_value());
  PLR_EXPECT_EQ(failure.error().code(), ErrorCode::InternalError);
  const Result<int> success(7);
  PLR_EXPECT(success.has_value());
  PLR_EXPECT_EQ(success.value(), 7);
  PLR_EXPECT_EQ(*success, 7);

  const Explanation explanation = explain_error(Error(ErrorCode::StaleGeneration, "stale")
                                                    .with_subject("loc-1"));
  PLR_EXPECT(!explanation.ok());
  PLR_EXPECT_EQ(explanation.category, ErrorCategory::Authority);
  PLR_EXPECT(explanation.summary.find("STALE_GENERATION") != std::string::npos);
  PLR_EXPECT(!explanation.hints.empty());
  PLR_EXPECT(explanation.to_string().find("hint:") != std::string::npos);
  PLR_EXPECT(explain_error_code(ErrorCode::Ok).ok());
}

PLR_TEST(version, identity) {
  PLR_EXPECT_EQ(version_string(), std::string_view("1.0.0"));
  PLR_EXPECT_EQ(library_name(), std::string_view("physical_location_registry"));
  PLR_EXPECT_EQ(snapshot_format_name(), std::string_view("PLRSNAP/1"));
}
