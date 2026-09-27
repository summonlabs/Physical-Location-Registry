// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#ifndef DCCP_PHYSICAL_LOCATION_REGISTRY_DIGEST_HPP
#define DCCP_PHYSICAL_LOCATION_REGISTRY_DIGEST_HPP

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "dccp/physical_location_registry/export.hpp"

namespace dccp::physical_location_registry {

/// Length of a SHA-256 digest in bytes.
inline constexpr std::size_t kSha256Bytes = 32;

/// Streaming SHA-256 (FIPS 180-4), implemented here so that integrity checking
/// has no third-party dependency and behaves identically on every platform.
class PLR_API Sha256 {
 public:
  Sha256() noexcept;

  void update(const void* data, std::size_t size) noexcept;
  void update(std::string_view data) noexcept { update(data.data(), data.size()); }

  /// Finalizes and returns the lowercase hexadecimal digest.
  std::string finish_hex() noexcept;

  /// Finalizes and returns the raw digest.
  void finish(std::uint8_t out[kSha256Bytes]) noexcept;

  /// One-shot helper: lowercase hexadecimal digest of a buffer.
  static std::string hex(std::string_view data) noexcept;

 private:
  void transform(const std::uint8_t block[64]) noexcept;

  std::uint32_t state_[8];
  std::uint64_t bit_count_;
  std::uint8_t buffer_[64];
  std::size_t buffer_size_;
};

/// True when a string is exactly 64 lowercase hexadecimal characters.
PLR_API bool is_lower_hex_sha256(std::string_view text) noexcept;

/// True when two digests are equal in constant time with respect to content.
PLR_API bool digest_equal(std::string_view lhs, std::string_view rhs) noexcept;

}  // namespace dccp::physical_location_registry

#endif  // DCCP_PHYSICAL_LOCATION_REGISTRY_DIGEST_HPP
