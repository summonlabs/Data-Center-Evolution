// Canonical codec for every domain type that is hashed, persisted or sent.
//
// One place owns the byte layout: the plan digest, the on-disk record payload
// and the wire message all go through here, so a change to a field order is a
// single, reviewable change with a format version consequence.
//
// Decoding is total and hostile-input safe: every length and count is bounded
// by dce/limits.hpp before anything is allocated, and a payload that does not
// decode exactly is rejected rather than partially accepted.
//
// Two fields are derived rather than stored and are therefore NOT written by
// the codec: EvolutionPlan::digest and SiteMembershipSnapshot::digest. Both are
// recomputed by the caller from the decoded value, which keeps the digest a
// pure function of the content and makes it impossible for a stored digest to
// disagree with the bytes it claims to summarise.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "dce/authority.hpp"
#include "dce/canonical.hpp"
#include "dce/capability.hpp"
#include "dce/compatibility.hpp"
#include "dce/evidence.hpp"
#include "dce/ids.hpp"
#include "dce/lifecycle.hpp"
#include "dce/limits.hpp"
#include "dce/plan.hpp"
#include "dce/reconcile.hpp"
#include "dce/rollout.hpp"
#include "dce/status.hpp"
#include "dce/text.hpp"
#include "dce/version.hpp"

