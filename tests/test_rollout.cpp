// Cohort progression: the purity of the rollout decision, cohort and wave
// ordering, the strategy limits and the gates that stop a sweep.
//
// The sweep is the property this suite exists for. Starting from a fleet in
// which no site has accepted anything, every advance_site decision is applied
// to the state until the rollout reports completion. A site's accepted stage
// moves when its receipt settles, exactly as the coordinator's belief does in
// production, so the sweep must leave every named site at exactly stage_count
// with nothing in flight, and no site may ever be asked to skip a stage, to
// advance against a wave cursor that has not moved, or to advance twice.
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "dce/plan.hpp"
#include "dce/rollout.hpp"
#include "dce/synthetic.hpp"
#include "harness.hpp"

namespace {

using dce::CohortId;
using dce::CohortWave;
using dce::EvolutionPlan;
using dce::RolloutAction;
using dce::RolloutActionKind;
using dce::RolloutCohort;
using dce::RolloutState;
using dce::RolloutStrategy;
using dce::SiteId;
using dce::SiteProgress;
using dce::StageOrdinal;
using dce::StepId;

std::string kind_name(RolloutActionKind kind) { return dce::to_string(kind); }

SiteId site_id(std::size_t index) { return *SiteId::parse("site-" + std::to_string(index)); }

StepId step_id(std::size_t index) { return *StepId::parse("step-" + std::to_string(index)); }

CohortId cohort_id(std::size_t index) { return *CohortId::parse("cohort-" + std::to_string(index)); }

// One assertion with a caller-supplied message, for checks whose context (the
// seed, the fleet shape, the site) a macro cannot render.
void record_check(bool condition, const std::string& message) {
  if (condition) {
    dce::test::count_assertion();
    return;
  }
  DCE_FAIL(message);
}

// A minimal plan: only what the rollout decision reads is populated, so a plan
// defect cannot hide behind unrelated content.
EvolutionPlan hand_plan(std::size_t steps, const std::vector<RolloutCohort>& cohorts,
                        const std::vector<SiteId>& membership) {
  EvolutionPlan plan;
  plan.steps.reserve(steps);
  for (std::size_t index = 0; index < steps; ++index) {
    dce::MigrationStep step;
    step.id = step_id(index);
    step.from = dce::Version{1, static_cast<std::uint32_t>(index), 0};
    step.to = dce::Version{1, static_cast<std::uint32_t>(index + 1), 0};
    plan.steps.push_back(std::move(step));
  }
  plan.cohorts = cohorts;
  for (const SiteId& member : membership) {
    dce::SiteRecord record;
    record.id = member;
    plan.membership.sites.push_back(std::move(record));
  }
  return plan;
}

RolloutCohort make_cohort(std::size_t index, std::uint32_t wave, RolloutStrategy strategy,
                          std::uint32_t max_parallel, const std::vector<SiteId>& sites) {
  RolloutCohort cohort;
  cohort.id = cohort_id(index);
  cohort.wave = CohortWave{wave};
  cohort.strategy = strategy;
  cohort.max_parallel = max_parallel;
  cohort.sites = sites;
  cohort.canary = strategy == RolloutStrategy::canary;
  return cohort;
}

bool build_state(const EvolutionPlan& plan, RolloutState& out, const std::string& label) {
  const dce::Result<RolloutState> state = dce::initial_rollout_state(plan);
  if (!state.ok()) {
    DCE_FAIL(label + ": initial_rollout_state failed: " +
             dce::test::outcome_detail(state.code(), state.message()));
    return false;
  }
  out = state.value();
  return true;
}

SiteProgress* mutable_site(RolloutState& state, const SiteId& id) {
  for (SiteProgress& progress : state.sites) {
    if (progress.site == id) {
      return &progress;
    }
  }
  return nullptr;
}

bool site_complete(const SiteProgress* progress, std::size_t stages) {
  return progress != nullptr &&
         static_cast<std::size_t>(progress->accepted_stage.value()) >= stages;
}

std::size_t in_flight_count(const RolloutState& state) {
  std::size_t count = 0;
  for (const SiteProgress& progress : state.sites) {
    if (progress.in_flight) {
      ++count;
    }
  }
  return count;
}

// A decision is compared and replayed through this canonical text, which names
// every field that identifies it.
std::string decision_key(const RolloutAction& action) {
  std::string key = kind_name(action.kind);
  key += "|site=" + action.site.str();
  key += "|cohort=" + action.cohort.str();
  key += "|step=" + action.step.str();
  key += "|wave=" + std::to_string(action.wave.value());
  key += "|stage=" + std::to_string(action.target_stage.value());
  return key;
}

std::string join(const std::vector<std::string>& parts) {
  std::string out;
  for (const std::string& part : parts) {
    out += part;
    out.push_back('\n');
  }
  return out;
}

// An advance that has been asked for but whose receipt has not settled yet. The
// site's accepted stage moves when the receipt settles, not when the decision
// is taken, which is what makes "in flight" mean something.
using Outstanding = std::vector<std::pair<SiteId, StageOrdinal>>;

std::size_t settle_receipts(RolloutState& state, Outstanding& outstanding) {
  std::size_t settled = 0;
  for (const std::pair<SiteId, StageOrdinal>& advance : outstanding) {
    SiteProgress* progress = mutable_site(state, advance.first);
    if (progress == nullptr) {
      continue;
    }
    progress->accepted_stage = advance.second;
    progress->in_flight = false;
    ++settled;
  }
  outstanding.clear();
  return settled;
}

struct SweepResult {
  std::vector<std::string> decisions;
  std::vector<std::uint32_t> advance_waves;
  RolloutState final_state;
  bool completed{false};
  std::size_t advances{0};
  std::size_t waits{0};
  std::size_t wave_moves{0};
};

// Drives the rollout to completion, applying every decision to the state and
// asserting the invariants that make the sweep meaningful.
SweepResult run_sweep(const EvolutionPlan& plan, RolloutState state, const std::string& label) {
  SweepResult result;
  Outstanding outstanding;
  const std::size_t stages = dce::stage_count(plan);
  const std::size_t limit =
      2u * stages * (state.sites.size() + 1u) + 2u * plan.cohorts.size() + 16u;
  for (std::size_t iteration = 0; iteration < limit; ++iteration) {
    const dce::Result<RolloutAction> decision = dce::next_rollout_action(plan, state);
    if (!decision.ok()) {
      DCE_FAIL(label + ": the rollout failed: " +
               dce::test::outcome_detail(decision.code(), decision.message()));
      return result;
    }
    result.decisions.push_back(decision_key(*decision));

    switch (decision->kind) {
      case RolloutActionKind::advance_site: {
        SiteProgress* progress = mutable_site(state, decision->site);
        const std::uint32_t accepted =
            progress == nullptr ? 0u : progress->accepted_stage.value();
        if (progress == nullptr || static_cast<std::size_t>(accepted) >= stages) {
          DCE_FAIL(label + ": site " + decision->site.str() + " cannot advance from stage " +
                   std::to_string(accepted) + " of " + std::to_string(stages));
          return result;
        }
        record_check(!progress->in_flight,
                     label + ": site " + decision->site.str() + " was asked to advance twice");
        record_check(decision->target_stage.value() == accepted + 1u,
                     label + ": site " + decision->site.str() + " was told to advance to stage " +
                         std::to_string(decision->target_stage.value()) + " from stage " +
                         std::to_string(accepted) + ", which is not one stage");
        record_check(static_cast<std::size_t>(decision->target_stage.value()) <= stages,
                     label + ": site " + decision->site.str() + " was told to advance past stage " +
                         std::to_string(stages));
        record_check(decision->step == plan.steps[accepted].id,
                     label + ": site " + decision->site.str() + " was told to accept step " +
                         decision->step.str() + " instead of " + plan.steps[accepted].id.str() +
                         ", the step that takes it from stage " + std::to_string(accepted));
        record_check(state.current_wave == decision->wave,
                     label + ": site " + decision->site.str() + " advanced in wave " +
                         std::to_string(decision->wave.value()) +
                         " while the rollout cursor stood at wave " +
                         std::to_string(state.current_wave.value()));
        record_check(dce::find_cohort(plan, decision->cohort) != nullptr,
                     label + ": the decision names cohort " + decision->cohort.str() +
                         ", which the plan does not declare");
        for (const RolloutCohort& earlier : plan.cohorts) {
          if (!(earlier.wave < decision->wave)) {
            continue;
          }
          for (const SiteId& member : earlier.sites) {
            record_check(site_complete(state.find(member), stages),
                         label + ": wave " + std::to_string(decision->wave.value()) +
                             " advanced while wave " + std::to_string(earlier.wave.value()) +
                             " still had site " + member.str() + " incomplete");
          }
        }
        progress->in_flight = true;
        outstanding.emplace_back(decision->site, decision->target_stage);
        result.advance_waves.push_back(decision->wave.value());
        ++result.advances;
        break;
      }
      case RolloutActionKind::await_receipt: {
        record_check(dce::find_cohort(plan, decision->cohort) != nullptr,
                     label + ": the wait names cohort " + decision->cohort.str() +
                         ", which the plan does not declare");
        record_check(in_flight_count(state) == outstanding.size(),
                     label + ": the in-flight flags and the outstanding advances disagree");
        const std::size_t settled = settle_receipts(state, outstanding);
        record_check(settled > 0,
                     label + ": the rollout awaited a receipt although no site was in flight");
        ++result.waits;
        break;
      }
      case RolloutActionKind::advance_wave: {
        record_check(state.current_wave != decision->wave,
                     label + ": the rollout was told to move to wave " +
                         std::to_string(decision->wave.value()) +
                         ", which is already the current wave");
        record_check(dce::find_cohort(plan, decision->cohort) != nullptr,
                     label + ": the wave move names cohort " + decision->cohort.str() +
                         ", which the plan does not declare");
        state.current_wave = decision->wave;
        ++result.wave_moves;
        break;
      }
      case RolloutActionKind::completed: {
        record_check(dce::rollout_complete(plan, state),
                     label + ": the rollout reported completion while rollout_complete is false");
        result.completed = true;
        result.final_state = state;
        return result;
      }
      case RolloutActionKind::paused:
      case RolloutActionKind::gate_blocked:
      case RolloutActionKind::refused: {
        DCE_FAIL(label + ": an unpaused sweep reached " + kind_name(decision->kind) + ": " +
                 decision->explanation);
        return result;
      }
    }
  }
  DCE_FAIL(label + ": the sweep did not finish within " + std::to_string(limit) + " decisions");
  return result;
}

// The final state must be exactly complete: every site named by the plan at
// stage_count, nothing in flight, and the predicate agreeing.
void expect_complete(const EvolutionPlan& plan, const RolloutState& state,
                     const std::string& label) {
  const std::size_t stages = dce::stage_count(plan);
  record_check(dce::rollout_complete(plan, state),
               label + ": rollout_complete is false after the sweep");
  for (const dce::SiteRecord& record : plan.membership.sites) {
    const SiteProgress* progress = state.find(record.id);
    record_check(progress != nullptr,
                 label + ": site " + record.id.str() + " is absent from the final state");
    if (progress == nullptr) {
      continue;
    }
    record_check(static_cast<std::size_t>(progress->accepted_stage.value()) == stages,
                 label + ": site " + record.id.str() + " ended at stage " +
                     std::to_string(progress->accepted_stage.value()) + " instead of " +
                     std::to_string(stages));
    record_check(!progress->in_flight,
                 label + ": site " + record.id.str() + " is still in flight at the end");
  }
}

// The decision must be a pure function of the plan and the state: the same
// inputs give the same action and nothing is mutated.
void expect_pure(const EvolutionPlan& plan, const RolloutState& state, const std::string& label) {
  const dce::Result<RolloutAction> first = dce::next_rollout_action(plan, state);
  const dce::Result<RolloutAction> second = dce::next_rollout_action(plan, state);
  if (!first.ok() || !second.ok()) {
    DCE_FAIL(label + ": next_rollout_action failed: " +
             dce::test::outcome_detail(first.ok() ? second.code() : first.code(),
                                       first.ok() ? second.message() : first.message()));
    return;
  }
  record_check(decision_key(*first) == decision_key(*second),
               label + ": two calls on identical inputs chose different actions: " +
                   decision_key(*first) + " and " + decision_key(*second));
  record_check(first->explanation == second->explanation,
               label + ": two calls on identical inputs explained themselves differently");
}

std::size_t wave_sites(const EvolutionPlan& plan, std::uint32_t wave) {
  std::size_t count = 0;
  for (const RolloutCohort& cohort : plan.cohorts) {
    if (cohort.wave.value() == wave) {
      count += cohort.sites.size();
    }
  }
  return count;
}

}  // namespace

