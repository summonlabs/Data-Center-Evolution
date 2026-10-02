// Strict version parsing. Compatibility is never inferred from these numbers
// alone; they are inputs to explicit compatibility evidence, not a substitute
// for it.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "dce/status.hpp"

namespace dce {

struct Version {
  std::uint32_t major{0};
  std::uint32_t minor{0};
  std::uint32_t patch{0};

  [[nodiscard]] static Result<Version> parse(std::string_view value);
  [[nodiscard]] std::string to_string() const;
  [[nodiscard]] bool is_zero() const noexcept { return major == 0 && minor == 0 && patch == 0; }

  auto operator<=>(const Version&) const = default;
};

// An inclusive version interval. An absent upper bound means "and above".
struct VersionRange {
  Version low{};
  std::optional<Version> high{};

  [[nodiscard]] static Result<VersionRange> parse(std::string_view value);
  [[nodiscard]] bool contains(const Version& version) const;
  [[nodiscard]] std::string to_string() const;

  auto operator<=>(const VersionRange&) const = default;
};

}  // namespace dce
