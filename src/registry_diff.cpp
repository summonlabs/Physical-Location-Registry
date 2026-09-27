// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "registry_internal.hpp"

#include <algorithm>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace dccp::physical_location_registry {
namespace {

struct FlagName {
  ChangeFlags flag;
  std::string_view name;
};

constexpr FlagName kFlagNames[] = {
    {kChangeCreated, "created"},
    {kChangeReparented, "reparented"},
    {kChangeReaddressed, "readdressed"},
    {kChangeRelabeled, "relabeled"},
    {kChangeLifecycle, "lifecycle"},
    {kChangeRackGeometry, "rack-geometry"},
    {kChangeAliasAdded, "alias-added"},
    {kChangeAliasRemoved, "alias-removed"},
    {kChangeReplacementLineage, "replacement-lineage"},
    {kChangeRemoved, "removed"},
    {kChangeKindChanged, "kind-changed"},
};

void merge_change(LocationChange& target, const LocationChange& source) {
  target.flags |= source.flags;
  target.kind = source.kind;
  if (source.path_before.has_value() && !target.path_before.has_value()) {
    target.path_before = source.path_before;
  }
  if (source.path_after.has_value()) {
    target.path_after = source.path_after;
  }
  if (source.parent_before.has_value() && !target.parent_before.has_value()) {
    target.parent_before = source.parent_before;
  }
  if (source.parent_after.has_value() || target.parent_after.has_value()) {
    target.parent_after = source.parent_after;
  }
  if (source.generation_before.published() && !target.generation_before.published()) {
    target.generation_before = source.generation_before;
  }
  if (source.generation_after.published()) {
    target.generation_after = source.generation_after;
  }
  if (source.label_before.has_value() && !target.label_before.has_value()) {
    target.label_before = source.label_before;
  }
  if (source.label_after.has_value()) {
    target.label_after = source.label_after;
  }
  if (source.lifecycle_before.has_value() && !target.lifecycle_before.has_value()) {
    target.lifecycle_before = source.lifecycle_before;
  }
  if (source.lifecycle_after.has_value()) {
    target.lifecycle_after = source.lifecycle_after;
  }
  if (source.unit_before.has_value() && !target.unit_before.has_value()) {
    target.unit_before = source.unit_before;
  }
  if (source.unit_after.has_value()) {
    target.unit_after = source.unit_after;
  }
  if (source.envelope_before.has_value() && !target.envelope_before.has_value()) {
    target.envelope_before = source.envelope_before;
  }
  if (source.envelope_after.has_value()) {
    target.envelope_after = source.envelope_after;
  }
  target.aliases_added.insert(target.aliases_added.end(), source.aliases_added.begin(),
                             source.aliases_added.end());
  target.aliases_removed.insert(target.aliases_removed.end(), source.aliases_removed.begin(),
                                source.aliases_removed.end());
}

void normalize_aliases(LocationChange& change) {
  std::sort(change.aliases_added.begin(), change.aliases_added.end());
  change.aliases_added.erase(std::unique(change.aliases_added.begin(), change.aliases_added.end()),
                             change.aliases_added.end());
  std::sort(change.aliases_removed.begin(), change.aliases_removed.end());
  change.aliases_removed.erase(
      std::unique(change.aliases_removed.begin(), change.aliases_removed.end()),
      change.aliases_removed.end());
}

std::string kind_text(LocationKind kind) { return std::string(location_kind_name(kind)); }

}  // namespace

std::string_view change_flag_name(ChangeFlags flag) noexcept {
  for (const FlagName& entry : kFlagNames) {
    if (entry.flag == flag) {
      return entry.name;
    }
  }
  return "unrecognized";
}

std::string format_change_flags(ChangeFlags flags) {
  std::string text;
  for (const FlagName& entry : kFlagNames) {
    if ((flags & entry.flag) == 0) {
      continue;
    }
    if (!text.empty()) {
      text.push_back(',');
    }
    text.append(entry.name);
  }
  if (text.empty()) {
    text.append("generation");
  }
  return text;
}

std::string LocationChange::to_string() const {
  std::string text;
  text.append("id=");
  text.append(id.value());
  text.append(" kind=");
  text.append(kind_text(kind));
  text.append(" change=");
  text.append(format_change_flags(flags));
  if (path_before.has_value() || path_after.has_value()) {
    text.append(" path=");
    text.append(path_before.has_value() ? path_before->to_string() : std::string("<none>"));
    text.append("->");
    text.append(path_after.has_value() ? path_after->to_string() : std::string("<none>"));
  }
  if (generation_before.published() || generation_after.published()) {
    text.append(" generation=");
    text.append(std::to_string(generation_before.value()));
    text.append("->");
    text.append(std::to_string(generation_after.value()));
  }
  if (lifecycle_before.has_value() || lifecycle_after.has_value()) {
    text.append(" lifecycle=");
    text.append(lifecycle_before.has_value() ? std::string(lifecycle_state_name(*lifecycle_before))
                                             : std::string("<none>"));
    text.append("->");
    text.append(lifecycle_after.has_value() ? std::string(lifecycle_state_name(*lifecycle_after))
                                            : std::string("<none>"));
  }
  if (label_before.has_value() || label_after.has_value()) {
    text.append(" label=\"");
    text.append(label_before.value_or(std::string()));
    text.append("\"->\"");
    text.append(label_after.value_or(std::string()));
    text.push_back('"');
  }
  if (parent_before.has_value() || parent_after.has_value()) {
    text.append(" parent=");
    text.append(parent_before.has_value() ? parent_before->str() : std::string("<root>"));
    text.append("->");
    text.append(parent_after.has_value() ? parent_after->str() : std::string("<root>"));
  }
  return text;
}

