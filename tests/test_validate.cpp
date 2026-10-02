// Validation is where the boundary decides whether a plan may proceed. Every
// case below breaks exactly one thing and asserts the exact refusal code, so a
// regression in any single check fails a single named test.
#include <algorithm>
#include <string>
#include <vector>

#include "dce/codec.hpp"
#include "dce/synthetic.hpp"
#include "dce/validate.hpp"
#include "harness.hpp"

namespace {

using dce::CohortWave;
using dce::ErrorCode;
using dce::EvolutionPlan;
using dce::RefusalCode;
using dce::SiteObservation;
using dce::ValidationContext;
using dce::ValidationReport;

std::vector<SiteObservation> observations_from(const EvolutionPlan& plan) {
  std::vector<SiteObservation> observations;
  for (const dce::SiteRecord& record : plan.membership.sites) {
    SiteObservation observation;
    observation.site = record.id;
    observation.dccp_version = record.dccp_version;
    observation.components = record.components;
    observation.accepted_generation = record.accepted_generation;
    observation.capabilities = record.capabilities;
    observation.policy = record.policy;
    observation.state_digest = record.state_digest;
    observation.delegated_rollout_authority = record.delegated_rollout_authority;
    observations.push_back(std::move(observation));
  }
  return observations;
}

// Restores the derived fields so that a deliberately introduced defect is the
// only thing wrong with the plan, and the refusal under test is the refusal we
// actually reach.
void reseal(EvolutionPlan& plan, std::vector<SiteObservation>& observations) {
  observations = observations_from(plan);
  const dce::Result<dce::Digest256> digest = dce::compute_observation_digest(observations);
  if (digest.ok()) {
    plan.source_state_digest = *digest;
  }
  (void)dce::canonicalize(plan);
}

ValidationContext context_for(const EvolutionPlan& plan,
                              const std::vector<SiteObservation>& observations) {
  ValidationContext context;
  context.epoch = plan.identity.epoch;
  context.current_generation = plan.identity.generation;
  context.observed_sites = observations;
  return context;
}

bool refuses(const ValidationReport& report, RefusalCode code) {
  for (const dce::Refusal& refusal : report.refusals) {
    if (refusal.code == code) {
      return true;
    }
  }
  return false;
}

struct Fleet {
  dce::SyntheticFleet fleet;
  std::vector<SiteObservation> observations;
};

bool build(Fleet& out, std::size_t sites, std::size_t components, std::size_t cohorts,
           std::uint64_t seed) {
  dce::SyntheticFleetOptions options;
  options.sites = sites;
  options.components = components;
  options.cohorts = cohorts;
  options.seed = seed;
  dce::Result<dce::SyntheticFleet> built = dce::make_synthetic_fleet(options);
  if (!built.ok()) {
    return false;
  }
  out.fleet = std::move(*built);
  out.observations = out.fleet.initial_observations;
  return true;
}

std::string first_refusal(const ValidationReport& report) {
  if (report.refusals.empty()) {
    return "no refusal";
  }
  return std::string(dce::to_string(report.refusals.front().code)) + ": " +
         report.refusals.front().explanation;
}

}  // namespace

DCE_TEST(validate, synthetic_fleets_are_accepted) {
  const std::uint64_t seeds[] = {1, 2, 3, 7, 11, 20260101};
  for (const std::uint64_t seed : seeds) {
    Fleet fleet;
    DCE_REQUIRE(build(fleet, 6, 2, 2, seed));
    const ValidationContext context = context_for(fleet.fleet.plan, fleet.observations);
    const dce::Result<ValidationReport> report = dce::validate_plan(fleet.fleet.plan, context);
    DCE_REQUIRE_OK(report);
    DCE_CHECK_TRUE(report->accepted());
    if (!report->accepted()) {
      DCE_FAIL("seed " + std::to_string(seed) + " refused: " + first_refusal(*report));
    }
    DCE_CHECK_EQ(report->refusals.size(), static_cast<std::size_t>(0));
    DCE_CHECK_EQ(report->checked_sites, static_cast<std::uint64_t>(6));
    DCE_CHECK_TRUE(report->checked_edges > 0);
  }
}

