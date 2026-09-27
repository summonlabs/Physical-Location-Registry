// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Seeded property tests. Every case is reproducible from its seed, and the
// derived facts (canonical paths, child order, resolution) are checked against
// an independent reference model written directly from the documented rules
// rather than by calling the library.

#include <algorithm>
#include <map>
#include <memory>
#include <optional>
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

/// An independent model of the hierarchy: it stores parents and components and
/// derives paths and child order from first principles.
class ReferenceModel {
 public:
  struct Node {
    LocationKind kind = LocationKind::Facility;
    std::optional<std::string> parent;
    std::string component;
    std::string label;
    LifecycleState lifecycle = LifecycleState::Active;
    std::vector<std::string> aliases;
  };

  bool contains(const std::string& id) const { return nodes_.find(id) != nodes_.end(); }

  void insert(const std::string& id, LocationKind kind, std::optional<std::string> parent,
              std::string component, std::string label) {
    Node node;
    node.kind = kind;
    node.parent = std::move(parent);
    node.component = std::move(component);
    node.label = std::move(label);
    nodes_.emplace(id, std::move(node));
  }

  Node* find(const std::string& id) {
    const auto iterator = nodes_.find(id);
    return iterator == nodes_.end() ? nullptr : &iterator->second;
  }

  const Node* find(const std::string& id) const {
    const auto iterator = nodes_.find(id);
    return iterator == nodes_.end() ? nullptr : &iterator->second;
  }

  void erase(const std::string& id) { nodes_.erase(id); }

  /// Canonical path derived by walking parents, with cycle detection.
  std::optional<std::string> path_of(const std::string& id) const {
    std::vector<std::string> components;
    std::string current = id;
    for (std::size_t step = 0; step <= nodes_.size(); ++step) {
      const Node* node = find(current);
      if (node == nullptr) {
        return std::nullopt;
      }
      components.push_back(node->component);
      if (!node->parent.has_value()) {
        std::reverse(components.begin(), components.end());
        std::string path;
        for (const std::string& component : components) {
          path.push_back('/');
          path.append(component);
        }
        return path;
      }
      current = node->parent.value();
    }
    return std::nullopt;  // a cycle
  }

  /// Addressable children in canonical order: replaced nodes are not addressable.
  std::vector<std::string> children_of(const std::optional<std::string>& parent) const {
    std::vector<std::pair<std::string, std::string>> pairs;
    for (const auto& entry : nodes_) {
      if (entry.second.lifecycle == LifecycleState::Replaced) {
        continue;
      }
      if (entry.second.parent == parent) {
        pairs.emplace_back(entry.second.component, entry.first);
      }
    }
    std::sort(pairs.begin(), pairs.end());
    std::vector<std::string> ids;
    ids.reserve(pairs.size());
    for (auto& pair : pairs) {
      ids.push_back(std::move(pair.second));
    }
    return ids;
  }

  std::size_t size() const noexcept { return nodes_.size(); }

  struct Counts {
    std::uint32_t locations = 0;
    std::uint32_t active = 0;
    std::uint32_t retired = 0;
    std::uint32_t replaced = 0;
    std::uint32_t roots = 0;
    std::uint32_t aliases = 0;
  };

  Counts counts() const {
    Counts counts;
    counts.locations = static_cast<std::uint32_t>(nodes_.size());
    for (const auto& entry : nodes_) {
      switch (entry.second.lifecycle) {
        case LifecycleState::Active:
          ++counts.active;
          break;
        case LifecycleState::Retired:
          ++counts.retired;
          break;
        case LifecycleState::Replaced:
          ++counts.replaced;
          break;
      }
      if (!entry.second.parent.has_value()) {
        ++counts.roots;
      }
      counts.aliases += static_cast<std::uint32_t>(entry.second.aliases.size());
    }
    return counts;
  }

  std::vector<std::string> all_ids() const {
    std::vector<std::string> ids;
    ids.reserve(nodes_.size());
    for (const auto& entry : nodes_) {
      ids.push_back(entry.first);
    }
    std::sort(ids.begin(), ids.end());
    return ids;
  }

 private:
  std::map<std::string, Node> nodes_;
};