// ---------------------------------------------------------------------------
// Purity
// ---------------------------------------------------------------------------
DCE_TEST(rollout, next_rollout_action_is_pure) {
  dce::SyntheticFleetOptions options;
  options.sites = 6;
  options.components = 2;
  options.cohorts = 2;
  options.seed = 5;
  const dce::Result<dce::SyntheticFleet> fleet = dce::make_synthetic_fleet(options);
  DCE_REQUIRE_OK(fleet);
  const EvolutionPlan& plan = fleet->plan;

  RolloutState state;
  DCE_REQUIRE(build_state(plan, state, "purity"));
  const RolloutState before = state;
  expect_pure(plan, state, "initial state");
  record_check(state.sites.size() == before.sites.size(),
               "next_rollout_action changed the size of the rollout state");
  for (std::size_t index = 0; index < state.sites.size() && index < before.sites.size(); ++index) {
    record_check(state.sites[index] == before.sites[index],
                 "next_rollout_action mutated the belief about site " +
                     before.sites[index].site.str());
  }
  record_check(state.current_wave == before.current_wave && state.paused == before.paused,
               "next_rollout_action mutated the rollout cursor or the pause flag");

  // A state with a site in flight and a partitioned site must be just as pure:
  // the decision reads the state, it never repairs it.
  DCE_REQUIRE(state.sites.size() >= 2);
  state.sites[0].in_flight = true;
  state.sites[1].partitioned = true;
  state.sites[1].accepted_stage = StageOrdinal{1};
  const RolloutState busy = state;
  expect_pure(plan, state, "state with a site in flight and a partitioned site");
  for (std::size_t index = 0; index < state.sites.size(); ++index) {
    record_check(state.sites[index] == busy.sites[index],
                 "next_rollout_action mutated the belief about site " +
                     busy.sites[index].site.str());
  }
}

