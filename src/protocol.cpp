// Framing and body encoding for the control-plane link.
//
// A frame is one 16-byte header followed by one canonically encoded body:
//
//   offset  width  field
//   0       4      magic, kFrameMagic
//   4       2      protocol version, kProtocolVersion
//   6       2      frame kind: a MessageKind, or kAdminFrameKind for
//                  administrative traffic
//   8       4      payload length in bytes
//   12      4      CRC-32 of exactly the payload bytes
//
// The declared length is validated against kMaxFrameBytes before a single byte
// is allocated for the payload, and the CRC is verified before the body is
// interpreted, so a corrupted frame is reported as corruption rather than as a
// malformed body. A peer that closes the connection cleanly is reported as
// ErrorCode::closed, which is what lets a caller tell a partition from a
// protocol defect.
//
// A body is the canonical encoding of the message fields in declaration order,
// written through CanonicalWriter. Optional fields are one presence byte
// followed by the value only when present, and every collection is written
// with the same bound from dce/limits.hpp that the decoder enforces, so an
// encoded body is accepted by the matching decoder whenever each identity
// field carries valid identifier text - the rule the canonical codec applies
// to an identity everywhere else. Decoding is total: a body that does not
// decode exactly, trailing bytes included, is refused rather than applied in
// part.
#include "dce/protocol.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <new>
#include <optional>
#include <span>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "dce/canonical.hpp"
#include "dce/codec.hpp"
#include "dce/digest.hpp"
#include "dce/limits.hpp"

namespace dce::wire {
namespace {

// ---------------------------------------------------------------------------
// Frame header
// ---------------------------------------------------------------------------
constexpr std::size_t kMagicOffset = 0;
constexpr std::size_t kMagicBytes = 4;
constexpr std::size_t kVersionOffset = kMagicOffset + kMagicBytes;
constexpr std::size_t kVersionBytes = 2;
constexpr std::size_t kKindOffset = kVersionOffset + kVersionBytes;
constexpr std::size_t kKindBytes = 2;
constexpr std::size_t kLengthOffset = kKindOffset + kKindBytes;
constexpr std::size_t kLengthBytes = 4;
constexpr std::size_t kCrcOffset = kLengthOffset + kLengthBytes;
constexpr std::size_t kCrcBytes = 4;

static_assert(kCrcOffset + kCrcBytes == kFrameHeaderBytes,
              "the frame header fields must cover exactly kFrameHeaderBytes");

// Little-endian, independent of the host byte order.
void store_le(std::span<std::byte> out, std::size_t offset, std::uint32_t value,
              std::size_t width) noexcept {
  for (std::size_t index = 0; index < width; ++index) {
    out[offset + index] = static_cast<std::byte>((value >> (index * 8u)) & 0xffu);
  }
}

[[nodiscard]] std::uint32_t load_le(std::span<const std::byte> in, std::size_t offset,
                                    std::size_t width) noexcept {
  std::uint32_t value = 0;
  for (std::size_t index = 0; index < width; ++index) {
    value |= std::to_integer<std::uint32_t>(in[offset + index]) << (index * 8u);
  }
  return value;
}

struct FrameHeader {
  std::uint16_t kind{0};
  std::uint32_t length{0};
};

// Magic, version and length are decided before the payload is touched. A frame
// that declares more than kMaxFrameBytes is refused here, so the length can
// never become an allocation.
[[nodiscard]] Result<FrameHeader> parse_header(std::span<const std::byte> header) {
  if (load_le(header, kMagicOffset, kMagicBytes) != kFrameMagic) {
    return Error{ErrorCode::malformed, "the frame magic is not the DCE frame magic"};
  }
  if (load_le(header, kVersionOffset, kVersionBytes) !=
      static_cast<std::uint32_t>(kProtocolVersion)) {
    return Error{ErrorCode::malformed, "the frame protocol version is not supported"};
  }
  const std::uint32_t length = load_le(header, kLengthOffset, kLengthBytes);
  if (length > kMaxFrameBytes) {
    return Error{ErrorCode::limit_exceeded, "the frame declares a payload above the documented bound"};
  }
  return FrameHeader{.kind = static_cast<std::uint16_t>(load_le(header, kKindOffset, kKindBytes)),
                     .length = length};
}

// Every entry point runs inside this guard, so an allocation failure or any
// other unexpected fault is reported as an Error instead of escaping as an
// exception. Frames are the hostile-input boundary; nothing here may throw.
template <class Function>
[[nodiscard]] auto guarded(Function&& function) -> std::invoke_result_t<Function> {
  try {
    return function();
  } catch (const std::bad_alloc&) {
    // The handler messages are short on purpose: reporting that an allocation
    // failed must not itself need one.
    return Error{ErrorCode::limit_exceeded, "out of memory"};
  } catch (...) {
    return Error{ErrorCode::internal, "internal fault"};
  }
}

// ---------------------------------------------------------------------------
// Enumerated fields
// ---------------------------------------------------------------------------
// MessageKind, AdminOp, ErrorCode and RefusalCode have no canonical codec
// specialisation, so they travel as a u16 and are range-checked in both
// directions: a value outside the declared range is refused rather than
// silently reinterpreted. The bounds are the declared enumerators themselves.
constexpr std::uint16_t kMinMessageKind = static_cast<std::uint16_t>(MessageKind::hello);
constexpr std::uint16_t kMaxMessageKind = static_cast<std::uint16_t>(MessageKind::failure);
constexpr std::uint16_t kMinAdminOp = static_cast<std::uint16_t>(AdminOp::status);
constexpr std::uint16_t kMaxAdminOp = static_cast<std::uint16_t>(AdminOp::stop);
constexpr std::uint16_t kMinErrorCode = static_cast<std::uint16_t>(ErrorCode::ok);
constexpr std::uint16_t kMaxErrorCode = static_cast<std::uint16_t>(ErrorCode::internal);
constexpr std::uint16_t kMinRefusalCode = static_cast<std::uint16_t>(RefusalCode::structural_defect);
constexpr std::uint16_t kMaxRefusalCode = static_cast<std::uint16_t>(RefusalCode::step_cycle);

template <class Enum>
[[nodiscard]] Status put_u16_enum(CanonicalWriter& writer, Enum value, std::uint16_t min_value,
                                  std::uint16_t max_value, const char* field) {
  const std::uint16_t raw = static_cast<std::uint16_t>(value);
  if (raw < min_value || raw > max_value) {
    return Error{ErrorCode::out_of_range, std::string("enumerated field out of range: ") + field};
  }
  return writer.put_u16(raw);
}

template <class Enum>
[[nodiscard]] Result<Enum> get_u16_enum(CanonicalReader& reader, std::uint16_t min_value,
                                        std::uint16_t max_value, const char* field) {
  DCE_ASSIGN(raw, reader.u16());
  if (raw < min_value || raw > max_value) {
    return Error{ErrorCode::malformed, std::string("enumerated field out of range: ") + field};
  }
  return static_cast<Enum>(raw);
}

// Text is written through here so that the encoder applies the same bound as
// the decoder: a field that could not be read back is refused instead of being
// written as a body no reader would accept.
[[nodiscard]] Status put_text(CanonicalWriter& writer, const std::string& value) {
  if (value.size() > kMaxTextBytes) {
    return Error{ErrorCode::limit_exceeded, "a text field exceeds its documented bound"};
  }
  return writer.put_string(value);
}

// ---------------------------------------------------------------------------
// Payload members
// ---------------------------------------------------------------------------
// Every member is written through PayloadCodec. The primary template forwards
// to the canonical codec in dce/codec.hpp; the types that have no codec
// specialisation are encoded here, in declaration order, under the same rules.
template <class T>
struct PayloadCodec {
  static Status put(CanonicalWriter& writer, const T& value) {
    return codec::put<T>(writer, value);
  }

