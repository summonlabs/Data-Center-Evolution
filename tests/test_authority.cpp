// Delegated rollout authority: token currency, the authority verdicts, receipt
// acceptance and its idempotence, and rollback eligibility.
//
// Each verdict is asserted by name rather than by "not valid", because the
// distinctions are the contract: a superseded generation, an epoch the
// coordinator has not reached and a plan that was written against other content
// are different answers, and collapsing them would lose the reason a site was
// told to stand down.
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "dce/authority.hpp"
#include "dce/synthetic.hpp"
#include "harness.hpp"

namespace {

using dce::AuthorityCheck;
using dce::AuthorityToken;
using dce::AuthorityVerdict;
using dce::CoordinatorEpoch;
using dce::CohortWave;
using dce::EvolutionPlan;
using dce::MigrationReceipt;
using dce::PlanGeneration;
using dce::ReceiptOutcome;
using dce::ReceiptVerdict;
using dce::RollbackEligibility;
using dce::RollbackMarker;
using dce::SiteGeneration;
using dce::StageOrdinal;

// One assertion with a caller-supplied message, for checks whose context (the
// case, the field, the site) a macro cannot render.
void record_check(bool condition, const std::string& message) {
  if (condition) {
    dce::test::count_assertion();
    return;
  }
  DCE_FAIL(message);
}

// A small, identical fleet for every case: four sites in two waves, two
// components with three versions each, so the plan has four migration steps.
bool make_fleet(bool delegate_authority, dce::SyntheticFleet& out, const std::string& label) {
  dce::SyntheticFleetOptions options;
  options.sites = 4;
  options.components = 2;
  options.cohorts = 2;
  options.versions_per_component = 3;
  options.seed = 3;
  options.delegate_authority = delegate_authority;
  const dce::Result<dce::SyntheticFleet> fleet = dce::make_synthetic_fleet(options);
  if (!fleet.ok()) {
    DCE_FAIL(label + ": make_synthetic_fleet failed: " +
             dce::test::outcome_detail(fleet.code(), fleet.message()));
    return false;
  }
  out = fleet.value();
  return true;
}

AuthorityToken token_for(const EvolutionPlan& plan, std::size_t site_index) {
  const dce::SiteRecord& record = plan.membership.sites[site_index];
  AuthorityToken token;
  token.plan = plan.identity.id;
  token.plan_generation = plan.identity.generation;
  token.epoch = plan.identity.epoch;
  token.site = record.id;
  token.cohort = plan.cohorts.empty() ? dce::CohortId{} : plan.cohorts.front().id;
  token.stage = StageOrdinal{};
  token.plan_digest = plan.digest;
  return token;
}

void expect_verdict(const AuthorityCheck& check, AuthorityVerdict expected,
                    const std::string& label) {
  record_check(check.verdict == expected,
               label + ": expected verdict " + dce::to_string(expected) + ", got " +
                   dce::to_string(check.verdict) + " (" + check.explanation + ")");
  record_check(!check.explanation.empty(),
               label + ": verdict " + dce::to_string(check.verdict) + " carries no explanation");
  record_check(check.valid() == (expected == AuthorityVerdict::valid),
               label + ": valid() disagrees with the verdict");
}

void expect_receipt_verdict(const dce::Result<ReceiptOutcome>& outcome, ReceiptVerdict expected,
                            const std::string& label) {
  if (!outcome.ok()) {
    DCE_FAIL(label + ": expected verdict " + dce::to_string(expected) +
             " but acceptance failed: " +
             dce::test::outcome_detail(outcome.code(), outcome.message()));
    return;
  }
  record_check(outcome->verdict == expected,
               label + ": expected verdict " + dce::to_string(expected) + ", got " +
                   dce::to_string(outcome->verdict) + " (" + outcome->explanation + ")");
  record_check(!outcome->explanation.empty(),
               label + ": verdict " + dce::to_string(outcome->verdict) + " carries no explanation");
}

// The receipt a site would send after accepting the step at step_index, which
// is stage step_index + 1 because ordinals count accepted steps.
MigrationReceipt receipt_for(const EvolutionPlan& plan, std::size_t site_index,
                             std::size_t step_index) {
  const dce::MigrationStep& step = plan.steps[step_index];
  MigrationReceipt receipt;
  receipt.id = *dce::ReceiptId::parse("receipt-0");
  receipt.plan = plan.identity.id;
  receipt.plan_generation = plan.identity.generation;
  receipt.epoch = plan.identity.epoch;
  receipt.site = plan.membership.sites[site_index].id;
  receipt.step = step.id;
  receipt.stage = StageOrdinal{static_cast<std::uint32_t>(step_index + 1u)};
  receipt.from = step.from;
  receipt.to = step.to;
  receipt.accepted_generation = SiteGeneration::from_value(1);
  receipt.evidence_digest = dce::Digest256::of("evidence-0");
  receipt.idempotency_key = "idem-0";
  return receipt;
}

void expect_eligibility(const RollbackEligibility& eligibility, bool expected,
                        std::string_view subject, const std::string& label) {
  record_check(eligibility.eligible == expected,
               label + ": expected eligible=" + dce::test::to_debug_string(expected) + ", got " +
                   dce::test::to_debug_string(eligibility.eligible) + " (" +
                   eligibility.explanation + ")");
  record_check(!eligibility.explanation.empty(),
               label + ": the eligibility carries no explanation");
  record_check(eligibility.explanation.find(subject) != std::string::npos,
               label + ": the explanation does not mention " + std::string(subject) + ": " +
                   eligibility.explanation);
}

// Marks the plan under test: rollback permitted exactly two waves back, the
// second migration step irreversible, and the plan's own point of no return far
// ahead of the marker each case builds.
bool mark_rollback_policy(EvolutionPlan& plan, const std::string& label) {
  if (plan.steps.size() < 2 || !plan.point_of_no_return.has_value()) {
    DCE_FAIL(label + ": the synthetic plan is not the shape this case needs");
    return false;
  }
  plan.rollback.permitted = true;
  plan.rollback.max_waves_back = CohortWave{2};
  plan.rollback.irreversible_steps = {plan.steps[1].id};
  plan.point_of_no_return->wave = CohortWave{9};
  return true;
}

RollbackMarker marker_at(const std::uint32_t wave, const std::uint32_t stage) {
  RollbackMarker marker;
  marker.wave = CohortWave{wave};
  marker.stage = StageOrdinal{stage};
  marker.crossed_point_of_no_return = false;
  marker.justification = "operator decision recorded before the boundary";
  return marker;
}

}  // namespace

