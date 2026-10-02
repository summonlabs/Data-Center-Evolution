// Checked arithmetic for capacities, counts, costs, durations, sequence numbers
// and externally supplied sizes.
//
// Every helper returns std::nullopt on overflow/underflow instead of wrapping.
// No first-party code computes a bound with unchecked arithmetic.
#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <type_traits>

namespace dce {

// True when the unsigned value fits exactly in T.
template <class T>
[[nodiscard]] constexpr bool fits(std::uint64_t value) noexcept {
  static_assert(std::is_unsigned_v<T>, "fits() is defined for unsigned destinations");
  return value <= static_cast<std::uint64_t>(std::numeric_limits<T>::max());
}

template <class T>
[[nodiscard]] constexpr std::optional<T> checked_add(T a, T b) noexcept {
  static_assert(std::is_unsigned_v<T>, "checked_add() is defined for unsigned types");
  if (b > static_cast<T>(std::numeric_limits<T>::max() - a)) {
    return std::nullopt;
  }
  return static_cast<T>(a + b);
}

template <class T>
[[nodiscard]] constexpr std::optional<T> checked_sub(T a, T b) noexcept {
  static_assert(std::is_unsigned_v<T>, "checked_sub() is defined for unsigned types");
  if (b > a) {
    return std::nullopt;
  }
  return static_cast<T>(a - b);
}

template <class T>
[[nodiscard]] constexpr std::optional<T> checked_mul(T a, T b) noexcept {
  static_assert(std::is_unsigned_v<T>, "checked_mul() is defined for unsigned types");
  if (a == 0 || b == 0) {
    return static_cast<T>(0);
  }
  if (a > static_cast<T>(std::numeric_limits<T>::max() / b)) {
    return std::nullopt;
  }
  return static_cast<T>(a * b);
}

// Increments with overflow detection; the canonical way to advance a counter.
template <class T>
[[nodiscard]] constexpr std::optional<T> checked_next(T value) noexcept {
  return checked_add<T>(value, static_cast<T>(1));
}

// Narrows a 64-bit quantity into T, refusing out-of-range values.
template <class T>
[[nodiscard]] constexpr std::optional<T> checked_narrow(std::uint64_t value) noexcept {
  static_assert(std::is_unsigned_v<T>, "checked_narrow() is defined for unsigned destinations");
  if (!fits<T>(value)) {
    return std::nullopt;
  }
  return static_cast<T>(value);
}

// Adds into an accumulator, refusing to wrap.
template <class T>
[[nodiscard]] constexpr bool accumulate(T& accumulator, T delta) noexcept {
  const std::optional<T> next = checked_add<T>(accumulator, delta);
  if (!next.has_value()) {
    return false;
  }
  accumulator = *next;
  return true;
}

// Deltas between two ordered counters; refuses to go backwards unless allowed.
template <class T>
[[nodiscard]] constexpr std::optional<T> checked_delta(T newer, T older) noexcept {
  return checked_sub<T>(newer, older);
}

}  // namespace dce
