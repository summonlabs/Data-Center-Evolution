// Small, dependency-free text helpers used for parsing, validation and output.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "dce/status.hpp"

namespace dce::text {

// Identifiers are the stable identities of authoritative objects, and they are
// also used as on-disk file and directory names. The accepted alphabet is
// therefore deliberately narrow: no separators that a path resolver would
// honour, no traversal sequences, and no Windows reserved device names.
inline constexpr std::size_t kMaxIdentifierLength = 96;

// A single line of untrusted external text that we are willing to echo back.
inline constexpr std::size_t kMaxOutputTextLength = 4096;

[[nodiscard]] bool is_ascii_alnum(char c) noexcept;
[[nodiscard]] bool valid_identifier(std::string_view value) noexcept;
[[nodiscard]] std::string_view trim(std::string_view value) noexcept;
[[nodiscard]] bool is_blank(std::string_view value) noexcept;
[[nodiscard]] bool is_printable_ascii(std::string_view value) noexcept;
[[nodiscard]] std::vector<std::string_view> split(std::string_view value, char delimiter);
[[nodiscard]] std::string join(const std::vector<std::string>& parts, std::string_view separator);
[[nodiscard]] std::string to_lower_ascii(std::string_view value);
[[nodiscard]] bool iequals_ascii(std::string_view a, std::string_view b) noexcept;

// Renders untrusted text for terminal output: printable ASCII is passed
// through, everything else becomes a \xNN escape so no control sequence can
// reach a terminal.
[[nodiscard]] std::string escape_for_output(std::string_view value);

[[nodiscard]] Result<std::uint64_t> parse_u64(std::string_view value);
[[nodiscard]] Result<std::uint32_t> parse_u32(std::string_view value);
[[nodiscard]] std::string u64_to_string(std::uint64_t value);
[[nodiscard]] std::string u32_to_string(std::uint32_t value);

}  // namespace dce::text
