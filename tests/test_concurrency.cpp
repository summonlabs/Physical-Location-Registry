// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Concurrency: many threads share one registry, the documented locking model
// holds under contention, cancelled work never publishes, and closing a session
// while work is in flight leaves an exact, accounted-for state.

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
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

MutationContext thread_context(const ActorId& actor, std::uint64_t tick) {
  MutationContext context;
  context.actor = actor;
  const auto at = Timestamp::from_unix_seconds(static_cast<std::int64_t>(1700000000ULL + tick));
  context.at = at.value();
  return context;
}

}  // namespace

PLR_TEST(concurrency, readers_observe_whole_committed_revisions) {
  TempDir dir;
  PLR_EXPECT_OK(registry, Registry::create(dir.path(), writer_options()));
  Fixture fixture(registry, 701);
  PLR_EXPECT_OK(facility, fixture.facility("FAC1"));
  PLR_EXPECT_OK(room_zero, fixture.add(facility, LocationKind::Room, "ROOM-0"));
  (void)room_zero;

  std::atomic<bool> stop{false};
  std::atomic<int> violations{0};
  std::atomic<std::uint64_t> reads{0};

  std::vector<std::thread> readers;
  for (int index = 0; index < 4; ++index) {
    readers.emplace_back([&]() {
      while (!stop.load(std::memory_order_relaxed)) {
        const std::shared_ptr<const Snapshot> snapshot = registry->copy_snapshot();
        if (snapshot == nullptr) {
          violations.fetch_add(1);
          return;
        }
        // Every listed location must resolve back to itself, and its path must
        // be consistent with its parent chain: a torn revision would break one
        // of these.
        const auto listed = snapshot->list(ListOptions{});
        if (!listed.has_value()) {
          violations.fetch_add(1);
          continue;
        }
        for (const LocationView& view : listed.value()) {
          const auto address = LocationPath::parse(view.path().to_string(), snapshot->limits());
          if (!address.has_value()) {
            violations.fetch_add(1);
            continue;
          }
          const auto resolved = snapshot->resolve(address.value());
          if (!resolved.has_value() || resolved.value().id != view.id()) {
            violations.fetch_add(1);
          }
          if (view.parent().has_value()) {
            const auto parent_view = snapshot->find(view.parent().value());
            if (!parent_view.has_value()) {
              violations.fetch_add(1);
            } else if (!parent_view.value().path().is_ancestor_of(view.path())) {
              violations.fetch_add(1);
            }
          }
        }
        reads.fetch_add(1, std::memory_order_relaxed);
      }
    });
  }

  // Concurrent writer: create rooms, rename them, and retire every other one.
  const auto actor = ActorId::parse("actor-writer");
  PLR_REQUIRE(actor.has_value());
  std::uint32_t accepted = 0;
  for (int index = 1; index <= 60; ++index) {
    auto request = CreateLocationRequest::make("loc-room-" + std::to_string(index),
                                               LocationKind::Room, facility.str(),
                                               "ROOM-" + std::to_string(index), "",
                                               thread_context(actor.value(), static_cast<std::uint64_t>(index)));
    PLR_REQUIRE(request.has_value());
    const auto receipt = registry->create_location(request.value());
    if (receipt.has_value()) {
      ++accepted;
    }
    if (index % 5 == 0) {
      auto relabel = RelabelRequest::make("loc-room-" + std::to_string(index),
                                          "renamed-" + std::to_string(index),
                                          thread_context(actor.value(), static_cast<std::uint64_t>(index)));
      if (relabel.has_value()) {
        (void)registry->relabel(relabel.value());
      }
    }
  }
  stop.store(true);
  for (std::thread& thread : readers) {
    thread.join();
  }

  PLR_EXPECT_EQ(violations.load(), 0);
  PLR_EXPECT(reads.load() > 0U);
  PLR_EXPECT_EQ(registry->statistics().locations, accepted + 2U);
  PLR_EXPECT(registry->revision().value() >= accepted);
  PLR_EXPECT(registry->verify_storage().has_value());
}

