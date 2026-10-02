// dce - the Data Center Evolution command line interface.
//
// This tool is a real operator surface, not a demo: it creates and inspects
// plans, runs the coordinator or a site as a long-lived process, drives the
// lifecycle, and reports the authoritative state that the runtime actually
// holds. Every decision it reports comes from the runtime, never from the tool.
#include <algorithm>
#include <cstdint>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "dce/codec.hpp"
#include "dce/digest.hpp"
#include "dce/node.hpp"
#include "dce/plan.hpp"
#include "dce/platform.hpp"
#include "dce/protocol.hpp"
#include "dce/synthetic.hpp"
#include "dce/text.hpp"
#include "dce/validate.hpp"

namespace {

using dce::Error;
using dce::ErrorCode;
using dce::Result;
using dce::Status;

int fail(const std::string& message) {
  std::cout << "dce: " << message << "\n";
  return 1;
}

int fail(const Error& error) {
  std::cout << "dce: " << dce::to_string(error.code) << ": " << error.message << "\n";
  return 1;
}

// A deliberately small flag reader: every option is --name value, and an
// unknown option is an error rather than being ignored.
class Arguments {
 public:
  Arguments(int argc, char** argv, int first) {
    for (int index = first; index < argc; ++index) {
      std::string token = argv[index];
      if (token.rfind("--", 0) == 0) {
        std::string name = token.substr(2);
        std::optional<std::string> value;
        if (index + 1 < argc) {
          std::string next = argv[index + 1];
          if (next.rfind("--", 0) != 0) {
            value = next;
            ++index;
          }
        }
        options_.push_back(Option{std::move(name), std::move(value)});
      } else {
        positionals_.push_back(std::move(token));
      }
    }
  }

  [[nodiscard]] bool has(std::string_view name) const {
    for (const Option& option : options_) {
      if (option.name == name) {
        return true;
      }
    }
    return false;
  }

  [[nodiscard]] std::optional<std::string> value(std::string_view name) const {
    for (const Option& option : options_) {
      if (option.name == name) {
        return option.value;
      }
    }
    return std::nullopt;
  }

  [[nodiscard]] std::string required(std::string_view name, Status& status) const {
    const std::optional<std::string> found = value(name);
    if (!found.has_value()) {
      status = Error{ErrorCode::invalid_argument,
                     std::string("--") + std::string(name) + " is required"};
      return {};
    }
    status = Status{};
    return *found;
  }

  [[nodiscard]] std::uint64_t number(std::string_view name, std::uint64_t fallback) const {
    const std::optional<std::string> found = value(name);
    if (!found.has_value()) {
      return fallback;
    }
    Result<std::uint64_t> parsed = dce::text::parse_u64(*found);
    return parsed.ok() ? *parsed : fallback;
  }

  [[nodiscard]] const std::vector<std::string>& positionals() const { return positionals_; }