/// Kind names that may be created under a given kind, mirroring the documented
/// containment schema but written independently as a table of strings.
std::vector<LocationKind> legal_children(LocationKind kind) {
  switch (kind) {
    case LocationKind::Facility:
      return {LocationKind::Building, LocationKind::Hall, LocationKind::Room, LocationKind::Cage,
              LocationKind::Zone};
    case LocationKind::Building:
      return {LocationKind::Hall, LocationKind::Room, LocationKind::Cage, LocationKind::Zone};
    case LocationKind::Hall:
      return {LocationKind::Room, LocationKind::Row, LocationKind::Cage, LocationKind::Zone};
    case LocationKind::Room:
      return {LocationKind::Row, LocationKind::Rack, LocationKind::Cage, LocationKind::Zone};
    case LocationKind::Row:
      return {LocationKind::Rack, LocationKind::Cage};
    case LocationKind::Cage:
      return {LocationKind::Rack};
    case LocationKind::Rack:
      return {LocationKind::RackUnit};
    case LocationKind::Zone:
      return {LocationKind::Row, LocationKind::Rack, LocationKind::Cage};
    case LocationKind::RackUnit:
      return {};
  }
  return {};
}

struct PropertyState {
  std::shared_ptr<Registry> registry;
  Fixture fixture;
  ReferenceModel reference;
  Rng rng;
  std::uint64_t serial = 0;
};

/// Creates one location in both models and verifies that they agree.
bool create_location(PropertyState& state, const std::string& parent, LocationKind kind,
                     const std::string& label) {
  const std::string id = "loc-" + std::to_string(++state.serial);
  const std::string component = state.fixture.next_component("C");
  auto request = CreateLocationRequest::make(id, kind, parent, component, label, state.fixture.context());
  if (!request.has_value()) {
    return false;
  }
  if (kind == LocationKind::Rack) {
    auto envelope = RackEnvelope::with_height(static_cast<std::uint32_t>(4 + state.rng.below(40)));
    if (!envelope.has_value()) {
      return false;
    }
    request.value().envelope = envelope.value();
  }
  if (kind == LocationKind::RackUnit) {
    auto coordinate = RackUnitCoordinate::parse(1 + state.rng.below(4));
    if (!coordinate.has_value()) {
      return false;
    }
    request.value().unit = coordinate.value();
  }
  const auto receipt = state.registry->create_location(request.value());
  if (!receipt.has_value()) {
    return false;
  }
  state.reference.insert(id, kind, parent.empty() ? std::nullopt : std::optional<std::string>(parent),
                         component, label);
  return true;
}

/// Compares the library's derived view with the reference model.
void verify_agreement(PropertyState& state, const char* context) {
  const std::shared_ptr<const Snapshot> snapshot = state.registry->copy_snapshot();
  PLR_REQUIRE(snapshot != nullptr);

  const ReferenceModel::Counts expected = state.reference.counts();
  const LocationStatistics actual = snapshot->statistics();
  PLR_EXPECT_MSG(actual.locations == expected.locations, context);
  PLR_EXPECT_MSG(actual.active == expected.active, context);
  PLR_EXPECT_MSG(actual.retired == expected.retired, context);
  PLR_EXPECT_MSG(actual.replaced == expected.replaced, context);
  PLR_EXPECT_MSG(actual.roots == expected.roots, context);
  PLR_EXPECT_MSG(actual.aliases == expected.aliases, context);

  for (const std::string& id : state.reference.all_ids()) {
    const ReferenceModel::Node* node = state.reference.find(id);
    PLR_REQUIRE(node != nullptr);
    const auto parsed = LocationId::parse(id);
    PLR_REQUIRE(parsed.has_value());
    const std::optional<std::string> expected_path = state.reference.path_of(id);
    PLR_REQUIRE(expected_path.has_value());
    const auto actual_path = snapshot->path_of(parsed.value());
    PLR_REQUIRE(actual_path.has_value());
    PLR_EXPECT_MSG(actual_path.value().to_string() == expected_path.value(), context);

    // A current location resolves by its canonical address; a replaced one never
    // resolves, and a retired one only when it is asked for.
    const auto address = LocationPath::parse(expected_path.value(), snapshot->limits());
    PLR_REQUIRE(address.has_value());
    const auto resolved = snapshot->resolve(address.value());
    if (node->lifecycle == LifecycleState::Active) {
      PLR_REQUIRE(resolved.has_value());
      PLR_EXPECT_MSG(resolved.value().id.str() == id, context);
    } else {
      PLR_EXPECT_MSG(!resolved.has_value(), context);
      if (node->lifecycle == LifecycleState::Retired) {
        const auto retired = snapshot->resolve(address.value(), ResolutionMode::IncludeRetired);
        PLR_REQUIRE(retired.has_value());
        PLR_EXPECT_MSG(retired.value().id.str() == id, context);
      }
    }
  }

  // Child order matches the independently derived order, for every parent.
  std::vector<std::optional<std::string>> parents;
  parents.emplace_back(std::nullopt);
  for (const std::string& id : state.reference.all_ids()) {
    parents.emplace_back(id);
  }
  for (const std::optional<std::string>& parent : parents) {
    const std::vector<std::string> expected_children = state.reference.children_of(parent);
    std::vector<std::string> actual_children;
    if (!parent.has_value()) {
      const auto roots = snapshot->roots();
      PLR_REQUIRE(roots.has_value());
      for (const ChildEntry& entry : roots.value()) {
        actual_children.push_back(entry.id.str());
      }
    } else {
      const auto parsed_parent = LocationId::parse(parent.value());
      PLR_REQUIRE(parsed_parent.has_value());
      const auto children = snapshot->children(parsed_parent.value());
      PLR_REQUIRE(children.has_value());
      for (const ChildEntry& entry : children.value()) {
        actual_children.push_back(entry.id.str());
      }
    }
    PLR_EXPECT_MSG(actual_children == expected_children, context);
  }
}

