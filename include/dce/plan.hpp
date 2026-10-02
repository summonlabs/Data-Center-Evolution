// The evolution plan: the single authoritative object this boundary owns.
//
// A plan is a statement about how a specific fleet, observed at a specific
// generation, moves from one control-plane version to another without a global
// shutdown. It is not a deployment script: it governs eligibility, proof,
// ordering and rollback, and delegates the mechanism of any individual runtime
// upgrade to the runtime that owns it.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "dce/capability.hpp"
#include "dce/compatibility.hpp"
#include "dce/digest.hpp"
#include "dce/evidence.hpp"
#include "dce/ids.hpp"
#include "dce/limits.hpp"
#include "dce/status.hpp"
#include "dce/version.hpp"

namespace dce {

enum class Irreversibility : std::uint8_t {
  reversible,
  irreversible_after_commit,  // crossing it retires rollback for this step
};

[[nodiscard]] const char* to_string(Irreversibility value) noexcept;

enum class RolloutStrategy : std::uint8_t {
  sequential,  // one site at a time within the cohort
  parallel,    // up to max_parallel sites at once
  canary,      // a deliberately small first subset, gated on observation
};

[[nodiscard]] const char* to_string(RolloutStrategy strategy) noexcept;

struct MigrationStep {
  StepId id;
  ComponentId component;
  Version from;
  Version to;
  TransitionKind kind{TransitionKind::upgrade};
  std::vector<StepId> depends_on;
  std::vector<GateId> gates;
  Irreversibility irreversibility{Irreversibility::reversible};
  bool idempotent{true};
  EvidenceId provenance;

  auto operator<=>(const MigrationStep&) const = default;
};

struct RolloutCohort {
  CohortId id;
  CohortWave wave;
  RolloutStrategy strategy{RolloutStrategy::sequential};
  std::uint32_t max_parallel{1};
  std::vector<SiteId> sites;
  std::vector<GateId> gates;
  bool canary{false};

  auto operator<=>(const RolloutCohort&) const = default;
};

struct SiteRecord {
  SiteId id;
  Version dccp_version;
  // The component versions this site actually runs. Compatibility is decided
  // per component, so the fleet snapshot must carry them rather than an
  // inferred single version.
  std::vector<ComponentVersion> components;
  SiteGeneration accepted_generation;
  CapabilitySet capabilities;
  PolicyGeneration policy;
  std::vector<DependencyCompatibilityRef> dependencies;
  std::vector<ObligationId> obligations;
  Digest256 state_digest;
  bool delegated_rollout_authority{false};

  auto operator<=>(const SiteRecord&) const = default;
};

struct SiteMembershipSnapshot {
  std::vector<SiteRecord> sites;
  Digest256 digest;

  // Digest over the canonically ordered site records. Two snapshots that
  // describe the same fleet in a different order are the same snapshot.
  [[nodiscard]] Result<Digest256> compute_digest() const;
  void canonicalize();
};

struct Precondition {
  std::string name;
  std::string description;
  GateId gate;

  auto operator<=>(const Precondition&) const = default;
};

// The explicit point after which rollback stops being available. Crossing it
// is a recorded event, not an inference.
struct PointOfNoReturn {
  CohortWave wave;
  std::optional<StepId> step;
  std::string justification;

  auto operator<=>(const PointOfNoReturn&) const = default;
};

struct RollbackPolicy {
  bool permitted{true};
  CohortWave max_waves_back;
  std::vector<StepId> irreversible_steps;

  auto operator<=>(const RollbackPolicy&) const = default;
};

// A capability may only be removed, or its compatibility path deprecated,
// once nothing still depends on it. The gate carries the proof; the runtime
// checks the proof rather than the assertion.
struct DeprecationGate {
  GateId gate;
  CapabilityId capability;
  std::optional<Version> removal_version;
  EvidenceId provenance;

  auto operator<=>(const DeprecationGate&) const = default;
};

struct ExceptionGrant {
  ExceptionId id;
  GateId waived_gate;
  ActorId granted_by;
  std::string justification;
  CoordinatorEpoch epoch;
  // An exception may never waive a point of no return or an irreversible step;
  // this field exists so that a request attempting it is refused explicitly
  // rather than silently ignored.
  bool requests_irreversible{false};

  auto operator<=>(const ExceptionGrant&) const = default;
};

struct PlanIdentity {
  EvolutionPlanId id;
  PlanRevision revision;
  PlanGeneration generation;
  CoordinatorEpoch epoch;

  auto operator<=>(const PlanIdentity&) const = default;
};

struct EvolutionPlan {
  PlanIdentity identity;
  std::string title;
  Version source_version;
  Version target_version;
  Digest256 source_state_digest;
  SiteMembershipSnapshot membership;
  CapabilityMatrix capabilities;
  CompatibilityGraph compatibility;
  std::vector<MigrationStep> steps;
  std::vector<RolloutCohort> cohorts;
  std::vector<Precondition> preconditions;
  GateSet gates;
  std::optional<PointOfNoReturn> point_of_no_return;
  RollbackPolicy rollback;
  std::vector<DeprecationGate> deprecations;
  std::vector<ExceptionGrant> exceptions;
  EvidenceSet evidence;
  // The policy generation the target components require. Policy semantics are
  // owned elsewhere; this boundary only requires that the generation is high
  // enough and says so explicitly when it is not.
  PolicyRequirement policy;
  Digest256 digest;
};

// Canonical order for every collection in the plan, then the plan digest.
// Hash order must never depend on insertion order.
[[nodiscard]] Status canonicalize(EvolutionPlan& plan);

// Structural checks that do not need any external authority: collection
// bounds, duplicate identities, and dangling references between the plan's own
// parts. Returns the first structural problem found.
[[nodiscard]] Status structural_check(const EvolutionPlan& plan);

// Digest over the canonical encoding of the plan with the digest field itself
// treated as zero, so that the digest is stable and self-consistent.
[[nodiscard]] Result<Digest256> compute_plan_digest(const EvolutionPlan& plan);

// The identity of the fleet state the plan was written against.
[[nodiscard]] Result<Digest256> compute_membership_digest(const SiteMembershipSnapshot& snapshot);

[[nodiscard]] const MigrationStep* find_step(const EvolutionPlan& plan, const StepId& id);
[[nodiscard]] const RolloutCohort* find_cohort(const EvolutionPlan& plan, const CohortId& id);
[[nodiscard]] const SiteRecord* find_site(const EvolutionPlan& plan, const SiteId& id);

}  // namespace dce
