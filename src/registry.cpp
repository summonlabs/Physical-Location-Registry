// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "dccp/physical_location_registry/registry.hpp"

#include <shared_mutex>
#include <string>
#include <utility>

#include "dccp/physical_location_registry/text.hpp"
#include "registry_internal.hpp"

namespace dccp::physical_location_registry {
namespace {

StoreOptions to_store_options(const RegistryOpenOptions& options) {
  StoreOptions store_options;
  store_options.mode = options.mode;
  store_options.create_if_missing = options.create_if_missing;
  store_options.fsync_state_before_publish = options.fsync_state_before_publish;
  store_options.fsync_directory_after_rename = options.fsync_directory_after_rename;
  store_options.requested_limits = options.limits;
  store_options.store_id = options.store_id;
  store_options.faults = options.faults;
  return store_options;
}

}  // namespace

Result<std::shared_ptr<Registry>> Registry::adopt(std::shared_ptr<Store> store,
                                                  const std::filesystem::path& directory) {
  auto registry = std::shared_ptr<Registry>(new Registry());
  registry->store_ = std::move(store);
  registry->directory_ = directory;
  registry->journal_ = std::make_unique<Journal>();

  PLR_TRY(snapshot, registry->store_->load());
  registry->model_ = std::make_unique<Snapshot>(std::move(snapshot));
  return registry;
}

Result<std::shared_ptr<Registry>> Registry::create(const std::filesystem::path& directory,
                                                   const RegistryOpenOptions& options) {
  RegistryOpenOptions effective = options;
  effective.mode = OpenMode::ReadWrite;
  effective.create_if_missing = true;
  PLR_TRY(store, Store::create(directory, to_store_options(effective)));
  return adopt(std::move(store), directory);
}

Result<std::shared_ptr<Registry>> Registry::open(const std::filesystem::path& directory,
                                                 const RegistryOpenOptions& options) {
  PLR_TRY(store, Store::open(directory, to_store_options(options)));
  return adopt(std::move(store), directory);
}

Registry::~Registry() {
  if (store_) {
    (void)store_->close();
  }
}

bool Registry::is_writer() const noexcept { return store_ != nullptr && store_->is_writer(); }

bool Registry::closed() const noexcept {
  std::shared_lock lock(mutex_);
  return closed_;
}

StoreId Registry::store_id() const {
  std::shared_lock lock(mutex_);
  return model_->store_id();
}

LocationRevision Registry::revision() const {
  std::shared_lock lock(mutex_);
  return model_->revision();
}

StateSequence Registry::sequence() const {
  std::shared_lock lock(mutex_);
  return model_->sequence();
}

WriterEpoch Registry::epoch() const {
  std::shared_lock lock(mutex_);
  return model_->epoch();
}

Limits Registry::limits() const {
  std::shared_lock lock(mutex_);
  return model_->limits();
}

RecoveryReport Registry::recovery() const {
  std::shared_lock lock(mutex_);
  return store_->recovery();
}

std::optional<MutationAuthority> Registry::authority() const {
  std::shared_lock lock(mutex_);
  if (!is_writer()) {
    return std::nullopt;
  }
  MutationAuthority authority;
  authority.store_id = model_->store_id();
  authority.writer_epoch = model_->epoch();
  return authority;
}

std::string Registry::state_file_name() const {
  std::shared_lock lock(mutex_);
  return store_->state_file_name();
}

std::uint64_t Registry::state_bytes() const {
  std::shared_lock lock(mutex_);
  return store_->state_bytes();
}

std::string Registry::state_digest() const {
  std::shared_lock lock(mutex_);
  return store_->state_digest();
}

Result<LocationView> Registry::find(const LocationId& id) const {
  std::shared_lock lock(mutex_);
  if (closed_) {
    return Error(ErrorCode::SessionClosed, "this registry session is closed");
  }
  return model_->find(id);
}

Result<ResolutionResult> Registry::resolve(const LocationPath& path, ResolutionMode mode) const {
  std::shared_lock lock(mutex_);
  if (closed_) {
    return Error(ErrorCode::SessionClosed, "this registry session is closed");
  }
  return model_->resolve(path, mode);
}

Result<LocationPath> Registry::path_of(const LocationId& id) const {
  std::shared_lock lock(mutex_);
  if (closed_) {
    return Error(ErrorCode::SessionClosed, "this registry session is closed");
  }
  return model_->path_of(id);
}

Result<std::vector<ChildEntry>> Registry::children(const LocationId& id) const {
  std::shared_lock lock(mutex_);
  if (closed_) {
    return Error(ErrorCode::SessionClosed, "this registry session is closed");
  }
  return model_->children(id);
}

Result<std::vector<ChildEntry>> Registry::descendants(const LocationId& id,
                                                      std::uint32_t max_depth) const {
  std::shared_lock lock(mutex_);
  if (closed_) {
    return Error(ErrorCode::SessionClosed, "this registry session is closed");
  }
  return model_->descendants(id, max_depth);
}

Result<std::vector<ChildEntry>> Registry::roots() const {
  std::shared_lock lock(mutex_);
  if (closed_) {
    return Error(ErrorCode::SessionClosed, "this registry session is closed");
  }
  return model_->roots();
}

Result<std::vector<LocationView>> Registry::list(const ListOptions& options) const {
  std::shared_lock lock(mutex_);
  if (closed_) {
    return Error(ErrorCode::SessionClosed, "this registry session is closed");
  }
  return model_->list(options);
}

Result<std::vector<AliasBinding>> Registry::aliases() const {
  std::shared_lock lock(mutex_);
  if (closed_) {
    return Error(ErrorCode::SessionClosed, "this registry session is closed");
  }
  return model_->aliases();
}

LocationStatistics Registry::statistics() const {
  std::shared_lock lock(mutex_);
  if (closed_) {
    return LocationStatistics{};
  }
  return model_->statistics();
}

Result<RevisionDiff> Registry::diff(LocationRevision from, LocationRevision to) const {
  std::shared_lock lock(mutex_);
  if (closed_) {
    return Error(ErrorCode::SessionClosed, "this registry session is closed");
  }
  return journal_->diff(from, to);
}

std::vector<LocationRevision> Registry::retained_revisions() const {
  std::shared_lock lock(mutex_);
  if (closed_) {
    return {};
  }
  return journal_->retained_revisions();
}

std::shared_ptr<const Snapshot> Registry::copy_snapshot() const {
  std::shared_lock lock(mutex_);
  if (closed_) {
    return nullptr;
  }
  return std::make_shared<const Snapshot>(SnapshotAccess::clone(*model_));
}

Result<std::string> Registry::encode_current_state() const {
  std::shared_lock lock(mutex_);
  if (closed_) {
    return Error(ErrorCode::SessionClosed, "this registry session is closed");
  }
  return encode_snapshot(*model_);
}

Result<void> Registry::verify_storage() const {
  std::shared_lock lock(mutex_);
  if (closed_) {
    return Error(ErrorCode::SessionClosed, "this registry session is closed");
  }
  return store_->verify();
}

Explanation Registry::explain_resolve(const LocationPath& path, ResolutionMode mode) const {
  std::shared_lock lock(mutex_);
  if (closed_) {
    return explain_error_code(ErrorCode::SessionClosed);
  }
  auto resolved = model_->resolve(path, mode);
  if (!resolved.has_value()) {
    Explanation explanation = explain_error(resolved.error());
    explanation.details.push_back("requested address: " + path.to_string());
    explanation.details.push_back(std::string("resolution mode: ") +
                                  (mode == ResolutionMode::CurrentOnly ? "current-only"
                                                                       : "include-retired"));
    return explanation;
  }
  Explanation explanation;
  explanation.code = ErrorCode::Ok;
  explanation.category = ErrorCategory::Ok;
  explanation.summary = path.to_string() + " resolves to " + resolved.value().id.str() + " (" +
                        std::string(resolution_kind_name(resolved.value().kind)) + ")";
  explanation.details.push_back("canonical address: " + resolved.value().canonical_path.to_string());
  explanation.details.push_back(std::string("lifecycle: ") +
                                std::string(lifecycle_state_name(resolved.value().lifecycle)));
  explanation.details.push_back("generation: " +
                                std::to_string(resolved.value().generation.value()));
  return explanation;
}

Result<void> Registry::close() {
  std::unique_lock lock(mutex_);
  if (closed_) {
    return ok();
  }
  closed_ = true;
  PLR_CHECK(store_->close());
  return ok();
}

namespace internal {

Result<void> validate_context(const MutationContext& context, const Limits& limits) {
  if (context.actor.empty()) {
    return Error(ErrorCode::InvalidArgument,
                 "a mutation must name the actor it is attributed to");
  }
  if (context.reason.size() > static_cast<std::size_t>(limits.max_reason_bytes) ||
      !is_valid_reason(context.reason)) {
    return Error(ErrorCode::MalformedText, "reason is malformed or longer than the configured maximum")
        .with_subject(context.reason.substr(0, 160));
  }
  return ok();
}

Result<void> validate_authority(const MutationContext& context,
                                const StoreId& store_id,
                                WriterEpoch epoch) {
  if (!context.authority.has_value()) {
    return ok();
  }
  if (context.authority->store_id != store_id) {
    return Error(ErrorCode::StoreMismatch,
                 "the supplied authority belongs to a different store")
        .with_subject(context.authority->store_id.str());
  }
  if (context.authority->writer_epoch != epoch) {
    return Error(ErrorCode::StaleAuthorityEpoch,
                 "the supplied authority carries writer epoch " +
                     std::to_string(context.authority->writer_epoch.value()) +
                     " but the current epoch is " + std::to_string(epoch.value()))
        .with_subject(std::to_string(context.authority->writer_epoch.value()));
  }
  return ok();
}

}  // namespace internal

}  // namespace dccp::physical_location_registry
