#include "dce/lifecycle.hpp"

#include <algorithm>
#include <array>

#include "dce/text.hpp"

namespace dce {
namespace {

struct StateName {
  PlanState state;
  std::string_view name;
};

constexpr std::array<StateName, 13> kStateNames = {{
    {PlanState::draft, "draft"},
    {PlanState::validated, "validated"},
    {PlanState::staged, "staged"},
    {PlanState::rolling_out, "rolling_out"},
    {PlanState::paused, "paused"},
    {PlanState::blocked, "blocked"},
    {PlanState::validating, "validating"},
    {PlanState::completed, "completed"},
    {PlanState::aborted, "aborted"},
    {PlanState::rolling_back, "rolling_back"},
    {PlanState::rolled_back, "rolled_back"},
    {PlanState::failed, "failed"},
    {PlanState::reconciled, "reconciled"},
}};

struct EventName {
  LifecycleEvent event;
  std::string_view name;
};

constexpr std::array<EventName, 14> kEventNames = {{
    {LifecycleEvent::validate, "validate"},
    {LifecycleEvent::stage, "stage"},
    {LifecycleEvent::begin_rollout, "begin_rollout"},
    {LifecycleEvent::pause, "pause"},
    {LifecycleEvent::resume, "resume"},
    {LifecycleEvent::block, "block"},
    {LifecycleEvent::unblock, "unblock"},
    {LifecycleEvent::begin_validation, "begin_validation"},
    {LifecycleEvent::complete, "complete"},
    {LifecycleEvent::abort, "abort"},
    {LifecycleEvent::begin_rollback, "begin_rollback"},
    {LifecycleEvent::finish_rollback, "finish_rollback"},
    {LifecycleEvent::fail, "fail"},
    {LifecycleEvent::reconcile, "reconcile"},
}};

// The transition table. Every legal edge of the lifecycle appears exactly
// once, and documentation is generated from this same list.
const std::vector<LegalTransition>& table() {
  static const std::vector<LegalTransition> transitions = {
      {PlanState::draft, LifecycleEvent::validate, PlanState::validated},
      {PlanState::draft, LifecycleEvent::abort, PlanState::aborted},
      {PlanState::draft, LifecycleEvent::fail, PlanState::failed},

      {PlanState::validated, LifecycleEvent::stage, PlanState::staged},
      {PlanState::validated, LifecycleEvent::block, PlanState::blocked},
      {PlanState::validated, LifecycleEvent::abort, PlanState::aborted},
      {PlanState::validated, LifecycleEvent::fail, PlanState::failed},

      {PlanState::staged, LifecycleEvent::begin_rollout, PlanState::rolling_out},
      {PlanState::staged, LifecycleEvent::block, PlanState::blocked},
      {PlanState::staged, LifecycleEvent::abort, PlanState::aborted},
      {PlanState::staged, LifecycleEvent::fail, PlanState::failed},

      {PlanState::rolling_out, LifecycleEvent::pause, PlanState::paused},
      {PlanState::rolling_out, LifecycleEvent::block, PlanState::blocked},
      {PlanState::rolling_out, LifecycleEvent::begin_validation, PlanState::validating},
      {PlanState::rolling_out, LifecycleEvent::begin_rollback, PlanState::rolling_back},
      {PlanState::rolling_out, LifecycleEvent::reconcile, PlanState::reconciled},
      {PlanState::rolling_out, LifecycleEvent::fail, PlanState::failed},

      {PlanState::paused, LifecycleEvent::resume, PlanState::rolling_out},
      {PlanState::paused, LifecycleEvent::block, PlanState::blocked},
      {PlanState::paused, LifecycleEvent::begin_rollback, PlanState::rolling_back},
      {PlanState::paused, LifecycleEvent::abort, PlanState::aborted},
      {PlanState::paused, LifecycleEvent::fail, PlanState::failed},

      {PlanState::blocked, LifecycleEvent::unblock, PlanState::rolling_out},
      {PlanState::blocked, LifecycleEvent::begin_rollback, PlanState::rolling_back},
      {PlanState::blocked, LifecycleEvent::reconcile, PlanState::reconciled},
      {PlanState::blocked, LifecycleEvent::fail, PlanState::failed},

      {PlanState::validating, LifecycleEvent::complete, PlanState::completed},
      {PlanState::validating, LifecycleEvent::begin_rollback, PlanState::rolling_back},
      {PlanState::validating, LifecycleEvent::reconcile, PlanState::reconciled},
      {PlanState::validating, LifecycleEvent::fail, PlanState::failed},

      {PlanState::rolling_back, LifecycleEvent::finish_rollback, PlanState::rolled_back},
      {PlanState::rolling_back, LifecycleEvent::fail, PlanState::failed},

      {PlanState::reconciled, LifecycleEvent::begin_rollout, PlanState::rolling_out},
      {PlanState::reconciled, LifecycleEvent::begin_rollback, PlanState::rolling_back},
      {PlanState::reconciled, LifecycleEvent::abort, PlanState::aborted},
      {PlanState::reconciled, LifecycleEvent::fail, PlanState::failed},
  };
  return transitions;
}

// Guards that must hold before an event may be applied. A guard that is not
// listed is not required by that event.
const char* guard_failure(PlanState from, LifecycleEvent event, const TransitionContext& context) {
  switch (event) {
    case LifecycleEvent::validate:
    case LifecycleEvent::stage:
    case LifecycleEvent::begin_rollout:
    case LifecycleEvent::unblock:
    case LifecycleEvent::complete:
      if (!context.gates_satisfied) {
        return "the plan's blocking gates are not all satisfied";
      }
      return nullptr;
    case LifecycleEvent::begin_validation:
      if (!context.all_cohorts_advanced) {
        return "not every cohort has reached its final stage";
      }
      return nullptr;
    case LifecycleEvent::begin_rollback:
      if (context.point_of_no_return_crossed) {
        return "the point of no return has been crossed, so rollback is no longer available";
      }
      if (!context.rollback_eligible) {
        return "the rollback policy does not permit returning to the requested wave";
      }
      return nullptr;
    case LifecycleEvent::abort:
      if (context.point_of_no_return_crossed) {
        return "the point of no return has been crossed, so the plan cannot be abandoned";
      }
      return nullptr;
    case LifecycleEvent::reconcile:
      if (!context.reconciliation_rewound) {
        return "reconciliation did not revise the coordinator's belief, so no reconciled state applies";
      }
      return nullptr;
    case LifecycleEvent::resume:
    case LifecycleEvent::pause:
    case LifecycleEvent::block:
    case LifecycleEvent::finish_rollback:
    case LifecycleEvent::fail:
      (void)from;
      return nullptr;
  }
  return nullptr;
}

}  // namespace

const char* to_string(PlanState state) noexcept {
  for (const StateName& entry : kStateNames) {
    if (entry.state == state) {
      return entry.name.data();
    }
  }
  return "unknown";
}

Result<PlanState> plan_state_from_string(std::string_view text_value) {
  for (const StateName& entry : kStateNames) {
    if (text::iequals_ascii(entry.name, text_value)) {
      return entry.state;
    }
  }
  return Error{ErrorCode::invalid_argument,
               "unknown plan state: " + text::escape_for_output(text_value)};
}

const char* to_string(LifecycleEvent event) noexcept {
  for (const EventName& entry : kEventNames) {
    if (entry.event == event) {
      return entry.name.data();
    }
  }
  return "unknown";
}

const std::vector<LegalTransition>& legal_transitions() { return table(); }

Result<TransitionOutcome> transition(PlanState from, LifecycleEvent event,
                                     const TransitionContext& context) {
  for (const LegalTransition& candidate : table()) {
    if (candidate.from != from || candidate.event != event) {
      continue;
    }
    if (const char* failure = guard_failure(from, event, context); failure != nullptr) {
      return Error{ErrorCode::refused, std::string("transition refused: ") + failure};
    }
    TransitionOutcome outcome;
    outcome.state = candidate.to;
    outcome.explanation = std::string(to_string(from)) + " + " + to_string(event) + " -> " +
                          to_string(candidate.to);
    return outcome;
  }
  return Error{ErrorCode::refused, std::string("no legal transition from ") + to_string(from) +
                                       " on event " + to_string(event)};
}

bool is_terminal(PlanState state) noexcept {
  return state == PlanState::completed || state == PlanState::aborted ||
         state == PlanState::rolled_back || state == PlanState::failed;
}

bool is_active(PlanState state) noexcept {
  return state == PlanState::rolling_out || state == PlanState::paused ||
         state == PlanState::blocked || state == PlanState::validating ||
         state == PlanState::rolling_back || state == PlanState::reconciled;
}

}  // namespace dce
