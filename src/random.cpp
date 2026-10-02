#include "dce/random.hpp"

namespace dce {

std::uint64_t DeterministicRng::next_u64() noexcept {
  // splitmix64: fully specified, no hidden state, and identical on every
  // platform because it uses only fixed-width unsigned arithmetic.
  state_ += 0x9e3779b97f4a7c15ull;
  std::uint64_t value = state_;
  value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ull;
  value = (value ^ (value >> 27)) * 0x94d049bb133111ebull;
  ++draws_;
  return value ^ (value >> 31);
}

std::uint32_t DeterministicRng::next_u32() noexcept {
  return static_cast<std::uint32_t>(next_u64() >> 32);
}

std::uint64_t DeterministicRng::uniform_below(std::uint64_t bound) noexcept {
  if (bound == 0) {
    return 0;
  }
  // Rejection sampling keeps the distribution uniform; the modulo bias is
  // removed rather than accepted.
  const std::uint64_t threshold = (0ull - bound) % bound;
  while (true) {
    const std::uint64_t candidate = next_u64();
    if (candidate >= threshold) {
      return candidate % bound;
    }
  }
}

bool DeterministicRng::chance(std::uint32_t numerator, std::uint32_t denominator) noexcept {
  if (denominator == 0) {
    return false;
  }
  if (numerator >= denominator) {
    return true;
  }
  return uniform_below(denominator) < numerator;
}

}  // namespace dce
