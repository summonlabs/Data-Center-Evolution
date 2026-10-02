#include "dce/capability.hpp"

#include <algorithm>

namespace dce {

Status CapabilitySet::insert(CapabilityId capability) {
  const auto position = std::lower_bound(capabilities_.begin(), capabilities_.end(), capability);
  if (position != capabilities_.end() && *position == capability) {
    return Status{};  // a set has no duplicate members: this is idempotent
  }
  if (capabilities_.size() >= kMaxCapabilities) {
    return Error{ErrorCode::limit_exceeded, "capability set exceeds its documented bound"};
  }
  capabilities_.insert(position, std::move(capability));
  return Status{};
}

bool CapabilitySet::contains(const CapabilityId& capability) const {
  return std::binary_search(capabilities_.begin(), capabilities_.end(), capability);
}

std::vector<CapabilityId> CapabilitySet::missing_from(const CapabilitySet& other) const {
  std::vector<CapabilityId> missing;
  for (const CapabilityId& capability : capabilities_) {
    if (!other.contains(capability)) {
      missing.push_back(capability);
    }
  }
  return missing;
}

Status CapabilityMatrix::add(ComponentCapabilities entry) {
  const auto position = std::lower_bound(
      entries_.begin(), entries_.end(), entry,
      [](const ComponentCapabilities& left, const ComponentCapabilities& right) {
        if (left.component != right.component) {
          return left.component < right.component;
        }
        return left.version < right.version;
      });
  if (position != entries_.end() && position->component == entry.component &&
      position->version == entry.version) {
    if (*position == entry) {
      return Status{};  // identical declaration repeated: idempotent
    }
    return Error{ErrorCode::conflict,
                 "capability matrix already describes this component version differently"};
  }
  if (entries_.size() >= kMaxEntries) {
    return Error{ErrorCode::limit_exceeded, "capability matrix exceeds its documented bound"};
  }
  entries_.insert(position, std::move(entry));
  return Status{};
}

void CapabilityMatrix::canonicalize() {
  std::sort(entries_.begin(), entries_.end(),
            [](const ComponentCapabilities& left, const ComponentCapabilities& right) {
              if (left.component != right.component) {
                return left.component < right.component;
              }
              return left.version < right.version;
            });
}

Result<ComponentCapabilities> CapabilityMatrix::lookup(const ComponentId& component,
                                                       const Version& version) const {
  for (const ComponentCapabilities& entry : entries_) {
    if (entry.component == component && entry.version == version) {
      return entry;
    }
  }
  return Error{ErrorCode::not_found, "capability matrix has no entry for this component version"};
}

bool CapabilityMatrix::describes(const ComponentId& component, const Version& version) const {
  return lookup(component, version).ok();
}

InteropVerdict CapabilityMatrix::interoperate(const ComponentId& component, const Version& first,
                                              const Version& second) const {
  InteropVerdict verdict;
  Result<ComponentCapabilities> left = lookup(component, first);
  Result<ComponentCapabilities> right = lookup(component, second);
  if (!left.ok() || !right.ok()) {
    verdict.outcome = InteropOutcome::unknown;
    verdict.explanation =
        "the capability matrix does not describe both versions, so interoperability is unknown "
        "rather than assumed";
    return verdict;
  }
  verdict.missing_from_first = left->requires_capabilities.missing_from(right->provides);
  verdict.missing_from_second = right->requires_capabilities.missing_from(left->provides);
  if (verdict.missing_from_first.empty() && verdict.missing_from_second.empty()) {
    verdict.outcome = InteropOutcome::interoperable;
    verdict.explanation = "both versions supply every capability the other requires";
    return verdict;
  }
  verdict.outcome = InteropOutcome::incompatible;
  verdict.explanation = "a mixed-version pair is missing capabilities the other side requires";
  return verdict;
}

PolicyVerdict evaluate_policy(const PolicyRequirement& requirement,
                              const std::optional<PolicyGeneration>& observed) {
  if (!observed.has_value()) {
    return PolicyVerdict::unknown;
  }
  if (requirement.exact.has_value() && observed->id != *requirement.exact) {
    return PolicyVerdict::identity_mismatch;
  }
  if (observed->ordinal < requirement.minimum_ordinal) {
    return PolicyVerdict::too_low;
  }
  return PolicyVerdict::satisfied;
}

DependencyAssessment assess_dependency(const DependencyCompatibilityRef& reference) {
  DependencyAssessment assessment;
  if (!reference.observed) {
    assessment.verdict = DependencyVerdict::indeterminate;
    assessment.explanation =
        "the neighbouring boundary has not been observed, so its compatibility is indeterminate";
    return assessment;
  }
  if (!reference.supported.contains(reference.observed_version)) {
    assessment.verdict = DependencyVerdict::version_out_of_range;
    assessment.explanation = "the observed neighbour version is outside the supported range";
    return assessment;
  }
  assessment.missing_capabilities = reference.required.missing_from(reference.observed_capabilities);
  if (!assessment.missing_capabilities.empty()) {
    assessment.verdict = DependencyVerdict::capability_missing;
    assessment.explanation = "the neighbour does not offer every capability this plan consumes";
    return assessment;
  }
  assessment.verdict = DependencyVerdict::satisfied;
  assessment.explanation = "the neighbour reported a supported version with every required capability";
  return assessment;
}

RemovalAssessment assess_removal(const CapabilityId& capability,
                                 const CapabilityDependencyProof* proof,
                                 CoordinatorEpoch current_epoch) {
  RemovalAssessment assessment;
  if (proof == nullptr || proof->capability != capability) {
    assessment.verdict = RemovalVerdict::proof_missing;
    assessment.explanation = "no dependency proof was supplied for this capability";
    return assessment;
  }
  if (proof->observed_epoch > current_epoch) {
    assessment.verdict = RemovalVerdict::proof_stale;
    assessment.explanation =
        "the dependency proof was taken in an epoch the coordinator has not reached";
    return assessment;
  }
  if (!proof->remaining_dependents.empty() || !proof->obligations.empty()) {
    assessment.verdict = RemovalVerdict::dependents_remain;
    assessment.explanation = "sites or obligations still depend on this capability";
    return assessment;
  }
  assessment.verdict = RemovalVerdict::permitted;
  assessment.explanation = "no site and no obligation depends on this capability";
  return assessment;
}

}  // namespace dce
