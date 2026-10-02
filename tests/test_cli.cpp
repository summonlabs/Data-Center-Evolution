// Command line smoke tests.
//
// The shipped executable is a real operator surface, so it is exercised as one:
// every command is run as a child process and its actual output is inspected.
#include <algorithm>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

#include "dce/codec.hpp"
#include "dce/platform.hpp"
#include "dce/text.hpp"
#include "harness.hpp"

namespace {

#ifdef _WIN32
constexpr const char* kExecutableSuffix = ".exe";
#else
constexpr const char* kExecutableSuffix = "";
#endif

std::string find_cli() {
  std::vector<std::string> candidates;
#ifdef DCE_CLI_BUILD_DIR
  const std::string build = DCE_CLI_BUILD_DIR;
  candidates.push_back(build + "/tools/dce" + kExecutableSuffix);
  candidates.push_back(build + "/tools/Release/dce" + kExecutableSuffix);
  candidates.push_back(build + "/tools/Debug/dce" + kExecutableSuffix);
  candidates.push_back(build + "/tools/RelWithDebInfo/dce" + kExecutableSuffix);
#endif
  const std::string self = dce::test::program_path();
  const std::size_t separator = self.find_last_of("/\\");
  if (separator != std::string::npos) {
    const std::string directory = self.substr(0, separator);
    candidates.push_back(directory + "/dce" + kExecutableSuffix);
    candidates.push_back(directory + "/../tools/dce" + kExecutableSuffix);
    candidates.push_back(directory + "/Release/dce" + kExecutableSuffix);
    candidates.push_back(directory + "/Debug/dce" + kExecutableSuffix);
  }
  // A manual run (a developer compiling a suite by hand) has no build-tree
  // define, so the working directory is searched too.
  candidates.push_back("build/full/tools/dce" + std::string(kExecutableSuffix));
  candidates.push_back("build/dev/tools/dce" + std::string(kExecutableSuffix));
  candidates.push_back("../build/full/tools/dce" + std::string(kExecutableSuffix));
  candidates.push_back("../build/dev/tools/dce" + std::string(kExecutableSuffix));
  for (const std::string& candidate : candidates) {
    const dce::Result<dce::platform::FileStat> stat = dce::platform::stat_path(candidate);
    if (stat.ok() && stat->is_regular_file) {
      return candidate;
    }
  }
  return {};
}

std::string temporary_root(const std::string& name) {
  const char* base = std::getenv("TEMP");
  if (base == nullptr) {
    base = std::getenv("TMPDIR");
  }
  if (base == nullptr) {
    base = ".";
  }
  return dce::platform::join_path(
      base, "dce-cli-" + name + "-" + std::to_string(dce::platform::process_id()));
}

struct Run {
  int exit_code{0};
  std::string output;
};

// Runs the executable to completion, collecting every line it prints. The child
// is expected to finish on its own; nothing here kills it or races a clock.
dce::Result<Run> run_cli(const std::string& program, const std::vector<std::string>& arguments) {
  dce::platform::ProcessOptions options;
  options.program = program;
  options.arguments = arguments;
  options.capture_stdout = true;
  options.capture_stderr = false;
  dce::Result<dce::platform::ChildProcess> child = dce::platform::ChildProcess::spawn(options);
  if (!child.ok()) {
    return child.error();
  }
  Run run;
  while (true) {
    dce::Result<std::optional<std::string>> line = child->read_line(8192);
    if (!line.ok()) {
      return line.error();
    }
    if (!line->has_value()) {
      break;
    }
    run.output.append(line->value());
    run.output.push_back('\n');
  }
  dce::Result<int> exit_code = child->wait();
  if (!exit_code.ok()) {
    return exit_code.error();
  }
  run.exit_code = *exit_code;
  return run;
}

bool contains(const std::string& text, const std::string& needle) {
  return text.find(needle) != std::string::npos;
}

}  // namespace

DCE_TEST(cli, reports_its_version) {
  const std::string cli = find_cli();
  DCE_REQUIRE(!cli.empty());
  dce::Result<Run> run = run_cli(cli, {"version"});
  DCE_REQUIRE_OK(run);
  DCE_CHECK_EQ(run->exit_code, 0);
  DCE_CHECK_TRUE(contains(run->output, "dce 1.0.1"));
}

