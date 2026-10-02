// A small first-party test harness: registration, assertions and reporting.
//
// Cases register themselves at static-initialisation time and run in
// registration order, so a suite is deterministic without a discovery step and
// without a third-party framework. An assertion never throws and never aborts
// the process: it records a failure with its exact source location and keeps
// going, so one broken expectation still reports every other case in the run.
//
// The harness holds its lock only while it records an assertion, never while a
// test body runs. A case may therefore spawn child processes, block on its own
// synchronisation, or record failures from several threads at once.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <ostream>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>

#include "dce/status.hpp"

namespace dce::test {

// ---------------------------------------------------------------------------
// Value rendering
// ---------------------------------------------------------------------------
// The overload set is deliberately small: the integer types, bool, the string
// types, optionals, error codes, and any type that streams. A value is rendered
// only when an assertion actually fails, so rendering never costs a passing
// run and never has an observable side effect.
[[nodiscard]] std::string to_debug_string(bool value);
[[nodiscard]] std::string to_debug_string(char value);
[[nodiscard]] std::string to_debug_string(signed char value);
[[nodiscard]] std::string to_debug_string(unsigned char value);
[[nodiscard]] std::string to_debug_string(short value);
[[nodiscard]] std::string to_debug_string(unsigned short value);
[[nodiscard]] std::string to_debug_string(int value);
[[nodiscard]] std::string to_debug_string(unsigned int value);
[[nodiscard]] std::string to_debug_string(long value);
[[nodiscard]] std::string to_debug_string(unsigned long value);
[[nodiscard]] std::string to_debug_string(long long value);
[[nodiscard]] std::string to_debug_string(unsigned long long value);
[[nodiscard]] std::string to_debug_string(const std::string& value);
[[nodiscard]] std::string to_debug_string(std::string_view value);
[[nodiscard]] std::string to_debug_string(const char* value);
[[nodiscard]] std::string to_debug_string(std::nullopt_t value);
[[nodiscard]] std::string to_debug_string(ErrorCode value);

// The overload is more specialised than the streaming fallback, so an optional
// always renders as an optional even though the standard library can stream it.
template <class T>
[[nodiscard]] std::string to_debug_string(const std::optional<T>& value) {
  if (!value.has_value()) {
    return "nullopt";
  }
  return "optional(" + to_debug_string(*value) + ")";
}

// A scoped enumeration has no stream operator by default, so it is rendered as
// its underlying value. Suites that want the name can compare to_string() text,
// but a bare DCE_CHECK_EQ on two enumerators still produces a usable message
// rather than failing to compile.
template <class T>
  requires std::is_enum_v<T>
[[nodiscard]] std::string to_debug_string(T value) {
  return std::to_string(static_cast<long long>(static_cast<std::underlying_type_t<T>>(value)));
}

template <class T>
  requires(!std::is_enum_v<T>) && requires(const T& value, std::ostream& out) { out << value; }
[[nodiscard]] std::string to_debug_string(const T& value) {
  std::ostringstream out;
  out << value;
  return out.str();
}

// ---------------------------------------------------------------------------
// Recording
// ---------------------------------------------------------------------------
// All entry points are callable from any thread. They are deliberately not
// noexcept: a harness that terminated instead of reporting a failure would hide
// the very defect it exists to surface.
void count_assertion();
void record_failure(const char* file, int line, std::string_view expression, std::string_view detail);

// Renders an error code and its message as one line of diagnostic text.
[[nodiscard]] std::string outcome_detail(ErrorCode code, std::string_view message);

// Counts one assertion whose outcome is a plain boolean.
inline void check_true(const char* file, int line, std::string_view expression, bool value) {
  count_assertion();
  if (!value) {
    record_failure(file, line, expression, "expression evaluated to false");
  }
}

// Counts one comparison and renders its operands only when it failed.
template <class A, class B>
void check_comparison(const char* file, int line, std::string_view expression, const A& actual,
                      const B& expected, bool passed) {
  count_assertion();
  if (passed) {
    return;
  }
  std::string detail = "actual: ";
  detail += to_debug_string(actual);
  detail += " | expected: ";
  detail += to_debug_string(expected);
  record_failure(file, line, expression, detail);
}

// Counts one explicit failure with a caller-supplied message.
inline void check_fail(const char* file, int line, std::string_view message) {
  count_assertion();
  record_failure(file, line, "DCE_FAIL", to_debug_string(message));
}

// Outcome assertions. The overload pair lets Status and Result<T> be checked
// without the caller unwrapping either of them.
inline void check_ok(const char* file, int line, std::string_view expression, const Status& status) {
  count_assertion();
  if (!status.ok()) {
    record_failure(file, line, expression, "outcome is not ok: " + outcome_detail(status.code(), status.message()));
  }
}

template <class T>
void check_ok(const char* file, int line, std::string_view expression, const Result<T>& result) {
  count_assertion();
  if (!result.ok()) {
    record_failure(file, line, expression, "outcome is not ok: " + outcome_detail(result.code(), result.message()));
  }
}

inline void check_code(const char* file, int line, std::string_view expression, const Status& status,
                       ErrorCode expected) {
  count_assertion();
  if (status.code() == expected) {
    return;
  }
  std::string detail = "expected code: ";
  detail += to_string(expected);
  detail += " | actual: ";
  detail += outcome_detail(status.code(), status.message());
  record_failure(file, line, expression, detail);
}

template <class T>
void check_code(const char* file, int line, std::string_view expression, const Result<T>& result,
                ErrorCode expected) {
  count_assertion();
  if (result.code() == expected) {
    return;
  }
  std::string detail = "expected code: ";
  detail += to_string(expected);
  detail += " | actual: ";
  detail += outcome_detail(result.code(), result.message());
  record_failure(file, line, expression, detail);
}

// ---------------------------------------------------------------------------
// Registration and the runner
// ---------------------------------------------------------------------------
class Registrar {
 public:
  Registrar(const char* suite, const char* name, const char* file, int line, void (*body)());
};

// Runs the registered cases according to the command line, which accepts
// --list, --filter <substring>, --repeat <count> and --help. Returns 0 when
// every selected assertion passed, 1 when any assertion failed, and 2 for a
// command line error.
[[nodiscard]] int run_command_line(int argc, char** argv);

// The path this test binary was invoked with. A suite that needs to spawn a
// real child process uses it to spawn itself, which is how the persistence
// suite produces a genuine crash rather than a simulation of one.
[[nodiscard]] const char* program_path();

// Called by main before the runner starts. Not part of the assertion surface.
void record_program_path(const char* path) noexcept;

}  // namespace dce::test

