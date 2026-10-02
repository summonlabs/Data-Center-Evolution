// The plan lifecycle: the published transition table, the guards that gate it,
// and the classification of its states.
//
// Every case here is driven from legal_transitions() rather than from a copy of
// the table. The full (state x event) cross product is enumerated, so an edge
// that is accepted but not declared and an edge that is declared but not
// accepted both fail the same run, and no undocumented transition can hide.
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "dce/lifecycle.hpp"
#include "harness.hpp"

namespace {

using dce::LifecycleEvent;
using dce::PlanState;
using dce::TransitionContext;

// dce/codec.hpp declares 12 as the highest PlanState enumerator and 13 as the
// highest LifecycleEvent enumerator, so 0..max visits every declared value. The
// table case re-checks both bounds by requiring the first value past each bound
// to render as unknown, which is what makes this enumeration exactly the
// declared domain rather than a guess.
constexpr std::uint8_t kMaxPlanState = 12;
constexpr std::uint8_t kMaxLifecycleEvent = 13;

std::vector<PlanState> all_states() {
  std::vector<PlanState> states;
  states.reserve(static_cast<std::size_t>(kMaxPlanState) + 1u);
  for (std::uint8_t raw = 0; raw <= kMaxPlanState; ++raw) {
    states.push_back(static_cast<PlanState>(raw));
  }
  return states;
}

std::vector<LifecycleEvent> all_events() {
  std::vector<LifecycleEvent> events;
  events.reserve(static_cast<std::size_t>(kMaxLifecycleEvent) + 1u);
  for (std::uint8_t raw = 0; raw <= kMaxLifecycleEvent; ++raw) {
    events.push_back(static_cast<LifecycleEvent>(raw));
  }
  return events;
}

// A context in which every guard the runtime can require holds at once. A
// declared edge is legal exactly when its guards hold, so this is the weakest
// context under which every declared edge must be accepted.
TransitionContext satisfied_context() {
  TransitionContext context;
  context.gates_satisfied = true;
  context.all_cohorts_advanced = true;
  context.rollback_eligible = true;
  context.point_of_no_return_crossed = false;
  context.reconciliation_rewound = true;
  return context;
}

std::string state_name(PlanState state) { return dce::to_string(state); }

std::string event_name(LifecycleEvent event) { return dce::to_string(event); }

std::string pair_name(PlanState from, LifecycleEvent event) {
  return state_name(from) + " + " + event_name(event);
}

std::string to_upper_ascii(std::string_view value) {
  std::string out;
  out.reserve(value.size());
  for (char character : value) {
    const bool lower = character >= 'a' && character <= 'z';
    out.push_back(lower ? static_cast<char>(character - 'a' + 'A') : character);
  }
  return out;
}

// One assertion with a caller-supplied message, for checks whose context (the
// edge, the state, the path) a macro cannot render.
void record_check(bool condition, const std::string& message) {
  if (condition) {
    dce::test::count_assertion();
    return;
  }
  DCE_FAIL(message);
}

const dce::LegalTransition* declared_edge(PlanState from, LifecycleEvent event) {
  for (const dce::LegalTransition& row : dce::legal_transitions()) {
    if (row.from == from && row.event == event) {
      return &row;
    }
  }
  return nullptr;
}

// A declared edge must be accepted under a context that satisfies every guard,
// and it must land on exactly the declared destination.
void expect_declared_edge_accepted(const dce::LegalTransition& edge) {
  const dce::Result<dce::TransitionOutcome> outcome =
      dce::transition(edge.from, edge.event, satisfied_context());
  if (!outcome.ok()) {
    DCE_FAIL("declared edge " + pair_name(edge.from, edge.event) + " -> " + state_name(edge.to) +
             " was refused: " + dce::test::outcome_detail(outcome.code(), outcome.message()));
    return;
  }
  record_check(outcome->state == edge.to,
               "declared edge " + pair_name(edge.from, edge.event) + " landed on " +
                   state_name(outcome->state) + " instead of " + state_name(edge.to));
  record_check(!outcome->explanation.empty(), "declared edge " + pair_name(edge.from, edge.event) +
                                                  " was accepted without an explanation");
}

// A pair that is absent from the table must be refused whatever the guards say.
void expect_undeclared_pair_refused(PlanState from, LifecycleEvent event) {
  const dce::Result<dce::TransitionOutcome> outcome =
      dce::transition(from, event, satisfied_context());
  record_check(outcome.code() == dce::ErrorCode::refused,
               "pair " + pair_name(from, event) + " is absent from the table but was not refused: " +
                   dce::test::outcome_detail(outcome.code(), outcome.message()));
}

// A pair that IS declared must still be refused when its guard fails, and the
// refusal must name the guard's subject rather than the absence of an edge.
void expect_guarded_refusal(PlanState from, LifecycleEvent event, const TransitionContext& context,
                            std::string_view subject) {
  if (declared_edge(from, event) == nullptr) {
    DCE_FAIL("guard case " + pair_name(from, event) +
             " has no declared edge, so its refusal would prove nothing");
    return;
  }
  const dce::Result<dce::TransitionOutcome> outcome = dce::transition(from, event, context);
  if (outcome.ok()) {
    DCE_FAIL(pair_name(from, event) + " was accepted although the guard on " + std::string(subject) +
             " did not hold");
    return;
  }
  record_check(outcome.code() == dce::ErrorCode::refused,
               "guard case " + pair_name(from, event) + " failed with " +
                   dce::test::outcome_detail(outcome.code(), outcome.message()) +
                   " instead of refusing");
  record_check(outcome.message().find(subject) != std::string::npos,
               "the refusal of " + pair_name(from, event) + " does not name " +
                   std::string(subject) + ": " + outcome.message());
}

// Walks a sequence of events that must be legal end to end.
PlanState walk_path(PlanState from, const std::vector<LifecycleEvent>& events,
                    const std::string& label) {
  PlanState state = from;
  const TransitionContext context = satisfied_context();
  for (std::size_t index = 0; index < events.size(); ++index) {
    const dce::Result<dce::TransitionOutcome> outcome =
        dce::transition(state, events[index], context);
    if (!outcome.ok()) {
      DCE_FAIL(label + " was refused at step " + std::to_string(index) + " (" +
               pair_name(state, events[index]) + "): " +
               dce::test::outcome_detail(outcome.code(), outcome.message()));
      return state;
    }
    state = outcome->state;
    dce::test::count_assertion();
  }
  return state;
}

}  // namespace

