#include "dce/version.hpp"

#include <vector>

#include "dce/text.hpp"

namespace dce {

Result<Version> Version::parse(std::string_view value) {
  const std::vector<std::string_view> fields = text::split(value, '.');
  if (fields.size() != 3) {
    return Error{ErrorCode::malformed, "version must have exactly three numeric fields: " +
                                           text::escape_for_output(value)};
  }
  Version out;
  std::uint32_t* targets[3] = {&out.major, &out.minor, &out.patch};
  for (std::size_t index = 0; index < 3; ++index) {
    if (fields[index].empty()) {
      return Error{ErrorCode::malformed, "version field is empty: " + text::escape_for_output(value)};
    }
    Result<std::uint32_t> field = text::parse_u32(fields[index]);
    if (!field.ok()) {
      return Error{field.code(), "version field rejected: " + text::escape_for_output(fields[index])};
    }
    *targets[index] = *field;
  }
  return out;
}

std::string Version::to_string() const {
  return text::u32_to_string(major) + "." + text::u32_to_string(minor) + "." + text::u32_to_string(patch);
}

Result<VersionRange> VersionRange::parse(std::string_view value) {
  const std::size_t separator = value.find("..");
  if (separator == std::string_view::npos) {
    return Error{ErrorCode::malformed, "version range must use the low..high form"};
  }
  const std::string_view low_text = text::trim(value.substr(0, separator));
  const std::string_view high_text = text::trim(value.substr(separator + 2));

  VersionRange out;
  Result<Version> low = Version::parse(low_text);
  if (!low.ok()) {
    return low.error();
  }
  out.low = *low;

  if (!high_text.empty()) {
    Result<Version> high = Version::parse(high_text);
    if (!high.ok()) {
      return high.error();
    }
    if (*high < out.low) {
      return Error{ErrorCode::invalid_argument, "version range upper bound precedes its lower bound"};
    }
    out.high = *high;
  }
  return out;
}

bool VersionRange::contains(const Version& version) const {
  if (version < low) {
    return false;
  }
  if (high.has_value() && *high < version) {
    return false;
  }
  return true;
}

std::string VersionRange::to_string() const {
  return low.to_string() + ".." + (high.has_value() ? high->to_string() : std::string());
}

}  // namespace dce
