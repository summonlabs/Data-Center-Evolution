// Deterministic synthetic fleets.
//
// Complicated evolution behaviour is only convincing when it is exercised on a
// fleet big enough to have cohorts, mixed versions and a real compatibility
// graph. This generator builds one from a seed alone, so tests and benchmarks
// reproduce exactly and a failure can be replayed.
#pragma once

#include <cstdint>
#include <vector>

#include "dce/ids.hpp"
#include "dce/plan.hpp"
#include "dce/rollout.hpp"
#include "dce/status.hpp"
#include "dce/validate.hpp"
#include "dce/version.hpp"

namespace dce {

struct SyntheticFleetOptions {
  std::size_t sites{8};
  std::size_t components{3};
  std::size_t cohorts{2};
  // How many distinct versions each component has, including the source and the
  // target; must be at least two.
  std::size_t versions_per_component{3};
  std::uint64_t seed{1};
  Version source_version;
  Version target_version;
  bool delegate_authority{true};
  std::size_t gates_per_cohort{1};
};

struct SyntheticFleet {
  EvolutionPlan plan;
  // One profile per site, matching plan.membership in the same order.
  std::vector<SiteRecord> profiles;
  // The fleet as the coordinator would observe it before any evolution.
  std::vector<SiteObservation> initial_observations;
  Digest256 observation_digest;
};

// Builds a fleet in which every site runs every component below the target
// version, every upgrade edge carries a compatibility certification, and the
// capability matrix describes every version present. The result is
// structurally sound and validates against its own initial observations.
[[nodiscard]] Result<SyntheticFleet> make_synthetic_fleet(const SyntheticFleetOptions& options);

// A rollout state in which no site has accepted any stage yet.
[[nodiscard]] Result<RolloutState> initial_rollout_state(const EvolutionPlan& plan);

}  // namespace dce