// ---------------------------------------------------------------------------
// Token digests
// ---------------------------------------------------------------------------
DCE_TEST(authority, the_token_digest_is_stable_and_covers_every_field) {
  dce::SyntheticFleet fleet;
  DCE_REQUIRE(make_fleet(true, fleet, "token digest"));
  const EvolutionPlan& plan = fleet.plan;
  DCE_REQUIRE(plan.membership.sites.size() >= static_cast<std::size_t>(2));

  const AuthorityToken base = token_for(plan, 0);
  const dce::Digest256 first = dce::compute_token_digest(base);
  record_check(!first.is_zero(), "the token digest is the documented no-digest value");
  record_check(dce::compute_token_digest(base) == first,
               "two digests of the same token differ");
  const AuthorityToken equal_copy = base;
  record_check(dce::compute_token_digest(equal_copy) == first,
               "two equal tokens have different digests");

  // Every field participates: a token that differs in any one of them is a
  // different authority and must not hash the same.
  {
    AuthorityToken changed = base;
    changed.plan = *dce::EvolutionPlanId::parse("plan.other");
    record_check(!(dce::compute_token_digest(changed) == first), "changing the plan did not change the token digest");
  }
  {
    AuthorityToken changed = base;
    changed.plan_generation = PlanGeneration::from_value(base.plan_generation.value() + 1u);
    record_check(!(dce::compute_token_digest(changed) == first),
                 "changing the plan generation did not change the token digest");
  }
  {
    AuthorityToken changed = base;
    changed.epoch = CoordinatorEpoch::from_value(base.epoch.value() + 1u);
    record_check(!(dce::compute_token_digest(changed) == first),
                 "changing the epoch did not change the token digest");
  }
  {
    AuthorityToken changed = base;
    changed.site = *dce::SiteId::parse("site-other");
    record_check(!(dce::compute_token_digest(changed) == first), "changing the site did not change the token digest");
  }
  {
    AuthorityToken changed = base;
    changed.cohort = *dce::CohortId::parse("cohort-other");
    record_check(!(dce::compute_token_digest(changed) == first), "changing the cohort did not change the token digest");
  }
  {
    AuthorityToken changed = base;
    changed.stage = StageOrdinal{7};
    record_check(!(dce::compute_token_digest(changed) == first), "changing the stage did not change the token digest");
  }
  {
    AuthorityToken changed = base;
    changed.plan_digest = dce::Digest256::of("other plan content");
    record_check(!(dce::compute_token_digest(changed) == first),
                 "changing the plan digest did not change the token digest");
  }
}