/// Re-encodes, reloads and compares: canonical state must survive a round trip.
void verify_durability(PropertyState& state, const char* context) {
  const auto bytes = state.registry->encode_current_state();
  PLR_REQUIRE(bytes.has_value());
  const auto decoded = decode_snapshot(bytes.value());
  PLR_REQUIRE(decoded.has_value());
  const auto reencoded = encode_snapshot(decoded.value());
  PLR_REQUIRE(reencoded.has_value());
  PLR_EXPECT_MSG(reencoded.value() == bytes.value(), context);
  const auto digest = state.registry->copy_snapshot()->canonical_digest();
  PLR_REQUIRE(digest.has_value());
  PLR_EXPECT_MSG(digest.value() == state.registry->state_digest(), context);
}

/// Re-encodes, reloads and compares: canonical state must survive a round trip.

/// Re-encodes, reloads and compares: canonical state must survive a round trip.
void verify_durability_of(const std::shared_ptr<Registry>& registry) {
  const auto bytes = registry->encode_current_state();
  PLR_REQUIRE(bytes.has_value());
  const auto decoded = decode_snapshot(bytes.value());
  PLR_REQUIRE(decoded.has_value());
  PLR_EXPECT_EQ(decoded.value().location_count(), registry->statistics().locations);
  const auto reencoded = encode_snapshot(decoded.value());
  PLR_REQUIRE(reencoded.has_value());
  PLR_EXPECT(reencoded.value() == bytes.value());
}

