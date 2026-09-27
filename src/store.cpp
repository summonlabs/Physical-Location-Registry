// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "dccp/physical_location_registry/store.hpp"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <optional>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include "dccp/physical_location_registry/digest.hpp"
#include "dccp/physical_location_registry/text.hpp"
#include "model.hpp"
#include "platform.hpp"

namespace dccp::physical_location_registry {
namespace {

constexpr std::string_view kHeadFile = "head";
constexpr std::string_view kHeadTempFile = "head.new";
constexpr std::string_view kLockFile = "store.lock";
constexpr std::string_view kStatePrefix = "state.";
constexpr std::string_view kStateSuffix = ".plr";
constexpr std::string_view kStateTempSuffix = ".plr.new";
constexpr std::string_view kHeadMagic = "PLRHEAD1";
constexpr std::size_t kMaxHeadBytes = 512;
constexpr std::size_t kMaxFileNameBytes = 64;

struct HeadRecord {
  StateSequence sequence;
  StoreId store_id;
  LocationRevision revision;
  WriterEpoch epoch;
  std::string file_name;
};

std::string publication_file_name(StateSequence sequence) {
  return std::string(kStatePrefix) + std::to_string(sequence.value()) + std::string(kStateSuffix);
}

std::string state_temp_file_name(StateSequence sequence) {
  return std::string(kStatePrefix) + std::to_string(sequence.value()) + std::string(kStateTempSuffix);
}

/// True when the file name is exactly "state.<digits>.plr"; the sequence is
/// returned through the out parameter.
bool parse_state_file_name(std::string_view name, std::uint64_t& sequence) {
  if (name.size() <= kStatePrefix.size() + kStateSuffix.size()) {
    return false;
  }
  if (name.substr(0, kStatePrefix.size()) != kStatePrefix) {
    return false;
  }
  if (name.substr(name.size() - kStateSuffix.size()) != kStateSuffix) {
    return false;
  }
  const std::string_view digits = name.substr(kStatePrefix.size(),
                                              name.size() - kStatePrefix.size() - kStateSuffix.size());
  if (digits.empty() || digits.size() > 20) {
    return false;
  }
  std::uint64_t value = 0;
  for (const char character : digits) {
    if (character < '0' || character > '9') {
      return false;
    }
    const std::uint64_t digit = static_cast<std::uint64_t>(character - '0');
    if (value > (UINT64_MAX - digit) / 10U) {
      return false;
    }
    value = value * 10U + digit;
  }
  if (value == 0) {
    return false;
  }
  sequence = value;
  return true;
}

bool is_temp_file_name(std::string_view name) {
  if (name == kHeadTempFile) {
    return true;
  }
  return name.size() > kStateTempSuffix.size() &&
         name.substr(name.size() - kStateTempSuffix.size()) == kStateTempSuffix;
}

std::string format_head(const HeadRecord& head) {
  std::string text(kHeadMagic);
  text.append(" seq=");
  text.append(std::to_string(head.sequence.value()));
  text.append(" store=");
  text.append(head.store_id.value());
  text.append(" rev=");
  text.append(std::to_string(head.revision.value()));
  text.append(" epoch=");
  text.append(std::to_string(head.epoch.value()));
  text.append(" file=");
  text.append(head.file_name);
  text.push_back('\n');
  return text;
}

Result<std::uint64_t> parse_decimal_field(std::string_view text, const char* field_name) {
  if (text.empty() || text.size() > 20) {
    return Error(ErrorCode::HeadCorrupt, std::string("head field ") + field_name + " is not a number")
        .with_subject(std::string(text.substr(0, 64)));
  }
  std::uint64_t value = 0;
  for (const char character : text) {
    if (character < '0' || character > '9') {
      return Error(ErrorCode::HeadCorrupt,
                   std::string("head field ") + field_name + " is not a number")
          .with_subject(std::string(text.substr(0, 64)));
    }
    const std::uint64_t digit = static_cast<std::uint64_t>(character - '0');
    if (value > (UINT64_MAX - digit) / 10U) {
      return Error(ErrorCode::HeadCorrupt,
                   std::string("head field ") + field_name + " overflows")
          .with_subject(std::string(text.substr(0, 64)));
    }
    value = value * 10U + digit;
  }
  return value;
}

/// Strict parse of the head line. Unknown, missing, duplicated or reordered
/// fields are rejected rather than ignored.
Result<HeadRecord> parse_head(std::string_view text) {
  if (text.size() > kMaxHeadBytes) {
    return Error(ErrorCode::HeadCorrupt, "head is longer than the permitted maximum");
  }
  if (text.empty() || text.back() != '\n') {
    return Error(ErrorCode::HeadCorrupt, "head must be a single newline-terminated line");
  }
  const std::string_view line = text.substr(0, text.size() - 1);
  if (line.find('\n') != std::string_view::npos || line.find('\r') != std::string_view::npos) {
    return Error(ErrorCode::HeadCorrupt, "head must be a single line");
  }
  if (line.substr(0, kHeadMagic.size()) != kHeadMagic) {
    return Error(ErrorCode::HeadCorrupt, "head does not begin with the expected marker");
  }
  std::string_view rest = line.substr(kHeadMagic.size());

  const auto take_field = [&rest](std::string_view key) -> Result<std::string_view> {
    while (!rest.empty() && rest.front() == ' ') {
      rest.remove_prefix(1);
    }
    if (rest.size() < key.size() + 1 || rest.substr(0, key.size()) != key || rest[key.size()] != '=') {
      return Error(ErrorCode::HeadCorrupt, "head field " + std::string(key) + " is missing or out of order");
    }
    rest.remove_prefix(key.size() + 1);
    const std::size_t space = rest.find(' ');
    const std::string_view value = space == std::string_view::npos ? rest : rest.substr(0, space);
    rest = space == std::string_view::npos ? std::string_view() : rest.substr(space + 1);
    return value;
  };

  PLR_TRY(sequence_text, take_field("seq"));
  PLR_TRY(sequence_value, parse_decimal_field(sequence_text, "seq"));
  PLR_TRY(store_text, take_field("store"));
  PLR_TRY(store_id, StoreId::parse(store_text));
  PLR_TRY(revision_text, take_field("rev"));
  PLR_TRY(revision_value, parse_decimal_field(revision_text, "rev"));
  PLR_TRY(epoch_text, take_field("epoch"));
  PLR_TRY(epoch_value, parse_decimal_field(epoch_text, "epoch"));
  PLR_TRY(file_text, take_field("file"));
  if (!rest.empty()) {
    return Error(ErrorCode::HeadCorrupt, "head carries trailing fields that are not understood");
  }
  if (file_text.empty() || file_text.size() > kMaxFileNameBytes) {
    return Error(ErrorCode::HeadCorrupt, "head file name is empty or too long");
  }
  if (file_text.find('/') != std::string_view::npos || file_text.find('\\') != std::string_view::npos ||
      file_text == "." || file_text == "..") {
    return Error(ErrorCode::HeadCorrupt, "head file name is not a plain file name")
        .with_subject(std::string(file_text));
  }
  std::uint64_t file_sequence = 0;
  if (!parse_state_file_name(file_text, file_sequence)) {
    return Error(ErrorCode::HeadCorrupt, "head file name is not a canonical publication name")
        .with_subject(std::string(file_text));
  }
  if (file_sequence != sequence_value) {
    return Error(ErrorCode::HeadCorrupt, "head sequence and publication name disagree")
        .with_subject(std::string(file_text));
  }
  if (sequence_value < StateSequence::kFirstPublication) {
    return Error(ErrorCode::HeadCorrupt, "head sequence must be at least 1");
  }

  HeadRecord head;
  head.sequence = StateSequence(sequence_value);
  head.store_id = store_id;
  head.revision = LocationRevision(revision_value);
  head.epoch = WriterEpoch(epoch_value);
  head.file_name = std::string(file_text);
  return head;
}

std::string generate_store_id() {
  std::random_device device;
  static constexpr char kAlphabet[] = "abcdefghijklmnopqrstuvwxyz0123456789";
  std::string id = "store-";
  for (int index = 0; index < 26; ++index) {
    id.push_back(kAlphabet[device() % 36U]);
  }
  return id;
}

Result<void> validate_fault_plan(const FaultPlan& plan) {
  if (plan.action == FaultAction::None) {
    if (plan.stage != PublishStage::None) {
      return Error(ErrorCode::InvalidArgument, "a fault stage requires a fault action");
    }
    return ok();
  }
  if (plan.stage == PublishStage::None) {
    return Error(ErrorCode::InvalidArgument, "a fault action requires a fault stage");
  }
  if (plan.action == FaultAction::Fail &&
      (plan.stage == PublishStage::AfterHeadPublish || plan.stage == PublishStage::BeforeRetire)) {
    return Error(ErrorCode::InvalidArgument,
                 "a failure cannot be reported after the commit point; use the crash action to "
                 "prove post-commit behavior");
  }
  return ok();
}

/// Full verification of framed canonical bytes: recomputes the integrity digest
/// and compares it with the stored trailer.
Result<void> verify_framed_digest(std::string_view bytes) {
  if (bytes.size() < kSha256Bytes) {
    return Error(ErrorCode::TruncatedInput, "state is too small to carry an integrity trailer");
  }
  Sha256 hasher;
  hasher.update(bytes.substr(0, bytes.size() - kSha256Bytes));
  std::uint8_t computed[kSha256Bytes];
  hasher.finish(computed);
  const std::string_view stored(bytes.data() + bytes.size() - kSha256Bytes, kSha256Bytes);
  if (!digest_equal(stored, std::string_view(reinterpret_cast<const char*>(computed), kSha256Bytes))) {
    return Error(ErrorCode::DigestMismatch,
                 "published state failed its integrity check; the file does not match its digest");
  }
  return ok();
}

}  // namespace

std::string_view open_mode_name(OpenMode mode) noexcept {
  switch (mode) {
    case OpenMode::ReadOnly:
      return "read-only";
    case OpenMode::ReadWrite:
      return "read-write";
  }
  return "unrecognized";
}

std::string_view publish_stage_name(PublishStage stage) noexcept {
  switch (stage) {
    case PublishStage::None:
      return "none";
    case PublishStage::BeforeStateWrite:
      return "before-state-write";
    case PublishStage::AfterStateWrite:
      return "after-state-write";
    case PublishStage::BeforeStateRename:
      return "before-state-rename";
    case PublishStage::BeforeHeadPublish:
      return "before-head-publish";
    case PublishStage::AfterHeadPublish:
      return "after-head-publish";
    case PublishStage::BeforeRetire:
      return "before-retire";
  }
  return "unrecognized";
}

std::string_view fault_action_name(FaultAction action) noexcept {
  switch (action) {
    case FaultAction::None:
      return "none";
    case FaultAction::Fail:
      return "fail";
    case FaultAction::Crash:
      return "crash";
  }
  return "unrecognized";
}

std::string RecoveryReport::to_string() const {
  std::string text;
  text.append("head-missing=");
  text.append(head_missing ? "true" : "false");
  text.append(" head-unreadable=");
  text.append(head_unreadable ? "true" : "false");
  text.append(" head-invalid=");
  text.append(head_invalid ? "true" : "false");
  text.append(" recovered-older-publication=");
  text.append(recovered_older_publication ? "true" : "false");
  text.append(" head-sequence=");
  text.append(std::to_string(head_sequence.value()));
  text.append(" recovered-sequence=");
  text.append(std::to_string(recovered_sequence.value()));
  text.append(" invalid-publications-skipped=");
  text.append(std::to_string(invalid_publications_skipped));
  text.append(" republished-head=");
  text.append(republished_head ? "true" : "false");
  for (const std::string& note : notes) {
    text.append("\nnote: ");
    text.append(note);
  }
  return text;
}

Result<StateSequence> Store::next_sequence() const { return sequence_.next(); }

namespace {

/// State loaded from disk plus what had to be done to obtain it.
struct LoadOutcome {
  std::unique_ptr<Snapshot::Impl> model;
  RecoveryReport report;
  bool created = false;
};

/// Reads and validates the publication referenced by head, or recovers the
/// newest valid publication when head is unusable.
Result<LoadOutcome> load_published_state(const std::filesystem::path& directory,
                                         std::uint64_t max_state_bytes) {
  LoadOutcome outcome;

  const auto head_path = directory / std::string(kHeadFile);
  std::optional<HeadRecord> head;

  auto head_exists = internal::regular_file_exists(head_path);
  if (!head_exists.has_value()) {
    return head_exists.error();
  }
  if (!head_exists.value()) {
    outcome.report.head_missing = true;
    outcome.report.notes.emplace_back("head is missing; scanning for the newest valid publication");
  } else {
    auto head_bytes = internal::read_file_bounded(head_path, kMaxHeadBytes);
    if (!head_bytes.has_value()) {
      outcome.report.head_unreadable = true;
      outcome.report.notes.emplace_back("head could not be read: " + head_bytes.error().message());
    } else {
      auto parsed = parse_head(head_bytes.value());
      if (!parsed.has_value()) {
        outcome.report.head_invalid = true;
        outcome.report.notes.emplace_back("head is not usable: " + parsed.error().message());
      } else {
        head = parsed.value();
        outcome.report.head_sequence = head->sequence;

        const auto state_path = directory / head->file_name;
        auto state_bytes = internal::read_file_bounded(state_path, max_state_bytes);
        if (!state_bytes.has_value()) {
          outcome.report.head_invalid = true;
          outcome.report.notes.emplace_back("head publication could not be read: " +
                                            state_bytes.error().message());
        } else if (auto verified = verify_framed_digest(state_bytes.value()); !verified.has_value()) {
          outcome.report.head_invalid = true;
          outcome.report.notes.emplace_back("head publication failed integrity verification: " +
                                            verified.error().message());
        } else {
          auto decoded = internal::decode_model_bytes(state_bytes.value());
          if (!decoded.has_value()) {
            outcome.report.head_invalid = true;
            outcome.report.notes.emplace_back("head publication could not be decoded: " +
                                              decoded.error().message());
          } else {
            const Snapshot::Impl& model = SnapshotAccess::get(decoded.value());
            if (model.store_id != head->store_id || model.sequence != head->sequence ||
                model.revision != head->revision || model.epoch != head->epoch) {
              outcome.report.head_invalid = true;
              outcome.report.notes.emplace_back(
                  "head metadata disagrees with the publication it points at");
            } else {
              outcome.model = std::make_unique<Snapshot::Impl>(model);
              return outcome;
            }
          }
        }
      }
    }
  }

  // Conservative recovery: only reached when head is unusable. The newest
  // publication that verifies end to end wins; nothing is repaired in place.
  auto names = internal::list_file_names(directory);
  if (!names.has_value()) {
    return names.error();
  }
  std::vector<std::uint64_t> sequences;
  for (const std::string& name : names.value()) {
    std::uint64_t sequence = 0;
    if (parse_state_file_name(name, sequence)) {
      sequences.push_back(sequence);
    }
  }
  std::sort(sequences.begin(), sequences.end(), std::greater<>());

  for (const std::uint64_t sequence : sequences) {
    const auto candidate_path = directory / publication_file_name(StateSequence(sequence));
    auto state_bytes = internal::read_file_bounded(candidate_path, max_state_bytes);
    if (!state_bytes.has_value()) {
      ++outcome.report.invalid_publications_skipped;
      continue;
    }
    if (auto verified = verify_framed_digest(state_bytes.value()); !verified.has_value()) {
      ++outcome.report.invalid_publications_skipped;
      continue;
    }
    auto decoded = internal::decode_model_bytes(state_bytes.value());
    if (!decoded.has_value()) {
      ++outcome.report.invalid_publications_skipped;
      continue;
    }
    const Snapshot::Impl& model = SnapshotAccess::get(decoded.value());
    outcome.model = std::make_unique<Snapshot::Impl>(model);
    outcome.report.recovered_older_publication = true;
    outcome.report.recovered_sequence = model.sequence;
    outcome.report.notes.emplace_back("recovered state from publication " +
                                      std::to_string(model.sequence.value()) +
                                      " at revision " + std::to_string(model.revision.value()));
    return outcome;
  }

  if (sequences.empty() && outcome.report.head_missing) {
    return Error(ErrorCode::StoreNotFound, "store directory contains no publication")
        .with_subject(directory.string());
  }
  return Error(ErrorCode::RecoveryUnavailable,
               "no publication in this store passed integrity verification; refusing to present "
               "unverified state as authoritative")
      .with_subject(directory.string());
}

}  // namespace

Result<std::shared_ptr<Store>> Store::create(const std::filesystem::path& directory,
                                             const StoreOptions& options) {
  if (options.mode != OpenMode::ReadWrite) {
    return Error(ErrorCode::InvalidArgument,
                 "a store is created by a writer session; request OpenMode::ReadWrite");
  }
  PLR_CHECK(validate_fault_plan(options.faults));
  Limits limits = options.requested_limits.value_or(Limits{});
  PLR_CHECK(limits.validate());

  PLR_CHECK(internal::ensure_directory(directory));
  auto lock = internal::acquire_exclusive_lock(directory / std::string(kLockFile));
  if (!lock.has_value()) {
    return lock.error();
  }

  // Refuse to create a store over anything that already looks like one.
  auto names = internal::list_file_names(directory);
  if (!names.has_value()) {
    return names.error();
  }
  for (const std::string& name : names.value()) {
    std::uint64_t sequence = 0;
    if (name == kHeadFile || parse_state_file_name(name, sequence)) {
      return Error(ErrorCode::StoreExists, "a store already exists in this directory")
          .with_subject(directory.string());
    }
  }

  auto store = std::shared_ptr<Store>(new Store());
  store->directory_ = directory;
  store->mode_ = OpenMode::ReadWrite;
  store->lock_ = lock.value();
  store->options_ = std::make_shared<const StoreOptions>(options);
  store->limits_ = limits;
  store->store_id_ = options.store_id.value_or(StoreId());
  if (store->store_id_.empty()) {
    PLR_TRY(generated, StoreId::parse(generate_store_id()));
    store->store_id_ = generated;
  }
  store->revision_ = LocationRevision(0);
  store->sequence_ = StateSequence(0);
  store->epoch_ = WriterEpoch(0);

  auto model = std::make_unique<Snapshot::Impl>();
  model->store_id = store->store_id_;
  model->revision = LocationRevision(0);
  model->sequence = StateSequence(StateSequence::kFirstPublication);
  PLR_TRY(epoch, store->epoch_.next());
  store->epoch_ = epoch;
  model->epoch = epoch;
  model->limits = limits;
  Snapshot initial = SnapshotAccess::make(std::move(model));

  PLR_CHECK(store->publish(initial));
  store->recovery_.notes.emplace_back("store created at publication " +
                                      std::to_string(store->sequence_.value()));
  return store;
}

Result<std::shared_ptr<Store>> Store::open(const std::filesystem::path& directory,
                                           const StoreOptions& options) {
  PLR_CHECK(validate_fault_plan(options.faults));

  auto exists = internal::directory_exists(directory);
  if (!exists.has_value()) {
    return exists.error();
  }
  if (!exists.value()) {
    if (!options.create_if_missing) {
      return Error(ErrorCode::StoreNotFound, "store directory does not exist")
          .with_subject(directory.string());
    }
    return create(directory, options);
  }

  if (options.create_if_missing && options.mode == OpenMode::ReadWrite) {
    auto names = internal::list_file_names(directory);
    if (!names.has_value()) {
      return names.error();
    }
    bool looks_like_store = false;
    for (const std::string& name : names.value()) {
      std::uint64_t sequence = 0;
      if (name == kHeadFile || parse_state_file_name(name, sequence)) {
        looks_like_store = true;
        break;
      }
    }
    if (!looks_like_store) {
      return create(directory, options);
    }
  }

  auto store = std::shared_ptr<Store>(new Store());
  store->directory_ = directory;
  store->mode_ = options.mode;
  store->options_ = std::make_shared<const StoreOptions>(options);

  if (options.mode == OpenMode::ReadWrite) {
    auto lock = internal::acquire_exclusive_lock(directory / std::string(kLockFile));
    if (!lock.has_value()) {
      return lock.error();
    }
    store->lock_ = lock.value();
  }

  const std::uint64_t read_bound =
      static_cast<std::uint64_t>(kHardMaxStateBytes) + 4096ULL;
  auto loaded = load_published_state(directory, read_bound);
  if (!loaded.has_value()) {
    return loaded.error();
  }
  store->recovery_ = loaded.value().report;
  store->store_id_ = loaded.value().model->store_id;
  store->revision_ = loaded.value().model->revision;
  store->sequence_ = loaded.value().model->sequence;
  store->epoch_ = loaded.value().model->epoch;
  store->limits_ = loaded.value().model->limits;

  if (options.requested_limits.has_value() && options.requested_limits.value() != store->limits_) {
    return Error(ErrorCode::LimitsMismatch,
                 "the requested limits differ from the limits this store was created with")
        .with_subject(directory.string());
  }

  if (options.mode == OpenMode::ReadOnly) {
    auto state_bytes =
        internal::read_file_bounded(directory / publication_file_name(store->sequence_), read_bound);
    if (!state_bytes.has_value()) {
      return state_bytes.error();
    }
    store->state_bytes_ = static_cast<std::uint64_t>(state_bytes.value().size());
    store->state_digest_ = Sha256::hex(state_bytes.value());
  }

  if (options.mode == OpenMode::ReadWrite) {
    // Advance the durable writer epoch before any mutation is authorized, and
    // heal an unusable head in the same publication.
    PLR_TRY(epoch, store->epoch_.next());
    store->epoch_ = epoch;
    auto model = std::make_unique<Snapshot::Impl>(*loaded.value().model);
    model->epoch = epoch;
    PLR_TRY(sequence, store->next_sequence());
    model->sequence = sequence;
    Snapshot authority_state = SnapshotAccess::make(std::move(model));
    PLR_CHECK(store->publish(authority_state));
    if (!store->recovery_.clean()) {
      store->recovery_.republished_head = true;
      store->recovery_.notes.emplace_back(
          "writer session published a recovered state and advanced the writer epoch to " +
          std::to_string(store->epoch_.value()));
    }
  }

  if (options.mode == OpenMode::ReadWrite) {
    // Cleanup is best effort: a reader may hold an old publication open, and the
    // next writer session retries.
    auto names = internal::list_file_names(directory);
    if (names.has_value()) {
      for (const std::string& name : names.value()) {
        if (is_temp_file_name(name)) {
          (void)internal::remove_file(directory / name);
        }
      }
    }
  }

  return store;
}

Store::~Store() {
  if (lock_ && lock_->held) {
    (void)internal::release_lock(lock_);
  }
}

std::string Store::state_file_name() const { return publication_file_name(sequence_); }

Result<std::string> Store::read_published_state() const {
  return internal::read_file_bounded(directory_ / publication_file_name(sequence_),
                                     static_cast<std::uint64_t>(kHardMaxStateBytes) + 4096ULL);
}

Result<Snapshot> Store::load() const {
  PLR_TRY(bytes, read_published_state());
  return internal::decode_model_bytes(bytes);
}

Result<void> Store::verify() const {
  const auto head_path = directory_ / std::string(kHeadFile);
  PLR_TRY(head_bytes, internal::read_file_bounded(head_path, kMaxHeadBytes));
  PLR_TRY(head, parse_head(head_bytes));
  PLR_TRY(state_bytes, internal::read_file_bounded(directory_ / head.file_name,
                                                   static_cast<std::uint64_t>(kHardMaxStateBytes) + 4096ULL));
  PLR_CHECK(verify_framed_digest(state_bytes));
  PLR_TRY(snapshot, internal::decode_model_bytes(state_bytes));
  const Snapshot::Impl& model = SnapshotAccess::get(snapshot);
  if (model.store_id != head.store_id || model.sequence != head.sequence ||
      model.revision != head.revision || model.epoch != head.epoch) {
    return Error(ErrorCode::HeadCorrupt,
                 "head metadata disagrees with the publication it points at")
        .with_subject(head.file_name);
  }
  return ok();
}

Result<StateSequence> Store::publish(const Snapshot& snapshot) {
  if (closed_) {
    return Error(ErrorCode::SessionClosed, "this store handle has been closed");
  }
  if (!is_writer()) {
    return Error(ErrorCode::SessionReadOnly, "read-only store sessions cannot publish");
  }
  const Snapshot::Impl& model = SnapshotAccess::get(snapshot);
  if (model.store_id != store_id_) {
    return Error(ErrorCode::StoreMismatch, "state belongs to a different store")
        .with_subject(model.store_id.str());
  }
  if (model.limits != limits_) {
    return Error(ErrorCode::LimitsMismatch, "state declares limits that differ from the store limits");
  }
  if (model.revision < revision_) {
    return Error(ErrorCode::StaleRevision, "state revision is older than the published revision")
        .with_subject(std::to_string(model.revision.value()));
  }
  if (model.epoch != epoch_) {
    return Error(ErrorCode::StaleAuthorityEpoch,
                 "state was prepared under a different writer epoch")
        .with_subject(std::to_string(model.epoch.value()));
  }
  PLR_TRY(next, next_sequence());
  if (model.sequence != next) {
    return Error(ErrorCode::InternalError, "state sequence is not the next publication sequence")
        .with_subject(std::to_string(model.sequence.value()));
  }
  PLR_CHECK(model.validate_state_bounds());

  PLR_TRY(encoded, internal::encode_model(model));
  if (encoded.size() > static_cast<std::size_t>(limits_.max_state_bytes) + 64U) {
    return Error(ErrorCode::LimitExceeded,
                 "encoded state exceeds the configured maximum of " +
                     std::to_string(limits_.max_state_bytes) + " bytes")
        .with_subject(std::to_string(encoded.size()));
  }

  const auto plans = options_ != nullptr ? *options_ : StoreOptions{};
  const auto inject = [&](PublishStage stage) -> Result<void> {
    if (plans.faults.action == FaultAction::None || plans.faults.stage != stage) {
      return ok();
    }
    if (plans.faults.publication_ordinal != 0 &&
        plans.faults.publication_ordinal != static_cast<std::uint32_t>(publications_) + 1U) {
      return ok();
    }
    if (plans.faults.action == FaultAction::Crash) {
      internal::abrupt_exit(internal::kInjectedCrashExitCode);
    }
    return Error(ErrorCode::InjectedFault,
                 std::string("injected failure at publish stage ") +
                     std::string(publish_stage_name(stage)));
  };

  const std::string final_name = publication_file_name(model.sequence);
  const std::string temp_name = state_temp_file_name(model.sequence);
  const auto temp_path = directory_ / temp_name;
  const auto final_path = directory_ / final_name;

  PLR_CHECK(inject(PublishStage::BeforeStateWrite));
  PLR_CHECK(internal::write_file(temp_path, encoded, plans.fsync_state_before_publish));

  PLR_CHECK(inject(PublishStage::AfterStateWrite));
  PLR_CHECK(inject(PublishStage::BeforeStateRename));

  // Give the publication its permanent name. It is not authoritative yet: the
  // head pointer still names the previous generation.
  PLR_CHECK(internal::replace_file(temp_path, final_path, false));

  if (plans.verify_written_state) {
    PLR_TRY(written_back, internal::read_file_bounded(final_path,
                                                      static_cast<std::uint64_t>(limits_.max_state_bytes) + 64ULL));
    if (written_back.size() != encoded.size()) {
      (void)internal::remove_file(final_path);
      return Error(ErrorCode::IntegrityFailure,
                   "publication read back a different size than it was written with")
          .with_subject(final_name);
    }
    if (auto verified = verify_framed_digest(written_back); !verified.has_value()) {
      (void)internal::remove_file(final_path);
      return verified.error();
    }
  }

  PLR_CHECK(inject(PublishStage::BeforeHeadPublish));

  HeadRecord head;
  head.sequence = model.sequence;
  head.store_id = store_id_;
  head.revision = model.revision;
  head.epoch = epoch_;
  head.file_name = final_name;
  const std::string head_text = format_head(head);

  PLR_CHECK(internal::write_file(directory_ / std::string(kHeadTempFile), head_text,
                               plans.fsync_state_before_publish));
  // The commit point: replacing head is a single atomic directory operation.
  PLR_CHECK(internal::replace_file(directory_ / std::string(kHeadTempFile),
                                 directory_ / std::string(kHeadFile),
                                 plans.fsync_directory_after_rename));
  PLR_CHECK(internal::sync_directory(directory_, plans.fsync_directory_after_rename));

  PLR_CHECK(inject(PublishStage::AfterHeadPublish));
  PLR_CHECK(inject(PublishStage::BeforeRetire));

  revision_ = model.revision;
  sequence_ = model.sequence;
  state_bytes_ = static_cast<std::uint64_t>(encoded.size());
  state_digest_ = Sha256::hex(encoded);
  ++publications_;

  // Retire superseded publications. Best effort: a concurrent reader may hold an
  // old generation open, and the next writer session retries.
  auto names = internal::list_file_names(directory_);
  if (names.has_value()) {
    std::vector<std::uint64_t> sequences;
    for (const std::string& name : names.value()) {
      std::uint64_t sequence = 0;
      if (parse_state_file_name(name, sequence)) {
        sequences.push_back(sequence);
      }
    }
    std::sort(sequences.begin(), sequences.end(), std::greater<>());
    for (std::size_t index = static_cast<std::size_t>(limits_.max_publications_retained);
         index < sequences.size(); ++index) {
      (void)internal::remove_file(directory_ / publication_file_name(StateSequence(sequences[index])));
    }
  }
  return sequence_;
}

Result<RecoveryReport> Store::recover_and_republish() {
  if (closed_) {
    return Error(ErrorCode::SessionClosed, "this store handle has been closed");
  }
  if (!is_writer()) {
    return Error(ErrorCode::SessionReadOnly, "recovery requires a writer session");
  }
  if (recovery_.clean()) {
    PLR_CHECK(verify());
    RecoveryReport report = recovery_;
    report.notes.emplace_back("head and publication agree; nothing to recover");
    return report;
  }

  const std::uint64_t read_bound = static_cast<std::uint64_t>(kHardMaxStateBytes) + 4096ULL;
  PLR_TRY(loaded, load_published_state(directory_, read_bound));
  if (!loaded.report.recovered_older_publication) {
    RecoveryReport report = loaded.report;
    report.notes.emplace_back("head is usable; nothing to recover");
    recovery_ = report;
    return report;
  }

  HeadRecord head;
  head.sequence = loaded.model->sequence;
  head.store_id = loaded.model->store_id;
  head.revision = loaded.model->revision;
  head.epoch = loaded.model->epoch;
  head.file_name = publication_file_name(head.sequence);
  const std::string head_text = format_head(head);
  const auto plans = options_ != nullptr ? *options_ : StoreOptions{};

  PLR_CHECK(internal::write_file(directory_ / std::string(kHeadTempFile), head_text,
                                        plans.fsync_state_before_publish));
  PLR_CHECK(internal::replace_file(directory_ / std::string(kHeadTempFile),
                                            directory_ / std::string(kHeadFile),
                                            plans.fsync_directory_after_rename));

  store_id_ = loaded.model->store_id;
  revision_ = loaded.model->revision;
  sequence_ = loaded.model->sequence;
  epoch_ = loaded.model->epoch;
  limits_ = loaded.model->limits;
  PLR_TRY(bytes, read_published_state());
  state_bytes_ = static_cast<std::uint64_t>(bytes.size());
  state_digest_ = Sha256::hex(bytes);

  RecoveryReport report = loaded.report;
  report.republished_head = true;
  report.notes.emplace_back("head republished to point at the verified publication");
  recovery_ = report;
  return report;
}

Result<void> Store::close() {
  if (closed_) {
    return ok();
  }
  closed_ = true;
  if (lock_) {
    PLR_CHECK(internal::release_lock(lock_));
  }
  return ok();
}

}  // namespace dccp::physical_location_registry