DCE_TEST(validate, larger_shapes_are_accepted) {
  Fleet fleet;
  DCE_REQUIRE(build(fleet, 64, 4, 4, 99));
  const ValidationContext context = context_for(fleet.fleet.plan, fleet.observations);
  const dce::Result<ValidationReport> report = dce::validate_plan(fleet.fleet.plan, context);
  DCE_REQUIRE_OK(report);
  DCE_CHECK_TRUE(report->accepted());
  if (!report->accepted()) {
    DCE_FAIL(first_refusal(*report));
  }
}

DCE_TEST(validate, is_deterministic) {
  Fleet fleet;
  DCE_REQUIRE(build(fleet, 12, 3, 3, 5));
  const ValidationContext context = context_for(fleet.fleet.plan, fleet.observations);
  const dce::Result<ValidationReport> first = dce::validate_plan(fleet.fleet.plan, context);
  const dce::Result<ValidationReport> second = dce::validate_plan(fleet.fleet.plan, context);
  DCE_REQUIRE_OK(first);
  DCE_REQUIRE_OK(second);
  DCE_CHECK_EQ(first->refusals.size(), second->refusals.size());
  DCE_CHECK_TRUE(first->plan_digest == second->plan_digest);
  DCE_CHECK_TRUE(first->observed_state_digest == second->observed_state_digest);
}

DCE_TEST(validate, refuses_a_tampered_plan_digest) {
  Fleet fleet;
  DCE_REQUIRE(build(fleet, 4, 2, 2, 1));
  EvolutionPlan plan = fleet.fleet.plan;
  plan.title = "a title that was changed after the digest was computed";
  const ValidationContext context = context_for(plan, fleet.observations);
  const dce::Result<ValidationReport> report = dce::validate_plan(plan, context);
  DCE_REQUIRE_OK(report);
  DCE_CHECK_TRUE(refuses(*report, RefusalCode::structural_defect));
}

DCE_TEST(validate, refuses_a_stale_epoch) {
  Fleet fleet;
  DCE_REQUIRE(build(fleet, 4, 2, 2, 1));
  ValidationContext context = context_for(fleet.fleet.plan, fleet.observations);
  context.epoch = dce::CoordinatorEpoch::from_value(context.epoch.value() + 1);
  const dce::Result<ValidationReport> report = dce::validate_plan(fleet.fleet.plan, context);
  DCE_REQUIRE_OK(report);
  DCE_CHECK_TRUE(refuses(*report, RefusalCode::stale_plan_epoch));
  DCE_CHECK_TRUE(!report->accepted());
}

DCE_TEST(validate, refuses_a_fenced_generation) {
  Fleet fleet;
  DCE_REQUIRE(build(fleet, 4, 2, 2, 1));
  ValidationContext context = context_for(fleet.fleet.plan, fleet.observations);
  context.current_generation =
      dce::PlanGeneration::from_value(context.current_generation.value() + 1);
  const dce::Result<ValidationReport> report = dce::validate_plan(fleet.fleet.plan, context);
  DCE_REQUIRE_OK(report);
  DCE_CHECK_TRUE(refuses(*report, RefusalCode::stale_plan_generation));
}

DCE_TEST(validate, refuses_when_the_observed_fleet_changed) {
  Fleet fleet;
  DCE_REQUIRE(build(fleet, 4, 2, 2, 1));
  std::vector<SiteObservation> observations = fleet.observations;
  observations.front().state_digest = dce::Digest256::of("a different state");
  // The plan's recorded source state no longer describes the fleet.
  const ValidationContext context = context_for(fleet.fleet.plan, observations);
  const dce::Result<ValidationReport> report = dce::validate_plan(fleet.fleet.plan, context);
  DCE_REQUIRE_OK(report);
  DCE_CHECK_TRUE(refuses(*report, RefusalCode::source_state_stale));
}

DCE_TEST(validate, refuses_an_unobserved_site) {
  Fleet fleet;
  DCE_REQUIRE(build(fleet, 4, 2, 2, 1));
  std::vector<SiteObservation> observations = fleet.observations;
  observations.pop_back();
  const ValidationContext context = context_for(fleet.fleet.plan, observations);
  const dce::Result<ValidationReport> report = dce::validate_plan(fleet.fleet.plan, context);
  DCE_REQUIRE_OK(report);
  DCE_CHECK_TRUE(refuses(*report, RefusalCode::source_state_stale));
  DCE_CHECK_TRUE(refuses(*report, RefusalCode::site_not_observed));
}

