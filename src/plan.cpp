// The evolution plan boundary: canonical ordering, structural checks, digests
// and identity lookup.
//
// Ordering and checking are deliberately separate. canonicalize() makes the
// digest a pure function of content, so two plans that describe the same
// evolution hash identically however they were assembled. structural_check()
// then reports the first defect in a documented order, so an operator sees the
// same problem for two plans that differ only in insertion order.
#include "dce/plan.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "dce/codec.hpp"
#include "dce/limits.hpp"
#include "dce/text.hpp"

namespace dce {
namespace {

// ---------------------------------------------------------------------------
// Canonical ordering
// ---------------------------------------------------------------------------

// Sorts by the element's own order and removes exact repeats only. Two entries
// that share an identity but differ in content are both kept, so that
// structural_check reports the conflict instead of canonicalisation hiding it.
template <class T>
void sort_and_deduplicate(std::vector<T>& items) {
  std::sort(items.begin(), items.end());
  items.erase(std::unique(items.begin(), items.end()), items.end());
}

void order_membership(SiteMembershipSnapshot& membership) {
  for (SiteRecord& site : membership.sites) {
    // The component versions a site runs are a set of (component, version)
    // pairs: the same pair twice is the same declaration, which is why only
    // exact repeats are removed.
    sort_and_deduplicate(site.components);
  }
  std::sort(membership.sites.begin(), membership.sites.end(),
            [](const SiteRecord& left, const SiteRecord& right) { return left.id < right.id; });
}

// Canonical order for every collection the plan owns. This only orders: the
// digest fields are left untouched so that the digest computation can order its
// own copy through here without recursing back into itself.
void order_plan(EvolutionPlan& plan) {
  order_membership(plan.membership);
  plan.capabilities.canonicalize();
  plan.compatibility.canonicalize();
  plan.gates.canonicalize();
  plan.evidence.canonicalize();

  for (MigrationStep& step : plan.steps) {
    sort_and_deduplicate(step.depends_on);
    sort_and_deduplicate(step.gates);
  }
  std::sort(plan.steps.begin(), plan.steps.end(),
            [](const MigrationStep& left, const MigrationStep& right) { return left.id < right.id; });

  for (RolloutCohort& cohort : plan.cohorts) {
    sort_and_deduplicate(cohort.sites);
    sort_and_deduplicate(cohort.gates);
  }
  std::sort(plan.cohorts.begin(), plan.cohorts.end(),
            [](const RolloutCohort& left, const RolloutCohort& right) {
              if (left.wave != right.wave) {
                return left.wave < right.wave;
              }
              return left.id < right.id;
            });

  std::sort(plan.preconditions.begin(), plan.preconditions.end(),
            [](const Precondition& left, const Precondition& right) {
              return left.name < right.name;
            });

  std::sort(plan.deprecations.begin(), plan.deprecations.end(),
            [](const DeprecationGate& left, const DeprecationGate& right) {
              if (left.capability != right.capability) {
                return left.capability < right.capability;
              }
              return left.gate < right.gate;
            });

  std::sort(plan.exceptions.begin(), plan.exceptions.end(),
            [](const ExceptionGrant& left, const ExceptionGrant& right) {
              if (left.epoch != right.epoch) {
                return left.epoch < right.epoch;
              }
              return left.id < right.id;
            });
}

// ---------------------------------------------------------------------------
// Structural checking
// ---------------------------------------------------------------------------

std::string count_text(std::size_t value) {
  return text::u64_to_string(static_cast<std::uint64_t>(value));
}

// Untrusted text is escaped, so a justification or precondition name cannot
// carry a control sequence into the message an operator reads.
std::string quoted(std::string_view value) { return "'" + text::escape_for_output(value) + "'"; }

[[nodiscard]] Error over_bound(const std::string& subject, std::size_t bound) {
  return Error{ErrorCode::limit_exceeded,
               subject + " exceeds the documented bound of " + count_text(bound)};
}

// Indices of items in comparator order, with the storage index as a tiebreak so
// that the order is a function of the values alone. Every scan below uses one
// of these, which is what makes the reported problem independent of how the
// plan happened to be assembled.
template <class Item, class Less>
[[nodiscard]] std::vector<std::size_t> ordered_indices(const std::vector<Item>& items, Less less) {
  std::vector<std::size_t> order(items.size());
  std::size_t next = 0;
  for (std::size_t& slot : order) {
    slot = next;
    ++next;
  }
  std::sort(order.begin(), order.end(), [&items, &less](std::size_t left, std::size_t right) {
    if (less(items[left], items[right])) {
      return true;
    }
    if (less(items[right], items[left])) {
      return false;
    }
    return left < right;
  });
  return order;
}

// Reports the first element that repeats an identity already seen in the given
// order. "Already seen" is identity equality, which is why the comparator must
// order by identity alone.
template <class Item, class Less, class NameOf>
[[nodiscard]] Status reject_duplicate_identity(const std::vector<Item>& items,
                                               const std::vector<std::size_t>& order, Less less,
                                               NameOf name_of, std::string_view what) {
  for (std::size_t index = 1; index < order.size(); ++index) {
    const Item& previous = items[order[index - 1]];
    const Item& current = items[order[index]];
    if (!less(previous, current) && !less(current, previous)) {
      return Error{ErrorCode::already_exists,
                   std::string(what) + " " + name_of(current) + " is declared more than once"};
    }
  }
  return Status{};
}

constexpr auto kSiteOrder = [](const SiteRecord& left, const SiteRecord& right) {
  return left.id < right.id;
};
constexpr auto kStepOrder = [](const MigrationStep& left, const MigrationStep& right) {
  return left.id < right.id;
};
constexpr auto kCohortOrder = [](const RolloutCohort& left, const RolloutCohort& right) {
  return left.id < right.id;
};
constexpr auto kGateOrder = [](const Gate& left, const Gate& right) { return left.id < right.id; };
constexpr auto kEvidenceOrder = [](const EvidenceRecord& left, const EvidenceRecord& right) {
  return left.id < right.id;
};
constexpr auto kPreconditionOrder = [](const Precondition& left, const Precondition& right) {
  return left.name < right.name;
};
constexpr auto kDeprecationOrder = [](const DeprecationGate& left, const DeprecationGate& right) {
  if (left.capability != right.capability) {
    return left.capability < right.capability;
  }
  return left.gate < right.gate;
};
constexpr auto kExceptionOrder = [](const ExceptionGrant& left, const ExceptionGrant& right) {
  return left.id < right.id;
};

constexpr auto kSiteName = [](const SiteRecord& site) { return quoted(site.id.view()); };
constexpr auto kStepName = [](const MigrationStep& step) { return quoted(step.id.view()); };
constexpr auto kCohortName = [](const RolloutCohort& cohort) { return quoted(cohort.id.view()); };
constexpr auto kGateName = [](const Gate& gate) { return quoted(gate.id.view()); };
constexpr auto kEvidenceName = [](const EvidenceRecord& record) { return quoted(record.id.view()); };
constexpr auto kPreconditionName = [](const Precondition& precondition) {
  return quoted(precondition.name);
};
constexpr auto kExceptionName = [](const ExceptionGrant& exception) {
  return quoted(exception.id.view());
};

// Position of a step in the identity order, or nothing when absent. The cycle
// check needs this to be O(log n): every dependency would otherwise rescan the
// whole step list.
[[nodiscard]] std::optional<std::size_t> lookup_step_index(
    const std::vector<MigrationStep>& steps, const std::vector<std::size_t>& step_order,
    const StepId& id) {
  std::size_t low = 0;
  std::size_t high = step_order.size();
  while (low < high) {
    const std::size_t middle = low + (high - low) / 2;
    if (steps[step_order[middle]].id < id) {
      low = middle + 1;
    } else {
      high = middle;
    }
  }
  if (low < step_order.size() && steps[step_order[low]].id == id) {
    return step_order[low];
  }
  return std::nullopt;
}

// Depth-first search over depends_on with the three-colour scheme, followed in
// identity order so that the reported step is stable. An explicit stack is used
// because the bound on steps is far beyond a safe recursion depth.
[[nodiscard]] Status reject_step_cycle(const std::vector<MigrationStep>& steps,
                                       const std::vector<std::size_t>& step_order) {
  std::vector<std::vector<std::size_t>> successors(steps.size());
  for (std::size_t index = 0; index < steps.size(); ++index) {
    for (const StepId& dependency : steps[index].depends_on) {
      const std::optional<std::size_t> target = lookup_step_index(steps, step_order, dependency);
      if (target.has_value()) {
        successors[index].push_back(*target);
      }
    }
  }
  for (std::vector<std::size_t>& list : successors) {
    std::sort(list.begin(), list.end());
    list.erase(std::unique(list.begin(), list.end()), list.end());
  }

  constexpr std::uint8_t kUnvisited = 0;
  constexpr std::uint8_t kOnStack = 1;
  constexpr std::uint8_t kFinished = 2;
  std::vector<std::uint8_t> colour(steps.size(), kUnvisited);
  for (const std::size_t root : step_order) {
    if (colour[root] != kUnvisited) {
      continue;
    }
    std::vector<std::pair<std::size_t, std::size_t>> stack;
    stack.emplace_back(root, std::size_t{0});
    colour[root] = kOnStack;
    while (!stack.empty()) {
      std::pair<std::size_t, std::size_t>& frame = stack.back();
      if (frame.second >= successors[frame.first].size()) {
        colour[frame.first] = kFinished;
        stack.pop_back();
        continue;
      }
      const std::size_t child = successors[frame.first][frame.second];
      ++frame.second;
      if (colour[child] == kOnStack) {
        return Error{ErrorCode::invalid_argument,
                     "step " + quoted(steps[child].id.view()) +
                         " participates in a dependency cycle"};
      }
      if (colour[child] == kUnvisited) {
        colour[child] = kOnStack;
        stack.emplace_back(child, std::size_t{0});
      }
    }
  }
  return Status{};
}

}  // namespace

const char* to_string(Irreversibility value) noexcept {
  switch (value) {
    case Irreversibility::reversible: return "reversible";
    case Irreversibility::irreversible_after_commit: return "irreversible_after_commit";
  }
  return "unknown";
}

const char* to_string(RolloutStrategy strategy) noexcept {
  switch (strategy) {
    case RolloutStrategy::sequential: return "sequential";
    case RolloutStrategy::parallel: return "parallel";
    case RolloutStrategy::canary: return "canary";
  }
  return "unknown";
}

void SiteMembershipSnapshot::canonicalize() { order_membership(*this); }

Result<Digest256> SiteMembershipSnapshot::compute_digest() const {
  SiteMembershipSnapshot copy = *this;
  copy.canonicalize();
  // The digest is derived rather than stored, so the field is zeroed before
  // encoding: a stale digest carried in the snapshot cannot influence the hash.
  copy.digest = Digest256::zero();
  CanonicalWriter writer;
  DCE_TRY(codec::put(writer, copy));
  return writer.digest();
}

Result<Digest256> compute_membership_digest(const SiteMembershipSnapshot& snapshot) {
  return snapshot.compute_digest();
}

Status canonicalize(EvolutionPlan& plan) {
  order_plan(plan);
  // The membership digest is part of what the plan asserts about the fleet it
  // was written against, so it is refreshed alongside the plan digest. Neither
  // digest field is encoded, so this cannot feed back into the hash.
  DCE_ASSIGN(membership_digest, compute_membership_digest(plan.membership));
  plan.membership.digest = membership_digest;
  DCE_ASSIGN(plan_digest, compute_plan_digest(plan));
  plan.digest = plan_digest;
  return Status{};
}

Status structural_check(const EvolutionPlan& plan) {
  // Every scan below walks one of these orders, so which defect is reported
  // first is a function of the plan's content and not of its insertion order.
  const std::vector<std::size_t> site_order = ordered_indices(plan.membership.sites, kSiteOrder);
  const std::vector<std::size_t> step_order = ordered_indices(plan.steps, kStepOrder);
  const std::vector<std::size_t> cohort_order = ordered_indices(plan.cohorts, kCohortOrder);
  const std::vector<std::size_t> gate_order = ordered_indices(plan.gates.entries(), kGateOrder);
  const std::vector<std::size_t> evidence_order =
      ordered_indices(plan.evidence.entries(), kEvidenceOrder);
  const std::vector<std::size_t> precondition_order =
      ordered_indices(plan.preconditions, kPreconditionOrder);
  const std::vector<std::size_t> deprecation_order =
      ordered_indices(plan.deprecations, kDeprecationOrder);
  const std::vector<std::size_t> exception_order =
      ordered_indices(plan.exceptions, kExceptionOrder);

  // --- bounds -------------------------------------------------------------
  if (plan.membership.sites.size() > kMaxSites) {
    return over_bound("the site list of the membership snapshot", kMaxSites);
  }
  for (const std::size_t index : site_order) {
    const SiteRecord& site = plan.membership.sites[index];
    if (site.components.size() > kMaxComponents) {
      return over_bound("the component list of site " + quoted(site.id.view()), kMaxComponents);
    }
    if (site.dependencies.size() > kMaxDependencies) {
      return over_bound("the dependency list of site " + quoted(site.id.view()), kMaxDependencies);
    }
    if (site.obligations.size() > kMaxSites) {
      return over_bound("the obligation list of site " + quoted(site.id.view()), kMaxSites);
    }
  }
  if (plan.capabilities.entries().size() > CapabilityMatrix::kMaxEntries) {
    return over_bound("the capability matrix", CapabilityMatrix::kMaxEntries);
  }
  if (plan.compatibility.nodes().size() > kMaxGraphNodes) {
    return over_bound("the node list of the compatibility graph", kMaxGraphNodes);
  }
  if (plan.compatibility.edges().size() > kMaxGraphEdges) {
    return over_bound("the edge list of the compatibility graph", kMaxGraphEdges);
  }
  if (plan.steps.size() > kMaxSteps) {
    return over_bound("the step list of the plan", kMaxSteps);
  }
  for (const std::size_t index : step_order) {
    const MigrationStep& step = plan.steps[index];
    if (step.depends_on.size() > kMaxStepDependencies) {
      return over_bound("the dependency list of step " + quoted(step.id.view()),
                        kMaxStepDependencies);
    }
    if (step.gates.size() > kMaxGates) {
      return over_bound("the gate list of step " + quoted(step.id.view()), kMaxGates);
    }
  }
  if (plan.cohorts.size() > kMaxCohorts) {
    return over_bound("the cohort list of the plan", kMaxCohorts);
  }
  for (const std::size_t index : cohort_order) {
    const RolloutCohort& cohort = plan.cohorts[index];
    if (cohort.sites.size() > kMaxCohortSites) {
      return over_bound("the site list of cohort " + quoted(cohort.id.view()), kMaxCohortSites);
    }
    if (cohort.gates.size() > kMaxGates) {
      return over_bound("the gate list of cohort " + quoted(cohort.id.view()), kMaxGates);
    }
  }
  if (plan.preconditions.size() > kMaxPreconditions) {
    return over_bound("the precondition list of the plan", kMaxPreconditions);
  }
  if (plan.gates.size() > kMaxGates) {
    return over_bound("the gate list of the plan", kMaxGates);
  }
  for (const std::size_t index : gate_order) {
    const Gate& gate = plan.gates.entries()[index];
    if (gate.required_evidence.size() > kMaxGateEvidence) {
      return over_bound("the evidence requirement list of gate " + quoted(gate.id.view()),
                        kMaxGateEvidence);
    }
    if (gate.required_capabilities.size() > CapabilitySet::kMaxCapabilities) {
      return over_bound("the capability requirement list of gate " + quoted(gate.id.view()),
                        CapabilitySet::kMaxCapabilities);
    }
  }
  if (plan.rollback.irreversible_steps.size() > kMaxSteps) {
    return over_bound("the irreversible step list of the rollback policy", kMaxSteps);
  }
  if (plan.deprecations.size() > kMaxDeprecations) {
    return over_bound("the deprecation list of the plan", kMaxDeprecations);
  }
  if (plan.exceptions.size() > kMaxExceptions) {
    return over_bound("the exception list of the plan", kMaxExceptions);
  }
  if (plan.evidence.size() > kMaxEvidence) {
    return over_bound("the evidence list of the plan", kMaxEvidence);
  }

  // --- identity -----------------------------------------------------------
  if (!plan.identity.id.valid()) {
    return Error{ErrorCode::invalid_argument,
                 "the plan identity is absent: an evolution plan must carry a valid "
                 "EvolutionPlanId"};
  }

  // --- duplicates ---------------------------------------------------------
  DCE_TRY(reject_duplicate_identity(plan.membership.sites, site_order, kSiteOrder, kSiteName,
                                    "membership site"));
  DCE_TRY(reject_duplicate_identity(plan.steps, step_order, kStepOrder, kStepName, "step"));
  DCE_TRY(reject_duplicate_identity(plan.cohorts, cohort_order, kCohortOrder, kCohortName,
                                    "cohort"));
  DCE_TRY(reject_duplicate_identity(plan.gates.entries(), gate_order, kGateOrder, kGateName,
                                    "gate"));
  DCE_TRY(reject_duplicate_identity(plan.evidence.entries(), evidence_order, kEvidenceOrder,
                                    kEvidenceName, "evidence record"));
  DCE_TRY(reject_duplicate_identity(plan.preconditions, precondition_order, kPreconditionOrder,
                                    kPreconditionName, "precondition"));
  DCE_TRY(reject_duplicate_identity(plan.exceptions, exception_order, kExceptionOrder,
                                    kExceptionName, "exception"));
  // A cohort that names the same site twice does not say which occurrence
  // governs, so it is reported rather than silently collapsed.
  for (const std::size_t index : cohort_order) {
    const RolloutCohort& cohort = plan.cohorts[index];
    std::vector<SiteId> named(cohort.sites);
    std::sort(named.begin(), named.end());
    for (std::size_t position = 1; position < named.size(); ++position) {
      if (named[position - 1] == named[position]) {
        return Error{ErrorCode::already_exists,
                     "cohort " + quoted(cohort.id.view()) + " names site " +
                         quoted(named[position].view()) + " more than once"};
      }
    }
  }

  // --- dangling references ------------------------------------------------
  for (const std::size_t index : step_order) {
    const MigrationStep& step = plan.steps[index];
    for (const StepId& dependency : step.depends_on) {
      if (dependency == step.id) {
        return Error{ErrorCode::invalid_argument,
                     "step " + quoted(step.id.view()) + " depends on itself"};
      }
      if (find_step(plan, dependency) == nullptr) {
        return Error{ErrorCode::not_found, "step " + quoted(step.id.view()) +
                                               " depends on unknown step " +
                                               quoted(dependency.view())};
      }
    }
  }
  for (const std::size_t index : step_order) {
    const MigrationStep& step = plan.steps[index];
    for (const GateId& gate : step.gates) {
      if (plan.gates.find(gate) == nullptr) {
        return Error{ErrorCode::not_found, "step " + quoted(step.id.view()) +
                                               " names unknown gate " + quoted(gate.view())};
      }
    }
  }
  for (const std::size_t index : cohort_order) {
    const RolloutCohort& cohort = plan.cohorts[index];
    for (const GateId& gate : cohort.gates) {
      if (plan.gates.find(gate) == nullptr) {
        return Error{ErrorCode::not_found, "cohort " + quoted(cohort.id.view()) +
                                               " names unknown gate " + quoted(gate.view())};
      }
    }
  }
  for (const std::size_t index : precondition_order) {
    const Precondition& precondition = plan.preconditions[index];
    if (plan.gates.find(precondition.gate) == nullptr) {
      return Error{ErrorCode::not_found,
                   "precondition " + quoted(precondition.name) + " names unknown gate " +
                       quoted(precondition.gate.view())};
    }
  }
  for (const std::size_t index : deprecation_order) {
    const DeprecationGate& deprecation = plan.deprecations[index];
    if (plan.gates.find(deprecation.gate) == nullptr) {
      return Error{ErrorCode::not_found,
                   "the deprecation of capability " + quoted(deprecation.capability.view()) +
                       " names unknown gate " + quoted(deprecation.gate.view())};
    }
  }
  for (const std::size_t index : exception_order) {
    const ExceptionGrant& exception = plan.exceptions[index];
    if (plan.gates.find(exception.waived_gate) == nullptr) {
      return Error{ErrorCode::not_found, "exception " + quoted(exception.id.view()) +
                                             " waives unknown gate " +
                                             quoted(exception.waived_gate.view())};
    }
  }
  for (const std::size_t index : cohort_order) {
    const RolloutCohort& cohort = plan.cohorts[index];
    for (const SiteId& site : cohort.sites) {
      if (find_site(plan, site) == nullptr) {
        return Error{ErrorCode::not_found,
                     "cohort " + quoted(cohort.id.view()) + " names site " +
                         quoted(site.view()) + " that is not in the membership snapshot"};
      }
    }
  }
  for (const std::size_t index : cohort_order) {
    const RolloutCohort& cohort = plan.cohorts[index];
    if (cohort.max_parallel == 0) {
      return Error{ErrorCode::invalid_argument,
                   "cohort " + quoted(cohort.id.view()) + " declares a max_parallel of zero"};
    }
  }
  for (const StepId& step : plan.rollback.irreversible_steps) {
    if (find_step(plan, step) == nullptr) {
      return Error{ErrorCode::not_found, "the rollback policy names unknown irreversible step " +
                                             quoted(step.view())};
    }
  }
  if (plan.point_of_no_return.has_value() && plan.point_of_no_return->step.has_value()) {
    const StepId& step = *plan.point_of_no_return->step;
    if (find_step(plan, step) == nullptr) {
      return Error{ErrorCode::not_found,
                   "the point of no return names unknown step " + quoted(step.view())};
    }
  }
  DCE_TRY(reject_step_cycle(plan.steps, step_order));
  for (const std::size_t index : step_order) {
    const MigrationStep& step = plan.steps[index];
    const bool changes_version = step.kind == TransitionKind::upgrade ||
                                 step.kind == TransitionKind::downgrade;
    if (changes_version && step.from == step.to) {
      return Error{ErrorCode::invalid_argument,
                   "step " + quoted(step.id.view()) + " is declared as " + to_string(step.kind) +
                       " but does not change the version: from and to are both " +
                       step.from.to_string()};
    }
  }

  return Status{};
}

Result<Digest256> compute_plan_digest(const EvolutionPlan& plan) {
  EvolutionPlan copy = plan;
  order_plan(copy);
  // Both digest fields are derived rather than stored, so they are zeroed
  // before encoding: a stale digest carried in the plan cannot influence the
  // hash it is supposed to summarise.
  copy.membership.digest = Digest256::zero();
  copy.digest = Digest256::zero();
  CanonicalWriter writer;
  DCE_TRY(codec::put(writer, copy));
  return writer.digest();
}

const MigrationStep* find_step(const EvolutionPlan& plan, const StepId& id) {
  for (const MigrationStep& step : plan.steps) {
    if (step.id == id) {
      return &step;
    }
  }
  return nullptr;
}

const RolloutCohort* find_cohort(const EvolutionPlan& plan, const CohortId& id) {
  for (const RolloutCohort& cohort : plan.cohorts) {
    if (cohort.id == id) {
      return &cohort;
    }
  }
  return nullptr;
}

const SiteRecord* find_site(const EvolutionPlan& plan, const SiteId& id) {
  for (const SiteRecord& site : plan.membership.sites) {
    if (site.id == id) {
      return &site;
    }
  }
  return nullptr;
}

}  // namespace dce
