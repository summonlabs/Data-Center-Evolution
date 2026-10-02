#include "dce/canonical.hpp"

#include <cstring>

#include "dce/checked.hpp"

namespace dce {
namespace {

void append_little_endian(std::vector<std::byte>& out, std::uint64_t value, std::size_t width) {
  for (std::size_t index = 0; index < width; ++index) {
    out.push_back(static_cast<std::byte>((value >> (index * 8)) & 0xffu));
  }
}

[[nodiscard]] std::uint64_t read_little_endian(std::span<const std::byte> data) noexcept {
  std::uint64_t value = 0;
  for (std::size_t index = 0; index < data.size(); ++index) {
    value |= static_cast<std::uint64_t>(static_cast<unsigned>(data[index])) << (index * 8);
  }
  return value;
}

constexpr std::size_t kU64Width = 8;

}  // namespace

Status CanonicalWriter::reserve(std::size_t additional) {
  const std::optional<std::size_t> next = checked_add<std::size_t>(bytes_.size(), additional);
  if (!next.has_value()) {
    return Error{ErrorCode::overflow, "canonical buffer size overflowed"};
  }
  if (*next > limit_) {
    return Error{ErrorCode::limit_exceeded, "canonical payload exceeds the configured bound"};
  }
  bytes_.reserve(*next);
  return Status{};
}

Status CanonicalWriter::put_u8(std::uint8_t value) {
  DCE_TRY(reserve(1));
  bytes_.push_back(static_cast<std::byte>(value));
  return Status{};
}

Status CanonicalWriter::put_bool(bool value) { return put_u8(value ? 1u : 0u); }

Status CanonicalWriter::put_u16(std::uint16_t value) {
  DCE_TRY(reserve(2));
  append_little_endian(bytes_, value, 2);
  return Status{};
}

Status CanonicalWriter::put_u32(std::uint32_t value) {
  DCE_TRY(reserve(4));
  append_little_endian(bytes_, value, 4);
  return Status{};
}

Status CanonicalWriter::put_u64(std::uint64_t value) {
  DCE_TRY(reserve(kU64Width));
  append_little_endian(bytes_, value, kU64Width);
  return Status{};
}

Status CanonicalWriter::put_i64(std::int64_t value) {
  std::uint64_t bits = 0;
  std::memcpy(&bits, &value, sizeof(bits));
  return put_u64(bits);
}

Status CanonicalWriter::put_bytes(std::span<const std::byte> value) {
  if (value.size() > kMaxBlobBytes) {
    return Error{ErrorCode::limit_exceeded, "canonical blob exceeds the documented maximum"};
  }
  const std::optional<std::uint32_t> length = checked_narrow<std::uint32_t>(value.size());
  if (!length.has_value()) {
    return Error{ErrorCode::overflow, "canonical blob length does not fit in 32 bits"};
  }
  DCE_TRY(put_u32(*length));
  DCE_TRY(reserve(value.size()));
  bytes_.insert(bytes_.end(), value.begin(), value.end());
  return Status{};
}

Status CanonicalWriter::put_string(std::string_view value) {
  return put_bytes(std::span<const std::byte>(reinterpret_cast<const std::byte*>(value.data()), value.size()));
}

Status CanonicalWriter::put_digest(const Digest256& value) {
  DCE_TRY(reserve(value.bytes.size()));
  bytes_.insert(bytes_.end(), value.bytes.begin(), value.bytes.end());
  return Status{};
}

Status CanonicalWriter::put_count(std::size_t count) {
  const std::optional<std::uint32_t> narrowed = checked_narrow<std::uint32_t>(count);
  if (!narrowed.has_value()) {
    return Error{ErrorCode::limit_exceeded, "canonical element count does not fit in 32 bits"};
  }
  return put_u32(*narrowed);
}

Digest256 CanonicalWriter::digest() const noexcept { return Digest256::of(bytes_); }

std::string CanonicalWriter::to_hex() const { return digest().to_hex(); }

Result<std::span<const std::byte>> CanonicalReader::take(std::size_t length) {
  const std::optional<std::size_t> end = checked_add<std::size_t>(offset_, length);
  if (!end.has_value() || *end > data_.size()) {
    return Error{ErrorCode::malformed, "canonical payload ended before the declared length"};
  }
  const std::span<const std::byte> slice = data_.subspan(offset_, length);
  offset_ = *end;
  return slice;
}

Result<std::uint8_t> CanonicalReader::u8() {
  Result<std::span<const std::byte>> raw = take(1);
  if (!raw.ok()) {
    return raw.error();
  }
  return static_cast<std::uint8_t>(static_cast<unsigned>((*raw)[0]));
}

Result<bool> CanonicalReader::boolean() {
  Result<std::uint8_t> raw = u8();
  if (!raw.ok()) {
    return raw.error();
  }
  if (*raw > 1u) {
    return Error{ErrorCode::malformed, "canonical boolean must be 0 or 1"};
  }
  return *raw == 1u;
}

Result<std::uint16_t> CanonicalReader::u16() {
  Result<std::span<const std::byte>> raw = take(2);
  if (!raw.ok()) {
    return raw.error();
  }
  return static_cast<std::uint16_t>(read_little_endian(*raw));
}

Result<std::uint32_t> CanonicalReader::u32() {
  Result<std::span<const std::byte>> raw = take(4);
  if (!raw.ok()) {
    return raw.error();
  }
  return static_cast<std::uint32_t>(read_little_endian(*raw));
}

Result<std::uint64_t> CanonicalReader::u64() {
  Result<std::span<const std::byte>> raw = take(kU64Width);
  if (!raw.ok()) {
    return raw.error();
  }
  return read_little_endian(*raw);
}

Result<std::int64_t> CanonicalReader::i64() {
  Result<std::uint64_t> raw = u64();
  if (!raw.ok()) {
    return raw.error();
  }
  std::int64_t value = 0;
  const std::uint64_t bits = *raw;
  std::memcpy(&value, &bits, sizeof(value));
  return value;
}

Result<std::span<const std::byte>> CanonicalReader::bytes(std::size_t max_length) {
  Result<std::uint32_t> declared = u32();
  if (!declared.ok()) {
    return declared.error();
  }
  const std::size_t length = *declared;
  // Both bounds are checked before the slice is formed, so a hostile length
  // prefix can never cause a read past the end or a large allocation.
  if (length > max_length) {
    return Error{ErrorCode::limit_exceeded, "declared length exceeds the accepted maximum"};
  }
  return take(length);
}

Result<std::string> CanonicalReader::string(std::size_t max_length) {
  Result<std::span<const std::byte>> raw = bytes(max_length);
  if (!raw.ok()) {
    return raw.error();
  }
  return std::string(reinterpret_cast<const char*>(raw->data()), raw->size());
}

Result<Digest256> CanonicalReader::digest() {
  Result<std::span<const std::byte>> raw = take(kDigestBytes);
  if (!raw.ok()) {
    return raw.error();
  }
  Digest256 out;
  std::memcpy(out.bytes.data(), raw->data(), kDigestBytes);
  return out;
}

Result<std::size_t> CanonicalReader::count(std::size_t max_allowed) {
  Result<std::uint32_t> declared = u32();
  if (!declared.ok()) {
    return declared.error();
  }
  if (*declared > max_allowed) {
    return Error{ErrorCode::limit_exceeded, "declared element count exceeds the accepted maximum"};
  }
  return static_cast<std::size_t>(*declared);
}

Status CanonicalReader::expect_end() const {
  if (!at_end()) {
    return Error{ErrorCode::malformed, "canonical payload has trailing bytes"};
  }
  return Status{};
}

}  // namespace dce