 private:
  struct Option {
    std::string name;
    std::optional<std::string> value;
  };
  std::vector<Option> options_;
  std::vector<std::string> positionals_;
};

Result<std::vector<std::byte>> read_file(const std::string& path) {
  DCE_ASSIGN(file, dce::platform::File::open(path, dce::platform::OpenMode::read_only));
  DCE_ASSIGN(size, file.size());
  if (size > dce::kMaxCanonicalBytes) {
    return Error{ErrorCode::limit_exceeded, "file exceeds the documented canonical bound"};
  }
  std::vector<std::byte> bytes(static_cast<std::size_t>(size));
  if (!bytes.empty()) {
    DCE_ASSIGN(read, file.read_at(0, bytes));
    if (read != bytes.size()) {
      return Error{ErrorCode::io_error, "file ended before its declared size"};
    }
  }
  return bytes;
}

Status write_file(const std::string& path, std::span<const std::byte> bytes) {
  DCE_ASSIGN(file, dce::platform::File::open(path, dce::platform::OpenMode::read_write));
  DCE_TRY(file.write_at(0, bytes));
  DCE_TRY(file.truncate(bytes.size()));
  return file.sync();
}

Result<dce::EvolutionPlan> load_plan(const std::string& path) {
  DCE_ASSIGN(bytes, read_file(path));
  dce::CanonicalReader reader(bytes);
  DCE_ASSIGN(plan, dce::codec::get<dce::EvolutionPlan>(reader));
  DCE_TRY(reader.expect_end());
  DCE_TRY(dce::canonicalize(plan));
  return plan;
}

Result<dce::SiteRecord> load_profile(const std::string& path) {
  DCE_ASSIGN(bytes, read_file(path));
  dce::CanonicalReader reader(bytes);
  DCE_ASSIGN(record, dce::codec::get<dce::SiteRecord>(reader));
  DCE_TRY(reader.expect_end());
  return record;
}

Result<dce::SiteObservation> load_observation(const std::string& path) {
  DCE_ASSIGN(record, load_profile(path));
  dce::SiteObservation observation;
  observation.site = record.id;
  observation.dccp_version = record.dccp_version;
  observation.components = record.components;
  observation.accepted_generation = record.accepted_generation;
  observation.capabilities = record.capabilities;
  observation.policy = record.policy;
  observation.state_digest = record.state_digest;
  observation.delegated_rollout_authority = record.delegated_rollout_authority;
  return observation;
}

void print_plan_summary(const dce::EvolutionPlan& plan) {
  std::cout << "plan            " << plan.identity.id.str() << "\n";
  std::cout << "generation      " << plan.identity.generation.value() << "\n";
  std::cout << "revision        " << plan.identity.revision.value() << "\n";
  std::cout << "epoch           " << plan.identity.epoch.value() << "\n";
  std::cout << "title           " << plan.title << "\n";
  std::cout << "source          " << plan.source_version.to_string() << "\n";
  std::cout << "target          " << plan.target_version.to_string() << "\n";
  std::cout << "digest          " << plan.digest.to_hex() << "\n";
  std::cout << "sites           " << plan.membership.sites.size() << "\n";
  for (const dce::SiteRecord& site : plan.membership.sites) {
    std::cout << "  site " << site.id.str() << " dccp " << site.dccp_version.to_string()
              << " generation " << site.accepted_generation.value() << " components";
    for (const dce::ComponentVersion& component : site.components) {
      std::cout << " " << component.component.str() << "@" << component.version.to_string();
    }
    std::cout << "\n";
  }
  std::cout << "steps           " << plan.steps.size() << "\n";
  for (const dce::MigrationStep& step : plan.steps) {
    std::cout << "  step " << step.id.str() << " " << step.component.str() << " "
              << step.from.to_string() << " -> " << step.to.to_string() << " "
              << dce::to_string(step.kind) << " " << dce::to_string(step.irreversibility) << "\n";
  }
  std::cout << "cohorts         " << plan.cohorts.size() << "\n";
  for (const dce::RolloutCohort& cohort : plan.cohorts) {
    std::cout << "  wave " << cohort.wave.value() << " " << cohort.id.str() << " "
              << dce::to_string(cohort.strategy) << " sites";
    for (const dce::SiteId& site : cohort.sites) {
      std::cout << " " << site.str();
    }
    std::cout << "\n";
  }
  std::cout << "gates           " << plan.gates.size() << "\n";
  for (const dce::Gate& gate : plan.gates.entries()) {
    std::cout << "  gate " << gate.id.str() << " " << dce::to_string(gate.kind) << " "
              << dce::to_string(gate.scope) << (gate.blocking ? " blocking" : " advisory") << "\n";
  }
  std::cout << "evidence        " << plan.evidence.size() << "\n";
  std::cout << "point of no return "
            << (plan.point_of_no_return.has_value() ? "declared at wave " +
                                                          std::to_string(
                                                              plan.point_of_no_return->wave.value())
                                                    : std::string("none"))
            << "\n";
  std::cout << "rollback        " << (plan.rollback.permitted ? "permitted" : "forbidden")
            << " max waves back " << plan.rollback.max_waves_back.value() << "\n";
}

void print_validation(const dce::ValidationReport& report) {
  std::cout << "verdict         " << (report.accepted() ? "accepted" : "refused") << "\n";
  std::cout << "plan digest     " << report.plan_digest.to_hex() << "\n";
  std::cout << "observed digest " << report.observed_state_digest.to_hex() << "\n";
  std::cout << "checked         sites=" << report.checked_sites << " steps=" << report.checked_steps
            << " edges=" << report.checked_edges << " gates=" << report.checked_gates << "\n";
  for (const dce::GateEvaluation& gate : report.gates) {
    std::cout << "gate " << gate.gate.str() << " " << dce::to_string(gate.outcome) << " "
              << gate.explanation << "\n";
  }
  for (const dce::Refusal& refusal : report.refusals) {
    std::cout << "refusal " << dce::to_string(refusal.code) << " [" << refusal.subject
              << "] " << refusal.explanation << "\n";
  }
}

const char* usage() {
  return R"(usage: dce <group> <command> [options]

  dce version
  dce coordinator serve --store DIR [--port N]
  dce site serve --store DIR --profile FILE --coordinator-port N [--delegated] [--rounds N]
  dce synthetic --seed S [--sites N] [--components N] [--cohorts N] [--versions N]
                --plan-out FILE --profiles-out DIR
  dce plan show --plan FILE
  dce plan digest --plan FILE
  dce plan submit --port N --plan FILE
  dce plan validate --port N
  dce status --port N
  dce observe --port N --profiles DIR
  dce event --port N --event NAME [--gates-satisfied] [--all-cohorts] [--rollback-eligible]
  dce rollback-eligibility --port N --wave W
)";
}