std::string RevisionDiff::to_string() const {
  std::string text;
  text.append("from-revision=");
  text.append(std::to_string(from_revision.value()));
  text.append(" to-revision=");
  text.append(std::to_string(to_revision.value()));
  text.append(" created=");
  text.append(std::to_string(created));
  text.append(" changed=");
  text.append(std::to_string(changed));
  text.append(" removed=");
  text.append(std::to_string(removed));
  text.append(" truncated=");
  text.append(truncated ? "true" : "false");
  for (const LocationChange& change : changes) {
    text.push_back('\n');
    text.append(change.to_string());
  }
  return text;
}

std::string format_revision_diff(const RevisionDiff& diff) { return diff.to_string(); }

void Registry::Journal::append(Entry entry, std::uint32_t max_entries) {
  entries.push_back(std::move(entry));
  while (entries.size() > static_cast<std::size_t>(max_entries)) {
    entries.pop_front();
  }
}

Result<RevisionDiff> Registry::Journal::diff(LocationRevision from, LocationRevision to) const {
  if (to < from) {
    return Error(ErrorCode::InvalidArgument, "diff range ends before it starts")
        .with_subject(std::to_string(from.value()) + ".." + std::to_string(to.value()));
  }
  RevisionDiff result;
  result.from_revision = from;
  result.to_revision = to;

  if (entries.empty()) {
    if (from == to) {
      return result;
    }
    return Error(ErrorCode::RevisionNotRetained,
                 "no revision history is retained in this session; compare published state files "
                 "instead")
        .with_subject(std::to_string(from.value()));
  }

  // Revisions are contiguous: the window holds entries for
  // [front().revision, back().revision], so the oldest diffable start is one
  // revision before the oldest retained entry.
  const LocationRevision newest = entries.back().revision;
  const LocationRevision oldest_start = entries.front().revision.value() == 0
                                            ? LocationRevision(0)
                                            : LocationRevision(entries.front().revision.value() - 1U);
  if (to > newest) {
    return Error(ErrorCode::InvalidArgument, "diff range ends after the committed revision")
        .with_subject(std::to_string(to.value()) + " vs " + std::to_string(newest.value()));
  }
  if (from < oldest_start) {
    return Error(ErrorCode::RevisionNotRetained,
                 "the requested start revision is older than the retained diff window; compare "
                 "published state files instead")
        .with_subject(std::to_string(from.value()) + " vs retained from " +
                      std::to_string(oldest_start.value()));
  }

  std::map<LocationId, LocationChange, std::less<>> merged;
  for (const Entry& entry : entries) {
    if (entry.revision <= from || entry.revision > to) {
      continue;
    }
    result.created += entry.created;
    result.changed += entry.changed;
    if (entry.truncated) {
      result.truncated = true;
      continue;
    }
    for (const LocationChange& change : entry.changes) {
      const auto existing = merged.find(change.id);
      if (existing == merged.end()) {
        merged.emplace(change.id, change);
      } else {
        merge_change(existing->second, change);
      }
    }
  }

  result.changes.reserve(merged.size());
  for (auto& item : merged) {
    normalize_aliases(item.second);
    result.changes.push_back(std::move(item.second));
  }
  return result;
}

std::vector<LocationRevision> Registry::Journal::retained_revisions() const {
  std::vector<LocationRevision> revisions;
  if (entries.empty()) {
    return revisions;
  }
  const LocationRevision oldest_start = entries.front().revision.value() == 0
                                            ? LocationRevision(0)
                                            : LocationRevision(entries.front().revision.value() - 1U);
  for (std::uint64_t value = oldest_start.value(); value <= entries.back().revision.value(); ++value) {
    revisions.emplace_back(value);
  }
  return revisions;
}

void Registry::record_journal(LocationRevision revision,
                              const MutationContext& context,
                              std::vector<LocationChange> changes,
                              std::uint32_t created,
                              std::uint32_t changed) {
  Journal::Entry entry;
  entry.revision = revision;
  entry.at = context.at;
  entry.actor = context.actor;
  entry.reason = context.reason;
  entry.created = created;
  entry.changed = changed;

  std::sort(changes.begin(), changes.end(),
            [](const LocationChange& left, const LocationChange& right) { return left.id < right.id; });
  if (changes.size() > static_cast<std::size_t>(kMaxJournalChangesPerEntry)) {
    changes.resize(static_cast<std::size_t>(kMaxJournalChangesPerEntry));
    entry.truncated = true;
  }
  entry.changes = std::move(changes);
  journal_->append(std::move(entry), model_->limits().max_retained_revisions);
}