void run_valid_sequence(std::uint64_t seed, int operations) {
  TempDir dir;
  auto seeded_options = writer_options();
  Limits limits;
  limits.max_moves_per_location = 64;
  limits.max_total_moves = 4096;
  limits.max_aliases_per_location = 4;
  limits.max_total_aliases = 4096;
  seeded_options.limits = limits;
  PLR_EXPECT_OK(registry, Registry::create(dir.path(), seeded_options));

  PropertyState state{registry, Fixture(registry, seed), ReferenceModel{}, Rng(seed), 0};

  // A facility is the only parentless kind.
  PLR_REQUIRE(create_location(state, std::string(), LocationKind::Facility, "Facility"));

  const std::vector<std::string> labels = {"plain", "R\xC3\xA9seau", "\xE6\x9C\xBA\xE6\x88\xBF",
                                           "\xF0\x9F\x9A\x80 unit", std::string(120, 'L')};

  for (int step = 0; step < operations; ++step) {
    const std::uint64_t before_revision = state.registry->revision().value();
    const std::vector<std::string> ids = state.reference.all_ids();
    const std::string target = ids[state.rng.below(static_cast<std::uint32_t>(ids.size()))];
    const ReferenceModel::Node* node = state.reference.find(target);
    PLR_REQUIRE(node != nullptr);
    const int choice = static_cast<int>(state.rng.below(100));

    if (choice < 30 && node->lifecycle == LifecycleState::Active) {
      // Rename the human label: address and identity are untouched.
      const std::string label = labels[state.rng.below(static_cast<std::uint32_t>(labels.size()))];
      auto request = RelabelRequest::make(target, label, state.fixture.context());
      PLR_REQUIRE(request.has_value());
      const auto receipt = state.registry->relabel(request.value());
      if (receipt.has_value()) {
        PLR_EXPECT_EQ(receipt.value().revision.value(), before_revision + 1U);
        state.reference.find(target)->label = label;
      } else {
        // Repeating the current label is refused rather than republished.
        PLR_EXPECT_EQ(receipt.error().code(), ErrorCode::NoOpMutation);
        PLR_EXPECT_EQ(state.registry->revision().value(), before_revision);
      }
    } else if (choice < 55) {
      // Create a legal child, if the schema allows one.
      const std::vector<LocationKind> kinds = legal_children(node->kind);
      if (!kinds.empty()) {
        const LocationKind kind = kinds[state.rng.below(static_cast<std::uint32_t>(kinds.size()))];
        const bool created = create_location(state, target, kind,
                                            labels[state.rng.below(static_cast<std::uint32_t>(
                                                labels.size()))]);
        if (created) {
          PLR_EXPECT_EQ(state.registry->revision().value(), before_revision + 1U);
        } else {
          // A creation is only generated for a legal parent kind, so a refusal
          // here means a bound was reached and nothing may have been published.
          PLR_EXPECT_EQ(state.registry->revision().value(), before_revision);
        }
      }
    } else if (choice < 70 && node->lifecycle == LifecycleState::Active &&
               node->kind != LocationKind::Facility) {
      // Readdress: the component changes, the identity and the subtree follow.
      const std::string component = state.fixture.next_component("R");
      auto request = ReaddressRequest::make(target, component, state.fixture.context());
      PLR_REQUIRE(request.has_value());
      const auto receipt = state.registry->readdress(request.value());
      if (receipt.has_value()) {
        PLR_EXPECT_EQ(state.registry->revision().value(), before_revision + 1U);
        state.reference.find(target)->component = component;
      }
    } else if (choice < 82 && node->lifecycle == LifecycleState::Active &&
               node->kind != LocationKind::Facility) {
      // Move under a different legal parent that is not inside the subtree.
      const std::vector<std::string> candidates = state.reference.all_ids();
      const std::string candidate =
          candidates[state.rng.below(static_cast<std::uint32_t>(candidates.size()))];
      const ReferenceModel::Node* candidate_node = state.reference.find(candidate);
      if (candidate_node != nullptr && candidate != target &&
          candidate_node->lifecycle == LifecycleState::Active) {
        const std::vector<LocationKind> kinds = legal_children(candidate_node->kind);
        if (std::find(kinds.begin(), kinds.end(), node->kind) != kinds.end()) {
          auto request = MoveLocationRequest::make(target, candidate, state.fixture.context());
          PLR_REQUIRE(request.has_value());
          const auto receipt = state.registry->move_location(request.value());
          if (receipt.has_value()) {
            PLR_EXPECT_EQ(state.registry->revision().value(), before_revision + 1U);
            state.reference.find(target)->parent = candidate;
          } else {
            // Only a cycle or an address clash may reject here.
            const ErrorCode code = receipt.error().code();
            PLR_EXPECT(code == ErrorCode::MoveIntoDescendant || code == ErrorCode::AddressInUse ||
                       code == ErrorCode::AddressLookAlike || code == ErrorCode::SelfMove ||
                       code == ErrorCode::NoOpMutation || code == ErrorCode::LimitExceeded);
          }
        }
      }
    } else if (choice < 90 && node->lifecycle == LifecycleState::Active) {
      // Retire a leaf, or a subtree when it has no active parent above it.
      auto request = RetireLocationRequest::make(target, SubtreeMode::LocationOnly,
                                                 state.fixture.context());
      PLR_REQUIRE(request.has_value());
      const auto receipt = state.registry->retire_location(request.value());
      if (receipt.has_value()) {
        PLR_EXPECT_EQ(state.registry->revision().value(), before_revision + 1U);
        state.reference.find(target)->lifecycle = LifecycleState::Retired;
      } else {
        PLR_EXPECT(receipt.error().code() == ErrorCode::LocationHasActiveChildren ||
                   receipt.error().code() == ErrorCode::LifecycleTransitionIllegal);
      }
    } else if (choice < 95 && node->lifecycle == LifecycleState::Retired) {
      // Reactivate only when the parent is active in both models.
      bool parent_active = true;
      if (node->parent.has_value()) {
        const ReferenceModel::Node* parent = state.reference.find(node->parent.value());
        parent_active = parent != nullptr && parent->lifecycle == LifecycleState::Active;
      }
      auto request = ReactivateLocationRequest::make(target, SubtreeMode::LocationOnly,
                                                     state.fixture.context());
      PLR_REQUIRE(request.has_value());
      const auto receipt = state.registry->reactivate_location(request.value());
      if (parent_active) {
        PLR_REQUIRE(receipt.has_value());
        state.reference.find(target)->lifecycle = LifecycleState::Active;
      } else {
        PLR_EXPECT(!receipt.has_value());
      }
    } else {
      // Bind an alias derived from the reference model's own path.
      if (node->lifecycle == LifecycleState::Active &&
          node->aliases.size() < limits.max_aliases_per_location) {
        const std::optional<std::string> path = state.reference.path_of(target);
        PLR_REQUIRE(path.has_value());
        const std::string alias =
            "/ALIAS-" + std::to_string(step) + "-" + std::to_string(state.serial) + path.value();
        auto request = AddAliasRequest::make(target, alias, state.registry->limits(),
                                             state.fixture.context());
        PLR_REQUIRE(request.has_value());
        const auto receipt = state.registry->add_alias(request.value());
        if (receipt.has_value()) {
          state.reference.find(target)->aliases.push_back(alias);
        } else {
          // The alias is unique per operation, so only a bound can refuse it.
          PLR_EXPECT_EQ(receipt.error().code(), ErrorCode::LimitExceeded);
          PLR_EXPECT_EQ(state.registry->revision().value(), before_revision);
        }
      }
    }

    if (step % 20 == 0) {
      verify_agreement(state, "during valid sequence");
    }
  }

  verify_agreement(state, "after valid sequence");
  verify_durability(state, "after valid sequence");
}

