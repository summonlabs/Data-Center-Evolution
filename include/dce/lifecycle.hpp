// The plan lifecycle.
//
// Transitions are a total function of (state, event, context): there is no
// ambient state and no implicit transition. An event that is not legal from
// the current state is refused with a reason, and no illegal edge exists in
// the table below, which is public so that documentation and tests derive from
// the same source.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "dce/status.hpp"

namespace dce {

enum class PlanState : std::uint8_t {
  draft,
  validated,
  staged,
  rolling_out,
  paused,
  blocked,
  validating,
  completed,
  aborted,
  rolling_back,
  rolled_back,
  failed,
  reconciled,
};

[[nodiscard]] const char* to_string(PlanState state) noexcept;
[[nodiscard]] Result<PlanState> plan_state_from_string(std::string_view text);

enum class LifecycleEvent : std::uint8_t {
  validate,
  stage,
  begin_rollout,
  pause,
  resume,
  block,
  unblock,
  begin_validation,
  complete,
  abort,
  begin_rollback,
  finish_rollback,
  fail,
  reconcile,
};

[[nodiscard]] const char* to_string(LifecycleEvent event) noexcept;

struct TransitionContext {
  bool gates_satisfied{false};
  bool all_cohorts_advanced{false};
  bool rollback_eligible{false};
  bool point_of_no_return_crossed{false};
  bool reconciliation_rewound{false};
};

struct TransitionOutcome {
  PlanState state{PlanState::draft};
  std::string explanation;
};

struct LegalTransition {
  PlanState from{PlanState::draft};
  LifecycleEvent event{LifecycleEvent::validate};
  PlanState to{PlanState::draft};
};

// The complete transition table, in a deterministic order.
[[nodiscard]] const std::vector<LegalTransition>& legal_transitions();

// Applies an event. Refuses with ErrorCode::refused when the transition is not
// legal for this state, when a guard is not satisfied, or when the event would
// roll back past the point of no return.
[[nodiscard]] Result<TransitionOutcome> transition(PlanState from, LifecycleEvent event,
                                                   const TransitionContext& context);

[[nodiscard]] bool is_terminal(PlanState state) noexcept;
[[nodiscard]] bool is_active(PlanState state) noexcept;

}  // namespace dce
