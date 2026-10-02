#include "dce/validate.hpp"

#include <algorithm>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "dce/codec.hpp"
#include "dce/text.hpp"

namespace dce {
namespace {

const char* refusal_name(RefusalCode code) noexcept {
  switch (code) {
    case RefusalCode::structural_defect: return "structural_defect";
    case RefusalCode::stale_plan_epoch: return "stale_plan_epoch";
    case RefusalCode::stale_plan_generation: return "stale_plan_generation";
    case RefusalCode::source_state_stale: return "source_state_stale";
    case RefusalCode::site_not_observed: return "site_not_observed";
    case RefusalCode::site_generation_mismatch: return "site_generation_mismatch";
    case RefusalCode::site_version_mismatch: return "site_version_mismatch";
    case RefusalCode::site_state_mismatch: return "site_state_mismatch";
    case RefusalCode::site_component_mismatch: return "site_component_mismatch";
    case RefusalCode::uncovered_site: return "uncovered_site";
    case RefusalCode::target_not_forward: return "target_not_forward";
    case RefusalCode::unknown_component: return "unknown_component";
    case RefusalCode::component_version_undescribed: return "component_version_undescribed";
    case RefusalCode::no_upgrade_path: return "no_upgrade_path";
    case RefusalCode::missing_intermediate_capability: return "missing_intermediate_capability";
    case RefusalCode::downgrade_incompatible: return "downgrade_incompatible";
    case RefusalCode::provenance_missing: return "provenance_missing";
    case RefusalCode::gate_unsatisfied: return "gate_unsatisfied";
    case RefusalCode::gate_indeterminate: return "gate_indeterminate";
    case RefusalCode::evidence_missing: return "evidence_missing";
    case RefusalCode::evidence_stale: return "evidence_stale";
    case RefusalCode::policy_generation_too_low: return "policy_generation_too_low";
    case RefusalCode::policy_generation_mismatch: return "policy_generation_mismatch";
    case RefusalCode::policy_generation_unknown: return "policy_generation_unknown";
    case RefusalCode::dependency_indeterminate: return "dependency_indeterminate";
    case RefusalCode::dependency_version_unsatisfied: return "dependency_version_unsatisfied";
    case RefusalCode::dependency_capability_missing: return "dependency_capability_missing";
    case RefusalCode::capability_removal_unproven: return "capability_removal_unproven";
    case RefusalCode::capability_removal_dependents_remain: return "capability_removal_dependents_remain";
    case RefusalCode::mixed_version_not_interoperable: return "mixed_version_not_interoperable";
    case RefusalCode::split_brain: return "split_brain";
    case RefusalCode::irreversibility_unjustified: return "irreversibility_unjustified";
    case RefusalCode::rollback_impossible: return "rollback_impossible";
    case RefusalCode::cohort_skips_wave: return "cohort_skips_wave";
    case RefusalCode::cohort_empty: return "cohort_empty";
    case RefusalCode::exception_requests_irreversible: return "exception_requests_irreversible";
    case RefusalCode::exception_out_of_scope: return "exception_out_of_scope";
    case RefusalCode::step_cycle: return "step_cycle";
  }
  return "unknown";
}

// A gate may only be waived by an exception for operational reasons. Dependency
// compatibility and capability removal are proofs, and an exception cannot
// substitute for a proof.
bool waiverable(GateKind kind) noexcept {
  return kind == GateKind::precondition || kind == GateKind::readiness ||
         kind == GateKind::health || kind == GateKind::exception;
}

}  // namespace

const char* to_string(RefusalCode code) noexcept { return refusal_name(code); }

const SiteObservation* ValidationContext::find(const SiteId& id) const {
  for (const SiteObservation& observation : observed_sites) {
    if (observation.site == id) {
      return &observation;
    }
  }
  return nullptr;
}

Result<Digest256> compute_observation_digest(const std::vector<SiteObservation>& sites) {
  std::vector<SiteObservation> ordered = sites;
  std::sort(ordered.begin(), ordered.end(),
            [](const SiteObservation& left, const SiteObservation& right) {
              return left.site < right.site;
            });
  CanonicalWriter writer;
  DCE_TRY(writer.put_count(ordered.size()));
  for (const SiteObservation& observation : ordered) {
    DCE_TRY(codec::put(writer, observation.site));
    DCE_TRY(codec::put(writer, observation.dccp_version));
    DCE_TRY(codec::put_list(writer, observation.components, kMaxComponents));
    DCE_TRY(codec::put(writer, observation.accepted_generation));
    DCE_TRY(codec::put(writer, observation.capabilities));
    DCE_TRY(codec::put(writer, observation.policy.has_value()));
    if (observation.policy.has_value()) {
      DCE_TRY(codec::put(writer, *observation.policy));
    }
    DCE_TRY(codec::put(writer, observation.state_digest));
    DCE_TRY(codec::put(writer, observation.delegated_rollout_authority));
  }
  return writer.digest();
}

Result<ValidationReport> validate_plan(const EvolutionPlan& plan,
                                       const ValidationContext& context) {
  ValidationReport report;
  report.plan_digest = plan.digest;
  report.planned_steps = plan.steps;
  report.planned_cohorts = plan.cohorts;

  const std::string plan_subject = plan.identity.id.str();
  const auto refuse = [&report](RefusalCode code, std::string subject, std::string explanation) {
    report.refusals.push_back(Refusal{code, std::move(subject), std::move(explanation)});
  };

  // 1. Structure first: everything below assumes the plan is internally sound.
  const Status structural = structural_check(plan);
  if (!structural.ok()) {
    refuse(RefusalCode::structural_defect, plan_subject, structural.message());
    return report;
  }

  // 2. The attached digest must summarise the content it is attached to.
  DCE_ASSIGN(recomputed, compute_plan_digest(plan));
  if (!(recomputed == plan.digest)) {
    refuse(RefusalCode::structural_defect, plan_subject,
           "the plan digest does not match the plan content");
    return report;
  }
  DCE_ASSIGN(membership_digest, plan.membership.compute_digest());
  if (!(membership_digest == plan.membership.digest)) {
    refuse(RefusalCode::structural_defect, plan_subject,
           "the membership snapshot digest does not match the snapshot content");
    return report;
  }

  // 3. Fencing: a plan from a superseded epoch or generation may not act.
  if (!(plan.identity.epoch == context.epoch)) {
    refuse(RefusalCode::stale_plan_epoch, plan_subject,
           "the plan was written in coordinator epoch " +
               text::u64_to_string(plan.identity.epoch.value()) + " but the coordinator is in epoch " +
               text::u64_to_string(context.epoch.value()));
  }
  if (plan.identity.generation < context.current_generation) {
    refuse(RefusalCode::stale_plan_generation, plan_subject,
           "the plan generation is behind the coordinator's authoritative generation");
  }

  // 4. The fleet the plan was written against must still be the fleet that exists.
  DCE_ASSIGN(observed_digest, compute_observation_digest(context.observed_sites));
  report.observed_state_digest = observed_digest;
  if (!(plan.source_state_digest == observed_digest)) {
    refuse(RefusalCode::source_state_stale, plan_subject,
           "the plan was written against a different observed fleet state");
  }

  std::vector<SiteObservation> observations = context.observed_sites;
  std::sort(observations.begin(), observations.end(),
            [](const SiteObservation& left, const SiteObservation& right) {
              return left.site < right.site;
            });
  for (std::size_t index = 1; index < observations.size(); ++index) {
    if (observations[index - 1].site == observations[index].site) {
      refuse(RefusalCode::structural_defect, observations[index].site.str(),
             "the observed fleet reports the same site more than once");
    }
  }

  // 5. Membership agreement, site by site.
  for (const SiteRecord& record : plan.membership.sites) {
    const SiteObservation* observation = nullptr;
    for (const SiteObservation& candidate : observations) {
      if (candidate.site == record.id) {
        observation = &candidate;
        break;
      }
    }
    if (observation == nullptr) {
      refuse(RefusalCode::site_not_observed, record.id.str(),
             "the plan names this site but the coordinator cannot observe it");
      continue;
    }
    ++report.checked_sites;
    if (!(observation->accepted_generation == record.accepted_generation)) {
      refuse(RefusalCode::site_generation_mismatch, record.id.str(),
             "the site's accepted generation is not the generation the plan recorded");
    }
    if (!(observation->dccp_version == record.dccp_version)) {
      refuse(RefusalCode::site_version_mismatch, record.id.str(),
             "the site runs a different control-plane version than the plan recorded");
    }
    if (!(observation->state_digest == record.state_digest)) {
      refuse(RefusalCode::site_state_mismatch, record.id.str(),
             "the site's state digest differs from the state the plan was written against");
    }
    if (!(observation->components == record.components)) {
      refuse(RefusalCode::site_component_mismatch, record.id.str(),
             "the component versions the site runs differ from the plan's snapshot");
    }
  }
  for (const SiteObservation& observation : observations) {
    if (find_site(plan, observation.site) == nullptr) {
      refuse(RefusalCode::uncovered_site, observation.site.str(),
             "the coordinator observes this site but the plan does not cover it; a site cannot "
             "be left out of an evolution");
    }
  }

  // 6. A plan moves forward or it is not an evolution plan.
  if (!(plan.source_version < plan.target_version)) {
    refuse(RefusalCode::target_not_forward, plan.target_version.to_string(),
           "the target version is not ahead of the source version");
  }

  // 7. Every observed component version must be described, because an
  //    undescribed version makes compatibility unknown rather than assumed.
  for (const SiteObservation& observation : observations) {
    for (const ComponentVersion& running : observation.components) {
      if (!plan.capabilities.describes(running.component, running.version)) {
        refuse(RefusalCode::component_version_undescribed,
               observation.site.str() + "/" + running.component.str(),
               "the capability matrix does not describe this component version, so its "
               "compatibility is unknown");
      }
    }
  }

  // 8. Every component the plan evolves must have exactly one destination, and
  //    every site must have an evidenced, capability-complete route to it.
  std::vector<ComponentId> evolved_components;
  for (const MigrationStep& step : plan.steps) {
    if (std::find(evolved_components.begin(), evolved_components.end(), step.component) ==
        evolved_components.end()) {
      evolved_components.push_back(step.component);
    }
  }
  std::vector<std::pair<ComponentId, Version>> targets;
  for (const ComponentId& component : evolved_components) {
    std::vector<Version> sinks;
    for (const MigrationStep& step : plan.steps) {
      if (step.component != component) {
        continue;
      }
      bool is_source_of_another = false;
      for (const MigrationStep& other : plan.steps) {
        if (other.component == component && other.from == step.to) {
          is_source_of_another = true;
          break;
        }
      }
      if (!is_source_of_another &&
          std::find(sinks.begin(), sinks.end(), step.to) == sinks.end()) {
        sinks.push_back(step.to);
      }
    }
    if (sinks.size() == 1) {
      targets.emplace_back(component, sinks.front());
    } else {
      refuse(RefusalCode::unknown_component, component.str(),
             "the plan's steps for this component do not converge on exactly one destination");
    }
  }

  for (const SiteObservation& observation : observations) {
    for (const ComponentVersion& running : observation.components) {
      const auto target = std::find_if(targets.begin(), targets.end(),
                                       [&running](const std::pair<ComponentId, Version>& entry) {
                                         return entry.first == running.component;
                                       });
      if (target == targets.end()) {
        continue;  // this component is not evolved by the plan
      }
      Result<PathOutcome> outcome = plan.compatibility.find_path(
          running.component, running.version, target->second, observation.capabilities,
          TransitionKind::upgrade, plan.evidence);
      if (!outcome.ok()) {
        refuse(RefusalCode::no_upgrade_path, observation.site.str() + "/" + running.component.str(),
               outcome.message());
        continue;
      }
      if (outcome->found()) {
        report.checked_edges += static_cast<std::uint64_t>(outcome->path->edges.size());
        ++report.checked_steps;
        continue;
      }
      const PathRefusal& refusal = *outcome->refusal;
      RefusalCode code = RefusalCode::no_upgrade_path;
      switch (refusal.kind) {
        case PathRefusalKind::source_unknown:
        case PathRefusalKind::target_unknown:
          code = RefusalCode::unknown_component;
          break;
        case PathRefusalKind::no_path:
          code = RefusalCode::no_upgrade_path;
          break;
        case PathRefusalKind::missing_intermediate_capability:
          code = RefusalCode::missing_intermediate_capability;
          break;
        case PathRefusalKind::downgrade_incompatible:
          code = RefusalCode::downgrade_incompatible;
          break;
        case PathRefusalKind::provenance_missing:
          code = RefusalCode::provenance_missing;
          break;
      }
      std::string detail = refusal.explanation;
      if (refusal.missing_capability.valid()) {
        detail += " (capability " + refusal.missing_capability.str() + " at version " +
                  refusal.at_version.to_string() + ")";
      }
      refuse(code, observation.site.str() + "/" + running.component.str(), std::move(detail));
    }
  }

  // 9. Gates. Each gate is evaluated from evidence that is present and current,
  //    and a blocking gate that is not satisfied refuses the plan.
  for (const Gate& gate : plan.gates.entries()) {
    GateEvaluation evaluation;
    evaluation.gate = gate.id;
    bool recorded = false;

    const ExceptionGrant* waiver = nullptr;
    for (const ExceptionGrant& grant : plan.exceptions) {
      if (grant.waived_gate == gate.id) {
        waiver = &grant;
        break;
      }
    }

    if (waiver != nullptr) {
      if (waiver->requests_irreversible) {
        refuse(RefusalCode::exception_requests_irreversible, gate.id.str(),
               "an exception may never waive a point of no return or an irreversible step");
        recorded = true;
      }
      if (waiver->epoch > context.epoch) {
        refuse(RefusalCode::evidence_stale, gate.id.str(),
               "the exception was granted in a coordinator epoch that has not been reached");
        recorded = true;
      }
      if (!waiverable(gate.kind)) {
        refuse(RefusalCode::exception_out_of_scope, gate.id.str(),
               "an exception cannot replace a compatibility, dependency or capability-removal "
               "proof");
        recorded = true;
      }
      if (!recorded) {
        evaluation.outcome = GateOutcome::passed;
        evaluation.explanation = "waived by exception " + waiver->id.str();
      } else {
        evaluation.outcome = GateOutcome::failed;
      }
    } else {
      bool missing = false;
      bool stale = false;
      bool capability_absent = false;
      for (const EvidenceKind kind : gate.required_evidence) {
        const std::vector<const EvidenceRecord*> matches = plan.evidence.of_kind(kind);
        if (matches.empty()) {
          refuse(RefusalCode::evidence_missing, gate.id.str(),
                 std::string("no ") + to_string(kind) + " evidence is attached to the plan");
          missing = true;
          recorded = true;
          break;
        }
        bool current = false;
        bool from_the_future = false;
        for (const EvidenceRecord* record : matches) {
          if (record->observed_epoch > context.epoch) {
            from_the_future = true;
            continue;
          }
          const bool epoch_scoped = !record->observed_epoch.is_zero();
          if (epoch_scoped && record->observed_epoch < plan.identity.epoch) {
            continue;  // observed before the revision it is meant to vouch for
          }
          current = true;
          evaluation.satisfied_by.push_back(record->id);
        }
        if (!current) {
          if (from_the_future) {
            refuse(RefusalCode::gate_indeterminate, gate.id.str(),
                   std::string("the only ") + to_string(kind) +
                       " records were observed in a coordinator epoch that has not been reached, "
                       "so the gate cannot be judged");
          } else {
            refuse(RefusalCode::evidence_stale, gate.id.str(),
                   std::string("every ") + to_string(kind) +
                       " record predates the plan revision it is meant to vouch for");
          }
          stale = true;
          recorded = true;
        }
      }
      for (const CapabilityId& required : gate.required_capabilities) {
        bool present = false;
        for (const SiteObservation& observation : observations) {
          if (observation.capabilities.contains(required)) {
            present = true;
            break;
          }
        }
        if (!present) {
          refuse(RefusalCode::gate_unsatisfied, gate.id.str(),
                 "no observed site offers the capability this gate requires");
          capability_absent = true;
          recorded = true;
          break;
        }
      }
      if (missing || capability_absent) {
        evaluation.outcome = GateOutcome::failed;
      } else if (stale) {
        evaluation.outcome = GateOutcome::indeterminate;
      } else {
        evaluation.outcome = GateOutcome::passed;
        evaluation.explanation = "every required evidence record is present and current";
      }
    }

    if (gate.blocking && evaluation.outcome != GateOutcome::passed && !recorded) {
      refuse(evaluation.outcome == GateOutcome::indeterminate ? RefusalCode::gate_indeterminate
                                                              : RefusalCode::gate_unsatisfied,
             gate.id.str(), "a blocking gate is not satisfied");
    }
    report.gates.push_back(std::move(evaluation));
    ++report.checked_gates;
  }

  // 10. Policy generation requirements.
  for (const SiteObservation& observation : observations) {
    switch (evaluate_policy(plan.policy, observation.policy)) {
      case PolicyVerdict::satisfied:
        break;
      case PolicyVerdict::too_low:
        refuse(RefusalCode::policy_generation_too_low, observation.site.str(),
               "the site's policy generation ordinal is below the minimum this plan requires");
        break;
      case PolicyVerdict::identity_mismatch:
        refuse(RefusalCode::policy_generation_mismatch, observation.site.str(),
               "the site's policy generation identity is not the one this plan requires");
        break;
      case PolicyVerdict::unknown:
        refuse(RefusalCode::policy_generation_unknown, observation.site.str(),
               "the site did not report a policy generation, so the requirement cannot be "
               "evaluated");
        break;
    }
  }

  // 11. Explicitly consumed compatibility with neighbouring boundaries.
  for (const SiteRecord& record : plan.membership.sites) {
    for (const DependencyCompatibilityRef& dependency : record.dependencies) {
      const DependencyAssessment assessment = assess_dependency(dependency);
      const std::string subject = record.id.str() + "/" + dependency.boundary;
      switch (assessment.verdict) {
        case DependencyVerdict::satisfied:
          break;
        case DependencyVerdict::indeterminate:
          refuse(RefusalCode::dependency_indeterminate, subject, assessment.explanation);
          break;
        case DependencyVerdict::version_out_of_range:
          refuse(RefusalCode::dependency_version_unsatisfied, subject, assessment.explanation);
          break;
        case DependencyVerdict::capability_missing:
          refuse(RefusalCode::dependency_capability_missing, subject, assessment.explanation);
          break;
      }
    }
  }

  // 12. Capability removal and deprecation require proof that nothing depends
  //     on the capability any more.
  for (const DeprecationGate& deprecation : plan.deprecations) {
    const CapabilityDependencyProof* proof = nullptr;
    for (const CapabilityDependencyProof& candidate : context.removal_proofs) {
      if (candidate.capability == deprecation.capability) {
        proof = &candidate;
        break;
      }
    }
    const RemovalAssessment assessment =
        assess_removal(deprecation.capability, proof, context.epoch);
    switch (assessment.verdict) {
      case RemovalVerdict::permitted:
        break;
      case RemovalVerdict::dependents_remain:
        refuse(RefusalCode::capability_removal_dependents_remain, deprecation.capability.str(),
               assessment.explanation);
        break;
      case RemovalVerdict::proof_missing:
      case RemovalVerdict::proof_stale:
        refuse(RefusalCode::capability_removal_unproven, deprecation.capability.str(),
               assessment.explanation);
        break;
    }
  }

  // 13. Cohorts must be complete, contiguous and cover every site exactly once.
  // Cohorts are canonically ordered by wave, so the waves are non-decreasing
  // here. Contiguity means the next wave is at most one greater than the last.
  std::uint32_t last_wave = 0;
  bool wave_seen = false;
  for (const RolloutCohort& cohort : plan.cohorts) {
    if (cohort.sites.empty()) {
      refuse(RefusalCode::cohort_empty, cohort.id.str(), "a cohort with no sites cannot advance");
    }
    if (!wave_seen) {
      if (cohort.wave.value() != 0) {
        refuse(RefusalCode::cohort_skips_wave, cohort.id.str(),
               "the first cohort must be wave 0; waves cannot start part way through");
      }
      wave_seen = true;
      last_wave = cohort.wave.value();
      continue;
    }
    if (cohort.wave.value() > last_wave && cohort.wave.value() - last_wave > 1) {
      refuse(RefusalCode::cohort_skips_wave, cohort.id.str(),
             "this wave follows a gap; cohorts cannot skip a wave");
    }
    last_wave = cohort.wave.value();
  }
  for (const SiteRecord& record : plan.membership.sites) {
    std::size_t memberships = 0;
    for (const RolloutCohort& cohort : plan.cohorts) {
      for (const SiteId& site : cohort.sites) {
        if (site == record.id) {
          ++memberships;
        }
      }
    }
    if (memberships == 0) {
      refuse(RefusalCode::uncovered_site, record.id.str(),
             "the site belongs to no cohort, so it could never be evolved");
    } else if (memberships > 1) {
      refuse(RefusalCode::uncovered_site, record.id.str(),
             "the site belongs to more than one cohort, so its progression is ambiguous");
    }
  }

  // 14. Irreversibility must be justified, and a rollback policy that can never
  //     be used is an inconsistency rather than a safety margin.
  if (plan.point_of_no_return.has_value() && plan.point_of_no_return->justification.empty()) {
    refuse(RefusalCode::irreversibility_unjustified, plan_subject,
           "a point of no return was declared without a justification");
  }
  if (plan.rollback.permitted && plan.rollback.max_waves_back.is_zero() &&
      plan.cohorts.size() > 1) {
    refuse(RefusalCode::rollback_impossible, plan_subject,
           "rollback is declared available but the policy permits returning at most zero waves, "
           "so no earlier wave could ever be recovered");
  }

  // 15. Mixed-version interoperability across the whole observed fleet.
  struct VersionClass {
    ComponentId component;
    Version version;
    SiteId representative;
  };
  std::vector<VersionClass> classes;
  for (const SiteObservation& observation : observations) {
    for (const ComponentVersion& running : observation.components) {
      const auto existing = std::find_if(
          classes.begin(), classes.end(), [&running](const VersionClass& entry) {
            return entry.component == running.component && entry.version == running.version;
          });
      if (existing == classes.end()) {
        classes.push_back(VersionClass{running.component, running.version, observation.site});
      }
    }
  }
  for (std::size_t first = 0; first < classes.size(); ++first) {
    for (std::size_t second = first + 1; second < classes.size(); ++second) {
      if (!(classes[first].component == classes[second].component)) {
        continue;
      }
      if (classes[first].version == classes[second].version) {
        continue;
      }
      const InteropVerdict verdict = plan.capabilities.interoperate(
          classes[first].component, classes[first].version, classes[second].version);
      if (verdict.outcome != InteropOutcome::incompatible) {
        continue;  // unknown is already reported as an undescribed version
      }
      const ComponentId& component = classes[first].component;
      const Version& lower = classes[first].version;
      const Version& upper = classes[second].version;
      bool plan_resolves = false;
      for (const MigrationStep& step : plan.steps) {
        if (step.component != component) {
          continue;
        }
        if (step.from == lower || step.from == upper) {
          plan_resolves = true;
          break;
        }
      }
      const std::string subject = component.str();
      if (!plan_resolves) {
        refuse(RefusalCode::split_brain, subject,
               "two observed versions of this component cannot interoperate and no step in the "
               "plan moves either of them, so the fleet would stay split");
      }
    }
  }

  return report;
}

}  // namespace dce
