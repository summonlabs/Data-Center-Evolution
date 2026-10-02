#include "dce/text.hpp"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <array>
#include <cstdio>

#include "dce/checked.hpp"

namespace dce::text {
namespace {

// Windows resolves these names to devices regardless of extension, so they are
// never accepted as identities that may become path components.
constexpr std::array<std::string_view, 22> kReservedDeviceNames = {
    "con",  "prn",  "aux",  "nul",  "com1", "com2", "com3", "com4", "com5",
    "com6", "com7", "com8", "com9", "lpt1", "lpt2", "lpt3", "lpt4", "lpt5",
    "lpt6", "lpt7", "lpt8", "lpt9"};

constexpr char kHexDigits[] = "0123456789abcdef";

}  // namespace

bool is_ascii_alnum(char c) noexcept {
  return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

bool iequals_ascii(std::string_view a, std::string_view b) noexcept {
  if (a.size() != b.size()) {
    return false;
  }
  for (std::size_t i = 0; i < a.size(); ++i) {
    char lhs = a[i];
    char rhs = b[i];
    if (lhs >= 'A' && lhs <= 'Z') {
      lhs = static_cast<char>(lhs - 'A' + 'a');
    }
    if (rhs >= 'A' && rhs <= 'Z') {
      rhs = static_cast<char>(rhs - 'A' + 'a');
    }
    if (lhs != rhs) {
      return false;
    }
  }
  return true;
}

bool valid_identifier(std::string_view value) noexcept {
  if (value.empty() || value.size() > kMaxIdentifierLength) {
    return false;
  }
  if (!is_ascii_alnum(value.front()) || !is_ascii_alnum(value.back())) {
    return false;
  }
  for (char c : value) {
    // ':' is deliberately excluded: on Windows it separates a file name from
    // an alternate data stream, and these identities become path components.
    const bool accepted = is_ascii_alnum(c) || c == '.' || c == '_' || c == '@' || c == '-';
    if (!accepted) {
      return false;
    }
  }
  if (value.find("..") != std::string_view::npos) {
    return false;
  }
  const std::size_t dot = value.find('.');
  const std::string_view stem = dot == std::string_view::npos ? value : value.substr(0, dot);
  for (std::string_view reserved : kReservedDeviceNames) {
    if (iequals_ascii(stem, reserved)) {
      return false;
    }
  }
  return true;
}

std::string_view trim(std::string_view value) noexcept {
  std::size_t begin = 0;
  std::size_t end = value.size();
  const auto is_space = [](char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v';
  };
  while (begin < end && is_space(value[begin])) {
    ++begin;
  }
  while (end > begin && is_space(value[end - 1])) {
    --end;
  }
  return value.substr(begin, end - begin);
}

bool is_blank(std::string_view value) noexcept { return trim(value).empty(); }

bool is_printable_ascii(std::string_view value) noexcept {
  for (char c : value) {
    const auto byte = static_cast<unsigned char>(c);
    if (byte < 0x20 || byte > 0x7e) {
      return false;
    }
  }
  return true;
}

std::vector<std::string_view> split(std::string_view value, char delimiter) {
  std::vector<std::string_view> parts;
  std::size_t start = 0;
  while (true) {
    const std::size_t next = value.find(delimiter, start);
    if (next == std::string_view::npos) {
      parts.push_back(value.substr(start));
      break;
    }
    parts.push_back(value.substr(start, next - start));
    start = next + 1;
  }
  // A well formed list never produces an unbounded number of fields, but the
  // caller owns the bound; split() itself does not invent one.
  return parts;
}

std::string join(const std::vector<std::string>& parts, std::string_view separator) {
  std::string out;
  for (std::size_t i = 0; i < parts.size(); ++i) {
    if (i != 0) {
      out.append(separator);
    }
    out.append(parts[i]);
  }
  return out;
}

std::string to_lower_ascii(std::string_view value) {
  std::string out;
  out.reserve(value.size());
  for (char c : value) {
    out.push_back((c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c);
  }
  return out;
}

std::string escape_for_output(std::string_view value) {
  const bool truncated = value.size() > kMaxOutputTextLength;
  const std::string_view shown = truncated ? value.substr(0, kMaxOutputTextLength) : value;
  std::string out;
  out.reserve(shown.size() + 8);
  for (char c : shown) {
    const auto byte = static_cast<unsigned char>(c);
    if (byte >= 0x20 && byte <= 0x7e && c != '\\') {
      out.push_back(c);
    } else {
      out.push_back('\\');
      out.push_back('x');
      out.push_back(kHexDigits[(byte >> 4) & 0x0f]);
      out.push_back(kHexDigits[byte & 0x0f]);
    }
  }
  if (truncated) {
    out.append("...");
  }
  return out;
}

Result<std::uint64_t> parse_u64(std::string_view value) {
  if (value.empty()) {
    return Error{ErrorCode::invalid_argument, "empty numeric field"};
  }
  if (value.size() > 20) {
    return Error{ErrorCode::out_of_range, "numeric field longer than 20 digits"};
  }
  std::uint64_t accumulator = 0;
  for (char c : value) {
    if (c < '0' || c > '9') {
      return Error{ErrorCode::invalid_argument, "numeric field contains a non-digit"};
    }
    const std::uint64_t digit = static_cast<std::uint64_t>(c - '0');
    const std::optional<std::uint64_t> scaled = checked_mul<std::uint64_t>(accumulator, 10u);
    if (!scaled.has_value()) {
      return Error{ErrorCode::overflow, "numeric field overflows a 64-bit counter"};
    }
    const std::optional<std::uint64_t> advanced = checked_add<std::uint64_t>(*scaled, digit);
    if (!advanced.has_value()) {
      return Error{ErrorCode::overflow, "numeric field overflows a 64-bit counter"};
    }
    accumulator = *advanced;
  }
  return accumulator;
}

Result<std::uint32_t> parse_u32(std::string_view value) {
  Result<std::uint64_t> wide = parse_u64(value);
  if (!wide.ok()) {
    return wide.error();
  }
  const std::optional<std::uint32_t> narrowed = checked_narrow<std::uint32_t>(*wide);
  if (!narrowed.has_value()) {
    return Error{ErrorCode::out_of_range, "numeric field does not fit in 32 bits"};
  }
  return *narrowed;
}

std::string u64_to_string(std::uint64_t value) {
  if (value == 0) {
    return "0";
  }
  char buffer[20];
  std::size_t index = 0;
  while (value != 0) {
    buffer[index++] = static_cast<char>('0' + (value % 10));
    value /= 10;
  }
  std::string out;
  out.reserve(index);
  while (index > 0) {
    out.push_back(buffer[--index]);
  }
  return out;
}

std::string u32_to_string(std::uint32_t value) { return u64_to_string(static_cast<std::uint64_t>(value)); }

}  // namespace dce::text