// Registers a case at static-initialisation time. The macro introduces the
// opening of a function definition, so the case body follows it in braces.
#define DCE_TEST(suite_name, case_name)                                                            \
  static void dce_test_body_##suite_name##_##case_name();                                          \
  [[maybe_unused]] static const ::dce::test::Registrar dce_test_registrar_##suite_name##_##case_name( \
      #suite_name, #case_name, __FILE__, __LINE__, &dce_test_body_##suite_name##_##case_name);      \
  static void dce_test_body_##suite_name##_##case_name()

#define DCE_CHECK(expr) ::dce::test::check_true(__FILE__, __LINE__, #expr, static_cast<bool>(expr))

#define DCE_CHECK_TRUE(expr) DCE_CHECK(expr)

// Aborts the enclosing case when the expression is false. It is written for use
// in a case body (a void function); helpers that must not abort use DCE_CHECK.
#define DCE_REQUIRE(expr)                                                     \
  do {                                                                        \
    const bool dce_require_value_ = static_cast<bool>(expr);                  \
    ::dce::test::check_true(__FILE__, __LINE__, #expr, dce_require_value_);   \
    if (!dce_require_value_) {                                                \
      return;                                                                 \
    }                                                                         \
  } while (false)

#define DCE_FAIL(message) ::dce::test::check_fail(__FILE__, __LINE__, (message))

#define DCE_CHECK_EQ(actual, expected)                                          \
  do {                                                                          \
    const auto& dce_actual_ = (actual);                                         \
    const auto& dce_expected_ = (expected);                                     \
    ::dce::test::check_comparison(__FILE__, __LINE__, #actual " == " #expected, \
                                  dce_actual_, dce_expected_,                   \
                                  dce_actual_ == dce_expected_);                \
  } while (false)

#define DCE_CHECK_NE(actual, expected)                                          \
  do {                                                                          \
    const auto& dce_actual_ = (actual);                                         \
    const auto& dce_expected_ = (expected);                                     \
    ::dce::test::check_comparison(__FILE__, __LINE__, #actual " != " #expected, \
                                  dce_actual_, dce_expected_,                   \
                                  dce_actual_ != dce_expected_);                \
  } while (false)

#define DCE_CHECK_LT(actual, expected)                                          \
  do {                                                                          \
    const auto& dce_actual_ = (actual);                                         \
    const auto& dce_expected_ = (expected);                                     \
    ::dce::test::check_comparison(__FILE__, __LINE__, #actual " < " #expected,  \
                                  dce_actual_, dce_expected_,                   \
                                  dce_actual_ < dce_expected_);                 \
  } while (false)

#define DCE_CHECK_LE(actual, expected)                                          \
  do {                                                                          \
    const auto& dce_actual_ = (actual);                                         \
    const auto& dce_expected_ = (expected);                                     \
    ::dce::test::check_comparison(__FILE__, __LINE__, #actual " <= " #expected, \
                                  dce_actual_, dce_expected_,                   \
                                  dce_actual_ <= dce_expected_);                \
  } while (false)

#define DCE_CHECK_OK(expr) ::dce::test::check_ok(__FILE__, __LINE__, #expr, (expr))

// Aborts the enclosing case when the outcome is not ok, after reporting the
// error code and message. Written for decode chains, where continuing after a
// failed read would only add noise to the report.
#define DCE_REQUIRE_OK(expr)                                                         \
  do {                                                                               \
    const auto& dce_outcome_ = (expr);                                               \
    ::dce::test::check_ok(__FILE__, __LINE__, #expr, dce_outcome_);                  \
    if (!dce_outcome_.ok()) {                                                        \
      return;                                                                        \
    }                                                                                \
  } while (false)

#define DCE_CHECK_CODE(expr, expected_code) \
  ::dce::test::check_code(__FILE__, __LINE__, #expr, (expr), (expected_code))