// ---------------------------------------------------------------------------
// The table is the single source of truth
// ---------------------------------------------------------------------------
DCE_TEST(lifecycle, the_declared_table_is_total_over_the_enumerated_domain) {
  const std::vector<dce::LegalTransition>& table = dce::legal_transitions();
  DCE_CHECK_TRUE(!table.empty());

  // Every row names real enumerators, and no (state, event) pair is declared
  // twice: a duplicate would leave two destinations for one input.
  std::vector<std::string> keys;
  keys.reserve(table.size());
  for (const dce::LegalTransition& edge : table) {
    record_check(state_name(edge.from) != "unknown",
                 "the table declares an edge from a state outside the enumerated domain");
    record_check(state_name(edge.to) != "unknown",
                 "the table declares an edge to a state outside the enumerated domain");
    record_check(event_name(edge.event) != "unknown",
                 "the table declares an event outside the enumerated domain");
    keys.push_back(pair_name(edge.from, edge.event));
  }
  std::sort(keys.begin(), keys.end());
  const auto repeated = std::adjacent_find(keys.begin(), keys.end());
  record_check(repeated == keys.end(),
               repeated == keys.end() ? std::string("no duplicate pair")
                                      : "the table declares " + *repeated + " more than once");

  // The enumeration used by the cross product really is the declared domain:
  // the first value past each bound is not an enumerator.
  DCE_CHECK_EQ(state_name(static_cast<PlanState>(kMaxPlanState + 1u)), std::string("unknown"));
  DCE_CHECK_EQ(event_name(static_cast<LifecycleEvent>(kMaxLifecycleEvent + 1u)),
               std::string("unknown"));
}