DCE_TEST(validate, refuses_an_uncovered_site) {
  Fleet fleet;
  DCE_REQUIRE(build(fleet, 4, 2, 2, 1));
  std::vector<SiteObservation> observations = fleet.observations;
  observations.push_back(observations.front());
  observations.back().site = *dce::SiteId::parse("site-extra");
  ValidationContext context = context_for(fleet.fleet.plan, observations);
  // Keep the fleet digest honest so that the uncovered-site check is reached.
  const dce::Result<dce::Digest256> digest = dce::compute_observation_digest(observations);
  DCE_REQUIRE_OK(digest);
  EvolutionPlan plan = fleet.fleet.plan;
  plan.source_state_digest = *digest;
  DCE_REQUIRE_OK(dce::canonicalize(plan));
  context.epoch = plan.identity.epoch;
  context.current_generation = plan.identity.generation;
  const dce::Result<ValidationReport> report = dce::validate_plan(plan, context);
  DCE_REQUIRE_OK(report);
  DCE_CHECK_TRUE(refuses(*report, RefusalCode::uncovered_site));
}

DCE_TEST(validate, refuses_a_generation_version_state_or_component_mismatch) {
  Fleet fleet;
  DCE_REQUIRE(build(fleet, 4, 2, 2, 1));
  {
    std::vector<SiteObservation> observations = fleet.observations;
    observations.front().accepted_generation = dce::SiteGeneration::from_value(9);
    const ValidationContext context = context_for(fleet.fleet.plan, observations);
    const dce::Result<ValidationReport> report = dce::validate_plan(fleet.fleet.plan, context);
    DCE_REQUIRE_OK(report);
    DCE_CHECK_TRUE(refuses(*report, RefusalCode::source_state_stale));
    DCE_CHECK_TRUE(refuses(*report, RefusalCode::site_generation_mismatch));
  }
  {
    std::vector<SiteObservation> observations = fleet.observations;
    observations.front().dccp_version = dce::Version{9, 9, 9};
    const ValidationContext context = context_for(fleet.fleet.plan, observations);
    const dce::Result<ValidationReport> report = dce::validate_plan(fleet.fleet.plan, context);
    DCE_REQUIRE_OK(report);
    DCE_CHECK_TRUE(refuses(*report, RefusalCode::site_version_mismatch));
  }
  {
    std::vector<SiteObservation> observations = fleet.observations;
    observations.front().state_digest = dce::Digest256::of("other");
    const ValidationContext context = context_for(fleet.fleet.plan, observations);
    const dce::Result<ValidationReport> report = dce::validate_plan(fleet.fleet.plan, context);
    DCE_REQUIRE_OK(report);
    DCE_CHECK_TRUE(refuses(*report, RefusalCode::site_state_mismatch));
  }
  {
    std::vector<SiteObservation> observations = fleet.observations;
    observations.front().components.clear();
    const ValidationContext context = context_for(fleet.fleet.plan, observations);
    const dce::Result<ValidationReport> report = dce::validate_plan(fleet.fleet.plan, context);
    DCE_REQUIRE_OK(report);
    DCE_CHECK_TRUE(refuses(*report, RefusalCode::site_component_mismatch));
  }
}

DCE_TEST(validate, refuses_a_target_that_is_not_forward) {
  Fleet fleet;
  DCE_REQUIRE(build(fleet, 4, 2, 2, 1));
  EvolutionPlan plan = fleet.fleet.plan;
  plan.target_version = plan.source_version;
  std::vector<SiteObservation> observations;
  reseal(plan, observations);
  const ValidationContext context = context_for(plan, observations);
  const dce::Result<ValidationReport> report = dce::validate_plan(plan, context);
  DCE_REQUIRE_OK(report);
  DCE_CHECK_TRUE(refuses(*report, RefusalCode::target_not_forward));
}

