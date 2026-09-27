// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#ifndef DCCP_PHYSICAL_LOCATION_REGISTRY_TEXT_HPP
#define DCCP_PHYSICAL_LOCATION_REGISTRY_TEXT_HPP

#include <cstddef>
#include <string>
#include <string_view>

#include "dccp/physical_location_registry/export.hpp"
#include "dccp/physical_location_registry/result.hpp"

namespace dccp::physical_location_registry {

/// Hard caps enforced by the library regardless of the configured Limits.
///
/// A Limits value may lower these, never raise them: untrusted durable state
/// can therefore never talk the loader into an unbounded allocation.
inline constexpr std::size_t kMaxAddressComponentBytes = 64;
inline constexpr std::size_t kMaxLabelBytes = 256;
inline constexpr std::size_t kMaxReasonBytes = 256;
inline constexpr std::size_t kMaxSourceBytes = 128;

/// Strict UTF-8 validation: rejects overlong encodings, UTF-16 surrogate code
/// points, values above U+10FFFF and truncated sequences. No replacement or
/// normalization is ever performed on rejected input.
PLR_API bool is_valid_utf8(std::string_view raw) noexcept;

/// Address component syntax (canonical form):
///   - 1..64 bytes;
///   - first and last byte are ASCII alphanumeric;
///   - interior bytes are ASCII alphanumeric or one of '.', '_', '-'.
///
/// The grammar cannot express '/', '\\', ':', whitespace, control characters or
/// a bare "."/".." component, so a canonical path can never be mistaken for a
/// filesystem path and persistence never derives a file name from it.
PLR_API bool is_valid_address_component(std::string_view raw) noexcept;

/// Explains the address component grammar; used in rejection messages.
PLR_API std::string_view address_component_syntax_help() noexcept;

/// Human-readable label syntax: valid UTF-8, at most 256 bytes, and free of
/// control characters (C0, DEL and C1). Labels are display text; they are
/// compared byte for byte and are never Unicode-normalized by the library.
PLR_API bool is_valid_label(std::string_view raw) noexcept;

/// Explains the label grammar; used in rejection messages.
PLR_API std::string_view label_syntax_help() noexcept;

/// Move/replacement reason syntax: valid UTF-8, at most 256 bytes, no control
/// characters other than nothing at all. Empty is allowed (no reason recorded).
PLR_API bool is_valid_reason(std::string_view raw) noexcept;

/// Provenance source syntax: valid UTF-8, at most 128 bytes, no control
/// characters. Empty is allowed (source unknown).
PLR_API bool is_valid_source(std::string_view raw) noexcept;

/// ASCII-only case folding, used exclusively for look-alike detection.
///
/// Byte-exact comparison remains the identity rule; folding exists so that two
/// sibling addresses (or an alias and an address) cannot differ only by ASCII
/// letter case and create a human-ambiguous address space.
PLR_API std::string ascii_fold(std::string_view raw);

/// True when two texts are equal ignoring ASCII letter case.
PLR_API bool ascii_case_insensitive_equal(std::string_view lhs, std::string_view rhs) noexcept;

/// Deterministic byte-wise three-way comparison.
PLR_API int byte_compare(std::string_view lhs, std::string_view rhs) noexcept;

}  // namespace dccp::physical_location_registry

#endif  // DCCP_PHYSICAL_LOCATION_REGISTRY_TEXT_HPP