// ---------------------------------------------------------------------------
// check_authority, one case per verdict
// ---------------------------------------------------------------------------
DCE_TEST(authority, a_current_token_from_a_delegated_site_is_valid) {
  dce::SyntheticFleet fleet;
  DCE_REQUIRE(make_fleet(true, fleet, "valid token"));
  const EvolutionPlan& plan = fleet.plan;
  DCE_REQUIRE(plan.membership.sites.size() >= static_cast<std::size_t>(1));

  const AuthorityToken token = token_for(plan, 0);
  const AuthorityCheck check =
      dce::check_authority(token, plan, plan.identity.epoch, plan.identity.generation);
  expect_verdict(check, AuthorityVerdict::valid, "current token");
  DCE_CHECK_TRUE(check.valid());
  // A valid verdict is auditable: it names the plan and the site it authorises.
  record_check(check.explanation.find(plan.identity.id.str()) != std::string::npos,
               "the valid verdict does not name the plan");
  record_check(check.explanation.find(token.site.str()) != std::string::npos,
               "the valid verdict does not name the site");
}

DCE_TEST(authority, a_token_for_another_plan_is_a_plan_mismatch) {
  dce::SyntheticFleet fleet;
  DCE_REQUIRE(make_fleet(true, fleet, "plan mismatch"));
  const EvolutionPlan& plan = fleet.plan;
  DCE_REQUIRE(plan.membership.sites.size() >= static_cast<std::size_t>(1));

  AuthorityToken token = token_for(plan, 0);
  token.plan = *dce::EvolutionPlanId::parse("plan.other");
  expect_verdict(dce::check_authority(token, plan, plan.identity.epoch, plan.identity.generation),
                 AuthorityVerdict::plan_mismatch, "another plan");

  // The identity is checked before the content, so a token that is wrong about
  // both is reported as naming another plan rather than as a digest mismatch.
  token.plan_digest = dce::Digest256::of("other content");
  expect_verdict(dce::check_authority(token, plan, plan.identity.epoch, plan.identity.generation),
                 AuthorityVerdict::plan_mismatch, "another plan and other content");
}

DCE_TEST(authority, a_token_for_other_plan_content_is_a_digest_mismatch) {
  dce::SyntheticFleet fleet;
  DCE_REQUIRE(make_fleet(true, fleet, "digest mismatch"));
  const EvolutionPlan& plan = fleet.plan;
  DCE_REQUIRE(plan.membership.sites.size() >= static_cast<std::size_t>(1));

  AuthorityToken token = token_for(plan, 0);
  token.plan_digest = dce::Digest256::of("other plan content");
  const AuthorityCheck check =
      dce::check_authority(token, plan, plan.identity.epoch, plan.identity.generation);
  expect_verdict(check, AuthorityVerdict::digest_mismatch, "other content");
  record_check(check.explanation.find(plan.identity.id.str()) != std::string::npos,
               "the digest mismatch does not name the plan it was checked against");
}

DCE_TEST(authority, an_epoch_that_is_not_current_is_stale_in_both_directions) {
  dce::SyntheticFleet fleet;
  DCE_REQUIRE(make_fleet(true, fleet, "stale epoch"));
  const EvolutionPlan& plan = fleet.plan;
  DCE_REQUIRE(plan.membership.sites.size() >= static_cast<std::size_t>(1));

  const AuthorityToken token = token_for(plan, 0);
  const CoordinatorEpoch current{4};
  const PlanGeneration generation = plan.identity.generation;

  AuthorityToken older = token;
  older.epoch = CoordinatorEpoch{3};
  const AuthorityCheck behind = dce::check_authority(older, plan, current, generation);
  expect_verdict(behind, AuthorityVerdict::stale_epoch, "older epoch");
  record_check(behind.explanation.find("3") != std::string::npos &&
                   behind.explanation.find("4") != std::string::npos,
               "the stale-epoch verdict does not name both epochs: " + behind.explanation);

  // An epoch the coordinator has not reached yet is rejected exactly like an
  // older one: only the issuing epoch can be current.
  AuthorityToken ahead = token;
  ahead.epoch = CoordinatorEpoch{5};
  expect_verdict(dce::check_authority(ahead, plan, current, generation),
                 AuthorityVerdict::stale_epoch, "epoch not yet reached");

  AuthorityToken current_epoch = token;
  current_epoch.epoch = current;
  expect_verdict(dce::check_authority(current_epoch, plan, current, generation),
                 AuthorityVerdict::valid, "the current epoch");
}