DCE_TEST(validate, refuses_an_undescribed_component_version) {
  Fleet fleet;
  DCE_REQUIRE(build(fleet, 4, 2, 2, 1));
  EvolutionPlan plan = fleet.fleet.plan;
  dce::CapabilityMatrix reduced;
  for (const dce::ComponentCapabilities& entry : plan.capabilities.entries()) {
    DCE_REQUIRE_OK(reduced.add(entry));
  }
  // Remove the declaration for the version the first site actually runs.
  DCE_REQUIRE(!plan.membership.sites.empty());
  DCE_REQUIRE(!plan.membership.sites.front().components.empty());
  const dce::ComponentVersion running = plan.membership.sites.front().components.front();
  plan.capabilities = dce::CapabilityMatrix{};
  for (const dce::ComponentCapabilities& entry : reduced.entries()) {
    if (entry.component == running.component && entry.version == running.version) {
      continue;
    }
    DCE_REQUIRE_OK(plan.capabilities.add(entry));
  }
  std::vector<SiteObservation> observations;
  reseal(plan, observations);
  const ValidationContext context = context_for(plan, observations);
  const dce::Result<ValidationReport> report = dce::validate_plan(plan, context);
  DCE_REQUIRE_OK(report);
  DCE_CHECK_TRUE(refuses(*report, RefusalCode::component_version_undescribed));
}

DCE_TEST(validate, refuses_a_missing_upgrade_path_and_missing_provenance) {
  Fleet fleet;
  DCE_REQUIRE(build(fleet, 4, 2, 2, 1));
  {
    EvolutionPlan plan = fleet.fleet.plan;
    dce::CompatibilityGraph reduced;
    for (const dce::ComponentVersion& node : plan.compatibility.nodes()) {
      DCE_REQUIRE_OK(reduced.add_node(node));
    }
    // Drop every edge so no route to the target exists at all.
    reduced.canonicalize();
    plan.compatibility = std::move(reduced);
    std::vector<SiteObservation> observations;
    reseal(plan, observations);
    const ValidationContext context = context_for(plan, observations);
    const dce::Result<ValidationReport> report = dce::validate_plan(plan, context);
    DCE_REQUIRE_OK(report);
    DCE_CHECK_TRUE(refuses(*report, RefusalCode::unknown_component) ||
                   refuses(*report, RefusalCode::no_upgrade_path));
  }
  {
    EvolutionPlan plan = fleet.fleet.plan;
    // Keep the edges but remove every piece of evidence, so the only route has
    // no provenance.
    plan.evidence = dce::EvidenceSet{};
    std::vector<SiteObservation> observations;
    reseal(plan, observations);
    const ValidationContext context = context_for(plan, observations);
    const dce::Result<ValidationReport> report = dce::validate_plan(plan, context);
    DCE_REQUIRE_OK(report);
    DCE_CHECK_TRUE(refuses(*report, RefusalCode::provenance_missing) ||
                   refuses(*report, RefusalCode::evidence_missing));
  }
}

DCE_TEST(validate, refuses_a_gate_with_no_evidence) {
  Fleet fleet;
  DCE_REQUIRE(build(fleet, 4, 2, 2, 1));
  EvolutionPlan plan = fleet.fleet.plan;
  dce::Gate gate;
  gate.id = *dce::GateId::parse("gate.health.plan");
  gate.kind = dce::GateKind::health;
  gate.scope = dce::GateScopeKind::plan;
  gate.description = "a health gate that nothing satisfies";
  gate.required_evidence.push_back(dce::EvidenceKind::health_observation);
  gate.blocking = true;
  DCE_REQUIRE_OK(plan.gates.add(std::move(gate)));
  std::vector<SiteObservation> observations;
  reseal(plan, observations);
  const ValidationContext context = context_for(plan, observations);
  const dce::Result<ValidationReport> report = dce::validate_plan(plan, context);
  DCE_REQUIRE_OK(report);
  DCE_CHECK_TRUE(refuses(*report, RefusalCode::evidence_missing));
  DCE_CHECK_TRUE(!report->accepted());
  bool found_failed_gate = false;
  for (const dce::GateEvaluation& evaluation : report->gates) {
    if (evaluation.gate == *dce::GateId::parse("gate.health.plan") &&
        evaluation.outcome == dce::GateOutcome::failed) {
      found_failed_gate = true;
    }
  }
  DCE_CHECK_TRUE(found_failed_gate);
}