DCE_TEST(lifecycle, every_declared_edge_is_accepted_and_every_other_pair_is_refused) {
  const std::vector<PlanState> states = all_states();
  const std::vector<LifecycleEvent> events = all_events();
  std::size_t accepted = 0;
  std::size_t refused = 0;
  for (PlanState from : states) {
    for (LifecycleEvent event : events) {
      const dce::LegalTransition* edge = declared_edge(from, event);
      if (edge != nullptr) {
        expect_declared_edge_accepted(*edge);
        ++accepted;
      } else {
        expect_undeclared_pair_refused(from, event);
        ++refused;
      }
    }
  }
  // Every row of the table was reached through the enumerated domain (a row
  // naming a value outside it would never be visited), and the domain is
  // strictly larger than the table: the refusals above are not vacuous.
  DCE_CHECK_EQ(accepted, dce::legal_transitions().size());
  DCE_CHECK_TRUE(refused > 0);
  DCE_CHECK_TRUE(accepted > 0);
}

DCE_TEST(lifecycle, terminal_and_active_agree_with_the_table) {
  const std::vector<dce::LegalTransition>& table = dce::legal_transitions();
  std::size_t terminal_states = 0;
  std::size_t active_states = 0;
  for (PlanState state : all_states()) {
    const std::string name = state_name(state);
    std::size_t outgoing = 0;
    for (const dce::LegalTransition& edge : table) {
      if (edge.from == state) {
        ++outgoing;
      }
    }
    const bool terminal = dce::is_terminal(state);
    const bool active = dce::is_active(state);
    record_check(!(terminal && active), name + " is reported as both terminal and active");
    if (terminal) {
      ++terminal_states;
      record_check(outgoing == static_cast<std::size_t>(0),
                   name + " is terminal but the table gives it " + std::to_string(outgoing) +
                       " outgoing transition(s)");
    } else {
      record_check(outgoing > static_cast<std::size_t>(0),
                   name + " is not terminal and has no legal event, so it is a dead end");
    }
    if (active) {
      ++active_states;
      record_check(outgoing > static_cast<std::size_t>(0),
                   name + " is active but the table gives it no outgoing transition");
    }
  }
  // A classification that called nothing terminal or nothing active would agree
  // with the table vacuously, so both have to be used.
  DCE_CHECK_TRUE(terminal_states > 0);
  DCE_CHECK_TRUE(active_states > 0);
}

// ---------------------------------------------------------------------------
// Guards
// ---------------------------------------------------------------------------
DCE_TEST(lifecycle, the_gated_events_are_refused_while_the_gates_are_unsatisfied) {
  const TransitionContext unsatisfied{};
  expect_guarded_refusal(PlanState::draft, LifecycleEvent::validate, unsatisfied, "gates");
  expect_guarded_refusal(PlanState::validated, LifecycleEvent::stage, unsatisfied, "gates");
  expect_guarded_refusal(PlanState::staged, LifecycleEvent::begin_rollout, unsatisfied, "gates");
  expect_guarded_refusal(PlanState::blocked, LifecycleEvent::unblock, unsatisfied, "gates");
  expect_guarded_refusal(PlanState::validating, LifecycleEvent::complete, unsatisfied, "gates");

  // The same five edges are accepted once the gates hold, so each refusal above
  // is the guard and not the absence of an edge.
  DCE_CHECK_EQ(state_name(walk_path(PlanState::draft, {LifecycleEvent::validate}, "validate")),
               std::string("validated"));
  DCE_CHECK_EQ(state_name(walk_path(PlanState::validated, {LifecycleEvent::stage}, "stage")),
               std::string("staged"));
  DCE_CHECK_EQ(
      state_name(walk_path(PlanState::staged, {LifecycleEvent::begin_rollout}, "begin_rollout")),
      std::string("rolling_out"));
  DCE_CHECK_EQ(state_name(walk_path(PlanState::blocked, {LifecycleEvent::unblock}, "unblock")),
               std::string("rolling_out"));
  DCE_CHECK_EQ(state_name(walk_path(PlanState::validating, {LifecycleEvent::complete}, "complete")),
               std::string("completed"));
}