  static Result<T> get(CanonicalReader& reader) { return codec::get<T>(reader); }
};

// An identity field on the wire may legitimately be absent. A site cannot name
// the plan before the coordinator has told it one, a coordinator reply does not
// always name a cohort or a step, and an administrative request that only asks
// for status names no plan at all. The canonical codec keeps its strict rule
// for durable payloads - a persisted identity must parse - but a transport
// field distinguishes absence by carrying no text, which round-trips exactly.
// Anything present must still be a valid identity, so this is not a relaxation
// of the identifier rules; it is an explicit representation of "not set".
template <class Tag>
struct PayloadCodec<BasicId<Tag>> {
  static Status put(CanonicalWriter& writer, const BasicId<Tag>& value) {
    return writer.put_string(value.view());
  }

  static Result<BasicId<Tag>> get(CanonicalReader& reader) {
    DCE_ASSIGN(text, reader.string(dce::text::kMaxIdentifierLength));
    if (text.empty()) {
      return BasicId<Tag>{};
    }
    return BasicId<Tag>::parse(text);
  }
};

// Encoders of one value call list and optional encoders, and those call back
// into the value encoders, so the helpers are declared before the
// specialisations that use them and defined immediately afterwards.
template <class T>
[[nodiscard]] Status put_value(CanonicalWriter& writer, const T& value);

template <class T>
[[nodiscard]] Result<T> get_value(CanonicalReader& reader);

template <class T>
[[nodiscard]] Status put_list(CanonicalWriter& writer, const std::vector<T>& items,
                              std::size_t max_items);

template <class T>
[[nodiscard]] Result<std::vector<T>> get_list(CanonicalReader& reader, std::size_t max_items);

template <class T>
[[nodiscard]] Status put_optional(CanonicalWriter& writer, const std::optional<T>& value);

template <class T>
[[nodiscard]] Result<std::optional<T>> get_optional(CanonicalReader& reader);

// The five booleans of a lifecycle transition, in declaration order.
template <>
struct PayloadCodec<TransitionContext> {
  static Status put(CanonicalWriter& writer, const TransitionContext& value) {
    DCE_TRY(writer.put_bool(value.gates_satisfied));
    DCE_TRY(writer.put_bool(value.all_cohorts_advanced));
    DCE_TRY(writer.put_bool(value.rollback_eligible));
    DCE_TRY(writer.put_bool(value.point_of_no_return_crossed));
    DCE_TRY(writer.put_bool(value.reconciliation_rewound));
    return Status{};
  }

