#include "dce/evidence.hpp"

#include <algorithm>
#include <array>

#include "dce/codec.hpp"
#include "dce/text.hpp"

namespace dce {
namespace {

struct KindName {
  EvidenceKind kind;
  std::string_view name;
};

constexpr std::array<KindName, 10> kEvidenceKindNames = {{
    {EvidenceKind::compatibility_certification, "compatibility_certification"},
    {EvidenceKind::migration_drill, "migration_drill"},
    {EvidenceKind::rollback_drill, "rollback_drill"},
    {EvidenceKind::readiness_gate, "readiness_gate"},
    {EvidenceKind::health_observation, "health_observation"},
    {EvidenceKind::dependency_attestation, "dependency_attestation"},
    {EvidenceKind::capability_dependency_proof, "capability_dependency_proof"},
    {EvidenceKind::authority_grant, "authority_grant"},
    {EvidenceKind::exception_grant, "exception_grant"},
    {EvidenceKind::provenance_statement, "provenance_statement"},
}};

struct GateKindName {
  GateKind kind;
  std::string_view name;
};

constexpr std::array<GateKindName, 10> kGateKindNames = {{
    {GateKind::precondition, "precondition"},
    {GateKind::readiness, "readiness"},
    {GateKind::compatibility, "compatibility"},
    {GateKind::health, "health"},
    {GateKind::policy, "policy"},
    {GateKind::dependency, "dependency"},
    {GateKind::capability_removal, "capability_removal"},
    {GateKind::deprecation, "deprecation"},
    {GateKind::exception, "exception"},
    {GateKind::provenance, "provenance"},
}};

}  // namespace

const char* to_string(EvidenceKind kind) noexcept {
  for (const KindName& entry : kEvidenceKindNames) {
    if (entry.kind == kind) {
      return entry.name.data();
    }
  }
  return "unknown";
}

Result<EvidenceKind> evidence_kind_from_string(std::string_view text_value) {
  for (const KindName& entry : kEvidenceKindNames) {
    if (text::iequals_ascii(entry.name, text_value)) {
      return entry.kind;
    }
  }
  return Error{ErrorCode::invalid_argument,
               "unknown evidence kind: " + text::escape_for_output(text_value)};
}

const char* to_string(GateKind kind) noexcept {
  for (const GateKindName& entry : kGateKindNames) {
    if (entry.kind == kind) {
      return entry.name.data();
    }
  }
  return "unknown";
}

const char* to_string(GateScopeKind scope) noexcept {
  switch (scope) {
    case GateScopeKind::plan: return "plan";
    case GateScopeKind::cohort: return "cohort";
    case GateScopeKind::site: return "site";
    case GateScopeKind::step: return "step";
  }
  return "unknown";
}

const char* to_string(GateOutcome outcome) noexcept {
  switch (outcome) {
    case GateOutcome::passed: return "passed";
    case GateOutcome::failed: return "failed";
    case GateOutcome::unevaluated: return "unevaluated";
    case GateOutcome::indeterminate: return "indeterminate";
  }
  return "unknown";
}

Status EvidenceSet::add(EvidenceRecord record) {
  const auto position =
      std::lower_bound(records_.begin(), records_.end(), record,
                       [](const EvidenceRecord& left, const EvidenceRecord& right) {
                         return left.id < right.id;
                       });
  if (position != records_.end() && position->id == record.id) {
    if (*position == record) {
      return Status{};  // the same evidence recorded twice is the same evidence
    }
    return Error{ErrorCode::conflict,
                 "two different records claim the same evidence identity"};
  }
  if (records_.size() >= kMaxEvidence) {
    return Error{ErrorCode::limit_exceeded, "evidence set exceeds its documented bound"};
  }
  records_.insert(position, std::move(record));
  return Status{};
}

const EvidenceRecord* EvidenceSet::find(const EvidenceId& id) const {
  for (const EvidenceRecord& record : records_) {
    if (record.id == id) {
      return &record;
    }
  }
  return nullptr;
}

std::vector<const EvidenceRecord*> EvidenceSet::of_kind(EvidenceKind kind) const {
  std::vector<const EvidenceRecord*> matches;
  for (const EvidenceRecord& record : records_) {
    if (record.kind == kind) {
      matches.push_back(&record);
    }
  }
  return matches;
}

void EvidenceSet::canonicalize() {
  std::sort(records_.begin(), records_.end(),
            [](const EvidenceRecord& left, const EvidenceRecord& right) { return left.id < right.id; });
}

Result<Digest256> EvidenceSet::digest() const {
  CanonicalWriter writer;
  DCE_TRY(codec::put(writer, *this));
  return writer.digest();
}

Status GateSet::add(Gate gate) {
  const auto position = std::lower_bound(
      gates_.begin(), gates_.end(), gate,
      [](const Gate& left, const Gate& right) { return left.id < right.id; });
  if (position != gates_.end() && position->id == gate.id) {
    if (*position == gate) {
      return Status{};
    }
    return Error{ErrorCode::conflict, "two different gates claim the same gate identity"};
  }
  if (gates_.size() >= kMaxGates) {
    return Error{ErrorCode::limit_exceeded, "gate set exceeds its documented bound"};
  }
  gates_.insert(position, std::move(gate));
  return Status{};
}

const Gate* GateSet::find(const GateId& id) const {
  for (const Gate& gate : gates_) {
    if (gate.id == id) {
      return &gate;
    }
  }
  return nullptr;
}

void GateSet::canonicalize() {
  std::sort(gates_.begin(), gates_.end(),
            [](const Gate& left, const Gate& right) { return left.id < right.id; });
}

}  // namespace dce
