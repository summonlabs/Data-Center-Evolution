// Delegated rollout authority, fencing, receipts and rollback eligibility.
//
// A site is sovereign by default. It acts on evolution only when the plan
// explicitly delegates rollout authority to it and it presents an authority
// token that is current for the coordinator's epoch and the plan's generation.
// A token from a superseded epoch is fenced, never honoured.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "dce/digest.hpp"
#include "dce/ids.hpp"
#include "dce/limits.hpp"
#include "dce/plan.hpp"
#include "dce/status.hpp"

namespace dce {

struct AuthorityToken {
  EvolutionPlanId plan;
  PlanGeneration plan_generation;
  CoordinatorEpoch epoch;
  SiteId site;
  CohortId cohort;
  StageOrdinal stage;
  Digest256 plan_digest;

  auto operator<=>(const AuthorityToken&) const = default;
};

[[nodiscard]] Digest256 compute_token_digest(const AuthorityToken& token);

enum class AuthorityVerdict : std::uint8_t {
  valid,
  stale_epoch,
  fenced_generation,
  plan_mismatch,
  digest_mismatch,
  not_delegated,
  unknown_site,
};

[[nodiscard]] const char* to_string(AuthorityVerdict verdict) noexcept;

struct AuthorityCheck {
  AuthorityVerdict verdict{AuthorityVerdict::valid};
  std::string explanation;

  [[nodiscard]] bool valid() const noexcept { return verdict == AuthorityVerdict::valid; }
};

[[nodiscard]] AuthorityCheck check_authority(const AuthorityToken& token, const EvolutionPlan& plan,
                                             const CoordinatorEpoch& current_epoch,
                                             const PlanGeneration& current_generation);

// A migration receipt is the durable record that a site accepted a stage.
// Acceptance is idempotent by idempotency key: replaying the same effect is a
// duplicate and changes nothing, while the same key carrying different content
// is a conflict that must never be silently accepted.
struct MigrationReceipt {
  ReceiptId id;
  EvolutionPlanId plan;
  PlanGeneration plan_generation;
  CoordinatorEpoch epoch;
  SiteId site;
  StepId step;
  StageOrdinal stage;
  Version from;
  Version to;
  SiteGeneration accepted_generation;
  Digest256 evidence_digest;
  std::string idempotency_key;

  auto operator<=>(const MigrationReceipt&) const = default;
};

enum class ReceiptVerdict : std::uint8_t {
  accepted,
  duplicate,
  conflicting,
  stale_epoch,
  fenced_generation,
  unknown_step,
  wrong_site,
  wrong_stage,
};

[[nodiscard]] const char* to_string(ReceiptVerdict verdict) noexcept;

struct ReceiptOutcome {
  ReceiptVerdict verdict{ReceiptVerdict::accepted};
  std::string explanation;
};

[[nodiscard]] Result<ReceiptOutcome> accept_receipt(const MigrationReceipt& receipt,
                                                    const EvolutionPlan& plan,
                                                    const std::vector<MigrationReceipt>& existing,
                                                    const CoordinatorEpoch& current_epoch,
                                                    const PlanGeneration& current_generation);

// A checkpoint is the coordinator's durable statement that a site reached a
// stage. It is written after the receipt, never instead of it.
struct StageCheckpoint {
  CheckpointId id;
  EvolutionPlanId plan;
  PlanGeneration plan_generation;
  CoordinatorEpoch epoch;
  SiteId site;
  CohortId cohort;
  CohortWave wave;
  StageOrdinal stage;
  SiteGeneration site_generation;
  Digest256 stage_digest;
  ReceiptId receipt;

  auto operator<=>(const StageCheckpoint&) const = default;
};

// The point of no return is crossed by an event, and the event is recorded.
struct RollbackMarker {
  EvolutionPlanId plan;
  PlanGeneration plan_generation;
  CoordinatorEpoch epoch;
  CohortWave wave;
  StageOrdinal stage;
  StepId step;
  bool crossed_point_of_no_return{false};
  std::string justification;

  auto operator<=>(const RollbackMarker&) const = default;
};

struct RollbackEligibility {
  bool eligible{false};
  std::string explanation;
};

// Rollback is available only before the recorded irreversible boundary, and
// only as far back as the policy allows.
[[nodiscard]] RollbackEligibility assess_rollback_eligibility(const EvolutionPlan& plan,
                                                              const RollbackMarker& marker,
                                                              CohortWave target_wave);

}  // namespace dce