namespace internal {

LocationChange lifecycle_change(const LocationRecord& record,
                                const LocationPath& path,
                                LifecycleState before,
                                LifecycleState after) {
  LocationChange change;
  change.id = record.id;
  change.kind = record.kind;
  change.flags = kChangeLifecycle;
  change.path_before = path;
  change.path_after = path;
  change.generation_after = record.generation;
  change.lifecycle_before = before;
  change.lifecycle_after = after;
  return change;
}

}  // namespace internal

RevisionDiff diff_snapshots(const Snapshot& from, const Snapshot& to) {  RevisionDiff diff;
  diff.from_revision = from.revision();
  diff.to_revision = to.revision();

  const Snapshot::Impl& left = SnapshotAccess::get(from);
  const Snapshot::Impl& right = SnapshotAccess::get(to);

  std::map<LocationId, LocationChange, std::less<>> merged;

  for (const auto& entry : left.records) {
    const LocationRecord& before = entry.second;
    const LocationRecord* after = right.find(entry.first);
    if (after == nullptr) {
      LocationChange change;
      change.id = entry.first;
      change.kind = before.kind;
      change.flags = kChangeRemoved;
      const auto removed_path = left.path_of(before);
      if (removed_path.has_value()) {
        change.path_before = removed_path.value();
      }
      change.generation_before = before.generation;
      change.lifecycle_before = before.lifecycle;
      change.label_before = before.label;
      merged.emplace(entry.first, std::move(change));
      ++diff.removed;
      continue;
    }

    LocationChange change;
    change.id = entry.first;
    change.kind = after->kind;
    change.generation_before = before.generation;
    change.generation_after = after->generation;
    if (before.kind != after->kind) {
      change.flags |= kChangeKindChanged;
    }
    if (before.parent != after->parent) {
      change.flags |= kChangeReparented;
      change.parent_before = before.parent;
      change.parent_after = after->parent;
    }
    if (!(before.component == after->component)) {
      change.flags |= kChangeReaddressed;
    }
    if (before.label != after->label) {
      change.flags |= kChangeRelabeled;
      change.label_before = before.label;
      change.label_after = after->label;
    }
    if (before.lifecycle != after->lifecycle) {
      change.flags |= kChangeLifecycle;
      change.lifecycle_before = before.lifecycle;
      change.lifecycle_after = after->lifecycle;
    }
    if (before.unit != after->unit || before.envelope != after->envelope) {
      change.flags |= kChangeRackGeometry;
      change.unit_before = before.unit;
      change.unit_after = after->unit;
      change.envelope_before = before.envelope;
      change.envelope_after = after->envelope;
    }
    if (before.replaces != after->replaces || before.replaced_by != after->replaced_by) {
      change.flags |= kChangeReplacementLineage;
    }
    for (const std::string& alias : after->aliases) {
      if (std::find(before.aliases.begin(), before.aliases.end(), alias) == before.aliases.end()) {
        change.flags |= kChangeAliasAdded;
        change.aliases_added.push_back(alias);
      }
    }
    for (const std::string& alias : before.aliases) {
      if (std::find(after->aliases.begin(), after->aliases.end(), alias) == after->aliases.end()) {
        change.flags |= kChangeAliasRemoved;
        change.aliases_removed.push_back(alias);
      }
    }

    if (change.flags == 0 && before.generation == after->generation) {
      continue;
    }

    const auto before_path = left.path_of(before);
    if (before_path.has_value()) {
      change.path_before = before_path.value();
    }
    const auto after_path = right.path_of(*after);
    if (after_path.has_value()) {
      change.path_after = after_path.value();
    }
    merged.emplace(entry.first, std::move(change));
    ++diff.changed;
  }

  for (const auto& entry : right.records) {
    if (left.find(entry.first) != nullptr) {
      continue;
    }
    const LocationRecord& after = entry.second;
    LocationChange change;
    change.id = entry.first;
    change.kind = after.kind;
    change.flags = kChangeCreated;
    const auto path = right.path_of(after);
    if (path.has_value()) {
      change.path_after = path.value();
    }
    change.generation_after = after.generation;
    change.lifecycle_after = after.lifecycle;
    change.label_after = after.label;
    change.parent_after = after.parent;
    merged.emplace(entry.first, std::move(change));
    ++diff.created;
  }

  diff.changes.reserve(merged.size());
  for (auto& item : merged) {
    normalize_aliases(item.second);
    diff.changes.push_back(std::move(item.second));
  }
  return diff;
}

}  // namespace dccp::physical_location_registry
