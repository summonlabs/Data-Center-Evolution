// Unit and adversarial tests for the primitive layer: checked arithmetic, text
// validation, numeric and version parsing, digests and the canonical codec.
//
// Every case asserts a documented bound rather than an implementation accident:
// an overflow must be refused instead of wrapping, an identifier that could
// become a path component must be rejected, and a hostile length prefix must
// never be trusted far enough to read or allocate past the payload.
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "dce/canonical.hpp"
#include "dce/checked.hpp"
#include "dce/digest.hpp"
#include "dce/ids.hpp"
#include "dce/status.hpp"
#include "dce/text.hpp"
#include "dce/version.hpp"
#include "harness.hpp"

namespace {

using dce::Error;
using dce::ErrorCode;

constexpr std::uint16_t kU16Max = std::numeric_limits<std::uint16_t>::max();
constexpr std::uint32_t kU32Max = std::numeric_limits<std::uint32_t>::max();
constexpr std::uint64_t kU64Max = std::numeric_limits<std::uint64_t>::max();

// The byte view of a text fragment, used where an API takes bytes rather than
// characters. The length is explicit so that embedded NUL bytes survive.
std::vector<std::byte> bytes_of(std::string_view text) {
  std::vector<std::byte> out;
  out.reserve(text.size());
  for (char character : text) {
    out.push_back(static_cast<std::byte>(static_cast<unsigned char>(character)));
  }
  return out;
}

unsigned byte_at(const std::vector<std::byte>& data, std::size_t index) {
  return static_cast<unsigned>(data[index]);
}

// Reports a table row that the validator judged differently from the expected
// outcome. The message names the exact input, which a macro cannot do when the
// input is a loop variable.
void expect_identifier(std::string_view value, bool accepted) {
  const bool actual = dce::text::valid_identifier(value);
  if (actual == accepted) {
    dce::test::count_assertion();
    return;
  }
  DCE_FAIL("valid_identifier(" + dce::test::to_debug_string(value) + ") returned " +
           dce::test::to_debug_string(actual) + ", expected " + dce::test::to_debug_string(accepted));
}

}  // namespace

// ---------------------------------------------------------------------------
// Checked arithmetic
// ---------------------------------------------------------------------------
DCE_TEST(primitives, checked_add_refuses_to_wrap) {
  DCE_CHECK_EQ(dce::checked_add<std::uint32_t>(0u, 0u), std::optional<std::uint32_t>(0u));
  DCE_CHECK_EQ(dce::checked_add<std::uint32_t>(0u, kU32Max), std::optional<std::uint32_t>(kU32Max));
  DCE_CHECK_EQ(dce::checked_add<std::uint32_t>(kU32Max, 0u), std::optional<std::uint32_t>(kU32Max));
  DCE_CHECK_EQ(dce::checked_add<std::uint32_t>(kU32Max - 1u, 1u), std::optional<std::uint32_t>(kU32Max));
  DCE_CHECK_EQ(dce::checked_add<std::uint32_t>(kU32Max, 1u), std::nullopt);
  DCE_CHECK_EQ(dce::checked_add<std::uint32_t>(kU32Max, kU32Max), std::nullopt);
  DCE_CHECK_EQ(dce::checked_add<std::uint32_t>(kU32Max / 2u, kU32Max / 2u),
               std::optional<std::uint32_t>(kU32Max - 1u));
  DCE_CHECK_EQ(dce::checked_add<std::uint32_t>(kU32Max / 2u, kU32Max / 2u + 1u),
               std::optional<std::uint32_t>(kU32Max));

  DCE_CHECK_EQ(dce::checked_add<std::uint64_t>(kU64Max - 1ull, 1ull), std::optional<std::uint64_t>(kU64Max));
  DCE_CHECK_EQ(dce::checked_add<std::uint64_t>(kU64Max, 1ull), std::nullopt);
  DCE_CHECK_EQ(dce::checked_add<std::uint64_t>(kU64Max, kU64Max), std::nullopt);
  DCE_CHECK_EQ(dce::checked_add<std::uint64_t>(1ull << 63, (1ull << 63) - 1ull),
               std::optional<std::uint64_t>(kU64Max));
  DCE_CHECK_EQ(dce::checked_add<std::uint64_t>(1ull << 63, 1ull << 63), std::nullopt);
}

DCE_TEST(primitives, checked_sub_refuses_to_wrap) {
  DCE_CHECK_EQ(dce::checked_sub<std::uint32_t>(0u, 0u), std::optional<std::uint32_t>(0u));
  DCE_CHECK_EQ(dce::checked_sub<std::uint32_t>(1u, 1u), std::optional<std::uint32_t>(0u));
  DCE_CHECK_EQ(dce::checked_sub<std::uint32_t>(0u, 1u), std::nullopt);
  DCE_CHECK_EQ(dce::checked_sub<std::uint32_t>(kU32Max, kU32Max), std::optional<std::uint32_t>(0u));
  DCE_CHECK_EQ(dce::checked_sub<std::uint32_t>(kU32Max, 1u), std::optional<std::uint32_t>(kU32Max - 1u));

  DCE_CHECK_EQ(dce::checked_sub<std::uint64_t>(0ull, 1ull), std::nullopt);
  DCE_CHECK_EQ(dce::checked_sub<std::uint64_t>(kU64Max, kU64Max), std::optional<std::uint64_t>(0ull));
  DCE_CHECK_EQ(dce::checked_sub<std::uint64_t>(kU64Max, kU64Max - 1ull), std::optional<std::uint64_t>(1ull));
  DCE_CHECK_EQ(dce::checked_sub<std::uint64_t>(1ull << 63, (1ull << 63) + 1ull), std::nullopt);
}

