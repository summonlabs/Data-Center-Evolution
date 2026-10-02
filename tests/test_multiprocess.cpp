// Real independent operating-system processes.
//
// This suite proves the claims that cannot be proved inside one address space:
// several site processes at differing versions rolling forward under a live
// coordinator, a site that stops and reconnects, and a coordinator that is
// killed outright and restarted on the same durable store. Every process here
// is a genuine child process running the shipped dce executable.
#include <algorithm>
#include <chrono>
#include <iostream>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "dce/codec.hpp"
#include "dce/node.hpp"
#include "dce/platform.hpp"
#include "dce/protocol.hpp"
#include "dce/synthetic.hpp"
#include "dce/validate.hpp"
#include "harness.hpp"

namespace {

using dce::platform::ChildProcess;
using dce::platform::ProcessOptions;

#ifdef _WIN32
constexpr const char* kExecutableSuffix = ".exe";
#else
constexpr const char* kExecutableSuffix = "";
#endif

// Locates the shipped dce executable. The build passes the build directory; the
// runner's own directory is a fallback so the suite also works when the tree is
// laid out differently.
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
  const std::size_t separator = self.find_last_of("/" "\\");
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

std::string temporary_root() {
  const char* base = std::getenv("TEMP");
  if (base == nullptr) {
    base = std::getenv("TMPDIR");
  }
  if (base == nullptr) {
    base = ".";
  }
  std::string name = "dce-multiprocess-";
  name += std::to_string(dce::platform::process_id());
  return dce::platform::join_path(base, name);
}

// A child process plus the lines it has emitted so far, so a readiness line can
// be awaited without guessing at timing.
// A hang must be attributable from the job log, so every stage announces
// itself as it is reached. Nothing here is a timeout: the lines exist to say
// where the suite got to, not to bound how long it may take.
void stage(const std::string& text) {
  std::cout << "[multiprocess] " << text << std::endl;
}
class Child {
 public:
  Child() = default;
  Child(Child&&) = default;
  Child& operator=(Child&&) = default;
  Child(const Child&) = delete;
  Child& operator=(const Child&) = delete;

  static dce::Result<std::unique_ptr<Child>> start(const std::string& program,
                                                   const std::vector<std::string>& arguments) {
    ProcessOptions options;
    options.program = program;
    options.arguments = arguments;
    options.capture_stdout = true;
    options.capture_stderr = false;
    dce::Result<ChildProcess> process = ChildProcess::spawn(options);
    if (!process.ok()) {
      return process.error();
    }
    auto child = std::make_unique<Child>();
    child->process_ = std::move(*process);
    child->program_ = program;
    return child;
  }

  // Blocks on the child's own output: no polling and no sleeping.
  dce::Result<std::string> await_line(const std::string& prefix) {
    stage("waiting for " + program_ + " to print a line starting with " + prefix);
    while (true) {
      dce::Result<std::optional<std::string>> line = process_.read_line(4096);
      if (!line.ok()) {
        return line.error();
      }
      if (!line->has_value()) {
        return dce::Error{dce::ErrorCode::closed,
                          program_ + " ended before it reported readiness"};
      }
      if (line->value().rfind(prefix, 0) == 0) {
        return line->value();
      }
    }
  }

  dce::Status terminate() { return process_.terminate(); }
  dce::Result<int> wait() { return process_.wait(); }
  dce::Result<bool> running() { return process_.running(); }