PLR_TEST(concurrency, concurrent_mutations_are_serialized_and_accounted_for) {
  TempDir dir;
  PLR_EXPECT_OK(registry, Registry::create(dir.path(), writer_options()));
  Fixture fixture(registry, 709);
  PLR_EXPECT_OK(facility, fixture.facility("FAC1"));

  constexpr int kThreads = 4;
  constexpr int kPerThread = 25;
  std::atomic<int> accepted{0};
  std::atomic<int> rejected{0};
  std::vector<std::thread> writers;

  for (int thread_index = 0; thread_index < kThreads; ++thread_index) {
    writers.emplace_back([&, thread_index]() {
      const auto actor = ActorId::parse("actor-" + std::to_string(thread_index));
      if (!actor.has_value()) {
        rejected.fetch_add(kPerThread);
        return;
      }
      for (int index = 0; index < kPerThread; ++index) {
        const int ordinal = thread_index * kPerThread + index;
        auto request = CreateLocationRequest::make(
            "loc-t" + std::to_string(thread_index) + "-" + std::to_string(index), LocationKind::Room,
            facility.str(), "ROOM-" + std::to_string(ordinal), "",
            thread_context(actor.value(), static_cast<std::uint64_t>(ordinal)));
        if (!request.has_value()) {
          rejected.fetch_add(1);
          continue;
        }
        const auto receipt = registry->create_location(request.value());
        if (receipt.has_value()) {
          accepted.fetch_add(1);
        } else {
          rejected.fetch_add(1);
        }
      }
    });
  }
  for (std::thread& thread : writers) {
    thread.join();
  }

  PLR_EXPECT_EQ(accepted.load(), kThreads * kPerThread);
  PLR_EXPECT_EQ(rejected.load(), 0);
  // Every accepted mutation advanced the revision by exactly one, and the
  // initial facility is the only other publication.
  PLR_EXPECT_EQ(registry->revision().value(), static_cast<std::uint64_t>(accepted.load()) + 1U);
  PLR_EXPECT_EQ(registry->statistics().locations,
                static_cast<std::uint32_t>(accepted.load()) + 1U);
  PLR_EXPECT_EQ(registry->statistics().active,
                static_cast<std::uint32_t>(accepted.load()) + 1U);

  // Children are unique and addressable, with no duplicates or lost updates.
  PLR_EXPECT_OK(children, registry->children(facility));
  PLR_EXPECT_EQ(children.size(), static_cast<std::size_t>(accepted.load()));
  for (std::size_t index = 1; index < children.size(); ++index) {
    PLR_EXPECT(children[index - 1].component < children[index].component);
  }
  PLR_EXPECT(registry->verify_storage().has_value());
}

PLR_TEST(concurrency, snapshot_isolation_holds_while_the_writer_moves_on) {
  TempDir dir;
  PLR_EXPECT_OK(registry, Registry::create(dir.path(), writer_options()));
  Fixture fixture(registry, 719);
  PLR_EXPECT_OK(facility, fixture.facility("FAC1"));
  PLR_EXPECT_OK(room, fixture.add(facility, LocationKind::Room, "ROOM-1"));

  const std::shared_ptr<const Snapshot> frozen = registry->copy_snapshot();
  PLR_REQUIRE(frozen != nullptr);
  const LocationRevision frozen_revision = frozen->revision();

  // Mutate heavily after the snapshot was taken.
  for (int index = 0; index < 20; ++index) {
    PLR_EXPECT_OK(added, fixture.add(facility, LocationKind::Room,
                                     "ROOM-" + std::to_string(index + 2)));
    (void)added;
  }
  auto rename = RelabelRequest::make(room.str(), "renamed after snapshot", fixture.context());
  PLR_REQUIRE(rename.has_value());
  PLR_EXPECT_OK(receipt, registry->relabel(rename.value()));
  (void)receipt;

  // The frozen snapshot still answers from the revision it was taken at.
  PLR_EXPECT(frozen->revision() == frozen_revision);
  PLR_EXPECT_EQ(frozen->location_count(), 2U);
  PLR_EXPECT_OK(old_view, frozen->find(room));
  PLR_EXPECT_EQ(old_view.label(), std::string(""));
  PLR_EXPECT_EQ(registry->statistics().locations, 22U);
  PLR_EXPECT_OK(new_view, registry->find(room));
  PLR_EXPECT_EQ(new_view.label(), std::string("renamed after snapshot"));
  PLR_EXPECT(frozen->revision() < registry->revision());
  PLR_EXPECT_OK(frozen_digest, frozen->canonical_digest());
  PLR_EXPECT_NE(frozen_digest, registry->state_digest());
}