  static Result<TransitionContext> get(CanonicalReader& reader) {
    DCE_ASSIGN(gates_satisfied, reader.boolean());
    DCE_ASSIGN(all_cohorts_advanced, reader.boolean());
    DCE_ASSIGN(rollback_eligible, reader.boolean());
    DCE_ASSIGN(point_of_no_return_crossed, reader.boolean());
    DCE_ASSIGN(reconciliation_rewound, reader.boolean());
    return TransitionContext{.gates_satisfied = gates_satisfied,
                             .all_cohorts_advanced = all_cohorts_advanced,
                             .rollback_eligible = rollback_eligible,
                             .point_of_no_return_crossed = point_of_no_return_crossed,
                             .reconciliation_rewound = reconciliation_rewound};
  }
};

// One observed site. The policy generation stays optional: an absent policy is
// "unknown", which is not a generation of zero.
template <>
struct PayloadCodec<SiteObservation> {
  static Status put(CanonicalWriter& writer, const SiteObservation& value) {
    DCE_TRY(put_value<SiteId>(writer, value.site));
    DCE_TRY(put_value<Version>(writer, value.dccp_version));
    DCE_TRY(put_list<ComponentVersion>(writer, value.components, kMaxComponents));
    DCE_TRY(put_value<SiteGeneration>(writer, value.accepted_generation));
    DCE_TRY(put_value<CapabilitySet>(writer, value.capabilities));
    DCE_TRY(put_optional<PolicyGeneration>(writer, value.policy));
    DCE_TRY(put_value<Digest256>(writer, value.state_digest));
    DCE_TRY(writer.put_bool(value.delegated_rollout_authority));
    return Status{};
  }

  static Result<SiteObservation> get(CanonicalReader& reader) {
    DCE_ASSIGN(site, get_value<SiteId>(reader));
    DCE_ASSIGN(dccp_version, get_value<Version>(reader));
    DCE_ASSIGN(components, get_list<ComponentVersion>(reader, kMaxComponents));
    DCE_ASSIGN(accepted_generation, get_value<SiteGeneration>(reader));
    DCE_ASSIGN(capabilities, get_value<CapabilitySet>(reader));
    DCE_ASSIGN(policy, get_optional<PolicyGeneration>(reader));
    DCE_ASSIGN(state_digest, get_value<Digest256>(reader));
    DCE_ASSIGN(delegated_rollout_authority, reader.boolean());
    return SiteObservation{.site = site,
                           .dccp_version = dccp_version,
                           .components = components,
                           .accepted_generation = accepted_generation,
                           .capabilities = capabilities,
                           .policy = policy,
                           .state_digest = state_digest,
                           .delegated_rollout_authority = delegated_rollout_authority};
  }
};

// What the coordinator observed, bound to the plan it validates.
template <>
struct PayloadCodec<ValidationContext> {
  static Status put(CanonicalWriter& writer, const ValidationContext& value) {
    DCE_TRY(put_value<CoordinatorEpoch>(writer, value.epoch));
    DCE_TRY(put_value<PlanGeneration>(writer, value.current_generation));
    DCE_TRY(put_list<SiteObservation>(writer, value.observed_sites, kMaxSites));
    DCE_TRY(put_list<CapabilityDependencyProof>(writer, value.removal_proofs, kMaxComponents));
    return Status{};
  }

  static Result<ValidationContext> get(CanonicalReader& reader) {
    DCE_ASSIGN(epoch, get_value<CoordinatorEpoch>(reader));
    DCE_ASSIGN(current_generation, get_value<PlanGeneration>(reader));
    DCE_ASSIGN(observed_sites, get_list<SiteObservation>(reader, kMaxSites));
    DCE_ASSIGN(removal_proofs, get_list<CapabilityDependencyProof>(reader, kMaxComponents));
    return ValidationContext{.epoch = epoch,
                             .current_generation = current_generation,
                             .observed_sites = observed_sites,
                             .removal_proofs = removal_proofs};
  }
};

// One validation finding: the code names the defect, the subject names what
// carries it, and the explanation says why in full.
template <>
struct PayloadCodec<Refusal> {
  static Status put(CanonicalWriter& writer, const Refusal& value) {
    DCE_TRY(put_u16_enum(writer, value.code, kMinRefusalCode, kMaxRefusalCode, "RefusalCode"));
    DCE_TRY(put_text(writer, value.subject));
    DCE_TRY(put_text(writer, value.explanation));
    return Status{};
  }

  static Result<Refusal> get(CanonicalReader& reader) {
    DCE_ASSIGN(code, get_u16_enum<RefusalCode>(reader, kMinRefusalCode, kMaxRefusalCode, "RefusalCode"));
    DCE_ASSIGN(subject, reader.string(kMaxTextBytes));
    DCE_ASSIGN(explanation, reader.string(kMaxTextBytes));
    return Refusal{.code = code, .subject = subject, .explanation = explanation};
  }
};

// The outcome of one gate, with the evidence that decided it.
template <>
struct PayloadCodec<GateEvaluation> {
  static Status put(CanonicalWriter& writer, const GateEvaluation& value) {
    DCE_TRY(put_value<GateId>(writer, value.gate));
    DCE_TRY(put_value<GateOutcome>(writer, value.outcome));
    DCE_TRY(put_text(writer, value.explanation));
    DCE_TRY(put_list<EvidenceId>(writer, value.satisfied_by, kMaxGateEvidence));
    return Status{};
  }