// ---------------------------------------------------------------------------
// The sweep
// ---------------------------------------------------------------------------
DCE_TEST(rollout, a_full_sweep_leaves_every_site_exactly_at_the_final_stage) {
  dce::SyntheticFleetOptions options;
  options.sites = 8;
  options.components = 3;
  options.cohorts = 2;
  options.seed = 1;
  const dce::Result<dce::SyntheticFleet> fleet = dce::make_synthetic_fleet(options);
  DCE_REQUIRE_OK(fleet);
  const EvolutionPlan& plan = fleet->plan;
  DCE_REQUIRE(dce::stage_count(plan) > 0);

  RolloutState state;
  DCE_REQUIRE(build_state(plan, state, "full sweep"));
  DCE_CHECK_TRUE(!dce::rollout_complete(plan, state));

  const SweepResult sweep = run_sweep(plan, state, "full sweep");
  DCE_CHECK_TRUE(sweep.completed);
  expect_complete(plan, sweep.final_state, "full sweep");
  // Every site advanced once per stage, and the cohort could not run ahead of
  // the receipt of the site before it.
  DCE_CHECK_EQ(sweep.advances, plan.membership.sites.size() * dce::stage_count(plan));
  DCE_CHECK_TRUE(sweep.waits > 0);
  DCE_CHECK_TRUE(sweep.wave_moves > 0);
}