DCE_TEST(lifecycle, begin_validation_requires_every_cohort_to_have_advanced) {
  TransitionContext waiting = satisfied_context();
  waiting.all_cohorts_advanced = false;
  expect_guarded_refusal(PlanState::rolling_out, LifecycleEvent::begin_validation, waiting, "cohort");
  DCE_CHECK_EQ(
      state_name(walk_path(PlanState::rolling_out, {LifecycleEvent::begin_validation}, "validation")),
      std::string("validating"));
}

DCE_TEST(lifecycle, rollback_requires_eligibility_and_an_uncrossed_boundary) {
  TransitionContext ineligible = satisfied_context();
  ineligible.rollback_eligible = false;
  expect_guarded_refusal(PlanState::rolling_out, LifecycleEvent::begin_rollback, ineligible,
                         "rollback policy");

  TransitionContext crossed = satisfied_context();
  crossed.point_of_no_return_crossed = true;
  expect_guarded_refusal(PlanState::rolling_out, LifecycleEvent::begin_rollback, crossed,
                         "point of no return");
  expect_guarded_refusal(PlanState::paused, LifecycleEvent::begin_rollback, crossed,
                         "point of no return");
  expect_guarded_refusal(PlanState::blocked, LifecycleEvent::begin_rollback, crossed,
                         "point of no return");
  expect_guarded_refusal(PlanState::validating, LifecycleEvent::begin_rollback, crossed,
                         "point of no return");
  expect_guarded_refusal(PlanState::reconciled, LifecycleEvent::begin_rollback, crossed,
                         "point of no return");

  // With the boundary uncrossed and the policy content, rollback starts from
  // every state that declares the edge.
  for (PlanState from : {PlanState::rolling_out, PlanState::paused, PlanState::blocked,
                         PlanState::validating, PlanState::reconciled}) {
    DCE_CHECK_EQ(state_name(walk_path(from, {LifecycleEvent::begin_rollback}, "begin_rollback")),
                 std::string("rolling_back"));
  }
}

DCE_TEST(lifecycle, abort_is_refused_after_the_point_of_no_return) {
  TransitionContext crossed = satisfied_context();
  crossed.point_of_no_return_crossed = true;
  expect_guarded_refusal(PlanState::draft, LifecycleEvent::abort, crossed, "point of no return");
  expect_guarded_refusal(PlanState::paused, LifecycleEvent::abort, crossed, "point of no return");
  expect_guarded_refusal(PlanState::reconciled, LifecycleEvent::abort, crossed, "point of no return");

  DCE_CHECK_EQ(state_name(walk_path(PlanState::draft, {LifecycleEvent::abort}, "abort")),
               std::string("aborted"));
}

DCE_TEST(lifecycle, reconcile_requires_a_rewound_belief) {
  TransitionContext not_rewound = satisfied_context();
  not_rewound.reconciliation_rewound = false;
  expect_guarded_refusal(PlanState::rolling_out, LifecycleEvent::reconcile, not_rewound,
                         "reconciliation");
  expect_guarded_refusal(PlanState::blocked, LifecycleEvent::reconcile, not_rewound,
                         "reconciliation");
  expect_guarded_refusal(PlanState::validating, LifecycleEvent::reconcile, not_rewound,
                         "reconciliation");
  DCE_CHECK_EQ(
      state_name(walk_path(PlanState::rolling_out, {LifecycleEvent::reconcile}, "reconcile")),
      std::string("reconciled"));
}

