// Content digests and integrity checks.
//
// SHA-256 and CRC-32 are implemented in-tree; the runtime has no third-party
// dependencies. Both are used for integrity and identity of persisted and
// hashed authoritative state, never as a substitute for evidence.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

#include "dce/status.hpp"

namespace dce {

inline constexpr std::size_t kDigestBytes = 32;

class Sha256 {
 public:
  Sha256() noexcept;

  void update(std::span<const std::byte> data) noexcept;
  void update(std::string_view data) noexcept;
  void update(std::uint8_t byte) noexcept;

  // Finalizes the digest. The object must not be updated afterwards.
  [[nodiscard]] std::array<std::byte, kDigestBytes> finish() noexcept;

  [[nodiscard]] static std::array<std::byte, kDigestBytes> of(std::span<const std::byte> data) noexcept;
  [[nodiscard]] static std::array<std::byte, kDigestBytes> of(std::string_view data) noexcept;

 private:
  void compress(const std::uint8_t* block) noexcept;

  std::array<std::uint32_t, 8> state_{};
  std::array<std::uint8_t, 64> buffer_{};
  std::size_t buffered_{0};
  std::uint64_t total_bytes_{0};
  bool finalized_{false};
};

struct Digest256 {
  std::array<std::byte, kDigestBytes> bytes{};

  [[nodiscard]] static Digest256 of(std::span<const std::byte> data) noexcept;
  [[nodiscard]] static Digest256 of(std::string_view data) noexcept;
  [[nodiscard]] static Result<Digest256> parse_hex(std::string_view text);
  [[nodiscard]] static Digest256 zero() noexcept { return Digest256{}; }

  [[nodiscard]] bool is_zero() const noexcept;
  [[nodiscard]] std::string to_hex() const;
  [[nodiscard]] std::span<const std::byte> span() const noexcept { return bytes; }

  auto operator<=>(const Digest256&) const = default;
};

// CRC-32 (IEEE 802.3, reflected, polynomial 0xEDB88320) over the given bytes.
[[nodiscard]] std::uint32_t crc32(std::span<const std::byte> data) noexcept;
[[nodiscard]] std::uint32_t crc32(std::string_view data) noexcept;

}  // namespace dce
