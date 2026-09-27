// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Deterministic fixture builder shared by the tests and the benchmarks. It only
// uses the public API, so a fixture proves the API is usable, not merely that
// internal state can be poked into shape.

#ifndef PLR_TESTS_SUPPORT_SYNTHETIC_HPP
#define PLR_TESTS_SUPPORT_SYNTHETIC_HPP

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "dccp/physical_location_registry/physical_location_registry.hpp"
#include "support/test_harness.hpp"

namespace plr_test {

using namespace dccp::physical_location_registry;

/// Builds location hierarchies through Registry and hands out deterministic
/// contexts, components and timestamps.
class Fixture {
 public:
  explicit Fixture(std::shared_ptr<Registry> registry, std::uint64_t seed = 1);

  Registry& registry() { return *registry_; }
  const std::shared_ptr<Registry>& registry_ptr() const { return registry_; }

  const ActorId& actor() const noexcept { return actor_; }
  std::uint64_t seed() const noexcept { return seed_; }

  /// A context with the fixture actor and a strictly increasing timestamp.
  MutationContext context() const;
  MutationContext context_with(std::optional<LocationGeneration> generation,
                              std::optional<LocationRevision> revision = std::nullopt) const;
  MutationContext context_with_operation(std::string_view operation_id) const;

  /// Deterministic unique address component.
  std::string next_component(std::string_view prefix);

  Result<LocationId> facility(std::string_view component, std::string_view label = {});
  Result<LocationId> add(const LocationId& parent, LocationKind kind, std::string_view component,
                         std::string_view label = {});
  Result<LocationId> add_rack(const LocationId& parent, std::string_view component,
                              std::uint32_t height, std::string_view label = {});
  Result<LocationId> add_unit(const LocationId& rack, std::uint32_t unit, std::string_view label = {});

  /// A facility with the given shape: rooms -> rows -> racks -> units.
  Result<std::vector<LocationId>> standard_facility(std::string_view facility_component,
                                                    std::uint32_t rooms, std::uint32_t rows,
                                                    std::uint32_t racks, std::uint32_t units);

  /// Every id created by this fixture, in creation order.
  const std::vector<LocationId>& created() const noexcept { return created_; }

 private:
  std::shared_ptr<Registry> registry_;
  ActorId actor_;
  std::uint64_t seed_;
  mutable std::uint64_t counter_ = 0;
  mutable std::uint64_t tick_ = 0;
  std::vector<LocationId> created_;
};

/// Builds a CreateLocationRequest with the fixture's context and provenance.
Result<CreateLocationRequest> make_create(std::string_view id, LocationKind kind,
                                          std::string_view parent, std::string_view component,
                                          std::string_view label, const MutationContext& context);

}  // namespace plr_test

#endif  // PLR_TESTS_SUPPORT_SYNTHETIC_HPP
