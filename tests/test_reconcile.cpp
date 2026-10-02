// Reconciliation of a reconnecting site: agreeing with reality, rewinding a
// belief that ran ahead, honouring a delegated advance, and refusing a report
// that contradicts the authority the site was given.
//
// Fencing is the property that matters most here: whenever reconciliation does
// not simply agree, the generation it issues must be strictly ahead of every
// generation issued before, so a stale completion can never mutate a newer one.
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "dce/random.hpp"
#include "dce/reconcile.hpp"
#include "harness.hpp"

namespace {

using dce::CoordinatorEpoch;
using dce::PlanGeneration;
using dce::ReconciliationAction;
using dce::ReconciliationOutcome;
using dce::SiteGeneration;
using dce::SiteProgress;
using dce::SiteStageReport;
using dce::StageOrdinal;

// One assertion with a caller-supplied message, for checks whose context (the
// seed, the iteration, the field) a macro cannot render.
void record_check(bool condition, const std::string& message) {
  if (condition) {
    dce::test::count_assertion();
    return;
  }
  DCE_FAIL(message);
}

const dce::SiteId& the_site() {
  static const dce::SiteId value = *dce::SiteId::parse("site-1");
  return value;
}

const dce::SiteId& other_site() {
  static const dce::SiteId value = *dce::SiteId::parse("site-2");
  return value;
}

const CoordinatorEpoch& the_epoch() {
  static const CoordinatorEpoch value{3};
  return value;
}

const PlanGeneration& the_plan_generation() {
  static const PlanGeneration value{2};
  return value;
}

// One digest per stage ordinal, so a stage and its digest can be varied
// independently.
const std::vector<dce::Digest256>& stage_digests() {
  static const std::vector<dce::Digest256> value = [] {
    std::vector<dce::Digest256> built;
    for (std::uint32_t index = 0; index <= 4u; ++index) {
      built.push_back(dce::Digest256::of("stage-" + std::to_string(index)));
    }
    return built;
  }();
  return value;
}

SiteProgress view_of(const dce::SiteId& site, std::uint32_t stage, std::uint64_t generation,
                     const dce::Digest256& digest) {
  SiteProgress view;
  view.site = site;
  view.accepted_stage = StageOrdinal{stage};
  view.generation = SiteGeneration::from_value(generation);
  view.stage_digest = digest;
  return view;
}

SiteStageReport report_of(const dce::SiteId& site, const CoordinatorEpoch& epoch,
                          std::uint32_t stage, std::uint64_t generation,
                          const dce::Digest256& digest) {
  SiteStageReport report;
  report.site = site;
  report.epoch = epoch;
  report.accepted_stage = StageOrdinal{stage};
  report.generation = SiteGeneration::from_value(generation);
  report.accepted_stage_digest = digest;
  report.partitioned = false;
  return report;
}

void expect_action(const dce::Result<ReconciliationOutcome>& outcome,
                   ReconciliationAction expected, const std::string& label) {
  if (!outcome.ok()) {
    DCE_FAIL(label + ": reconcile failed: " +
             dce::test::outcome_detail(outcome.code(), outcome.message()));
    return;
  }
  record_check(outcome->action == expected,
               label + ": expected " + dce::to_string(expected) + ", got " +
                   dce::to_string(outcome->action) + " (" + outcome->explanation + ")");
  record_check(!outcome->explanation.empty(), label + ": the outcome carries no explanation");
}

bool evidence_of(const SiteProgress& view, const SiteStageReport& report, dce::Digest256& out,
                 const std::string& label) {
  const dce::Result<ReconciliationOutcome> outcome =
      dce::reconcile(view, report, the_plan_generation(), the_epoch(), true);
  if (!outcome.ok()) {
    DCE_FAIL(label + ": reconcile failed: " +
             dce::test::outcome_detail(outcome.code(), outcome.message()));
    return false;
  }
  out = outcome->evidence_digest;
  return true;
}

// Two reconciliations of different pairs must not produce the same evidence.
void expect_different_evidence(const SiteProgress& first_view,
                               const SiteStageReport& first_report,
                               const SiteProgress& second_view,
                               const SiteStageReport& second_report,
                               const std::string& field) {
  dce::Digest256 first;
  dce::Digest256 second;
  DCE_REQUIRE(evidence_of(first_view, first_report, first, "evidence of " + field));
  DCE_REQUIRE(evidence_of(second_view, second_report, second, "evidence of " + field));
  record_check(!(first == second), "changing the " + field + " did not change the evidence digest");
}

}  // namespace