DCE_TEST(validate, distinguishes_stale_evidence_from_future_evidence) {
  Fleet fleet;
  DCE_REQUIRE(build(fleet, 4, 2, 2, 1));
  {
    // An epoch-scoped record from before the revision is stale.
    EvolutionPlan plan = fleet.fleet.plan;
    plan.identity.epoch = dce::CoordinatorEpoch::from_value(5);
    dce::EvidenceSet rebuilt;
    for (const dce::EvidenceRecord& record : plan.evidence.entries()) {
      dce::EvidenceRecord scoped = record;
      scoped.observed_epoch = dce::CoordinatorEpoch::from_value(1);
      DCE_REQUIRE_OK(rebuilt.add(std::move(scoped)));
    }
    plan.evidence = std::move(rebuilt);
    std::vector<SiteObservation> observations;
    reseal(plan, observations);
    ValidationContext context = context_for(plan, observations);
    context.epoch = dce::CoordinatorEpoch::from_value(5);
    const dce::Result<ValidationReport> report = dce::validate_plan(plan, context);
    DCE_REQUIRE_OK(report);
    DCE_CHECK_TRUE(refuses(*report, RefusalCode::evidence_stale));
  }
  {
    // A record from an epoch the coordinator has not reached is indeterminate.
    EvolutionPlan plan = fleet.fleet.plan;
    dce::EvidenceSet rebuilt;
    for (const dce::EvidenceRecord& record : plan.evidence.entries()) {
      dce::EvidenceRecord scoped = record;
      scoped.observed_epoch = dce::CoordinatorEpoch::from_value(50);
      DCE_REQUIRE_OK(rebuilt.add(std::move(scoped)));
    }
    plan.evidence = std::move(rebuilt);
    std::vector<SiteObservation> observations;
    reseal(plan, observations);
    const ValidationContext context = context_for(plan, observations);
    const dce::Result<ValidationReport> report = dce::validate_plan(plan, context);
    DCE_REQUIRE_OK(report);
    DCE_CHECK_TRUE(refuses(*report, RefusalCode::gate_indeterminate));
  }
}

DCE_TEST(validate, refuses_policy_generations_that_are_too_low_or_unknown) {
  Fleet fleet;
  DCE_REQUIRE(build(fleet, 4, 2, 2, 1));
  EvolutionPlan plan = fleet.fleet.plan;
  plan.policy.minimum_ordinal = 50;
  std::vector<SiteObservation> observations;
  reseal(plan, observations);
  const ValidationContext context = context_for(plan, observations);
  const dce::Result<ValidationReport> low = dce::validate_plan(plan, context);
  DCE_REQUIRE_OK(low);
  DCE_CHECK_TRUE(refuses(*low, RefusalCode::policy_generation_too_low));

  std::vector<SiteObservation> unknown = observations;
  for (SiteObservation& observation : unknown) {
    observation.policy.reset();
  }
  ValidationContext unknown_context = context_for(plan, unknown);
  const dce::Result<ValidationReport> report = dce::validate_plan(plan, unknown_context);
  DCE_REQUIRE_OK(report);
  DCE_CHECK_TRUE(refuses(*report, RefusalCode::policy_generation_unknown));
}

DCE_TEST(validate, refuses_dependencies_that_were_never_observed) {
  Fleet fleet;
  DCE_REQUIRE(build(fleet, 4, 2, 2, 1));
  EvolutionPlan plan = fleet.fleet.plan;
  for (dce::SiteRecord& site : plan.membership.sites) {
    for (dce::DependencyCompatibilityRef& dependency : site.dependencies) {
      dependency.observed = false;
    }
  }
  std::vector<SiteObservation> observations;
  reseal(plan, observations);
  const ValidationContext context = context_for(plan, observations);
  const dce::Result<ValidationReport> report = dce::validate_plan(plan, context);
  DCE_REQUIRE_OK(report);
  DCE_CHECK_TRUE(refuses(*report, RefusalCode::dependency_indeterminate));
}

