// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// plr-store-agent: a deliberately small process helper. It exercises real
// library authority from a second process so that writer fencing, epoch
// advancement and crash behaviour can be proven between processes rather than
// asserted in comments. It also serves as an operator diagnostic for "who holds
// this store" and "does this publication verify".

#include <chrono>
#include <cstdlib>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "tool_support.hpp"

namespace {

using namespace dccp::physical_location_registry;
using plr_tool::Arguments;
using plr_tool::ExitCode;

Result<void> write_marker(const std::string& path, const std::string& line) {
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  if (!stream) {
    return Error(ErrorCode::IoError, "cannot write marker file").with_subject(path);
  }
  stream << line << "\n";
  stream.flush();
  if (!stream) {
    return Error(ErrorCode::IoError, "cannot flush marker file").with_subject(path);
  }
  return ok();
}

bool file_exists(const std::string& path) {
  std::ifstream stream(path, std::ios::binary);
  return static_cast<bool>(stream);
}

int report_and_fail(const Error& error) {
  plr_tool::print_error(error);
  return static_cast<int>(plr_tool::exit_code_for(error));
}

/// Opens a store for writing, announces readiness, and holds the writer lock
/// until a release file appears (or a bounded number of polls elapses).
int hold_lock(const Arguments& arguments) {
  const std::string store = arguments.value_or("--store", std::string());
  const std::string ready = arguments.value_or("--ready-file", std::string());
  const std::string release = arguments.value_or("--release-file", std::string());
  if (store.empty() || ready.empty()) {
    std::cerr << "usage: plr-store-agent hold-lock --store DIR --ready-file FILE "
                 "[--release-file FILE] [--poll-iterations N]"
              << std::endl;
    return static_cast<int>(ExitCode::Usage);
  }

  RegistryOpenOptions options;
  options.mode = OpenMode::ReadWrite;
  auto opened = Registry::open(store, options);
  if (!opened.has_value()) {
    return report_and_fail(opened.error());
  }

  const auto written = write_marker(ready, "ready");
  if (!written.has_value()) {
    return report_and_fail(written.error());
  }

  std::uint64_t iterations = 4000;
  if (arguments.has("--poll-iterations")) {
    const auto value = arguments.unsigned_value("--poll-iterations");
    if (!value.has_value()) {
      return report_and_fail(value.error());
    }
    iterations = value.value();
  }

  if (!release.empty()) {
    bool released = false;
    for (std::uint64_t index = 0; index < iterations; ++index) {
      if (file_exists(release)) {
        released = true;
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    if (!released) {
      std::cerr << "error-code=IO_ERROR message=release file never appeared" << std::endl;
      return static_cast<int>(ExitCode::Internal);
    }
  }

  std::cout << "store=" << opened.value()->store_id().str()
            << " epoch=" << opened.value()->epoch().value()
            << " revision=" << opened.value()->revision().value() << std::endl;
  const auto closed = opened.value()->close();
  if (!closed.has_value()) {
    return report_and_fail(closed.error());
  }
  return static_cast<int>(ExitCode::Ok);
}

/// Opens a store for writing and then exits immediately without unwinding: the
/// process dies while holding the writer lock, which is how a real crash looks.
int crash_hold(const Arguments& arguments) {
  const std::string store = arguments.value_or("--store", std::string());
  const std::string ready = arguments.value_or("--ready-file", std::string());
  if (store.empty() || ready.empty()) {
    std::cerr << "usage: plr-store-agent crash-hold --store DIR --ready-file FILE" << std::endl;
    return static_cast<int>(ExitCode::Usage);
  }
  RegistryOpenOptions options;
  options.mode = OpenMode::ReadWrite;
  auto opened = Registry::open(store, options);
  if (!opened.has_value()) {
    return report_and_fail(opened.error());
  }
  const auto written = write_marker(ready, "ready");
  if (!written.has_value()) {
    return report_and_fail(written.error());
  }
  std::cout << "crashing epoch=" << opened.value()->epoch().value() << std::endl;
  std::cout.flush();
  // An abrupt exit: no destructors, no lock release by this process, no flush.
  std::_Exit(70);
}

/// Creates one location from a second process.
int write_location(const Arguments& arguments) {
  const std::string store = arguments.value_or("--store", std::string());
  RegistryOpenOptions options;
  options.mode = OpenMode::ReadWrite;
  auto opened = Registry::open(store, options);
  if (!opened.has_value()) {
    return report_and_fail(opened.error());
  }
  std::shared_ptr<Registry> registry = opened.value();

  MutationContext context;
  const auto actor = ActorId::parse(arguments.value_or("--actor", "store-agent"));
  if (!actor.has_value()) {
    return report_and_fail(actor.error());
  }
  context.actor = actor.value();
  const std::string at_text = arguments.value_or("--at", std::string());
  if (!at_text.empty()) {
    const auto at = Timestamp::parse_text(at_text);
    if (!at.has_value()) {
      return report_and_fail(at.error());
    }
    context.at = at.value();
  }

  const std::string kind_text = arguments.value_or("--kind", "room");
  const auto kind = parse_location_kind(kind_text);
  if (!kind.has_value()) {
    return report_and_fail(kind.error());
  }
  auto request = CreateLocationRequest::make(arguments.value_or("--id", std::string()), kind.value(),
                                             arguments.value_or("--parent", std::string()),
                                             arguments.value_or("--component", std::string()),
                                             arguments.value_or("--label", std::string()), context);
  if (!request.has_value()) {
    return report_and_fail(request.error());
  }
  request.value().source = "plr-store-agent";

  auto receipt = registry->create_location(request.value());
  if (!receipt.has_value()) {
    return report_and_fail(receipt.error());
  }
  std::cout << "revision=" << receipt.value().revision.value()
            << " sequence=" << receipt.value().sequence.value()
            << " generation=" << receipt.value().generation.value() << std::endl;
  const auto closed = registry->close();
  if (!closed.has_value()) {
    return report_and_fail(closed.error());
  }
  return static_cast<int>(ExitCode::Ok);
}

/// Reads the store from a second process without taking the writer lock.
int read_location(const Arguments& arguments) {
  RegistryOpenOptions options;
  options.mode = OpenMode::ReadOnly;
  auto opened = Registry::open(arguments.value_or("--store", std::string()), options);
  if (!opened.has_value()) {
    return report_and_fail(opened.error());
  }
  std::shared_ptr<Registry> registry = opened.value();
  const std::string path_text = arguments.value_or("--path", std::string());
  if (!path_text.empty()) {
    const auto path = LocationPath::parse(path_text, registry->limits());
    if (!path.has_value()) {
      return report_and_fail(path.error());
    }
    auto resolved = registry->resolve(path.value());
    if (!resolved.has_value()) {
      return report_and_fail(resolved.error());
    }
    std::cout << "id=" << resolved.value().id.str()
              << " path=" << resolved.value().canonical_path.to_string()
              << " generation=" << resolved.value().generation.value() << std::endl;
    return static_cast<int>(ExitCode::Ok);
  }
  std::cout << "revision=" << registry->revision().value()
            << " sequence=" << registry->sequence().value()
            << " epoch=" << registry->epoch().value()
            << " locations=" << registry->statistics().locations
            << " digest=" << registry->state_digest() << std::endl;
  return static_cast<int>(ExitCode::Ok);
}

/// Publishes once and then dies at a configured publication stage, so the
/// on-disk effect of a crash mid-publication can be inspected afterwards.
int crash_publish(const Arguments& arguments) {
  const std::string store = arguments.value_or("--store", std::string());
  const std::string stage_text = arguments.value_or("--stage", std::string());
  PublishStage stage = PublishStage::None;
  const PublishStage stages[] = {PublishStage::BeforeStateWrite, PublishStage::AfterStateWrite,
                                 PublishStage::BeforeStateRename, PublishStage::BeforeHeadPublish,
                                 PublishStage::AfterHeadPublish, PublishStage::BeforeRetire};
  for (const PublishStage candidate : stages) {
    if (stage_text == publish_stage_name(candidate)) {
      stage = candidate;
    }
  }
  if (stage == PublishStage::None) {
    std::cerr << "usage: plr-store-agent crash-publish --store DIR --stage STAGE --id ID "
                 "--component C [--kind K] [--parent ID] [--label L]"
              << std::endl;
    return static_cast<int>(ExitCode::Usage);
  }

  RegistryOpenOptions options;
  options.mode = OpenMode::ReadWrite;
  options.faults.stage = stage;
  options.faults.action = FaultAction::Crash;
  options.faults.publication_ordinal = 2;  // the first publication is the epoch bump
  auto opened = Registry::open(store, options);
  if (!opened.has_value()) {
    return report_and_fail(opened.error());
  }
  std::shared_ptr<Registry> registry = opened.value();

  MutationContext context;
  const auto actor = ActorId::parse("store-agent-crash");
  if (!actor.has_value()) {
    return report_and_fail(actor.error());
  }
  context.actor = actor.value();
  const auto kind = parse_location_kind(arguments.value_or("--kind", "room"));
  if (!kind.has_value()) {
    return report_and_fail(kind.error());
  }
  auto request = CreateLocationRequest::make(arguments.value_or("--id", std::string()), kind.value(),
                                             arguments.value_or("--parent", std::string()),
                                             arguments.value_or("--component", std::string()),
                                             arguments.value_or("--label", std::string()), context);
  if (!request.has_value()) {
    return report_and_fail(request.error());
  }
  request.value().source = "plr-store-agent";

  std::cout << "publishing epoch=" << registry->epoch().value() << std::endl;
  std::cout.flush();
  auto receipt = registry->create_location(request.value());
  // Only reachable when the injected stage was never hit.
  if (!receipt.has_value()) {
    return report_and_fail(receipt.error());
  }
  std::cout << "revision=" << receipt.value().revision.value() << std::endl;
  return static_cast<int>(ExitCode::Ok);
}

/// Verifies the published state and prints the recovery report.
int verify_store(const Arguments& arguments) {
  RegistryOpenOptions options;
  options.mode = OpenMode::ReadOnly;
  auto opened = Registry::open(arguments.value_or("--store", std::string()), options);
  if (!opened.has_value()) {
    return report_and_fail(opened.error());
  }
  const auto verified = opened.value()->verify_storage();
  if (!verified.has_value()) {
    return report_and_fail(verified.error());
  }
  const RecoveryReport recovery = opened.value()->recovery();
  std::cout << "verified=true recovery-clean=" << (recovery.clean() ? "true" : "false")
            << " revision=" << opened.value()->revision().value()
            << " digest=" << opened.value()->state_digest() << std::endl;
  return static_cast<int>(ExitCode::Ok);
}

void usage() {
  std::cout <<
      "plr-store-agent - process helper for store fencing, restart and crash proof\n"
      "\n"
      "usage: plr-store-agent <command> [options]\n"
      "\n"
      "  hold-lock --store DIR --ready-file FILE [--release-file FILE]\n"
      "            [--poll-iterations N]   hold the writer lock until released\n"
      "  crash-hold --store DIR --ready-file FILE\n"
      "                                      die while holding the writer lock\n"
      "  write --store DIR --id ID --component C [--kind K] [--parent ID] [--label L]\n"
      "            [--actor A] [--at TIME]   create one location from this process\n"
      "  read --store DIR [--path P]         read committed state from this process\n"
      "  crash-publish --store DIR --stage STAGE --id ID --component C\n"
      "            [--kind K] [--parent ID] [--label L]\n"
      "                                      die during publication at STAGE\n"
      "  verify --store DIR                  verify the publication and report recovery\n"
      "\n"
      "stages: before-state-write, after-state-write, before-state-rename,\n"
      "        before-head-publish, after-head-publish, before-retire\n"
      "\n"
      "exit codes: 0 ok, 2 usage, 3 store unavailable, 4 integrity failure, 5 rejected\n";
}

}  // namespace

int main(int argc, char** argv) {
  const Arguments arguments(argc, argv);
  const std::string command = arguments.command();
  if (command.empty() || command == "help" || arguments.has("--help")) {
    usage();
    return static_cast<int>(command.empty() ? ExitCode::Usage : ExitCode::Ok);
  }
  if (command == "hold-lock") {
    return hold_lock(arguments);
  }
  if (command == "crash-hold") {
    return crash_hold(arguments);
  }
  if (command == "write") {
    return write_location(arguments);
  }
  if (command == "read") {
    return read_location(arguments);
  }
  if (command == "crash-publish") {
    return crash_publish(arguments);
  }
  if (command == "verify") {
    return verify_store(arguments);
  }
  std::cerr << "unknown command: " << command << std::endl;
  usage();
  return static_cast<int>(ExitCode::Usage);
}