namespace dce::codec {

// The primary template is deliberately undefined: using it without an explicit
// specialisation is a compile-time error rather than a silent byte copy.
template <class T>
struct Codec;

template <class T>
[[nodiscard]] Status put(CanonicalWriter& writer, const T& value) {
  return Codec<T>::put(writer, value);
}

template <class T>
[[nodiscard]] Result<T> get(CanonicalReader& reader) {
  return Codec<T>::get(reader);
}

template <class T>
[[nodiscard]] Status put_list(CanonicalWriter& writer, const std::vector<T>& items,
                              std::size_t max_items) {
  if (items.size() > max_items) {
    return Error{ErrorCode::limit_exceeded, "collection exceeds its documented bound"};
  }
  DCE_TRY(writer.put_count(items.size()));
  for (const T& item : items) {
    DCE_TRY(put<T>(writer, item));
  }
  return Status{};
}

template <class T>
[[nodiscard]] Result<std::vector<T>> get_list(CanonicalReader& reader, std::size_t max_items) {
  DCE_ASSIGN(count, reader.count(max_items));
  std::vector<T> items;
  items.reserve(count);
  for (std::size_t index = 0; index < count; ++index) {
    DCE_ASSIGN(item, get<T>(reader));
    items.push_back(std::move(item));
  }
  return items;
}

// Fixed-width enums are encoded as one byte; anything outside the declared
// range is malformed rather than reinterpreted.
#define DCE_ENUM_CODEC(Type, MaxValue)                                              \
  template <>                                                                       \
  struct Codec<Type> {                                                              \
    static Status put(CanonicalWriter& writer, Type value) {                        \
      return writer.put_u8(static_cast<std::uint8_t>(value));                       \
    }                                                                               \
    static Result<Type> get(CanonicalReader& reader) {                              \
      DCE_ASSIGN(raw, reader.u8());                                                 \
      if (raw > static_cast<std::uint8_t>(MaxValue)) {                              \
        return Error{ErrorCode::malformed, "enum value out of range: " #Type};      \
      }                                                                             \
      return static_cast<Type>(raw);                                                \
    }                                                                               \
  }

DCE_ENUM_CODEC(EvidenceKind, 9);
DCE_ENUM_CODEC(GateKind, 9);
DCE_ENUM_CODEC(GateScopeKind, 3);
DCE_ENUM_CODEC(GateOutcome, 3);
DCE_ENUM_CODEC(InteropOutcome, 2);
DCE_ENUM_CODEC(TransitionKind, 2);
DCE_ENUM_CODEC(Irreversibility, 1);
DCE_ENUM_CODEC(RolloutStrategy, 2);
DCE_ENUM_CODEC(PlanState, 12);
DCE_ENUM_CODEC(LifecycleEvent, 13);
DCE_ENUM_CODEC(ReconciliationAction, 4);
DCE_ENUM_CODEC(AuthorityVerdict, 6);
DCE_ENUM_CODEC(ReceiptVerdict, 7);
DCE_ENUM_CODEC(RolloutActionKind, 6);

#undef DCE_ENUM_CODEC

template <class Tag>
struct Codec<BasicId<Tag>> {
  static Status put(CanonicalWriter& writer, const BasicId<Tag>& value) {
    return writer.put_string(value.view());
  }
  // An identity that was never assigned is written as no text and read back as
  // no identity. Presence is a semantic question and the model layer answers it:
  // structural_check, validate_plan and the reference checks all reject an
  // absent identity wherever one is required. A facility that has not published
  // a policy generation is the case that makes this necessary - there is no
  // honest identity to invent for it, and refusing to decode the record would
  // lose every other field it carries.
  static Result<BasicId<Tag>> get(CanonicalReader& reader) {
    DCE_ASSIGN(text, reader.string(dce::text::kMaxIdentifierLength));
    if (text.empty()) {
      return BasicId<Tag>{};
    }
    return BasicId<Tag>::parse(text);
  }
};

template <class Tag, class Rep>
struct Codec<BasicCounter<Tag, Rep>> {
  static Status put(CanonicalWriter& writer, const BasicCounter<Tag, Rep>& value) {
    return writer.put_u64(static_cast<std::uint64_t>(value.value()));
  }
  static Result<BasicCounter<Tag, Rep>> get(CanonicalReader& reader) {
    DCE_ASSIGN(raw, reader.u64());
    const std::optional<Rep> narrowed = checked_narrow<Rep>(raw);
    if (!narrowed.has_value()) {
      return Error{ErrorCode::out_of_range, "counter does not fit its declared width"};
    }
    return BasicCounter<Tag, Rep>::from_value(*narrowed);
  }
};

template <>
struct Codec<bool> {
  static Status put(CanonicalWriter& writer, bool value) { return writer.put_bool(value); }
  static Result<bool> get(CanonicalReader& reader) { return reader.boolean(); }
};

template <>
struct Codec<std::string> {
  static Status put(CanonicalWriter& writer, const std::string& value) {
    return writer.put_string(value);
  }
  static Result<std::string> get(CanonicalReader& reader) {
    return reader.string(kMaxTextBytes);
  }
};

template <>
struct Codec<Version> {
  static Status put(CanonicalWriter& writer, const Version& value);
  static Result<Version> get(CanonicalReader& reader);
};

template <>
struct Codec<VersionRange> {
  static Status put(CanonicalWriter& writer, const VersionRange& value);
  static Result<VersionRange> get(CanonicalReader& reader);
};

template <>
struct Codec<Digest256> {
  static Status put(CanonicalWriter& writer, const Digest256& value);
  static Result<Digest256> get(CanonicalReader& reader);
};

template <>
struct Codec<CapabilitySet> {
  static Status put(CanonicalWriter& writer, const CapabilitySet& value);
  static Result<CapabilitySet> get(CanonicalReader& reader);
};

template <>
struct Codec<ComponentCapabilities> {
  static Status put(CanonicalWriter& writer, const ComponentCapabilities& value);
  static Result<ComponentCapabilities> get(CanonicalReader& reader);
};

template <>
struct Codec<CapabilityMatrix> {
  static Status put(CanonicalWriter& writer, const CapabilityMatrix& value);
  static Result<CapabilityMatrix> get(CanonicalReader& reader);
};

template <>
struct Codec<PolicyGeneration> {
  static Status put(CanonicalWriter& writer, const PolicyGeneration& value);
  static Result<PolicyGeneration> get(CanonicalReader& reader);
};

template <>
struct Codec<PolicyRequirement> {
  static Status put(CanonicalWriter& writer, const PolicyRequirement& value);
  static Result<PolicyRequirement> get(CanonicalReader& reader);
};

template <>
struct Codec<DependencyCompatibilityRef> {
  static Status put(CanonicalWriter& writer, const DependencyCompatibilityRef& value);
  static Result<DependencyCompatibilityRef> get(CanonicalReader& reader);
};

template <>
struct Codec<CapabilityDependencyProof> {
  static Status put(CanonicalWriter& writer, const CapabilityDependencyProof& value);
  static Result<CapabilityDependencyProof> get(CanonicalReader& reader);
};

template <>
struct Codec<EvidenceRecord> {
  static Status put(CanonicalWriter& writer, const EvidenceRecord& value);
  static Result<EvidenceRecord> get(CanonicalReader& reader);
};

template <>
struct Codec<EvidenceSet> {
  static Status put(CanonicalWriter& writer, const EvidenceSet& value);
  static Result<EvidenceSet> get(CanonicalReader& reader);
};

template <>
struct Codec<Gate> {
  static Status put(CanonicalWriter& writer, const Gate& value);
  static Result<Gate> get(CanonicalReader& reader);
};

template <>
struct Codec<GateSet> {
  static Status put(CanonicalWriter& writer, const GateSet& value);
  static Result<GateSet> get(CanonicalReader& reader);
};

template <>
struct Codec<ComponentVersion> {
  static Status put(CanonicalWriter& writer, const ComponentVersion& value);
  static Result<ComponentVersion> get(CanonicalReader& reader);
};

template <>
struct Codec<CompatEdge> {
  static Status put(CanonicalWriter& writer, const CompatEdge& value);
  static Result<CompatEdge> get(CanonicalReader& reader);
};

template <>
struct Codec<CompatibilityGraph> {
  static Status put(CanonicalWriter& writer, const CompatibilityGraph& value);
  static Result<CompatibilityGraph> get(CanonicalReader& reader);
};

template <>
struct Codec<MigrationStep> {
  static Status put(CanonicalWriter& writer, const MigrationStep& value);
  static Result<MigrationStep> get(CanonicalReader& reader);
};

template <>
struct Codec<RolloutCohort> {
  static Status put(CanonicalWriter& writer, const RolloutCohort& value);
  static Result<RolloutCohort> get(CanonicalReader& reader);
};

template <>
struct Codec<SiteRecord> {
  static Status put(CanonicalWriter& writer, const SiteRecord& value);
  static Result<SiteRecord> get(CanonicalReader& reader);
};

template <>
struct Codec<SiteMembershipSnapshot> {
  static Status put(CanonicalWriter& writer, const SiteMembershipSnapshot& value);
  static Result<SiteMembershipSnapshot> get(CanonicalReader& reader);
};

template <>
struct Codec<Precondition> {
  static Status put(CanonicalWriter& writer, const Precondition& value);
  static Result<Precondition> get(CanonicalReader& reader);
};

template <>
struct Codec<PointOfNoReturn> {
  static Status put(CanonicalWriter& writer, const PointOfNoReturn& value);
  static Result<PointOfNoReturn> get(CanonicalReader& reader);
};

template <>
struct Codec<RollbackPolicy> {
  static Status put(CanonicalWriter& writer, const RollbackPolicy& value);
  static Result<RollbackPolicy> get(CanonicalReader& reader);
};

template <>
struct Codec<DeprecationGate> {
  static Status put(CanonicalWriter& writer, const DeprecationGate& value);
  static Result<DeprecationGate> get(CanonicalReader& reader);
};

template <>
struct Codec<ExceptionGrant> {
  static Status put(CanonicalWriter& writer, const ExceptionGrant& value);
  static Result<ExceptionGrant> get(CanonicalReader& reader);
};

template <>
struct Codec<PlanIdentity> {
  static Status put(CanonicalWriter& writer, const PlanIdentity& value);
  static Result<PlanIdentity> get(CanonicalReader& reader);
};

template <>
struct Codec<EvolutionPlan> {
  static Status put(CanonicalWriter& writer, const EvolutionPlan& value);
  static Result<EvolutionPlan> get(CanonicalReader& reader);
};

template <>
struct Codec<AuthorityToken> {
  static Status put(CanonicalWriter& writer, const AuthorityToken& value);
  static Result<AuthorityToken> get(CanonicalReader& reader);
};

template <>
struct Codec<MigrationReceipt> {
  static Status put(CanonicalWriter& writer, const MigrationReceipt& value);
  static Result<MigrationReceipt> get(CanonicalReader& reader);
};

template <>
struct Codec<StageCheckpoint> {
  static Status put(CanonicalWriter& writer, const StageCheckpoint& value);
  static Result<StageCheckpoint> get(CanonicalReader& reader);
};

template <>
struct Codec<RollbackMarker> {
  static Status put(CanonicalWriter& writer, const RollbackMarker& value);
  static Result<RollbackMarker> get(CanonicalReader& reader);
};

template <>
struct Codec<SiteProgress> {
  static Status put(CanonicalWriter& writer, const SiteProgress& value);
  static Result<SiteProgress> get(CanonicalReader& reader);
};

template <>
struct Codec<SiteStageReport> {
  static Status put(CanonicalWriter& writer, const SiteStageReport& value);
  static Result<SiteStageReport> get(CanonicalReader& reader);
};

}  // namespace dce::codec
