// A small, reproducible pseudo-random generator.
//
// Seeded randomized testing is only useful when a failure can be replayed, so
// this generator is fully specified, has no hidden state beyond its seed, and
// is the only source of randomness in the runtime. Production code paths do
// not consult it: rollout decisions are functions of the plan and the observed
// state alone.
#pragma once

#include <cstdint>

namespace dce {

class DeterministicRng {
 public:
  explicit DeterministicRng(std::uint64_t seed) noexcept : state_(seed) {}

  [[nodiscard]] std::uint64_t next_u64() noexcept;
  [[nodiscard]] std::uint32_t next_u32() noexcept;

  // Uniform over [0, bound); bound must be non-zero.
  [[nodiscard]] std::uint64_t uniform_below(std::uint64_t bound) noexcept;

  // True with probability numerator/denominator; denominator must be non-zero.
  [[nodiscard]] bool chance(std::uint32_t numerator, std::uint32_t denominator) noexcept;

  [[nodiscard]] std::uint64_t seed() const noexcept { return seed_; }
  [[nodiscard]] std::uint64_t draw_count() const noexcept { return draws_; }

 private:
  std::uint64_t state_{0};
  std::uint64_t seed_{0};
  std::uint64_t draws_{0};
};

}  // namespace dce