DCE_TEST(primitives, checked_mul_refuses_to_wrap) {
  DCE_CHECK_EQ(dce::checked_mul<std::uint32_t>(0u, kU32Max), std::optional<std::uint32_t>(0u));
  DCE_CHECK_EQ(dce::checked_mul<std::uint32_t>(kU32Max, 1u), std::optional<std::uint32_t>(kU32Max));
  DCE_CHECK_EQ(dce::checked_mul<std::uint32_t>(1u, kU32Max), std::optional<std::uint32_t>(kU32Max));
  DCE_CHECK_EQ(dce::checked_mul<std::uint32_t>(kU32Max, 2u), std::nullopt);
  // 65536 * 65536 wraps to exactly zero in 32 bits; the wrap must be refused.
  DCE_CHECK_EQ(dce::checked_mul<std::uint32_t>(65536u, 65536u), std::nullopt);
  DCE_CHECK_EQ(dce::checked_mul<std::uint32_t>(65536u, 65535u), std::optional<std::uint32_t>(4294901760u));
  // 65535 * 65537 is exactly 2^32-1, the largest value that still fits.
  DCE_CHECK_EQ(dce::checked_mul<std::uint32_t>(65535u, 65537u), std::optional<std::uint32_t>(kU32Max));
  DCE_CHECK_EQ(dce::checked_mul<std::uint32_t>(65535u, 65538u), std::nullopt);

  DCE_CHECK_EQ(dce::checked_mul<std::uint64_t>(kU64Max, 1ull), std::optional<std::uint64_t>(kU64Max));
  DCE_CHECK_EQ(dce::checked_mul<std::uint64_t>(kU64Max, 2ull), std::nullopt);
  // 2^32 * 2^32 wraps to exactly zero in 64 bits; the wrap must be refused.
  DCE_CHECK_EQ(dce::checked_mul<std::uint64_t>(1ull << 32, 1ull << 32), std::nullopt);
  DCE_CHECK_EQ(dce::checked_mul<std::uint64_t>(1ull << 32, (1ull << 32) - 1ull),
               std::optional<std::uint64_t>(0xffffffff00000000ull));
}

DCE_TEST(primitives, checked_next_and_accumulate) {
  DCE_CHECK_EQ(dce::checked_next<std::uint32_t>(0u), std::optional<std::uint32_t>(1u));
  DCE_CHECK_EQ(dce::checked_next<std::uint32_t>(kU32Max - 1u), std::optional<std::uint32_t>(kU32Max));
  DCE_CHECK_EQ(dce::checked_next<std::uint32_t>(kU32Max), std::nullopt);
  DCE_CHECK_EQ(dce::checked_next<std::uint64_t>(kU64Max - 1ull), std::optional<std::uint64_t>(kU64Max));
  DCE_CHECK_EQ(dce::checked_next<std::uint64_t>(kU64Max), std::nullopt);

  std::uint32_t accumulator = kU32Max - 1u;
  DCE_CHECK_TRUE(dce::accumulate(accumulator, 1u));
  DCE_CHECK_EQ(accumulator, kU32Max);
  DCE_CHECK_TRUE(!dce::accumulate(accumulator, 1u));
  // A refused accumulation must leave the accumulator exactly as it was.
  DCE_CHECK_EQ(accumulator, kU32Max);
  DCE_CHECK_TRUE(dce::accumulate(accumulator, 0u));
  DCE_CHECK_EQ(accumulator, kU32Max);
}

DCE_TEST(primitives, checked_narrow_and_fits) {
  DCE_CHECK_TRUE(dce::fits<std::uint32_t>(kU32Max));
  DCE_CHECK_TRUE(!dce::fits<std::uint32_t>(static_cast<std::uint64_t>(kU32Max) + 1ull));
  DCE_CHECK_TRUE(dce::fits<std::uint8_t>(255ull));
  DCE_CHECK_TRUE(!dce::fits<std::uint8_t>(256ull));

  DCE_CHECK_EQ(dce::checked_narrow<std::uint32_t>(kU32Max), std::optional<std::uint32_t>(kU32Max));
  DCE_CHECK_EQ(dce::checked_narrow<std::uint32_t>(static_cast<std::uint64_t>(kU32Max) + 1ull), std::nullopt);
  DCE_CHECK_EQ(dce::checked_narrow<std::uint16_t>(65535ull), std::optional<std::uint16_t>(kU16Max));
  DCE_CHECK_EQ(dce::checked_narrow<std::uint16_t>(65536ull), std::nullopt);
  DCE_CHECK_EQ(dce::checked_narrow<std::uint8_t>(0ull), std::optional<std::uint8_t>(0u));
  DCE_CHECK_EQ(dce::checked_narrow<std::uint8_t>(255ull), std::optional<std::uint8_t>(255u));
  DCE_CHECK_EQ(dce::checked_narrow<std::uint8_t>(256ull), std::nullopt);
}

DCE_TEST(primitives, checked_delta_refuses_to_go_backwards) {
  DCE_CHECK_EQ(dce::checked_delta<std::uint64_t>(9ull, 4ull), std::optional<std::uint64_t>(5ull));
  DCE_CHECK_EQ(dce::checked_delta<std::uint64_t>(4ull, 4ull), std::optional<std::uint64_t>(0ull));
  DCE_CHECK_EQ(dce::checked_delta<std::uint64_t>(4ull, 9ull), std::nullopt);
  DCE_CHECK_EQ(dce::checked_delta<std::uint32_t>(0u, 0u), std::optional<std::uint32_t>(0u));
}

// ---------------------------------------------------------------------------
// Identities and counters
// ---------------------------------------------------------------------------
DCE_TEST(primitives, counters_refuse_to_wrap_or_go_backwards) {
  const dce::PlanGeneration zero = dce::PlanGeneration::from_value(0ull);
  DCE_CHECK_TRUE(zero.is_zero());
  const auto advanced = zero.next();
  DCE_REQUIRE_OK(advanced);
  DCE_CHECK_EQ(advanced->value(), 1ull);
  DCE_CHECK_EQ(zero.value(), 0ull);

  const dce::LogSequence last = dce::LogSequence::from_value(kU64Max);
  DCE_CHECK_TRUE(!last.is_zero());
  DCE_CHECK_CODE(last.next(), ErrorCode::overflow);

  const dce::StageOrdinal top = dce::StageOrdinal::from_value(kU32Max);
  DCE_CHECK_CODE(top.next(), ErrorCode::overflow);

  DCE_CHECK_OK(top.require_at_least(dce::StageOrdinal::from_value(kU32Max)));
  DCE_CHECK_OK(top.require_at_least(dce::StageOrdinal::from_value(0u)));
  DCE_CHECK_CODE(dce::StageOrdinal::from_value(3u).require_at_least(dce::StageOrdinal::from_value(4u)),
                 ErrorCode::stale);
}

