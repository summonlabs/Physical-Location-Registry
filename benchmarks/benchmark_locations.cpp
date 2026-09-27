// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Benchmarks for the Physical Location Registry.
//
// Every measurement is a completed operation: mutations include their durable
// publication (write, flush, verify, atomic head switch), and reads measure the
// finished answer. The scale, the seed and the integrity result are printed with
// the numbers so any run can be reproduced and compared.
//
// These numbers describe this machine and this build. They are not a claim about
// accelerator, network or facility hardware: no such hardware is involved.

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "dccp/physical_location_registry/physical_location_registry.hpp"
#include "support/synthetic.hpp"

namespace {

using namespace dccp::physical_location_registry;
using plr_test::Fixture;

class ScratchStore {
 public:
  explicit ScratchStore(std::string_view name) {
    std::error_code error;
    const auto base = std::filesystem::temp_directory_path(error);
    path_ = (error ? std::filesystem::path(".") : base) / ("plr-benchmark-" + std::string(name));
    std::filesystem::remove_all(path_, error);
  }
  ~ScratchStore() {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
  }
  const std::filesystem::path& path() const { return path_; }

 private:
  std::filesystem::path path_;
};

struct Measurement {
  std::string operation;
  std::uint64_t completed = 0;
  double total_ms = 0.0;
  std::string note;
};

void report(const Measurement& measurement) {
  const double per_operation_us =
      measurement.completed == 0 ? 0.0 : (measurement.total_ms * 1000.0) /
                                           static_cast<double>(measurement.completed);
  std::cout << std::left << std::setw(46) << measurement.operation << std::right << std::setw(12)
            << measurement.completed << std::setw(12) << std::fixed << std::setprecision(2)
            << measurement.total_ms << std::setw(14) << std::setprecision(3) << per_operation_us;
  if (!measurement.note.empty()) {
    std::cout << "  " << measurement.note;
  }
  std::cout << std::endl;
}

struct Options {
  std::uint32_t rooms = 2;
  std::uint32_t rows = 2;
  std::uint32_t racks = 4;
  std::uint32_t units = 8;
  std::uint32_t mutations = 200;
  std::uint32_t resolves = 20000;
  std::uint64_t seed = 20260101;
  bool fsync = true;
};

Options parse_options(int argc, char** argv) {
  Options options;
  for (int index = 1; index < argc; ++index) {
    const std::string argument = argv[index];
    const auto value_of = [&](const char* name) -> std::optional<std::string> {
      const std::string prefix = std::string(name) + "=";
      if (argument.rfind(prefix, 0) == 0) {
        return argument.substr(prefix.size());
      }
      if (argument == name && index + 1 < argc) {
        return std::string(argv[++index]);
      }
      return std::nullopt;
    };
    const auto rooms = value_of("--rooms");
    const auto rows = value_of("--rows");
    const auto racks = value_of("--racks");
    const auto units = value_of("--units");
    const auto mutations = value_of("--mutations");
    const auto resolves = value_of("--resolves");
    const auto seed = value_of("--seed");
    if (rooms.has_value()) {
      options.rooms = static_cast<std::uint32_t>(std::stoul(*rooms));
    } else if (rows.has_value()) {
      options.rows = static_cast<std::uint32_t>(std::stoul(*rows));
    } else if (racks.has_value()) {
      options.racks = static_cast<std::uint32_t>(std::stoul(*racks));
    } else if (units.has_value()) {
      options.units = static_cast<std::uint32_t>(std::stoul(*units));
    } else if (mutations.has_value()) {
      options.mutations = static_cast<std::uint32_t>(std::stoul(*mutations));
    } else if (resolves.has_value()) {
      options.resolves = static_cast<std::uint32_t>(std::stoul(*resolves));
    } else if (seed.has_value()) {
      options.seed = std::stoull(*seed);
    } else if (argument == "--no-fsync") {
      options.fsync = false;
    }
  }
  return options;
}

}  // namespace

