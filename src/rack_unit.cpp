// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "dccp/physical_location_registry/rack_unit.hpp"

#include <string>

namespace dccp::physical_location_registry {
namespace {

/// Parses a run of ASCII digits into a bounded value without overflowing.
Result<std::uint64_t> parse_decimal(std::string_view raw) {
  if (raw.empty() || raw.size() > 10) {
    return Error(ErrorCode::InvalidArgument, "expected a decimal number of at most 10 digits")
        .with_subject(std::string(raw.substr(0, 64)));
  }
  std::uint64_t value = 0;
  for (const char raw_character : raw) {
    if (raw_character < '0' || raw_character > '9') {
      return Error(ErrorCode::InvalidArgument, "expected decimal digits only")
          .with_subject(std::string(raw.substr(0, 64)));
    }
    value = value * 10U + static_cast<std::uint64_t>(raw_character - '0');
  }
  return value;
}

/// Removes one optional leading or trailing 'U' (either case), then parses digits.
Result<std::uint64_t> parse_unit_text(std::string_view raw) {
  if (raw.size() >= 2) {
    const char first = raw.front();
    const char last = raw.back();
    if (first == 'U' || first == 'u') {
      raw.remove_prefix(1);
    } else if (last == 'U' || last == 'u') {
      raw.remove_suffix(1);
    }
  }
  return parse_decimal(raw);
}

std::string decimal(std::uint64_t value) { return std::to_string(value); }

}  // namespace

Result<RackUnitCoordinate> RackUnitCoordinate::parse(std::uint64_t unit) {
  if (unit == 0 || unit > kMaxRackUnitCoordinate) {
    return Error(ErrorCode::InvalidArgument,
                 "rack-unit coordinate must be between 1 and " +
                     decimal(kMaxRackUnitCoordinate))
        .with_subject(decimal(unit));
  }
  return RackUnitCoordinate(static_cast<std::uint32_t>(unit));
}

Result<RackUnitCoordinate> RackUnitCoordinate::parse_text(std::string_view raw) {
  PLR_TRY(value, parse_unit_text(raw));
  return parse(value);
}

std::string RackUnitCoordinate::to_string() const { return "U" + decimal(value_); }

Result<RackEnvelope> RackEnvelope::make(RackUnitCoordinate first, std::uint32_t height) {
  if (height == 0) {
    return Error(ErrorCode::RackEnvelopeInvalid, "rack envelope height must be at least 1 unit");
  }
  const std::uint64_t last = static_cast<std::uint64_t>(first.value()) + height - 1U;
  if (last > kMaxRackUnitCoordinate) {
    return Error(ErrorCode::RackEnvelopeInvalid,
                 "rack envelope extends past U" + decimal(kMaxRackUnitCoordinate))
        .with_subject("first=" + first.to_string() + " height=" + decimal(height));
  }
  return RackEnvelope(first, height);
}

Result<RackEnvelope> RackEnvelope::with_height(std::uint32_t height) {
  PLR_TRY(first, RackUnitCoordinate::parse(1));
  return make(first, height);
}

Result<RackEnvelope> RackEnvelope::parse_text(std::string_view raw) {
  const std::size_t separator = raw.find('-');
  if (separator == std::string_view::npos) {
    PLR_TRY(height, parse_unit_text(raw));
    if (height == 0 || height > kMaxRackUnitCoordinate) {
      return Error(ErrorCode::RackEnvelopeInvalid,
                   "rack envelope height must be between 1 and " + decimal(kMaxRackUnitCoordinate))
          .with_subject(std::string(raw.substr(0, 64)));
    }
    return with_height(static_cast<std::uint32_t>(height));
  }

  const std::string_view first_text = raw.substr(0, separator);
  const std::string_view last_text = raw.substr(separator + 1);
  PLR_TRY(first_value, parse_unit_text(first_text));
  PLR_TRY(last_value, parse_unit_text(last_text));
  PLR_TRY(first, RackUnitCoordinate::parse(first_value));
  PLR_TRY(last, RackUnitCoordinate::parse(last_value));
  if (last < first) {
    return Error(ErrorCode::RackEnvelopeInvalid, "rack envelope range ends before it starts")
        .with_subject(std::string(raw.substr(0, 64)));
  }
  return make(first, static_cast<std::uint32_t>(last.value() - first.value()) + 1U);
}

std::string RackEnvelope::to_string() const {
  return decimal(first_.value()) + "-" + decimal(last().value()) + "U";
}

}  // namespace dccp::physical_location_registry