DCE_TEST(primitives, identity_parse_validates) {
  const auto site = dce::SiteId::parse("site-1");
  DCE_REQUIRE_OK(site);
  DCE_CHECK_TRUE(site->valid());
  DCE_CHECK_EQ(site->str(), std::string("site-1"));
  DCE_CHECK_EQ(site->view(), std::string_view("site-1"));

  const auto same = dce::SiteId::parse("site-1");
  DCE_REQUIRE_OK(same);
  const auto other = dce::SiteId::parse("site-2");
  DCE_REQUIRE_OK(other);
  DCE_CHECK_TRUE(*site == *same);
  DCE_CHECK_TRUE(*site != *other);

  DCE_CHECK_CODE(dce::SiteId::parse(""), ErrorCode::invalid_argument);
  DCE_CHECK_CODE(dce::SiteId::parse("a/b"), ErrorCode::invalid_argument);
  DCE_CHECK_CODE(dce::SiteId::parse(".."), ErrorCode::invalid_argument);
  // The default-constructed identity is absent, not a valid empty identity.
  DCE_CHECK_TRUE(!dce::SiteId{}.valid());
}

// ---------------------------------------------------------------------------
// Text validation and escaping
// ---------------------------------------------------------------------------
DCE_TEST(primitives, identifier_accepts_ordinary_names) {
  DCE_CHECK_TRUE(dce::text::valid_identifier("a"));
  DCE_CHECK_TRUE(dce::text::valid_identifier("A9"));
  DCE_CHECK_TRUE(dce::text::valid_identifier("0"));
  DCE_CHECK_TRUE(dce::text::valid_identifier("site-1"));
  DCE_CHECK_TRUE(dce::text::valid_identifier("component_2"));
  DCE_CHECK_TRUE(dce::text::valid_identifier("evolution.plan.step"));
  DCE_CHECK_TRUE(dce::text::valid_identifier("actor@example"));
  // A colon is deliberately NOT accepted: these identities become file name
  // components, and on Windows a colon separates a name from an alternate data
  // stream, so "site:a" would not name the file it appears to name.
  DCE_CHECK_TRUE(!dce::text::valid_identifier("namespace:name"));
  DCE_CHECK_TRUE(!dce::text::valid_identifier("site:stream"));
  // Names that merely start with a device name are ordinary identities.
  DCE_CHECK_TRUE(dce::text::valid_identifier("con1"));
  DCE_CHECK_TRUE(dce::text::valid_identifier("console"));
  DCE_CHECK_TRUE(dce::text::valid_identifier("nul1"));
  DCE_CHECK_TRUE(dce::text::valid_identifier("aux-1"));
  // 96 characters is the documented maximum and is accepted.
  DCE_CHECK_TRUE(dce::text::valid_identifier(std::string(dce::text::kMaxIdentifierLength, 'a')));
}

DCE_TEST(primitives, identifier_rejects_hostile_forms) {
  expect_identifier("", false);
  expect_identifier(".", false);
  expect_identifier("..", false);
  expect_identifier(".leading", false);
  expect_identifier("trailing.", false);
  expect_identifier("-leading", false);
  expect_identifier("_leading", false);
  expect_identifier("trailing-", false);
  expect_identifier("a..b", false);
  expect_identifier("a/b", false);
  expect_identifier("a\\b", false);
  expect_identifier("a b", false);
  expect_identifier("a\tb", false);
  expect_identifier("a\nb", false);
  expect_identifier("caf\xc3\xa9", false);
  expect_identifier(std::string_view("\x80", 1), false);
  expect_identifier(std::string_view("\x7f", 1), false);
  expect_identifier(std::string_view("a\0b", 3), false);
  // One character past the documented maximum must be refused.
  expect_identifier(std::string(dce::text::kMaxIdentifierLength + 1u, 'a'), false);
}

DCE_TEST(primitives, identifier_rejects_reserved_device_names) {
  const std::vector<std::string_view> reserved = {
      "con",  "CON",  "Con",  "prn",  "aux",  "nul",  "com1", "com2", "com3", "com4", "com5", "com6",
      "com7", "com8", "com9", "lpt1", "lpt2", "lpt3", "lpt4", "lpt5", "lpt6", "lpt7", "lpt8", "lpt9"};
  for (std::string_view name : reserved) {
    expect_identifier(name, false);
  }
  // Windows resolves these names with an extension, so the stem decides.
  const std::vector<std::string_view> with_extension = {"CON.txt", "con.log",    "NUL.tar.gz",
                                                        "com1.bin", "lpt9.dat",  "aux.tar.gz"};
  for (std::string_view name : with_extension) {
    expect_identifier(name, false);
  }
}

DCE_TEST(primitives, escape_for_output_neutralises_terminal_control) {
  DCE_CHECK_EQ(dce::text::escape_for_output("plain text"), std::string("plain text"));
  DCE_CHECK_EQ(dce::text::escape_for_output("\x1b[31mred\x1b[0m"), std::string("\\x1b[31mred\\x1b[0m"));
  DCE_CHECK_EQ(dce::text::escape_for_output("\r\n"), std::string("\\x0d\\x0a"));
  DCE_CHECK_EQ(dce::text::escape_for_output(std::string_view("\x00", 1)), std::string("\\x00"));
  DCE_CHECK_EQ(dce::text::escape_for_output(std::string_view("\x7f", 1)), std::string("\\x7f"));
  DCE_CHECK_EQ(dce::text::escape_for_output(std::string_view("\xff", 1)), std::string("\\xff"));
  // A backslash is escaped too, so an escape sequence cannot be reassembled by
  // a reader that interprets backslashes on the way out.
  DCE_CHECK_EQ(dce::text::escape_for_output("a\\b"), std::string("a\\x5cb"));
  DCE_CHECK_TRUE(dce::text::is_printable_ascii(dce::text::escape_for_output("\x1b]0;title\x07")));
}

DCE_TEST(primitives, escape_for_output_neutralises_every_byte) {
  std::string every_byte;
  for (unsigned value = 0; value < 256u; ++value) {
    every_byte.push_back(static_cast<char>(value));
  }
  const std::string escaped = dce::text::escape_for_output(every_byte);
  DCE_REQUIRE(dce::text::is_printable_ascii(escaped));
  for (char character : escaped) {
    DCE_CHECK_TRUE(character >= ' ' && character <= '~');
  }
  DCE_CHECK_NE(escaped.find("\\x00"), std::string::npos);
  DCE_CHECK_NE(escaped.find("\\x1b"), std::string::npos);
  DCE_CHECK_NE(escaped.find("\\x5c"), std::string::npos);
  DCE_CHECK_NE(escaped.find("\\xff"), std::string::npos);
}