PLR_TEST(concurrency, cancellation_from_another_thread_is_bounded_and_safe) {
  TempDir dir;
  PLR_EXPECT_OK(registry, Registry::create(dir.path(), writer_options()));
  Fixture fixture(registry, 727);
  PLR_EXPECT_OK(facility, fixture.facility("FAC1"));

  // A stop request raised before the mutation is submitted always prevents
  // publication; one raised during it either prevents publication or leaves a
  // fully committed mutation, never a partial one.
  std::atomic<int> published{0};
  std::atomic<int> cancelled{0};
  std::vector<std::thread> workers;
  for (int index = 0; index < 8; ++index) {
    workers.emplace_back([&, index]() {
      // The stop source is shared with the canceller rather than captured by
      // reference, and the canceller is joined: no thread can outlive the state
      // it touches.
      auto source = std::make_shared<std::stop_source>();
      auto context = thread_context(fixture.actor(), static_cast<std::uint64_t>(index));
      context.stop = source->get_token();
      std::thread canceller;
      if (index % 2 == 0) {
        source->request_stop();
      } else {
        canceller = std::thread([source]() {
          std::this_thread::sleep_for(std::chrono::microseconds(50));
          source->request_stop();
        });
      }
      auto request = CreateLocationRequest::make("loc-c" + std::to_string(index),
                                                 LocationKind::Room, facility.str(),
                                                 "ROOM-" + std::to_string(index), "", context);
      if (request.has_value()) {
        const auto receipt = registry->create_location(request.value());
        if (receipt.has_value()) {
          published.fetch_add(1);
        } else if (receipt.error().code() == ErrorCode::Cancelled) {
          cancelled.fetch_add(1);
        }
      }
      if (canceller.joinable()) {
        canceller.join();
      }
    });
  }
  for (std::thread& thread : workers) {
    thread.join();
  }

  PLR_EXPECT_EQ(published.load() + cancelled.load(), 8);
  // Even-indexed workers were cancelled before submission and never published.
  PLR_EXPECT(cancelled.load() >= 4);
  PLR_EXPECT_EQ(registry->statistics().locations,
                static_cast<std::uint32_t>(published.load()) + 1U);
  PLR_EXPECT(registry->verify_storage().has_value());

  // A cancelled mutation leaves nothing behind: the same request without a
  // cancelled token still succeeds.
  auto retry = CreateLocationRequest::make("loc-c0", LocationKind::Room, facility.str(), "ROOM-0",
                                           "",
                                           thread_context(fixture.actor(), 99));
  PLR_REQUIRE(retry.has_value());
  PLR_EXPECT_OK(receipt, registry->create_location(retry.value()));
  (void)receipt;
}

