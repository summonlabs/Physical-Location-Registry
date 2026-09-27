// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "tool_support.hpp"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <utility>

namespace plr_tool {
namespace {

constexpr std::uint64_t kMaxReadBytes = kHardMaxStateBytes + (1ULL << 20U);

std::string decimal(std::uint64_t value) { return std::to_string(value); }

}  // namespace

Arguments::Arguments(int argc, char** argv) {
  for (int index = 0; index < argc; ++index) {
    raw_.emplace_back(argv[index]);
  }
  for (int index = 1; index < argc; ++index) {
    const std::string argument = argv[index];
    if (argument.rfind("--", 0) == 0) {
      const std::size_t equals = argument.find('=');
      if (equals != std::string::npos) {
        options_.emplace_back(argument.substr(0, equals), argument.substr(equals + 1));
      } else if (index + 1 < argc && argv[index + 1][0] != '-') {
        options_.emplace_back(argument, std::string(argv[index + 1]));
        ++index;
      } else {
        options_.emplace_back(argument, std::string());
      }
      continue;
    }
    positional_.push_back(argument);
  }
}

bool Arguments::has(std::string_view name) const {
  for (const auto& option : options_) {
    if (option.first == name) {
      return true;
    }
  }
  return false;
}

std::optional<std::string> Arguments::value(std::string_view name) const {
  for (const auto& option : options_) {
    if (option.first == name) {
      consumed_.emplace_back(option.first);
      return option.second;
    }
  }
  return std::nullopt;
}

std::string Arguments::value_or(std::string_view name, std::string fallback) const {
  const std::optional<std::string> found = value(name);
  if (found.has_value() && !found->empty()) {
    return found.value();
  }
  return fallback;
}

Result<std::uint64_t> Arguments::unsigned_value(std::string_view name) const {
  const std::optional<std::string> text = value(name);
  if (!text.has_value() || text->empty()) {
    return Error(ErrorCode::InvalidArgument, "option requires a value").with_subject(std::string(name));
  }
  std::uint64_t result = 0;
  for (const char character : text.value()) {
    if (character < '0' || character > '9') {
      return Error(ErrorCode::InvalidArgument, "option requires a decimal value")
          .with_subject(std::string(name));
    }
    const std::uint64_t digit = static_cast<std::uint64_t>(character - '0');
    if (result > (UINT64_MAX - digit) / 10U) {
      return Error(ErrorCode::InvalidArgument, "option value overflows").with_subject(std::string(name));
    }
    result = result * 10U + digit;
  }
  return result;
}

Result<std::uint32_t> Arguments::unsigned32_value(std::string_view name) const {
  PLR_TRY(value64, unsigned_value(name));
  if (value64 > UINT32_MAX) {
    return Error(ErrorCode::InvalidArgument, "option value is too large")
        .with_subject(std::string(name));
  }
  return static_cast<std::uint32_t>(value64);
}

std::string Arguments::command() const {
  return positional_.empty() ? std::string() : positional_.front();
}

std::vector<std::string> Arguments::unconsumed() const {
  std::vector<std::string> result;
  for (const auto& option : options_) {
    if (std::find(consumed_.begin(), consumed_.end(), option.first) == consumed_.end()) {
      result.push_back(option.first);
    }
  }
  std::sort(result.begin(), result.end());
  result.erase(std::unique(result.begin(), result.end()), result.end());
  return result;
}

void Arguments::mark_consumed(std::string_view name) { consumed_.emplace_back(name); }

std::vector<std::string> Arguments::option_names() const {
  std::vector<std::string> names;
  names.reserve(options_.size());
  for (const auto& option : options_) {
    names.push_back(option.first);
  }
  std::sort(names.begin(), names.end());
  names.erase(std::unique(names.begin(), names.end()), names.end());
  return names;
}

ExitCode exit_code_for(const Error& error) {
  // Specific codes first: an integrity failure is an integrity failure whatever
  // category the untrusted-input decoder that noticed it belongs to.
  switch (error.code()) {
    case ErrorCode::Ok:
      return ExitCode::Ok;
    case ErrorCode::DigestMismatch:
    case ErrorCode::StoreCorrupt:
    case ErrorCode::HeadCorrupt:
    case ErrorCode::HeadMissing:
    case ErrorCode::TruncatedInput:
    case ErrorCode::CountMismatch:
    case ErrorCode::MalformedRecord:
    case ErrorCode::UnsupportedSchemaVersion:
    case ErrorCode::UnsupportedFormatFlag:
    case ErrorCode::IntegrityFailure:
      return ExitCode::IntegrityFailure;
    case ErrorCode::StoreNotFound:
    case ErrorCode::StoreExists:
    case ErrorCode::StoreNotEmpty:
    case ErrorCode::StoreLocked:
    case ErrorCode::RecoveryUnavailable:
    case ErrorCode::RecoveryRequired:
    case ErrorCode::IoError:
    case ErrorCode::SessionReadOnly:
    case ErrorCode::SessionClosed:
      return ExitCode::StoreUnavailable;
    default:
      break;
  }

  switch (error.category()) {
    case ErrorCategory::Ok:
      return ExitCode::Ok;
    case ErrorCategory::Argument:
      return ExitCode::Usage;
    case ErrorCategory::Limit:
      return ExitCode::Rejected;
    case ErrorCategory::Persistence:
      return ExitCode::IntegrityFailure;
    case ErrorCategory::Lifecycle:
      return ExitCode::StoreUnavailable;
    case ErrorCategory::Structure:
    case ErrorCategory::Authority:
    case ErrorCategory::Cancelled:
      return ExitCode::Rejected;
    case ErrorCategory::Internal:
      return ExitCode::Internal;
  }
  return ExitCode::Internal;
}

void print_error(const Error& error) {
  std::cerr << "error-code=" << error_code_name(error.code())
            << " category=" << error_category_name(error.category());
  if (!error.subject().empty()) {
    std::cerr << " subject=" << error.subject();
  }
  std::cerr << " message=" << error.message() << std::endl;
}

std::string json_escape(std::string_view text) {
  std::string escaped;
  escaped.reserve(text.size() + 2);
  for (const char character : text) {
    switch (character) {
      case '"':
        escaped.append("\\\"");
        break;
      case '\\':
        escaped.append("\\\\");
        break;
      case '\n':
        escaped.append("\\n");
        break;
      case '\r':
        escaped.append("\\r");
        break;
      case '\t':
        escaped.append("\\t");
        break;
      default:
        if (static_cast<unsigned char>(character) < 0x20U) {
          char buffer[8];
          std::snprintf(buffer, sizeof(buffer), "\\u%04x", character);
          escaped.append(buffer);
        } else {
          escaped.push_back(character);
        }
        break;
    }
  }
  return escaped;
}

Json::Json(std::string_view kind) {
  text_.push_back('{');
  text_.append("\"kind\":\"");
  text_.append(json_escape(kind));
  text_.push_back('"');
}

Json& Json::field(std::string_view name, std::string_view value) {
  text_.append(",\"");
  text_.append(json_escape(name));
  text_.append("\":\"");
  text_.append(json_escape(value));
  text_.push_back('"');
  return *this;
}

Json& Json::field(std::string_view name, const std::string& value) {
  return field(name, std::string_view(value));
}

Json& Json::field(std::string_view name, std::uint64_t value) {
  text_.append(",\"");
  text_.append(json_escape(name));
  text_.append("\":");
  text_.append(decimal(value));
  return *this;
}

Json& Json::field(std::string_view name, std::int64_t value) {
  text_.append(",\"");
  text_.append(json_escape(name));
  text_.append("\":");
  text_.append(std::to_string(value));
  return *this;
}

Json& Json::field(std::string_view name, bool value) {
  text_.append(",\"");
  text_.append(json_escape(name));
  text_.append("\":");
  text_.append(value ? "true" : "false");
  return *this;
}

Json& Json::raw_field(std::string_view name, std::string_view raw_json) {
  text_.append(",\"");
  text_.append(json_escape(name));
  text_.append("\":");
  text_.append(raw_json);
  return *this;
}

std::string Json::str() const {
  std::string result = text_;
  result.push_back('}');
  return result;
}

Result<std::string> read_file(const std::string& path, std::uint64_t max_bytes) {
  std::error_code error;
  const auto size = std::filesystem::file_size(path, error);
  if (error) {
    return Error(ErrorCode::StoreNotFound, "cannot stat file: " + error.message()).with_subject(path);
  }
  if (static_cast<std::uint64_t>(size) > max_bytes) {
    return Error(ErrorCode::LimitExceeded, "file is larger than the permitted maximum")
        .with_subject(path);
  }
  std::ifstream stream(path, std::ios::binary);
  if (!stream) {
    return Error(ErrorCode::IoError, "cannot open file").with_subject(path);
  }
  std::string content;
  content.resize(static_cast<std::size_t>(size));
  stream.read(content.data(), static_cast<std::streamsize>(size));
  if (!stream && !stream.eof()) {
    return Error(ErrorCode::IoError, "cannot read file").with_subject(path);
  }
  return content;
}

Result<void> write_file_atomically(const std::string& path, std::string_view bytes) {
  const std::filesystem::path target(path);
  const std::filesystem::path temporary = target.string() + ".tmp";
  {
    std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
    if (!stream) {
      return Error(ErrorCode::IoError, "cannot open temporary file").with_subject(temporary.string());
    }
    stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    stream.flush();
    if (!stream) {
      return Error(ErrorCode::IoError, "cannot write temporary file").with_subject(temporary.string());
    }
  }
  std::error_code error;
  std::filesystem::rename(temporary, target, error);
  if (error) {
    std::filesystem::remove(temporary, error);
    return Error(ErrorCode::IoError, "cannot publish file: " + error.message()).with_subject(path);
  }
  return ok();
}

}  // namespace plr_tool