DCE_TEST(validate, refuses_dependencies_out_of_range_or_missing_capabilities) {
  Fleet fleet;
  DCE_REQUIRE(build(fleet, 4, 2, 2, 1));
  {
    EvolutionPlan plan = fleet.fleet.plan;
    for (dce::SiteRecord& site : plan.membership.sites) {
      for (dce::DependencyCompatibilityRef& dependency : site.dependencies) {
        dependency.observed_version = dce::Version{99, 0, 0};
      }
    }
    std::vector<SiteObservation> observations;
    reseal(plan, observations);
    const ValidationContext context = context_for(plan, observations);
    const dce::Result<ValidationReport> report = dce::validate_plan(plan, context);
    DCE_REQUIRE_OK(report);
    DCE_CHECK_TRUE(refuses(*report, RefusalCode::dependency_version_unsatisfied));
  }
  {
    EvolutionPlan plan = fleet.fleet.plan;
    for (dce::SiteRecord& site : plan.membership.sites) {
      for (dce::DependencyCompatibilityRef& dependency : site.dependencies) {
        dependency.observed_capabilities = dce::CapabilitySet{};
      }
    }
    std::vector<SiteObservation> observations;
    reseal(plan, observations);
    const ValidationContext context = context_for(plan, observations);
    const dce::Result<ValidationReport> report = dce::validate_plan(plan, context);
    DCE_REQUIRE_OK(report);
    DCE_CHECK_TRUE(refuses(*report, RefusalCode::dependency_capability_missing));
  }
}

DCE_TEST(validate, refuses_capability_removal_without_proof) {
  Fleet fleet;
  DCE_REQUIRE(build(fleet, 4, 2, 2, 1));
  const dce::CapabilityId capability = *dce::CapabilityId::parse("cap.0.0");

  {
    EvolutionPlan plan = fleet.fleet.plan;
    dce::DeprecationGate deprecation;
    deprecation.gate = *dce::GateId::parse("gate.retire.cap00");
    deprecation.capability = capability;
    plan.deprecations.push_back(deprecation);
    dce::Gate gate;
    gate.id = deprecation.gate;
    gate.kind = dce::GateKind::capability_removal;
    gate.scope = dce::GateScopeKind::plan;
    gate.description = "retire the capability once nothing depends on it";
    DCE_REQUIRE_OK(plan.gates.add(std::move(gate)));
    std::vector<SiteObservation> observations;
    reseal(plan, observations);
    ValidationContext context = context_for(plan, observations);
    const dce::Result<ValidationReport> report = dce::validate_plan(plan, context);
    DCE_REQUIRE_OK(report);
    DCE_CHECK_TRUE(refuses(*report, RefusalCode::capability_removal_unproven));
  }
  {
    // With a proof that names remaining dependents, the refusal is the
    // dependents rather than the absence of a proof.
    EvolutionPlan plan = fleet.fleet.plan;
    dce::DeprecationGate deprecation;
    deprecation.gate = *dce::GateId::parse("gate.retire.cap00");
    deprecation.capability = capability;
    plan.deprecations.push_back(deprecation);
    dce::Gate gate;
    gate.id = deprecation.gate;
    gate.kind = dce::GateKind::capability_removal;
    gate.scope = dce::GateScopeKind::plan;
    gate.description = "retire the capability once nothing depends on it";
    DCE_REQUIRE_OK(plan.gates.add(std::move(gate)));
    std::vector<SiteObservation> observations;
    reseal(plan, observations);

    dce::CapabilityDependencyProof proof;
    proof.capability = capability;
    proof.remaining_dependents.push_back(plan.membership.sites.front().id);
    proof.provenance = *dce::EvidenceId::parse("ev.removal.cap00");
    proof.observed_epoch = plan.identity.epoch;
    ValidationContext context = context_for(plan, observations);
    context.removal_proofs.push_back(proof);
    const dce::Result<ValidationReport> report = dce::validate_plan(plan, context);
    DCE_REQUIRE_OK(report);
    DCE_CHECK_TRUE(refuses(*report, RefusalCode::capability_removal_dependents_remain));
  }
  {
    // A proof with nothing outstanding is accepted.
    EvolutionPlan plan = fleet.fleet.plan;
    dce::DeprecationGate deprecation;
    deprecation.gate = *dce::GateId::parse("gate.retire.cap00");
    deprecation.capability = capability;
    plan.deprecations.push_back(deprecation);
    dce::Gate gate;
    gate.id = deprecation.gate;
    gate.kind = dce::GateKind::capability_removal;
    gate.scope = dce::GateScopeKind::plan;
    gate.description = "retire the capability once nothing depends on it";
    DCE_REQUIRE_OK(plan.gates.add(std::move(gate)));
    std::vector<SiteObservation> observations;
    reseal(plan, observations);

    dce::CapabilityDependencyProof proof;
    proof.capability = capability;
    proof.provenance = *dce::EvidenceId::parse("ev.removal.cap00");
    proof.observed_epoch = plan.identity.epoch;
    ValidationContext context = context_for(plan, observations);
    context.removal_proofs.push_back(proof);
    const dce::Result<ValidationReport> report = dce::validate_plan(plan, context);
    DCE_REQUIRE_OK(report);
    DCE_CHECK_TRUE(!refuses(*report, RefusalCode::capability_removal_unproven));
    DCE_CHECK_TRUE(!refuses(*report, RefusalCode::capability_removal_dependents_remain));
  }
}

