// Evidence and gates.
//
// Evidence is provenance-bearing: it records who asserted what, about which
// generation, and with which content digest. A gate is the only thing allowed
// to turn evidence into permission, and an unevaluated gate is never treated
// as passing.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "dce/capability.hpp"
#include "dce/digest.hpp"
#include "dce/ids.hpp"
#include "dce/status.hpp"

namespace dce {

enum class EvidenceKind : std::uint8_t {
  compatibility_certification,
  migration_drill,
  rollback_drill,
  readiness_gate,
  health_observation,
  dependency_attestation,
  capability_dependency_proof,
  authority_grant,
  exception_grant,
  provenance_statement,
};

[[nodiscard]] const char* to_string(EvidenceKind kind) noexcept;
[[nodiscard]] Result<EvidenceKind> evidence_kind_from_string(std::string_view text);

struct EvidenceRecord {
  EvidenceId id;
  EvidenceKind kind{EvidenceKind::provenance_statement};
  std::string claim;
  Digest256 subject_digest;
  ActorId producer;
  SiteGeneration generation;
  // The coordinator epoch the evidence was observed in. Zero means the record is
  // not epoch-scoped, which is the honest description of a static property of a
  // version pair such as a compatibility certification. An epoch-scoped record
  // that predates the plan revision it accompanies is stale.
  CoordinatorEpoch observed_epoch;
  std::optional<BoundaryRefId> external_reference;

  auto operator<=>(const EvidenceRecord&) const = default;
};

class EvidenceSet {
 public:
  static constexpr std::size_t kMaxEvidence = 4096;

  // Re-adding identical evidence is idempotent; a repeated identity with
  // different content is a conflict, because two different claims cannot both
  // be the same piece of evidence.
  [[nodiscard]] Status add(EvidenceRecord record);

  [[nodiscard]] const EvidenceRecord* find(const EvidenceId& id) const;
  [[nodiscard]] std::vector<const EvidenceRecord*> of_kind(EvidenceKind kind) const;

  void canonicalize();
  [[nodiscard]] const std::vector<EvidenceRecord>& entries() const noexcept { return records_; }
  [[nodiscard]] std::size_t size() const noexcept { return records_.size(); }

  // Fails only when the canonical encoding exceeds the documented bound; it
  // never returns a placeholder digest for content it could not encode.
  [[nodiscard]] Result<Digest256> digest() const;

 private:
  std::vector<EvidenceRecord> records_;
};

enum class GateKind : std::uint8_t {
  precondition,
  readiness,
  compatibility,
  health,
  policy,
  dependency,
  capability_removal,
  deprecation,
  exception,
  provenance,
};

[[nodiscard]] const char* to_string(GateKind kind) noexcept;

enum class GateScopeKind : std::uint8_t { plan, cohort, site, step };

[[nodiscard]] const char* to_string(GateScopeKind scope) noexcept;

struct Gate {
  GateId id;
  GateKind kind{GateKind::precondition};
  GateScopeKind scope{GateScopeKind::plan};
  std::string description;
  std::vector<EvidenceKind> required_evidence;
  std::vector<CapabilityId> required_capabilities;
  bool blocking{true};

  auto operator<=>(const Gate&) const = default;
};

enum class GateOutcome : std::uint8_t {
  passed,
  failed,
  unevaluated,    // nothing has assessed the gate yet
  indeterminate,  // assessment was attempted and the truth is not knowable
};

[[nodiscard]] const char* to_string(GateOutcome outcome) noexcept;

struct GateEvaluation {
  GateId gate;
  GateOutcome outcome{GateOutcome::unevaluated};
  std::string explanation;
  std::vector<EvidenceId> satisfied_by;

  auto operator<=>(const GateEvaluation&) const = default;
};

// A bounded collection of gate declarations with canonical lookup order.
class GateSet {
 public:
  static constexpr std::size_t kMaxGates = 2048;

  [[nodiscard]] Status add(Gate gate);
  [[nodiscard]] const Gate* find(const GateId& id) const;
  void canonicalize();
  [[nodiscard]] const std::vector<Gate>& entries() const noexcept { return gates_; }
  [[nodiscard]] std::size_t size() const noexcept { return gates_.size(); }

 private:
  std::vector<Gate> gates_;
};

}  // namespace dce
