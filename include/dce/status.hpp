// Outcome vocabulary for the whole runtime.
//
// Unknown, stale, conflicting, unsupported, invalid and indeterminate are
// distinct codes on purpose: collapsing them would lose semantics the
// evolution boundary is required to report.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <utility>

namespace dce {

enum class ErrorCode : std::uint16_t {
  ok = 0,
  invalid_argument,   // caller supplied something structurally unusable
  malformed,          // bytes did not decode under the documented format
  out_of_range,       // a value was well formed but outside the accepted domain
  overflow,           // checked arithmetic refused to wrap
  not_found,          // a referenced authoritative object does not exist
  already_exists,     // a create-only operation hit an existing identity
  duplicate,          // the same effect was already accepted (idempotent replay)
  conflict,           // two authoritative claims disagree
  stale,              // the request is older than the authority it targets
  fenced,             // the writer/generation was superseded and may no longer act
  unsupported,        // the capability is understood but not offered
  indeterminate,      // the truth is not knowable from the evidence at hand
  refused,            // a guard or gate declined the transition, with a reason
  denied,             // authority was required and was not presented
  io_error,           // an operating-system level failure
  corruption,         // durable bytes are internally inconsistent
  torn_tail,          // the final record was not fully committed
  busy,               // an exclusive resource is held elsewhere
  closed,             // the object was shut down
  cancelled,          // the work was cancelled and may not report success
  limit_exceeded,     // a documented bound was exceeded
  internal,           // an invariant that should hold did not
};

[[nodiscard]] const char* to_string(ErrorCode code) noexcept;

struct Error {
  ErrorCode code{ErrorCode::ok};
  std::string message;

  [[nodiscard]] bool ok() const noexcept { return code == ErrorCode::ok; }
};

// A no-value outcome: either ok, or a single Error.
class Status {
 public:
  Status() noexcept = default;
  Status(Error error) : error_(std::move(error)) {}

  // The default-constructed Status is success; there is no separate factory,
  // because a static ok() and a predicate ok() cannot coexist.
  [[nodiscard]] bool ok() const noexcept { return !error_.has_value(); }
  [[nodiscard]] explicit operator bool() const noexcept { return ok(); }

  [[nodiscard]] const Error& error() const noexcept {
    static const Error none{};
    return error_.has_value() ? *error_ : none;
  }
  [[nodiscard]] ErrorCode code() const noexcept { return error().code; }
  [[nodiscard]] const std::string& message() const noexcept { return error().message; }

 private:
  std::optional<Error> error_;
};

// A value-or-error outcome. Constructed implicitly from either, so that
// functions can simply return the value on success or the Error on failure.
template <class T>
class Result {
 public:
  Result(T value) : value_(std::move(value)) {}          // NOLINT(google-explicit-constructor)
  Result(Error error) : error_(std::move(error)) {}      // NOLINT(google-explicit-constructor)

  [[nodiscard]] bool ok() const noexcept { return value_.has_value(); }
  [[nodiscard]] explicit operator bool() const noexcept { return ok(); }

  [[nodiscard]] const T& value() const& noexcept { return *value_; }
  [[nodiscard]] T& value() & noexcept { return *value_; }
  [[nodiscard]] T&& value() && noexcept { return std::move(*value_); }

  [[nodiscard]] T value_or(T fallback) const {
    return value_.has_value() ? *value_ : std::move(fallback);
  }

  [[nodiscard]] const T& operator*() const& noexcept { return *value_; }
  [[nodiscard]] T& operator*() & noexcept { return *value_; }
  [[nodiscard]] const T* operator->() const noexcept { return &*value_; }
  [[nodiscard]] T* operator->() noexcept { return &*value_; }

  [[nodiscard]] const Error& error() const noexcept {
    static const Error none{};
    return error_.has_value() ? *error_ : none;
  }
  [[nodiscard]] ErrorCode code() const noexcept { return error().code; }
  [[nodiscard]] const std::string& message() const noexcept { return error().message; }

  // Moves the value out when present; otherwise reports the error.
  [[nodiscard]] Status copy_to(T& out) const {
    if (!value_.has_value()) {
      return error();
    }
    out = *value_;
    return Status{};
  }

  [[nodiscard]] Status status() const {
    return value_.has_value() ? Status{} : Status(error());
  }

 private:
  std::optional<T> value_;
  std::optional<Error> error_;
};

// Propagate a failure out of the enclosing function.
#define DCE_TRY(expr)                        \
  do {                                       \
    const ::dce::Status dce_status_ = (expr); \
    if (!dce_status_.ok()) {                 \
      return dce_status_.error();            \
    }                                        \
  } while (false)

// Bind a value or propagate the failure.
#define DCE_ASSIGN(name, expr)                       \
  auto dce_result_##name = (expr);                   \
  if (!dce_result_##name.ok()) {                     \
    return dce_result_##name.error();                \
  }                                                  \
  auto& name = dce_result_##name.value()

}  // namespace dce