DCE_TEST(rollout, wave_order_and_the_decision_sequence_are_reproducible) {
  dce::SyntheticFleetOptions options;
  options.sites = 8;
  options.components = 2;
  options.cohorts = 2;
  options.versions_per_component = 3;
  options.seed = 11;
  const dce::Result<dce::SyntheticFleet> fleet = dce::make_synthetic_fleet(options);
  DCE_REQUIRE_OK(fleet);
  const EvolutionPlan& plan = fleet->plan;
  const std::size_t stages = dce::stage_count(plan);
  DCE_REQUIRE(stages > 0);
  DCE_REQUIRE(plan.cohorts.size() == static_cast<std::size_t>(2));

  RolloutState first_state;
  RolloutState second_state;
  DCE_REQUIRE(build_state(plan, first_state, "first run"));
  DCE_REQUIRE(build_state(plan, second_state, "second run"));

  const SweepResult first = run_sweep(plan, first_state, "first run");
  const SweepResult second = run_sweep(plan, second_state, "second run");
  DCE_CHECK_TRUE(first.completed);
  DCE_CHECK_TRUE(second.completed);
  DCE_CHECK_EQ(join(first.decisions), join(second.decisions));

  // Wave 0 is completed before wave 1 begins: the wave of each advance is
  // non-decreasing, both waves are present, and wave 0 contributed exactly one
  // advance per site per stage before the cursor ever moved.
  DCE_CHECK_TRUE(std::is_sorted(first.advance_waves.begin(), first.advance_waves.end()));
  const std::size_t wave_zero_advances = static_cast<std::size_t>(
      std::count(first.advance_waves.begin(), first.advance_waves.end(), 0u));
  DCE_CHECK_EQ(wave_zero_advances, wave_sites(plan, 0u) * stages);
  DCE_CHECK_TRUE(std::find(first.advance_waves.begin(), first.advance_waves.end(), 1u) !=
                 first.advance_waves.end());
}