DCE_TEST(validate, refuses_broken_cohorts) {
  Fleet fleet;
  DCE_REQUIRE(build(fleet, 4, 2, 2, 1));
  {
    EvolutionPlan plan = fleet.fleet.plan;
    plan.cohorts.front().sites.clear();
    std::vector<SiteObservation> observations;
    reseal(plan, observations);
    const ValidationContext context = context_for(plan, observations);
    const dce::Result<ValidationReport> report = dce::validate_plan(plan, context);
    DCE_REQUIRE_OK(report);
    DCE_CHECK_TRUE(refuses(*report, RefusalCode::cohort_empty));
    DCE_CHECK_TRUE(refuses(*report, RefusalCode::uncovered_site));
  }
  {
    EvolutionPlan plan = fleet.fleet.plan;
    plan.cohorts.back().wave = CohortWave::from_value(7);
    std::vector<SiteObservation> observations;
    reseal(plan, observations);
    const ValidationContext context = context_for(plan, observations);
    const dce::Result<ValidationReport> report = dce::validate_plan(plan, context);
    DCE_REQUIRE_OK(report);
    DCE_CHECK_TRUE(refuses(*report, RefusalCode::cohort_skips_wave));
  }
}

DCE_TEST(validate, refuses_unjustified_irreversibility_and_impossible_rollback) {
  Fleet fleet;
  DCE_REQUIRE(build(fleet, 4, 2, 2, 1));
  {
    EvolutionPlan plan = fleet.fleet.plan;
    DCE_REQUIRE(plan.point_of_no_return.has_value());
    plan.point_of_no_return->justification.clear();
    std::vector<SiteObservation> observations;
    reseal(plan, observations);
    const ValidationContext context = context_for(plan, observations);
    const dce::Result<ValidationReport> report = dce::validate_plan(plan, context);
    DCE_REQUIRE_OK(report);
    DCE_CHECK_TRUE(refuses(*report, RefusalCode::irreversibility_unjustified));
  }
  {
    EvolutionPlan plan = fleet.fleet.plan;
    plan.rollback.permitted = true;
    plan.rollback.max_waves_back = dce::CohortWave{};
    DCE_REQUIRE(plan.cohorts.size() > 1);
    std::vector<SiteObservation> observations;
    reseal(plan, observations);
    const ValidationContext context = context_for(plan, observations);
    const dce::Result<ValidationReport> report = dce::validate_plan(plan, context);
    DCE_REQUIRE_OK(report);
    DCE_CHECK_TRUE(refuses(*report, RefusalCode::rollback_impossible));
  }
}

DCE_TEST(validate, refuses_an_exception_that_reaches_too_far) {
  Fleet fleet;
  DCE_REQUIRE(build(fleet, 4, 2, 2, 1));
  {
    EvolutionPlan plan = fleet.fleet.plan;
    dce::ExceptionGrant grant;
    grant.id = *dce::ExceptionId::parse("exception.1");
    grant.waived_gate = plan.gates.entries().front().id;
    grant.granted_by = *dce::ActorId::parse("operator");
    grant.justification = "attempting to waive a compatibility proof";
    grant.epoch = plan.identity.epoch;
    plan.exceptions.push_back(grant);
    std::vector<SiteObservation> observations;
    reseal(plan, observations);
    const ValidationContext context = context_for(plan, observations);
    const dce::Result<ValidationReport> report = dce::validate_plan(plan, context);
    DCE_REQUIRE_OK(report);
    DCE_CHECK_TRUE(refuses(*report, RefusalCode::exception_out_of_scope));
  }
  {
    EvolutionPlan plan = fleet.fleet.plan;
    dce::ExceptionGrant grant;
    grant.id = *dce::ExceptionId::parse("exception.2");
    grant.waived_gate = plan.gates.entries().front().id;
    grant.granted_by = *dce::ActorId::parse("operator");
    grant.justification = "attempting to waive an irreversible boundary";
    grant.epoch = plan.identity.epoch;
    grant.requests_irreversible = true;
    plan.exceptions.push_back(grant);
    std::vector<SiteObservation> observations;
    reseal(plan, observations);
    const ValidationContext context = context_for(plan, observations);
    const dce::Result<ValidationReport> report = dce::validate_plan(plan, context);
    DCE_REQUIRE_OK(report);
    DCE_CHECK_TRUE(refuses(*report, RefusalCode::exception_requests_irreversible));
  }
}