/// Operations that must be rejected, each leaving the committed state untouched.
void run_invalid_sequence(std::uint64_t seed, int operations) {
  TempDir dir;
  PLR_EXPECT_OK(registry, Registry::create(dir.path(), writer_options()));
  PropertyState state{registry, Fixture(registry, seed), ReferenceModel{}, Rng(seed), 0};
  PLR_REQUIRE(create_location(state, std::string(), LocationKind::Facility, "Facility"));
  const std::string facility = state.reference.all_ids()[0];
  PLR_REQUIRE(create_location(state, facility, LocationKind::Room, "Room"));
  const std::string room = state.reference.all_ids()[1];
  PLR_REQUIRE(create_location(state, room, LocationKind::Row, "Row"));
  const std::string row = state.reference.all_ids()[2];

  for (int step = 0; step < operations; ++step) {
    const std::uint64_t before_revision = state.registry->revision().value();
    const std::string before_digest = state.registry->state_digest();
    const int choice = static_cast<int>(state.rng.below(100));
    const auto context = state.fixture.context();

    if (choice < 15) {
      // Duplicate identity with a fresh component.
      auto request = CreateLocationRequest::make(room, LocationKind::Row, facility,
                                                 state.fixture.next_component("D"), "", context);
      PLR_REQUIRE(request.has_value());
      PLR_EXPECT_ERR(registry->create_location(request.value()), ErrorCode::AlreadyPresent);
    } else if (choice < 30) {
      // A kind that cannot be contained by the parent.
      auto request = CreateLocationRequest::make("loc-bad-" + std::to_string(step),
                                                 LocationKind::RackUnit, facility,
                                                 state.fixture.next_component("K"), "", context);
      PLR_REQUIRE(request.has_value());
      auto coordinate = RackUnitCoordinate::parse(1);
      PLR_REQUIRE(coordinate.has_value());
      request.value().unit = coordinate.value();
      PLR_EXPECT_ERR(registry->create_location(request.value()), ErrorCode::InvalidKindForParent);
    } else if (choice < 45) {
      // A parent that does not exist.
      auto request = CreateLocationRequest::make("loc-missing-" + std::to_string(step),
                                                 LocationKind::Row, "loc-absent",
                                                 state.fixture.next_component("M"), "", context);
      PLR_REQUIRE(request.has_value());
      PLR_EXPECT_ERR(registry->create_location(request.value()), ErrorCode::NotFound);
    } else if (choice < 60) {
      // A sibling component that collides with an existing one.
      auto existing = registry->children(LocationId::parse(facility).value());
      PLR_REQUIRE(existing.has_value());
      if (!existing.value().empty()) {
        auto request = CreateLocationRequest::make("loc-clash-" + std::to_string(step),
                                                   LocationKind::Room, facility,
                                                   existing.value()[0].component, "", context);
        PLR_REQUIRE(request.has_value());
        PLR_EXPECT_ERR(registry->create_location(request.value()), ErrorCode::AddressInUse);
      }
    } else if (choice < 75) {
      // A stale generation for a location that has moved on.
      PLR_EXPECT_OK(view, registry->find(LocationId::parse(room).value()));
      auto request = RelabelRequest::make(room, "stale", state.fixture.context_with(
                                                        LocationGeneration(view.generation().value() + 7U)));
      PLR_REQUIRE(request.has_value());
      PLR_EXPECT_ERR(registry->relabel(request.value()), ErrorCode::StaleGeneration);
    } else if (choice < 85) {
      // A stale revision.
      auto request = CreateLocationRequest::make("loc-rev-" + std::to_string(step), LocationKind::Row,
                                                 room, state.fixture.next_component("V"), "",
                                                 state.fixture.context_with(
                                                     std::nullopt, LocationRevision(UINT64_MAX)));
      PLR_REQUIRE(request.has_value());
      PLR_EXPECT_ERR(registry->create_location(request.value()), ErrorCode::StaleRevision);
    } else if (choice < 95) {
      // Moving a node into itself or into its own subtree. A facility is refused
      // earlier as a root, so these use the room and its own child.
      auto request = MoveLocationRequest::make(room, room, context);
      PLR_REQUIRE(request.has_value());
      PLR_EXPECT_ERR(registry->move_location(request.value()), ErrorCode::SelfMove);
      auto deeper = MoveLocationRequest::make(room, row, context);
      PLR_REQUIRE(deeper.has_value());
      PLR_EXPECT_ERR(registry->move_location(deeper.value()), ErrorCode::MoveIntoDescendant);
    } else {
      // A malformed path for an alias.
      const std::vector<std::string> hostile = {"..", "/..", "relative", "/C:/x", "/a b",
                                                std::string(5000, 'z')};
      const std::string text = hostile[state.rng.below(
          static_cast<std::uint32_t>(hostile.size()))];
      auto request = AddAliasRequest::make(room, text, registry->limits(), context);
      PLR_EXPECT(!request.has_value());
    }

    PLR_EXPECT_EQ(registry->revision().value(), before_revision);
    PLR_EXPECT_EQ(registry->state_digest(), before_digest);
  }

  verify_agreement(state, "after invalid sequence");
  verify_durability(state, "after invalid sequence");
}

}  // namespace

