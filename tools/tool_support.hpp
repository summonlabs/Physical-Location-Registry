// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Shared command-line plumbing for the inspection tool and the process agent:
// argument parsing, stable exit codes and deterministic text/JSON output.

#ifndef PLR_TOOLS_TOOL_SUPPORT_HPP
#define PLR_TOOLS_TOOL_SUPPORT_HPP

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "dccp/physical_location_registry/physical_location_registry.hpp"

namespace plr_tool {

using namespace dccp::physical_location_registry;

/// Stable process exit codes. Documented in the README and asserted by tests.
enum class ExitCode : int {
  Ok = 0,
  Internal = 1,
  Usage = 2,
  StoreUnavailable = 3,
  IntegrityFailure = 4,
  Rejected = 5,
};

/// Parses "--name=value" and "--name value" forms plus positional arguments.
class Arguments {
 public:
  Arguments(int argc, char** argv);

  bool has(std::string_view name) const;
  std::optional<std::string> value(std::string_view name) const;
  std::string value_or(std::string_view name, std::string fallback) const;
  Result<std::uint64_t> unsigned_value(std::string_view name) const;
  Result<std::uint32_t> unsigned32_value(std::string_view name) const;

  const std::vector<std::string>& positional() const noexcept { return positional_; }
  std::string command() const;
  const std::vector<std::string>& raw() const noexcept { return raw_; }

  /// Names that were supplied but never consumed, for "unknown option" checks.
  std::vector<std::string> unconsumed() const;
  void mark_consumed(std::string_view name);

  /// Every option name that was supplied, deduplicated and sorted.
  std::vector<std::string> option_names() const;

 private:
  std::vector<std::string> raw_;
  std::vector<std::pair<std::string, std::string>> options_;
  std::vector<std::string> positional_;
  mutable std::vector<std::string> consumed_;
};

/// Maps an error to the exit code that describes it.
ExitCode exit_code_for(const Error& error);

/// Writes "error-code=NAME category=CATEGORY message=..." to standard error.
void print_error(const Error& error);

/// Escapes a string for a JSON string literal.
std::string json_escape(std::string_view text);

/// Minimal deterministic JSON writer: fixed key order, no whitespace surprises.
class Json {
 public:
  explicit Json(std::string_view kind);
  Json& field(std::string_view name, std::string_view value);
  Json& field(std::string_view name, const std::string& value);
  Json& field(std::string_view name, std::uint64_t value);
  Json& field(std::string_view name, std::int64_t value);
  Json& field(std::string_view name, bool value);
  Json& raw_field(std::string_view name, std::string_view raw_json);
  std::string str() const;

 private:
  std::string text_;
};

/// Reads a full file, bounded, for snapshot import.
Result<std::string> read_file(const std::string& path, std::uint64_t max_bytes);

/// Writes bytes to a file, atomically through a sibling temporary file.
Result<void> write_file_atomically(const std::string& path, std::string_view bytes);

}  // namespace plr_tool

#endif  // PLR_TOOLS_TOOL_SUPPORT_HPP
