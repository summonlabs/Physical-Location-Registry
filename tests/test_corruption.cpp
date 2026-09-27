// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Hostile, malformed, truncated and oversized inputs. Every case must be
// rejected deterministically, without allocating from an unvalidated length and
// without presenting unverified bytes as authoritative state.

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "dccp/physical_location_registry/physical_location_registry.hpp"
#include "support/synthetic.hpp"
#include "support/test_harness.hpp"

using namespace dccp::physical_location_registry;
using plr_test::Fixture;
using plr_test::Rng;
using plr_test::TempDir;

namespace {

RegistryOpenOptions writer_options() {
  RegistryOpenOptions options;
  options.mode = OpenMode::ReadWrite;
  options.create_if_missing = true;
  return options;
}

RegistryOpenOptions reader_options() {
  RegistryOpenOptions options;
  options.mode = OpenMode::ReadOnly;
  return options;
}

std::string canonical_store_bytes(TempDir& dir) {
  auto registry = Registry::create(dir.path(), writer_options());
  if (!registry.has_value()) {
    return {};
  }
  Fixture fixture(registry.value(), 501);
  auto all = fixture.standard_facility("FAC1", 2, 2, 2, 2);
  if (!all.has_value()) {
    return {};
  }
  // Add aliases and a replacement so every record shape is present.
  const Limits limits = registry.value()->limits();
  auto alias = AddAliasRequest::make(all.value()[5].str(), "/LEGACY/RACK-1", limits,
                                     fixture.context());
  if (alias.has_value()) {
    (void)registry.value()->add_alias(alias.value());
  }
  auto replace = ReplaceLocationRequest::make(all.value()[all.value().size() - 1].str(),
                                              "loc-replacement", "replacement", fixture.context());
  if (replace.has_value()) {
    (void)registry.value()->replace_location(replace.value());
  }
  auto bytes = registry.value()->encode_current_state();
  if (!bytes.has_value()) {
    return {};
  }
  (void)registry.value()->close();
  return bytes.value();
}

void write_bytes(const std::filesystem::path& path, const std::string& content) {
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  stream.write(content.data(), static_cast<std::streamsize>(content.size()));
}

/// Replaces the value at "offset" with a byte-flipped variant.
std::string flip_byte(const std::string& bytes, std::size_t offset) {
  std::string copy = bytes;
  if (offset < copy.size()) {
    copy[offset] = static_cast<char>(copy[offset] ^ 0x5AU);
  }
  return copy;
}

}  // namespace

PLR_TEST(corruption, digest_and_header_damage_is_rejected) {
  TempDir dir;
  const std::string canonical = canonical_store_bytes(dir);
  PLR_REQUIRE(!canonical.empty());
  PLR_EXPECT_OK(snapshot, decode_snapshot(canonical));
  // 1 facility + 1 building + 2 rooms + 4 rows + 8 racks + 16 units, plus the
  // replacement successor created below.
  PLR_EXPECT_EQ(snapshot.location_count(), 33U);

  // Damage anywhere in the payload or in the integrity trailer is caught by the
  // digest; damage to the framing is caught by the framing checks. Either way,
  // nothing is accepted.
  for (const std::size_t offset : {std::size_t{20}, std::size_t{21}, canonical.size() / 2,
                                   canonical.size() - 33, canonical.size() - 1}) {
    const auto damaged = decode_snapshot(flip_byte(canonical, offset));
    PLR_EXPECT(!damaged.has_value());
    PLR_EXPECT_EQ(damaged.error().code(), ErrorCode::DigestMismatch);
  }

  // Magic, version, flags and declared length are validated in that order,
  // before any payload byte is interpreted.
  PLR_EXPECT_ERR(decode_snapshot(flip_byte(canonical, 0)), ErrorCode::MalformedRecord);
  PLR_EXPECT_ERR(decode_snapshot(flip_byte(canonical, 8)), ErrorCode::UnsupportedSchemaVersion);
  PLR_EXPECT_ERR(decode_snapshot(flip_byte(canonical, 10)), ErrorCode::UnsupportedFormatFlag);
  PLR_EXPECT_ERR(decode_snapshot(flip_byte(canonical, 13)), ErrorCode::CountMismatch);

  // Truncation at every prefix length is rejected, never partially accepted.
  for (std::size_t length = 0; length < canonical.size(); length += 97) {
    const auto truncated = decode_snapshot(canonical.substr(0, length));
    PLR_EXPECT(!truncated.has_value());
  }
  PLR_EXPECT_ERR(decode_snapshot(""), ErrorCode::TruncatedInput);
  PLR_EXPECT_ERR(decode_snapshot("PLRSTAT"), ErrorCode::TruncatedInput);
}