int command_synthetic(const Arguments& args) {
  Status status;
  const std::string plan_out = args.required("plan-out", status);
  if (!status.ok()) {
    return fail(status.error());
  }
  const std::string profiles_out = args.required("profiles-out", status);
  if (!status.ok()) {
    return fail(status.error());
  }

  dce::SyntheticFleetOptions options;
  options.seed = args.number("seed", 1);
  options.sites = static_cast<std::size_t>(args.number("sites", 8));
  options.components = static_cast<std::size_t>(args.number("components", 3));
  options.cohorts = static_cast<std::size_t>(args.number("cohorts", 2));
  options.versions_per_component = static_cast<std::size_t>(args.number("versions", 3));

  Result<dce::SyntheticFleet> fleet = dce::make_synthetic_fleet(options);
  if (!fleet.ok()) {
    return fail(fleet.error());
  }
  if (!dce::platform::make_directories(profiles_out).ok()) {
    return fail("cannot create the profile directory");
  }

  dce::CanonicalWriter writer;
  if (!dce::codec::put(writer, fleet->plan).ok()) {
    return fail("cannot encode the plan");
  }
  if (!write_file(plan_out, writer.bytes()).ok()) {
    return fail("cannot write the plan file");
  }
  for (const dce::SiteRecord& record : fleet->profiles) {
    dce::CanonicalWriter profile;
    if (!dce::codec::put(profile, record).ok()) {
      return fail("cannot encode a site profile");
    }
    const std::string path = dce::platform::join_path(profiles_out, record.id.str() + ".site");
    if (!write_file(path, profile.bytes()).ok()) {
      return fail("cannot write a site profile");
    }
  }

  std::cout << "plan            " << plan_out << "\n";
  std::cout << "profiles        " << profiles_out << " (" << fleet->profiles.size() << ")\n";
  std::cout << "digest          " << fleet->plan.digest.to_hex() << "\n";
  std::cout << "steps           " << fleet->plan.steps.size() << "\n";
  return 0;
}