// ---------------------------------------------------------------------------
// Paths
// ---------------------------------------------------------------------------
DCE_TEST(lifecycle, the_happy_path_runs_draft_to_completed) {
  PlanState state = PlanState::draft;
  DCE_CHECK_TRUE(!dce::is_terminal(state));
  DCE_CHECK_TRUE(!dce::is_active(state));

  state = walk_path(state, {LifecycleEvent::validate}, "happy path: validate");
  DCE_CHECK_EQ(state_name(state), std::string("validated"));
  state = walk_path(state, {LifecycleEvent::stage}, "happy path: stage");
  DCE_CHECK_EQ(state_name(state), std::string("staged"));
  state = walk_path(state, {LifecycleEvent::begin_rollout}, "happy path: begin_rollout");
  DCE_CHECK_EQ(state_name(state), std::string("rolling_out"));
  DCE_CHECK_TRUE(dce::is_active(state));
  state = walk_path(state, {LifecycleEvent::begin_validation}, "happy path: begin_validation");
  DCE_CHECK_EQ(state_name(state), std::string("validating"));
  state = walk_path(state, {LifecycleEvent::complete}, "happy path: complete");
  DCE_CHECK_EQ(state_name(state), std::string("completed"));
  DCE_CHECK_TRUE(dce::is_terminal(state));
  DCE_CHECK_TRUE(!dce::is_active(state));
}

DCE_TEST(lifecycle, every_other_documented_path_reaches_its_declared_state) {
  const PlanState paused =
      walk_path(PlanState::draft,
                {LifecycleEvent::validate, LifecycleEvent::stage, LifecycleEvent::begin_rollout,
                 LifecycleEvent::pause},
                "paused path");
  DCE_CHECK_EQ(state_name(paused), std::string("paused"));
  DCE_CHECK_TRUE(dce::is_active(paused));
  DCE_CHECK_EQ(state_name(walk_path(paused, {LifecycleEvent::resume}, "resume")),
               std::string("rolling_out"));

  const PlanState blocked =
      walk_path(PlanState::draft, {LifecycleEvent::validate, LifecycleEvent::block},
                "blocked from validated");
  DCE_CHECK_EQ(state_name(blocked), std::string("blocked"));
  DCE_CHECK_TRUE(dce::is_active(blocked));
  DCE_CHECK_EQ(state_name(walk_path(blocked, {LifecycleEvent::unblock}, "unblock")),
               std::string("rolling_out"));
  DCE_CHECK_EQ(state_name(walk_path(PlanState::draft,
                                    {LifecycleEvent::validate, LifecycleEvent::stage,
                                     LifecycleEvent::begin_rollout, LifecycleEvent::block},
                                    "blocked from rolling_out")),
               std::string("blocked"));

  const PlanState reconciled =
      walk_path(PlanState::draft,
                {LifecycleEvent::validate, LifecycleEvent::stage, LifecycleEvent::begin_rollout,
                 LifecycleEvent::reconcile},
                "reconciled path");
  DCE_CHECK_EQ(state_name(reconciled), std::string("reconciled"));
  DCE_CHECK_TRUE(dce::is_active(reconciled));
  DCE_CHECK_EQ(state_name(walk_path(reconciled, {LifecycleEvent::begin_rollout},
                                    "rolling out again after reconciliation")),
               std::string("rolling_out"));

  const PlanState rolling_back =
      walk_path(PlanState::draft,
                {LifecycleEvent::validate, LifecycleEvent::stage, LifecycleEvent::begin_rollout,
                 LifecycleEvent::begin_rollback},
                "rollback path");
  DCE_CHECK_EQ(state_name(rolling_back), std::string("rolling_back"));
  DCE_CHECK_TRUE(dce::is_active(rolling_back));
  DCE_CHECK_EQ(state_name(walk_path(rolling_back, {LifecycleEvent::finish_rollback},
                                    "finish rollback")),
               std::string("rolled_back"));

  const PlanState failed = walk_path(PlanState::draft, {LifecycleEvent::fail}, "failed path");
  DCE_CHECK_EQ(state_name(failed), std::string("failed"));

  const PlanState aborted = walk_path(PlanState::draft, {LifecycleEvent::abort}, "aborted path");
  DCE_CHECK_EQ(state_name(aborted), std::string("aborted"));
  DCE_CHECK_EQ(state_name(walk_path(PlanState::draft,
                                    {LifecycleEvent::validate, LifecycleEvent::stage,
                                     LifecycleEvent::begin_rollout, LifecycleEvent::pause,
                                     LifecycleEvent::abort},
                                    "abort from paused")),
               std::string("aborted"));

  for (PlanState terminal :
       {PlanState::completed, PlanState::aborted, PlanState::rolled_back, PlanState::failed}) {
    DCE_CHECK_TRUE(dce::is_terminal(terminal));
    DCE_CHECK_TRUE(!dce::is_active(terminal));
  }
}

