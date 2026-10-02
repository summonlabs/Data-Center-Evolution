#include "dce/reconcile.hpp"

#include <cstdint>
#include <string>

#include "dce/canonical.hpp"
#include "dce/codec.hpp"
#include "dce/text.hpp"

namespace dce {

const char* to_string(ReconciliationAction action) noexcept {
  switch (action) {
    case ReconciliationAction::agree:
      return "agree";
    case ReconciliationAction::advance:
      return "advance";
    case ReconciliationAction::rewind:
      return "rewind";
    case ReconciliationAction::hold:
      return "hold";
    case ReconciliationAction::reject:
      return "reject";
  }
  return "unknown";
}

namespace {

template <class Tag>
std::string name_of(const BasicId<Tag>& id) {
  return text::escape_for_output(id.view());
}

std::string number(std::uint32_t value) { return text::u32_to_string(value); }
std::string number(std::uint64_t value) { return text::u64_to_string(value); }

// Whenever reconciliation revises the coordinator's belief - advance, rewind or
// hold - it issues the next generation rather than adopting the site's counter:
// a generation that may already have been published must never be reused.
// Agreement revises nothing, and a rejection revises nothing and issues nothing.
Result<SiteGeneration> issue_next_generation(const SiteGeneration& generation) {
  const Result<SiteGeneration> advanced = generation.next();
  if (!advanced.ok()) {
    return Error{ErrorCode::overflow,
                 "site generation is exhausted at its maximum value, so no further authority can "
                 "be issued"};
  }
  return advanced.value();
}

}  // namespace

// plan_generation takes part in no rule below: reconciliation revises a site's
// accepted stage and site generation, while the plan identity and the plan
// generation were already checked when the authority that let the site move was
// issued. The parameter is part of the fixed contract and is therefore
// deliberately unnamed here rather than consulted for a decision it cannot
// change.
Result<ReconciliationOutcome> reconcile(const SiteProgress& coordinator_view,
                                        const SiteStageReport& reported,
                                        const PlanGeneration& /*plan_generation*/,
                                        const CoordinatorEpoch& epoch,
                                        bool site_has_delegated_authority) {
  CanonicalWriter writer;
  const Status view_encoded = codec::put(writer, coordinator_view);
  if (!view_encoded.ok()) {
    return Error{view_encoded.code(),
                 "coordinator view could not be encoded as evidence: " + view_encoded.message()};
  }
  const Status report_encoded = codec::put(writer, reported);
  if (!report_encoded.ok()) {
    return Error{report_encoded.code(),
                 "site report could not be encoded as evidence: " + report_encoded.message()};
  }

  ReconciliationOutcome outcome;
  outcome.evidence_digest = writer.digest();
  // A rejection changes nothing: the coordinator keeps its own belief and the
  // generation it has already published goes on fencing the superseded one.
  outcome.agreed_stage = coordinator_view.accepted_stage;
  outcome.next_generation = coordinator_view.generation;

  if (reported.site != coordinator_view.site) {
    outcome.action = ReconciliationAction::reject;
    outcome.explanation = "report describes site " + name_of(reported.site) +
                          " but this coordinator's view is of site " +
                          name_of(coordinator_view.site) +
                          "; a report about another site proves nothing here";
    return outcome;
  }

  if (reported.epoch > epoch) {
    outcome.action = ReconciliationAction::reject;
    outcome.explanation = "site " + name_of(reported.site) + " reports epoch " +
                          number(reported.epoch.value()) +
                          " but this coordinator has issued only epoch " + number(epoch.value()) +
                          "; a report from an epoch that was never issued cannot be reconciled";
    return outcome;
  }

  if (reported.epoch != epoch && reported.accepted_stage > coordinator_view.accepted_stage) {
    outcome.action = ReconciliationAction::reject;
    outcome.explanation =
        "site " + name_of(reported.site) + " reports epoch " + number(reported.epoch.value()) +
        " while the coordinator is at epoch " + number(epoch.value()) + " and claims stage " +
        number(reported.accepted_stage.value()) + " beyond the coordinator's stage " +
        number(coordinator_view.accepted_stage.value()) +
        "; a report from a superseded epoch cannot advance a site";
    return outcome;
  }

  if (reported.accepted_stage > coordinator_view.accepted_stage &&
      !site_has_delegated_authority) {
    outcome.action = ReconciliationAction::reject;
    outcome.explanation =
        "site " + name_of(reported.site) + " claims stage " +
        number(reported.accepted_stage.value()) + " beyond the coordinator's stage " +
        number(coordinator_view.accepted_stage.value()) +
        " but the plan delegates no rollout authority to it: it advanced beyond the authority it "
        "was given";
    return outcome;
  }

  if (reported.accepted_stage > coordinator_view.accepted_stage) {
    // Reaching this branch means the site does hold delegated authority: the
    // preceding rule refuses every advance that is not delegated.
    const Result<SiteGeneration> next = issue_next_generation(coordinator_view.generation);
    if (!next.ok()) {
      return next.error();
    }
    outcome.action = ReconciliationAction::advance;
    outcome.agreed_stage = reported.accepted_stage;
    outcome.next_generation = next.value();
    outcome.explanation =
        "site " + name_of(reported.site) + " advanced from stage " +
        number(coordinator_view.accepted_stage.value()) + " to stage " +
        number(reported.accepted_stage.value()) +
        " under delegated authority; the coordinator's belief is raised to the accepted stage and "
        "generation " +
        number(outcome.next_generation.value()) +
        " is issued so the superseded generation can never be reused";
    return outcome;
  }

  if (reported.accepted_stage < coordinator_view.accepted_stage) {
    const Result<SiteGeneration> next = issue_next_generation(coordinator_view.generation);
    if (!next.ok()) {
      return next.error();
    }
    outcome.action = ReconciliationAction::rewind;
    outcome.agreed_stage = reported.accepted_stage;
    outcome.next_generation = next.value();
    outcome.explanation =
        "the coordinator's belief that site " + name_of(reported.site) +
        " had accepted stage " + number(coordinator_view.accepted_stage.value()) +
        " exceeded the stage " + number(reported.accepted_stage.value()) +
        " that the site actually accepted; the belief is revised down to the accepted stage before "
        "any new authority is issued, and generation " +
        number(outcome.next_generation.value()) +
        " is issued so the superseded generation can never be reused";
    return outcome;
  }

  if (reported.accepted_stage_digest != coordinator_view.stage_digest) {
    outcome.action = ReconciliationAction::reject;
    outcome.explanation = "site " + name_of(reported.site) +
                          " and the coordinator both claim stage " +
                          number(coordinator_view.accepted_stage.value()) +
                          " but report different stage digests; two irreconcilable claims about "
                          "the same stage cannot be reconciled";
    return outcome;
  }

  if (reported.generation < coordinator_view.generation) {
    const Result<SiteGeneration> next = issue_next_generation(coordinator_view.generation);
    if (!next.ok()) {
      return next.error();
    }
    outcome.action = ReconciliationAction::hold;
    outcome.next_generation = next.value();
    outcome.explanation =
        "site " + name_of(reported.site) + " reports generation " +
        number(reported.generation.value()) + ", earlier than the recorded generation " +
        number(coordinator_view.generation.value()) + ", at stage " +
        number(coordinator_view.accepted_stage.value()) +
        "; authority is withheld until the site re-establishes the current generation, and "
        "generation " +
        number(outcome.next_generation.value()) + " is issued";
    return outcome;
  }

  outcome.action = ReconciliationAction::agree;
  outcome.explanation = "site " + name_of(reported.site) + " reports stage " +
                        number(coordinator_view.accepted_stage.value()) + " at generation " +
                        number(coordinator_view.generation.value()) +
                        ", matching the coordinator's belief exactly; nothing was revised and no "
                        "new authority is issued";
  return outcome;
}

}  // namespace dce
