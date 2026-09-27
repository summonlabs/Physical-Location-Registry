// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Internal bounded little-endian codec used by the canonical snapshot format.
// Every read is bounds-checked against the remaining payload before any byte is
// consumed, and every length is validated against a caller-supplied cap before
// it can drive an allocation.

#ifndef DCCP_PHYSICAL_LOCATION_REGISTRY_SRC_BYTE_CODEC_HPP
#define DCCP_PHYSICAL_LOCATION_REGISTRY_SRC_BYTE_CODEC_HPP

#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

#include "dccp/physical_location_registry/result.hpp"

namespace dccp::physical_location_registry::internal {

class ByteWriter {
 public:
  void u8(std::uint8_t value) { bytes_.push_back(static_cast<char>(value)); }

  void u16(std::uint16_t value) {
    u8(static_cast<std::uint8_t>(value & 0xFFU));
    u8(static_cast<std::uint8_t>((value >> 8U) & 0xFFU));
  }

  void u32(std::uint32_t value) {
    for (unsigned shift = 0; shift < 32U; shift += 8U) {
      u8(static_cast<std::uint8_t>((value >> shift) & 0xFFU));
    }
  }

  void u64(std::uint64_t value) {
    for (unsigned shift = 0; shift < 64U; shift += 8U) {
      u8(static_cast<std::uint8_t>((value >> shift) & 0xFFU));
    }
  }

  void i64(std::int64_t value) { u64(static_cast<std::uint64_t>(value)); }

  void bytes(std::string_view value) {
    u64(static_cast<std::uint64_t>(value.size()));
    bytes_.append(value);
  }

  void raw(std::string_view value) { bytes_.append(value); }

  const std::string& take() const noexcept { return bytes_; }
  std::string&& release() noexcept { return std::move(bytes_); }
  std::size_t size() const noexcept { return bytes_.size(); }

 private:
  std::string bytes_;
};

class ByteReader {
 public:
  ByteReader(const char* data, std::size_t size) noexcept
      : data_(data), size_(size), offset_(0) {}

  std::size_t remaining() const noexcept { return size_ - offset_; }
  std::size_t offset() const noexcept { return offset_; }
  bool exhausted() const noexcept { return offset_ == size_; }

  Result<std::uint8_t> u8() {
    PLR_TRY(raw, take(1));
    return static_cast<std::uint8_t>(raw[0]);
  }

  Result<std::uint16_t> u16() {
    PLR_TRY(raw, take(2));
    return static_cast<std::uint16_t>(static_cast<std::uint8_t>(raw[0]) |
                                      (static_cast<std::uint16_t>(static_cast<std::uint8_t>(raw[1])) << 8U));
  }

  Result<std::uint32_t> u32() {
    PLR_TRY(raw, take(4));
    std::uint32_t value = 0;
    for (unsigned index = 0; index < 4U; ++index) {
      value |= static_cast<std::uint32_t>(static_cast<std::uint8_t>(raw[index])) << (index * 8U);
    }
    return value;
  }

  Result<std::uint64_t> u64() {
    PLR_TRY(raw, take(8));
    std::uint64_t value = 0;
    for (unsigned index = 0; index < 8U; ++index) {
      value |= static_cast<std::uint64_t>(static_cast<std::uint8_t>(raw[index])) << (index * 8U);
    }
    return value;
  }

  Result<std::int64_t> i64() {
    PLR_TRY(value, u64());
    return static_cast<std::int64_t>(value);
  }

  /// Reads a length-prefixed string, rejecting a length above max_bytes before
  /// any allocation happens.
  Result<std::string> text(std::uint64_t max_bytes, const char* field_name) {
    PLR_TRY(length, u64());
    if (length > max_bytes) {
      return Error(ErrorCode::LimitExceeded,
                   std::string(field_name) + " length exceeds the permitted maximum")
          .with_subject(std::to_string(length));
    }
    if (length > static_cast<std::uint64_t>(remaining())) {
      return Error(ErrorCode::TruncatedInput,
                   std::string(field_name) + " extends past the end of the payload")
          .with_subject(std::to_string(length));
    }
    PLR_TRY(raw, take(static_cast<std::size_t>(length)));
    return std::string(raw);
  }

  Result<std::string_view> take(std::size_t count) {
    if (count > remaining()) {
      return Error(ErrorCode::TruncatedInput, "payload ends before the declared structure does")
          .with_subject("needed=" + std::to_string(count) + " remaining=" + std::to_string(remaining()));
    }
    const char* start = data_ + offset_;
    offset_ += count;
    return std::string_view(start, count);
  }

 private:
  const char* data_;
  std::size_t size_;
  std::size_t offset_;
};

}  // namespace dccp::physical_location_registry::internal

#endif  // DCCP_PHYSICAL_LOCATION_REGISTRY_SRC_BYTE_CODEC_HPP