// ---------------------------------------------------------------------------
// One case per action
// ---------------------------------------------------------------------------
DCE_TEST(reconcile, a_report_that_matches_the_belief_agrees) {
  const std::vector<dce::Digest256>& digests = stage_digests();
  const SiteProgress view = view_of(the_site(), 3, 5, digests[3]);
  const SiteStageReport report = report_of(the_site(), the_epoch(), 3, 5, digests[3]);

  const dce::Result<ReconciliationOutcome> outcome =
      dce::reconcile(view, report, the_plan_generation(), the_epoch(), true);
  expect_action(outcome, ReconciliationAction::agree, "matching report");
  DCE_REQUIRE_OK(outcome);
  DCE_CHECK_EQ(outcome->agreed_stage.value(), 3u);
  // Nothing was revised, so no new authority is issued.
  DCE_CHECK_EQ(outcome->next_generation.value(), 5u);

  // A site that reports a generation the coordinator has not seen is still in
  // agreement about the stage, and still gets no new authority from a report
  // that proves nothing new.
  const SiteStageReport newer = report_of(the_site(), the_epoch(), 3, 9, digests[3]);
  const dce::Result<ReconciliationOutcome> ahead =
      dce::reconcile(view, newer, the_plan_generation(), the_epoch(), true);
  expect_action(ahead, ReconciliationAction::agree, "newer generation, same stage");
  DCE_REQUIRE_OK(ahead);
  DCE_CHECK_EQ(ahead->agreed_stage.value(), 3u);
  DCE_CHECK_EQ(ahead->next_generation.value(), 5u);
}

DCE_TEST(reconcile, a_site_behind_the_belief_rewinds_and_issues_a_newer_generation) {
  const std::vector<dce::Digest256>& digests = stage_digests();
  const SiteProgress view = view_of(the_site(), 3, 5, digests[3]);
  const SiteStageReport report = report_of(the_site(), the_epoch(), 2, 5, digests[2]);

  const dce::Result<ReconciliationOutcome> outcome =
      dce::reconcile(view, report, the_plan_generation(), the_epoch(), false);
  expect_action(outcome, ReconciliationAction::rewind, "site behind the belief");
  DCE_REQUIRE_OK(outcome);
  // The coordinator's belief is revised down to what the site actually kept.
  DCE_CHECK_EQ(outcome->agreed_stage.value(), 2u);
  // The generation is not adopted from the site: it moves past the one the
  // coordinator may already have published.
  DCE_CHECK_EQ(outcome->next_generation.value(), 6u);
  record_check(outcome->next_generation.value() > view.generation.value(),
               "a rewind did not issue a generation ahead of the coordinator's");
}

DCE_TEST(reconcile, a_site_ahead_with_delegated_authority_advances) {
  const std::vector<dce::Digest256>& digests = stage_digests();
  const SiteProgress view = view_of(the_site(), 3, 5, digests[3]);
  const SiteStageReport report = report_of(the_site(), the_epoch(), 4, 9, digests[4]);

  const dce::Result<ReconciliationOutcome> outcome =
      dce::reconcile(view, report, the_plan_generation(), the_epoch(), true);
  expect_action(outcome, ReconciliationAction::advance, "delegated advance");
  DCE_REQUIRE_OK(outcome);
  DCE_CHECK_EQ(outcome->agreed_stage.value(), 4u);
  DCE_CHECK_EQ(outcome->next_generation.value(), 6u);
}

DCE_TEST(reconcile, a_site_ahead_without_delegated_authority_is_rejected) {
  const std::vector<dce::Digest256>& digests = stage_digests();
  const SiteProgress view = view_of(the_site(), 3, 5, digests[3]);
  const SiteStageReport report = report_of(the_site(), the_epoch(), 4, 9, digests[4]);

  const dce::Result<ReconciliationOutcome> outcome =
      dce::reconcile(view, report, the_plan_generation(), the_epoch(), false);
  expect_action(outcome, ReconciliationAction::reject, "sovereign site ahead");
  DCE_REQUIRE_OK(outcome);
  // A rejection changes nothing: the coordinator keeps its own belief.
  DCE_CHECK_EQ(outcome->agreed_stage.value(), 3u);
  record_check(outcome->next_generation.value() >= view.generation.value(),
               "a rejection moved the generation backwards");
  record_check(outcome->explanation.find("authority") != std::string::npos,
               "the rejection does not say the advance was not delegated: " + outcome->explanation);
}

