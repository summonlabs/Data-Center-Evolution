#include "dce/digest.hpp"

#include <algorithm>
#include <cstring>

namespace dce {
namespace {

constexpr std::array<std::uint32_t, 64> kRoundConstants = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u,
    0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu,
    0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu,
    0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu,
    0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
    0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u,
    0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u,
    0xc67178f2u};

constexpr std::array<std::uint32_t, 8> kInitialState = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u,
                                                        0xa54ff53au, 0x510e527fu, 0x9b05688cu,
                                                        0x1f83d9abu, 0x5be0cd19u};

[[nodiscard]] std::uint32_t rotr(std::uint32_t value, std::uint32_t count) noexcept {
  return (value >> count) | (value << (32u - count));
}

struct Crc32Table {
  std::array<std::uint32_t, 256> values{};

  constexpr Crc32Table() noexcept {
    for (std::uint32_t index = 0; index < 256u; ++index) {
      std::uint32_t accumulator = index;
      for (int bit = 0; bit < 8; ++bit) {
        accumulator = (accumulator & 1u) != 0u ? (0xedb88320u ^ (accumulator >> 1)) : (accumulator >> 1);
      }
      values[index] = accumulator;
    }
  }
};

constexpr Crc32Table kCrc32Table{};

constexpr char kHexDigits[] = "0123456789abcdef";

[[nodiscard]] int hex_value(char c) noexcept {
  if (c >= '0' && c <= '9') {
    return c - '0';
  }
  if (c >= 'a' && c <= 'f') {
    return c - 'a' + 10;
  }
  if (c >= 'A' && c <= 'F') {
    return c - 'A' + 10;
  }
  return -1;
}

}  // namespace

Sha256::Sha256() noexcept : state_(kInitialState) {}

void Sha256::compress(const std::uint8_t* block) noexcept {
  std::array<std::uint32_t, 64> schedule{};
  for (std::size_t index = 0; index < 16; ++index) {
    const std::size_t base = index * 4;
    schedule[index] = (static_cast<std::uint32_t>(block[base]) << 24) |
                      (static_cast<std::uint32_t>(block[base + 1]) << 16) |
                      (static_cast<std::uint32_t>(block[base + 2]) << 8) |
                      static_cast<std::uint32_t>(block[base + 3]);
  }
  for (std::size_t index = 16; index < 64; ++index) {
    const std::uint32_t s0 = rotr(schedule[index - 15], 7) ^ rotr(schedule[index - 15], 18) ^
                             (schedule[index - 15] >> 3);
    const std::uint32_t s1 = rotr(schedule[index - 2], 17) ^ rotr(schedule[index - 2], 19) ^
                             (schedule[index - 2] >> 10);
    schedule[index] = schedule[index - 16] + s0 + schedule[index - 7] + s1;
  }

  std::uint32_t a = state_[0];
  std::uint32_t b = state_[1];
  std::uint32_t c = state_[2];
  std::uint32_t d = state_[3];
  std::uint32_t e = state_[4];
  std::uint32_t f = state_[5];
  std::uint32_t g = state_[6];
  std::uint32_t h = state_[7];

  for (std::size_t index = 0; index < 64; ++index) {
    const std::uint32_t sigma1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
    const std::uint32_t choice = (e & f) ^ (~e & g);
    const std::uint32_t temp1 = h + sigma1 + choice + kRoundConstants[index] + schedule[index];
    const std::uint32_t sigma0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
    const std::uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
    const std::uint32_t temp2 = sigma0 + majority;

    h = g;
    g = f;
    f = e;
    e = d + temp1;
    d = c;
    c = b;
    b = a;
    a = temp1 + temp2;
  }

  state_[0] += a;
  state_[1] += b;
  state_[2] += c;
  state_[3] += d;
  state_[4] += e;
  state_[5] += f;
  state_[6] += g;
  state_[7] += h;
}