DCE_TEST(authority, a_superseded_plan_generation_is_fenced) {
  dce::SyntheticFleet fleet;
  DCE_REQUIRE(make_fleet(true, fleet, "fenced generation"));
  const EvolutionPlan& plan = fleet.plan;
  DCE_REQUIRE(plan.membership.sites.size() >= static_cast<std::size_t>(1));

  AuthorityToken token = token_for(plan, 0);
  const CoordinatorEpoch epoch = plan.identity.epoch;
  token.plan_generation = PlanGeneration::from_value(1);
  const AuthorityCheck fenced = dce::check_authority(token, plan, epoch, PlanGeneration::from_value(2));
  expect_verdict(fenced, AuthorityVerdict::fenced_generation, "superseded generation");
  record_check(fenced.explanation.find("1") != std::string::npos &&
                   fenced.explanation.find("2") != std::string::npos,
               "the fenced verdict does not name both generations: " + fenced.explanation);

  token.plan_generation = PlanGeneration::from_value(2);
  expect_verdict(dce::check_authority(token, plan, epoch, PlanGeneration::from_value(2)),
                 AuthorityVerdict::valid, "the current generation");
}

DCE_TEST(authority, a_token_for_a_site_outside_the_plan_is_unknown) {
  dce::SyntheticFleet fleet;
  DCE_REQUIRE(make_fleet(true, fleet, "unknown site"));
  const EvolutionPlan& plan = fleet.plan;
  DCE_REQUIRE(plan.membership.sites.size() >= static_cast<std::size_t>(1));

  AuthorityToken token = token_for(plan, 0);
  token.site = *dce::SiteId::parse("site-999");
  const AuthorityCheck check =
      dce::check_authority(token, plan, plan.identity.epoch, plan.identity.generation);
  expect_verdict(check, AuthorityVerdict::unknown_site, "site outside the plan");
  record_check(check.explanation.find("site-999") != std::string::npos,
               "the unknown-site verdict does not name the site: " + check.explanation);
}

DCE_TEST(authority, a_token_for_a_sovereign_site_is_not_delegated) {
  dce::SyntheticFleet fleet;
  DCE_REQUIRE(make_fleet(false, fleet, "not delegated"));
  const EvolutionPlan& plan = fleet.plan;
  DCE_REQUIRE(plan.membership.sites.size() >= static_cast<std::size_t>(1));
  DCE_CHECK_TRUE(!plan.membership.sites.front().delegated_rollout_authority);

  const AuthorityToken token = token_for(plan, 0);
  const AuthorityCheck check =
      dce::check_authority(token, plan, plan.identity.epoch, plan.identity.generation);
  expect_verdict(check, AuthorityVerdict::not_delegated, "sovereign site");
  record_check(check.explanation.find(token.site.str()) != std::string::npos,
               "the not-delegated verdict does not name the site: " + check.explanation);
}

// ---------------------------------------------------------------------------
// accept_receipt
// ---------------------------------------------------------------------------
DCE_TEST(authority, a_receipt_for_the_current_epoch_is_accepted) {
  dce::SyntheticFleet fleet;
  DCE_REQUIRE(make_fleet(true, fleet, "accept receipt"));
  const EvolutionPlan& plan = fleet.plan;
  DCE_REQUIRE(plan.membership.sites.size() >= static_cast<std::size_t>(1));
  DCE_REQUIRE(plan.steps.size() >= static_cast<std::size_t>(2));

  const MigrationReceipt receipt = receipt_for(plan, 0, 0);
  const std::vector<MigrationReceipt> existing;
  const dce::Result<ReceiptOutcome> outcome =
      dce::accept_receipt(receipt, plan, existing, plan.identity.epoch, plan.identity.generation);
  expect_receipt_verdict(outcome, ReceiptVerdict::accepted, "current receipt");
  record_check(outcome.ok() && outcome->explanation.find(receipt.site.str()) != std::string::npos,
               "the acceptance does not name the site it accepted for");
}