int command_plan(const Arguments& args) {
  const std::vector<std::string>& positional = args.positionals();
  if (positional.empty()) {
    return fail("plan requires show, digest, submit or validate");
  }
  const std::string& action = positional.front();

  if (action == "show" || action == "digest") {
    Status status;
    const std::string path = args.required("plan", status);
    if (!status.ok()) {
      return fail(status.error());
    }
    Result<dce::EvolutionPlan> plan = load_plan(path);
    if (!plan.ok()) {
      return fail(plan.error());
    }
    if (action == "digest") {
      std::cout << plan->digest.to_hex() << "\n";
      return 0;
    }
    print_plan_summary(*plan);
    return 0;
  }

  Status status;
  const std::uint64_t port = args.number("port", 0);
  if (port == 0) {
    return fail("--port is required");
  }
  Result<std::unique_ptr<dce::AdminClient>> client = dce::AdminClient::connect(
      static_cast<std::uint16_t>(port));
  if (!client.ok()) {
    return fail(client.error());
  }

  if (action == "submit") {
    const std::string path = args.required("plan", status);
    if (!status.ok()) {
      return fail(status.error());
    }
    Result<dce::EvolutionPlan> plan = load_plan(path);
    if (!plan.ok()) {
      return fail(plan.error());
    }
    dce::wire::AdminRequest request;
    request.op = dce::wire::AdminOp::submit_plan;
    request.request_id = 1;
    request.plan_body = *plan;
    Result<dce::wire::AdminResponse> response = (*client)->call(request);
    if (!response.ok()) {
      return fail(response.error());
    }
    std::cout << dce::to_string(response->status) << ": " << response->text << "\n";
    return response->status == ErrorCode::ok ? 0 : 1;
  }

  if (action == "validate") {
    dce::wire::AdminRequest request;
    request.op = dce::wire::AdminOp::validate;
    request.request_id = 1;
    Result<dce::wire::AdminResponse> response = (*client)->call(request);
    if (!response.ok()) {
      return fail(response.error());
    }
    if (response->status != ErrorCode::ok || !response->report.has_value()) {
      std::cout << dce::to_string(response->status) << ": " << response->text << "\n";
      return 1;
    }
    print_validation(*response->report);
    return response->report->accepted() ? 0 : 1;
  }

  return fail("unknown plan command: " + action);
}

int command_status(const Arguments& args) {
  const std::uint64_t port = args.number("port", 0);
  if (port == 0) {
    return fail("--port is required");
  }
  Result<std::unique_ptr<dce::AdminClient>> client = dce::AdminClient::connect(
      static_cast<std::uint16_t>(port));
  if (!client.ok()) {
    return fail(client.error());
  }
  dce::wire::AdminRequest request;
  request.op = dce::wire::AdminOp::status;
  request.request_id = 1;
  Result<dce::wire::AdminResponse> response = (*client)->call(request);
  if (!response.ok()) {
    return fail(response.error());
  }
  if (response->status != ErrorCode::ok || !response->state.has_value()) {
    std::cout << "state           " << dce::to_string(response->status) << ": " << response->text
              << "\n";
    return 0;
  }
  std::cout << "state           " << dce::to_string(*response->state) << "\n";
  std::cout << "epoch           " << response->epoch.value() << "\n";
  std::cout << "generation      " << response->plan_generation.value() << "\n";
  std::cout << "plan digest     " << response->plan_digest.to_hex() << "\n";
  std::cout << "sites           " << response->sites << " complete " << response->sites_complete
            << "\n";
  std::cout << "receipts        " << response->receipts << "\n";
  std::cout << "checkpoints     " << response->checkpoints << "\n";
  std::cout << "reconciliations " << response->reconciliations << "\n";
  std::cout << "refusals        " << response->refusals << "\n";
  return 0;
}