DCE_TEST(primitives, escape_for_output_truncates_oversized_text) {
  const std::string oversized(dce::text::kMaxOutputTextLength + 64u, 'a');
  const std::string escaped = dce::text::escape_for_output(oversized);
  DCE_CHECK_EQ(escaped.size(), dce::text::kMaxOutputTextLength + 3u);
  DCE_CHECK_EQ(escaped.substr(escaped.size() - 3u), std::string("..."));
  DCE_CHECK_EQ(dce::text::escape_for_output(std::string(dce::text::kMaxOutputTextLength, 'a')).size(),
               dce::text::kMaxOutputTextLength);
}

// ---------------------------------------------------------------------------
// Numeric parsing
// ---------------------------------------------------------------------------
DCE_TEST(primitives, parse_u64_accepts_its_full_domain) {
  const auto zero = dce::text::parse_u64("0");
  DCE_REQUIRE_OK(zero);
  DCE_CHECK_EQ(*zero, 0ull);

  const auto maximum = dce::text::parse_u64("18446744073709551615");
  DCE_REQUIRE_OK(maximum);
  DCE_CHECK_EQ(*maximum, kU64Max);

  const auto leading_zeroes = dce::text::parse_u64("0007");
  DCE_REQUIRE_OK(leading_zeroes);
  DCE_CHECK_EQ(*leading_zeroes, 7ull);
}

DCE_TEST(primitives, parse_u64_rejects_malformed_and_overflow) {
  DCE_CHECK_CODE(dce::text::parse_u64(""), ErrorCode::invalid_argument);
  DCE_CHECK_CODE(dce::text::parse_u64("+1"), ErrorCode::invalid_argument);
  DCE_CHECK_CODE(dce::text::parse_u64("-1"), ErrorCode::invalid_argument);
  DCE_CHECK_CODE(dce::text::parse_u64("1 2"), ErrorCode::invalid_argument);
  DCE_CHECK_CODE(dce::text::parse_u64("0x10"), ErrorCode::invalid_argument);
  DCE_CHECK_CODE(dce::text::parse_u64("1.0"), ErrorCode::invalid_argument);
  DCE_CHECK_CODE(dce::text::parse_u64(" 1"), ErrorCode::invalid_argument);
  DCE_CHECK_CODE(dce::text::parse_u64("1\n"), ErrorCode::invalid_argument);
  // Exactly 2^64 and the largest 20-digit value overflow a 64-bit accumulator.
  DCE_CHECK_CODE(dce::text::parse_u64("18446744073709551616"), ErrorCode::overflow);
  DCE_CHECK_CODE(dce::text::parse_u64("99999999999999999999"), ErrorCode::overflow);
  // More digits than a 64-bit value can have are refused before accumulation.
  DCE_CHECK_CODE(dce::text::parse_u64("123456789012345678901"), ErrorCode::out_of_range);
}

DCE_TEST(primitives, parse_u32_rejects_overflow_and_malformed) {
  const auto zero = dce::text::parse_u32("0");
  DCE_REQUIRE_OK(zero);
  DCE_CHECK_EQ(*zero, 0u);

  const auto maximum = dce::text::parse_u32("4294967295");
  DCE_REQUIRE_OK(maximum);
  DCE_CHECK_EQ(*maximum, kU32Max);

  DCE_CHECK_CODE(dce::text::parse_u32("4294967296"), ErrorCode::out_of_range);
  DCE_CHECK_CODE(dce::text::parse_u32("18446744073709551616"), ErrorCode::overflow);
  DCE_CHECK_CODE(dce::text::parse_u32("123456789012345678901"), ErrorCode::out_of_range);
  DCE_CHECK_CODE(dce::text::parse_u32(""), ErrorCode::invalid_argument);
  DCE_CHECK_CODE(dce::text::parse_u32("+1"), ErrorCode::invalid_argument);
  DCE_CHECK_CODE(dce::text::parse_u32("-1"), ErrorCode::invalid_argument);
  DCE_CHECK_CODE(dce::text::parse_u32("1 2"), ErrorCode::invalid_argument);
  DCE_CHECK_CODE(dce::text::parse_u32("0x10"), ErrorCode::invalid_argument);
}

DCE_TEST(primitives, number_formatting_round_trips) {
  DCE_CHECK_EQ(dce::text::u64_to_string(0ull), std::string("0"));
  DCE_CHECK_EQ(dce::text::u64_to_string(kU64Max), std::string("18446744073709551615"));
  DCE_CHECK_EQ(dce::text::u32_to_string(0u), std::string("0"));
  DCE_CHECK_EQ(dce::text::u32_to_string(kU32Max), std::string("4294967295"));

  const std::vector<std::uint64_t> values = {0ull, 1ull, 9ull, 10ull, 999ull, 1000000000000ull, kU64Max};
  for (std::uint64_t value : values) {
    const auto parsed = dce::text::parse_u64(dce::text::u64_to_string(value));
    DCE_REQUIRE_OK(parsed);
    DCE_CHECK_EQ(*parsed, value);
  }
}

// ---------------------------------------------------------------------------
// Versions
// ---------------------------------------------------------------------------
DCE_TEST(primitives, version_parse_accepts_exactly_three_fields) {
  const auto version = dce::Version::parse("1.2.3");
  DCE_REQUIRE_OK(version);
  DCE_CHECK_EQ(version->major, 1u);
  DCE_CHECK_EQ(version->minor, 2u);
  DCE_CHECK_EQ(version->patch, 3u);
  DCE_CHECK_EQ(version->to_string(), std::string("1.2.3"));
  DCE_CHECK_TRUE(!version->is_zero());

  const auto zero = dce::Version::parse("0.0.0");
  DCE_REQUIRE_OK(zero);
  DCE_CHECK_TRUE(zero->is_zero());
  DCE_CHECK_TRUE(*zero < *version);

  const auto maximum = dce::Version::parse("4294967295.4294967295.4294967295");
  DCE_REQUIRE_OK(maximum);
  DCE_CHECK_EQ(maximum->major, kU32Max);
  DCE_CHECK_EQ(maximum->to_string(), std::string("4294967295.4294967295.4294967295"));
}

