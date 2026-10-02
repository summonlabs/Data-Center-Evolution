// A downstream consumer of the installed DataCenterEvolution package.
//
// The CMakeLists.txt next to this file is a standalone project: it calls
// find_package(DataCenterEvolution CONFIG REQUIRED) and links the namespaced
// imported target DataCenterEvolution::dce, so everything below uses installed
// public headers only. Every check exits non-zero with a clear failure line.

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <utility>

#include "dce/capability.hpp"
#include "dce/digest.hpp"
#include "dce/ids.hpp"
#include "dce/plan.hpp"
#include "dce/rollout.hpp"
#include "dce/status.hpp"
#include "dce/synthetic.hpp"
#include "dce/text.hpp"
#include "dce/validate.hpp"
#include "dce/version.hpp"

#ifndef DCE_CONSUMER_EXPECTED_VERSION
#error "the consumer build must define DCE_CONSUMER_EXPECTED_VERSION"
#endif

static_assert(sizeof(DCE_CONSUMER_EXPECTED_VERSION) > 1,
              "the installed package must report a version of its own");

namespace {

[[nodiscard]] int fail(const std::string& message) {
  std::cerr << "downstream_consumer: FAILED: " << message << '\n';
  return EXIT_FAILURE;
}

// dce::Status and dce::Result<T> share one outcome vocabulary.
template <class Outcome>
[[nodiscard]] int fail(const std::string& what, const Outcome& outcome) {
  return fail(what + ": " + dce::to_string(outcome.code()) + ": " + outcome.message());
}

int run() {
  // 1. The version is the one the installed CMake config reported, so this line
  //    cannot be right unless find_package(DataCenterEvolution CONFIG) worked.
  std::cout << "downstream_consumer: DataCenterEvolution " << DCE_CONSUMER_EXPECTED_VERSION << '\n';

  // 2. A small plan, canonicalised twice: the digest must be stable.
  dce::SyntheticFleetOptions options;
  options.sites = 3;
  options.components = 1;
  options.cohorts = 1;
  options.seed = 7;
  auto fleet_result = dce::make_synthetic_fleet(options);
  if (!fleet_result.ok()) return fail("make_synthetic_fleet", fleet_result);
  dce::SyntheticFleet fleet = std::move(fleet_result).value();
  dce::EvolutionPlan& plan = fleet.plan;

  if (!dce::canonicalize(plan).ok()) return fail("canonicalize");
  auto first_digest = dce::compute_plan_digest(plan);
  if (!first_digest.ok()) return fail("compute_plan_digest", first_digest);
  if (!dce::canonicalize(plan).ok()) return fail("canonicalize (second pass)");
  auto second_digest = dce::compute_plan_digest(plan);
  if (!second_digest.ok()) return fail("compute_plan_digest (second pass)", second_digest);
  std::cout << "plan digest: " << second_digest.value().to_hex() << '\n';
  if (!(first_digest.value() == second_digest.value())) {
    return fail("canonicalising the same plan twice produced two different digests");
  }
  if (!(second_digest.value() == plan.digest)) {
    return fail("the plan digest does not summarise the plan it is attached to");
  }

  // 3. Validation against the fleet the plan was written against.
  dce::ValidationContext context;
  context.epoch = plan.identity.epoch;
  context.current_generation = plan.identity.generation;
  context.observed_sites = fleet.initial_observations;
  auto report_result = dce::validate_plan(plan, context);
  if (!report_result.ok()) return fail("validate_plan", report_result);
  const dce::ValidationReport& report = report_result.value();
  if (!report.accepted()) {
    for (const dce::Refusal& refusal : report.refusals) {
      std::cout << "refused: " << dce::to_string(refusal.code) << " ["
                << dce::text::escape_for_output(refusal.subject)
                << "]: " << dce::text::escape_for_output(refusal.explanation) << '\n';
    }
    return fail("the plan was refused by validation");
  }
  std::cout << "validation: accepted\n";

  // 4. Behaviour that can only come from the linked library.
  auto range_result = dce::VersionRange::parse("1.0.0..2.0.0");
  if (!range_result.ok()) return fail("VersionRange::parse", range_result);
  const dce::Version probe{1, 5, 0};
  const bool in_range = range_result.value().contains(probe);
  std::cout << "version range " << range_result.value().to_string() << " contains 1.5.0: "
            << (in_range ? "yes" : "no") << '\n';
  if (!in_range) return fail("the parsed version range does not contain 1.5.0");

  dce::CapabilitySet capabilities;
  auto capability = dce::CapabilityId::parse("cap.consumer.probe");
  if (!capability.ok()) return fail("CapabilityId::parse", capability);
  if (!capabilities.insert(capability.value()).ok()) return fail("CapabilitySet::insert");
  const bool present = capabilities.contains(capability.value());
  std::cout << "capability set contains cap.consumer.probe: " << (present ? "yes" : "no") << '\n';
  if (!present) return fail("the capability set lost the capability it was given");

  auto rollout_result = dce::initial_rollout_state(plan);
  if (!rollout_result.ok()) return fail("initial_rollout_state", rollout_result);
  auto action_result = dce::next_rollout_action(plan, rollout_result.value());
  if (!action_result.ok()) return fail("next_rollout_action", action_result);
  const dce::RolloutAction& action = action_result.value();
  std::cout << "next rollout action: " << dce::to_string(action.kind) << " site=" << action.site.str()
            << " step=" << action.step.str() << " stage 0 -> "
            << dce::text::u32_to_string(action.target_stage.value()) << '\n';
  if (action.kind != dce::RolloutActionKind::advance_site) {
    return fail("the first rollout decision should advance a site");
  }
  if (action.target_stage.value() != 1U) {
    return fail("the first rollout decision must advance exactly one stage");
  }

  std::cout << "downstream_consumer: all checks passed\n";
  return EXIT_SUCCESS;
}

}  // namespace

int main() { return run(); }