  static Result<GateEvaluation> get(CanonicalReader& reader) {
    DCE_ASSIGN(gate, get_value<GateId>(reader));
    DCE_ASSIGN(outcome, get_value<GateOutcome>(reader));
    DCE_ASSIGN(explanation, reader.string(kMaxTextBytes));
    DCE_ASSIGN(satisfied_by, get_list<EvidenceId>(reader, kMaxGateEvidence));
    return GateEvaluation{.gate = gate,
                          .outcome = outcome,
                          .explanation = explanation,
                          .satisfied_by = satisfied_by};
  }
};

// A refusal is reported per finding, so the finding list carries the widest
// bound a single validation pass can produce; the collections it names carry
// the bounds their own names declare.
template <>
struct PayloadCodec<ValidationReport> {
  static Status put(CanonicalWriter& writer, const ValidationReport& value) {
    DCE_TRY(put_value<Digest256>(writer, value.plan_digest));
    DCE_TRY(put_value<Digest256>(writer, value.observed_state_digest));
    DCE_TRY(put_list<Refusal>(writer, value.refusals, kMaxSites));
    DCE_TRY(put_list<GateEvaluation>(writer, value.gates, kMaxGates));
    DCE_TRY(put_list<MigrationStep>(writer, value.planned_steps, kMaxSteps));
    DCE_TRY(put_list<RolloutCohort>(writer, value.planned_cohorts, kMaxCohorts));
    DCE_TRY(writer.put_u64(value.checked_sites));
    DCE_TRY(writer.put_u64(value.checked_steps));
    DCE_TRY(writer.put_u64(value.checked_edges));
    DCE_TRY(writer.put_u64(value.checked_gates));
    return Status{};
  }

  static Result<ValidationReport> get(CanonicalReader& reader) {
    DCE_ASSIGN(plan_digest, get_value<Digest256>(reader));
    DCE_ASSIGN(observed_state_digest, get_value<Digest256>(reader));
    DCE_ASSIGN(refusals, get_list<Refusal>(reader, kMaxSites));
    DCE_ASSIGN(gates, get_list<GateEvaluation>(reader, kMaxGates));
    DCE_ASSIGN(planned_steps, get_list<MigrationStep>(reader, kMaxSteps));
    DCE_ASSIGN(planned_cohorts, get_list<RolloutCohort>(reader, kMaxCohorts));
    DCE_ASSIGN(checked_sites, reader.u64());
    DCE_ASSIGN(checked_steps, reader.u64());
    DCE_ASSIGN(checked_edges, reader.u64());
    DCE_ASSIGN(checked_gates, reader.u64());
    return ValidationReport{.plan_digest = plan_digest,
                            .observed_state_digest = observed_state_digest,
                            .refusals = refusals,
                            .gates = gates,
                            .planned_steps = planned_steps,
                            .planned_cohorts = planned_cohorts,
                            .checked_sites = checked_sites,
                            .checked_steps = checked_steps,
                            .checked_edges = checked_edges,
                            .checked_gates = checked_gates};
  }
};

// The next rollout action, as an instruction the receiving runtime can carry
// out without holding the plan.
template <>
struct PayloadCodec<RolloutAction> {
  static Status put(CanonicalWriter& writer, const RolloutAction& value) {
    DCE_TRY(put_value<RolloutActionKind>(writer, value.kind));
    DCE_TRY(put_value<SiteId>(writer, value.site));
    DCE_TRY(put_value<CohortId>(writer, value.cohort));
    DCE_TRY(put_value<StepId>(writer, value.step));
    DCE_TRY(put_value<CohortWave>(writer, value.wave));
    DCE_TRY(put_value<StageOrdinal>(writer, value.target_stage));
    DCE_TRY(put_text(writer, value.explanation));
    return Status{};
  }

  static Result<RolloutAction> get(CanonicalReader& reader) {
    DCE_ASSIGN(kind, get_value<RolloutActionKind>(reader));
    DCE_ASSIGN(site, get_value<SiteId>(reader));
    DCE_ASSIGN(cohort, get_value<CohortId>(reader));
    DCE_ASSIGN(step, get_value<StepId>(reader));
    DCE_ASSIGN(wave, get_value<CohortWave>(reader));
    DCE_ASSIGN(target_stage, get_value<StageOrdinal>(reader));
    DCE_ASSIGN(explanation, reader.string(kMaxTextBytes));
    return RolloutAction{.kind = kind,
                         .site = site,
                         .cohort = cohort,
                         .step = step,
                         .wave = wave,
                         .target_stage = target_stage,
                         .explanation = explanation};
  }
};

template <>
struct PayloadCodec<RollbackEligibility> {
  static Status put(CanonicalWriter& writer, const RollbackEligibility& value) {
    DCE_TRY(writer.put_bool(value.eligible));
    DCE_TRY(put_text(writer, value.explanation));
    return Status{};
  }

  static Result<RollbackEligibility> get(CanonicalReader& reader) {
    DCE_ASSIGN(eligible, reader.boolean());
    DCE_ASSIGN(explanation, reader.string(kMaxTextBytes));
    return RollbackEligibility{.eligible = eligible, .explanation = explanation};
  }
};

template <>
struct PayloadCodec<ReceiptOutcome> {
  static Status put(CanonicalWriter& writer, const ReceiptOutcome& value) {
    DCE_TRY(put_value<ReceiptVerdict>(writer, value.verdict));
    DCE_TRY(put_text(writer, value.explanation));
    return Status{};
  }