// ---------------------------------------------------------------------------
// Strategies
// ---------------------------------------------------------------------------
DCE_TEST(rollout, a_sequential_cohort_waits_for_the_site_in_flight) {
  const std::vector<SiteId> members{site_id(0), site_id(1)};
  const std::vector<RolloutCohort> cohorts{
      make_cohort(0, 0, RolloutStrategy::sequential, 1, members)};
  const EvolutionPlan plan = hand_plan(1, cohorts, members);
  RolloutState state;
  DCE_REQUIRE(build_state(plan, state, "sequential"));

  // Nothing in flight: the first site in canonical order advances.
  const dce::Result<RolloutAction> ready = dce::next_rollout_action(plan, state);
  DCE_REQUIRE_OK(ready);
  DCE_CHECK_EQ(kind_name(ready->kind), std::string("advance_site"));
  DCE_CHECK_EQ(ready->site.str(), members[0].str());

  SiteProgress* progress = mutable_site(state, ready->site);
  DCE_REQUIRE(progress != nullptr);
  const StageOrdinal target = ready->target_stage;
  progress->in_flight = true;

  // One site in flight is already one too many for a sequential cohort.
  const CohortId expected_cohort = cohort_id(0);
  const dce::Result<RolloutAction> waiting = dce::next_rollout_action(plan, state);
  DCE_REQUIRE_OK(waiting);
  DCE_CHECK_EQ(kind_name(waiting->kind), std::string("await_receipt"));
  DCE_CHECK_EQ(waiting->cohort.str(), expected_cohort.str());
  DCE_CHECK_TRUE(waiting->explanation.find("in flight") != std::string::npos);

  // The wait is caused by the flight and not by the cohort: the receipt settles
  // the first site (which completes it), and the next site becomes eligible.
  progress->accepted_stage = target;
  progress->in_flight = false;
  DCE_CHECK_TRUE(site_complete(state.find(members[0]), dce::stage_count(plan)));
  const dce::Result<RolloutAction> after = dce::next_rollout_action(plan, state);
  DCE_REQUIRE_OK(after);
  DCE_CHECK_EQ(kind_name(after->kind), std::string("advance_site"));
  DCE_CHECK_EQ(after->site.str(), members[1].str());
}

DCE_TEST(rollout, a_parallel_cohort_admits_exactly_max_parallel_sites) {
  const std::vector<SiteId> members{site_id(0), site_id(1), site_id(2)};
  const std::vector<RolloutCohort> cohorts{
      make_cohort(0, 0, RolloutStrategy::parallel, 2, members)};
  const EvolutionPlan plan = hand_plan(1, cohorts, members);
  RolloutState state;
  DCE_REQUIRE(build_state(plan, state, "parallel"));

  // Two sites may be in flight at once.
  std::vector<SiteId> started;
  for (int attempt = 0; attempt < 2; ++attempt) {
    const dce::Result<RolloutAction> decision = dce::next_rollout_action(plan, state);
    DCE_REQUIRE_OK(decision);
    DCE_CHECK_EQ(kind_name(decision->kind), std::string("advance_site"));
    SiteProgress* progress = mutable_site(state, decision->site);
    DCE_REQUIRE(progress != nullptr);
    record_check(!progress->in_flight,
                 "a site already in flight was chosen again under the parallel strategy");
    progress->in_flight = true;
    started.push_back(decision->site);
  }
  DCE_REQUIRE(started.size() == static_cast<std::size_t>(2));
  DCE_CHECK_TRUE(!(started[0] == started[1]));

  // The third needs a receipt first.
  const dce::Result<RolloutAction> at_limit = dce::next_rollout_action(plan, state);
  DCE_REQUIRE_OK(at_limit);
  DCE_CHECK_EQ(kind_name(at_limit->kind), std::string("await_receipt"));
  DCE_CHECK_TRUE(at_limit->explanation.find("limit") != std::string::npos);

  SiteProgress* settled = mutable_site(state, started[0]);
  DCE_REQUIRE(settled != nullptr);
  settled->accepted_stage = StageOrdinal{1};
  settled->in_flight = false;
  const dce::Result<RolloutAction> next = dce::next_rollout_action(plan, state);
  DCE_REQUIRE_OK(next);
  DCE_CHECK_EQ(kind_name(next->kind), std::string("advance_site"));
  DCE_CHECK_EQ(next->site.str(), members[2].str());
}

