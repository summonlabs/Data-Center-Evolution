// Reconciliation of a reconnecting site.
//
// A partitioned site keeps serving locally: local sovereignty is not suspended
// by losing the coordinator. What it loses is the right to advance. When it
// reconnects it reports what it actually accepted, and the coordinator moves
// its own belief to match reality before any new authority is issued. A
// coordinator that believed it was further along than the fleet really is must
// rewind, and rewinding is recorded rather than hidden.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "dce/digest.hpp"
#include "dce/ids.hpp"
#include "dce/limits.hpp"
#include "dce/rollout.hpp"
#include "dce/status.hpp"

namespace dce {

struct SiteStageReport {
  SiteId site;
  SiteGeneration generation;
  CoordinatorEpoch epoch;
  StageOrdinal accepted_stage;
  Digest256 accepted_stage_digest;
  std::vector<ReceiptId> receipts;
  bool partitioned{false};

  auto operator<=>(const SiteStageReport&) const = default;
};

enum class ReconciliationAction : std::uint8_t {
  agree,    // the coordinator's belief matches the reported reality
  advance,  // the site legitimately advanced under delegated authority
  rewind,   // the coordinator believed more progress than actually happened
  hold,     // the site reported nothing conclusive; authority stays withheld
  reject,   // the report contradicts authority that was never delegated
};

[[nodiscard]] const char* to_string(ReconciliationAction action) noexcept;

// FENCING. Whenever reconciliation does not simply agree, the coordinator
// issues next_generation = coordinator_view.generation + 1 rather than
// adopting the site's counter, because a generation the coordinator may
// already have published must never be reused. Generations therefore stay
// monotonic across a rewind, and a stale completion from before the rewind
// can never mutate the newer generation.
struct ReconciliationOutcome {
  ReconciliationAction action{ReconciliationAction::hold};
  StageOrdinal agreed_stage;
  SiteGeneration next_generation;
  Digest256 evidence_digest;
  std::string explanation;
};

[[nodiscard]] Result<ReconciliationOutcome> reconcile(const SiteProgress& coordinator_view,
                                                      const SiteStageReport& reported,
                                                      const PlanGeneration& plan_generation,
                                                      const CoordinatorEpoch& epoch,
                                                      bool site_has_delegated_authority);

}  // namespace dce
