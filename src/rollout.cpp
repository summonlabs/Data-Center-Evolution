#include "dce/rollout.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "dce/text.hpp"

namespace dce {

const char* to_string(RolloutActionKind kind) noexcept {
  switch (kind) {
    case RolloutActionKind::advance_site:
      return "advance_site";
    case RolloutActionKind::await_receipt:
      return "await_receipt";
    case RolloutActionKind::advance_wave:
      return "advance_wave";
    case RolloutActionKind::gate_blocked:
      return "gate_blocked";
    case RolloutActionKind::paused:
      return "paused";
    case RolloutActionKind::completed:
      return "completed";
    case RolloutActionKind::refused:
      return "refused";
  }
  return "unknown";
}

const SiteProgress* RolloutState::find(const SiteId& id) const {
  const auto found = std::find_if(sites.begin(), sites.end(), [&id](const SiteProgress& progress) {
    return progress.site == id;
  });
  return found == sites.end() ? nullptr : &*found;
}

void RolloutState::canonicalize() {
  std::sort(sites.begin(), sites.end(),
            [](const SiteProgress& lhs, const SiteProgress& rhs) { return lhs.site < rhs.site; });
}

std::size_t stage_count(const EvolutionPlan& plan) { return plan.steps.size(); }

namespace {

template <class Tag>
std::string name_of(const BasicId<Tag>& id) {
  return text::escape_for_output(id.view());
}

std::string number(std::uint32_t value) { return text::u32_to_string(value); }
std::string number(std::uint64_t value) { return text::u64_to_string(value); }

// A site is complete once it has durably accepted every migration step, which
// is exactly accepted_stage >= stage_count(plan).
bool complete_stage(const SiteProgress& progress, std::size_t stages) {
  return static_cast<std::uint64_t>(progress.accepted_stage.value()) >=
         static_cast<std::uint64_t>(stages);
}

// Cohorts in canonical (wave, id) order: the order in which waves are rolled
// out, and the order in which two cohorts of the same wave are considered.
std::vector<const RolloutCohort*> canonical_cohorts(const EvolutionPlan& plan) {
  std::vector<const RolloutCohort*> ordered;
  ordered.reserve(plan.cohorts.size());
  for (const RolloutCohort& cohort : plan.cohorts) {
    ordered.push_back(&cohort);
  }
  std::sort(ordered.begin(), ordered.end(), [](const RolloutCohort* lhs, const RolloutCohort* rhs) {
    if (lhs->wave != rhs->wave) {
      return lhs->wave < rhs->wave;
    }
    return lhs->id < rhs->id;
  });
  return ordered;
}

// A cohort whose sites are not all visible in the state is never called
// complete: an unobserved site is not an accepted site.
bool cohort_complete(const RolloutCohort& cohort, const RolloutState& state, std::size_t stages) {
  for (const SiteId& id : cohort.sites) {
    const SiteProgress* progress = state.find(id);
    if (progress == nullptr || !complete_stage(*progress, stages)) {
      return false;
    }
  }
  return true;
}

// Site order within a cohort is canonical whatever order the plan listed.
std::vector<SiteId> canonical_sites(const RolloutCohort& cohort) {
  std::vector<SiteId> sites = cohort.sites;
  std::sort(sites.begin(), sites.end());
  return sites;
}

RolloutAction decision(RolloutActionKind kind, std::string explanation) {
  RolloutAction action;
  action.kind = kind;
  action.explanation = std::move(explanation);
  return action;
}

}  // namespace

bool rollout_complete(const EvolutionPlan& plan, const RolloutState& state) {
  const std::size_t stages = stage_count(plan);
  for (const SiteRecord& record : plan.membership.sites) {
    const SiteProgress* progress = state.find(record.id);
    if (progress == nullptr || !complete_stage(*progress, stages)) {
      return false;
    }
  }
  return true;
}

Result<RolloutAction> next_rollout_action(const EvolutionPlan& plan, const RolloutState& state) {
  if (state.paused) {
    return decision(RolloutActionKind::paused,
                    "the rollout is paused; no site advances until it is resumed");
  }

  const std::size_t stages = stage_count(plan);
  if (stages == 0) {
    return decision(RolloutActionKind::refused,
                    "the plan declares no migration steps, so no site has a stage to advance to");
  }

  const RolloutCohort* target = nullptr;
  for (const RolloutCohort* cohort : canonical_cohorts(plan)) {
    if (!cohort_complete(*cohort, state, stages)) {
      target = cohort;
      break;
    }
  }
  if (target == nullptr) {
    return decision(RolloutActionKind::completed,
                    "every cohort of the plan has reached the final stage");
  }

  if (state.current_wave != target->wave) {
    RolloutAction action;
    action.kind = RolloutActionKind::advance_wave;
    action.cohort = target->id;
    action.wave = target->wave;
    action.explanation =
        "cohort " + name_of(target->id) + " in wave " + number(target->wave.value()) +
        " is the lowest incomplete cohort and the rollout cursor stands at wave " +
        number(state.current_wave.value()) + "; the cursor must be at wave " +
        number(target->wave.value()) + " before any site can advance";
    return action;
  }

  const std::vector<SiteId> sites = canonical_sites(*target);

  std::size_t in_flight = 0;
  for (const SiteId& id : sites) {
    const SiteProgress* progress = state.find(id);
    if (progress != nullptr && progress->in_flight) {
      ++in_flight;
    }
  }

  const bool sequential = target->strategy == RolloutStrategy::sequential;
  const bool at_limit =
      sequential ? in_flight > 0 : in_flight >= static_cast<std::size_t>(target->max_parallel);
  if (at_limit) {
    RolloutAction action;
    action.kind = RolloutActionKind::await_receipt;
    action.cohort = target->id;
    action.wave = target->wave;
    action.explanation =
        sequential
            ? "cohort " + name_of(target->id) + " has " +
                  number(static_cast<std::uint64_t>(in_flight)) +
                  " site(s) in flight and advances one site at a time; the rollout awaits a "
                  "receipt before choosing another site"
            : "cohort " + name_of(target->id) + " has " +
                  number(static_cast<std::uint64_t>(in_flight)) +
                  " site(s) in flight, at or above its limit of " + number(target->max_parallel) +
                  " concurrent advance(s); the rollout awaits a receipt before starting another "
                  "site";
    return action;
  }

  const SiteProgress* chosen = nullptr;
  bool partitioned_remaining = false;
  for (const SiteId& id : sites) {
    const SiteProgress* progress = state.find(id);
    if (progress == nullptr) {
      continue;
    }
    if (complete_stage(*progress, stages) || progress->in_flight) {
      continue;
    }
    if (progress->partitioned) {
      partitioned_remaining = true;
      continue;
    }
    chosen = progress;
    break;
  }

  if (chosen == nullptr) {
    // A site the cohort names but the state does not describe is refused
    // before anything else: a rollout must never skip a site it cannot see.
    for (const SiteId& id : sites) {
      if (state.find(id) == nullptr) {
        RolloutAction action;
        action.kind = RolloutActionKind::refused;
        action.site = id;
        action.cohort = target->id;
        action.wave = target->wave;
        action.explanation = "cohort " + name_of(target->id) + " names site " + name_of(id) +
                             ", which is absent from the rollout state; a rollout must never "
                             "silently skip a site it cannot see";
        return action;
      }
    }
    if (partitioned_remaining) {
      RolloutAction action;
      action.kind = RolloutActionKind::gate_blocked;
      action.cohort = target->id;
      action.wave = target->wave;
      action.explanation = "the remaining sites of cohort " + name_of(target->id) + " (wave " +
                           number(target->wave.value()) +
                           ") are partitioned; no progress is safe until a partition heals";
      return action;
    }
    // Every site still incomplete in this cohort is already in flight, so the
    // only safe action is to wait for one of those receipts to arrive.
    RolloutAction action;
    action.kind = RolloutActionKind::await_receipt;
    action.cohort = target->id;
    action.wave = target->wave;
    action.explanation = "every site still incomplete in cohort " + name_of(target->id) +
                         " (wave " + number(target->wave.value()) +
                         ") is already in flight; the rollout awaits a receipt";
    return action;
  }

  const std::size_t index = static_cast<std::size_t>(chosen->accepted_stage.value());
  // The site is incomplete, so its ordinal still indexes a real step of the plan.
  const MigrationStep& step = plan.steps[index];

  const Result<StageOrdinal> next_stage = chosen->accepted_stage.next();
  if (!next_stage.ok()) {
    return next_stage.error();
  }

  RolloutAction action;
  action.kind = RolloutActionKind::advance_site;
  action.site = chosen->site;
  action.cohort = target->id;
  action.step = step.id;
  action.wave = target->wave;
  action.target_stage = next_stage.value();
  action.explanation = "site " + name_of(chosen->site) + " advances from stage " +
                       number(chosen->accepted_stage.value()) + " to stage " +
                       number(action.target_stage.value()) + " by accepting migration step " +
                       name_of(step.id) + " (cohort " + name_of(target->id) + ", wave " +
                       number(target->wave.value()) + ")";
  return action;
}

}  // namespace dce