PLR_TEST(concurrency, close_stops_new_work_and_keeps_committed_work) {
  TempDir dir;
  PLR_EXPECT_OK(registry, Registry::create(dir.path(), writer_options()));
  Fixture fixture(registry, 733);
  PLR_EXPECT_OK(facility, fixture.facility("FAC1"));

  std::atomic<int> published{0};
  std::atomic<int> refused{0};
  std::atomic<bool> start{false};
  std::vector<std::thread> writers;
  for (int index = 0; index < 4; ++index) {
    writers.emplace_back([&, index]() {
      while (!start.load()) {
        std::this_thread::yield();
      }
      for (int step = 0; step < 40; ++step) {
        auto request = CreateLocationRequest::make(
            "loc-w" + std::to_string(index) + "-" + std::to_string(step), LocationKind::Room,
            facility.str(), "ROOM-" + std::to_string(index * 100 + step), "",
            thread_context(fixture.actor(), static_cast<std::uint64_t>(step)));
        if (!request.has_value()) {
          refused.fetch_add(1);
          continue;
        }
        const auto receipt = registry->create_location(request.value());
        if (receipt.has_value()) {
          published.fetch_add(1);
        } else if (receipt.error().code() == ErrorCode::SessionClosed ||
                   receipt.error().code() == ErrorCode::SessionReadOnly) {
          refused.fetch_add(1);
        }
      }
    });
  }
  start.store(true);
  std::this_thread::sleep_for(std::chrono::milliseconds(1));
  PLR_EXPECT(registry->close().has_value());
  for (std::thread& thread : writers) {
    thread.join();
  }

  const std::uint64_t committed_revision = registry->revision().value();
  PLR_EXPECT_EQ(committed_revision, static_cast<std::uint64_t>(published.load()) + 1U);
  PLR_EXPECT_EQ(registry->statistics().locations,
                static_cast<std::uint32_t>(published.load()) + 1U);
  PLR_EXPECT(refused.load() > 0);

  // Everything published before the close is on disk and it is complete.
  PLR_EXPECT_OK(reader, Registry::open(dir.path(), reader_options()));
  PLR_EXPECT_EQ(reader->revision().value(), committed_revision);
  PLR_EXPECT_EQ(reader->statistics().locations,
                static_cast<std::uint32_t>(published.load()) + 1U);
  PLR_EXPECT(reader->verify_storage().has_value());
  // No further publication happens after the close.
  PLR_EXPECT(registry->relabel(RelabelRequest::make(facility.str(), "late", fixture.context())
                                   .value())
                 .error()
                 .code() == ErrorCode::SessionClosed);
  // Publishing from a closed session is refused: nothing new may be committed.
  PLR_EXPECT_ERR(registry->encode_current_state(), ErrorCode::SessionClosed);
  PLR_EXPECT_ERR(registry->relabel(RelabelRequest::make(facility.str(), "later", fixture.context())
                                       .value()),
                 ErrorCode::SessionClosed);
}

PLR_TEST(concurrency, repeated_open_and_close_is_stable) {
  TempDir dir;
  StoreId store_id;
  for (int round = 0; round < 6; ++round) {
    PLR_EXPECT_OK(registry, Registry::open(dir.path(), writer_options()));
    if (round == 0) {
      store_id = registry->store_id();
      Fixture fixture(registry, 739);
      PLR_EXPECT_OK(created, fixture.facility("FAC1"));
      (void)created;
    } else {
      PLR_EXPECT(registry->store_id() == store_id);
      PLR_EXPECT_EQ(registry->epoch().value(), static_cast<std::uint64_t>(round + 1));
      PLR_EXPECT_EQ(registry->statistics().locations, 1U);
      PLR_EXPECT(registry->verify_storage().has_value());
    }
    PLR_EXPECT(registry->close().has_value());
    // The lock is released, so the next round can take it immediately.
  }
  PLR_EXPECT_OK(reader, Registry::open(dir.path(), reader_options()));
  PLR_EXPECT_EQ(reader->statistics().locations, 1U);
  PLR_EXPECT(reader->store_id() == store_id);
}

PLR_TEST(concurrency, many_handles_share_one_store_without_interference) {
  TempDir dir;
  PLR_EXPECT_OK(writer, Registry::create(dir.path(), writer_options()));
  Fixture fixture(writer, 743);
  PLR_EXPECT_OK(facility, fixture.facility("FAC1"));

  // Several read-only handles may coexist with the writer and with each other.
  std::vector<std::shared_ptr<Registry>> readers;
  for (int index = 0; index < 4; ++index) {
    PLR_EXPECT_OK(handle, Registry::open(dir.path(), reader_options()));
    readers.push_back(handle);
  }
  for (const auto& handle : readers) {
    PLR_EXPECT_EQ(handle->statistics().locations, 1U);
    PLR_EXPECT(handle->verify_storage().has_value());
  }

  // A reader opened before a mutation keeps answering from its own snapshot,
  // while a freshly opened one sees the new revision.
  PLR_EXPECT_OK(room, fixture.add(facility, LocationKind::Room, "ROOM-1"));
  (void)room;
  PLR_EXPECT_EQ(readers[0]->statistics().locations, 1U);
  PLR_EXPECT_OK(fresh, Registry::open(dir.path(), reader_options()));
  PLR_EXPECT_EQ(fresh->statistics().locations, 2U);
  PLR_EXPECT(fresh->revision() > readers[0]->revision());
}