DCE_TEST(cli, rejects_a_missing_or_unknown_command) {
  const std::string cli = find_cli();
  DCE_REQUIRE(!cli.empty());
  dce::Result<Run> bare = run_cli(cli, {});
  DCE_REQUIRE_OK(bare);
  DCE_CHECK_EQ(bare->exit_code, 2);
  DCE_CHECK_TRUE(contains(bare->output, "usage:"));

  dce::Result<Run> unknown = run_cli(cli, {"definitely-not-a-command"});
  DCE_REQUIRE_OK(unknown);
  DCE_CHECK_EQ(unknown->exit_code, 2);

  dce::Result<Run> incomplete = run_cli(cli, {"coordinator", "serve"});
  DCE_REQUIRE_OK(incomplete);
  DCE_CHECK_EQ(incomplete->exit_code, 2);
}

DCE_TEST(cli, generates_inspects_and_digests_a_plan) {
  const std::string cli = find_cli();
  DCE_REQUIRE(!cli.empty());
  const std::string root = temporary_root("synthetic");
  DCE_REQUIRE(dce::platform::make_directories(root).ok());
  const std::string plan_path = dce::platform::join_path(root, "plan.dce");
  const std::string profiles = dce::platform::join_path(root, "profiles");

  dce::Result<Run> generated = run_cli(cli, {"synthetic", "--seed", "20260101", "--sites", "6",
                                             "--components", "2", "--cohorts", "2",
                                             "--plan-out", plan_path, "--profiles-out", profiles});
  DCE_REQUIRE_OK(generated);
  DCE_CHECK_EQ(generated->exit_code, 0);
  DCE_CHECK_TRUE(contains(generated->output, "digest"));

  const dce::Result<dce::platform::FileStat> plan_stat = dce::platform::stat_path(plan_path);
  DCE_REQUIRE_OK(plan_stat);
  DCE_CHECK_TRUE(plan_stat->size > 0);

  dce::Result<Run> shown = run_cli(cli, {"plan", "show", "--plan", plan_path});
  DCE_REQUIRE_OK(shown);
  DCE_CHECK_EQ(shown->exit_code, 0);
  DCE_CHECK_TRUE(contains(shown->output, "plan            plan.synthetic.20260101"));
  DCE_CHECK_TRUE(contains(shown->output, "sites           6"));
  DCE_CHECK_TRUE(contains(shown->output, "cohorts         2"));
  DCE_CHECK_TRUE(contains(shown->output, "point of no return"));

  dce::Result<Run> digest = run_cli(cli, {"plan", "digest", "--plan", plan_path});
  DCE_REQUIRE_OK(digest);
  DCE_CHECK_EQ(digest->exit_code, 0);
  DCE_CHECK_EQ(digest->output.size(), static_cast<std::size_t>(65));

  // The printed digest must be the digest the library computes.
  dce::Result<dce::platform::File> file =
      dce::platform::File::open(plan_path, dce::platform::OpenMode::read_only);
  DCE_REQUIRE_OK(file);
  dce::Result<std::uint64_t> size = file->size();
  DCE_REQUIRE_OK(size);
  std::vector<std::byte> bytes(static_cast<std::size_t>(*size));
  DCE_REQUIRE_OK(file->read_at(0, bytes));
  dce::CanonicalReader reader(bytes);
  dce::Result<dce::EvolutionPlan> plan = dce::codec::get<dce::EvolutionPlan>(reader);
  DCE_REQUIRE_OK(plan);
  DCE_REQUIRE_OK(dce::canonicalize(*plan));
  DCE_CHECK_TRUE(contains(digest->output, plan->digest.to_hex()));

  DCE_CHECK_TRUE(dce::platform::remove_tree(root).ok());
}

DCE_TEST(cli, reports_a_clean_error_when_the_coordinator_is_absent) {
  const std::string cli = find_cli();
  DCE_REQUIRE(!cli.empty());
  // Obtain a port that is definitely free by binding and releasing one.
  dce::Result<dce::platform::Socket> listener = dce::platform::Socket::listen_loopback(0);
  DCE_REQUIRE_OK(listener);
  dce::Result<std::uint16_t> port = listener->local_port();
  DCE_REQUIRE_OK(port);
  DCE_REQUIRE_OK(listener->close());

  dce::Result<Run> run = run_cli(cli, {"status", "--port", std::to_string(*port)});
  DCE_REQUIRE_OK(run);
  DCE_CHECK_TRUE(run->exit_code != 0);
  DCE_CHECK_TRUE(contains(run->output, "dce: "));
}

DCE_TEST(cli, rejects_an_unknown_lifecycle_event) {
  const std::string cli = find_cli();
  DCE_REQUIRE(!cli.empty());
  dce::Result<Run> run = run_cli(cli, {"event", "--port", "1", "--event", "not-an-event"});
  DCE_REQUIRE_OK(run);
  DCE_CHECK_TRUE(run->exit_code != 0);
  DCE_CHECK_TRUE(contains(run->output, "unknown lifecycle event"));
}