DCE_TEST(validate, refuses_a_split_brain_fleet) {
  // Four versions of one component exist, of which the first and the last
  // cannot interoperate. Two observed sites sit on those two versions and every
  // step that could move either of them is removed, so nothing in the plan can
  // ever resolve the split.
  dce::SyntheticFleetOptions options;
  options.sites = 6;
  options.components = 1;
  options.cohorts = 2;
  options.versions_per_component = 4;
  options.seed = 3;
  dce::Result<dce::SyntheticFleet> built = dce::make_synthetic_fleet(options);
  DCE_REQUIRE_OK(built);
  EvolutionPlan plan = built->plan;

  const dce::ComponentId component = plan.compatibility.nodes().front().component;
  std::vector<dce::Version> versions;
  for (const dce::ComponentVersion& node : plan.compatibility.nodes()) {
    if (node.component == component) {
      versions.push_back(node.version);
    }
  }
  std::sort(versions.begin(), versions.end());
  DCE_REQUIRE(versions.size() >= 3);
  const dce::Version lowest = versions.front();
  const dce::Version highest = versions.back();
  DCE_CHECK_EQ(plan.capabilities.interoperate(component, lowest, highest).outcome,
               dce::InteropOutcome::incompatible);

  const dce::Result<dce::ComponentCapabilities> low_entry =
      plan.capabilities.lookup(component, lowest);
  const dce::Result<dce::ComponentCapabilities> high_entry =
      plan.capabilities.lookup(component, highest);
  DCE_REQUIRE_OK(low_entry);
  DCE_REQUIRE_OK(high_entry);

  std::vector<SiteObservation> observations = built->initial_observations;
  DCE_REQUIRE(observations.size() >= 2);
  for (SiteObservation& observation : observations) {
    observation.components = {dce::ComponentVersion{component, lowest}};
    observation.capabilities = low_entry->provides;
  }
  observations.front().components = {dce::ComponentVersion{component, highest}};
  observations.front().capabilities = high_entry->provides;
  for (SiteObservation& observation : observations) {
    observation.state_digest = dce::Digest256::of(observation.site.str());
  }

  // Removing every step is what makes the split unresolvable.
  plan.steps.clear();
  plan.rollback.irreversible_steps.clear();
  plan.point_of_no_return.reset();
  for (const SiteObservation& observation : observations) {
    dce::SiteRecord* record = nullptr;
    for (dce::SiteRecord& candidate : plan.membership.sites) {
      if (candidate.id == observation.site) {
        record = &candidate;
      }
    }
    DCE_REQUIRE(record != nullptr);
    record->components = observation.components;
    record->capabilities = observation.capabilities;
    record->state_digest = observation.state_digest;
  }
  const dce::Result<dce::Digest256> digest = dce::compute_observation_digest(observations);
  DCE_REQUIRE_OK(digest);
  plan.source_state_digest = *digest;
  DCE_REQUIRE_OK(dce::canonicalize(plan));

  ValidationContext context = context_for(plan, observations);
  const dce::Result<ValidationReport> report = dce::validate_plan(plan, context);
  DCE_REQUIRE_OK(report);
  DCE_CHECK_TRUE(refuses(*report, RefusalCode::split_brain));
  if (!refuses(*report, RefusalCode::split_brain)) {
    DCE_FAIL("expected a split_brain refusal, got: " + first_refusal(*report));
  }
}
