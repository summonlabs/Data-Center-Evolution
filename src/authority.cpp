#include "dce/authority.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "dce/canonical.hpp"
#include "dce/checked.hpp"
#include "dce/codec.hpp"
#include "dce/text.hpp"

namespace dce {

const char* to_string(AuthorityVerdict verdict) noexcept {
  switch (verdict) {
    case AuthorityVerdict::valid:
      return "valid";
    case AuthorityVerdict::stale_epoch:
      return "stale_epoch";
    case AuthorityVerdict::fenced_generation:
      return "fenced_generation";
    case AuthorityVerdict::plan_mismatch:
      return "plan_mismatch";
    case AuthorityVerdict::digest_mismatch:
      return "digest_mismatch";
    case AuthorityVerdict::not_delegated:
      return "not_delegated";
    case AuthorityVerdict::unknown_site:
      return "unknown_site";
  }
  return "unknown";
}

const char* to_string(ReceiptVerdict verdict) noexcept {
  switch (verdict) {
    case ReceiptVerdict::accepted:
      return "accepted";
    case ReceiptVerdict::duplicate:
      return "duplicate";
    case ReceiptVerdict::conflicting:
      return "conflicting";
    case ReceiptVerdict::stale_epoch:
      return "stale_epoch";
    case ReceiptVerdict::fenced_generation:
      return "fenced_generation";
    case ReceiptVerdict::unknown_step:
      return "unknown_step";
    case ReceiptVerdict::wrong_site:
      return "wrong_site";
    case ReceiptVerdict::wrong_stage:
      return "wrong_stage";
  }
  return "unknown";
}

Digest256 compute_token_digest(const AuthorityToken& token) {
  CanonicalWriter writer;
  const Status encoded = codec::put(writer, token);
  if (!encoded.ok()) {
    // A token's identifiers are validated and bounded before it can exist, so
    // the canonical writer cannot refuse one. If it ever did, the zero digest
    // is returned as the documented "no digest" value rather than the digest
    // of a half-written encoding; is_zero() makes that visible to the caller.
    return Digest256::zero();
  }
  return writer.digest();
}

namespace {

template <class Tag>
std::string name_of(const BasicId<Tag>& id) {
  return text::escape_for_output(id.view());
}

std::string number(std::uint32_t value) { return text::u32_to_string(value); }
std::string number(std::uint64_t value) { return text::u64_to_string(value); }

const SiteRecord* find_site_record(const EvolutionPlan& plan, const SiteId& id) {
  for (const SiteRecord& record : plan.membership.sites) {
    if (record.id == id) {
      return &record;
    }
  }
  return nullptr;
}

// Index of a step in canonical step order. A step's stage ordinal is one past
// that index, because ordinals count accepted steps rather than slots.
std::optional<std::size_t> step_index(const EvolutionPlan& plan, const StepId& id) {
  for (std::size_t index = 0; index < plan.steps.size(); ++index) {
    if (plan.steps[index].id == id) {
      return index;
    }
  }
  return std::nullopt;
}

// The fields that make two receipts the same accepted effect. The receipt
// identity and the idempotency key are deliberately not part of it: they are
// what the duplicate/conflict decision is made on.
bool same_receipt_content(const MigrationReceipt& lhs, const MigrationReceipt& rhs) {
  return lhs.site == rhs.site && lhs.step == rhs.step && lhs.stage == rhs.stage &&
         lhs.from == rhs.from && lhs.to == rhs.to &&
         lhs.accepted_generation == rhs.accepted_generation &&
         lhs.evidence_digest == rhs.evidence_digest;
}

}  // namespace

AuthorityCheck check_authority(const AuthorityToken& token, const EvolutionPlan& plan,
                               const CoordinatorEpoch& current_epoch,
                               const PlanGeneration& current_generation) {
  AuthorityCheck check;

  if (token.plan != plan.identity.id) {
    check.verdict = AuthorityVerdict::plan_mismatch;
    check.explanation = "token names plan " + name_of(token.plan) +
                        " but this coordinator owns plan " + name_of(plan.identity.id);
    return check;
  }
  if (token.plan_digest != plan.digest) {
    check.verdict = AuthorityVerdict::digest_mismatch;
    check.explanation = "token carries a plan digest that does not match plan " +
                        name_of(plan.identity.id) +
                        ", so it was issued against other plan content";
    return check;
  }
  if (token.epoch != current_epoch) {
    check.verdict = AuthorityVerdict::stale_epoch;
    check.explanation =
        "token was issued for epoch " + number(token.epoch.value()) +
        " but the coordinator is at epoch " + number(current_epoch.value()) +
        "; a token is only current for the epoch that issued it, so an epoch the coordinator has "
        "not reached is rejected exactly like an older one";
    return check;
  }
  if (token.plan_generation < current_generation) {
    check.verdict = AuthorityVerdict::fenced_generation;
    check.explanation =
        "token carries plan generation " + number(token.plan_generation.value()) +
        " but the plan is at generation " + number(current_generation.value()) +
        "; a superseded generation may no longer act";
    return check;
  }
  const SiteRecord* record = find_site_record(plan, token.site);
  if (record == nullptr) {
    check.verdict = AuthorityVerdict::unknown_site;
    check.explanation = "token names site " + name_of(token.site) +
                        ", which is not a member of plan " + name_of(plan.identity.id);
    return check;
  }
  if (!record->delegated_rollout_authority) {
    check.verdict = AuthorityVerdict::not_delegated;
    check.explanation = "site " + name_of(token.site) +
                        " is sovereign by default: the plan delegates no rollout authority to it";
    return check;
  }

  check.verdict = AuthorityVerdict::valid;
  check.explanation = "token is current for plan " + name_of(plan.identity.id) + " at epoch " +
                      number(token.epoch.value()) + " and generation " +
                      number(token.plan_generation.value()) + ", and site " + name_of(token.site) +
                      " holds delegated rollout authority";
  return check;
}

Result<ReceiptOutcome> accept_receipt(const MigrationReceipt& receipt, const EvolutionPlan& plan,
                                      const std::vector<MigrationReceipt>& existing,
                                      const CoordinatorEpoch& current_epoch,
                                      const PlanGeneration& current_generation) {
  if (!receipt.id.valid()) {
    return Error{ErrorCode::invalid_argument,
                 "receipt carries no identity: a receipt that cannot be named cannot be audited or "
                 "replayed"};
  }
  if (receipt.idempotency_key.empty()) {
    return Error{ErrorCode::invalid_argument,
                 "receipt carries an empty idempotency key: acceptance must always be identifyable "
                 "for replay"};
  }
  if (!text::valid_identifier(receipt.idempotency_key)) {
    return Error{ErrorCode::invalid_argument,
                 "receipt idempotency key is not a usable identity: " +
                     text::escape_for_output(receipt.idempotency_key)};
  }

  ReceiptOutcome outcome;
  if (receipt.plan_generation < current_generation) {
    outcome.verdict = ReceiptVerdict::fenced_generation;
    outcome.explanation = "receipt carries plan generation " +
                          number(receipt.plan_generation.value()) + " but the plan is at generation " +
                          number(current_generation.value()) +
                          "; a receipt from a superseded generation is fenced";
    return outcome;
  }
  if (receipt.epoch != current_epoch) {
    outcome.verdict = ReceiptVerdict::stale_epoch;
    outcome.explanation =
        "receipt was issued for epoch " + number(receipt.epoch.value()) +
        " but the coordinator is at epoch " + number(current_epoch.value()) +
        "; only the epoch that issued the authority can accept its receipt, so an epoch the "
        "coordinator has not reached is rejected exactly like an older one";
    return outcome;
  }
  if (find_site_record(plan, receipt.site) == nullptr) {
    outcome.verdict = ReceiptVerdict::wrong_site;
    outcome.explanation = "receipt names site " + name_of(receipt.site) +
                          ", which is not a member of plan " + name_of(plan.identity.id);
    return outcome;
  }
  const std::optional<std::size_t> index = step_index(plan, receipt.step);
  if (!index.has_value()) {
    outcome.verdict = ReceiptVerdict::unknown_step;
    outcome.explanation = "receipt names step " + name_of(receipt.step) +
                          ", which is not a migration step of plan " + name_of(plan.identity.id);
    return outcome;
  }
  const MigrationStep& step = plan.steps[*index];
  const std::uint64_t expected_stage = static_cast<std::uint64_t>(*index) + 1u;
  if (static_cast<std::uint64_t>(receipt.stage.value()) != expected_stage) {
    outcome.verdict = ReceiptVerdict::wrong_stage;
    outcome.explanation = "receipt claims stage " + number(receipt.stage.value()) + " for step " +
                          name_of(receipt.step) + ", but that step is stage " +
                          number(expected_stage) +
                          " of canonical step order, so the site did not advance by exactly one "
                          "stage";
    return outcome;
  }
  if (receipt.from != step.from || receipt.to != step.to) {
    outcome.verdict = ReceiptVerdict::wrong_stage;
    outcome.explanation =
        "receipt moves step " + name_of(receipt.step) + " from " + receipt.from.to_string() +
        " to " + receipt.to.to_string() + ", but the plan moves it from " + step.from.to_string() +
        " to " + step.to.to_string();
    return outcome;
  }

  bool key_conflict = false;
  bool id_conflict = false;
  bool key_replay = false;
  for (const MigrationReceipt& prior : existing) {
    const bool same_key = prior.idempotency_key == receipt.idempotency_key;
    const bool same_id = prior.id == receipt.id;
    if (!same_key && !same_id) {
      continue;
    }
    if (same_receipt_content(prior, receipt)) {
      key_replay = key_replay || same_key;
      continue;
    }
    key_conflict = key_conflict || same_key;
    id_conflict = id_conflict || same_id;
  }
  if (key_conflict || id_conflict) {
    outcome.verdict = ReceiptVerdict::conflicting;
    outcome.explanation =
        key_conflict
            ? "idempotency key " + text::escape_for_output(receipt.idempotency_key) +
                  " is already recorded with different content; one key cannot describe two "
                  "different acceptances"
            : "receipt identity " + name_of(receipt.id) +
                  " is already recorded with different content; the same receipt cannot describe "
                  "two different acceptances";
    return outcome;
  }
  if (key_replay) {
    outcome.verdict = ReceiptVerdict::duplicate;
    outcome.explanation = "idempotency key " + text::escape_for_output(receipt.idempotency_key) +
                          " was already accepted with identical content; this is an idempotent "
                          "replay and changes nothing";
    return outcome;
  }

  outcome.verdict = ReceiptVerdict::accepted;
  outcome.explanation = "receipt " + name_of(receipt.id) + " accepted: site " +
                        name_of(receipt.site) + " reached stage " + number(receipt.stage.value()) +
                        " by accepting step " + name_of(receipt.step) + " at generation " +
                        number(current_generation.value());
  return outcome;
}

RollbackEligibility assess_rollback_eligibility(const EvolutionPlan& plan,
                                                const RollbackMarker& marker,
                                                CohortWave target_wave) {
  if (!plan.rollback.permitted) {
    return RollbackEligibility{false,
                               "rollback is refused: the plan's rollback policy does not permit it"};
  }
  if (marker.crossed_point_of_no_return) {
    return RollbackEligibility{
        false, "rollback is refused: the marker records that the point of no return was crossed"};
  }
  if (plan.point_of_no_return.has_value() && marker.wave > plan.point_of_no_return->wave) {
    return RollbackEligibility{
        false, "rollback is refused: the marker stands at wave " + number(marker.wave.value()) +
                   ", beyond the plan's point of no return at wave " +
                   number(plan.point_of_no_return->wave.value())};
  }

  const std::optional<std::uint32_t> waves_back =
      checked_sub<std::uint32_t>(marker.wave.value(), target_wave.value());
  if (!waves_back.has_value()) {
    return RollbackEligibility{
        false, "rollback is refused: a rollback never moves forward, and target wave " +
                   number(target_wave.value()) + " is ahead of the marker at wave " +
                   number(marker.wave.value())};
  }
  if (*waves_back > plan.rollback.max_waves_back.value()) {
    return RollbackEligibility{
        false, "rollback is refused: target wave " + number(target_wave.value()) + " is " +
                   number(*waves_back) + " wave(s) behind the marker at wave " +
                   number(marker.wave.value()) + ", beyond the policy limit of " +
                   number(plan.rollback.max_waves_back.value()) + " wave(s) back"};
  }

  for (const StepId& step : plan.rollback.irreversible_steps) {
    const std::optional<std::size_t> index = step_index(plan, step);
    if (!index.has_value()) {
      // Not a step of this plan, so it records nothing that has been applied.
      continue;
    }
    const std::uint64_t applied_through = static_cast<std::uint64_t>(*index) + 1u;
    if (static_cast<std::uint64_t>(marker.stage.value()) > applied_through) {
      return RollbackEligibility{
          false, "rollback is refused: irreversible step " + name_of(step) + " (stage " +
                     number(applied_through) + " of canonical step order) has already been applied, "
                     "and the marker stands at stage " + number(marker.stage.value())};
    }
  }

  return RollbackEligibility{
      true, "rollback is eligible: the marker stands at wave " + number(marker.wave.value()) +
                " and stage " + number(marker.stage.value()) + ", and may move back to wave " +
                number(target_wave.value()) + ", " + number(*waves_back) +
                " wave(s) back within the policy limit of " +
                number(plan.rollback.max_waves_back.value()) +
                " wave(s); no irreversible step has been applied"};
}

}  // namespace dce
