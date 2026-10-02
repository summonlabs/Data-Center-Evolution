// A worked example of the evolution boundary doing its job.
//
// Everything below is driven through the public API, exactly as a coordinator
// process would drive it: generate a deterministic fleet, persist its plan
// canonically, validate the plan against the fleet the coordinator actually
// observes, walk the lifecycle, and take the first deterministic rollout
// decisions. No private header and no test support library is involved.
//
// Every step is checked. A refusal is printed with its code, subject and
// explanation, and any failure exits non-zero with a clear message, because an
// example that ignores an error teaches the wrong thing.

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>

#include "dce/canonical.hpp"
#include "dce/codec.hpp"
#include "dce/digest.hpp"
#include "dce/lifecycle.hpp"
#include "dce/plan.hpp"
#include "dce/rollout.hpp"
#include "dce/status.hpp"
#include "dce/synthetic.hpp"
#include "dce/text.hpp"
#include "dce/validate.hpp"

namespace {

// dce::Status and dce::Result<T> carry the same outcome vocabulary, so one
// check covers every call in this example.
template <class Outcome>
[[nodiscard]] bool check(const Outcome& outcome, std::string_view step) {
  if (outcome.ok()) {
    return true;
  }
  std::cerr << "evolve_fleet: " << step << ": " << dce::to_string(outcome.code()) << ": "
            << dce::text::escape_for_output(outcome.message()) << '\n';
  return false;
}

[[nodiscard]] std::string number(std::uint64_t value) { return dce::text::u64_to_string(value); }

[[nodiscard]] std::string stage_number(dce::StageOrdinal stage) {
  return dce::text::u32_to_string(stage.value());
}

[[nodiscard]] dce::SiteProgress* find_progress(dce::RolloutState& state, const dce::SiteId& site) {
  for (dce::SiteProgress& progress : state.sites) {
    if (progress.site == site) {
      return &progress;
    }
  }
  return nullptr;
}

int run() {
  // --- 1. A deterministic fleet: six sites, two components, two cohorts -----
  dce::SyntheticFleetOptions options;
  options.sites = 6;
  options.components = 2;
  options.cohorts = 2;
  options.seed = 20260101;

  auto fleet_result = dce::make_synthetic_fleet(options);
  if (!check(fleet_result, "make_synthetic_fleet")) {
    return EXIT_FAILURE;
  }
  dce::SyntheticFleet fleet = std::move(fleet_result).value();
  dce::EvolutionPlan& plan = fleet.plan;

  // Canonical order is what makes the digest a function of content alone; it
  // also refreshes the plan and membership digests that validation checks.
  if (!check(dce::canonicalize(plan), "canonicalize")) {
    return EXIT_FAILURE;
  }

  // --- 2. Persist the plan in its canonical form ---------------------------
  dce::CanonicalWriter writer;
  if (!check(dce::codec::put(writer, plan), "codec::put<EvolutionPlan>")) {
    return EXIT_FAILURE;
  }
  std::cout << "plan:          " << dce::text::escape_for_output(plan.identity.id.view()) << '\n';
  std::cout << "title:         " << dce::text::escape_for_output(plan.title) << '\n';
  std::cout << "digest:        " << plan.digest.to_hex() << '\n';
  std::cout << "canonical:     " << number(static_cast<std::uint64_t>(writer.size())) << " bytes\n";
  std::cout << "sites:         " << number(static_cast<std::uint64_t>(plan.membership.sites.size()))
            << '\n';
  std::cout << "steps:         " << number(static_cast<std::uint64_t>(plan.steps.size())) << '\n';
  std::cout << "cohorts:       " << number(static_cast<std::uint64_t>(plan.cohorts.size())) << '\n';
  std::cout << "version range: " << plan.source_version.to_string() << " -> "
            << plan.target_version.to_string() << '\n';

  // --- 3. Real validation against the observed fleet -----------------------
  dce::ValidationContext context;
  context.epoch = plan.identity.epoch;
  context.current_generation = plan.identity.generation;
  context.observed_sites = fleet.initial_observations;

  auto report_result = dce::validate_plan(plan, context);
  if (!check(report_result, "validate_plan")) {
    return EXIT_FAILURE;
  }
  const dce::ValidationReport& report = report_result.value();

  std::cout << "\nvalidation:    ";
  if (report.accepted()) {
    std::cout << "accepted\n";
  } else {
    std::cout << number(static_cast<std::uint64_t>(report.refusals.size())) << " refusal(s)\n";
    for (const dce::Refusal& refusal : report.refusals) {
      std::cout << "  refusal " << dce::to_string(refusal.code) << " ["
                << dce::text::escape_for_output(refusal.subject)
                << "]: " << dce::text::escape_for_output(refusal.explanation) << '\n';
    }
  }
  std::cout << "checked:       sites=" << number(report.checked_sites)
            << " steps=" << number(report.checked_steps) << " edges=" << number(report.checked_edges)
            << " gates=" << number(report.checked_gates) << '\n';

  // --- 4. The lifecycle: validate -> stage -> begin_rollout ----------------
  // Each event is a total function of (state, event, context): the guard an
  // event needs is supplied here rather than assumed by the boundary.
  dce::TransitionContext transition_context;
  transition_context.gates_satisfied = report.accepted();

  dce::PlanState state = dce::PlanState::draft;
  std::cout << "\nlifecycle:\n";
  const auto apply = [&transition_context, &state](dce::LifecycleEvent event) {
    const auto outcome = dce::transition(state, event, transition_context);
    if (!check(outcome, std::string("transition ") + dce::to_string(event))) {
      return false;
    }
    std::cout << "  " << dce::to_string(event) << ": " << dce::to_string(state) << " -> "
              << dce::to_string(outcome->state) << " ("
              << dce::text::escape_for_output(outcome->explanation) << ")\n";
    state = outcome->state;
    return true;
  };
  if (!apply(dce::LifecycleEvent::validate) || !apply(dce::LifecycleEvent::stage) ||
      !apply(dce::LifecycleEvent::begin_rollout)) {
    return EXIT_FAILURE;
  }

  // --- 5. The first deterministic rollout decisions ------------------------
  // A site's stage ordinal is the number of steps it has durably accepted, and
  // a decision may only ever move it forward by exactly one: that is how "no
  // site skips a gate" is enforced structurally rather than by convention.
  auto initial_state = dce::initial_rollout_state(plan);
  if (!check(initial_state, "initial_rollout_state")) {
    return EXIT_FAILURE;
  }
  dce::RolloutState rollout = std::move(initial_state).value();

  constexpr std::size_t kDecisionsPrinted = 8;
  std::cout << "\nrollout decisions:\n";
  std::size_t decisions = 0;
  std::size_t advances = 0;
  while (decisions < kDecisionsPrinted) {
    const auto action_result = dce::next_rollout_action(plan, rollout);
    if (!check(action_result, "next_rollout_action")) {
      return EXIT_FAILURE;
    }
    const dce::RolloutAction& action = action_result.value();
    std::cout << "  " << number(static_cast<std::uint64_t>(decisions + 1)) << ". "
              << dce::to_string(action.kind);
    if (action.site.valid()) {
      std::cout << " site=" << dce::text::escape_for_output(action.site.view());
    }
    if (action.cohort.valid()) {
      std::cout << " cohort=" << dce::text::escape_for_output(action.cohort.view());
    }
    if (action.step.valid()) {
      std::cout << " step=" << dce::text::escape_for_output(action.step.view());
    }

    bool progressed = false;
    if (action.kind == dce::RolloutActionKind::advance_site) {
      dce::SiteProgress* progress = find_progress(rollout, action.site);
      if (progress == nullptr) {
        std::cerr << "evolve_fleet: the rollout state does not describe site "
                  << dce::text::escape_for_output(action.site.view()) << '\n';
        return EXIT_FAILURE;
      }
      const std::uint32_t before = progress->accepted_stage.value();
      if (action.target_stage.value() != before + 1U) {
        std::cerr << "evolve_fleet: site " << dce::text::escape_for_output(action.site.view())
                  << " would advance from stage " << stage_number(progress->accepted_stage)
                  << " to stage " << stage_number(action.target_stage)
                  << ", which is more than one stage\n";
        return EXIT_FAILURE;
      }
      std::cout << " stage " << dce::text::u32_to_string(before) << " -> "
                << stage_number(action.target_stage);
      progress->accepted_stage = action.target_stage;
      ++advances;
      progressed = true;
    } else if (action.kind == dce::RolloutActionKind::advance_wave) {
      rollout.current_wave = action.wave;
      std::cout << " wave " << dce::text::u32_to_string(action.wave.value());
      progressed = true;
    } else if (action.kind == dce::RolloutActionKind::await_receipt) {
      for (dce::SiteProgress& progress : rollout.sites) {
        progress.in_flight = false;
      }
      progressed = true;
    }
    std::cout << '\n';
    ++decisions;
    if (!progressed) {
      break;  // completed, paused, gate_blocked or refused: the rollout stops here
    }
  }
  std::cout << "  " << number(static_cast<std::uint64_t>(decisions)) << " decision(s), "
            << number(static_cast<std::uint64_t>(advances))
            << " single-stage advance(s); no site advanced by more than one stage\n";

  // --- 6. One line a script or an operator can act on ----------------------
  std::cout << "\nsummary: plan " << dce::text::escape_for_output(plan.identity.id.view())
            << " digest " << plan.digest.to_hex() << " state " << dce::to_string(state)
            << " validation " << (report.accepted() ? "accepted" : "refused") << " decisions "
            << number(static_cast<std::uint64_t>(decisions)) << " advances "
            << number(static_cast<std::uint64_t>(advances)) << '\n';
  return EXIT_SUCCESS;
}

}  // namespace

int main() { return run(); }