// ---------------------------------------------------------------------------
// Names
// ---------------------------------------------------------------------------
DCE_TEST(lifecycle, state_names_round_trip_and_parsing_is_case_insensitive) {
  std::vector<std::string> names;
  for (PlanState state : all_states()) {
    const std::string name = state_name(state);
    record_check(name != "unknown", "a state in the enumerated domain has no name");
    names.push_back(name);

    const dce::Result<PlanState> exact = dce::plan_state_from_string(name);
    if (!exact.ok()) {
      DCE_FAIL("state name " + name + " does not parse: " +
               dce::test::outcome_detail(exact.code(), exact.message()));
      continue;
    }
    record_check(exact.value() == state,
                 "state name " + name + " parsed as " + state_name(exact.value()));

    const dce::Result<PlanState> upper = dce::plan_state_from_string(to_upper_ascii(name));
    if (!upper.ok()) {
      DCE_FAIL("state name " + to_upper_ascii(name) + " does not parse: " +
               dce::test::outcome_detail(upper.code(), upper.message()));
      continue;
    }
    record_check(upper.value() == state,
                 "state name " + to_upper_ascii(name) + " parsed as " + state_name(upper.value()));
  }

  std::sort(names.begin(), names.end());
  record_check(std::adjacent_find(names.begin(), names.end()) == names.end(),
               "two states share a name, so the round trip is ambiguous");

  // Documented case insensitivity, including mixed case.
  const dce::Result<PlanState> mixed = dce::plan_state_from_string("RoLLiNg_OuT");
  if (mixed.ok()) {
    record_check(mixed.value() == PlanState::rolling_out, "mixed-case parsing returned the wrong state");
  } else {
    DCE_FAIL("mixed-case state name was rejected: " +
             dce::test::outcome_detail(mixed.code(), mixed.message()));
  }

  DCE_CHECK_CODE(dce::plan_state_from_string("not-a-state"), dce::ErrorCode::invalid_argument);
  DCE_CHECK_CODE(dce::plan_state_from_string(""), dce::ErrorCode::invalid_argument);
  DCE_CHECK_CODE(dce::plan_state_from_string("draft "), dce::ErrorCode::invalid_argument);
}

DCE_TEST(lifecycle, every_event_has_a_distinct_name) {
  std::vector<std::string> names;
  for (LifecycleEvent event : all_events()) {
    const std::string name = event_name(event);
    record_check(name != "unknown", "an event in the enumerated domain has no name");
    names.push_back(name);
  }
  std::sort(names.begin(), names.end());
  record_check(std::adjacent_find(names.begin(), names.end()) == names.end(),
               "two events share a name");
}