DCE_TEST(primitives, version_parse_rejects_malformed_fields) {
  DCE_CHECK_CODE(dce::Version::parse("1.2"), ErrorCode::malformed);
  DCE_CHECK_CODE(dce::Version::parse("1.2.3.4"), ErrorCode::malformed);
  DCE_CHECK_CODE(dce::Version::parse("1..2"), ErrorCode::malformed);
  DCE_CHECK_CODE(dce::Version::parse("1.2."), ErrorCode::malformed);
  DCE_CHECK_CODE(dce::Version::parse(""), ErrorCode::malformed);
  DCE_CHECK_CODE(dce::Version::parse("..."), ErrorCode::malformed);
  DCE_CHECK_CODE(dce::Version::parse("1.2.-1"), ErrorCode::invalid_argument);
  DCE_CHECK_CODE(dce::Version::parse("a.b.c"), ErrorCode::invalid_argument);
  DCE_CHECK_CODE(dce::Version::parse("1.2.3 "), ErrorCode::invalid_argument);
  // A field above 2^32-1 is refused rather than truncated.
  DCE_CHECK_CODE(dce::Version::parse("4294967296.0.0"), ErrorCode::out_of_range);
  DCE_CHECK_CODE(dce::Version::parse("0.0.4294967296"), ErrorCode::out_of_range);
  DCE_CHECK_CODE(dce::Version::parse("18446744073709551616.0.0"), ErrorCode::overflow);
}

DCE_TEST(primitives, version_range_parse_and_contains) {
  const auto range = dce::VersionRange::parse("1.0.0..2.0.0");
  DCE_REQUIRE_OK(range);
  DCE_CHECK_EQ(range->low.to_string(), std::string("1.0.0"));
  DCE_CHECK_TRUE(range->high.has_value());
  DCE_CHECK_EQ(range->to_string(), std::string("1.0.0..2.0.0"));
  // Both bounds are inclusive.
  DCE_CHECK_TRUE(range->contains(dce::Version{1u, 0u, 0u}));
  DCE_CHECK_TRUE(range->contains(dce::Version{1u, 5u, 0u}));
  DCE_CHECK_TRUE(range->contains(dce::Version{2u, 0u, 0u}));
  DCE_CHECK_TRUE(!range->contains(dce::Version{0u, 9u, 9u}));
  DCE_CHECK_TRUE(!range->contains(dce::Version{2u, 0u, 1u}));

  const auto open = dce::VersionRange::parse("1.0.0..");
  DCE_REQUIRE_OK(open);
  DCE_CHECK_TRUE(!open->high.has_value());
  DCE_CHECK_TRUE(open->contains(dce::Version{99u, 0u, 0u}));
  DCE_CHECK_TRUE(!open->contains(dce::Version{0u, 9u, 9u}));
  DCE_CHECK_EQ(open->to_string(), std::string("1.0.0.."));

  // Whitespace around either bound is trimmed, not rejected.
  const auto spaced = dce::VersionRange::parse(" 1.0.0 .. 2.0.0 ");
  DCE_REQUIRE_OK(spaced);
  DCE_CHECK_EQ(spaced->to_string(), std::string("1.0.0..2.0.0"));

  DCE_CHECK_CODE(dce::VersionRange::parse("2.0.0..1.0.0"), ErrorCode::invalid_argument);
  DCE_CHECK_CODE(dce::VersionRange::parse("1.0.0"), ErrorCode::malformed);
  DCE_CHECK_CODE(dce::VersionRange::parse(""), ErrorCode::malformed);
  DCE_CHECK_CODE(dce::VersionRange::parse(".."), ErrorCode::malformed);
  DCE_CHECK_CODE(dce::VersionRange::parse("1.0.0..2.0"), ErrorCode::malformed);
}