DCE_TEST(reconcile, a_stage_digest_conflict_at_the_same_stage_is_rejected) {
  const std::vector<dce::Digest256>& digests = stage_digests();
  const SiteProgress view = view_of(the_site(), 3, 5, digests[3]);
  const SiteStageReport report = report_of(the_site(), the_epoch(), 3, 5,
                                           dce::Digest256::of("a different stage"));

  const dce::Result<ReconciliationOutcome> outcome =
      dce::reconcile(view, report, the_plan_generation(), the_epoch(), true);
  expect_action(outcome, ReconciliationAction::reject, "two claims about one stage");
  DCE_REQUIRE_OK(outcome);
  DCE_CHECK_EQ(outcome->agreed_stage.value(), 3u);
  record_check(outcome->explanation.find("digest") != std::string::npos,
               "the rejection does not say the digests disagree: " + outcome->explanation);
}

DCE_TEST(reconcile, a_report_about_another_site_is_rejected) {
  const std::vector<dce::Digest256>& digests = stage_digests();
  const SiteProgress view = view_of(the_site(), 3, 5, digests[3]);
  const SiteStageReport report = report_of(other_site(), the_epoch(), 3, 5, digests[3]);

  const dce::Result<ReconciliationOutcome> outcome =
      dce::reconcile(view, report, the_plan_generation(), the_epoch(), true);
  expect_action(outcome, ReconciliationAction::reject, "another site");
  DCE_REQUIRE_OK(outcome);
  DCE_CHECK_EQ(outcome->agreed_stage.value(), 3u);
  record_check(outcome->explanation.find(the_site().str()) != std::string::npos &&
                   outcome->explanation.find(other_site().str()) != std::string::npos,
               "the rejection does not name both sites: " + outcome->explanation);
}

DCE_TEST(reconcile, a_report_from_an_epoch_the_coordinator_never_issued_is_rejected) {
  const std::vector<dce::Digest256>& digests = stage_digests();
  const SiteProgress view = view_of(the_site(), 3, 5, digests[3]);

  // Even a report that claims to be behind is refused: an epoch the coordinator
  // has not issued cannot be used to move any belief, in either direction.
  const SiteStageReport future = report_of(the_site(), CoordinatorEpoch{9}, 2, 5, digests[2]);
  const dce::Result<ReconciliationOutcome> ahead =
      dce::reconcile(view, future, the_plan_generation(), the_epoch(), true);
  expect_action(ahead, ReconciliationAction::reject, "epoch never issued, behind");
  DCE_REQUIRE_OK(ahead);
  DCE_CHECK_EQ(ahead->agreed_stage.value(), 3u);
  record_check(ahead->explanation.find("9") != std::string::npos,
               "the rejection does not name the reported epoch: " + ahead->explanation);

  const SiteStageReport future_and_ahead = report_of(the_site(), CoordinatorEpoch{9}, 4, 5, digests[4]);
  expect_action(dce::reconcile(view, future_and_ahead, the_plan_generation(), the_epoch(), true),
                ReconciliationAction::reject, "epoch never issued, ahead");

  // A report from a superseded epoch may not advance the site either.
  const SiteStageReport superseded = report_of(the_site(), CoordinatorEpoch{1}, 4, 5, digests[4]);
  const dce::Result<ReconciliationOutcome> stale =
      dce::reconcile(view, superseded, the_plan_generation(), the_epoch(), true);
  expect_action(stale, ReconciliationAction::reject, "superseded epoch claiming an advance");
  DCE_REQUIRE_OK(stale);
  DCE_CHECK_EQ(stale->agreed_stage.value(), 3u);
}

DCE_TEST(reconcile, an_earlier_generation_at_the_same_stage_holds_and_still_moves_ahead) {
  const std::vector<dce::Digest256>& digests = stage_digests();
  const SiteProgress view = view_of(the_site(), 3, 5, digests[3]);
  const SiteStageReport report = report_of(the_site(), the_epoch(), 3, 2, digests[3]);

  const dce::Result<ReconciliationOutcome> outcome =
      dce::reconcile(view, report, the_plan_generation(), the_epoch(), true);
  expect_action(outcome, ReconciliationAction::hold, "earlier generation");
  DCE_REQUIRE_OK(outcome);
  DCE_CHECK_EQ(outcome->agreed_stage.value(), 3u);
  // Authority stays withheld until the site re-establishes the current
  // generation, and the generation issued is still ahead.
  DCE_CHECK_EQ(outcome->next_generation.value(), 6u);
  record_check(outcome->explanation.find("6") != std::string::npos,
               "the hold does not name the generation it issues: " + outcome->explanation);
}