int command_observe(const Arguments& args) {
  Status status;
  const std::uint64_t port = args.number("port", 0);
  if (port == 0) {
    return fail("--port is required");
  }
  const std::string profiles = args.required("profiles", status);
  if (!status.ok()) {
    return fail(status.error());
  }
  Result<std::vector<std::string>> entries = dce::platform::list_directory(profiles);
  if (!entries.ok()) {
    return fail(entries.error());
  }
  dce::ValidationContext context;
  for (const std::string& entry : *entries) {
    if (entry.size() < 5 || entry.compare(entry.size() - 5, 5, ".site") != 0) {
      continue;
    }
    Result<dce::SiteObservation> observation =
        load_observation(dce::platform::join_path(profiles, entry));
    if (!observation.ok()) {
      return fail(observation.error());
    }
    context.observed_sites.push_back(*observation);
  }
  if (context.observed_sites.empty()) {
    return fail("no .site profiles were found in " + profiles);
  }

  Result<std::unique_ptr<dce::AdminClient>> client = dce::AdminClient::connect(
      static_cast<std::uint16_t>(port));
  if (!client.ok()) {
    return fail(client.error());
  }
  dce::wire::AdminRequest request;
  request.op = dce::wire::AdminOp::observe;
  request.request_id = 1;
  request.context = context;
  Result<dce::wire::AdminResponse> response = (*client)->call(request);
  if (!response.ok()) {
    return fail(response.error());
  }
  std::cout << dce::to_string(response->status) << ": " << response->text << "\n";
  return response->status == ErrorCode::ok ? 0 : 1;
}

int command_event(const Arguments& args) {
  Status status;
  const std::uint64_t port = args.number("port", 0);
  if (port == 0) {
    return fail("--port is required");
  }
  const std::string name = args.required("event", status);
  if (!status.ok()) {
    return fail(status.error());
  }

  dce::LifecycleEvent event = dce::LifecycleEvent::validate;
  bool known = false;
  for (const dce::LegalTransition& candidate : dce::legal_transitions()) {
    if (std::string(dce::to_string(candidate.event)) == name) {
      event = candidate.event;
      known = true;
      break;
    }
  }
  if (!known) {
    return fail("unknown lifecycle event: " + name);
  }

  dce::wire::AdminRequest request;
  request.op = dce::wire::AdminOp::event;
  request.request_id = 1;
  request.event = event;
  request.transition.gates_satisfied = args.has("gates-satisfied");
  request.transition.all_cohorts_advanced = args.has("all-cohorts");
  request.transition.rollback_eligible = args.has("rollback-eligible");
  request.transition.point_of_no_return_crossed = args.has("point-of-no-return");
  request.transition.reconciliation_rewound = args.has("reconciled");

  Result<std::unique_ptr<dce::AdminClient>> client = dce::AdminClient::connect(
      static_cast<std::uint16_t>(port));
  if (!client.ok()) {
    return fail(client.error());
  }
  Result<dce::wire::AdminResponse> response = (*client)->call(request);
  if (!response.ok()) {
    return fail(response.error());
  }
  std::cout << dce::to_string(response->status) << ": " << response->text << "\n";
  return response->status == ErrorCode::ok ? 0 : 1;
}

int command_rollback(const Arguments& args) {
  const std::uint64_t port = args.number("port", 0);
  if (port == 0) {
    return fail("--port is required");
  }
  dce::wire::AdminRequest request;
  request.op = dce::wire::AdminOp::rollback_eligibility;
  request.request_id = 1;
  request.target_wave = dce::CohortWave{static_cast<std::uint32_t>(args.number("wave", 0))};
  Result<std::unique_ptr<dce::AdminClient>> client = dce::AdminClient::connect(
      static_cast<std::uint16_t>(port));
  if (!client.ok()) {
    return fail(client.error());
  }
  Result<dce::wire::AdminResponse> response = (*client)->call(request);
  if (!response.ok()) {
    return fail(response.error());
  }
  if (response->status != ErrorCode::ok || !response->rollback.has_value()) {
    std::cout << dce::to_string(response->status) << ": " << response->text << "\n";
    return 1;
  }
  std::cout << "eligible        " << (response->rollback->eligible ? "yes" : "no") << "\n";
  std::cout << "reason          " << response->rollback->explanation << "\n";
  return response->rollback->eligible ? 0 : 1;
}

