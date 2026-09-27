// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// plr-cli: read-only inspection and narrowly scoped mutation of a Physical
// Location Registry store. Every mutation goes through the same public API a
// library consumer uses, including authority, generation and revision checks;
// the tool has no privileged path into the store.

#include <algorithm>
#include <cstdint>
#include <utility>
#include <filesystem>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "tool_support.hpp"

namespace {

using namespace dccp::physical_location_registry;
using plr_tool::Arguments;
using plr_tool::ExitCode;
using plr_tool::Json;

struct Session {
  std::shared_ptr<Registry> registry;
  bool json = false;
};

ExitCode report(const Error& error, bool json) {
  if (json) {
    const Json payload = Json("error")
                             .field("code", error_code_name(error.code()))
                             .field("category", error_category_name(error.category()))
                             .field("subject", error.subject())
                             .field("message", error.message());
    std::cerr << payload.str() << std::endl;
  } else {
    plr_tool::print_error(error);
  }
  return plr_tool::exit_code_for(error);
}

/// Binds a successful Result or returns the mapped exit code.
#define CLI_TRY(value_name, expression)                          \
  auto value_name##_cli_result = (expression);                   \
  if (!value_name##_cli_result.has_value()) {                    \
    return report(value_name##_cli_result.error(), session.json); \
  }                                                              \
  auto& value_name = *value_name##_cli_result

void usage() {
  std::cout <<
      "plr-cli - Physical Location Registry inspection and mutation tool\n"
      "\n"
      "usage: plr-cli [--store DIR] [--format text|json] <command> [options]\n"
      "\n"
      "read-only commands\n"
      "  status                                  store identity, revision, epoch and digest\n"
      "  verify                                  verify the published state against its digest\n"
      "  roots                                   list top-level locations\n"
      "  list [--kind K] [--lifecycle S] [--root ID] [--max N]\n"
      "  show --id ID                            one location with provenance, moves and aliases\n"
      "  path --id ID                            canonical address of a location\n"
      "  resolve --path P [--include-retired]    resolve an address or alias\n"
      "  children --id ID [--depth N]            children, or descendants to a depth\n"
      "  tree [--root ID] [--depth N]            indented subtree\n"
      "  aliases                                 every alias binding\n"
      "  diff --from-revision N [--to-revision M]\n"
      "  diff --from-file A --to-file B          compare two published state files\n"
      "  snapshot --out FILE                     export canonical state bytes\n"
      "  explain-resolve --path P [--include-retired]\n"
      "  explain-move --id ID --to-parent ID\n"
      "  recover                                 report what a writer session recovered\n"
      "\n"
      "mutation commands (open the store read-write)\n"
      "  init [--max-locations N] [--max-depth N] [--max-children N]\n"
      "       [--max-aliases N] [--max-moves N] [--max-state-bytes N]\n"
      "       [--max-publications N] [--max-retained-revisions N]\n"
      "  create --id ID --kind K [--parent ID] --component C [--label L]\n"
      "         [--unit N] [--u-height N] [--source S]\n"
      "  readdress --id ID --component C\n"
      "  relabel --id ID --label L\n"
      "  move --id ID --to-parent ID\n"
      "  retire --id ID [--subtree]\n"
      "  reactivate --id ID [--subtree]\n"
      "  replace --id ID --new-id ID [--label L] [--unit N] [--u-height N]\n"
      "  alias-add --id ID --path P\n"
      "  alias-remove --id ID --path P\n"
      "  set-rack --id ID [--unit N] [--u-height N]\n"
      "\n"
      "mutation options: --actor A [--at TIME] [--expected-generation N]\n"
      "                  [--expected-revision N] [--op-id ID] [--reason TEXT]\n"
      "\n"
      "exit codes: 0 ok, 1 internal, 2 usage, 3 store unavailable,\n"
      "            4 integrity failure, 5 operation rejected\n";
}

Result<MutationContext> build_context(const Arguments& arguments, const Registry& registry,
                                      bool require_actor) {
  MutationContext context;
  const std::string actor_text = arguments.value_or("--actor", std::string());
  if (actor_text.empty()) {
    if (require_actor) {
      return Error(ErrorCode::InvalidArgument, "--actor is required for mutations");
    }
    PLR_TRY(default_actor, ActorId::parse("plr-cli"));
    context.actor = default_actor;
  } else {
    PLR_TRY(actor, ActorId::parse(actor_text));
    context.actor = actor;
  }

  const std::string at_text = arguments.value_or("--at", std::string());
  if (at_text.empty()) {
    PLR_TRY(unknown_time, Timestamp::from_unix_seconds(0));
    context.at = unknown_time;
  } else {
    PLR_TRY(at, Timestamp::parse_text(at_text));
    context.at = at;
  }

  if (arguments.has("--expected-generation")) {
    PLR_TRY(generation, arguments.unsigned_value("--expected-generation"));
    context.expected_generation = LocationGeneration(generation);
  }
  if (arguments.has("--expected-revision")) {
    PLR_TRY(revision, arguments.unsigned_value("--expected-revision"));
    context.expected_revision = LocationRevision(revision);
  }
  if (arguments.has("--op-id")) {
    PLR_TRY(operation, OperationId::parse(arguments.value_or("--op-id", std::string())));
    context.operation_id = operation;
  }
  context.reason = arguments.value_or("--reason", std::string());
  if (registry.is_writer()) {
    context.authority = registry.authority();
  }
  return context;
}

void print_view(const LocationView& view, bool json) {
  std::string parent;
  if (view.parent().has_value()) {
    parent = view.parent()->str();
  }
  std::string unit;
  if (view.unit().has_value()) {
    unit = view.unit()->to_string();
  }
  std::string envelope;
  if (view.envelope().has_value()) {
    envelope = view.envelope()->to_string();
  }
  std::string replaces;
  if (view.replaces().has_value()) {
    replaces = view.replaces()->str();
  }
  std::string replaced_by;
  if (view.replaced_by().has_value()) {
    replaced_by = view.replaced_by()->str();
  }

  if (json) {
    Json payload = Json("location");
    payload.field("id", view.id().str())
        .field("kind", location_kind_name(view.kind()))
        .field("path", view.path().to_string())
        .field("component", view.component().str())
        .field("label", view.label())
        .field("lifecycle", lifecycle_state_name(view.lifecycle()))
        .field("generation", view.generation().value())
        .field("parent", parent)
        .field("unit", unit)
        .field("envelope", envelope)
        .field("replaces", replaces)
        .field("replaced-by", replaced_by)
        .field("created-by", view.provenance().created_by.str())
        .field("created-at", view.provenance().created_at.to_string())
        .field("created-revision", view.provenance().created_revision.value())
        .field("source", view.provenance().source)
        .field("last-modified-by", view.provenance().last_modified_by.str())
        .field("last-modified-at", view.provenance().last_modified_at.to_string())
        .field("last-modified-revision", view.provenance().last_modified_revision.value())
        .field("moves", static_cast<std::uint64_t>(view.moves().size()))
        .field("aliases", static_cast<std::uint64_t>(view.aliases().size()));
    std::cout << payload.str() << std::endl;
    return;
  }

  std::cout << view.summary() << std::endl;
  if (!view.provenance().source.empty()) {
    std::cout << "  source=" << view.provenance().source << std::endl;
  }
  std::cout << "  created-by=" << view.provenance().created_by.str()
            << " created-at=" << view.provenance().created_at.to_string()
            << " created-revision=" << view.provenance().created_revision.value() << std::endl;
  std::cout << "  last-modified-by=" << view.provenance().last_modified_by.str()
            << " last-modified-at=" << view.provenance().last_modified_at.to_string()
            << " last-modified-revision=" << view.provenance().last_modified_revision.value()
            << std::endl;
  for (const MoveRecord& move : view.moves()) {
    std::cout << "  move kind=" << move_kind_name(move.kind)
              << " revision=" << move.revision.value() << " from=" << move.from_path
              << " to=" << move.to_path << " descendants=" << move.affected_descendants
              << " at=" << move.at.to_string() << " actor=" << move.actor.str();
    if (!move.reason.empty()) {
      std::cout << " reason=\"" << move.reason << "\"";
    }
    std::cout << std::endl;
  }
  for (const std::string& alias : view.aliases()) {
    std::cout << "  alias=" << alias << std::endl;
  }
}

void print_receipt(const MutationReceipt& receipt, bool json) {
  if (json) {
    Json payload = Json("mutation");
    payload.field("revision", receipt.revision.value())
        .field("sequence", receipt.sequence.value())
        .field("generation", receipt.generation.value())
        .field("affected-locations", static_cast<std::uint64_t>(receipt.affected_locations))
        .field("affected-descendants", static_cast<std::uint64_t>(receipt.affected_descendants))
        .field("replayed", receipt.replayed);
    if (receipt.successor_generation.has_value()) {
      payload.field("successor-generation", receipt.successor_generation->value());
    }
    std::cout << payload.str() << std::endl;
    return;
  }
  std::cout << "revision=" << receipt.revision.value() << " sequence=" << receipt.sequence.value()
            << " generation=" << receipt.generation.value()
            << " affected-locations=" << receipt.affected_locations
            << " affected-descendants=" << receipt.affected_descendants
            << " replayed=" << (receipt.replayed ? "true" : "false");
  if (receipt.successor_generation.has_value()) {
    std::cout << " successor-generation=" << receipt.successor_generation->value();
  }
  std::cout << std::endl;
}

void print_explanation(const Explanation& explanation, bool json) {
  if (json) {
    const Json payload = Json("explanation")
                             .field("code", error_code_name(explanation.code))
                             .field("category", error_category_name(explanation.category))
                             .field("ok", explanation.ok())
                             .field("summary", explanation.summary);
    std::cout << payload.str() << std::endl;
    return;
  }
  std::cout << explanation.to_string() << std::endl;
}

std::string describe_state(const Registry& registry) {
  std::string text;
  text.append("revision=");
  text.append(std::to_string(registry.revision().value()));
  text.append(" sequence=");
  text.append(std::to_string(registry.sequence().value()));
  text.append(" epoch=");
  text.append(std::to_string(registry.epoch().value()));
  text.append(" locations=");
  text.append(std::to_string(registry.statistics().locations));
  text.append(" file=");
  text.append(registry.state_file_name());
  return text;
}

Result<LocationKind> kind_from(const Arguments& arguments) {
  const std::string kind_text = arguments.value_or("--kind", std::string());
  if (kind_text.empty()) {
    return Error(ErrorCode::InvalidArgument, "--kind is required");
  }
  return parse_location_kind(kind_text);
}

Result<std::optional<RackEnvelope>> envelope_from(const Arguments& arguments) {
  if (!arguments.has("--u-height")) {
    return std::optional<RackEnvelope>();
  }
  PLR_TRY(height, arguments.unsigned32_value("--u-height"));
  PLR_TRY(envelope, RackEnvelope::with_height(height));
  return std::optional<RackEnvelope>(envelope);
}

Result<std::optional<RackUnitCoordinate>> unit_from(const Arguments& arguments) {
  if (!arguments.has("--unit")) {
    return std::optional<RackUnitCoordinate>();
  }
  PLR_TRY(unit_value, arguments.unsigned_value("--unit"));
  PLR_TRY(coordinate, RackUnitCoordinate::parse(unit_value));
  return std::optional<RackUnitCoordinate>(coordinate);
}

ExitCode status_command(const Arguments& arguments, Session& session) {
  const Registry& registry = *session.registry;
  if (arguments.value_or("--format", "text") == "json") {
    Json payload = Json("status")
                             .field("store", registry.store_id().str())
                             .field("revision", registry.revision().value())
                             .field("sequence", registry.sequence().value())
                             .field("epoch", registry.epoch().value())
                             .field("mode", open_mode_name(registry.is_writer()
                                                               ? OpenMode::ReadWrite
                                                               : OpenMode::ReadOnly))
                             .field("state-file", registry.state_file_name())
                             .field("state-bytes", registry.state_bytes())
                             .field("state-digest", registry.state_digest())
                             .field("locations", static_cast<std::uint64_t>(registry.statistics().locations))
                             .field("limits", registry.limits().to_string());
    std::cout << payload.str() << std::endl;
    return ExitCode::Ok;
  }
  std::cout << "store=" << registry.store_id().str() << std::endl;
  std::cout << describe_state(registry) << std::endl;
  std::cout << "state-digest=" << registry.state_digest() << std::endl;
  std::cout << "recovery=" << registry.recovery().to_string() << std::endl;
  std::cout << "limits=" << registry.limits().to_string() << std::endl;
  return ExitCode::Ok;
}

ExitCode list_command(const Arguments& arguments, Session& session) {
  ListOptions options;
  if (arguments.has("--kind")) {
    CLI_TRY(kind, kind_from(arguments));
    options.kind = kind;
  }
  if (arguments.has("--lifecycle")) {
    CLI_TRY(state, parse_lifecycle_state(arguments.value_or("--lifecycle", std::string())));
    options.lifecycle = state;
  }
  if (arguments.has("--root")) {
    CLI_TRY(root, LocationId::parse(arguments.value_or("--root", std::string())));
    options.root = root;
  }
  if (arguments.has("--max")) {
    CLI_TRY(maximum, arguments.unsigned32_value("--max"));
    options.max_nodes = maximum;
  }
  CLI_TRY(listed, session.registry->list(options));
  for (const LocationView& view : listed) {
    if (session.json) {
      print_view(view, true);
    } else {
      std::cout << view.summary() << std::endl;
    }
  }
  return ExitCode::Ok;
}

ExitCode children_command(const Arguments& arguments, Session& session) {
  CLI_TRY(id, LocationId::parse(arguments.value_or("--id", std::string())));
  if (arguments.has("--depth")) {
    CLI_TRY(depth, arguments.unsigned32_value("--depth"));
    CLI_TRY(descendants, session.registry->descendants(id, depth));
    for (const ChildEntry& entry : descendants) {
      std::cout << format_child_entry(entry) << std::endl;
    }
    return ExitCode::Ok;
  }
  CLI_TRY(children, session.registry->children(id));
  for (const ChildEntry& entry : children) {
    std::cout << format_child_entry(entry) << std::endl;
  }
  return ExitCode::Ok;
}

ExitCode tree_command(const Arguments& arguments, Session& session) {
  std::vector<ChildEntry> roots;
  if (arguments.has("--root")) {
    CLI_TRY(root, LocationId::parse(arguments.value_or("--root", std::string())));
    CLI_TRY(view, session.registry->find(root));
    ChildEntry entry;
    entry.id = view.id();
    entry.kind = view.kind();
    entry.component = view.component().str();
    entry.label = view.label();
    entry.lifecycle = view.lifecycle();
    entry.generation = view.generation();
    entry.path = view.path().to_string();
    roots.push_back(entry);
  } else {
    CLI_TRY(listed, session.registry->roots());
    roots = listed;
  }

  std::uint32_t max_depth = 32;
  if (arguments.has("--depth")) {
    CLI_TRY(depth, arguments.unsigned32_value("--depth"));
    max_depth = depth;
  }

  for (const ChildEntry& root : roots) {
    std::cout << root.path << " [" << location_kind_name(root.kind) << " "
              << lifecycle_state_name(root.lifecycle) << "]" << std::endl;
    CLI_TRY(descendants, session.registry->descendants(root.id, max_depth));
    const std::size_t root_depth =
        static_cast<std::size_t>(std::count(root.path.begin(), root.path.end(), '/'));
    for (const ChildEntry& entry : descendants) {
      const std::size_t depth =
          static_cast<std::size_t>(std::count(entry.path.begin(), entry.path.end(), '/'));
      const std::size_t indent = (depth > root_depth ? depth - root_depth : 0) * 2;
      std::cout << std::string(indent, ' ') << entry.component << " ["
                << location_kind_name(entry.kind) << " "
                << lifecycle_state_name(entry.lifecycle) << "]" << std::endl;
    }
  }
  return ExitCode::Ok;
}

ExitCode diff_command(const Arguments& arguments, Session& session) {
  if (arguments.has("--from-file") || arguments.has("--to-file")) {
    const std::string from_path = arguments.value_or("--from-file", std::string());
    const std::string to_path = arguments.value_or("--to-file", std::string());
    if (from_path.empty() || to_path.empty()) {
      return report(Error(ErrorCode::InvalidArgument,
                          "--from-file and --to-file must be given together"),
                    session.json);
    }
    CLI_TRY(from_bytes, plr_tool::read_file(from_path, kHardMaxStateBytes + (1ULL << 20U)));
    CLI_TRY(to_bytes, plr_tool::read_file(to_path, kHardMaxStateBytes + (1ULL << 20U)));
    CLI_TRY(from_snapshot, decode_snapshot(from_bytes));
    CLI_TRY(to_snapshot, decode_snapshot(to_bytes));
    std::cout << format_revision_diff(diff_snapshots(from_snapshot, to_snapshot)) << std::endl;
    return ExitCode::Ok;
  }

  // A revision pair is diffed from the published generations the store still
  // retains. Each publication carries its own revision, so this works across
  // processes and restarts without a durable journal; the price is that only
  // retained generations can be compared, which is reported explicitly.
  CLI_TRY(from, arguments.unsigned_value("--from-revision"));
  LocationRevision to = session.registry->revision();
  if (arguments.has("--to-revision")) {
    CLI_TRY(to_value, arguments.unsigned_value("--to-revision"));
    to = LocationRevision(to_value);
  }
  if (to < LocationRevision(from)) {
    return report(Error(ErrorCode::InvalidArgument, "diff range ends before it starts"), session.json);
  }

  std::error_code error;
  std::vector<std::string> publications;
  for (const auto& entry : std::filesystem::directory_iterator(session.registry->directory(), error)) {
    const std::string name = entry.path().filename().string();
    if (name.rfind("state.", 0) == 0 && name.size() > 10 && name.substr(name.size() - 4) == ".plr") {
      publications.push_back(name);
    }
  }
  std::sort(publications.begin(), publications.end());

  std::optional<Snapshot> from_snapshot;
  std::optional<Snapshot> to_snapshot;
  std::vector<std::uint64_t> available;
  for (const std::string& name : publications) {
    CLI_TRY(bytes, plr_tool::read_file((session.registry->directory() / name).string(),
                                       kHardMaxStateBytes + (1ULL << 20U)));
    auto decoded = decode_snapshot(bytes);
    if (!decoded.has_value()) {
      continue;  // a damaged older publication is skipped, never guessed at
    }
    available.push_back(decoded.value().revision().value());
    const std::uint64_t revision = decoded.value().revision().value();
    if (revision <= from && (!from_snapshot.has_value() ||
                             from_snapshot->revision().value() < revision)) {
      from_snapshot = std::move(decoded.value());
      continue;
    }
    if (revision <= to.value() &&
        (!to_snapshot.has_value() || to_snapshot->revision().value() < revision)) {
      to_snapshot = std::move(decoded.value());
    }
  }

  if (!from_snapshot.has_value() || !to_snapshot.has_value()) {
    std::sort(available.begin(), available.end());
    available.erase(std::unique(available.begin(), available.end()), available.end());
    std::string detail = "retained revisions:";
    if (available.empty()) {
      detail.append(" none");
    }
    for (const std::uint64_t revision : available) {
      detail.append(" ");
      detail.append(std::to_string(revision));
    }
    return report(Error(ErrorCode::RevisionNotRetained,
                        "the requested revisions are not both retained; publish more state "
                        "generations with --max-publications, or compare snapshot files: " +
                            detail)
                      .with_subject(std::to_string(from)),
                  session.json);
  }

  std::cout << format_revision_diff(diff_snapshots(from_snapshot.value(), to_snapshot.value()))
            << std::endl;
  return ExitCode::Ok;
}

ExitCode snapshot_command(const Arguments& arguments, Session& session) {
  const std::string out = arguments.value_or("--out", std::string());
  if (out.empty()) {
    return report(Error(ErrorCode::InvalidArgument, "--out is required"), session.json);
  }
  CLI_TRY(bytes, session.registry->encode_current_state());
  auto written = plr_tool::write_file_atomically(out, bytes);
  if (!written.has_value()) {
    return report(written.error(), session.json);
  }
  std::cout << "snapshot-file=" << out << " bytes=" << bytes.size()
            << " digest=" << session.registry->state_digest() << std::endl;
  return ExitCode::Ok;
}

ExitCode create_command(const Arguments& arguments, Session& session) {
  CLI_TRY(context, build_context(arguments, *session.registry, true));
  CLI_TRY(kind, kind_from(arguments));
  CLI_TRY(request, CreateLocationRequest::make(arguments.value_or("--id", std::string()), kind,
                                               arguments.value_or("--parent", std::string()),
                                               arguments.value_or("--component", std::string()),
                                               arguments.value_or("--label", std::string()),
                                               context));
  CLI_TRY(unit, unit_from(arguments));
  CLI_TRY(envelope, envelope_from(arguments));
  request.unit = unit;
  request.envelope = envelope;
  request.source = arguments.value_or("--source", "plr-cli");
  auto receipt = session.registry->create_location(request);
  if (!receipt.has_value()) {
    return report(receipt.error(), session.json);
  }
  print_receipt(receipt.value(), session.json);
  return ExitCode::Ok;
}

ExitCode mutation_command(const Arguments& arguments, Session& session, std::string_view command) {
  CLI_TRY(context, build_context(arguments, *session.registry, true));
  const std::string id = arguments.value_or("--id", std::string());

  if (command == "readdress") {
    CLI_TRY(request, ReaddressRequest::make(id, arguments.value_or("--component", std::string()),
                                            context));
    auto receipt = session.registry->readdress(request);
    if (!receipt.has_value()) {
      return report(receipt.error(), session.json);
    }
    print_receipt(receipt.value(), session.json);
    return ExitCode::Ok;
  }
  if (command == "relabel") {
    CLI_TRY(request, RelabelRequest::make(id, arguments.value_or("--label", std::string()), context));
    auto receipt = session.registry->relabel(request);
    if (!receipt.has_value()) {
      return report(receipt.error(), session.json);
    }
    print_receipt(receipt.value(), session.json);
    return ExitCode::Ok;
  }
  if (command == "move") {
    CLI_TRY(request, MoveLocationRequest::make(id, arguments.value_or("--to-parent", std::string()),
                                               context));
    auto receipt = session.registry->move_location(request);
    if (!receipt.has_value()) {
      return report(receipt.error(), session.json);
    }
    print_receipt(receipt.value(), session.json);
    return ExitCode::Ok;
  }
  if (command == "retire" || command == "reactivate") {
    const SubtreeMode mode =
        arguments.has("--subtree") ? SubtreeMode::Subtree : SubtreeMode::LocationOnly;
    if (command == "retire") {
      CLI_TRY(request, RetireLocationRequest::make(id, mode, context));
      auto receipt = session.registry->retire_location(request);
      if (!receipt.has_value()) {
        return report(receipt.error(), session.json);
      }
      print_receipt(receipt.value(), session.json);
      return ExitCode::Ok;
    }
    CLI_TRY(request, ReactivateLocationRequest::make(id, mode, context));
    auto receipt = session.registry->reactivate_location(request);
    if (!receipt.has_value()) {
      return report(receipt.error(), session.json);
    }
    print_receipt(receipt.value(), session.json);
    return ExitCode::Ok;
  }
  if (command == "replace") {
    CLI_TRY(request, ReplaceLocationRequest::make(id, arguments.value_or("--new-id", std::string()),
                                                  arguments.value_or("--label", std::string()),
                                                  context));
    CLI_TRY(unit, unit_from(arguments));
    CLI_TRY(envelope, envelope_from(arguments));
    request.successor_unit = unit;
    request.successor_envelope = envelope;
    request.source = arguments.value_or("--source", "plr-cli");
    auto receipt = session.registry->replace_location(request);
    if (!receipt.has_value()) {
      return report(receipt.error(), session.json);
    }
    print_receipt(receipt.value(), session.json);
    return ExitCode::Ok;
  }
  if (command == "alias-add" || command == "alias-remove") {
    const std::string alias = arguments.value_or("--path", std::string());
    const Limits limits = session.registry->limits();
    if (command == "alias-add") {
      CLI_TRY(request, AddAliasRequest::make(id, alias, limits, context));
      auto receipt = session.registry->add_alias(request);
      if (!receipt.has_value()) {
        return report(receipt.error(), session.json);
      }
      print_receipt(receipt.value(), session.json);
      return ExitCode::Ok;
    }
    CLI_TRY(request, RemoveAliasRequest::make(id, alias, limits, context));
    auto receipt = session.registry->remove_alias(request);
    if (!receipt.has_value()) {
      return report(receipt.error(), session.json);
    }
    print_receipt(receipt.value(), session.json);
    return ExitCode::Ok;
  }
  if (command == "set-rack") {
    CLI_TRY(request, SetRackGeometryRequest::make(id, context));
    CLI_TRY(unit, unit_from(arguments));
    CLI_TRY(envelope, envelope_from(arguments));
    request.unit = unit;
    request.envelope = envelope;
    auto receipt = session.registry->set_rack_geometry(request);
    if (!receipt.has_value()) {
      return report(receipt.error(), session.json);
    }
    print_receipt(receipt.value(), session.json);
    return ExitCode::Ok;
  }

  return report(Error(ErrorCode::InvalidArgument, "unknown mutation command")
                    .with_subject(std::string(command)),
                session.json);
}

ExitCode run_read_only(const Arguments& arguments, Session& session) {
  const std::string command = arguments.command();
  if (command == "status" || command == "stats") {
    return status_command(arguments, session);
  }
  if (command == "verify") {
    const auto verified = session.registry->verify_storage();
    if (!verified.has_value()) {
      return report(verified.error(), session.json);
    }
    std::cout << "verified store=" << session.registry->store_id().str()
              << " revision=" << session.registry->revision().value()
              << " digest=" << session.registry->state_digest() << std::endl;
    return ExitCode::Ok;
  }
  if (command == "roots") {
    CLI_TRY(roots, session.registry->roots());
    for (const ChildEntry& entry : roots) {
      std::cout << format_child_entry(entry) << std::endl;
    }
    return ExitCode::Ok;
  }
  if (command == "list") {
    return list_command(arguments, session);
  }
  if (command == "show" || command == "path") {
    CLI_TRY(id, LocationId::parse(arguments.value_or("--id", std::string())));
    if (command == "path") {
      CLI_TRY(path, session.registry->path_of(id));
      std::cout << path.to_string() << std::endl;
      return ExitCode::Ok;
    }
    CLI_TRY(view, session.registry->find(id));
    print_view(view, session.json);
    return ExitCode::Ok;
  }
  if (command == "resolve") {
    CLI_TRY(path, LocationPath::parse(arguments.value_or("--path", std::string()),
                                      session.registry->limits()));
    const ResolutionMode mode = arguments.has("--include-retired") ? ResolutionMode::IncludeRetired
                                                                   : ResolutionMode::CurrentOnly;
    CLI_TRY(resolved, session.registry->resolve(path, mode));
    if (session.json) {
      const Json payload = Json("resolution")
                               .field("id", resolved.id.str())
                               .field("kind", resolution_kind_name(resolved.kind))
                               .field("canonical-path", resolved.canonical_path.to_string())
                               .field("lifecycle", lifecycle_state_name(resolved.lifecycle))
                               .field("generation", resolved.generation.value());
      std::cout << payload.str() << std::endl;
      return ExitCode::Ok;
    }
    std::cout << "id=" << resolved.id.str()
              << " kind=" << resolution_kind_name(resolved.kind)
              << " path=" << resolved.canonical_path.to_string()
              << " lifecycle=" << lifecycle_state_name(resolved.lifecycle)
              << " generation=" << resolved.generation.value() << std::endl;
    return ExitCode::Ok;
  }
  if (command == "children") {
    return children_command(arguments, session);
  }
  if (command == "tree") {
    return tree_command(arguments, session);
  }
  if (command == "aliases") {
    CLI_TRY(bindings, session.registry->aliases());
    for (const AliasBinding& binding : bindings) {
      std::cout << "alias=" << binding.alias << " id=" << binding.id.str()
                << " path=" << binding.canonical_path.to_string()
                << " lifecycle=" << lifecycle_state_name(binding.lifecycle)
                << " generation=" << binding.generation.value() << std::endl;
    }
    return ExitCode::Ok;
  }
  if (command == "diff") {
    return diff_command(arguments, session);
  }
  if (command == "snapshot") {
    return snapshot_command(arguments, session);
  }
  if (command == "explain-resolve") {
    CLI_TRY(path, LocationPath::parse(arguments.value_or("--path", std::string()),
                                      session.registry->limits()));
    const ResolutionMode mode = arguments.has("--include-retired") ? ResolutionMode::IncludeRetired
                                                                   : ResolutionMode::CurrentOnly;
    print_explanation(session.registry->explain_resolve(path, mode), session.json);
    return ExitCode::Ok;
  }
  if (command == "explain-move") {
    CLI_TRY(context, build_context(arguments, *session.registry, false));
    CLI_TRY(request, MoveLocationRequest::make(arguments.value_or("--id", std::string()),
                                               arguments.value_or("--to-parent", std::string()),
                                               context));
    print_explanation(session.registry->explain_move(request), session.json);
    return ExitCode::Ok;
  }
  return report(Error(ErrorCode::InvalidArgument, "unknown read-only command").with_subject(command),
                session.json);
}

int run_init(const Arguments& arguments, const std::string& store_path) {
  const bool json = arguments.value_or("--format", "text") == "json";
  RegistryOpenOptions options;
  options.mode = OpenMode::ReadWrite;
  options.create_if_missing = true;
  Limits limits;
  const auto set32 = [&](const char* name, std::uint32_t& target) -> Result<void> {
    if (!arguments.has(name)) {
      return ok();
    }
    PLR_TRY(value, arguments.unsigned32_value(name));
    target = value;
    return ok();
  };
  const std::pair<const char*, std::uint32_t*> numeric_options[] = {
      {"--max-locations", &limits.max_locations},
      {"--max-depth", &limits.max_depth},
      {"--max-children", &limits.max_children_per_location},
      {"--max-aliases", &limits.max_total_aliases},
      {"--max-moves", &limits.max_total_moves},
      {"--max-publications", &limits.max_publications_retained},
      {"--max-retained-revisions", &limits.max_retained_revisions}};
  for (const auto& option : numeric_options) {
    auto applied = set32(option.first, *option.second);
    if (!applied.has_value()) {
      return static_cast<int>(report(applied.error(), json));
    }
  }
  if (limits.max_aliases_per_location > limits.max_total_aliases) {
    limits.max_aliases_per_location = limits.max_total_aliases;
  }
  if (limits.max_moves_per_location > limits.max_total_moves) {
    limits.max_moves_per_location = limits.max_total_moves;
  }
  if (arguments.has("--max-state-bytes")) {
    auto value = arguments.unsigned_value("--max-state-bytes");
    if (!value.has_value()) {
      return static_cast<int>(report(value.error(), json));
    }
    limits.max_state_bytes = value.value();
  }
  options.limits = limits;

  auto created = Registry::create(store_path, options);
  if (!created.has_value()) {
    return static_cast<int>(report(created.error(), json));
  }
  Session session;
  session.registry = created.value();
  session.json = json;
  return static_cast<int>(status_command(arguments, session));
}

}  // namespace

/// Options every command accepts.
const char* const kGlobalOptions[] = {"--store", "--format", "--help"};

/// Options a mutation may carry in addition to its own subject matter.
const char* const kContextOptions[] = {"--actor",           "--at",
                                       "--expected-generation", "--expected-revision",
                                       "--op-id",           "--reason"};

/// One entry per command: the command-specific options it accepts.
const std::vector<std::pair<std::string_view, std::vector<std::string_view>>>& command_options() {
  static const std::vector<std::pair<std::string_view, std::vector<std::string_view>>> table = {
      {"status", {}},
      {"stats", {}},
      {"verify", {}},
      {"roots", {}},
      {"list", {"--kind", "--lifecycle", "--root", "--max"}},
      {"show", {"--id"}},
      {"path", {"--id"}},
      {"resolve", {"--path", "--include-retired"}},
      {"children", {"--id", "--depth"}},
      {"tree", {"--root", "--depth"}},
      {"aliases", {}},
      {"diff", {"--from-revision", "--to-revision", "--from-file", "--to-file"}},
      {"snapshot", {"--out"}},
      {"explain-resolve", {"--path", "--include-retired"}},
      {"explain-move", {"--id", "--to-parent", "--actor", "--at"}},
      {"recover", {}},
      {"init",
       {"--max-locations", "--max-depth", "--max-children", "--max-aliases", "--max-moves",
        "--max-state-bytes", "--max-publications", "--max-retained-revisions"}},
      {"create",
       {"--id", "--kind", "--parent", "--component", "--label", "--unit", "--u-height", "--source",
        "--actor", "--at", "--expected-generation", "--expected-revision", "--op-id", "--reason"}},
      {"readdress",
       {"--id", "--component", "--actor", "--at", "--expected-generation", "--expected-revision",
        "--op-id", "--reason"}},
      {"relabel",
       {"--id", "--label", "--actor", "--at", "--expected-generation", "--expected-revision",
        "--op-id", "--reason"}},
      {"move",
       {"--id", "--to-parent", "--actor", "--at", "--expected-generation", "--expected-revision",
        "--op-id", "--reason"}},
      {"retire",
       {"--id", "--subtree", "--actor", "--at", "--expected-generation", "--expected-revision",
        "--op-id", "--reason"}},
      {"reactivate",
       {"--id", "--subtree", "--actor", "--at", "--expected-generation", "--expected-revision",
        "--op-id", "--reason"}},
      {"replace",
       {"--id", "--new-id", "--label", "--unit", "--u-height", "--source", "--actor", "--at",
        "--expected-generation", "--expected-revision", "--op-id", "--reason"}},
      {"alias-add",
       {"--id", "--path", "--actor", "--at", "--expected-generation", "--expected-revision",
        "--op-id", "--reason"}},
      {"alias-remove",
       {"--id", "--path", "--actor", "--at", "--expected-generation", "--expected-revision",
        "--op-id", "--reason"}},
      {"set-rack",
       {"--id", "--unit", "--u-height", "--actor", "--at", "--expected-generation",
        "--expected-revision", "--op-id", "--reason"}},
  };
  return table;
}

/// Rejects any option the named command does not understand. Silently ignoring a
/// typo such as "--compnent" would commit a location with the wrong address while
/// the operator believes otherwise.
Result<void> check_options(const Arguments& arguments, std::string_view command) {
  const std::vector<std::string> supplied = arguments.option_names();
  if (supplied.empty()) {
    return ok();
  }

  std::vector<std::string_view> allowed;
  for (const char* option : kGlobalOptions) {
    allowed.emplace_back(option);
  }
  bool known_command = false;
  for (const auto& entry : command_options()) {
    if (entry.first == command) {
      known_command = true;
      for (const std::string_view option : entry.second) {
        allowed.push_back(option);
      }
    }
  }
  if (!known_command) {
    return Error(ErrorCode::InvalidArgument, "unknown command").with_subject(std::string(command));
  }

  for (const std::string& option : supplied) {
    if (std::find(allowed.begin(), allowed.end(), option) == allowed.end()) {
      return Error(ErrorCode::InvalidArgument, "unknown option for this command")
          .with_subject(option);
    }
  }
  return ok();
}

int main(int argc, char** argv) {
  const Arguments arguments(argc, argv);
  const std::string command = arguments.command();
  if (command.empty() || command == "help" || arguments.has("--help")) {
    usage();
    return static_cast<int>(command.empty() && !arguments.has("--help") ? ExitCode::Usage
                                                                        : ExitCode::Ok);
  }
  if (command == "version") {
    std::cout << library_name() << " " << version_string() << std::endl;
    return static_cast<int>(ExitCode::Ok);
  }

  const std::string store_path = arguments.value_or("--store", "plr-store");
  const bool json = arguments.value_or("--format", "text") == "json";

  // A typo in an option name is a usage error, not something to ignore: silently
  // accepting "--compnent" would commit a location with the wrong address while
  // the operator believes otherwise.
  const auto options_ok = check_options(arguments, command);
  if (!options_ok.has_value()) {
    return static_cast<int>(report(options_ok.error(), json));
  }

  if (command == "init") {
    return run_init(arguments, store_path);
  }

  const bool mutation = command == "create" || command == "readdress" || command == "relabel" ||
                        command == "move" || command == "retire" || command == "reactivate" ||
                        command == "replace" || command == "alias-add" ||
                        command == "alias-remove" || command == "set-rack" || command == "recover";

  Session session;
  session.json = json;

  RegistryOpenOptions options;
  options.mode = mutation ? OpenMode::ReadWrite : OpenMode::ReadOnly;
  auto opened = Registry::open(store_path, options);
  if (!opened.has_value()) {
    return static_cast<int>(report(opened.error(), session.json));
  }
  session.registry = opened.value();

  if (command == "recover") {
    const RecoveryReport recovery = session.registry->recovery();
    std::cout << "recovery=" << recovery.to_string() << std::endl;
    std::cout << (recovery.clean() ? "result=clean" : "result=recovered") << std::endl;
    return static_cast<int>(ExitCode::Ok);
  }

  if (mutation) {
    if (command == "create") {
      return static_cast<int>(create_command(arguments, session));
    }
    return static_cast<int>(mutation_command(arguments, session, command));
  }
  return static_cast<int>(run_read_only(arguments, session));
}
