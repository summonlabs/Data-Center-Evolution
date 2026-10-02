#include "harness.hpp"

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace dce::test {
namespace {

// Rendering is bounded so that a huge value cannot turn one failure into a wall
// of text, and every byte that is not printable ASCII is escaped so that a
// failure is always exactly one line.
constexpr std::size_t kMaxRenderedBytes = 384;
constexpr char kHexDigits[] = "0123456789abcdef";

std::string hex_escape(unsigned value) {
  std::string out;
  out.push_back('\\');
  out.push_back('x');
  out.push_back(kHexDigits[(value >> 4) & 0x0fu]);
  out.push_back(kHexDigits[value & 0x0fu]);
  return out;
}

std::string quote(std::string_view value, char delimiter) {
  const std::size_t shown = value.size() < kMaxRenderedBytes ? value.size() : kMaxRenderedBytes;
  std::string out;
  out.reserve(shown + 2);
  out.push_back(delimiter);
  for (std::size_t index = 0; index < shown; ++index) {
    const auto byte = static_cast<unsigned>(static_cast<unsigned char>(value[index]));
    if (byte == static_cast<unsigned>(static_cast<unsigned char>(delimiter)) || byte == '\\') {
      out.push_back('\\');
      out.push_back(static_cast<char>(byte));
    } else if (byte >= 0x20 && byte <= 0x7e) {
      out.push_back(static_cast<char>(byte));
    } else {
      out += hex_escape(byte);
    }
  }
  if (shown < value.size()) {
    out += "...";
  }
  out.push_back(delimiter);
  return out;
}

// Maps every control byte to an escape, so a message that carries a newline or
// a terminal escape sequence still occupies one line of output.
std::string single_line(std::string_view value) {
  std::string out;
  out.reserve(value.size());
  for (char character : value) {
    const auto byte = static_cast<unsigned>(static_cast<unsigned char>(character));
    if (byte < 0x20 || byte == 0x7f) {
      out += hex_escape(byte);
    } else {
      out.push_back(character);
    }
  }
  return out;
}

void write_all(std::FILE* stream, std::string_view text) {
  if (!text.empty()) {
    (void)std::fwrite(text.data(), 1, text.size(), stream);
  }
}

void write_line(std::string_view text) {
  write_all(stdout, text);
  (void)std::fputc('\n', stdout);
  (void)std::fflush(stdout);
}

void write_error_line(std::string_view text) {
  write_all(stderr, text);
  (void)std::fputc('\n', stderr);
  (void)std::fflush(stderr);
}

struct TestCase {
  std::string suite;
  std::string name;
  std::string file;
  int line{0};
  void (*body)(){nullptr};
};

struct Statistics {
  std::uint64_t assertions{0};
  std::uint64_t failures{0};
};

// One lock guards the registry, the counters and the name of the case that is
// running. It is never held while a case body executes.
std::mutex& state_mutex() {
  static std::mutex mutex;
  return mutex;
}

std::vector<TestCase>& registry() {
  static std::vector<TestCase> cases;
  return cases;
}

Statistics& statistics() {
  static Statistics value;
  return value;
}

std::string& current_case() {
  static std::string value;
  return value;
}

struct Options {
  bool list_only{false};
  bool help{false};
  std::string filter;
  std::uint32_t repeat{1};
};

void print_usage(std::FILE* stream) {
  write_all(stream, "usage: dce_test [--list] [--filter <substring>] [--repeat <count>] [--help]\n");
}

int usage_error(std::string_view message) {
  write_error_line(std::string("dce_test: ") + single_line(message));
  print_usage(stderr);
  return 2;
}

bool parse_count(std::string_view text, std::uint32_t& out) {
  if (text.empty()) {
    return false;
  }
  std::uint32_t value = 0;
  const char* const begin = text.data();
  const char* const end = text.data() + text.size();
  const std::from_chars_result parsed = std::from_chars(begin, end, value);
  if (parsed.ec != std::errc{} || parsed.ptr != end) {
    return false;
  }
  out = value;
  return true;
}

}  // namespace

std::string to_debug_string(bool value) { return value ? "true" : "false"; }

std::string to_debug_string(char value) { return quote(std::string_view(&value, 1), '\''); }

std::string to_debug_string(signed char value) { return std::to_string(static_cast<int>(value)); }

std::string to_debug_string(unsigned char value) { return std::to_string(static_cast<unsigned>(value)); }

std::string to_debug_string(short value) { return std::to_string(static_cast<int>(value)); }

std::string to_debug_string(unsigned short value) { return std::to_string(static_cast<unsigned>(value)); }

std::string to_debug_string(int value) { return std::to_string(value); }

std::string to_debug_string(unsigned int value) { return std::to_string(value); }

std::string to_debug_string(long value) { return std::to_string(value); }

std::string to_debug_string(unsigned long value) { return std::to_string(value); }

std::string to_debug_string(long long value) { return std::to_string(value); }

std::string to_debug_string(unsigned long long value) { return std::to_string(value); }

std::string to_debug_string(const std::string& value) { return quote(value, '"'); }

std::string to_debug_string(std::string_view value) { return quote(value, '"'); }

std::string to_debug_string(const char* value) {
  return value == nullptr ? std::string("(null)") : quote(std::string_view(value), '"');
}

std::string to_debug_string(std::nullopt_t) { return "nullopt"; }

std::string to_debug_string(ErrorCode value) { return std::string(to_string(value)); }

std::string outcome_detail(ErrorCode code, std::string_view message) {
  std::string out = to_string(code);
  if (!message.empty()) {
    out += " (";
    out += single_line(message);
    out += ')';
  }
  return out;
}

void count_assertion() {
  const std::lock_guard<std::mutex> lock(state_mutex());
  ++statistics().assertions;
}

void record_failure(const char* file, int line, std::string_view expression, std::string_view detail) {
  const std::lock_guard<std::mutex> lock(state_mutex());
  ++statistics().failures;
  std::string text = file != nullptr ? single_line(file) : std::string("<unknown>");
  text += ':';
  text += std::to_string(line);
  text += ": FAILED";
  if (!current_case().empty()) {
    text += " [";
    text += current_case();
    text += ']';
  }
  text += ' ';
  text += single_line(expression);
  text += " -- ";
  text += single_line(detail);
  write_line(text);
}

Registrar::Registrar(const char* suite, const char* name, const char* file, int line, void (*body)()) {
  const std::lock_guard<std::mutex> lock(state_mutex());
  registry().push_back(TestCase{suite, name, file, line, body});
}

int run_command_line(int argc, char** argv) {
  Options options;
  for (int index = 1; index < argc; ++index) {
    const char* const raw = argv[index];
    const std::string_view argument = raw != nullptr ? std::string_view(raw) : std::string_view();
    if (argument == "--list") {
      options.list_only = true;
    } else if (argument == "--help") {
      options.help = true;
    } else if (argument == "--filter") {
      if (index + 1 >= argc || argv[index + 1] == nullptr) {
        return usage_error("--filter requires a substring");
      }
      ++index;
      options.filter = argv[index];
    } else if (argument == "--repeat") {
      if (index + 1 >= argc || argv[index + 1] == nullptr) {
        return usage_error("--repeat requires a count");
      }
      ++index;
      std::uint32_t count = 0;
      if (!parse_count(argv[index], count) || count == 0) {
        return usage_error("--repeat requires a count of at least one");
      }
      options.repeat = count;
    } else {
      return usage_error("unknown argument: " + single_line(argument));
    }
  }

  std::vector<TestCase> selection;
  {
    const std::lock_guard<std::mutex> lock(state_mutex());
    for (const TestCase& item : registry()) {
      const std::string full_name = item.suite + "." + item.name;
      if (options.filter.empty() || full_name.find(options.filter) != std::string::npos) {
        selection.push_back(item);
      }
    }
  }

  if (options.help) {
    print_usage(stdout);
    return 0;
  }

  if (options.list_only) {
    for (const TestCase& item : selection) {
      write_line(item.suite + "." + item.name);
    }
    return 0;
  }

  if (selection.empty()) {
    return usage_error(options.filter.empty() ? "no test case is registered"
                                              : "--filter \"" + options.filter + "\" matches no test case");
  }

  {
    const std::lock_guard<std::mutex> lock(state_mutex());
    statistics() = Statistics{};
  }

  std::uint64_t tests_run = 0;
  for (std::uint32_t pass = 0; pass < options.repeat; ++pass) {
    for (const TestCase& item : selection) {
      {
        const std::lock_guard<std::mutex> lock(state_mutex());
        current_case() = item.suite + "." + item.name;
      }
      // A case that throws is a failure, not the end of the run: the remaining
      // cases still report, and the exit code is still non-zero.
      try {
        item.body();
      } catch (const std::exception& error) {
        record_failure(item.file.c_str(), item.line, "<case body>",
                       std::string("unhandled exception: ") + error.what());
      } catch (...) {
        record_failure(item.file.c_str(), item.line, "<case body>", "unhandled exception of unknown type");
      }
      {
        const std::lock_guard<std::mutex> lock(state_mutex());
        current_case().clear();
      }
      ++tests_run;
    }
  }

  std::uint64_t assertions = 0;
  std::uint64_t failures = 0;
  {
    const std::lock_guard<std::mutex> lock(state_mutex());
    assertions = statistics().assertions;
    failures = statistics().failures;
  }

  std::string summary = "tests: ";
  summary += std::to_string(tests_run);
  summary += ", assertions: ";
  summary += std::to_string(assertions);
  summary += ", failures: ";
  summary += std::to_string(failures);
  write_line(summary);

  return failures == 0 ? 0 : 1;
}

namespace {
const char* g_program_path = "";
}  // namespace

void record_program_path(const char* path) noexcept { g_program_path = path == nullptr ? "" : path; }

const char* program_path() { return g_program_path; }

}  // namespace dce::test

int main(int argc, char** argv) {
  dce::test::record_program_path(argc > 0 ? argv[0] : "");
  return dce::test::run_command_line(argc, argv);
}