// ---------------------------------------------------------------------------
// Evidence
// ---------------------------------------------------------------------------
DCE_TEST(reconcile, the_evidence_digest_covers_the_reconciled_pair) {
  const std::vector<dce::Digest256>& digests = stage_digests();
  const SiteProgress view = view_of(the_site(), 3, 5, digests[3]);
  const SiteStageReport report = report_of(the_site(), the_epoch(), 3, 5, digests[3]);

  dce::Digest256 baseline;
  DCE_REQUIRE(evidence_of(view, report, baseline, "baseline"));
  record_check(!baseline.is_zero(), "the evidence digest is the documented no-digest value");
  dce::Digest256 repeated;
  DCE_REQUIRE(evidence_of(view, report, repeated, "repeat"));
  record_check(baseline == repeated,
               "two identical reconciliations produced different evidence digests");

  {
    SiteProgress changed = view;
    changed.site = other_site();
    expect_different_evidence(view, report, changed, report, "coordinator view site");
  }
  {
    SiteProgress changed = view;
    changed.cohort = *dce::CohortId::parse("cohort-other");
    expect_different_evidence(view, report, changed, report, "coordinator view cohort");
  }
  {
    SiteProgress changed = view;
    changed.wave = dce::CohortWave{7};
    expect_different_evidence(view, report, changed, report, "coordinator view wave");
  }
  {
    SiteProgress changed = view;
    changed.accepted_stage = StageOrdinal{2};
    expect_different_evidence(view, report, changed, report, "coordinator view stage");
  }
  {
    SiteProgress changed = view;
    changed.generation = SiteGeneration::from_value(4);
    expect_different_evidence(view, report, changed, report, "coordinator view generation");
  }
  {
    SiteProgress changed = view;
    changed.stage_digest = digests[2];
    expect_different_evidence(view, report, changed, report, "coordinator view stage digest");
  }
  {
    SiteProgress changed = view;
    changed.partitioned = true;
    expect_different_evidence(view, report, changed, report, "coordinator view partition flag");
  }
  {
    SiteProgress changed = view;
    changed.in_flight = true;
    expect_different_evidence(view, report, changed, report, "coordinator view in-flight flag");
  }

  {
    SiteStageReport changed = report;
    changed.site = other_site();
    expect_different_evidence(view, report, view, changed, "report site");
  }
  {
    SiteStageReport changed = report;
    changed.generation = SiteGeneration::from_value(6);
    expect_different_evidence(view, report, view, changed, "report generation");
  }
  {
    SiteStageReport changed = report;
    changed.epoch = CoordinatorEpoch{4};
    expect_different_evidence(view, report, view, changed, "report epoch");
  }
  {
    SiteStageReport changed = report;
    changed.accepted_stage = StageOrdinal{2};
    expect_different_evidence(view, report, view, changed, "report stage");
  }
  {
    SiteStageReport changed = report;
    changed.accepted_stage_digest = digests[2];
    expect_different_evidence(view, report, view, changed, "report stage digest");
  }
  {
    SiteStageReport changed = report;
    changed.receipts.push_back(*dce::ReceiptId::parse("receipt-1"));
    expect_different_evidence(view, report, view, changed, "report receipts");
  }
  {
    SiteStageReport changed = report;
    changed.partitioned = true;
    expect_different_evidence(view, report, view, changed, "report partition flag");
  }

  // The evidence is the reconciled pair. The plan generation and the epoch are
  // decision inputs, not evidence: they are not encoded into it, so a change to
  // either alone leaves the digest of the same pair unchanged.
  dce::Digest256 other_ambient;
  const dce::Result<ReconciliationOutcome> ambient =
      dce::reconcile(view, report, PlanGeneration{9}, CoordinatorEpoch{7}, true);
  DCE_REQUIRE_OK(ambient);
  other_ambient = ambient->evidence_digest;
  record_check(baseline == other_ambient,
               "the evidence digest changed with an ambient parameter that is not part of the "
               "reconciled pair");
}