DCE_TEST(authority, a_receipt_replayed_under_the_same_key_is_a_duplicate) {
  dce::SyntheticFleet fleet;
  DCE_REQUIRE(make_fleet(true, fleet, "duplicate receipt"));
  const EvolutionPlan& plan = fleet.plan;
  DCE_REQUIRE(plan.membership.sites.size() >= static_cast<std::size_t>(1));
  DCE_REQUIRE(!plan.steps.empty());

  const MigrationReceipt receipt = receipt_for(plan, 0, 0);
  const std::vector<MigrationReceipt> empty;
  const dce::Result<ReceiptOutcome> first =
      dce::accept_receipt(receipt, plan, empty, plan.identity.epoch, plan.identity.generation);
  expect_receipt_verdict(first, ReceiptVerdict::accepted, "first acceptance");

  // The same effect under the same key, arriving with a different receipt
  // identity: the key is what makes it a replay, and it changes nothing.
  MigrationReceipt replay = receipt;
  replay.id = *dce::ReceiptId::parse("receipt-1");
  const std::vector<MigrationReceipt> accepted{receipt};
  const dce::Result<ReceiptOutcome> second =
      dce::accept_receipt(replay, plan, accepted, plan.identity.epoch, plan.identity.generation);
  expect_receipt_verdict(second, ReceiptVerdict::duplicate, "replay under the same key");
  if (second.ok()) {
    record_check(second->verdict != ReceiptVerdict::accepted,
                 "the replay was reported as a fresh acceptance");
    record_check(second->explanation.find("replay") != std::string::npos,
                 "the duplicate does not say it is a replay: " + second->explanation);
    record_check(second->explanation.find("accepted:") == std::string::npos,
                 "the duplicate reads like a fresh acceptance: " + second->explanation);
    record_check(first.ok() && second->explanation != first->explanation,
                 "the replay is reported exactly like the acceptance it repeats");
  }
  // A third delivery of the identical receipt is still only a duplicate.
  const dce::Result<ReceiptOutcome> third =
      dce::accept_receipt(receipt, plan, accepted, plan.identity.epoch, plan.identity.generation);
  expect_receipt_verdict(third, ReceiptVerdict::duplicate, "identical receipt again");

  // A replay does not consume the plan: a genuinely new acceptance under its
  // own key is still accepted afterwards.
  MigrationReceipt fresh = receipt;
  fresh.id = *dce::ReceiptId::parse("receipt-2");
  fresh.idempotency_key = "idem-2";
  expect_receipt_verdict(
      dce::accept_receipt(fresh, plan, accepted, plan.identity.epoch, plan.identity.generation),
      ReceiptVerdict::accepted, "a new acceptance after two replays");
}

DCE_TEST(authority, the_same_key_with_different_content_conflicts) {
  dce::SyntheticFleet fleet;
  DCE_REQUIRE(make_fleet(true, fleet, "key conflict"));
  const EvolutionPlan& plan = fleet.plan;
  DCE_REQUIRE(plan.membership.sites.size() >= static_cast<std::size_t>(1));
  DCE_REQUIRE(!plan.steps.empty());

  const MigrationReceipt receipt = receipt_for(plan, 0, 0);
  MigrationReceipt different = receipt;
  different.id = *dce::ReceiptId::parse("receipt-1");
  different.evidence_digest = dce::Digest256::of("other evidence");

  const std::vector<MigrationReceipt> existing{receipt};
  const dce::Result<ReceiptOutcome> outcome =
      dce::accept_receipt(different, plan, existing, plan.identity.epoch, plan.identity.generation);
  expect_receipt_verdict(outcome, ReceiptVerdict::conflicting, "one key, two acceptances");
  if (outcome.ok()) {
    record_check(outcome->explanation.find(receipt.idempotency_key) != std::string::npos,
                 "the conflict does not name the key it conflicts on: " + outcome->explanation);
  }
}

