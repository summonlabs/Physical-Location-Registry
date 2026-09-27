// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "dccp/physical_location_registry/text.hpp"

#include <cstdint>

namespace dccp::physical_location_registry {
namespace {

struct DecodedCodePoint {
  bool valid = false;
  std::size_t length = 0;
  std::uint32_t code_point = 0;
};

bool is_continuation(std::uint8_t byte) noexcept { return (byte & 0xC0U) == 0x80U; }

/// Strict UTF-8 decode of one code point, rejecting overlong forms, surrogates
/// and values above U+10FFFF.
DecodedCodePoint decode_one(std::string_view raw, std::size_t index) noexcept {
  const std::uint8_t first = static_cast<std::uint8_t>(raw[index]);
  const std::size_t remaining = raw.size() - index;

  if (first < 0x80U) {
    return DecodedCodePoint{true, 1, first};
  }
  if (first >= 0xC2U && first <= 0xDFU) {
    if (remaining < 2) {
      return {};
    }
    const std::uint8_t second = static_cast<std::uint8_t>(raw[index + 1]);
    if (!is_continuation(second)) {
      return {};
    }
    return DecodedCodePoint{true, 2, (static_cast<std::uint32_t>(first & 0x1FU) << 6U) |
                                        static_cast<std::uint32_t>(second & 0x3FU)};
  }
  if (first >= 0xE0U && first <= 0xEFU) {
    if (remaining < 3) {
      return {};
    }
    const std::uint8_t second = static_cast<std::uint8_t>(raw[index + 1]);
    const std::uint8_t third = static_cast<std::uint8_t>(raw[index + 2]);
    if (!is_continuation(third)) {
      return {};
    }
    std::uint32_t lower = 0x80U;
    std::uint32_t upper = 0xBFU;
    if (first == 0xE0U) {
      lower = 0xA0U;  // no overlong three-byte forms
    } else if (first == 0xEDU) {
      upper = 0x9FU;  // no UTF-16 surrogates
    }
    if (second < lower || second > upper) {
      return {};
    }
    return DecodedCodePoint{true, 3, (static_cast<std::uint32_t>(first & 0x0FU) << 12U) |
                                        (static_cast<std::uint32_t>(second & 0x3FU) << 6U) |
                                        static_cast<std::uint32_t>(third & 0x3FU)};
  }
  if (first >= 0xF0U && first <= 0xF4U) {
    if (remaining < 4) {
      return {};
    }
    const std::uint8_t second = static_cast<std::uint8_t>(raw[index + 1]);
    const std::uint8_t third = static_cast<std::uint8_t>(raw[index + 2]);
    const std::uint8_t fourth = static_cast<std::uint8_t>(raw[index + 3]);
    if (!is_continuation(third) || !is_continuation(fourth)) {
      return {};
    }
    std::uint32_t lower = 0x80U;
    std::uint32_t upper = 0xBFU;
    if (first == 0xF0U) {
      lower = 0x90U;  // no overlong four-byte forms
    } else if (first == 0xF4U) {
      upper = 0x8FU;  // nothing above U+10FFFF
    }
    if (second < lower || second > upper) {
      return {};
    }
    return DecodedCodePoint{true, 4, (static_cast<std::uint32_t>(first & 0x07U) << 18U) |
                                        (static_cast<std::uint32_t>(second & 0x3FU) << 12U) |
                                        (static_cast<std::uint32_t>(third & 0x3FU) << 6U) |
                                        static_cast<std::uint32_t>(fourth & 0x3FU)};
  }
  return {};
}

bool is_disallowed_in_text(std::uint32_t code_point) noexcept {
  if (code_point < 0x20U) {
    return true;  // C0 controls, including NUL, tab and newline
  }
  if (code_point == 0x7FU) {
    return true;  // DEL
  }
  if (code_point >= 0x80U && code_point <= 0x9FU) {
    return true;  // C1 controls
  }
  if (code_point >= 0xFDD0U && code_point <= 0xFDEFU) {
    return true;  // noncharacters
  }
  if ((code_point & 0xFFFEU) == 0xFFFEU) {
    return true;  // U+xFFFE and U+xFFFF noncharacters
  }
  return false;
}

bool is_ascii_alphanumeric(std::uint8_t byte) noexcept {
  return (byte >= '0' && byte <= '9') || (byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z');
}

/// Shared implementation of the display-text rules.
bool is_valid_display_text(std::string_view raw, std::size_t max_bytes) noexcept {
  if (raw.size() > max_bytes) {
    return false;
  }
  std::size_t index = 0;
  while (index < raw.size()) {
    const DecodedCodePoint decoded = decode_one(raw, index);
    if (!decoded.valid) {
      return false;
    }
    if (is_disallowed_in_text(decoded.code_point)) {
      return false;
    }
    index += decoded.length;
  }
  return true;
}

}  // namespace

bool is_valid_utf8(std::string_view raw) noexcept {
  std::size_t index = 0;
  while (index < raw.size()) {
    const DecodedCodePoint decoded = decode_one(raw, index);
    if (!decoded.valid) {
      return false;
    }
    index += decoded.length;
  }
  return true;
}

bool is_valid_address_component(std::string_view raw) noexcept {
  if (raw.empty() || raw.size() > kMaxAddressComponentBytes) {
    return false;
  }
  const auto first = static_cast<std::uint8_t>(raw.front());
  const auto last = static_cast<std::uint8_t>(raw.back());
  if (!is_ascii_alphanumeric(first) || !is_ascii_alphanumeric(last)) {
    return false;
  }
  for (const char raw_byte : raw) {
    const auto byte = static_cast<std::uint8_t>(raw_byte);
    if (is_ascii_alphanumeric(byte) || byte == '.' || byte == '_' || byte == '-') {
      continue;
    }
    return false;
  }
  return true;
}

std::string_view address_component_syntax_help() noexcept {
  return "an address component is 1..64 bytes, begins and ends with an ASCII letter or digit, and "
         "contains only ASCII letters, digits, '.', '_' and '-'";
}

bool is_valid_label(std::string_view raw) noexcept {
  return is_valid_display_text(raw, kMaxLabelBytes);
}

std::string_view label_syntax_help() noexcept {
  return "a label is at most 256 bytes of valid UTF-8 without control characters or noncharacters";
}

bool is_valid_reason(std::string_view raw) noexcept {
  return is_valid_display_text(raw, kMaxReasonBytes);
}

bool is_valid_source(std::string_view raw) noexcept {
  return is_valid_display_text(raw, kMaxSourceBytes);
}

std::string ascii_fold(std::string_view raw) {
  std::string folded;
  folded.reserve(raw.size());
  for (const char raw_byte : raw) {
    const auto byte = static_cast<std::uint8_t>(raw_byte);
    if (byte >= 'A' && byte <= 'Z') {
      folded.push_back(static_cast<char>(byte + ('a' - 'A')));
    } else {
      folded.push_back(raw_byte);
    }
  }
  return folded;
}

bool ascii_case_insensitive_equal(std::string_view lhs, std::string_view rhs) noexcept {
  if (lhs.size() != rhs.size()) {
    return false;
  }
  for (std::size_t index = 0; index < lhs.size(); ++index) {
    const auto left = static_cast<std::uint8_t>(lhs[index]);
    const auto right = static_cast<std::uint8_t>(rhs[index]);
    const auto fold = [](std::uint8_t byte) -> std::uint8_t {
      return (byte >= 'A' && byte <= 'Z') ? static_cast<std::uint8_t>(byte + ('a' - 'A')) : byte;
    };
    if (fold(left) != fold(right)) {
      return false;
    }
  }
  return true;
}

int byte_compare(std::string_view lhs, std::string_view rhs) noexcept {
  const int cmp = lhs.compare(rhs);
  return cmp < 0 ? -1 : (cmp > 0 ? 1 : 0);
}

}  // namespace dccp::physical_location_registry