PLR_TEST(corruption, oversized_and_hostile_lengths_are_rejected_before_allocation) {
  // A payload length that claims far more than the file holds.
  std::string bytes;
  bytes.append("PLRSTAT\0", 8);
  bytes.push_back(static_cast<char>(1));
  bytes.push_back(static_cast<char>(0));
  bytes.push_back(static_cast<char>(0));
  bytes.push_back(static_cast<char>(0));
  for (int index = 0; index < 8; ++index) {
    bytes.push_back(static_cast<char>(0xFF));
  }
  bytes.append(std::string(64, '\0'));
  PLR_EXPECT_ERR(decode_snapshot(bytes), ErrorCode::CountMismatch);

  // A state larger than the absolute ceiling is refused without parsing.
  const std::string enormous(kHardMaxStateBytes + 4096, 'A');
  PLR_EXPECT_ERR(decode_snapshot(enormous), ErrorCode::LimitExceeded);

  // Random bytes of many lengths never decode, and never crash.
  Rng rng(987654321);
  for (int round = 0; round < 200; ++round) {
    const std::size_t length = rng.below(600);
    std::string noise;
    noise.reserve(length);
    for (std::size_t index = 0; index < length; ++index) {
      noise.push_back(static_cast<char>(rng.below(256)));
    }
    const auto decoded = decode_snapshot(noise);
    PLR_EXPECT(!decoded.has_value());
  }

  // A well-formed frame carrying noise is rejected by the digest, and a
  // well-formed frame with a valid digest but nonsense payload is rejected by
  // the structural validation.
  std::string framed;
  framed.append("PLRSTAT\0", 8);
  framed.push_back(static_cast<char>(1));
  framed.push_back(static_cast<char>(0));
  framed.push_back(static_cast<char>(0));
  framed.push_back(static_cast<char>(0));
  const std::string payload(128, 'Z');
  const std::uint64_t payload_size = payload.size();
  for (unsigned shift = 0; shift < 64; shift += 8) {
    framed.push_back(static_cast<char>((payload_size >> shift) & 0xFFU));
  }
  framed.append(payload);
  Sha256 hasher;
  hasher.update(framed);
  std::uint8_t digest[kSha256Bytes];
  hasher.finish(digest);
  std::string framed_with_digest = framed;
  framed_with_digest.append(reinterpret_cast<const char*>(digest), kSha256Bytes);
  const auto decoded = decode_snapshot(framed_with_digest);
  PLR_EXPECT(!decoded.has_value());
  PLR_EXPECT(decoded.error().code() == ErrorCode::TruncatedInput ||
             decoded.error().code() == ErrorCode::MalformedIdentifier ||
             decoded.error().code() == ErrorCode::LimitExceeded);
}