void Sha256::update(std::span<const std::byte> data) noexcept {
  if (finalized_ || data.empty()) {
    return;
  }
  const auto* cursor = reinterpret_cast<const std::uint8_t*>(data.data());
  std::size_t remaining = data.size();
  total_bytes_ += static_cast<std::uint64_t>(remaining);
  while (remaining > 0) {
    const std::size_t space = buffer_.size() - buffered_;
    const std::size_t chunk = remaining < space ? remaining : space;
    std::memcpy(buffer_.data() + buffered_, cursor, chunk);
    buffered_ += chunk;
    cursor += chunk;
    remaining -= chunk;
    if (buffered_ == buffer_.size()) {
      compress(buffer_.data());
      buffered_ = 0;
    }
  }
}

void Sha256::update(std::string_view data) noexcept {
  update(std::span<const std::byte>(reinterpret_cast<const std::byte*>(data.data()), data.size()));
}

void Sha256::update(std::uint8_t byte) noexcept {
  const std::byte raw = static_cast<std::byte>(byte);
  update(std::span<const std::byte>(&raw, 1));
}

std::array<std::byte, kDigestBytes> Sha256::finish() noexcept {
  if (!finalized_) {
    const std::uint64_t bit_length = total_bytes_ * 8u;
    update(static_cast<std::uint8_t>(0x80));
    while (buffered_ != 56) {
      update(static_cast<std::uint8_t>(0x00));
    }
    for (int shift = 56; shift >= 0; shift -= 8) {
      update(static_cast<std::uint8_t>((bit_length >> static_cast<unsigned>(shift)) & 0xffu));
    }
    finalized_ = true;
  }

  std::array<std::byte, kDigestBytes> out{};
  for (std::size_t index = 0; index < state_.size(); ++index) {
    const std::size_t base = index * 4;
    out[base] = static_cast<std::byte>((state_[index] >> 24) & 0xffu);
    out[base + 1] = static_cast<std::byte>((state_[index] >> 16) & 0xffu);
    out[base + 2] = static_cast<std::byte>((state_[index] >> 8) & 0xffu);
    out[base + 3] = static_cast<std::byte>(state_[index] & 0xffu);
  }
  return out;
}

std::array<std::byte, kDigestBytes> Sha256::of(std::span<const std::byte> data) noexcept {
  Sha256 hasher;
  hasher.update(data);
  return hasher.finish();
}

std::array<std::byte, kDigestBytes> Sha256::of(std::string_view data) noexcept {
  Sha256 hasher;
  hasher.update(data);
  return hasher.finish();
}

Digest256 Digest256::of(std::span<const std::byte> data) noexcept {
  Digest256 out;
  out.bytes = Sha256::of(data);
  return out;
}

Digest256 Digest256::of(std::string_view data) noexcept {
  Digest256 out;
  out.bytes = Sha256::of(data);
  return out;
}

Result<Digest256> Digest256::parse_hex(std::string_view text) {
  if (text.size() != kDigestBytes * 2) {
    return Error{ErrorCode::invalid_argument, "digest must be exactly 64 hexadecimal characters"};
  }
  Digest256 out;
  for (std::size_t index = 0; index < kDigestBytes; ++index) {
    const int high = hex_value(text[index * 2]);
    const int low = hex_value(text[index * 2 + 1]);
    if (high < 0 || low < 0) {
      return Error{ErrorCode::invalid_argument, "digest contains a non-hexadecimal character"};
    }
    out.bytes[index] = static_cast<std::byte>((high << 4) | low);
  }
  return out;
}

bool Digest256::is_zero() const noexcept {
  return std::all_of(bytes.begin(), bytes.end(), [](std::byte b) { return b == std::byte{0}; });
}

std::string Digest256::to_hex() const {
  std::string out;
  out.reserve(kDigestBytes * 2);
  for (std::byte b : bytes) {
    const auto value = static_cast<unsigned>(b);
    out.push_back(kHexDigits[(value >> 4) & 0x0f]);
    out.push_back(kHexDigits[value & 0x0f]);
  }
  return out;
}

std::uint32_t crc32(std::span<const std::byte> data) noexcept {
  std::uint32_t crc = 0xffffffffu;
  for (std::byte b : data) {
    crc = kCrc32Table.values[(crc ^ static_cast<std::uint32_t>(b)) & 0xffu] ^ (crc >> 8);
  }
  return crc ^ 0xffffffffu;
}

std::uint32_t crc32(std::string_view data) noexcept {
  return crc32(std::span<const std::byte>(reinterpret_cast<const std::byte*>(data.data()), data.size()));
}

}  // namespace dce