PLR_TEST(property, valid_operation_sequences_match_the_reference_model) {
  const std::uint64_t seeds[] = {1, 42, 1337, 20260101};
  for (const std::uint64_t seed : seeds) {
    run_valid_sequence(seed, 120);
  }
}

PLR_TEST(property, rejected_operations_never_change_committed_state) {
  const std::uint64_t seeds[] = {2, 43, 1338, 20260102};
  for (const std::uint64_t seed : seeds) {
    run_invalid_sequence(seed, 120);
  }
}

PLR_TEST(property, serialization_is_stable_across_reopens) {
  TempDir dir;
  std::string digest_before;
  std::string bytes_before;
  {
    PLR_EXPECT_OK(registry, Registry::create(dir.path(), writer_options()));
    Fixture fixture(registry, 31337);
    PLR_EXPECT_OK(all, fixture.standard_facility("FAC1", 2, 2, 2, 2));
    (void)all;
    const Limits limits = registry->limits();
    auto alias = AddAliasRequest::make(all[10].str(), "/LEGACY/ONE", limits, fixture.context());
    PLR_REQUIRE(alias.has_value());
    PLR_EXPECT_OK(alias_receipt, registry->add_alias(alias.value()));
    (void)alias_receipt;
    PLR_EXPECT_OK(bytes, registry->encode_current_state());
    bytes_before = bytes;
    digest_before = registry->state_digest();
    PLR_EXPECT(registry->close().has_value());
  }

  for (int round = 0; round < 3; ++round) {
    PLR_EXPECT_OK(reader, Registry::open(dir.path(), reader_options()));
    PLR_EXPECT_EQ(reader->state_digest(), digest_before);
    PLR_EXPECT_OK(bytes, reader->encode_current_state());
    PLR_EXPECT(bytes == bytes_before);
    PLR_EXPECT_EQ(reader->statistics().locations, 32U);
    PLR_EXPECT_EQ(reader->statistics().aliases, 1U);
    // The decoded snapshot resolves the alias exactly as the writer did.
    PLR_EXPECT_OK(alias_path, LocationPath::parse("/LEGACY/ONE", reader->limits()));
    PLR_EXPECT_OK(resolved, reader->resolve(alias_path));
    PLR_EXPECT_EQ(resolved.kind, ResolutionKind::Alias);
    PLR_EXPECT(reader->close().has_value());
  }
}