 private:
  ChildProcess process_;
  std::string program_;
};

dce::Result<std::unique_ptr<dce::AdminClient>> connect_admin(std::uint16_t port) {
  return dce::AdminClient::connect(port);
}

dce::Result<dce::wire::AdminResponse> submit(dce::AdminClient& client,
                                             const dce::EvolutionPlan& plan) {
  dce::wire::AdminRequest request;
  request.op = dce::wire::AdminOp::submit_plan;
  request.request_id = 1;
  request.plan_body = plan;
  return client.call(request);
}

dce::Result<dce::wire::AdminResponse> validate(dce::AdminClient& client) {
  dce::wire::AdminRequest request;
  request.op = dce::wire::AdminOp::validate;
  request.request_id = 2;
  return client.call(request);
}

dce::Result<dce::wire::AdminResponse> lifecycle(dce::AdminClient& client,
                                                dce::LifecycleEvent event, bool gates) {
  dce::wire::AdminRequest request;
  request.op = dce::wire::AdminOp::event;
  request.request_id = 3;
  request.event = event;
  request.transition.gates_satisfied = gates;
  return client.call(request);
}

dce::Result<dce::wire::AdminResponse> status_of(dce::AdminClient& client) {
  dce::wire::AdminRequest request;
  request.op = dce::wire::AdminOp::status;
  request.request_id = 4;
  return client.call(request);
}

// Waits for the fleet to reach full progress. The loop is bounded by a number
// of observed rounds rather than by a clock, so a rollout that genuinely stops
// progressing fails the test instead of being killed.
bool await_completion(dce::AdminClient& client, std::uint64_t sites, std::size_t budget,
                      std::uint64_t& observed_sites) {
  for (std::size_t attempt = 0; attempt < budget; ++attempt) {
    if (attempt % 250 == 0) {
      stage("awaiting completion, probe " + std::to_string(attempt));
    }
    dce::Result<dce::wire::AdminResponse> response = status_of(client);
    if (!response.ok()) {
      return false;
    }
    observed_sites = response->sites_complete;
    if (response->sites_complete >= sites) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  return false;
}

void write_profiles(const std::string& directory, const std::vector<dce::SiteRecord>& profiles,
                    std::vector<std::string>& paths) {
  for (const dce::SiteRecord& profile : profiles) {
    dce::CanonicalWriter writer;
    const dce::Status encoded = dce::codec::put(writer, profile);
    DCE_REQUIRE(encoded.ok());
    const std::string path = dce::platform::join_path(directory, profile.id.str() + ".site");
    DCE_REQUIRE(dce::platform::make_directories(directory).ok());
    dce::Result<dce::platform::File> file =
        dce::platform::File::open(path, dce::platform::OpenMode::read_write);
    DCE_REQUIRE_OK(file);
    DCE_REQUIRE(file->write_at(0, writer.bytes()).ok());
    DCE_REQUIRE(file->truncate(writer.size()).ok());
    DCE_REQUIRE(file->sync().ok());
    paths.push_back(path);
  }
}

struct Scenario {
  std::string root;
  std::string plan_path;
  std::vector<std::string> profile_paths;
  dce::SyntheticFleet fleet;
};

bool prepare(Scenario& scenario, std::size_t sites, std::size_t components, std::size_t cohorts,
             std::uint64_t seed) {
  scenario.root = temporary_root();
  if (!dce::platform::make_directories(scenario.root).ok()) {
    return false;
  }
  dce::SyntheticFleetOptions options;
  options.sites = sites;
  options.components = components;
  options.cohorts = cohorts;
  options.seed = seed;
  dce::Result<dce::SyntheticFleet> fleet = dce::make_synthetic_fleet(options);
  if (!fleet.ok()) {
    return false;
  }
  scenario.fleet = std::move(*fleet);

  const std::string profiles = dce::platform::join_path(scenario.root, "profiles");
  write_profiles(profiles, scenario.fleet.profiles, scenario.profile_paths);

  dce::CanonicalWriter writer;
  if (!dce::codec::put(writer, scenario.fleet.plan).ok()) {
    return false;
  }
  scenario.plan_path = dce::platform::join_path(scenario.root, "plan.dce");
  dce::Result<dce::platform::File> file =
      dce::platform::File::open(scenario.plan_path, dce::platform::OpenMode::read_write);
  if (!file.ok()) {
    return false;
  }
  if (!file->write_at(0, writer.bytes()).ok() || !file->truncate(writer.size()).ok() ||
      !file->sync().ok()) {
    return false;
  }
  return true;
}

std::uint16_t parse_listen_port(const std::string& line) {
  const std::size_t space = line.find(' ');
  if (space == std::string::npos) {
    return 0;
  }
  dce::Result<std::uint64_t> parsed = dce::text::parse_u64(line.substr(space + 1));
  return parsed.ok() ? static_cast<std::uint16_t>(*parsed) : 0;
}

}  // namespace

DCE_TEST(multiprocess, rolling_evolution_across_independent_site_processes) {
  stage("test 1: four site processes roll forward one stage at a time");
  Scenario scenario;
  DCE_REQUIRE(prepare(scenario, 4, 2, 2, 41));
  const std::string cli = find_cli();
  DCE_REQUIRE(!cli.empty());

  const std::string coordinator_store = dce::platform::join_path(scenario.root, "coordinator");
  dce::Result<std::unique_ptr<Child>> coordinator = Child::start(
      cli, {"coordinator", "serve", "--store", coordinator_store, "--port", "0"});
  DCE_REQUIRE_OK(coordinator);
  dce::Result<std::string> listen = (*coordinator)->await_line("LISTEN ");
  DCE_REQUIRE_OK(listen);
  const std::uint16_t port = parse_listen_port(*listen);
  DCE_REQUIRE(port != 0);

  std::vector<std::unique_ptr<Child>> sites;
  for (std::size_t index = 0; index < scenario.fleet.profiles.size(); ++index) {
    const std::string store = dce::platform::join_path(
        scenario.root, "site-store-" + std::to_string(index));
    dce::Result<std::unique_ptr<Child>> node = Child::start(
        cli, {"site", "serve", "--store", store, "--profile",
                       scenario.profile_paths[index], "--coordinator-port",
                       std::to_string(port), "--delegated", "--rounds", "500"});
    DCE_REQUIRE_OK(node);
    dce::Result<std::string> ready = (*node)->await_line("SITE ");
    DCE_REQUIRE_OK(ready);
    sites.push_back(std::move(*node));
  }

  dce::Result<std::unique_ptr<dce::AdminClient>> client = connect_admin(port);
  DCE_REQUIRE_OK(client);

  dce::Result<dce::wire::AdminResponse> submitted = submit(**client, scenario.fleet.plan);
  DCE_REQUIRE_OK(submitted);
  DCE_CHECK_EQ(submitted->status, dce::ErrorCode::ok);

  // A site becomes observable only once its process has connected, and the
  // plan is written against the whole fleet, so a validation that arrives
  // before the last site has reported is legitimately refused. Only the two
  // refusals that mean "this site has not connected yet" are retried, and the
  // loop is bounded by a number of attempts rather than by a clock: any other
  // refusal, or a fleet that never becomes fully observable, fails the test.
  bool accepted = false;
  std::string refusal_text;
  for (std::size_t attempt = 0; attempt < 500 && !accepted; ++attempt) {
    dce::Result<dce::wire::AdminResponse> validated = validate(**client);
    DCE_REQUIRE_OK(validated);
    DCE_CHECK_EQ(validated->status, dce::ErrorCode::ok);
    DCE_REQUIRE(validated->report.has_value());
    accepted = validated->report->accepted();
    if (accepted) {
      break;
    }
    refusal_text.clear();
    bool waiting_for_observation = !validated->report->refusals.empty();
    for (const auto& refusal : validated->report->refusals) {
      if (refusal.code != dce::RefusalCode::source_state_stale &&
          refusal.code != dce::RefusalCode::site_not_observed) {
        waiting_for_observation = false;
      }
      if (!refusal_text.empty()) {
        refusal_text += "; ";
      }
      refusal_text += std::string(dce::to_string(refusal.code)) + ": " + refusal.explanation;
    }
    if (!waiting_for_observation) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  DCE_CHECK_TRUE(accepted);
  if (!accepted) {
    DCE_FAIL("the live coordinator refused a synthetic plan: " + refusal_text);
  }

  DCE_REQUIRE_OK(lifecycle(**client, dce::LifecycleEvent::validate, true));
  DCE_REQUIRE_OK(lifecycle(**client, dce::LifecycleEvent::stage, true));
  dce::Result<dce::wire::AdminResponse> rolling =
      lifecycle(**client, dce::LifecycleEvent::begin_rollout, true);
  DCE_REQUIRE_OK(rolling);
  DCE_CHECK_EQ(rolling->status, dce::ErrorCode::ok);

  std::uint64_t complete = 0;
  const bool finished =
      await_completion(**client, scenario.fleet.profiles.size(), 2000, complete);
  DCE_CHECK_TRUE(finished);
  if (!finished) {
    DCE_FAIL("only " + std::to_string(complete) + " of " +
             std::to_string(scenario.fleet.profiles.size()) +
             " site processes reached the final stage");
  }
  dce::Result<dce::wire::AdminResponse> final_status = status_of(**client);
  DCE_REQUIRE_OK(final_status);
  DCE_CHECK_EQ(final_status->sites_complete, scenario.fleet.profiles.size());
  DCE_CHECK_TRUE(final_status->checkpoints >=
                 scenario.fleet.profiles.size() * scenario.fleet.plan.steps.size());
  DCE_CHECK_EQ(final_status->refusals, static_cast<std::uint64_t>(0));

  for (std::unique_ptr<Child>& site : sites) {
    DCE_REQUIRE(site->terminate().ok());
  }
  sites.clear();
  DCE_REQUIRE((*coordinator)->terminate().ok());
  coordinator->reset();
  DCE_CHECK_TRUE(dce::platform::remove_tree(scenario.root).ok());
}

DCE_TEST(multiprocess, coordinator_restart_fences_and_preserves_progress) {
  stage("test 2: a killed coordinator is restarted on the same store");
  Scenario scenario;
  DCE_REQUIRE(prepare(scenario, 3, 2, 2, 77));
  const std::string cli = find_cli();
  DCE_REQUIRE(!cli.empty());

  const std::string coordinator_store = dce::platform::join_path(scenario.root, "coordinator-restart");
  dce::Result<std::unique_ptr<Child>> coordinator = Child::start(
      cli, {"coordinator", "serve", "--store", coordinator_store, "--port", "0"});
  DCE_REQUIRE_OK(coordinator);
  dce::Result<std::string> listen = (*coordinator)->await_line("LISTEN ");
  DCE_REQUIRE_OK(listen);
  const std::uint16_t port = parse_listen_port(*listen);
  DCE_REQUIRE(port != 0);

  // The sites must be running so that the coordinator observes the fleet the
  // plan was written against: a plan is only ever validated against an observed
  // fleet, and that observation is itself durable state.
  std::vector<std::unique_ptr<Child>> sites;
  for (std::size_t index = 0; index < scenario.fleet.profiles.size(); ++index) {
    const std::string store =
        dce::platform::join_path(scenario.root, "restart-store-" + std::to_string(index));
    dce::Result<std::unique_ptr<Child>> node =
        Child::start(cli, {"site", "serve", "--store", store, "--profile",
                           scenario.profile_paths[index], "--coordinator-port",
                           std::to_string(port), "--delegated", "--rounds", "2000"});
    DCE_REQUIRE_OK(node);
    dce::Result<std::string> ready = (*node)->await_line("SITE ");
    DCE_REQUIRE_OK(ready);
    sites.push_back(std::move(*node));
  }

  dce::Result<std::unique_ptr<dce::AdminClient>> client = connect_admin(port);
  DCE_REQUIRE_OK(client);
  DCE_REQUIRE_OK(submit(**client, scenario.fleet.plan));
  dce::Result<dce::wire::AdminResponse> first_validation = validate(**client);
  DCE_REQUIRE_OK(first_validation);
  DCE_CHECK_TRUE(first_validation->report.has_value());
  DCE_CHECK_TRUE(first_validation->report->accepted());
  dce::Result<dce::wire::AdminResponse> before = status_of(**client);
  DCE_REQUIRE_OK(before);
  const std::uint64_t epoch_before = before->epoch.value();

  // A hard kill: no graceful shutdown, no flush, no chance to tidy up.
  DCE_REQUIRE((*coordinator)->terminate().ok());
  dce::Result<int> exit_code = (*coordinator)->wait();
  DCE_CHECK_TRUE(exit_code.ok());
  coordinator->reset();

  dce::Result<std::unique_ptr<Child>> restarted = Child::start(
      cli, {"coordinator", "serve", "--store", coordinator_store, "--port", "0"});
  DCE_REQUIRE_OK(restarted);
  dce::Result<std::string> listen_again = (*restarted)->await_line("LISTEN ");
  DCE_REQUIRE_OK(listen_again);
  const std::uint16_t port_again = parse_listen_port(*listen_again);
  DCE_REQUIRE(port_again != 0);

  dce::Result<std::unique_ptr<dce::AdminClient>> client_again = connect_admin(port_again);
  DCE_REQUIRE_OK(client_again);
  dce::Result<dce::wire::AdminResponse> after = status_of(**client_again);
  DCE_REQUIRE_OK(after);
  DCE_CHECK_TRUE(after->epoch.value() > epoch_before);
  DCE_CHECK_TRUE(after->plan_digest == before->plan_digest);

  // The recovered plan was written under the previous epoch, so it is fenced.
  dce::Result<dce::wire::AdminResponse> fenced = validate(**client_again);
  DCE_REQUIRE_OK(fenced);
  DCE_CHECK_TRUE(fenced->report.has_value());
  DCE_CHECK_TRUE(!fenced->report->accepted());
  bool stale_epoch = false;
  for (const dce::Refusal& refusal : fenced->report->refusals) {
    if (refusal.code == dce::RefusalCode::stale_plan_epoch) {
      stale_epoch = true;
    }
  }
  DCE_CHECK_TRUE(stale_epoch);

  // Re-affirming the plan keeps the durable progress and takes the new epoch.
  dce::Result<dce::wire::AdminResponse> reaffirmed = submit(**client_again, scenario.fleet.plan);
  DCE_REQUIRE_OK(reaffirmed);
  DCE_CHECK_EQ(reaffirmed->status, dce::ErrorCode::ok);
  DCE_CHECK_TRUE(reaffirmed->plan_generation.value() > 1);
  dce::Result<dce::wire::AdminResponse> reconfirmed = validate(**client_again);
  DCE_REQUIRE_OK(reconfirmed);
  DCE_CHECK_TRUE(reconfirmed->report.has_value());
  DCE_CHECK_TRUE(reconfirmed->report->accepted());
  if (reconfirmed->report.has_value() && !reconfirmed->report->accepted()) {
    DCE_FAIL("the re-affirmed plan was refused: " +
             std::string(dce::to_string(reconfirmed->report->refusals.front().code)) + ": " +
             reconfirmed->report->refusals.front().explanation);
  }
  // The restored observation is what makes the re-affirmed plan valid again.
  DCE_CHECK_TRUE(after->sites == before->sites);

  for (std::unique_ptr<Child>& site : sites) {
    DCE_REQUIRE(site->terminate().ok());
  }
  sites.clear();
  DCE_REQUIRE((*restarted)->terminate().ok());
  restarted->reset();
  DCE_CHECK_TRUE(dce::platform::remove_tree(scenario.root).ok());
}

DCE_TEST(multiprocess, a_stopped_site_is_reconciled_when_it_returns) {
  stage("test 3: a stopped site is reconciled when it returns");
  Scenario scenario;
  DCE_REQUIRE(prepare(scenario, 3, 2, 2, 123));
  const std::string cli = find_cli();
  DCE_REQUIRE(!cli.empty());

  const std::string coordinator_store = dce::platform::join_path(scenario.root, "coordinator-partition");
  dce::Result<std::unique_ptr<Child>> coordinator = Child::start(
      cli, {"coordinator", "serve", "--store", coordinator_store, "--port", "0"});
  DCE_REQUIRE_OK(coordinator);
  dce::Result<std::string> listen = (*coordinator)->await_line("LISTEN ");
  DCE_REQUIRE_OK(listen);
  const std::uint16_t port = parse_listen_port(*listen);
  DCE_REQUIRE(port != 0);

  // Two sites run to completion; the third runs for a single round and stops,
  // which is exactly a site that was partitioned away while the fleet moved on.
  std::vector<std::unique_ptr<Child>> sites;
  for (std::size_t index = 0; index < scenario.fleet.profiles.size(); ++index) {
    const std::string store = dce::platform::join_path(
        scenario.root, "partition-store-" + std::to_string(index));
    const std::string rounds = index == 0 ? "1" : "500";
    dce::Result<std::unique_ptr<Child>> node = Child::start(
        cli, {"site", "serve", "--store", store, "--profile",
                       scenario.profile_paths[index], "--coordinator-port",
                       std::to_string(port), "--delegated", "--rounds", rounds});
    DCE_REQUIRE_OK(node);
    dce::Result<std::string> ready = (*node)->await_line("SITE ");
    DCE_REQUIRE_OK(ready);
    sites.push_back(std::move(*node));
  }
  // The one-round site exits on its own.
  dce::Result<int> stopped = sites.front()->wait();
  DCE_CHECK_TRUE(stopped.ok());
  sites.erase(sites.begin());

  dce::Result<std::unique_ptr<dce::AdminClient>> client = connect_admin(port);
  DCE_REQUIRE_OK(client);
  DCE_REQUIRE_OK(submit(**client, scenario.fleet.plan));
  DCE_REQUIRE_OK(lifecycle(**client, dce::LifecycleEvent::validate, true));
  DCE_REQUIRE_OK(lifecycle(**client, dce::LifecycleEvent::stage, true));
  DCE_REQUIRE_OK(lifecycle(**client, dce::LifecycleEvent::begin_rollout, true));

  std::uint64_t complete = 0;
  const bool partial = await_completion(**client, 3, 200, complete);
  DCE_CHECK_TRUE(!partial);
  DCE_CHECK_TRUE(complete < 3);

  // The site returns. Its stored stage is behind the plan's intent, and the
  // coordinator reconciles rather than assuming it kept up.
  const std::string returning_store = dce::platform::join_path(scenario.root, "partition-store-0");
  dce::Result<std::unique_ptr<Child>> returned = Child::start(
      cli, {"site", "serve", "--store", returning_store, "--profile",
                     scenario.profile_paths.front(), "--coordinator-port",
                     std::to_string(port), "--delegated", "--rounds", "500"});
  DCE_REQUIRE_OK(returned);
  dce::Result<std::string> ready_again = (*returned)->await_line("SITE ");
  DCE_REQUIRE_OK(ready_again);

  const bool finished = await_completion(**client, 3, 2000, complete);
  DCE_CHECK_TRUE(finished);
  if (!finished) {
    DCE_FAIL("the returning site never caught up: " + std::to_string(complete) + " of 3 complete");
  }

  DCE_REQUIRE((*returned)->terminate().ok());
  returned->reset();
  for (std::unique_ptr<Child>& site : sites) {
    DCE_REQUIRE(site->terminate().ok());
  }
  sites.clear();
  DCE_REQUIRE((*coordinator)->terminate().ok());
  coordinator->reset();
  DCE_CHECK_TRUE(dce::platform::remove_tree(scenario.root).ok());
}
