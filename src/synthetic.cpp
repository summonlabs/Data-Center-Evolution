#include "dce/synthetic.hpp"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include "dce/codec.hpp"
#include "dce/random.hpp"
#include "dce/text.hpp"

namespace dce {
namespace {

[[nodiscard]] Result<ComponentId> component_id(std::size_t index) {
  return ComponentId::parse("comp-" + text::u64_to_string(index));
}

[[nodiscard]] Result<CapabilityId> capability_id(std::size_t component, std::size_t version) {
  return CapabilityId::parse("cap." + text::u64_to_string(component) + "." +
                             text::u64_to_string(version));
}

[[nodiscard]] Result<SiteId> site_id(std::size_t index) {
  return SiteId::parse("site-" + text::u64_to_string(index));
}

[[nodiscard]] Result<StepId> step_id(std::size_t component, std::size_t version) {
  return StepId::parse("step.comp-" + text::u64_to_string(component) + "." +
                       text::u64_to_string(version));
}

[[nodiscard]] Result<EvidenceId> compatibility_evidence(std::size_t component,
                                                        std::size_t version) {
  return EvidenceId::parse("ev.compat." + text::u64_to_string(component) + "." +
                           text::u64_to_string(version));
}

[[nodiscard]] Result<GateId> cohort_gate(std::size_t wave) {
  return GateId::parse("gate.wave." + text::u64_to_string(wave));
}

[[nodiscard]] Result<CohortId> cohort_id(std::size_t wave) {
  return CohortId::parse("wave-" + text::u64_to_string(wave));
}

// The version a component has at step index. The last index is the plan target,
// so the graph always has exactly one sink per component.
[[nodiscard]] Version version_at(std::size_t index, std::size_t count,
                                 const Version& source, const Version& target) {
  if (index + 1 == count) {
    return target;
  }
  Version version;
  version.major = source.major;
  version.minor = source.minor + static_cast<std::uint32_t>(index);
  version.patch = 0;
  return version;
}

}  // namespace

Result<SyntheticFleet> make_synthetic_fleet(const SyntheticFleetOptions& options) {
  if (options.sites == 0) {
    return Error{ErrorCode::invalid_argument, "a synthetic fleet needs at least one site"};
  }
  if (options.components == 0) {
    return Error{ErrorCode::invalid_argument, "a synthetic fleet needs at least one component"};
  }
  if (options.versions_per_component < 2) {
    return Error{ErrorCode::invalid_argument,
                 "a synthetic component needs at least a source and a target version"};
  }
  if (options.cohorts == 0 || options.cohorts > options.sites) {
    return Error{ErrorCode::invalid_argument,
                 "a synthetic fleet needs a non-zero number of cohorts no larger than its site "
                 "count"};
  }
  if (options.sites > kMaxSites || options.components > kMaxComponents) {
    return Error{ErrorCode::limit_exceeded, "the requested fleet exceeds a documented bound"};
  }

  Version source = options.source_version;
  if (source.is_zero()) {
    source = Version{1, 0, 0};
  }
  Version target = options.target_version;
  if (target.is_zero()) {
    target = Version{2, 0, 0};
  }
  if (!(source < target)) {
    return Error{ErrorCode::invalid_argument, "the target version must be ahead of the source"};
  }

  SyntheticFleet fleet;
  EvolutionPlan& plan = fleet.plan;
  DCE_ASSIGN(plan_id, EvolutionPlanId::parse("plan.synthetic." + text::u64_to_string(options.seed)));
  plan.identity.id = plan_id;
  plan.identity.revision = PlanRevision{1};
  plan.identity.generation = PlanGeneration{1};
  plan.identity.epoch = CoordinatorEpoch{};
  plan.title = "synthetic evolution " + text::u64_to_string(options.seed);
  plan.source_version = source;
  plan.target_version = target;

  DeterministicRng rng(options.seed);

  // --- capability matrix and compatibility graph -------------------------
  for (std::size_t component = 0; component < options.components; ++component) {
    DCE_ASSIGN(component_name, component_id(component));
    for (std::size_t index = 0; index < options.versions_per_component; ++index) {
      const Version version = version_at(index, options.versions_per_component, source, target);

      ComponentCapabilities declared;
      declared.component = component_name;
      declared.version = version;
      // A version provides its own capability and every capability of the
      // versions below it, which is what makes a mixed-version window bounded.
      for (std::size_t earlier = 0; earlier <= index; ++earlier) {
        DCE_ASSIGN(capability, capability_id(component, earlier));
        DCE_TRY(declared.provides.insert(capability));
      }
      if (index > 0) {
        DCE_ASSIGN(prerequisite, capability_id(component, index - 1));
        DCE_TRY(declared.requires_capabilities.insert(prerequisite));
      }
      DCE_TRY(plan.capabilities.add(std::move(declared)));

      DCE_TRY(plan.compatibility.add_node(ComponentVersion{component_name, version}));

      if (index + 1 < options.versions_per_component) {
        const Version next = version_at(index + 1, options.versions_per_component, source, target);
        DCE_ASSIGN(evidence_id, compatibility_evidence(component, index));
        DCE_ASSIGN(prerequisite, capability_id(component, index));
        DCE_ASSIGN(provided, capability_id(component, index + 1));
        CompatEdge edge;
        edge.component = component_name;
        edge.from = version;
        edge.to = next;
        edge.kind = TransitionKind::upgrade;
        DCE_TRY(edge.requires_capabilities.insert(prerequisite));
        DCE_TRY(edge.provides_capabilities.insert(provided));
        edge.reversible = true;
        edge.provenance = evidence_id;
        edge.justification = "certified upgrade within the same DCCP generation";
        DCE_TRY(plan.compatibility.add_edge(edge));

        EvidenceRecord record;
        record.id = evidence_id;
        record.kind = EvidenceKind::compatibility_certification;
        record.claim = "upgrade " + version.to_string() + " -> " + next.to_string() +
                       " certified for " + component_name.str();
        CanonicalWriter subject;
        DCE_TRY(codec::put(subject, component_name));
        DCE_TRY(codec::put(subject, version));
        DCE_TRY(codec::put(subject, next));
        record.subject_digest = subject.digest();
        DCE_ASSIGN(author, ActorId::parse("evolution-author"));
        record.producer = author;
        record.generation = SiteGeneration{};
        record.observed_epoch = CoordinatorEpoch{};
        DCE_TRY(plan.evidence.add(std::move(record)));

        MigrationStep step;
        DCE_ASSIGN(identifier, step_id(component, index));
        step.id = identifier;
        step.component = component_name;
        step.from = version;
        step.to = next;
        step.kind = TransitionKind::upgrade;
        if (index > 0) {
          DCE_ASSIGN(previous, step_id(component, index - 1));
          step.depends_on.push_back(previous);
        }
        step.irreversibility = index + 2 == options.versions_per_component
                                   ? Irreversibility::irreversible_after_commit
                                   : Irreversibility::reversible;
        step.idempotent = true;
        step.provenance = evidence_id;
        plan.steps.push_back(std::move(step));
      }
    }
  }

  // --- sites --------------------------------------------------------------
  DCE_ASSIGN(policy_id, PolicyGenerationId::parse("policy.facility"));
  DCE_ASSIGN(dfi_reference, BoundaryRefId::parse("dfi.fabric.contract"));
  DCE_ASSIGN(asi_reference, BoundaryRefId::parse("asi.execution.contract"));
  DCE_ASSIGN(dfi_capability, CapabilityId::parse("cap.dfi.transport"));
  DCE_ASSIGN(asi_capability, CapabilityId::parse("cap.asi.scheduling"));
  DCE_ASSIGN(dfi_evidence, EvidenceId::parse("ev.dfi.attestation"));
  DCE_ASSIGN(asi_evidence, EvidenceId::parse("ev.asi.attestation"));

  for (std::size_t index = 0; index < options.sites; ++index) {
    DCE_ASSIGN(identifier, site_id(index));
    SiteRecord record;
    record.id = identifier;
    record.dccp_version = source;
    record.policy = PolicyGeneration{policy_id, 5};
    record.delegated_rollout_authority = options.delegate_authority;
    record.accepted_generation = SiteGeneration{};

    std::vector<std::size_t> chosen(options.components, 0);
    for (std::size_t component = 0; component < options.components; ++component) {
      const std::size_t span = options.versions_per_component - 1;
      chosen[component] = static_cast<std::size_t>(rng.uniform_below(span));
      DCE_ASSIGN(component_name, component_id(component));
      const Version version =
          version_at(chosen[component], options.versions_per_component, source, target);
      record.components.push_back(ComponentVersion{component_name, version});
      for (std::size_t earlier = 0; earlier <= chosen[component]; ++earlier) {
        DCE_ASSIGN(capability, capability_id(component, earlier));
        DCE_TRY(record.capabilities.insert(capability));
      }
    }

    DependencyCompatibilityRef dfi;
    dfi.reference = dfi_reference;
    dfi.boundary = "dfi";
    DCE_ASSIGN(dfi_range, VersionRange::parse("1.0.0..2.0.0"));
    dfi.supported = dfi_range;
    DCE_TRY(dfi.required.insert(dfi_capability));
    dfi.observed_version = Version{1, 4, 0};
    DCE_TRY(dfi.observed_capabilities.insert(dfi_capability));
    dfi.observed_generation = SiteGeneration{};
    dfi.provenance = dfi_evidence;
    dfi.observed = true;
    record.dependencies.push_back(std::move(dfi));

    DependencyCompatibilityRef asi;
    asi.reference = asi_reference;
    asi.boundary = "asi";
    DCE_ASSIGN(asi_range, VersionRange::parse("1.0.0..3.0.0"));
    asi.supported = asi_range;
    DCE_TRY(asi.required.insert(asi_capability));
    asi.observed_version = Version{2, 1, 0};
    DCE_TRY(asi.observed_capabilities.insert(asi_capability));
    asi.observed_generation = SiteGeneration{};
    asi.provenance = asi_evidence;
    asi.observed = true;
    record.dependencies.push_back(std::move(asi));

    CanonicalWriter state;
    DCE_TRY(codec::put(state, record.id));
    DCE_TRY(codec::put_list(state, record.components, kMaxComponents));
    DCE_TRY(codec::put(state, record.capabilities));
    record.state_digest = state.digest();

    fleet.profiles.push_back(record);
  }

  // --- cohorts and gates ---------------------------------------------------
  for (std::size_t wave = 0; wave < options.cohorts; ++wave) {
    DCE_ASSIGN(identifier, cohort_id(wave));
    RolloutCohort cohort;
    cohort.id = identifier;
    cohort.wave = CohortWave{static_cast<std::uint32_t>(wave)};
    cohort.strategy = wave == 0 && options.cohorts > 1 ? RolloutStrategy::canary
                                                       : RolloutStrategy::sequential;
    cohort.max_parallel = 1;
    cohort.canary = cohort.strategy == RolloutStrategy::canary;
    for (std::size_t index = wave; index < options.sites; index += options.cohorts) {
      cohort.sites.push_back(fleet.profiles[index].id);
    }
    for (std::size_t gate = 0; gate < options.gates_per_cohort; ++gate) {
      DCE_ASSIGN(gate_identifier, cohort_gate(wave));
      Gate declaration;
      declaration.id = gate_identifier;
      declaration.kind = GateKind::compatibility;
      declaration.scope = GateScopeKind::cohort;
      declaration.description =
          "wave " + text::u64_to_string(wave) + " requires certified compatibility";
      declaration.required_evidence.push_back(EvidenceKind::compatibility_certification);
      declaration.blocking = true;
      DCE_TRY(plan.gates.add(std::move(declaration)));
      cohort.gates.push_back(gate_identifier);
    }
    plan.cohorts.push_back(std::move(cohort));
  }

  // --- rollback policy and point of no return ------------------------------
  plan.rollback.permitted = true;
  plan.rollback.max_waves_back = CohortWave{static_cast<std::uint32_t>(options.cohorts)};
  PointOfNoReturn marker;
  marker.wave = CohortWave{static_cast<std::uint32_t>(options.cohorts - 1)};
  marker.justification =
      "the final wave retires the previous control-plane generation and cannot be undone";
  plan.point_of_no_return = marker;

  // --- membership and observations ----------------------------------------
  for (const SiteRecord& record : fleet.profiles) {
    plan.membership.sites.push_back(record);
    SiteObservation observation;
    observation.site = record.id;
    observation.dccp_version = record.dccp_version;
    observation.components = record.components;
    observation.accepted_generation = record.accepted_generation;
    observation.capabilities = record.capabilities;
    observation.policy = record.policy;
    observation.state_digest = record.state_digest;
    observation.delegated_rollout_authority = record.delegated_rollout_authority;
    fleet.initial_observations.push_back(std::move(observation));
  }

  DCE_TRY(canonicalize(plan));

  DCE_ASSIGN(observation_digest, compute_observation_digest(fleet.initial_observations));
  fleet.observation_digest = observation_digest;
  plan.source_state_digest = observation_digest;
  DCE_TRY(canonicalize(plan));
  return fleet;
}

Result<RolloutState> initial_rollout_state(const EvolutionPlan& plan) {
  RolloutState state;
  for (const SiteRecord& record : plan.membership.sites) {
    SiteProgress progress;
    progress.site = record.id;
    progress.generation = record.accepted_generation;
    for (const RolloutCohort& cohort : plan.cohorts) {
      for (const SiteId& member : cohort.sites) {
        if (member == record.id) {
          progress.cohort = cohort.id;
          progress.wave = cohort.wave;
        }
      }
    }
    state.sites.push_back(std::move(progress));
  }
  if (state.sites.size() > kMaxSites) {
    return Error{ErrorCode::limit_exceeded, "the rollout state exceeds its documented bound"};
  }
  state.canonicalize();
  state.current_wave = plan.cohorts.empty() ? CohortWave{} : plan.cohorts.front().wave;
  return state;
}

}  // namespace dce