// ---------------------------------------------------------------------------
// Partitions and missing sites
// ---------------------------------------------------------------------------
DCE_TEST(rollout, a_partitioned_site_is_never_advanced_and_blocks_its_cohort) {
  const std::vector<SiteId> members{site_id(0), site_id(1)};
  const std::vector<RolloutCohort> cohorts{
      make_cohort(0, 0, RolloutStrategy::sequential, 1, members)};
  const EvolutionPlan plan = hand_plan(3, cohorts, members);
  const std::size_t stages = dce::stage_count(plan);
  RolloutState state;
  DCE_REQUIRE(build_state(plan, state, "partition"));

  SiteProgress* partitioned = mutable_site(state, members[0]);
  DCE_REQUIRE(partitioned != nullptr);
  partitioned->partitioned = true;

  // The healthy site advances; the partitioned one is passed over every time.
  for (std::size_t step = 0; step < stages; ++step) {
    const dce::Result<RolloutAction> decision = dce::next_rollout_action(plan, state);
    DCE_REQUIRE_OK(decision);
    DCE_CHECK_EQ(kind_name(decision->kind), std::string("advance_site"));
    DCE_CHECK_EQ(decision->site.str(), members[1].str());
    SiteProgress* advancing = mutable_site(state, decision->site);
    DCE_REQUIRE(advancing != nullptr);
    advancing->accepted_stage = decision->target_stage;
  }
  const SiteProgress* untouched = state.find(members[0]);
  DCE_REQUIRE(untouched != nullptr);
  DCE_CHECK_EQ(untouched->accepted_stage.value(), static_cast<std::uint32_t>(0));
  DCE_CHECK_TRUE(untouched->partitioned);

  // With the last remaining site of the cohort partitioned, the only safe
  // decision is to stop and say why.
  const CohortId expected_cohort = cohort_id(0);
  const dce::Result<RolloutAction> blocked = dce::next_rollout_action(plan, state);
  DCE_REQUIRE_OK(blocked);
  DCE_CHECK_EQ(kind_name(blocked->kind), std::string("gate_blocked"));
  DCE_CHECK_EQ(blocked->cohort.str(), expected_cohort.str());
  DCE_CHECK_TRUE(blocked->explanation.find("partition") != std::string::npos);
  DCE_CHECK_EQ(state.find(members[0])->accepted_stage.value(), static_cast<std::uint32_t>(0));

  // A cohort made of nothing but a partitioned site is blocked as well.
  const std::vector<SiteId> lonely{site_id(4)};
  const std::vector<RolloutCohort> lonely_cohorts{
      make_cohort(0, 0, RolloutStrategy::sequential, 1, lonely)};
  const EvolutionPlan lonely_plan = hand_plan(2, lonely_cohorts, lonely);
  RolloutState lonely_state;
  DCE_REQUIRE(build_state(lonely_plan, lonely_state, "lonely partition"));
  DCE_REQUIRE(mutable_site(lonely_state, lonely[0]) != nullptr);
  mutable_site(lonely_state, lonely[0])->partitioned = true;
  const dce::Result<RolloutAction> lonely_blocked =
      dce::next_rollout_action(lonely_plan, lonely_state);
  DCE_REQUIRE_OK(lonely_blocked);
  DCE_CHECK_EQ(kind_name(lonely_blocked->kind), std::string("gate_blocked"));
  DCE_CHECK_TRUE(lonely_blocked->explanation.find("partition") != std::string::npos);
}

