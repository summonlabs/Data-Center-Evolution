// Plan validation: the authoritative answer to "may this plan proceed?".
//
// Validation is evidence-driven and never inferred. A plan is accepted only
// when the fleet it was written against is still the fleet that exists, every
// gate is satisfied by evidence that is present and current, every step has an
// evidenced and capability-complete route, every stated dependency on a
// neighbouring boundary was observed explicitly, and no capability is removed
// while something still depends on it.
//
// A refusal is always explainable: it names the subject and says why.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "dce/capability.hpp"
#include "dce/evidence.hpp"
#include "dce/ids.hpp"
#include "dce/limits.hpp"
#include "dce/plan.hpp"
#include "dce/status.hpp"

namespace dce {

enum class RefusalCode : std::uint16_t {
  structural_defect,
  stale_plan_epoch,
  stale_plan_generation,
  source_state_stale,
  site_not_observed,
  site_generation_mismatch,
  site_version_mismatch,
  site_state_mismatch,
  site_component_mismatch,
  uncovered_site,
  target_not_forward,
  unknown_component,
  component_version_undescribed,
  no_upgrade_path,
  missing_intermediate_capability,
  downgrade_incompatible,
  provenance_missing,
  gate_unsatisfied,
  gate_indeterminate,
  evidence_missing,
  evidence_stale,
  policy_generation_too_low,
  policy_generation_mismatch,
  policy_generation_unknown,
  dependency_indeterminate,
  dependency_version_unsatisfied,
  dependency_capability_missing,
  capability_removal_unproven,
  capability_removal_dependents_remain,
  mixed_version_not_interoperable,
  split_brain,
  irreversibility_unjustified,
  rollback_impossible,
  cohort_skips_wave,
  cohort_empty,
  exception_requests_irreversible,
  exception_out_of_scope,
  step_cycle,
};

[[nodiscard]] const char* to_string(RefusalCode code) noexcept;

struct Refusal {
  RefusalCode code{RefusalCode::structural_defect};
  std::string subject;
  std::string explanation;

  auto operator<=>(const Refusal&) const = default;
};

// What the coordinator actually observes right now, consumed from the site
// runtimes rather than inferred from the plan.
struct SiteObservation {
  SiteId site;
  Version dccp_version;
  std::vector<ComponentVersion> components;
  SiteGeneration accepted_generation;
  CapabilitySet capabilities;
  // Absent when the site did not report a policy generation: that is unknown,
  // which is not the same as a generation that is too low.
  std::optional<PolicyGeneration> policy;
  Digest256 state_digest;
  bool delegated_rollout_authority{false};

  auto operator<=>(const SiteObservation&) const = default;
};

struct ValidationContext {
  CoordinatorEpoch epoch;
  PlanGeneration current_generation;
  std::vector<SiteObservation> observed_sites;
  std::vector<CapabilityDependencyProof> removal_proofs;

  [[nodiscard]] const SiteObservation* find(const SiteId& id) const;
};

struct ValidationReport {
  Digest256 plan_digest;
  Digest256 observed_state_digest;
  std::vector<Refusal> refusals;
  std::vector<GateEvaluation> gates;
  std::vector<MigrationStep> planned_steps;
  std::vector<RolloutCohort> planned_cohorts;
  std::uint64_t checked_sites{0};
  std::uint64_t checked_steps{0};
  std::uint64_t checked_edges{0};
  std::uint64_t checked_gates{0};

  [[nodiscard]] bool accepted() const noexcept { return refusals.empty(); }
};

[[nodiscard]] Result<ValidationReport> validate_plan(const EvolutionPlan& plan,
                                                     const ValidationContext& context);

// The digest of an observed fleet, used to detect a plan written against a
// fleet that no longer exists.
[[nodiscard]] Result<Digest256> compute_observation_digest(
    const std::vector<SiteObservation>& sites);

}  // namespace dce