DCE_TEST(authority, the_same_receipt_identity_with_different_content_conflicts) {
  dce::SyntheticFleet fleet;
  DCE_REQUIRE(make_fleet(true, fleet, "identity conflict"));
  const EvolutionPlan& plan = fleet.plan;
  DCE_REQUIRE(plan.membership.sites.size() >= static_cast<std::size_t>(1));
  DCE_REQUIRE(!plan.steps.empty());

  const MigrationReceipt receipt = receipt_for(plan, 0, 0);
  MigrationReceipt different = receipt;
  different.idempotency_key = "idem-other";
  different.evidence_digest = dce::Digest256::of("other evidence");

  const std::vector<MigrationReceipt> existing{receipt};
  const dce::Result<ReceiptOutcome> outcome =
      dce::accept_receipt(different, plan, existing, plan.identity.epoch, plan.identity.generation);
  expect_receipt_verdict(outcome, ReceiptVerdict::conflicting, "one receipt identity, two acceptances");
  if (outcome.ok()) {
    record_check(outcome->explanation.find(receipt.id.str()) != std::string::npos,
                 "the conflict does not name the receipt identity: " + outcome->explanation);
  }
}

DCE_TEST(authority, a_receipt_from_a_superseded_generation_is_fenced) {
  dce::SyntheticFleet fleet;
  DCE_REQUIRE(make_fleet(true, fleet, "fenced receipt"));
  const EvolutionPlan& plan = fleet.plan;
  DCE_REQUIRE(plan.membership.sites.size() >= static_cast<std::size_t>(1));
  DCE_REQUIRE(!plan.steps.empty());

  MigrationReceipt receipt = receipt_for(plan, 0, 0);
  receipt.plan_generation = PlanGeneration::from_value(1);
  const std::vector<MigrationReceipt> existing;
  const dce::Result<ReceiptOutcome> outcome =
      dce::accept_receipt(receipt, plan, existing, plan.identity.epoch, PlanGeneration::from_value(2));
  expect_receipt_verdict(outcome, ReceiptVerdict::fenced_generation, "superseded generation");
}

DCE_TEST(authority, a_receipt_from_another_epoch_is_stale) {
  dce::SyntheticFleet fleet;
  DCE_REQUIRE(make_fleet(true, fleet, "stale receipt"));
  const EvolutionPlan& plan = fleet.plan;
  DCE_REQUIRE(plan.membership.sites.size() >= static_cast<std::size_t>(1));
  DCE_REQUIRE(!plan.steps.empty());

  const std::vector<MigrationReceipt> existing;
  const PlanGeneration generation = plan.identity.generation;
  const CoordinatorEpoch current{4};

  MigrationReceipt older = receipt_for(plan, 0, 0);
  older.epoch = CoordinatorEpoch{3};
  expect_receipt_verdict(dce::accept_receipt(older, plan, existing, current, generation),
                         ReceiptVerdict::stale_epoch, "older epoch");

  MigrationReceipt ahead = receipt_for(plan, 0, 0);
  ahead.epoch = CoordinatorEpoch{5};
  expect_receipt_verdict(dce::accept_receipt(ahead, plan, existing, current, generation),
                         ReceiptVerdict::stale_epoch, "epoch not yet reached");

  MigrationReceipt current_epoch = receipt_for(plan, 0, 0);
  current_epoch.epoch = current;
  expect_receipt_verdict(dce::accept_receipt(current_epoch, plan, existing, current, generation),
                         ReceiptVerdict::accepted, "the current epoch");
}

DCE_TEST(authority, a_receipt_for_an_unknown_site_is_refused) {
  dce::SyntheticFleet fleet;
  DCE_REQUIRE(make_fleet(true, fleet, "wrong site"));
  const EvolutionPlan& plan = fleet.plan;
  DCE_REQUIRE(plan.membership.sites.size() >= static_cast<std::size_t>(1));
  DCE_REQUIRE(!plan.steps.empty());

  MigrationReceipt receipt = receipt_for(plan, 0, 0);
  receipt.site = *dce::SiteId::parse("site-999");
  const std::vector<MigrationReceipt> existing;
  const dce::Result<ReceiptOutcome> outcome =
      dce::accept_receipt(receipt, plan, existing, plan.identity.epoch, plan.identity.generation);
  expect_receipt_verdict(outcome, ReceiptVerdict::wrong_site, "site outside the plan");
  if (outcome.ok()) {
    record_check(outcome->explanation.find("site-999") != std::string::npos,
                 "the refusal does not name the site: " + outcome->explanation);
  }
}

