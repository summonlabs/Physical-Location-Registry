// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "dccp/physical_location_registry/provenance.hpp"

#include <array>
#include <cstdio>
#include <string>

#include "dccp/physical_location_registry/text.hpp"

namespace dccp::physical_location_registry {
namespace {

constexpr std::int64_t kSecondsPerDay = 86400;

/// Days between 1970-01-01 and the given civil date (Howard Hinnant's
/// days_from_civil, valid for the whole proleptic Gregorian calendar).
constexpr std::int64_t days_from_civil(std::int64_t year, unsigned month, unsigned day) noexcept {
  year -= month <= 2 ? 1 : 0;
  const std::int64_t era = (year >= 0 ? year : year - 399) / 400;
  const unsigned year_of_era = static_cast<unsigned>(year - era * 400);
  const unsigned month_prime = month > 2 ? month - 3U : month + 9U;
  const unsigned day_of_year = (153U * month_prime + 2U) / 5U + day - 1U;
  const unsigned day_of_era =
      year_of_era * 365U + year_of_era / 4U - year_of_era / 100U + day_of_year;
  return era * 146097 + static_cast<std::int64_t>(day_of_era) - 719468;
}

void civil_from_days(std::int64_t days, std::int64_t& year, unsigned& month, unsigned& day) noexcept {
  days += 719468;
  const std::int64_t era = (days >= 0 ? days : days - 146096) / 146097;
  const unsigned day_of_era = static_cast<unsigned>(days - era * 146097);
  const unsigned year_of_era =
      (day_of_era - day_of_era / 1460U + day_of_era / 36524U - day_of_era / 146096U) / 365U;
  year = static_cast<std::int64_t>(year_of_era) + era * 400;
  const unsigned day_of_year =
      day_of_era - (365U * year_of_era + year_of_era / 4U - year_of_era / 100U);
  const unsigned month_prime = (5U * day_of_year + 2U) / 153U;
  day = day_of_year - (153U * month_prime + 2U) / 5U + 1U;
  month = month_prime < 10U ? month_prime + 3U : month_prime - 9U;
  year += month <= 2 ? 1 : 0;
}

bool is_leap_year(std::int64_t year) noexcept {
  return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

unsigned days_in_month(std::int64_t year, unsigned month) noexcept {
  constexpr std::array<unsigned, 12> kMonthLengths{31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (month < 1 || month > 12) {
    return 0;
  }
  if (month == 2 && is_leap_year(year)) {
    return 29;
  }
  return kMonthLengths[month - 1];
}

bool all_digits(std::string_view raw) noexcept {
  if (raw.empty()) {
    return false;
  }
  for (const char character : raw) {
    if (character < '0' || character > '9') {
      return false;
    }
  }
  return true;
}

Result<std::uint64_t> parse_unsigned(std::string_view raw, std::size_t max_digits) {
  if (raw.empty() || raw.size() > max_digits) {
    return Error(ErrorCode::InvalidArgument, "expected a bounded decimal number")
        .with_subject(std::string(raw.substr(0, 32)));
  }
  std::uint64_t value = 0;
  for (const char character : raw) {
    if (character < '0' || character > '9') {
      return Error(ErrorCode::InvalidArgument, "expected decimal digits only")
          .with_subject(std::string(raw.substr(0, 32)));
    }
    value = value * 10U + static_cast<std::uint64_t>(character - '0');
  }
  return value;
}

}  // namespace

Result<Timestamp> Timestamp::make(std::int64_t unix_seconds, std::uint32_t nanos) {
  if (unix_seconds < 0) {
    return Error(ErrorCode::InvalidArgument,
                 "timestamps before 1970-01-01T00:00:00Z are not accepted");
  }
  if (unix_seconds > kMaxUnixSeconds) {
    return Error(ErrorCode::InvalidArgument, "timestamp is beyond 9999-12-31T23:59:59Z")
        .with_subject(std::to_string(unix_seconds));
  }
  if (nanos >= 1000000000U) {
    return Error(ErrorCode::InvalidArgument, "nanoseconds must be below 1000000000")
        .with_subject(std::to_string(nanos));
  }
  Timestamp timestamp;
  timestamp.unix_seconds_ = unix_seconds;
  timestamp.nanos_ = nanos;
  return timestamp;
}

Result<Timestamp> Timestamp::from_unix_seconds(std::int64_t unix_seconds) {
  return make(unix_seconds, 0);
}

Result<Timestamp> Timestamp::parse_text(std::string_view raw) {
  if (all_digits(raw)) {
    PLR_TRY(seconds, parse_unsigned(raw, 12));
    if (seconds > static_cast<std::uint64_t>(kMaxUnixSeconds)) {
      return Error(ErrorCode::InvalidArgument, "timestamp is beyond 9999-12-31T23:59:59Z")
          .with_subject(std::string(raw));
    }
    return make(static_cast<std::int64_t>(seconds), 0);
  }

  // Canonical form: YYYY-MM-DDTHH:MM:SS[.fffffffff]Z
  if (raw.size() < 20 || raw.size() > 30) {
    return Error(ErrorCode::InvalidArgument,
                 "timestamp must be Unix seconds or YYYY-MM-DDTHH:MM:SS[.fffffffff]Z")
        .with_subject(std::string(raw.substr(0, 64)));
  }
  if (raw[4] != '-' || raw[7] != '-' || raw[10] != 'T' || raw[13] != ':' || raw[16] != ':' ||
      raw.back() != 'Z') {
    return Error(ErrorCode::InvalidArgument,
                 "timestamp must be Unix seconds or YYYY-MM-DDTHH:MM:SS[.fffffffff]Z")
        .with_subject(std::string(raw.substr(0, 64)));
  }
  PLR_TRY(year, parse_unsigned(raw.substr(0, 4), 4));
  PLR_TRY(month, parse_unsigned(raw.substr(5, 2), 2));
  PLR_TRY(day, parse_unsigned(raw.substr(8, 2), 2));
  PLR_TRY(hour, parse_unsigned(raw.substr(11, 2), 2));
  PLR_TRY(minute, parse_unsigned(raw.substr(14, 2), 2));
  PLR_TRY(second, parse_unsigned(raw.substr(17, 2), 2));

  std::uint32_t nanos = 0;
  if (raw.size() > 20) {
    if (raw[19] != '.' || raw.size() < 22) {
      return Error(ErrorCode::InvalidArgument, "fractional seconds must be written as .fffffffff")
          .with_subject(std::string(raw.substr(0, 64)));
    }
    const std::string_view fraction = raw.substr(20, raw.size() - 21);
    if (fraction.empty() || fraction.size() > 9) {
      return Error(ErrorCode::InvalidArgument, "fractional seconds must have 1 to 9 digits")
          .with_subject(std::string(raw.substr(0, 64)));
    }
    PLR_TRY(fraction_value, parse_unsigned(fraction, 9));
    std::uint32_t scale = 1;
    for (std::size_t index = fraction.size(); index < 9; ++index) {
      scale *= 10U;
    }
    nanos = static_cast<std::uint32_t>(fraction_value) * scale;
  }

  const std::int64_t year_value = static_cast<std::int64_t>(year);
  if (year_value < 1970 || year_value > 9999) {
    return Error(ErrorCode::InvalidArgument, "year must be between 1970 and 9999")
        .with_subject(std::string(raw.substr(0, 64)));
  }
  if (month < 1 || month > 12) {
    return Error(ErrorCode::InvalidArgument, "month must be between 1 and 12")
        .with_subject(std::string(raw.substr(0, 64)));
  }
  if (day < 1 || day > days_in_month(year_value, static_cast<unsigned>(month))) {
    return Error(ErrorCode::InvalidArgument, "day is out of range for the month")
        .with_subject(std::string(raw.substr(0, 64)));
  }
  if (hour > 23 || minute > 59 || second > 59) {
    return Error(ErrorCode::InvalidArgument, "time of day is out of range")
        .with_subject(std::string(raw.substr(0, 64)));
  }

  const std::int64_t days = days_from_civil(year_value, static_cast<unsigned>(month),
                                            static_cast<unsigned>(day));
  const std::int64_t seconds =
      days * kSecondsPerDay + static_cast<std::int64_t>(hour) * 3600 +
      static_cast<std::int64_t>(minute) * 60 + static_cast<std::int64_t>(second);
  return make(seconds, nanos);
}

std::string Timestamp::to_string() const { return format_timestamp(unix_seconds_, nanos_); }

std::string format_timestamp(std::int64_t unix_seconds, std::uint32_t nanos) {
  if (unix_seconds < 0) {
    return "invalid-timestamp";
  }
  const std::int64_t days = unix_seconds / kSecondsPerDay;
  const std::int64_t second_of_day = unix_seconds % kSecondsPerDay;
  std::int64_t year = 1970;
  unsigned month = 1;
  unsigned day = 1;
  civil_from_days(days, year, month, day);

  const unsigned hour = static_cast<unsigned>(second_of_day / 3600);
  const unsigned minute = static_cast<unsigned>((second_of_day % 3600) / 60);
  const unsigned second = static_cast<unsigned>(second_of_day % 60);

  char buffer[48];
  if (nanos == 0) {
    const int written = std::snprintf(buffer, sizeof(buffer), "%04lld-%02u-%02uT%02u:%02u:%02uZ",
                                      static_cast<long long>(year), month, day, hour, minute, second);
    if (written <= 0) {
      return "invalid-timestamp";
    }
    return std::string(buffer, static_cast<std::size_t>(written));
  }
  const int written = std::snprintf(buffer, sizeof(buffer), "%04lld-%02u-%02uT%02u:%02u:%02u.%09uZ",
                                    static_cast<long long>(year), month, day, hour, minute, second,
                                    nanos);
  if (written <= 0) {
    return "invalid-timestamp";
  }
  return std::string(buffer, static_cast<std::size_t>(written));
}

std::string_view move_kind_name(MoveKind kind) noexcept {
  switch (kind) {
    case MoveKind::Reparent:
      return "reparent";
    case MoveKind::Readdress:
      return "readdress";
  }
  return "unrecognized";
}

Result<MoveKind> parse_move_kind(std::string_view raw) {
  if (ascii_case_insensitive_equal(raw, "reparent")) {
    return MoveKind::Reparent;
  }
  if (ascii_case_insensitive_equal(raw, "readdress")) {
    return MoveKind::Readdress;
  }
  return Error(ErrorCode::UnknownEnumToken, "unknown move kind; expected reparent or readdress")
      .with_subject(std::string(raw.substr(0, 64)));
}

}  // namespace dccp::physical_location_registry