int command_coordinator(const Arguments& args) {
  Status status;
  const std::string store = args.required("store", status);
  if (!status.ok()) {
    return fail(status.error());
  }
  dce::CoordinatorServerOptions options;
  options.coordinator.store_directory = store;
  options.port = static_cast<std::uint16_t>(args.number("port", 0));
  Result<std::unique_ptr<dce::CoordinatorServer>> server = dce::CoordinatorServer::start(options);
  if (!server.ok()) {
    return fail(server.error());
  }
  Result<std::uint16_t> port = (*server)->port();
  if (!port.ok()) {
    return fail(port.error());
  }
  std::cout << "LISTEN " << *port << "\n";
  std::cout.flush();
  const Status served = (*server)->serve();
  if (!served.ok()) {
    return fail(served.error());
  }
  return 0;
}

int command_site(const Arguments& args) {
  Status status;
  const std::string store = args.required("store", status);
  if (!status.ok()) {
    return fail(status.error());
  }
  const std::string profile_path = args.required("profile", status);
  if (!status.ok()) {
    return fail(status.error());
  }
  const std::uint64_t port = args.number("coordinator-port", 0);
  if (port == 0) {
    return fail("--coordinator-port is required");
  }
  Result<dce::SiteRecord> profile = load_profile(profile_path);
  if (!profile.ok()) {
    return fail(profile.error());
  }

  dce::SiteNodeOptions options;
  options.store_directory = store;
  options.profile = *profile;
  options.coordinator_port = static_cast<std::uint16_t>(port);
  options.accept_delegated_authority = args.has("delegated") ||
                                       profile->delegated_rollout_authority;
  options.exit_after_rounds = args.number("rounds", 0);

  Result<std::unique_ptr<dce::SiteNode>> node = dce::SiteNode::start(options);
  if (!node.ok()) {
    return fail(node.error());
  }
  const std::string site_name = profile->id.str();
  std::cout << "SITE " << site_name << " ready\n";
  std::cout.flush();
  const Status ran = (*node)->run();
  const dce::SiteNodeStatus final = (*node)->status();
  std::cout << "SITE " << site_name << " rounds " << final.rounds << " accepted "
            << final.accepted_stages << " refused " << final.refused_offers << " stage "
            << final.accepted_stage.value() << " generation "
            << final.accepted_generation.value() << "\n";
  if (!ran.ok()) {
    return fail(ran.error());
  }
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::cout << usage();
    return 2;
  }
  if (dce::platform::initialize_networking().code() != ErrorCode::ok) {
    return fail("cannot initialise platform networking");
  }

  const std::string group = argv[1];
  if (group == "version") {
    std::cout << "dce 1.0.1\n";
    return 0;
  }
  if (group == "--help" || group == "-h" || group == "help") {
    std::cout << usage();
    return 0;
  }

  Arguments args(argc, argv, 2);
  if (group == "coordinator") {
    if (!args.has("store")) {
      std::cout << usage();
      return 2;
    }
    return command_coordinator(args);
  }
  if (group == "site") {
    if (!args.has("store")) {
      std::cout << usage();
      return 2;
    }
    return command_site(args);
  }
  if (group == "synthetic") {
    return command_synthetic(args);
  }
  if (group == "plan") {
    return command_plan(args);
  }
  if (group == "status") {
    return command_status(args);
  }
  if (group == "observe") {
    return command_observe(args);
  }
  if (group == "event") {
    return command_event(args);
  }
  if (group == "rollback-eligibility") {
    return command_rollback(args);
  }

  std::cout << usage();
  return 2;
}