// ---------------------------------------------------------------------------
// Digests
// ---------------------------------------------------------------------------
DCE_TEST(primitives, sha256_published_vectors) {
  // FIPS 180-4 sample values and the classic multi-block vectors.
  DCE_CHECK_EQ(dce::Digest256::of(std::string_view("")).to_hex(),
               std::string("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
  DCE_CHECK_EQ(dce::Digest256::of(std::string_view("abc")).to_hex(),
               std::string("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));

  const std::string_view fifty_six =
      "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
  DCE_CHECK_EQ(fifty_six.size(), std::size_t{56});
  DCE_CHECK_EQ(dce::Digest256::of(fifty_six).to_hex(),
               std::string("248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"));

  const std::string million_a(std::size_t{1000000}, 'a');
  DCE_CHECK_EQ(dce::Digest256::of(million_a).to_hex(),
               std::string("cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"));
}

DCE_TEST(primitives, sha256_streaming_matches_one_shot) {
  std::string message;
  message.reserve(1000);
  for (unsigned index = 0; index < 1000u; ++index) {
    message.push_back(static_cast<char>('a' + static_cast<int>(index % 26u)));
  }

  const std::string expected = dce::Digest256::of(message).to_hex();
  // Every chunk size straddles a block boundary differently: 63 and 64 fill the
  // block exactly, 65 forces a spill into the next one.
  const std::vector<std::size_t> chunk_sizes = {1u, 7u, 63u, 64u, 65u, 1000u};
  for (std::size_t chunk : chunk_sizes) {
    dce::Sha256 hasher;
    std::size_t offset = 0;
    while (offset < message.size()) {
      const std::size_t length = std::min(chunk, message.size() - offset);
      hasher.update(std::string_view(message).substr(offset, length));
      offset += length;
    }
    dce::Digest256 digest;
    digest.bytes = hasher.finish();
    DCE_CHECK_EQ(digest.to_hex(), expected);
  }

  // Byte-at-a-time and byte-span entry points agree with the string entry point.
  dce::Sha256 bytewise;
  for (char character : message) {
    bytewise.update(static_cast<std::uint8_t>(static_cast<unsigned char>(character)));
  }
  dce::Digest256 from_bytes;
  from_bytes.bytes = bytewise.finish();
  DCE_CHECK_EQ(from_bytes.to_hex(), expected);

  dce::Digest256 from_span;
  from_span.bytes = dce::Sha256::of(std::span<const std::byte>(bytes_of(message)));
  DCE_CHECK_EQ(from_span.to_hex(), expected);
}

DCE_TEST(primitives, crc32_check_values) {
  DCE_CHECK_EQ(dce::crc32(std::string_view{}), 0u);
  DCE_CHECK_EQ(dce::crc32("123456789"), 0xCBF43926u);
  DCE_CHECK_EQ(dce::crc32("a"), 0xE8B7BE43u);
  DCE_CHECK_EQ(dce::crc32("The quick brown fox jumps over the lazy dog"), 0x414FA339u);

  const std::vector<std::byte> bytes = bytes_of("123456789");
  DCE_CHECK_EQ(dce::crc32(std::span<const std::byte>(bytes)), 0xCBF43926u);
}

DCE_TEST(primitives, digest256_hex_round_trip) {
  const dce::Digest256 digest = dce::Digest256::of(std::string_view("digest round trip"));
  const std::string hex = digest.to_hex();
  DCE_CHECK_EQ(hex.size(), std::size_t{64});
  DCE_CHECK_TRUE(!digest.is_zero());
  DCE_CHECK_TRUE(dce::Digest256::zero().is_zero());

  const auto parsed = dce::Digest256::parse_hex(hex);
  DCE_REQUIRE_OK(parsed);
  DCE_CHECK_EQ(parsed->to_hex(), hex);

  // Upper case input is accepted and normalised to the canonical lower case.
  std::string upper = hex;
  for (char& character : upper) {
    if (character >= 'a' && character <= 'f') {
      character = static_cast<char>(character - 'a' + 'A');
    }
  }
  const auto parsed_upper = dce::Digest256::parse_hex(upper);
  DCE_REQUIRE_OK(parsed_upper);
  DCE_CHECK_EQ(parsed_upper->to_hex(), hex);

  DCE_CHECK_CODE(dce::Digest256::parse_hex(""), ErrorCode::invalid_argument);
  DCE_CHECK_CODE(dce::Digest256::parse_hex(hex.substr(0u, 63u)), ErrorCode::invalid_argument);
  DCE_CHECK_CODE(dce::Digest256::parse_hex(hex + "0"), ErrorCode::invalid_argument);
  std::string non_hex = hex;
  non_hex[10] = 'z';
  DCE_CHECK_CODE(dce::Digest256::parse_hex(non_hex), ErrorCode::invalid_argument);
  // A space is a byte that is not a hexadecimal digit, not a separator.
  std::string spaced = hex;
  spaced[10] = ' ';
  DCE_CHECK_CODE(dce::Digest256::parse_hex(spaced), ErrorCode::invalid_argument);
}

DCE_TEST(primitives, error_code_names_and_outcomes) {
  DCE_CHECK_EQ(dce::to_string(ErrorCode::ok), std::string("ok"));
  DCE_CHECK_EQ(dce::to_string(ErrorCode::malformed), std::string("malformed"));
  DCE_CHECK_EQ(dce::to_string(ErrorCode::limit_exceeded), std::string("limit_exceeded"));
  DCE_CHECK_EQ(dce::to_string(ErrorCode::torn_tail), std::string("torn_tail"));

  const dce::Status success;
  DCE_CHECK_TRUE(success.ok());
  DCE_CHECK_EQ(success.code(), ErrorCode::ok);
  DCE_CHECK_TRUE(success.message().empty());

  const dce::Status refused{Error{ErrorCode::refused, "the guard declined"}};
  DCE_CHECK_TRUE(!refused.ok());
  DCE_CHECK_EQ(refused.code(), ErrorCode::refused);
  DCE_CHECK_EQ(refused.message(), std::string("the guard declined"));

  const dce::Result<int> value{7};
  DCE_CHECK_OK(value);
  DCE_CHECK_EQ(value.value(), 7);

  const dce::Result<int> missing{Error{ErrorCode::stale, "older than the authority floor"}};
  DCE_CHECK_CODE(missing, ErrorCode::stale);
  DCE_CHECK_TRUE(!missing.ok());
  DCE_CHECK_EQ(missing.value_or(-1), -1);
}

// ---------------------------------------------------------------------------
// Canonical encoding
// ---------------------------------------------------------------------------
DCE_TEST(primitives, canonical_round_trip_every_primitive) {
  const dce::Digest256 digest = dce::Digest256::of(std::string_view("canonical round trip"));
  const std::vector<std::byte> blob = bytes_of(std::string_view("\x00\x01\xff", 3));

  dce::CanonicalWriter writer;
  DCE_CHECK_OK(writer.put_u8(static_cast<std::uint8_t>(0xfeu)));
  DCE_CHECK_OK(writer.put_bool(true));
  DCE_CHECK_OK(writer.put_bool(false));
  DCE_CHECK_OK(writer.put_u16(static_cast<std::uint16_t>(0xbeefu)));
  DCE_CHECK_OK(writer.put_u32(0xdeadbeefu));
  DCE_CHECK_OK(writer.put_u64(0x0123456789abcdefull));
  DCE_CHECK_OK(writer.put_i64(std::numeric_limits<std::int64_t>::min()));
  DCE_CHECK_OK(writer.put_digest(digest));
  DCE_CHECK_OK(writer.put_string("hello"));
  DCE_CHECK_OK(writer.put_bytes(blob));
  DCE_CHECK_OK(writer.put_count(std::size_t{2}));
  DCE_CHECK_OK(writer.put_u32(7u));
  DCE_CHECK_OK(writer.put_u32(9u));

  dce::CanonicalReader reader(writer.bytes());
  const auto byte = reader.u8();
  DCE_REQUIRE_OK(byte);
  DCE_CHECK_EQ(*byte, static_cast<std::uint8_t>(0xfeu));

  const auto enabled = reader.boolean();
  DCE_REQUIRE_OK(enabled);
  DCE_CHECK_TRUE(*enabled);
  const auto disabled = reader.boolean();
  DCE_REQUIRE_OK(disabled);
  DCE_CHECK_TRUE(!*disabled);

  const auto short_value = reader.u16();
  DCE_REQUIRE_OK(short_value);
  DCE_CHECK_EQ(*short_value, static_cast<std::uint16_t>(0xbeefu));

  const auto word = reader.u32();
  DCE_REQUIRE_OK(word);
  DCE_CHECK_EQ(*word, 0xdeadbeefu);

  const auto wide = reader.u64();
  DCE_REQUIRE_OK(wide);
  DCE_CHECK_EQ(*wide, 0x0123456789abcdefull);

  const auto signed_wide = reader.i64();
  DCE_REQUIRE_OK(signed_wide);
  DCE_CHECK_EQ(*signed_wide, std::numeric_limits<std::int64_t>::min());

  const auto read_digest = reader.digest();
  DCE_REQUIRE_OK(read_digest);
  DCE_CHECK_EQ(read_digest->to_hex(), digest.to_hex());

  const auto text_value = reader.string();
  DCE_REQUIRE_OK(text_value);
  DCE_CHECK_EQ(*text_value, std::string("hello"));

  const auto read_blob = reader.bytes();
  DCE_REQUIRE_OK(read_blob);
  DCE_CHECK_EQ(read_blob->size(), blob.size());
  DCE_CHECK_TRUE(std::equal(read_blob->begin(), read_blob->end(), blob.begin(), blob.end()));

  const auto count = reader.count(4u);
  DCE_REQUIRE_OK(count);
  DCE_CHECK_EQ(*count, std::size_t{2});
  for (std::size_t index = 0; index < *count; ++index) {
    const auto element = reader.u32();
    DCE_REQUIRE_OK(element);
    DCE_CHECK_EQ(*element, index == 0u ? 7u : 9u);
  }

  DCE_CHECK_OK(reader.expect_end());
  DCE_CHECK_TRUE(reader.at_end());
  DCE_CHECK_EQ(reader.offset(), writer.size());

  // The canonical digest of the payload is the digest of the encoded bytes.
  DCE_CHECK_EQ(writer.digest().to_hex(), dce::Digest256::of(std::span<const std::byte>(writer.bytes())).to_hex());
  DCE_CHECK_EQ(writer.to_hex(), writer.digest().to_hex());
}

DCE_TEST(primitives, canonical_layout_is_little_endian) {
  dce::CanonicalWriter writer;
  DCE_CHECK_OK(writer.put_u32(0x01020304u));
  DCE_CHECK_OK(writer.put_u16(static_cast<std::uint16_t>(0x0506u)));
  DCE_CHECK_OK(writer.put_u8(static_cast<std::uint8_t>(0x07u)));
  DCE_CHECK_OK(writer.put_bool(true));
  const std::vector<std::byte>& payload = writer.bytes();
  DCE_REQUIRE(payload.size() == std::size_t{8});
  DCE_CHECK_EQ(byte_at(payload, 0u), 0x04u);
  DCE_CHECK_EQ(byte_at(payload, 1u), 0x03u);
  DCE_CHECK_EQ(byte_at(payload, 2u), 0x02u);
  DCE_CHECK_EQ(byte_at(payload, 3u), 0x01u);
  DCE_CHECK_EQ(byte_at(payload, 4u), 0x06u);
  DCE_CHECK_EQ(byte_at(payload, 5u), 0x05u);
  DCE_CHECK_EQ(byte_at(payload, 6u), 0x07u);
  DCE_CHECK_EQ(byte_at(payload, 7u), 0x01u);

  // A length prefix is a 32-bit little-endian count of the bytes that follow.
  dce::CanonicalWriter text_writer;
  DCE_CHECK_OK(text_writer.put_string("ab"));
  const std::vector<std::byte>& text_payload = text_writer.bytes();
  DCE_REQUIRE(text_payload.size() == std::size_t{6});
  DCE_CHECK_EQ(byte_at(text_payload, 0u), 0x02u);
  DCE_CHECK_EQ(byte_at(text_payload, 1u), 0x00u);
  DCE_CHECK_EQ(byte_at(text_payload, 2u), 0x00u);
  DCE_CHECK_EQ(byte_at(text_payload, 3u), 0x00u);
  DCE_CHECK_EQ(byte_at(text_payload, 4u), static_cast<unsigned>('a'));
  DCE_CHECK_EQ(byte_at(text_payload, 5u), static_cast<unsigned>('b'));
}

DCE_TEST(primitives, canonical_writer_refuses_values_above_its_bounds) {
  // A payload above the writer's own limit is refused, and the refused write
  // leaves the buffer exactly as it was.
  dce::CanonicalWriter bounded(std::size_t{8});
  DCE_CHECK_OK(bounded.put_u64(1ull));
  DCE_CHECK_CODE(bounded.put_u8(static_cast<std::uint8_t>(0u)), ErrorCode::limit_exceeded);
  DCE_CHECK_CODE(bounded.put_string(""), ErrorCode::limit_exceeded);
  DCE_CHECK_EQ(bounded.size(), std::size_t{8});
  DCE_CHECK_EQ(bounded.bytes().size(), std::size_t{8});

  dce::CanonicalWriter writer;
  const std::vector<std::byte> oversized_blob(dce::kMaxBlobBytes + 1u, std::byte{0});
  DCE_CHECK_CODE(writer.put_bytes(oversized_blob), ErrorCode::limit_exceeded);
  DCE_CHECK_EQ(writer.size(), std::size_t{0});

  const std::vector<std::byte> largest_blob(dce::kMaxBlobBytes, std::byte{0});
  DCE_CHECK_OK(writer.put_bytes(largest_blob));
  DCE_CHECK_EQ(writer.size(), dce::kMaxBlobBytes + 4u);

  // An element count that does not fit a 32-bit prefix is refused rather than
  // truncated into a plausible-looking count.
  DCE_CHECK_CODE(writer.put_count(static_cast<std::size_t>(kU32Max) + 1u), ErrorCode::limit_exceeded);
}

DCE_TEST(primitives, canonical_reader_rejects_truncated_payload) {
  dce::CanonicalWriter writer;
  DCE_CHECK_OK(writer.put_u64(0x0102030405060708ull));
  const std::vector<std::byte> encoded = writer.bytes();
  DCE_REQUIRE(encoded.size() == std::size_t{8});

  for (std::size_t length = 0; length < encoded.size(); ++length) {
    dce::CanonicalReader reader(std::span<const std::byte>(encoded).first(length));
    const auto value = reader.u64();
    DCE_CHECK_CODE(value, ErrorCode::malformed);
    // A refused fixed-width read must not consume anything at all.
    DCE_CHECK_EQ(reader.offset(), std::size_t{0});
    DCE_CHECK_EQ(reader.remaining(), length);
  }
}

DCE_TEST(primitives, canonical_reader_rejects_oversized_length_prefix) {
  // The declared length fits the accepted maximum but exceeds what is present.
  dce::CanonicalWriter short_writer;
  DCE_CHECK_OK(short_writer.put_u32(1000u));
  DCE_CHECK_OK(short_writer.put_u8(static_cast<std::uint8_t>(0x41u)));
  dce::CanonicalReader short_reader(short_writer.bytes());
  DCE_CHECK_CODE(short_reader.string(), ErrorCode::malformed);
  DCE_CHECK_EQ(short_reader.offset(), std::size_t{4});
  DCE_CHECK_EQ(short_reader.remaining(), std::size_t{1});
  DCE_CHECK_TRUE(!short_reader.at_end());

  // The declared length is above the accepted maximum, so it is refused before
  // any slice or allocation is formed.
  dce::CanonicalWriter wide_writer;
  DCE_CHECK_OK(wide_writer.put_u32(static_cast<std::uint32_t>(dce::kMaxStringBytes + 1u)));
  dce::CanonicalReader wide_reader(wide_writer.bytes());
  DCE_CHECK_CODE(wide_reader.string(), ErrorCode::limit_exceeded);
  DCE_CHECK_EQ(wide_reader.offset(), std::size_t{4});
  dce::CanonicalReader narrow_reader(wide_writer.bytes());
  DCE_CHECK_CODE(narrow_reader.bytes(16u), ErrorCode::limit_exceeded);
  DCE_CHECK_EQ(narrow_reader.offset(), std::size_t{4});

  // A legal but unsupplied blob length fails without allocating the 1 MiB it
  // claims, because the declared length is checked against what remains.
  dce::CanonicalWriter declared_writer;
  DCE_CHECK_OK(declared_writer.put_u32(static_cast<std::uint32_t>(dce::kMaxBlobBytes)));
  DCE_CHECK_OK(declared_writer.put_u32(0u));
  dce::CanonicalReader declared_reader(declared_writer.bytes());
  DCE_CHECK_CODE(declared_reader.bytes(), ErrorCode::malformed);
  DCE_CHECK_EQ(declared_reader.offset(), std::size_t{4});
  DCE_CHECK_EQ(declared_reader.remaining(), std::size_t{4});
}

DCE_TEST(primitives, canonical_reader_bounds_counts_and_blobs) {
  dce::CanonicalWriter count_writer;
  DCE_CHECK_OK(count_writer.put_u32(65u));
  dce::CanonicalReader over(count_writer.bytes());
  DCE_CHECK_CODE(over.count(64u), ErrorCode::limit_exceeded);

  dce::CanonicalReader exact(count_writer.bytes());
  const auto count = exact.count(65u);
  DCE_REQUIRE_OK(count);
  DCE_CHECK_EQ(*count, std::size_t{65});

  // Zero is a valid count and a valid blob length.
  dce::CanonicalWriter empty_writer;
  DCE_CHECK_OK(empty_writer.put_count(std::size_t{0}));
  DCE_CHECK_OK(empty_writer.put_bytes(std::span<const std::byte>{}));
  DCE_CHECK_EQ(empty_writer.size(), std::size_t{8});
  dce::CanonicalReader empty_reader(empty_writer.bytes());
  const auto empty_count = empty_reader.count(0u);
  DCE_REQUIRE_OK(empty_count);
  DCE_CHECK_EQ(*empty_count, std::size_t{0});
  const auto empty_blob = empty_reader.bytes(0u);
  DCE_REQUIRE_OK(empty_blob);
  DCE_CHECK_EQ(empty_blob->size(), std::size_t{0});
  DCE_CHECK_OK(empty_reader.expect_end());

  // A blob one byte longer than the caller's bound is refused even though the
  // bytes are present.
  dce::CanonicalWriter blob_writer;
  DCE_CHECK_OK(blob_writer.put_string("abcd"));
  dce::CanonicalReader blob_reader(blob_writer.bytes());
  DCE_CHECK_CODE(blob_reader.bytes(3u), ErrorCode::limit_exceeded);
  DCE_CHECK_EQ(blob_reader.offset(), std::size_t{4});
}

DCE_TEST(primitives, canonical_reader_rejects_non_canonical_boolean) {
  dce::CanonicalWriter writer;
  DCE_CHECK_OK(writer.put_u8(static_cast<std::uint8_t>(2u)));
  DCE_CHECK_OK(writer.put_u8(static_cast<std::uint8_t>(0xffu)));
  dce::CanonicalReader reader(writer.bytes());
  DCE_CHECK_CODE(reader.boolean(), ErrorCode::malformed);
  DCE_CHECK_CODE(reader.boolean(), ErrorCode::malformed);

  // The two canonical encodings decode to the two canonical values.
  dce::CanonicalWriter values;
  DCE_CHECK_OK(values.put_bool(false));
  DCE_CHECK_OK(values.put_bool(true));
  DCE_CHECK_EQ(values.size(), std::size_t{2});
  DCE_CHECK_EQ(byte_at(values.bytes(), 0u), 0x00u);
  DCE_CHECK_EQ(byte_at(values.bytes(), 1u), 0x01u);
  dce::CanonicalReader value_reader(values.bytes());
  const auto no = value_reader.boolean();
  DCE_REQUIRE_OK(no);
  DCE_CHECK_TRUE(!*no);
  const auto yes = value_reader.boolean();
  DCE_REQUIRE_OK(yes);
  DCE_CHECK_TRUE(*yes);
  DCE_CHECK_OK(value_reader.expect_end());
}

DCE_TEST(primitives, canonical_reader_rejects_trailing_bytes) {
  dce::CanonicalWriter writer;
  DCE_CHECK_OK(writer.put_u32(7u));
  DCE_CHECK_OK(writer.put_u8(static_cast<std::uint8_t>(9u)));
  dce::CanonicalReader reader(writer.bytes());
  const auto value = reader.u32();
  DCE_REQUIRE_OK(value);
  DCE_CHECK_EQ(*value, 7u);
  DCE_CHECK_CODE(reader.expect_end(), ErrorCode::malformed);
  DCE_CHECK_TRUE(!reader.at_end());
  DCE_CHECK_EQ(reader.remaining(), std::size_t{1});

  // Consuming the trailing byte makes the same payload canonical again.
  const auto trailing = reader.u8();
  DCE_REQUIRE_OK(trailing);
  DCE_CHECK_EQ(*trailing, static_cast<std::uint8_t>(9u));
  DCE_CHECK_OK(reader.expect_end());
  DCE_CHECK_TRUE(reader.at_end());

  // An empty payload is not a canonical encoding of anything, but it does
  // decode as an empty value: the reader reports end, it does not underflow.
  dce::CanonicalReader empty(std::span<const std::byte>{});
  DCE_CHECK_TRUE(empty.at_end());
  DCE_CHECK_EQ(empty.remaining(), std::size_t{0});
  DCE_CHECK_OK(empty.expect_end());
  DCE_CHECK_CODE(empty.u8(), ErrorCode::malformed);
  DCE_CHECK_CODE(empty.u32(), ErrorCode::malformed);
  DCE_CHECK_CODE(empty.u64(), ErrorCode::malformed);
  DCE_CHECK_CODE(empty.i64(), ErrorCode::malformed);
  DCE_CHECK_CODE(empty.digest(), ErrorCode::malformed);
  DCE_CHECK_CODE(empty.boolean(), ErrorCode::malformed);
  DCE_CHECK_CODE(empty.string(), ErrorCode::malformed);
}
