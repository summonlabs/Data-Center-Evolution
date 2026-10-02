// Control-plane wiring between the evolution coordinator and site agents.
//
// The frame is length-prefixed and CRC-checked, every field is bounded, and a
// frame that is not exactly one canonical message is rejected. The wire format
// is owned here rather than by the domain codec because it carries transport
// concerns - a protocol version and an integrity check - that the durable
// record format does not.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "dce/authority.hpp"
#include "dce/ids.hpp"
#include "dce/lifecycle.hpp"
#include "dce/limits.hpp"
#include "dce/platform.hpp"
#include "dce/plan.hpp"
#include "dce/reconcile.hpp"
#include "dce/status.hpp"
#include "dce/validate.hpp"

namespace dce::wire {

inline constexpr std::uint16_t kProtocolVersion = 1;
inline constexpr std::uint32_t kFrameMagic = 0x31454344u;  // "DCE1" on the wire
inline constexpr std::size_t kFrameHeaderBytes = 16;
inline constexpr std::size_t kMaxFrameBytes = 8u * 1024u * 1024u;

enum class MessageKind : std::uint16_t {
  hello = 1,           // site -> coordinator: who I am and what I run
  hello_ack = 2,       // coordinator -> site: accepted, here is the epoch
  stage_offer = 3,     // coordinator -> site: advance exactly one stage
  stage_result = 4,    // site -> coordinator: the receipt, or the refusal
  reconcile_request = 5,
  reconcile_result = 6,
  pause = 7,
  resume = 8,
  stop = 9,
  failure = 10,
};

[[nodiscard]] const char* to_string(MessageKind kind) noexcept;

enum class AdminOp : std::uint16_t {
  status = 1,
  submit_plan = 2,
  validate = 3,
  event = 4,
  observe = 5,
  next_action = 6,
  rollback_eligibility = 7,
  stop = 8,
};

[[nodiscard]] const char* to_string(AdminOp op) noexcept;

// One canonical message. Optional bodies are explicit so that "absent" can
// never be confused with "present but empty".
struct Message {
  MessageKind kind{MessageKind::failure};
  std::uint64_t request_id{0};
  ErrorCode status{ErrorCode::ok};
  std::string text;
  CoordinatorEpoch epoch;
  PlanGeneration plan_generation;
  EvolutionPlanId plan;
  Digest256 plan_digest;
  SiteId site;
  CohortId cohort;
  CohortWave wave;
  StageOrdinal stage;
  // Carried with a stage offer so the receiving site can build a receipt for
  // exactly the step it was authorised to take, without holding the plan.
  StepId step;
  Version from_version;
  Version to_version;
  SiteGeneration site_generation;
  Digest256 stage_digest;
  std::optional<SiteRecord> profile;
  std::optional<AuthorityToken> token;
  std::optional<MigrationReceipt> receipt;
  std::optional<SiteStageReport> report;
  std::optional<SiteProgress> progress;
  std::optional<StageCheckpoint> checkpoint;
};

struct AdminRequest {
  AdminOp op{AdminOp::status};
  std::uint64_t request_id{0};
  EvolutionPlanId plan;
  PlanGeneration plan_generation;
  CoordinatorEpoch epoch;
  std::optional<EvolutionPlan> plan_body;
  std::optional<ValidationContext> context;
  LifecycleEvent event{LifecycleEvent::validate};
  TransitionContext transition;
  CohortWave target_wave;
  std::string text;
};

struct AdminResponse {
  std::uint64_t request_id{0};
  ErrorCode status{ErrorCode::ok};
  std::string text;
  std::optional<PlanState> state;
  std::optional<ValidationReport> report;
  std::optional<RolloutAction> action;
  std::optional<RollbackEligibility> rollback;
  std::optional<ReceiptOutcome> receipt;
  std::optional<ReconciliationOutcome> reconciliation;
  std::uint64_t sites{0};
  std::uint64_t sites_complete{0};
  std::uint64_t receipts{0};
  std::uint64_t checkpoints{0};
  std::uint64_t reconciliations{0};
  std::uint64_t refusals{0};
  CoordinatorEpoch epoch;
  PlanGeneration plan_generation;
  Digest256 plan_digest;
};

// The frame kind reserved for administrative traffic. A connection's first
// frame decides its role: this kind is an AdminRequest, anything else is a
// Message. The header is identical either way, so one framing layer serves
// both and neither can be confused for the other.
inline constexpr std::uint16_t kAdminFrameKind = 0x00ffu;

// Writes one frame. Fails rather than truncating anything.
[[nodiscard]] Status write_frame(platform::Socket& socket, std::uint16_t kind,
                                 std::span<const std::byte> payload);

// Reads exactly one frame and reports its kind. A frame that is short,
// over-long, has the wrong magic or version, or fails its CRC is an error.
[[nodiscard]] Result<std::vector<std::byte>> read_frame(platform::Socket& socket,
                                                        std::uint16_t& kind);

// Writes one framed message. Fails rather than truncating anything.
[[nodiscard]] Status write_message(platform::Socket& socket, const Message& message);

// Reads exactly one framed message. A frame that is short, over-long, has the
// wrong magic or version, or fails its CRC is an error; a peer that closed the
// connection cleanly is reported as ErrorCode::closed so the caller can
// distinguish a partition from a protocol defect.
[[nodiscard]] Result<Message> read_message(platform::Socket& socket);

[[nodiscard]] Status write_admin_request(platform::Socket& socket, const AdminRequest& request);
[[nodiscard]] Result<AdminRequest> read_admin_request(platform::Socket& socket);
[[nodiscard]] Status write_admin_response(platform::Socket& socket, const AdminResponse& response);
[[nodiscard]] Result<AdminResponse> read_admin_response(platform::Socket& socket);

// Encodes and decodes a message body without a socket, so that the format can
// be attacked directly by tests.
[[nodiscard]] Result<std::vector<std::byte>> encode_message(const Message& message);
[[nodiscard]] Result<Message> decode_message(std::span<const std::byte> body);

[[nodiscard]] Result<std::vector<std::byte>> encode_admin_request(const AdminRequest& request);
[[nodiscard]] Result<AdminRequest> decode_admin_request(std::span<const std::byte> body);
[[nodiscard]] Result<std::vector<std::byte>> encode_admin_response(const AdminResponse& response);
[[nodiscard]] Result<AdminResponse> decode_admin_response(std::span<const std::byte> body);

}  // namespace dce::wire