DCE_TEST(authority, a_receipt_for_an_unknown_step_is_refused) {
  dce::SyntheticFleet fleet;
  DCE_REQUIRE(make_fleet(true, fleet, "unknown step"));
  const EvolutionPlan& plan = fleet.plan;
  DCE_REQUIRE(plan.membership.sites.size() >= static_cast<std::size_t>(1));
  DCE_REQUIRE(!plan.steps.empty());

  MigrationReceipt receipt = receipt_for(plan, 0, 0);
  receipt.step = *dce::StepId::parse("step.does.not.exist");
  const std::vector<MigrationReceipt> existing;
  const dce::Result<ReceiptOutcome> outcome =
      dce::accept_receipt(receipt, plan, existing, plan.identity.epoch, plan.identity.generation);
  expect_receipt_verdict(outcome, ReceiptVerdict::unknown_step, "step outside the plan");
  if (outcome.ok()) {
    record_check(outcome->explanation.find("step.does.not.exist") != std::string::npos,
                 "the refusal does not name the step: " + outcome->explanation);
  }
}

DCE_TEST(authority, a_receipt_whose_stage_does_not_match_its_step_is_refused) {
  dce::SyntheticFleet fleet;
  DCE_REQUIRE(make_fleet(true, fleet, "wrong stage"));
  const EvolutionPlan& plan = fleet.plan;
  DCE_REQUIRE(plan.membership.sites.size() >= static_cast<std::size_t>(1));
  DCE_REQUIRE(plan.steps.size() >= static_cast<std::size_t>(2));

  const std::vector<MigrationReceipt> existing;

  // The second step is stage 2; a receipt that claims stage 1 for it skipped a
  // stage, whatever the site believes it did.
  MigrationReceipt skipped = receipt_for(plan, 0, 1);
  skipped.stage = StageOrdinal{1};
  expect_receipt_verdict(
      dce::accept_receipt(skipped, plan, existing, plan.identity.epoch, plan.identity.generation),
      ReceiptVerdict::wrong_stage, "a stage that does not correspond to the step");

  // The declared move must be the plan's move, not merely the right ordinal.
  MigrationReceipt moved = receipt_for(plan, 0, 0);
  moved.from = plan.steps[1].from;
  moved.to = plan.steps[1].to;
  const dce::Result<ReceiptOutcome> outcome =
      dce::accept_receipt(moved, plan, existing, plan.identity.epoch, plan.identity.generation);
  expect_receipt_verdict(outcome, ReceiptVerdict::wrong_stage, "from/to that do not match the step");
  if (outcome.ok()) {
    record_check(outcome->explanation.find(plan.steps[0].from.to_string()) != std::string::npos,
                 "the refusal does not name the plan's own move: " + outcome->explanation);
  }
}

DCE_TEST(authority, a_receipt_without_an_usable_key_is_invalid) {
  dce::SyntheticFleet fleet;
  DCE_REQUIRE(make_fleet(true, fleet, "invalid receipt"));
  const EvolutionPlan& plan = fleet.plan;
  DCE_REQUIRE(plan.membership.sites.size() >= static_cast<std::size_t>(1));
  DCE_REQUIRE(!plan.steps.empty());

  const std::vector<MigrationReceipt> existing;

  MigrationReceipt keyless = receipt_for(plan, 0, 0);
  keyless.idempotency_key.clear();
  DCE_CHECK_CODE(
      dce::accept_receipt(keyless, plan, existing, plan.identity.epoch, plan.identity.generation),
      dce::ErrorCode::invalid_argument);

  // A key that is not a usable identity is refused for the same reason: an
  // acceptance that cannot be replayed cannot be audited.
  MigrationReceipt malformed = receipt_for(plan, 0, 0);
  malformed.idempotency_key = "not a key";
  DCE_CHECK_CODE(
      dce::accept_receipt(malformed, plan, existing, plan.identity.epoch, plan.identity.generation),
      dce::ErrorCode::invalid_argument);

  MigrationReceipt anonymous = receipt_for(plan, 0, 0);
  anonymous.id = dce::ReceiptId{};
  DCE_CHECK_CODE(
      dce::accept_receipt(anonymous, plan, existing, plan.identity.epoch, plan.identity.generation),
      dce::ErrorCode::invalid_argument);
}

// ---------------------------------------------------------------------------
// Rollback eligibility
// ---------------------------------------------------------------------------
DCE_TEST(authority, rollback_is_eligible_before_the_point_of_no_return) {
  dce::SyntheticFleet fleet;
  DCE_REQUIRE(make_fleet(true, fleet, "eligible rollback"));
  EvolutionPlan plan = fleet.plan;
  DCE_REQUIRE(mark_rollback_policy(plan, "eligible rollback"));

  const RollbackMarker marker = marker_at(4, 0);
  expect_eligibility(dce::assess_rollback_eligibility(plan, marker, CohortWave{3}), true, "eligible",
                     "one wave back");
  expect_eligibility(dce::assess_rollback_eligibility(plan, marker, CohortWave{4}), true, "eligible",
                     "no wave back");
  expect_eligibility(dce::assess_rollback_eligibility(plan, marker, CohortWave{2}), true, "eligible",
                     "exactly the policy distance");
}