DCE_TEST(rollout, a_site_missing_from_the_state_is_named_rather_than_skipped) {
  const SiteId absent = site_id(9);
  const std::vector<SiteId> members{site_id(0), absent};
  const std::vector<RolloutCohort> cohorts{
      make_cohort(0, 0, RolloutStrategy::sequential, 1, members)};
  const EvolutionPlan plan = hand_plan(3, cohorts, std::vector<SiteId>{site_id(0)});
  RolloutState state;
  DCE_REQUIRE(build_state(plan, state, "missing site"));
  DCE_CHECK_TRUE(state.find(absent) == nullptr);

  // Every site the state does describe has finished, so the only reason left to
  // refuse is the site the cohort names and the state does not describe. A
  // rollout that skipped it would report a wait for a receipt instead.
  SiteProgress* known = mutable_site(state, site_id(0));
  DCE_REQUIRE(known != nullptr);
  known->accepted_stage = StageOrdinal{static_cast<std::uint32_t>(dce::stage_count(plan))};

  const CohortId expected_cohort = cohort_id(0);
  const dce::Result<RolloutAction> decision = dce::next_rollout_action(plan, state);
  DCE_REQUIRE_OK(decision);
  DCE_CHECK_EQ(kind_name(decision->kind), std::string("refused"));
  DCE_CHECK_EQ(decision->site.str(), absent.str());
  DCE_CHECK_EQ(decision->cohort.str(), expected_cohort.str());
  DCE_CHECK_TRUE(decision->explanation.find(absent.str()) != std::string::npos);

  // The same holds when the cohort names nothing else at all.
  const std::vector<RolloutCohort> only_absent{
      make_cohort(0, 0, RolloutStrategy::sequential, 1, std::vector<SiteId>{absent})};
  const EvolutionPlan empty_plan = hand_plan(2, only_absent, std::vector<SiteId>{});
  RolloutState empty_state;
  DCE_REQUIRE(build_state(empty_plan, empty_state, "no site at all"));
  const dce::Result<RolloutAction> nothing = dce::next_rollout_action(empty_plan, empty_state);
  DCE_REQUIRE_OK(nothing);
  DCE_CHECK_EQ(kind_name(nothing->kind), std::string("refused"));
  DCE_CHECK_EQ(nothing->site.str(), absent.str());
  DCE_CHECK_TRUE(nothing->explanation.find(absent.str()) != std::string::npos);
}

// ---------------------------------------------------------------------------
// Stage bookkeeping
// ---------------------------------------------------------------------------
DCE_TEST(rollout, stage_count_is_the_number_of_migration_steps) {
  for (std::uint64_t seed : {std::uint64_t{1}, std::uint64_t{2}, std::uint64_t{3}}) {
    dce::SyntheticFleetOptions options;
    options.sites = 5;
    options.components = 3;
    options.cohorts = 2;
    options.versions_per_component = 4;
    options.seed = seed;
    const dce::Result<dce::SyntheticFleet> fleet = dce::make_synthetic_fleet(options);
    DCE_REQUIRE_OK(fleet);
    DCE_CHECK_EQ(dce::stage_count(fleet->plan), fleet->plan.steps.size());
    DCE_CHECK_EQ(dce::stage_count(fleet->plan), static_cast<std::size_t>(9));
  }

  // A plan with no steps has no stage to advance to: the decision must be a
  // refusal, not a completion that no site ever reached.
  const std::vector<SiteId> members{site_id(0)};
  const std::vector<RolloutCohort> cohorts{
      make_cohort(0, 0, RolloutStrategy::sequential, 1, members)};
  const EvolutionPlan empty_plan = hand_plan(0, cohorts, members);
  DCE_CHECK_EQ(dce::stage_count(empty_plan), static_cast<std::size_t>(0));
  RolloutState state;
  DCE_REQUIRE(build_state(empty_plan, state, "no steps"));
  const dce::Result<RolloutAction> decision = dce::next_rollout_action(empty_plan, state);
  DCE_REQUIRE_OK(decision);
  DCE_CHECK_EQ(kind_name(decision->kind), std::string("refused"));
  DCE_CHECK_TRUE(decision->explanation.find("no migration steps") != std::string::npos);
}