PLR_TEST(corruption, a_damaged_publication_never_becomes_authoritative) {
  TempDir dir;
  std::string committed_digest;
  {
    PLR_EXPECT_OK(registry, Registry::create(dir.path(), writer_options()));
    Fixture fixture(registry, 503);
    PLR_EXPECT_OK(all, fixture.standard_facility("FAC1", 1, 1, 1, 1));
    (void)all;
    committed_digest = registry->state_digest();
    PLR_EXPECT(registry->close().has_value());
  }

  // Damage the newest publication, then reopen: the store either recovers an
  // older verified generation or refuses, but never loads the damaged bytes.
  const std::filesystem::path newest = dir.path() / "state.7.plr";
  PLR_REQUIRE(std::filesystem::exists(newest));
  std::string bytes;
  {
    std::ifstream stream(newest, std::ios::binary);
    bytes.assign(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
  }
  write_bytes(newest, flip_byte(bytes, 40));

  {
    // Point head at the damaged publication explicitly.
    PLR_EXPECT_OK(reader, Registry::open(dir.path(), reader_options()));
    PLR_EXPECT(reader->recovery().head_invalid);
    PLR_EXPECT(reader->recovery().recovered_older_publication);
    PLR_EXPECT_NE(reader->state_digest(), std::string());
    PLR_EXPECT(reader->statistics().locations <= 6U);
  }

  // With every publication damaged, the store refuses to open at all.
  for (const auto& entry : std::filesystem::directory_iterator(dir.path())) {
    const std::string name = entry.path().filename().string();
    if (name.rfind("state.", 0) == 0) {
      write_bytes(entry.path(), flip_byte(bytes, 41));
    }
  }
  std::filesystem::remove(dir.path() / "head");
  PLR_EXPECT_ERR(Registry::open(dir.path(), reader_options()), ErrorCode::RecoveryUnavailable);
}

PLR_TEST(corruption, a_damaged_head_file_is_rejected_deterministically) {
  TempDir dir;
  {
    PLR_EXPECT_OK(registry, Registry::create(dir.path(), writer_options()));
    Fixture fixture(registry, 509);
    PLR_EXPECT_OK(all, fixture.standard_facility("FAC1", 1, 1, 1, 1));
    (void)all;
    PLR_EXPECT(registry->close().has_value());
  }
  const std::filesystem::path head = dir.path() / "head";
  const std::vector<std::string> hostile_heads = {
      "",
      "not-a-head\n",
      "PLRHEAD1\n",
      "PLRHEAD1 seq=1\n",
      "PLRHEAD1 seq=1 store=store-x rev=0 epoch=0\n",
      "PLRHEAD1 seq=1 store=store-x rev=0 epoch=0 file=state.1.plr",  // no newline
      "PLRHEAD1 seq=abc store=store-x rev=0 epoch=0 file=state.1.plr\n",
      "PLRHEAD1 seq=1 store=store-x rev=0 epoch=0 file=../../escape\n",
      "PLRHEAD1 seq=1 store=store-x rev=0 epoch=0 file=C:/windows/system32\n",
      "PLRHEAD1 seq=2 store=store-x rev=0 epoch=0 file=state.1.plr\n",  // sequence mismatch
      "PLRHEAD1 file=state.1.plr seq=1 store=store-x rev=0 epoch=0\n",  // reordered
      "PLRHEAD1 seq=1 store=store-x rev=0 epoch=0 file=state.1.plr extra=1\n",
      "PLRHEAD1 seq=1 store=store-x rev=0 epoch=0 file=state.1.plr\n\n",
      std::string("PLRHEAD1 seq=1 store=") + std::string(200, 'a') +
          " rev=0 epoch=0 file=state.1.plr\n",
  };
  for (const std::string& hostile : hostile_heads) {
    write_bytes(head, hostile);
    PLR_EXPECT(Registry::open(dir.path(), reader_options()).has_value());
    auto reader = Registry::open(dir.path(), reader_options());
    PLR_EXPECT(reader.value()->recovery().head_invalid ||
               reader.value()->recovery().recovered_older_publication);
    PLR_EXPECT_EQ(reader.value()->statistics().locations, 6U);
  }

  // A head that is far too large is refused by the bounded read.
  write_bytes(head, std::string(4096, 'H'));
  PLR_EXPECT_OK(reader, Registry::open(dir.path(), reader_options()));
  PLR_EXPECT(reader->recovery().head_unreadable || reader->recovery().head_invalid);
  PLR_EXPECT_EQ(reader->statistics().locations, 6U);
}

PLR_TEST(corruption, hostile_component_and_label_text_never_reaches_state) {
  TempDir dir;
  PLR_EXPECT_OK(registry, Registry::create(dir.path(), writer_options()));
  Fixture fixture(registry, 521);
  PLR_EXPECT_OK(facility, fixture.facility("FAC1"));

  const std::vector<std::string> hostile_components = {
      "..", ".", "../..", "..\\..", "a/../../b", "C:", "C:\\", "/absolute", "\\absolute",
      "%2e%2e%2f", "a%00b", "a b", "a\tb", "a\nb", "\xEF\xBB\xBF" "bom", "\xE2\x80\xAE" "rtl",
      std::string(65, 'a'),         std::string("a\0b", 3),
  };
  for (const std::string& component : hostile_components) {
    auto request = CreateLocationRequest::make("loc-" + std::to_string(component.size()),
                                               LocationKind::Room, facility.str(), component, "",
                                               fixture.context());
    if (request.has_value()) {
      const auto receipt = registry->create_location(request.value());
      PLR_EXPECT(!receipt.has_value());
    }
  }

  const std::vector<std::string> hostile_labels = {
      "with\nnewline", "with\ttab", std::string("nul\0byte", 8), "\x7F", "\xC2\x85",
      "\xEF\xB7\x90", "\xC3\x28", std::string(257, 'x'),
  };
  for (const std::string& label : hostile_labels) {
    auto request = CreateLocationRequest::make("loc-label" + std::to_string(label.size()),
                                               LocationKind::Room, facility.str(), "ROOM-1", label,
                                               fixture.context());
    PLR_EXPECT(!request.has_value());
  }

  // Nothing hostile was committed: only the facility exists.
  PLR_EXPECT_EQ(registry->statistics().locations, 1U);
  PLR_EXPECT_EQ(registry->revision().value(), 1U);

  // The store directory contains exactly the files the registry owns.
  std::vector<std::string> names;
  for (const auto& entry : std::filesystem::directory_iterator(dir.path())) {
    names.push_back(entry.path().filename().string());
  }
  for (const std::string& name : names) {
    PLR_EXPECT(name == "head" || name == "store.lock" || name.rfind("state.", 0) == 0);
  }
}

PLR_TEST(corruption, unresolvable_paths_report_precisely) {
  TempDir dir;
  PLR_EXPECT_OK(registry, Registry::create(dir.path(), writer_options()));
  Fixture fixture(registry, 523);
  PLR_EXPECT_OK(all, fixture.standard_facility("FAC1", 1, 1, 1, 1));
  (void)all;
  const Limits limits = registry->limits();

  struct Case {
    const char* path;
    ErrorCode code;
  };
  const Case cases[] = {
      {"/NOPE", ErrorCode::NotFound},
      {"/FAC1/NOPE", ErrorCode::NotFound},
      {"/FAC1/BLDG-1/NOPE", ErrorCode::NotFound},
      {"/FAC1/BLDG-1/ROOM-1/ROW-1/RACK-1/U1/DEEPER", ErrorCode::NotFound},
  };
  for (const Case& item : cases) {
    PLR_EXPECT_OK(path, LocationPath::parse(item.path, limits));
    PLR_EXPECT_ERR(registry->resolve(path), item.code);
    const Explanation explanation = registry->explain_resolve(path);
    PLR_EXPECT(!explanation.ok());
    PLR_EXPECT(!explanation.details.empty());
  }
}