DCE_TEST(authority, rollback_is_refused_once_the_marker_crossed_the_point_of_no_return) {
  dce::SyntheticFleet fleet;
  DCE_REQUIRE(make_fleet(true, fleet, "crossed boundary"));
  EvolutionPlan plan = fleet.plan;
  DCE_REQUIRE(mark_rollback_policy(plan, "crossed boundary"));

  RollbackMarker marker = marker_at(1, 0);
  marker.crossed_point_of_no_return = true;
  expect_eligibility(dce::assess_rollback_eligibility(plan, marker, CohortWave{0}), false,
                     "point of no return", "crossed marker");

  // The plan's own boundary refuses a marker that stands beyond it even when
  // the marker itself does not record a crossing.
  const RollbackMarker beyond = marker_at(10, 0);
  expect_eligibility(dce::assess_rollback_eligibility(plan, beyond, CohortWave{9}), false,
                     "point of no return", "marker beyond the plan boundary");
}

DCE_TEST(authority, rollback_is_refused_when_the_policy_forbids_it) {
  dce::SyntheticFleet fleet;
  DCE_REQUIRE(make_fleet(true, fleet, "forbidden rollback"));
  EvolutionPlan plan = fleet.plan;
  DCE_REQUIRE(mark_rollback_policy(plan, "forbidden rollback"));
  plan.rollback.permitted = false;

  const RollbackMarker marker = marker_at(4, 0);
  expect_eligibility(dce::assess_rollback_eligibility(plan, marker, CohortWave{3}), false,
                     "rollback policy", "policy forbids rollback");
}

DCE_TEST(authority, rollback_is_refused_for_a_wave_ahead_of_the_marker) {
  dce::SyntheticFleet fleet;
  DCE_REQUIRE(make_fleet(true, fleet, "forward rollback"));
  EvolutionPlan plan = fleet.plan;
  DCE_REQUIRE(mark_rollback_policy(plan, "forward rollback"));

  const RollbackMarker marker = marker_at(4, 0);
  expect_eligibility(dce::assess_rollback_eligibility(plan, marker, CohortWave{5}), false, "ahead",
                     "target ahead of the marker");
}

DCE_TEST(authority, rollback_is_refused_beyond_the_policy_distance) {
  dce::SyntheticFleet fleet;
  DCE_REQUIRE(make_fleet(true, fleet, "distance"));
  EvolutionPlan plan = fleet.plan;
  DCE_REQUIRE(mark_rollback_policy(plan, "distance"));
  DCE_CHECK_EQ(plan.rollback.max_waves_back.value(), 2u);

  const RollbackMarker marker = marker_at(4, 0);
  expect_eligibility(dce::assess_rollback_eligibility(plan, marker, CohortWave{1}), false,
                     "policy limit", "three waves back with a limit of two");
  // The boundary itself is inside the policy: exactly max_waves_back is still
  // a rollback the plan allows.
  expect_eligibility(dce::assess_rollback_eligibility(plan, marker, CohortWave{2}), true,
                     "eligible", "exactly the limit");
}

DCE_TEST(authority, rollback_is_refused_once_an_irreversible_step_is_applied) {
  dce::SyntheticFleet fleet;
  DCE_REQUIRE(make_fleet(true, fleet, "irreversible step"));
  EvolutionPlan plan = fleet.plan;
  DCE_REQUIRE(mark_rollback_policy(plan, "irreversible step"));

  // The irreversible step is the plan's second step, which is stage 2. A marker
  // standing beyond that stage records that the step has been applied.
  const RollbackMarker applied = marker_at(4, 3);
  expect_eligibility(dce::assess_rollback_eligibility(plan, applied, CohortWave{3}), false,
                     plan.steps[1].id.str(), "irreversible step applied");

  // A marker that has not yet passed the step's stage leaves rollback available.
  const RollbackMarker before = marker_at(4, 2);
  expect_eligibility(dce::assess_rollback_eligibility(plan, before, CohortWave{3}), true, "eligible",
                     "irreversible step not yet passed");
}