// ---------------------------------------------------------------------------
// Seeded property
// ---------------------------------------------------------------------------
DCE_TEST(reconcile, seeded_reports_never_move_generations_backwards) {
  const std::uint64_t seed = 20240917u;
  const std::string seed_text = std::to_string(seed);
  dce::DeterministicRng rng(seed);

  const std::vector<dce::Digest256>& digests = stage_digests();
  const dce::Digest256 tampered = dce::Digest256::of("tampered-stage-digest");
  constexpr std::uint32_t kStages = 4;
  constexpr std::size_t kIterations = 4000;

  SiteProgress view = view_of(the_site(), 0, 1, digests[0]);
  std::uint64_t high_water = view.generation.value();
  std::array<std::size_t, 5> seen{};

  for (std::size_t iteration = 0; iteration < kIterations; ++iteration) {
    const std::uint32_t stage =
        static_cast<std::uint32_t>(rng.uniform_below(static_cast<std::uint64_t>(kStages) + 1u));
    SiteStageReport report =
        report_of(rng.chance(9u, 10u) ? the_site() : other_site(),
                  CoordinatorEpoch::from_value(rng.uniform_below(5u)), stage, rng.uniform_below(12u),
                  rng.chance(4u, 5u) ? digests[stage] : tampered);
    report.partitioned = rng.chance(1u, 2u);
    const bool delegated = rng.chance(1u, 2u);

    const std::uint64_t generation_before = view.generation.value();
    const std::uint32_t stage_before = view.accepted_stage.value();
    const dce::Result<ReconciliationOutcome> outcome =
        dce::reconcile(view, report, the_plan_generation(), the_epoch(), delegated);
    if (!outcome.ok()) {
      DCE_FAIL("seed " + seed_text + ": reconcile failed at iteration " +
               std::to_string(iteration) + ": " +
               dce::test::outcome_detail(outcome.code(), outcome.message()));
      return;
    }
    const ReconciliationOutcome& result = *outcome;
    const std::string where =
        "seed " + seed_text + " at iteration " + std::to_string(iteration);

    // No outcome ever moves a generation the coordinator issued backwards.
    record_check(result.next_generation.value() >= generation_before,
                 where + ": next generation " + std::to_string(result.next_generation.value()) +
                     " is behind the coordinator's " + std::to_string(generation_before));

    if (result.action == ReconciliationAction::advance) {
      record_check(delegated, where + ": a site without delegated authority advanced");
      record_check(report.accepted_stage.value() > stage_before,
                   where + ": an advance did not move the stage forward");
      record_check(report.epoch.value() == the_epoch().value(),
                   where + ": an advance was granted to a report from epoch " +
                       std::to_string(report.epoch.value()));
      record_check(result.agreed_stage.value() == report.accepted_stage.value(),
                   where + ": the agreed stage is not the site's accepted stage");
    }
    if (result.action == ReconciliationAction::rewind) {
      record_check(report.accepted_stage.value() < stage_before,
                   where + ": a rewind happened although the site was not behind");
      record_check(result.agreed_stage.value() == report.accepted_stage.value(),
                   where + ": the agreed stage is not the site's accepted stage");
    }
    if (result.action == ReconciliationAction::hold) {
      record_check(result.agreed_stage.value() == stage_before,
                   where + ": a hold changed the agreed stage");
    }
    if (result.action == ReconciliationAction::reject) {
      // A rejection never advances the coordinator's belief.
      record_check(result.agreed_stage.value() == stage_before,
                   where + ": a rejection advanced the coordinator's belief from stage " +
                       std::to_string(stage_before) + " to " +
                       std::to_string(result.agreed_stage.value()));
    }
    if (result.action == ReconciliationAction::agree) {
      record_check(result.agreed_stage.value() == stage_before,
                   where + ": an agreement changed the agreed stage");
    }

    // A newly issued generation is strictly ahead of every generation issued
    // before it: a generation that may already have been published is never
    // handed out a second time.
    if (result.action != ReconciliationAction::agree &&
        result.action != ReconciliationAction::reject) {
      record_check(result.next_generation.value() > high_water,
                   where + ": generation " + std::to_string(result.next_generation.value()) +
                       " was reused, with a high water mark of " + std::to_string(high_water));
      high_water = result.next_generation.value();
      view.generation = result.next_generation;
      view.accepted_stage = result.agreed_stage;
      view.stage_digest = report.accepted_stage_digest;
    }
    seen[static_cast<std::size_t>(result.action)] += 1;
  }

  for (std::size_t index = 0; index < seen.size(); ++index) {
    const std::string action =
        dce::to_string(static_cast<ReconciliationAction>(index));
    record_check(seen[index] > 0,
                 "seed " + seed_text + ": the sequence never produced a " + action + " outcome");
  }
  record_check(high_water > 1u,
               "seed " + seed_text + ": no new generation was issued in the whole sequence");
}
