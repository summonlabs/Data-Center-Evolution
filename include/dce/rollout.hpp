// Deterministic cohort progression.
//
// The next rollout action is a pure function of the plan and the observed
// state. Nothing here knows how a vendor performs a deployment; this decides
// which site is eligible to advance to which stage, and refuses when a gate or
// a partition makes that unsafe.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "dce/digest.hpp"
#include "dce/ids.hpp"
#include "dce/limits.hpp"
#include "dce/plan.hpp"
#include "dce/status.hpp"

namespace dce {

// What this runtime believes about one site. The belief is always provisional:
// a reconnecting site reconciles its actual accepted stage over it.
struct SiteProgress {
  SiteId site;
  CohortId cohort;
  CohortWave wave;
  StageOrdinal accepted_stage;
  SiteGeneration generation;
  Digest256 stage_digest;
  bool partitioned{false};
  bool in_flight{false};

  auto operator<=>(const SiteProgress&) const = default;
};

struct RolloutState {
  std::vector<SiteProgress> sites;
  CohortWave current_wave;
  bool paused{false};

  [[nodiscard]] const SiteProgress* find(const SiteId& id) const;
  void canonicalize();
};

enum class RolloutActionKind : std::uint8_t {
  advance_site,
  await_receipt,
  advance_wave,
  gate_blocked,
  paused,
  completed,
  refused,
};

[[nodiscard]] const char* to_string(RolloutActionKind kind) noexcept;

struct RolloutAction {
  RolloutActionKind kind{RolloutActionKind::refused};
  SiteId site;
  CohortId cohort;
  StepId step;
  CohortWave wave;
  StageOrdinal target_stage;
  std::string explanation;
};

// STAGE SEMANTICS. A site's StageOrdinal is the number of migration steps it
// has durably accepted, so 0 means "nothing accepted yet" and
// stage_count(plan) means "fully evolved". The step a site accepts to move
// from ordinal n to ordinal n+1 is plan.steps[n] in canonical step order. A
// site may only ever advance by exactly one stage, which is how "sites cannot
// skip gates" is enforced structurally rather than by convention.
//
// The number of migration steps in the plan is therefore the number of stages
// a site must pass through.
[[nodiscard]] std::size_t stage_count(const EvolutionPlan& plan);

[[nodiscard]] Result<RolloutAction> next_rollout_action(const EvolutionPlan& plan,
                                                        const RolloutState& state);

// Whether every site named by the plan has reached the final stage.
[[nodiscard]] bool rollout_complete(const EvolutionPlan& plan, const RolloutState& state);

}  // namespace dce