DCE_TEST(rollout, rollout_complete_follows_the_slowest_site) {
  dce::SyntheticFleetOptions options;
  options.sites = 4;
  options.components = 2;
  options.cohorts = 2;
  options.seed = 7;
  const dce::Result<dce::SyntheticFleet> fleet = dce::make_synthetic_fleet(options);
  DCE_REQUIRE_OK(fleet);
  const EvolutionPlan& plan = fleet->plan;
  const std::size_t stages = dce::stage_count(plan);
  DCE_REQUIRE(stages > 0);
  DCE_REQUIRE(plan.membership.sites.size() >= static_cast<std::size_t>(2));

  RolloutState state;
  DCE_REQUIRE(build_state(plan, state, "completion"));
  DCE_CHECK_TRUE(!dce::rollout_complete(plan, state));

  const SiteId leader = plan.membership.sites.front().id;
  DCE_REQUIRE(mutable_site(state, leader) != nullptr);
  mutable_site(state, leader)->accepted_stage = StageOrdinal{static_cast<std::uint32_t>(stages)};
  DCE_CHECK_TRUE(!dce::rollout_complete(plan, state));

  for (const dce::SiteRecord& record : plan.membership.sites) {
    SiteProgress* progress = mutable_site(state, record.id);
    DCE_REQUIRE(progress != nullptr);
    progress->accepted_stage = StageOrdinal{static_cast<std::uint32_t>(stages)};
  }
  DCE_CHECK_TRUE(dce::rollout_complete(plan, state));

  // One step short of the final stage is not complete, and neither is a fleet
  // whose site the state cannot see.
  mutable_site(state, leader)->accepted_stage =
      StageOrdinal{static_cast<std::uint32_t>(stages - 1u)};
  DCE_CHECK_TRUE(!dce::rollout_complete(plan, state));
  mutable_site(state, leader)->accepted_stage = StageOrdinal{static_cast<std::uint32_t>(stages)};
  DCE_CHECK_TRUE(dce::rollout_complete(plan, state));

  RolloutState missing = state;
  missing.sites.erase(std::remove_if(missing.sites.begin(), missing.sites.end(),
                                     [&leader](const SiteProgress& progress) {
                                       return progress.site == leader;
                                     }),
                      missing.sites.end());
  DCE_CHECK_EQ(missing.sites.size(), state.sites.size() - 1u);
  DCE_CHECK_TRUE(!dce::rollout_complete(plan, missing));
}

// ---------------------------------------------------------------------------
// The completion property over seeds and shapes
// ---------------------------------------------------------------------------
DCE_TEST(rollout, the_completion_property_holds_for_every_seed_and_shape) {
  struct Shape {
    std::size_t sites;
    std::size_t components;
    std::size_t cohorts;
    std::size_t versions_per_component;
    const char* name;
  };
  const std::vector<Shape> shapes{
      Shape{1u, 1u, 1u, 2u, "single site, single component"},
      Shape{4u, 1u, 2u, 2u, "two waves of one component"},
      Shape{5u, 2u, 3u, 3u, "three waves, two components"},
      Shape{8u, 3u, 2u, 3u, "two waves, three components"},
      Shape{12u, 2u, 4u, 4u, "four waves, three versions"},
      Shape{7u, 4u, 2u, 2u, "two waves, four components"},
  };
  const std::vector<std::uint64_t> seeds{1u, 2u, 3u, 7u, 11u};

  for (std::uint64_t seed : seeds) {
    for (const Shape& shape : shapes) {
      dce::SyntheticFleetOptions options;
      options.sites = shape.sites;
      options.components = shape.components;
      options.cohorts = shape.cohorts;
      options.versions_per_component = shape.versions_per_component;
      options.seed = seed;
      const dce::Result<dce::SyntheticFleet> fleet = dce::make_synthetic_fleet(options);
      const std::string label = "seed " + std::to_string(seed) + ", " + shape.name;
      if (!fleet.ok()) {
        DCE_FAIL(label + ": make_synthetic_fleet failed: " +
                 dce::test::outcome_detail(fleet.code(), fleet.message()));
        continue;
      }
      const EvolutionPlan& plan = fleet->plan;
      RolloutState state;
      if (!build_state(plan, state, label)) {
        continue;
      }
      if (!dce::rollout_complete(plan, state)) {
        dce::test::count_assertion();
      } else {
        DCE_FAIL(label + ": an untouched fleet is already complete");
      }
      const SweepResult sweep = run_sweep(plan, state, label);
      if (!sweep.completed) {
        DCE_FAIL(label + ": the sweep did not reach completion");
        continue;
      }
      expect_complete(plan, sweep.final_state, label);
    }
  }
}
