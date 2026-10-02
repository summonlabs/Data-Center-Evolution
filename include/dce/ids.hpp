// Stable identities and monotonic counters.
//
// Identity types are distinct C++ types even though they share a
// representation, so a site identity cannot be passed where a component
// identity is expected. Counters are distinct types for the same reason, and
// every increment is overflow-checked.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "dce/checked.hpp"
#include "dce/status.hpp"
#include "dce/text.hpp"

namespace dce {

// A validated, stable identifier. The empty string is not a valid identity:
// "absent" is expressed with std::optional at the use site rather than with a
// sentinel value here.
template <class Tag>
class BasicId {
 public:
  BasicId() = default;

  [[nodiscard]] static Result<BasicId> parse(std::string_view value) {
    if (!text::valid_identifier(value)) {
      return Error{ErrorCode::invalid_argument, "identifier rejected: " + text::escape_for_output(value)};
    }
    return BasicId(std::string(value));
  }

  [[nodiscard]] const std::string& str() const noexcept { return value_; }
  [[nodiscard]] std::string_view view() const noexcept { return value_; }
  [[nodiscard]] bool valid() const noexcept { return !value_.empty(); }

  auto operator<=>(const BasicId&) const = default;

 private:
  explicit BasicId(std::string value) : value_(std::move(value)) {}

  std::string value_;
};

struct ActorIdTag;
struct EvidenceIdTag;
struct EvolutionPlanIdTag;
struct SiteIdTag;
struct ComponentIdTag;
struct CohortIdTag;
struct StepIdTag;
struct GateIdTag;
struct CheckpointIdTag;
struct ReceiptIdTag;
struct CapabilityIdTag;
struct PolicyGenerationIdTag;
struct BoundaryRefIdTag;
struct ObligationIdTag;
struct ExceptionIdTag;

using ActorId = BasicId<ActorIdTag>;
using EvidenceId = BasicId<EvidenceIdTag>;
using EvolutionPlanId = BasicId<EvolutionPlanIdTag>;
using SiteId = BasicId<SiteIdTag>;
using ComponentId = BasicId<ComponentIdTag>;
using CohortId = BasicId<CohortIdTag>;
using StepId = BasicId<StepIdTag>;
using GateId = BasicId<GateIdTag>;
using CheckpointId = BasicId<CheckpointIdTag>;
using ReceiptId = BasicId<ReceiptIdTag>;
using CapabilityId = BasicId<CapabilityIdTag>;
using PolicyGenerationId = BasicId<PolicyGenerationIdTag>;
using BoundaryRefId = BasicId<BoundaryRefIdTag>;
using ObligationId = BasicId<ObligationIdTag>;
using ExceptionId = BasicId<ExceptionIdTag>;

// An unsigned counter with a distinct type per meaning. Default-constructed
// counters are zero, which is the documented "none yet" value for generations
// and the first stage for ordinals.
template <class Tag, class Rep>
class BasicCounter {
 public:
  using rep_type = Rep;

  constexpr BasicCounter() noexcept = default;
  constexpr explicit BasicCounter(Rep value) noexcept : value_(value) {}

  [[nodiscard]] static constexpr BasicCounter from_value(Rep value) noexcept {
    return BasicCounter(value);
  }

  [[nodiscard]] constexpr Rep value() const noexcept { return value_; }
  [[nodiscard]] constexpr bool is_zero() const noexcept { return value_ == 0; }

  auto operator<=>(const BasicCounter&) const = default;

  // Advances the counter, refusing to wrap.
  [[nodiscard]] Result<BasicCounter> next() const {
    const std::optional<Rep> advanced = checked_next<Rep>(value_);
    if (!advanced.has_value()) {
      return Error{ErrorCode::overflow, "counter exhausted at its maximum value"};
    }
    return BasicCounter(*advanced);
  }

  // Refuses to move a counter backwards, which is how monotonicity is enforced
  // rather than merely asserted in a comment.
  [[nodiscard]] Status require_at_least(BasicCounter floor) const {
    if (value_ < floor.value_) {
      return Error{ErrorCode::stale, "counter is behind the authority floor"};
    }
    return Status{};
  }

 private:
  Rep value_{0};
};

struct PlanGenerationTag;
struct CoordinatorEpochTag;
struct SiteGenerationTag;
struct LogSequenceTag;
struct StageOrdinalTag;
struct CohortWaveTag;
struct PlanRevisionTag;

using PlanGeneration = BasicCounter<PlanGenerationTag, std::uint64_t>;
using CoordinatorEpoch = BasicCounter<CoordinatorEpochTag, std::uint64_t>;
using SiteGeneration = BasicCounter<SiteGenerationTag, std::uint64_t>;
using LogSequence = BasicCounter<LogSequenceTag, std::uint64_t>;
using StageOrdinal = BasicCounter<StageOrdinalTag, std::uint32_t>;
using CohortWave = BasicCounter<CohortWaveTag, std::uint32_t>;
using PlanRevision = BasicCounter<PlanRevisionTag, std::uint32_t>;

}  // namespace dce