PLR_TEST(property, deep_and_wide_hierarchies_stay_consistent) {
  TempDir dir;
  PLR_EXPECT_OK(registry, Registry::create(dir.path(), writer_options()));
  Fixture fixture(registry, 777);
  PLR_EXPECT_OK(facility, fixture.facility("FAC1"));

  // Deep: the longest legal chain, then a rename at every level.
  std::vector<LocationId> chain;
  chain.push_back(facility);
  chain.push_back(fixture.add(facility, LocationKind::Building, "B1").value());
  chain.push_back(fixture.add(chain.back(), LocationKind::Hall, "H1").value());
  chain.push_back(fixture.add(chain.back(), LocationKind::Room, "M1").value());
  chain.push_back(fixture.add(chain.back(), LocationKind::Zone, "Z1").value());
  chain.push_back(fixture.add(chain.back(), LocationKind::Row, "W1").value());
  chain.push_back(fixture.add_rack(chain.back(), "K1", 48).value());
  chain.push_back(fixture.add_unit(chain.back(), 1).value());
  PLR_EXPECT_EQ(chain.size(), std::size_t{8});
  PLR_EXPECT_EQ(registry->statistics().max_depth_observed, 8U);

  for (std::size_t index = 1; index < chain.size(); ++index) {
    auto request = ReaddressRequest::make(chain[index].str(), "RENAMED-" + std::to_string(index),
                                          fixture.context());
    PLR_REQUIRE(request.has_value());
    PLR_EXPECT_OK(receipt, registry->readdress(request.value()));
    (void)receipt;
  }
  PLR_EXPECT_EQ(registry->statistics().max_depth_observed, 8U);
  PLR_EXPECT_OK(deepest, registry->path_of(chain.back()));
  PLR_EXPECT_EQ(deepest.to_string(),
                std::string("/FAC1/RENAMED-1/RENAMED-2/RENAMED-3/RENAMED-4/RENAMED-5/RENAMED-6/"
                            "RENAMED-7"));
  PLR_EXPECT_OK(resolved, registry->resolve(deepest));
  PLR_EXPECT(resolved.id == chain.back());

  // Wide: many siblings, each independently addressable and ordered byte-wise.
  PLR_EXPECT_OK(wide, fixture.add(facility, LocationKind::Room, "WIDE"));
  for (int index = 0; index < 200; ++index) {
    const auto added = fixture.add(wide, LocationKind::Row, "R" + std::to_string(index));
    PLR_REQUIRE(added.has_value());
  }
  PLR_EXPECT_OK(children, registry->children(wide));
  PLR_REQUIRE(children.size() == 200);
  for (std::size_t index = 1; index < children.size(); ++index) {
    PLR_EXPECT(children[index - 1].component < children[index].component);
  }
  PLR_EXPECT_OK(descendants, registry->descendants(wide, 1));
  PLR_EXPECT_EQ(descendants.size(), std::size_t{200});
  verify_durability_of(registry);
}

