// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#ifndef DCCP_PHYSICAL_LOCATION_REGISTRY_EXPLANATION_HPP
#define DCCP_PHYSICAL_LOCATION_REGISTRY_EXPLANATION_HPP

#include <string>
#include <string_view>
#include <vector>

#include "dccp/physical_location_registry/export.hpp"
#include "dccp/physical_location_registry/result.hpp"

namespace dccp::physical_location_registry {

/// A machine-readable outcome plus the human explanation of it.
///
/// Every rejection the registry produces carries a stable ErrorCode; the
/// explanation adds what the code means, which input caused it, and what a
/// caller can do next. Explanations are produced without mutating anything, so a
/// caller can ask "would this be accepted, and why not" before committing.
struct PLR_API Explanation {
  ErrorCode code = ErrorCode::Ok;
  ErrorCategory category = ErrorCategory::Ok;

  /// One-line statement of the outcome.
  std::string summary;

  /// Ordered detail lines; each line is a complete sentence.
  std::vector<std::string> details;

  /// Ordered, deterministic suggestions. Empty for accepted outcomes.
  std::vector<std::string> hints;

  bool ok() const noexcept { return code == ErrorCode::Ok; }

  /// Multi-line rendering: summary, then "detail: ..." lines, then "hint: ...".
  std::string to_string() const;
};

/// Builds an explanation for an Error produced by this library.
PLR_API Explanation explain_error(const Error& error);

/// Builds an explanation directly from a code (no subjects or details).
PLR_API Explanation explain_error_code(ErrorCode code);

}  // namespace dccp::physical_location_registry

#endif  // DCCP_PHYSICAL_LOCATION_REGISTRY_EXPLANATION_HPP