int main(int argc, char** argv) {
  const Options options = parse_options(argc, argv);
  const std::uint64_t expected_locations =
      2ULL + static_cast<std::uint64_t>(options.rooms) +
      static_cast<std::uint64_t>(options.rooms) * options.rows +
      static_cast<std::uint64_t>(options.rooms) * options.rows * options.racks +
      static_cast<std::uint64_t>(options.rooms) * options.rows * options.racks * options.units;

  std::cout << "physical_location_registry " << version_string() << " benchmark" << std::endl;
  std::cout << "build=" <<
#if defined(NDEBUG)
      "Release"
#else
      "Debug"
#endif
            << " seed=" << options.seed << " durability-fsync=" << (options.fsync ? "on" : "off")
            << std::endl;
  std::cout << "shape: rooms=" << options.rooms << " rows/room=" << options.rows
            << " racks/row=" << options.racks << " units/rack=" << options.units
            << " locations=" << expected_locations << std::endl;
  std::cout << "note: facility state only; no accelerator, network or facility hardware is used"
            << std::endl;
  std::cout << std::endl;

  std::cout << std::left << std::setw(46) << "operation" << std::right << std::setw(12) << "count"
            << std::setw(12) << "total-ms" << std::setw(14) << "per-op-us" << std::endl;

  ScratchStore scratch("main");
  std::vector<Measurement> measurements;

  RegistryOpenOptions open_options;
  open_options.mode = OpenMode::ReadWrite;
  open_options.create_if_missing = true;
  open_options.fsync_state_before_publish = options.fsync;
  open_options.fsync_directory_after_rename = options.fsync;
  Limits limits;
  limits.max_locations = static_cast<std::uint32_t>(expected_locations) + 1024U;
  limits.max_children_per_location = static_cast<std::uint32_t>(expected_locations) + 1024U;
  limits.max_total_moves = options.mutations * 4U + 1024U;
  limits.max_moves_per_location = options.mutations + 16U;
  limits.max_total_aliases = 4096U;
  limits.max_aliases_per_location = 64U;
  open_options.limits = limits;

  auto created = Registry::create(scratch.path(), open_options);
  if (!created.has_value()) {
    std::cerr << created.error().to_string() << std::endl;
    return 1;
  }
  std::shared_ptr<Registry> registry = created.value();
  Fixture fixture(registry, options.seed);

  // ---- build ---------------------------------------------------------------
  {
    const auto started = std::chrono::steady_clock::now();
    auto built = fixture.standard_facility("FAC1", options.rooms, options.rows, options.racks,
                                           options.units);
    const auto finished = std::chrono::steady_clock::now();
    if (!built.has_value()) {
      std::cerr << built.error().to_string() << std::endl;
      return 1;
    }
    const double total_ms =
        std::chrono::duration<double, std::milli>(finished - started).count();
    measurements.push_back({"create location (durably committed)", expected_locations - 1U, total_ms,
                            "hierarchy build"});
  }

  const auto roots = registry->roots();
  if (!roots.has_value() || roots.value().empty()) {
    std::cerr << "facility root missing" << std::endl;
    return 1;
  }
  const LocationId facility = roots.value()[0].id;

  // ---- reads ---------------------------------------------------------------
  const std::vector<LocationId>& ids = fixture.created();
  {
    std::uint64_t found = 0;
    const auto started = std::chrono::steady_clock::now();
    for (std::uint32_t index = 0; index < options.resolves; ++index) {
      const LocationId& id = ids[index % ids.size()];
      const auto view = registry->find(id);
      if (view.has_value()) {
        ++found;
      }
    }
    const auto finished = std::chrono::steady_clock::now();
    const double total_ms =
        std::chrono::duration<double, std::milli>(finished - started).count();
    measurements.push_back({"resolve by identity (find)", found, total_ms, "indexed by identity"});
  }

  {
    std::vector<std::string> addresses;
    addresses.reserve(ids.size());
    for (const LocationId& id : ids) {
      const auto path = registry->path_of(id);
      if (path.has_value()) {
        addresses.push_back(path.value().to_string());
      }
    }
    const Limits active_limits = registry->limits();
    std::uint64_t resolved = 0;
    const auto started = std::chrono::steady_clock::now();
    for (std::uint32_t index = 0; index < options.resolves; ++index) {
      const std::string& text = addresses[index % addresses.size()];
      const auto path = LocationPath::parse(text, active_limits);
      if (!path.has_value()) {
        continue;
      }
      const auto result = registry->resolve(path.value());
      if (result.has_value()) {
        ++resolved;
      }
    }
    const auto finished = std::chrono::steady_clock::now();
    const double total_ms =
        std::chrono::duration<double, std::milli>(finished - started).count();
    measurements.push_back({"resolve by canonical address", resolved, total_ms,
                            "walk from the facility root"});
  }

  {
    std::uint64_t traversed = 0;
    const std::uint32_t rounds = options.resolves / 100U + 1U;
    const auto started = std::chrono::steady_clock::now();
    for (std::uint32_t index = 0; index < rounds; ++index) {
      const auto descendants = registry->descendants(facility, 8);
      if (descendants.has_value()) {
        traversed += descendants.value().size();
      }
    }
    const auto finished = std::chrono::steady_clock::now();
    const double total_ms =
        std::chrono::duration<double, std::milli>(finished - started).count();
    measurements.push_back({"subtree traversal (whole facility)", traversed, total_ms,
                            "nodes visited, not calls"});
  }

  {
    std::uint64_t listed = 0;
    const std::uint32_t rounds = options.resolves / 1000U + 1U;
    const auto started = std::chrono::steady_clock::now();
    for (std::uint32_t index = 0; index < rounds; ++index) {
      ListOptions list_options;
      list_options.max_nodes = static_cast<std::uint32_t>(expected_locations) + 16U;
      const auto views = registry->list(list_options);
      if (views.has_value()) {
        listed += views.value().size();
      }
    }
    const auto finished = std::chrono::steady_clock::now();
    const double total_ms =
        std::chrono::duration<double, std::milli>(finished - started).count();
    measurements.push_back({"full listing in canonical order", listed, total_ms,
                            "locations returned"});
  }

  // ---- generation mutations (each durably committed) -----------------------
  {
    std::uint64_t committed = 0;
    double total_ms = 0.0;
    const std::size_t sample = static_cast<std::size_t>(options.mutations);
    for (std::size_t index = 0; index < sample; ++index) {
      const LocationId& id = ids[(index * 7U + 3U) % ids.size()];
      const auto current = registry->find(id);
      if (!current.has_value()) {
        continue;
      }
      MutationContext context;
      const auto actor = ActorId::parse("benchmark");
      context.actor = actor.value();
      context.at = Timestamp::from_unix_seconds(
          1767225600 + static_cast<std::int64_t>(index)).value();
      context.authority = registry->authority();
      context.expected_generation = current.value().generation();
      auto request = RelabelRequest::make(id.str(), "label-" + std::to_string(index), context);
      if (!request.has_value()) {
        continue;
      }
      const auto started = std::chrono::steady_clock::now();
      const auto receipt = registry->relabel(request.value());
      const auto finished = std::chrono::steady_clock::now();
      if (receipt.has_value()) {
        ++committed;
        total_ms += std::chrono::duration<double, std::milli>(finished - started).count();
      }
    }
    measurements.push_back({"relabel (durable commit per mutation)", committed, total_ms,
                            options.fsync ? "includes flush and atomic publish"
                                          : "flush disabled by request"});
  }

  {
    // Readdressing a row restamps every rack and unit beneath it, so this
    // measures a subtree address change rather than a single record.
    std::uint64_t committed = 0;
    std::uint64_t restamped = 0;
    double total_ms = 0.0;
    std::vector<LocationId> rows;
    for (const LocationId& id : ids) {
      const auto view = registry->find(id);
      if (view.has_value() && view.value().kind() == LocationKind::Row) {
        rows.push_back(id);
      }
    }
    const std::size_t sample = std::min<std::size_t>(rows.size(), 8U);
    for (std::size_t index = 0; index < sample; ++index) {
      const LocationId& row = rows[index];
      const auto current = registry->find(row);
      if (!current.has_value()) {
        continue;
      }
      MutationContext context;
      const auto actor = ActorId::parse("benchmark");
      context.actor = actor.value();
      context.at = Timestamp::from_unix_seconds(
          1767225600 + static_cast<std::int64_t>(index)).value();
      context.authority = registry->authority();
      context.expected_generation = current.value().generation();
      auto request = ReaddressRequest::make(row.str(), "AISLE-" + std::to_string(index), context);
      if (!request.has_value()) {
        continue;
      }
      const auto started = std::chrono::steady_clock::now();
      const auto receipt = registry->readdress(request.value());
      const auto finished = std::chrono::steady_clock::now();
      if (receipt.has_value()) {
        ++committed;
        restamped += receipt.value().affected_locations;
        total_ms += std::chrono::duration<double, std::milli>(finished - started).count();
      }
    }
    measurements.push_back({"subtree readdress (durable commit per change)", committed, total_ms,
                            "locations restamped: " + std::to_string(restamped)});
  }
  // ---- serialization and reopen -------------------------------------------
  {
    const std::uint32_t rounds = 5;
    std::uint64_t bytes_total = 0;
    const auto started = std::chrono::steady_clock::now();
    for (std::uint32_t index = 0; index < rounds; ++index) {
      const auto bytes = registry->encode_current_state();
      if (!bytes.has_value()) {
        continue;
      }
      bytes_total += bytes.value().size();
      const auto decoded = decode_snapshot(bytes.value());
      if (!decoded.has_value()) {
        std::cerr << decoded.error().to_string() << std::endl;
        return 1;
      }
    }
    const auto finished = std::chrono::steady_clock::now();
    const double total_ms =
        std::chrono::duration<double, std::milli>(finished - started).count();
    measurements.push_back({"encode + decode + validate state", rounds, total_ms,
                            "state bytes: " + std::to_string(bytes_total / rounds)});
  }

  const std::string digest_before = registry->state_digest();
  const std::uint64_t revision_before = registry->revision().value();
  const LocationStatistics statistics_before = registry->statistics();
  if (!registry->close().has_value()) {
    std::cerr << "close failed" << std::endl;
    return 1;
  }

  {
    RegistryOpenOptions read_options;
    read_options.mode = OpenMode::ReadOnly;
    const auto started = std::chrono::steady_clock::now();
    auto reopened = Registry::open(scratch.path(), read_options);
    const auto finished = std::chrono::steady_clock::now();
    if (!reopened.has_value()) {
      std::cerr << reopened.error().to_string() << std::endl;
      return 1;
    }
    const double total_ms =
        std::chrono::duration<double, std::milli>(finished - started).count();
    measurements.push_back({"open + verify published state", 1, total_ms,
                            "includes digest verification"});
    if (reopened.value()->state_digest() != digest_before) {
      std::cerr << "reopened digest differs" << std::endl;
      return 1;
    }
    if (reopened.value()->revision().value() != revision_before) {
      std::cerr << "reopened revision differs" << std::endl;
      return 1;
    }
    (void)reopened.value()->close();
  }

  std::cout << std::endl;
  for (const Measurement& measurement : measurements) {
    report(measurement);
  }

  const LocationStatistics after = registry->statistics();
  std::cout << std::endl;
  std::cout << "state: revision=" << revision_before << " locations=" << after.locations
            << " (expected " << expected_locations << ")" << std::endl;
  std::cout << "state: moves=" << after.moves << " aliases=" << after.aliases
            << " digest=" << digest_before << std::endl;
  std::cout << "integrity: reopened state verified against its digest and matched revision "
            << revision_before << std::endl;
  if (statistics_before.locations != after.locations) {
    std::cerr << "location count changed during the benchmark" << std::endl;
    return 1;
  }
  return 0;
}