  static Result<ReceiptOutcome> get(CanonicalReader& reader) {
    DCE_ASSIGN(verdict, get_value<ReceiptVerdict>(reader));
    DCE_ASSIGN(explanation, reader.string(kMaxTextBytes));
    return ReceiptOutcome{.verdict = verdict, .explanation = explanation};
  }
};

template <>
struct PayloadCodec<ReconciliationOutcome> {
  static Status put(CanonicalWriter& writer, const ReconciliationOutcome& value) {
    DCE_TRY(put_value<ReconciliationAction>(writer, value.action));
    DCE_TRY(put_value<StageOrdinal>(writer, value.agreed_stage));
    DCE_TRY(put_value<SiteGeneration>(writer, value.next_generation));
    DCE_TRY(put_value<Digest256>(writer, value.evidence_digest));
    DCE_TRY(put_text(writer, value.explanation));
    return Status{};
  }

  static Result<ReconciliationOutcome> get(CanonicalReader& reader) {
    DCE_ASSIGN(action, get_value<ReconciliationAction>(reader));
    DCE_ASSIGN(agreed_stage, get_value<StageOrdinal>(reader));
    DCE_ASSIGN(next_generation, get_value<SiteGeneration>(reader));
    DCE_ASSIGN(evidence_digest, get_value<Digest256>(reader));
    DCE_ASSIGN(explanation, reader.string(kMaxTextBytes));
    return ReconciliationOutcome{.action = action,
                                 .agreed_stage = agreed_stage,
                                 .next_generation = next_generation,
                                 .evidence_digest = evidence_digest,
                                 .explanation = explanation};
  }
};

// ---------------------------------------------------------------------------
// Member helpers
// ---------------------------------------------------------------------------
template <class T>
Status put_value(CanonicalWriter& writer, const T& value) {
  return PayloadCodec<T>::put(writer, value);
}

template <class T>
Result<T> get_value(CanonicalReader& reader) {
  return PayloadCodec<T>::get(reader);
}

template <class T>
Status put_list(CanonicalWriter& writer, const std::vector<T>& items, std::size_t max_items) {
  if (items.size() > max_items) {
    return Error{ErrorCode::limit_exceeded, "a collection exceeds its documented bound"};
  }
  DCE_TRY(writer.put_count(items.size()));
  for (const T& item : items) {
    DCE_TRY(put_value<T>(writer, item));
  }
  return Status{};
}

template <class T>
Result<std::vector<T>> get_list(CanonicalReader& reader, std::size_t max_items) {
  DCE_ASSIGN(count, reader.count(max_items));
  std::vector<T> items;
  items.reserve(count);
  for (std::size_t index = 0; index < count; ++index) {
    DCE_ASSIGN(item, get_value<T>(reader));
    items.push_back(std::move(item));
  }
  return items;
}

template <class T>
Status put_optional(CanonicalWriter& writer, const std::optional<T>& value) {
  DCE_TRY(writer.put_bool(value.has_value()));
  if (value.has_value()) {
    DCE_TRY(put_value<T>(writer, *value));
  }
  return Status{};
}

template <class T>
Result<std::optional<T>> get_optional(CanonicalReader& reader) {
  DCE_ASSIGN(present, reader.boolean());
  if (!present) {
    return std::optional<T>{};
  }
  DCE_ASSIGN(value, get_value<T>(reader));
  return std::optional<T>(std::move(value));
}

// ---------------------------------------------------------------------------
// Bodies
// ---------------------------------------------------------------------------
Status encode_message_body(CanonicalWriter& writer, const Message& message) {
  DCE_TRY(put_u16_enum(writer, message.kind, kMinMessageKind, kMaxMessageKind, "MessageKind"));
  DCE_TRY(writer.put_u64(message.request_id));
  DCE_TRY(put_u16_enum(writer, message.status, kMinErrorCode, kMaxErrorCode, "ErrorCode"));
  DCE_TRY(put_text(writer, message.text));
  DCE_TRY(put_value<CoordinatorEpoch>(writer, message.epoch));
  DCE_TRY(put_value<PlanGeneration>(writer, message.plan_generation));
  DCE_TRY(put_value<EvolutionPlanId>(writer, message.plan));
  DCE_TRY(put_value<Digest256>(writer, message.plan_digest));
  DCE_TRY(put_value<SiteId>(writer, message.site));
  DCE_TRY(put_value<CohortId>(writer, message.cohort));
  DCE_TRY(put_value<CohortWave>(writer, message.wave));
  DCE_TRY(put_value<StageOrdinal>(writer, message.stage));
  DCE_TRY(put_value<StepId>(writer, message.step));
  DCE_TRY(put_value<Version>(writer, message.from_version));
  DCE_TRY(put_value<Version>(writer, message.to_version));
  DCE_TRY(put_value<SiteGeneration>(writer, message.site_generation));
  DCE_TRY(put_value<Digest256>(writer, message.stage_digest));
  DCE_TRY(put_optional<SiteRecord>(writer, message.profile));
  DCE_TRY(put_optional<AuthorityToken>(writer, message.token));
  DCE_TRY(put_optional<MigrationReceipt>(writer, message.receipt));
  DCE_TRY(put_optional<SiteStageReport>(writer, message.report));
  DCE_TRY(put_optional<SiteProgress>(writer, message.progress));
  DCE_TRY(put_optional<StageCheckpoint>(writer, message.checkpoint));
  return Status{};
}

Result<Message> decode_message_body(CanonicalReader& reader) {
  DCE_ASSIGN(kind, get_u16_enum<MessageKind>(reader, kMinMessageKind, kMaxMessageKind, "MessageKind"));
  DCE_ASSIGN(request_id, reader.u64());
  DCE_ASSIGN(status, get_u16_enum<ErrorCode>(reader, kMinErrorCode, kMaxErrorCode, "ErrorCode"));
  DCE_ASSIGN(text, reader.string(kMaxTextBytes));
  DCE_ASSIGN(epoch, get_value<CoordinatorEpoch>(reader));
  DCE_ASSIGN(plan_generation, get_value<PlanGeneration>(reader));
  DCE_ASSIGN(plan, get_value<EvolutionPlanId>(reader));
  DCE_ASSIGN(plan_digest, get_value<Digest256>(reader));
  DCE_ASSIGN(site, get_value<SiteId>(reader));
  DCE_ASSIGN(cohort, get_value<CohortId>(reader));
  DCE_ASSIGN(wave, get_value<CohortWave>(reader));
  DCE_ASSIGN(stage, get_value<StageOrdinal>(reader));
  DCE_ASSIGN(step, get_value<StepId>(reader));
  DCE_ASSIGN(from_version, get_value<Version>(reader));
  DCE_ASSIGN(to_version, get_value<Version>(reader));
  DCE_ASSIGN(site_generation, get_value<SiteGeneration>(reader));
  DCE_ASSIGN(stage_digest, get_value<Digest256>(reader));
  DCE_ASSIGN(profile, get_optional<SiteRecord>(reader));
  DCE_ASSIGN(token, get_optional<AuthorityToken>(reader));
  DCE_ASSIGN(receipt, get_optional<MigrationReceipt>(reader));
  DCE_ASSIGN(report, get_optional<SiteStageReport>(reader));
  DCE_ASSIGN(progress, get_optional<SiteProgress>(reader));
  DCE_ASSIGN(checkpoint, get_optional<StageCheckpoint>(reader));
  return Message{.kind = kind,
                 .request_id = request_id,
                 .status = status,
                 .text = text,
                 .epoch = epoch,
                 .plan_generation = plan_generation,
                 .plan = plan,
                 .plan_digest = plan_digest,
                 .site = site,
                 .cohort = cohort,
                 .wave = wave,
                 .stage = stage,
                 .step = step,
                 .from_version = from_version,
                 .to_version = to_version,
                 .site_generation = site_generation,
                 .stage_digest = stage_digest,
                 .profile = profile,
                 .token = token,
                 .receipt = receipt,
                 .report = report,
                 .progress = progress,
                 .checkpoint = checkpoint};
}

Status encode_admin_request_body(CanonicalWriter& writer, const AdminRequest& request) {
  DCE_TRY(put_u16_enum(writer, request.op, kMinAdminOp, kMaxAdminOp, "AdminOp"));
  DCE_TRY(writer.put_u64(request.request_id));
  DCE_TRY(put_value<EvolutionPlanId>(writer, request.plan));
  DCE_TRY(put_value<PlanGeneration>(writer, request.plan_generation));
  DCE_TRY(put_value<CoordinatorEpoch>(writer, request.epoch));
  DCE_TRY(put_optional<EvolutionPlan>(writer, request.plan_body));
  DCE_TRY(put_optional<ValidationContext>(writer, request.context));
  DCE_TRY(put_value<LifecycleEvent>(writer, request.event));
  DCE_TRY(put_value<TransitionContext>(writer, request.transition));
  DCE_TRY(put_value<CohortWave>(writer, request.target_wave));
  DCE_TRY(put_text(writer, request.text));
  return Status{};
}

Result<AdminRequest> decode_admin_request_body(CanonicalReader& reader) {
  DCE_ASSIGN(op, get_u16_enum<AdminOp>(reader, kMinAdminOp, kMaxAdminOp, "AdminOp"));
  DCE_ASSIGN(request_id, reader.u64());
  DCE_ASSIGN(plan, get_value<EvolutionPlanId>(reader));
  DCE_ASSIGN(plan_generation, get_value<PlanGeneration>(reader));
  DCE_ASSIGN(epoch, get_value<CoordinatorEpoch>(reader));
  DCE_ASSIGN(plan_body, get_optional<EvolutionPlan>(reader));
  DCE_ASSIGN(context, get_optional<ValidationContext>(reader));
  DCE_ASSIGN(event, get_value<LifecycleEvent>(reader));
  DCE_ASSIGN(transition, get_value<TransitionContext>(reader));
  DCE_ASSIGN(target_wave, get_value<CohortWave>(reader));
  DCE_ASSIGN(text, reader.string(kMaxTextBytes));
  return AdminRequest{.op = op,
                      .request_id = request_id,
                      .plan = plan,
                      .plan_generation = plan_generation,
                      .epoch = epoch,
                      .plan_body = plan_body,
                      .context = context,
                      .event = event,
                      .transition = transition,
                      .target_wave = target_wave,
                      .text = text};
}

Status encode_admin_response_body(CanonicalWriter& writer, const AdminResponse& response) {
  DCE_TRY(writer.put_u64(response.request_id));
  DCE_TRY(put_u16_enum(writer, response.status, kMinErrorCode, kMaxErrorCode, "ErrorCode"));
  DCE_TRY(put_text(writer, response.text));
  DCE_TRY(put_optional<PlanState>(writer, response.state));
  DCE_TRY(put_optional<ValidationReport>(writer, response.report));
  DCE_TRY(put_optional<RolloutAction>(writer, response.action));
  DCE_TRY(put_optional<RollbackEligibility>(writer, response.rollback));
  DCE_TRY(put_optional<ReceiptOutcome>(writer, response.receipt));
  DCE_TRY(put_optional<ReconciliationOutcome>(writer, response.reconciliation));
  DCE_TRY(writer.put_u64(response.sites));
  DCE_TRY(writer.put_u64(response.sites_complete));
  DCE_TRY(writer.put_u64(response.receipts));
  DCE_TRY(writer.put_u64(response.checkpoints));
  DCE_TRY(writer.put_u64(response.reconciliations));
  DCE_TRY(writer.put_u64(response.refusals));
  DCE_TRY(put_value<CoordinatorEpoch>(writer, response.epoch));
  DCE_TRY(put_value<PlanGeneration>(writer, response.plan_generation));
  DCE_TRY(put_value<Digest256>(writer, response.plan_digest));
  return Status{};
}

Result<AdminResponse> decode_admin_response_body(CanonicalReader& reader) {
  DCE_ASSIGN(request_id, reader.u64());
  DCE_ASSIGN(status, get_u16_enum<ErrorCode>(reader, kMinErrorCode, kMaxErrorCode, "ErrorCode"));
  DCE_ASSIGN(text, reader.string(kMaxTextBytes));
  DCE_ASSIGN(state, get_optional<PlanState>(reader));
  DCE_ASSIGN(report, get_optional<ValidationReport>(reader));
  DCE_ASSIGN(action, get_optional<RolloutAction>(reader));
  DCE_ASSIGN(rollback, get_optional<RollbackEligibility>(reader));
  DCE_ASSIGN(receipt, get_optional<ReceiptOutcome>(reader));
  DCE_ASSIGN(reconciliation, get_optional<ReconciliationOutcome>(reader));
  DCE_ASSIGN(sites, reader.u64());
  DCE_ASSIGN(sites_complete, reader.u64());
  DCE_ASSIGN(receipts, reader.u64());
  DCE_ASSIGN(checkpoints, reader.u64());
  DCE_ASSIGN(reconciliations, reader.u64());
  DCE_ASSIGN(refusals, reader.u64());
  DCE_ASSIGN(epoch, get_value<CoordinatorEpoch>(reader));
  DCE_ASSIGN(plan_generation, get_value<PlanGeneration>(reader));
  DCE_ASSIGN(plan_digest, get_value<Digest256>(reader));
  return AdminResponse{.request_id = request_id,
                       .status = status,
                       .text = text,
                       .state = state,
                       .report = report,
                       .action = action,
                       .rollback = rollback,
                       .receipt = receipt,
                       .reconciliation = reconciliation,
                       .sites = sites,
                       .sites_complete = sites_complete,
                       .receipts = receipts,
                       .checkpoints = checkpoints,
                       .reconciliations = reconciliations,
                       .refusals = refusals,
                       .epoch = epoch,
                       .plan_generation = plan_generation,
                       .plan_digest = plan_digest};
}

// A body is copied out of the writer once, at the size it actually reached.
[[nodiscard]] std::vector<std::byte> take_bytes(const CanonicalWriter& writer) {
  const std::vector<std::byte>& bytes = writer.bytes();
  return std::vector<std::byte>(bytes.begin(), bytes.end());
}

// Each decoder finishes by rejecting trailing bytes, so a body is exactly one
// canonical value and never a prefix of one.
template <class T, class Decode>
[[nodiscard]] Result<T> decode_exactly(std::span<const std::byte> body, Decode decode) {
  CanonicalReader reader(body);
  DCE_ASSIGN(value, decode(reader));
  DCE_TRY(reader.expect_end());
  return std::move(value);
}

}  // namespace

const char* to_string(MessageKind kind) noexcept {
  switch (kind) {
    case MessageKind::hello: return "hello";
    case MessageKind::hello_ack: return "hello_ack";
    case MessageKind::stage_offer: return "stage_offer";
    case MessageKind::stage_result: return "stage_result";
    case MessageKind::reconcile_request: return "reconcile_request";
    case MessageKind::reconcile_result: return "reconcile_result";
    case MessageKind::pause: return "pause";
    case MessageKind::resume: return "resume";
    case MessageKind::stop: return "stop";
    case MessageKind::failure: return "failure";
  }
  return "unknown";
}

const char* to_string(AdminOp op) noexcept {
  switch (op) {
    case AdminOp::status: return "status";
    case AdminOp::submit_plan: return "submit_plan";
    case AdminOp::validate: return "validate";
    case AdminOp::event: return "event";
    case AdminOp::observe: return "observe";
    case AdminOp::next_action: return "next_action";
    case AdminOp::rollback_eligibility: return "rollback_eligibility";
    case AdminOp::stop: return "stop";
  }
  return "unknown";
}

Status write_frame(platform::Socket& socket, std::uint16_t kind,
                   std::span<const std::byte> payload) {
  return guarded([&]() -> Status {
    if (payload.size() > kMaxFrameBytes) {
      return Error{ErrorCode::limit_exceeded, "the frame payload exceeds the documented bound"};
    }
    std::array<std::byte, kFrameHeaderBytes> header{};
    store_le(header, kMagicOffset, kFrameMagic, kMagicBytes);
    store_le(header, kVersionOffset, static_cast<std::uint32_t>(kProtocolVersion), kVersionBytes);
    store_le(header, kKindOffset, static_cast<std::uint32_t>(kind), kKindBytes);
    store_le(header, kLengthOffset, static_cast<std::uint32_t>(payload.size()), kLengthBytes);
    store_le(header, kCrcOffset, crc32(payload), kCrcBytes);

    // One contiguous buffer, one send: a reader never sees half a frame from a
    // caller that used this function.
    std::vector<std::byte> frame;
    frame.reserve(kFrameHeaderBytes + payload.size());
    frame.insert(frame.end(), header.begin(), header.end());
    frame.insert(frame.end(), payload.begin(), payload.end());
    return socket.send_all(frame);
  });
}

Result<std::vector<std::byte>> read_frame(platform::Socket& socket, std::uint16_t& kind) {
  return guarded([&]() -> Result<std::vector<std::byte>> {
    std::array<std::byte, kFrameHeaderBytes> header{};
    DCE_TRY(socket.recv_exact(header));
    DCE_ASSIGN(parsed, parse_header(header));
    // The length is already bounded, so this allocation can never be larger
    // than a frame the protocol accepts.
    std::vector<std::byte> payload(static_cast<std::size_t>(parsed.length));
    if (!payload.empty()) {
      DCE_TRY(socket.recv_exact(payload));
    }
    if (crc32(payload) != load_le(header, kCrcOffset, kCrcBytes)) {
      return Error{ErrorCode::corruption, "the frame failed its CRC check"};
    }
    kind = parsed.kind;
    return payload;
  });
}

Status write_message(platform::Socket& socket, const Message& message) {
  return guarded([&]() -> Status {
    DCE_ASSIGN(body, encode_message(message));
    return write_frame(socket, static_cast<std::uint16_t>(message.kind), body);
  });
}

Result<Message> read_message(platform::Socket& socket) {
  return guarded([&]() -> Result<Message> {
    std::uint16_t kind = 0;
    DCE_ASSIGN(payload, read_frame(socket, kind));
    if (kind == kAdminFrameKind) {
      return Error{ErrorCode::malformed, "an administrative frame is not a site message"};
    }
    return decode_message(payload);
  });
}

Status write_admin_request(platform::Socket& socket, const AdminRequest& request) {
  return guarded([&]() -> Status {
    DCE_ASSIGN(body, encode_admin_request(request));
    return write_frame(socket, kAdminFrameKind, body);
  });
}

Result<AdminRequest> read_admin_request(platform::Socket& socket) {
  return guarded([&]() -> Result<AdminRequest> {
    std::uint16_t kind = 0;
    DCE_ASSIGN(payload, read_frame(socket, kind));
    if (kind != kAdminFrameKind) {
      return Error{ErrorCode::malformed, "a site message is not an administrative request"};
    }
    return decode_admin_request(payload);
  });
}

Status write_admin_response(platform::Socket& socket, const AdminResponse& response) {
  return guarded([&]() -> Status {
    DCE_ASSIGN(body, encode_admin_response(response));
    return write_frame(socket, kAdminFrameKind, body);
  });
}

Result<AdminResponse> read_admin_response(platform::Socket& socket) {
  return guarded([&]() -> Result<AdminResponse> {
    std::uint16_t kind = 0;
    DCE_ASSIGN(payload, read_frame(socket, kind));
    if (kind != kAdminFrameKind) {
      return Error{ErrorCode::malformed, "a site message is not an administrative response"};
    }
    return decode_admin_response(payload);
  });
}

Result<std::vector<std::byte>> encode_message(const Message& message) {
  return guarded([&]() -> Result<std::vector<std::byte>> {
    CanonicalWriter writer(kMaxFrameBytes);
    DCE_TRY(encode_message_body(writer, message));
    return take_bytes(writer);
  });
}

Result<Message> decode_message(std::span<const std::byte> body) {
  return guarded([&]() -> Result<Message> {
    return decode_exactly<Message>(body, [](CanonicalReader& reader) {
      return decode_message_body(reader);
    });
  });
}

Result<std::vector<std::byte>> encode_admin_request(const AdminRequest& request) {
  return guarded([&]() -> Result<std::vector<std::byte>> {
    CanonicalWriter writer(kMaxFrameBytes);
    DCE_TRY(encode_admin_request_body(writer, request));
    return take_bytes(writer);
  });
}

Result<AdminRequest> decode_admin_request(std::span<const std::byte> body) {
  return guarded([&]() -> Result<AdminRequest> {
    return decode_exactly<AdminRequest>(body, [](CanonicalReader& reader) {
      return decode_admin_request_body(reader);
    });
  });
}

Result<std::vector<std::byte>> encode_admin_response(const AdminResponse& response) {
  return guarded([&]() -> Result<std::vector<std::byte>> {
    CanonicalWriter writer(kMaxFrameBytes);
    DCE_TRY(encode_admin_response_body(writer, response));
    return take_bytes(writer);
  });
}

Result<AdminResponse> decode_admin_response(std::span<const std::byte> body) {
  return guarded([&]() -> Result<AdminResponse> {
    return decode_exactly<AdminResponse>(body, [](CanonicalReader& reader) {
      return decode_admin_response_body(reader);
    });
  });
}

}  // namespace dce::wire
