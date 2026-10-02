// Canonical serialization.
//
// Everything that is hashed, persisted or put on the wire is written through
// these writers. The encoding is fixed-width little-endian with explicit
// 32-bit length prefixes, so the same logical value always produces the same
// bytes and the same digest regardless of host, container or insertion order.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "dce/digest.hpp"
#include "dce/status.hpp"

namespace dce {

// Documented bounds. Untrusted input is validated against these before any
// allocation happens, so a hostile length prefix cannot become a huge reserve.
inline constexpr std::size_t kMaxCanonicalBytes = 8u * 1024u * 1024u;
inline constexpr std::size_t kMaxStringBytes = 4096;
inline constexpr std::size_t kMaxBlobBytes = 1u * 1024u * 1024u;

class CanonicalWriter {
 public:
  explicit CanonicalWriter(std::size_t limit = kMaxCanonicalBytes) : limit_(limit) {}

  [[nodiscard]] Status put_u8(std::uint8_t value);
  [[nodiscard]] Status put_bool(bool value);
  [[nodiscard]] Status put_u16(std::uint16_t value);
  [[nodiscard]] Status put_u32(std::uint32_t value);
  [[nodiscard]] Status put_u64(std::uint64_t value);
  [[nodiscard]] Status put_i64(std::int64_t value);

  // Length-prefixed opaque bytes, bounded by kMaxBlobBytes.
  [[nodiscard]] Status put_bytes(std::span<const std::byte> value);

  // Length-prefixed UTF-8 text, bounded by kMaxStringBytes.
  [[nodiscard]] Status put_string(std::string_view value);

  [[nodiscard]] Status put_digest(const Digest256& value);

  // Writes an element count that the reader will bound-check before allocating.
  [[nodiscard]] Status put_count(std::size_t count);

  [[nodiscard]] const std::vector<std::byte>& bytes() const noexcept { return bytes_; }
  [[nodiscard]] std::size_t size() const noexcept { return bytes_.size(); }

  [[nodiscard]] Digest256 digest() const noexcept;
  [[nodiscard]] std::string to_hex() const;

 private:
  [[nodiscard]] Status reserve(std::size_t additional);

  std::vector<std::byte> bytes_;
  std::size_t limit_;
};

class CanonicalReader {
 public:
  explicit CanonicalReader(std::span<const std::byte> data) noexcept : data_(data) {}

  [[nodiscard]] Result<std::uint8_t> u8();
  [[nodiscard]] Result<bool> boolean();
  [[nodiscard]] Result<std::uint16_t> u16();
  [[nodiscard]] Result<std::uint32_t> u32();
  [[nodiscard]] Result<std::uint64_t> u64();
  [[nodiscard]] Result<std::int64_t> i64();

  // Reads a length-prefixed blob, refusing lengths above max_length or beyond
  // the remaining bytes before allocating anything.
  [[nodiscard]] Result<std::span<const std::byte>> bytes(std::size_t max_length = kMaxBlobBytes);

  [[nodiscard]] Result<std::string> string(std::size_t max_length = kMaxStringBytes);

  [[nodiscard]] Result<Digest256> digest();

  // Reads an element count and refuses anything above max_allowed.
  [[nodiscard]] Result<std::size_t> count(std::size_t max_allowed);

  [[nodiscard]] std::size_t remaining() const noexcept { return data_.size() - offset_; }
  [[nodiscard]] std::size_t offset() const noexcept { return offset_; }
  [[nodiscard]] bool at_end() const noexcept { return offset_ == data_.size(); }

  // Rejects trailing bytes: a canonical payload is consumed exactly.
  [[nodiscard]] Status expect_end() const;

 private:
  [[nodiscard]] Result<std::span<const std::byte>> take(std::size_t length);

  std::span<const std::byte> data_;
  std::size_t offset_{0};
};

}  // namespace dce
