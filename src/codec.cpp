// Canonical codec definitions for every type declared in dce/codec.hpp.
//
// Every type is written and read in the order its fields are declared, one
// field at a time. Nothing derived is written: EvolutionPlan::digest and
// SiteMembershipSnapshot::digest are left zero by decoding and recomputed by
// the caller from the decoded value.
//
// Both directions apply the same documented bound from dce/limits.hpp to every
// collection and every text field, so a value that encodes always decodes, and
// a payload that is short, declares an over-large length or count, or carries
// an out-of-range enum is refused instead of being partially applied. Semantic
// questions (duplicate identities, dangling references, gate outcomes) are not
// decided here: the codec owns the byte layout, the model layer owns meaning.
#include "dce/codec.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace dce::codec {
namespace {

// Text is written through here rather than through Codec<std::string> so that
// the encoder applies the same bound as the decoder: a text field that could
// not be read back is refused instead of being written as a payload no reader
// would accept.
[[nodiscard]] Status put_text(CanonicalWriter& writer, const std::string& value,
                              std::size_t max_bytes) {
  if (value.size() > max_bytes) {
    return Error{ErrorCode::limit_exceeded, "text field exceeds its documented bound"};
  }
  return writer.put_string(value);
}

// std::optional<T>: one presence byte, then the value only when present.
template <class T>
[[nodiscard]] Status put_optional(CanonicalWriter& writer, const std::optional<T>& value) {
  DCE_TRY(writer.put_bool(value.has_value()));
  if (value.has_value()) {
    DCE_TRY(dce::codec::put<T>(writer, *value));
  }
  return Status{};
}

template <class T>
[[nodiscard]] Result<std::optional<T>> get_optional(CanonicalReader& reader) {
  DCE_ASSIGN(present, reader.boolean());
  if (!present) {
    return std::optional<T>{};
  }
  DCE_ASSIGN(value, dce::codec::get<T>(reader));
  return std::optional<T>(std::move(value));
}

}  // namespace

Status Codec<Version>::put(CanonicalWriter& writer, const Version& value) {
  DCE_TRY(writer.put_u32(value.major));
  DCE_TRY(writer.put_u32(value.minor));
  DCE_TRY(writer.put_u32(value.patch));
  return Status{};
}

Result<Version> Codec<Version>::get(CanonicalReader& reader) {
  DCE_ASSIGN(major, reader.u32());
  DCE_ASSIGN(minor, reader.u32());
  DCE_ASSIGN(patch, reader.u32());
  return Version{.major = major, .minor = minor, .patch = patch};
}

Status Codec<VersionRange>::put(CanonicalWriter& writer, const VersionRange& value) {
  DCE_TRY(dce::codec::put<Version>(writer, value.low));
  DCE_TRY(put_optional<Version>(writer, value.high));
  return Status{};
}

Result<VersionRange> Codec<VersionRange>::get(CanonicalReader& reader) {
  DCE_ASSIGN(low, dce::codec::get<Version>(reader));
  DCE_ASSIGN(high, get_optional<Version>(reader));
  return VersionRange{.low = low, .high = high};
}

Status Codec<Digest256>::put(CanonicalWriter& writer, const Digest256& value) {
  return writer.put_digest(value);
}

Result<Digest256> Codec<Digest256>::get(CanonicalReader& reader) { return reader.digest(); }

Status Codec<CapabilitySet>::put(CanonicalWriter& writer, const CapabilitySet& value) {
  return put_list<CapabilityId>(writer, value.entries(), CapabilitySet::kMaxCapabilities);
}

Result<CapabilitySet> Codec<CapabilitySet>::get(CanonicalReader& reader) {
  DCE_ASSIGN(capabilities, get_list<CapabilityId>(reader, CapabilitySet::kMaxCapabilities));
  CapabilitySet value;
  for (const CapabilityId& capability : capabilities) {
    DCE_TRY(value.insert(capability));
  }
  return value;
}

Status Codec<ComponentCapabilities>::put(CanonicalWriter& writer, const ComponentCapabilities& value) {
  DCE_TRY(dce::codec::put<ComponentId>(writer, value.component));
  DCE_TRY(dce::codec::put<Version>(writer, value.version));
  DCE_TRY(dce::codec::put<CapabilitySet>(writer, value.provides));
  DCE_TRY(dce::codec::put<CapabilitySet>(writer, value.requires_capabilities));
  return Status{};
}

Result<ComponentCapabilities> Codec<ComponentCapabilities>::get(CanonicalReader& reader) {
  DCE_ASSIGN(component, dce::codec::get<ComponentId>(reader));
  DCE_ASSIGN(version, dce::codec::get<Version>(reader));
  DCE_ASSIGN(provides, dce::codec::get<CapabilitySet>(reader));
  DCE_ASSIGN(requires_capabilities, dce::codec::get<CapabilitySet>(reader));
  return ComponentCapabilities{.component = component,
                               .version = version,
                               .provides = provides,
                               .requires_capabilities = requires_capabilities};
}

Status Codec<CapabilityMatrix>::put(CanonicalWriter& writer, const CapabilityMatrix& value) {
  return put_list<ComponentCapabilities>(writer, value.entries(), CapabilityMatrix::kMaxEntries);
}

Result<CapabilityMatrix> Codec<CapabilityMatrix>::get(CanonicalReader& reader) {
  DCE_ASSIGN(entries, get_list<ComponentCapabilities>(reader, CapabilityMatrix::kMaxEntries));
  CapabilityMatrix value;
  for (ComponentCapabilities& entry : entries) {
    DCE_TRY(value.add(std::move(entry)));
  }
  return value;
}

Status Codec<PolicyGeneration>::put(CanonicalWriter& writer, const PolicyGeneration& value) {
  DCE_TRY(dce::codec::put<PolicyGenerationId>(writer, value.id));
  DCE_TRY(writer.put_u64(value.ordinal));
  return Status{};
}

Result<PolicyGeneration> Codec<PolicyGeneration>::get(CanonicalReader& reader) {
  DCE_ASSIGN(id, dce::codec::get<PolicyGenerationId>(reader));
  DCE_ASSIGN(ordinal, reader.u64());
  return PolicyGeneration{.id = id, .ordinal = ordinal};
}

Status Codec<PolicyRequirement>::put(CanonicalWriter& writer, const PolicyRequirement& value) {
  DCE_TRY(writer.put_u64(value.minimum_ordinal));
  DCE_TRY(put_optional<PolicyGenerationId>(writer, value.exact));
  return Status{};
}

Result<PolicyRequirement> Codec<PolicyRequirement>::get(CanonicalReader& reader) {
  DCE_ASSIGN(minimum_ordinal, reader.u64());
  DCE_ASSIGN(exact, get_optional<PolicyGenerationId>(reader));
  return PolicyRequirement{.minimum_ordinal = minimum_ordinal, .exact = exact};
}

Status Codec<DependencyCompatibilityRef>::put(CanonicalWriter& writer, const DependencyCompatibilityRef& value) {
  DCE_TRY(dce::codec::put<BoundaryRefId>(writer, value.reference));
  DCE_TRY(put_text(writer, value.boundary, kMaxTextBytes));
  DCE_TRY(dce::codec::put<VersionRange>(writer, value.supported));
  DCE_TRY(dce::codec::put<CapabilitySet>(writer, value.required));
  DCE_TRY(dce::codec::put<Version>(writer, value.observed_version));
  DCE_TRY(dce::codec::put<CapabilitySet>(writer, value.observed_capabilities));
  DCE_TRY(dce::codec::put<SiteGeneration>(writer, value.observed_generation));
  DCE_TRY(dce::codec::put<EvidenceId>(writer, value.provenance));
  DCE_TRY(writer.put_bool(value.observed));
  return Status{};
}

Result<DependencyCompatibilityRef> Codec<DependencyCompatibilityRef>::get(CanonicalReader& reader) {
  DCE_ASSIGN(reference, dce::codec::get<BoundaryRefId>(reader));
  DCE_ASSIGN(boundary, reader.string(kMaxTextBytes));
  DCE_ASSIGN(supported, dce::codec::get<VersionRange>(reader));
  DCE_ASSIGN(required, dce::codec::get<CapabilitySet>(reader));
  DCE_ASSIGN(observed_version, dce::codec::get<Version>(reader));
  DCE_ASSIGN(observed_capabilities, dce::codec::get<CapabilitySet>(reader));
  DCE_ASSIGN(observed_generation, dce::codec::get<SiteGeneration>(reader));
  DCE_ASSIGN(provenance, dce::codec::get<EvidenceId>(reader));
  DCE_ASSIGN(observed, reader.boolean());
  return DependencyCompatibilityRef{.reference = reference,
                                    .boundary = boundary,
                                    .supported = supported,
                                    .required = required,
                                    .observed_version = observed_version,
                                    .observed_capabilities = observed_capabilities,
                                    .observed_generation = observed_generation,
                                    .provenance = provenance,
                                    .observed = observed};
}

Status Codec<CapabilityDependencyProof>::put(CanonicalWriter& writer, const CapabilityDependencyProof& value) {
  DCE_TRY(dce::codec::put<CapabilityId>(writer, value.capability));
  DCE_TRY(put_list<SiteId>(writer, value.remaining_dependents, kMaxSites));
  DCE_TRY(put_list<ObligationId>(writer, value.obligations, kMaxSites));
  DCE_TRY(dce::codec::put<EvidenceId>(writer, value.provenance));
  DCE_TRY(dce::codec::put<CoordinatorEpoch>(writer, value.observed_epoch));
  return Status{};
}

Result<CapabilityDependencyProof> Codec<CapabilityDependencyProof>::get(CanonicalReader& reader) {
  DCE_ASSIGN(capability, dce::codec::get<CapabilityId>(reader));
  DCE_ASSIGN(remaining_dependents, get_list<SiteId>(reader, kMaxSites));
  DCE_ASSIGN(obligations, get_list<ObligationId>(reader, kMaxSites));
  DCE_ASSIGN(provenance, dce::codec::get<EvidenceId>(reader));
  DCE_ASSIGN(observed_epoch, dce::codec::get<CoordinatorEpoch>(reader));
  return CapabilityDependencyProof{.capability = capability,
                                   .remaining_dependents = remaining_dependents,
                                   .obligations = obligations,
                                   .provenance = provenance,
                                   .observed_epoch = observed_epoch};
}

Status Codec<EvidenceRecord>::put(CanonicalWriter& writer, const EvidenceRecord& value) {
  DCE_TRY(dce::codec::put<EvidenceId>(writer, value.id));
  DCE_TRY(dce::codec::put<EvidenceKind>(writer, value.kind));
  // The claim is the one text field with its own, larger bound.
  DCE_TRY(put_text(writer, value.claim, kMaxClaimBytes));
  DCE_TRY(dce::codec::put<Digest256>(writer, value.subject_digest));
  DCE_TRY(dce::codec::put<ActorId>(writer, value.producer));
  DCE_TRY(dce::codec::put<SiteGeneration>(writer, value.generation));
  DCE_TRY(dce::codec::put<CoordinatorEpoch>(writer, value.observed_epoch));
  DCE_TRY(put_optional<BoundaryRefId>(writer, value.external_reference));
  return Status{};
}

Result<EvidenceRecord> Codec<EvidenceRecord>::get(CanonicalReader& reader) {
  DCE_ASSIGN(id, dce::codec::get<EvidenceId>(reader));
  DCE_ASSIGN(kind, dce::codec::get<EvidenceKind>(reader));
  DCE_ASSIGN(claim, reader.string(kMaxClaimBytes));
  DCE_ASSIGN(subject_digest, dce::codec::get<Digest256>(reader));
  DCE_ASSIGN(producer, dce::codec::get<ActorId>(reader));
  DCE_ASSIGN(generation, dce::codec::get<SiteGeneration>(reader));
  DCE_ASSIGN(observed_epoch, dce::codec::get<CoordinatorEpoch>(reader));
  DCE_ASSIGN(external_reference, get_optional<BoundaryRefId>(reader));
  return EvidenceRecord{.id = id,
                        .kind = kind,
                        .claim = claim,
                        .subject_digest = subject_digest,
                        .producer = producer,
                        .generation = generation,
                        .observed_epoch = observed_epoch,
                        .external_reference = external_reference};
}

Status Codec<EvidenceSet>::put(CanonicalWriter& writer, const EvidenceSet& value) {
  return put_list<EvidenceRecord>(writer, value.entries(), EvidenceSet::kMaxEvidence);
}

Result<EvidenceSet> Codec<EvidenceSet>::get(CanonicalReader& reader) {
  DCE_ASSIGN(records, get_list<EvidenceRecord>(reader, EvidenceSet::kMaxEvidence));
  EvidenceSet value;
  for (EvidenceRecord& record : records) {
    DCE_TRY(value.add(std::move(record)));
  }
  return value;
}

Status Codec<Gate>::put(CanonicalWriter& writer, const Gate& value) {
  DCE_TRY(dce::codec::put<GateId>(writer, value.id));
  DCE_TRY(dce::codec::put<GateKind>(writer, value.kind));
  DCE_TRY(dce::codec::put<GateScopeKind>(writer, value.scope));
  DCE_TRY(put_text(writer, value.description, kMaxTextBytes));
  DCE_TRY(put_list<EvidenceKind>(writer, value.required_evidence, kMaxGateEvidence));
  DCE_TRY(put_list<CapabilityId>(writer, value.required_capabilities,
                                 CapabilitySet::kMaxCapabilities));
  DCE_TRY(writer.put_bool(value.blocking));
  return Status{};
}

Result<Gate> Codec<Gate>::get(CanonicalReader& reader) {
  DCE_ASSIGN(id, dce::codec::get<GateId>(reader));
  DCE_ASSIGN(kind, dce::codec::get<GateKind>(reader));
  DCE_ASSIGN(scope, dce::codec::get<GateScopeKind>(reader));
  DCE_ASSIGN(description, reader.string(kMaxTextBytes));
  DCE_ASSIGN(required_evidence, get_list<EvidenceKind>(reader, kMaxGateEvidence));
  DCE_ASSIGN(required_capabilities,
             get_list<CapabilityId>(reader, CapabilitySet::kMaxCapabilities));
  DCE_ASSIGN(blocking, reader.boolean());
  return Gate{.id = id,
              .kind = kind,
              .scope = scope,
              .description = description,
              .required_evidence = required_evidence,
              .required_capabilities = required_capabilities,
              .blocking = blocking};
}

Status Codec<GateSet>::put(CanonicalWriter& writer, const GateSet& value) {
  return put_list<Gate>(writer, value.entries(), GateSet::kMaxGates);
}

Result<GateSet> Codec<GateSet>::get(CanonicalReader& reader) {
  DCE_ASSIGN(gates, get_list<Gate>(reader, GateSet::kMaxGates));
  GateSet value;
  for (Gate& gate : gates) {
    DCE_TRY(value.add(std::move(gate)));
  }
  return value;
}

Status Codec<ComponentVersion>::put(CanonicalWriter& writer, const ComponentVersion& value) {
  DCE_TRY(dce::codec::put<ComponentId>(writer, value.component));
  DCE_TRY(dce::codec::put<Version>(writer, value.version));
  return Status{};
}

Result<ComponentVersion> Codec<ComponentVersion>::get(CanonicalReader& reader) {
  DCE_ASSIGN(component, dce::codec::get<ComponentId>(reader));
  DCE_ASSIGN(version, dce::codec::get<Version>(reader));
  return ComponentVersion{.component = component, .version = version};
}

Status Codec<CompatEdge>::put(CanonicalWriter& writer, const CompatEdge& value) {
  DCE_TRY(dce::codec::put<ComponentId>(writer, value.component));
  DCE_TRY(dce::codec::put<Version>(writer, value.from));
  DCE_TRY(dce::codec::put<Version>(writer, value.to));
  DCE_TRY(dce::codec::put<TransitionKind>(writer, value.kind));
  DCE_TRY(dce::codec::put<CapabilitySet>(writer, value.requires_capabilities));
  DCE_TRY(dce::codec::put<CapabilitySet>(writer, value.provides_capabilities));
  DCE_TRY(writer.put_bool(value.reversible));
  DCE_TRY(dce::codec::put<EvidenceId>(writer, value.provenance));
  DCE_TRY(put_text(writer, value.justification, kMaxTextBytes));
  return Status{};
}

Result<CompatEdge> Codec<CompatEdge>::get(CanonicalReader& reader) {
  DCE_ASSIGN(component, dce::codec::get<ComponentId>(reader));
  DCE_ASSIGN(from, dce::codec::get<Version>(reader));
  DCE_ASSIGN(to, dce::codec::get<Version>(reader));
  DCE_ASSIGN(kind, dce::codec::get<TransitionKind>(reader));
  DCE_ASSIGN(requires_capabilities, dce::codec::get<CapabilitySet>(reader));
  DCE_ASSIGN(provides_capabilities, dce::codec::get<CapabilitySet>(reader));
  DCE_ASSIGN(reversible, reader.boolean());
  DCE_ASSIGN(provenance, dce::codec::get<EvidenceId>(reader));
  DCE_ASSIGN(justification, reader.string(kMaxTextBytes));
  return CompatEdge{.component = component,
                    .from = from,
                    .to = to,
                    .kind = kind,
                    .requires_capabilities = requires_capabilities,
                    .provides_capabilities = provides_capabilities,
                    .reversible = reversible,
                    .provenance = provenance,
                    .justification = justification};
}

Status Codec<CompatibilityGraph>::put(CanonicalWriter& writer, const CompatibilityGraph& value) {
  DCE_TRY(put_list<ComponentVersion>(writer, value.nodes(), kMaxGraphNodes));
  DCE_TRY(put_list<CompatEdge>(writer, value.edges(), kMaxGraphEdges));
  return Status{};
}

Result<CompatibilityGraph> Codec<CompatibilityGraph>::get(CanonicalReader& reader) {
  DCE_ASSIGN(nodes, get_list<ComponentVersion>(reader, kMaxGraphNodes));
  DCE_ASSIGN(edges, get_list<CompatEdge>(reader, kMaxGraphEdges));
  CompatibilityGraph value;
  for (const ComponentVersion& node : nodes) {
    DCE_TRY(value.add_node(node));
  }
  for (const CompatEdge& edge : edges) {
    DCE_TRY(value.add_edge(edge));
  }
  return value;
}

Status Codec<MigrationStep>::put(CanonicalWriter& writer, const MigrationStep& value) {
  DCE_TRY(dce::codec::put<StepId>(writer, value.id));
  DCE_TRY(dce::codec::put<ComponentId>(writer, value.component));
  DCE_TRY(dce::codec::put<Version>(writer, value.from));
  DCE_TRY(dce::codec::put<Version>(writer, value.to));
  DCE_TRY(dce::codec::put<TransitionKind>(writer, value.kind));
  DCE_TRY(put_list<StepId>(writer, value.depends_on, kMaxStepDependencies));
  DCE_TRY(put_list<GateId>(writer, value.gates, kMaxGates));
  DCE_TRY(dce::codec::put<Irreversibility>(writer, value.irreversibility));
  DCE_TRY(writer.put_bool(value.idempotent));
  DCE_TRY(dce::codec::put<EvidenceId>(writer, value.provenance));
  return Status{};
}

Result<MigrationStep> Codec<MigrationStep>::get(CanonicalReader& reader) {
  DCE_ASSIGN(id, dce::codec::get<StepId>(reader));
  DCE_ASSIGN(component, dce::codec::get<ComponentId>(reader));
  DCE_ASSIGN(from, dce::codec::get<Version>(reader));
  DCE_ASSIGN(to, dce::codec::get<Version>(reader));
  DCE_ASSIGN(kind, dce::codec::get<TransitionKind>(reader));
  DCE_ASSIGN(depends_on, get_list<StepId>(reader, kMaxStepDependencies));
  DCE_ASSIGN(gates, get_list<GateId>(reader, kMaxGates));
  DCE_ASSIGN(irreversibility, dce::codec::get<Irreversibility>(reader));
  DCE_ASSIGN(idempotent, reader.boolean());
  DCE_ASSIGN(provenance, dce::codec::get<EvidenceId>(reader));
  return MigrationStep{.id = id,
                       .component = component,
                       .from = from,
                       .to = to,
                       .kind = kind,
                       .depends_on = depends_on,
                       .gates = gates,
                       .irreversibility = irreversibility,
                       .idempotent = idempotent,
                       .provenance = provenance};
}

Status Codec<RolloutCohort>::put(CanonicalWriter& writer, const RolloutCohort& value) {
  DCE_TRY(dce::codec::put<CohortId>(writer, value.id));
  DCE_TRY(dce::codec::put<CohortWave>(writer, value.wave));
  DCE_TRY(dce::codec::put<RolloutStrategy>(writer, value.strategy));
  DCE_TRY(writer.put_u32(value.max_parallel));
  DCE_TRY(put_list<SiteId>(writer, value.sites, kMaxCohortSites));
  DCE_TRY(put_list<GateId>(writer, value.gates, kMaxGates));
  DCE_TRY(writer.put_bool(value.canary));
  return Status{};
}

Result<RolloutCohort> Codec<RolloutCohort>::get(CanonicalReader& reader) {
  DCE_ASSIGN(id, dce::codec::get<CohortId>(reader));
  DCE_ASSIGN(wave, dce::codec::get<CohortWave>(reader));
  DCE_ASSIGN(strategy, dce::codec::get<RolloutStrategy>(reader));
  DCE_ASSIGN(max_parallel, reader.u32());
  DCE_ASSIGN(sites, get_list<SiteId>(reader, kMaxCohortSites));
  DCE_ASSIGN(gates, get_list<GateId>(reader, kMaxGates));
  DCE_ASSIGN(canary, reader.boolean());
  return RolloutCohort{.id = id,
                       .wave = wave,
                       .strategy = strategy,
                       .max_parallel = max_parallel,
                       .sites = sites,
                       .gates = gates,
                       .canary = canary};
}

Status Codec<SiteRecord>::put(CanonicalWriter& writer, const SiteRecord& value) {
  DCE_TRY(dce::codec::put<SiteId>(writer, value.id));
  DCE_TRY(dce::codec::put<Version>(writer, value.dccp_version));
  DCE_TRY(put_list<ComponentVersion>(writer, value.components, kMaxComponents));
  DCE_TRY(dce::codec::put<SiteGeneration>(writer, value.accepted_generation));
  DCE_TRY(dce::codec::put<CapabilitySet>(writer, value.capabilities));
  DCE_TRY(dce::codec::put<PolicyGeneration>(writer, value.policy));
  DCE_TRY(put_list<DependencyCompatibilityRef>(writer, value.dependencies, kMaxDependencies));
  DCE_TRY(put_list<ObligationId>(writer, value.obligations, kMaxSites));
  DCE_TRY(dce::codec::put<Digest256>(writer, value.state_digest));
  DCE_TRY(writer.put_bool(value.delegated_rollout_authority));
  return Status{};
}

Result<SiteRecord> Codec<SiteRecord>::get(CanonicalReader& reader) {
  DCE_ASSIGN(id, dce::codec::get<SiteId>(reader));
  DCE_ASSIGN(dccp_version, dce::codec::get<Version>(reader));
  DCE_ASSIGN(components, get_list<ComponentVersion>(reader, kMaxComponents));
  DCE_ASSIGN(accepted_generation, dce::codec::get<SiteGeneration>(reader));
  DCE_ASSIGN(capabilities, dce::codec::get<CapabilitySet>(reader));
  DCE_ASSIGN(policy, dce::codec::get<PolicyGeneration>(reader));
  DCE_ASSIGN(dependencies, get_list<DependencyCompatibilityRef>(reader, kMaxDependencies));
  DCE_ASSIGN(obligations, get_list<ObligationId>(reader, kMaxSites));
  DCE_ASSIGN(state_digest, dce::codec::get<Digest256>(reader));
  DCE_ASSIGN(delegated_rollout_authority, reader.boolean());
  return SiteRecord{.id = id,
                    .dccp_version = dccp_version,
                    .components = components,
                    .accepted_generation = accepted_generation,
                    .capabilities = capabilities,
                    .policy = policy,
                    .dependencies = dependencies,
                    .obligations = obligations,
                    .state_digest = state_digest,
                    .delegated_rollout_authority = delegated_rollout_authority};
}

Status Codec<SiteMembershipSnapshot>::put(CanonicalWriter& writer, const SiteMembershipSnapshot& value) {
  // The digest is derived; only the sites it summarises are stored.
  return put_list<SiteRecord>(writer, value.sites, kMaxSites);
}

Result<SiteMembershipSnapshot> Codec<SiteMembershipSnapshot>::get(CanonicalReader& reader) {
  DCE_ASSIGN(sites, get_list<SiteRecord>(reader, kMaxSites));
  // The derived digest is left zero; the caller recomputes it.
  return SiteMembershipSnapshot{.sites = sites, .digest = Digest256{}};
}

Status Codec<Precondition>::put(CanonicalWriter& writer, const Precondition& value) {
  DCE_TRY(put_text(writer, value.name, kMaxTextBytes));
  DCE_TRY(put_text(writer, value.description, kMaxTextBytes));
  DCE_TRY(dce::codec::put<GateId>(writer, value.gate));
  return Status{};
}

Result<Precondition> Codec<Precondition>::get(CanonicalReader& reader) {
  DCE_ASSIGN(name, reader.string(kMaxTextBytes));
  DCE_ASSIGN(description, reader.string(kMaxTextBytes));
  DCE_ASSIGN(gate, dce::codec::get<GateId>(reader));
  return Precondition{.name = name, .description = description, .gate = gate};
}

Status Codec<PointOfNoReturn>::put(CanonicalWriter& writer, const PointOfNoReturn& value) {
  DCE_TRY(dce::codec::put<CohortWave>(writer, value.wave));
  DCE_TRY(put_optional<StepId>(writer, value.step));
  DCE_TRY(put_text(writer, value.justification, kMaxTextBytes));
  return Status{};
}

Result<PointOfNoReturn> Codec<PointOfNoReturn>::get(CanonicalReader& reader) {
  DCE_ASSIGN(wave, dce::codec::get<CohortWave>(reader));
  DCE_ASSIGN(step, get_optional<StepId>(reader));
  DCE_ASSIGN(justification, reader.string(kMaxTextBytes));
  return PointOfNoReturn{.wave = wave, .step = step, .justification = justification};
}

Status Codec<RollbackPolicy>::put(CanonicalWriter& writer, const RollbackPolicy& value) {
  DCE_TRY(writer.put_bool(value.permitted));
  DCE_TRY(dce::codec::put<CohortWave>(writer, value.max_waves_back));
  DCE_TRY(put_list<StepId>(writer, value.irreversible_steps, kMaxSteps));
  return Status{};
}

Result<RollbackPolicy> Codec<RollbackPolicy>::get(CanonicalReader& reader) {
  DCE_ASSIGN(permitted, reader.boolean());
  DCE_ASSIGN(max_waves_back, dce::codec::get<CohortWave>(reader));
  DCE_ASSIGN(irreversible_steps, get_list<StepId>(reader, kMaxSteps));
  return RollbackPolicy{.permitted = permitted,
                        .max_waves_back = max_waves_back,
                        .irreversible_steps = irreversible_steps};
}

Status Codec<DeprecationGate>::put(CanonicalWriter& writer, const DeprecationGate& value) {
  DCE_TRY(dce::codec::put<GateId>(writer, value.gate));
  DCE_TRY(dce::codec::put<CapabilityId>(writer, value.capability));
  DCE_TRY(put_optional<Version>(writer, value.removal_version));
  DCE_TRY(dce::codec::put<EvidenceId>(writer, value.provenance));
  return Status{};
}

Result<DeprecationGate> Codec<DeprecationGate>::get(CanonicalReader& reader) {
  DCE_ASSIGN(gate, dce::codec::get<GateId>(reader));
  DCE_ASSIGN(capability, dce::codec::get<CapabilityId>(reader));
  DCE_ASSIGN(removal_version, get_optional<Version>(reader));
  DCE_ASSIGN(provenance, dce::codec::get<EvidenceId>(reader));
  return DeprecationGate{.gate = gate,
                         .capability = capability,
                         .removal_version = removal_version,
                         .provenance = provenance};
}

Status Codec<ExceptionGrant>::put(CanonicalWriter& writer, const ExceptionGrant& value) {
  DCE_TRY(dce::codec::put<ExceptionId>(writer, value.id));
  DCE_TRY(dce::codec::put<GateId>(writer, value.waived_gate));
  DCE_TRY(dce::codec::put<ActorId>(writer, value.granted_by));
  DCE_TRY(put_text(writer, value.justification, kMaxTextBytes));
  DCE_TRY(dce::codec::put<CoordinatorEpoch>(writer, value.epoch));
  DCE_TRY(writer.put_bool(value.requests_irreversible));
  return Status{};
}

Result<ExceptionGrant> Codec<ExceptionGrant>::get(CanonicalReader& reader) {
  DCE_ASSIGN(id, dce::codec::get<ExceptionId>(reader));
  DCE_ASSIGN(waived_gate, dce::codec::get<GateId>(reader));
  DCE_ASSIGN(granted_by, dce::codec::get<ActorId>(reader));
  DCE_ASSIGN(justification, reader.string(kMaxTextBytes));
  DCE_ASSIGN(epoch, dce::codec::get<CoordinatorEpoch>(reader));
  DCE_ASSIGN(requests_irreversible, reader.boolean());
  return ExceptionGrant{.id = id,
                        .waived_gate = waived_gate,
                        .granted_by = granted_by,
                        .justification = justification,
                        .epoch = epoch,
                        .requests_irreversible = requests_irreversible};
}

Status Codec<PlanIdentity>::put(CanonicalWriter& writer, const PlanIdentity& value) {
  DCE_TRY(dce::codec::put<EvolutionPlanId>(writer, value.id));
  DCE_TRY(dce::codec::put<PlanRevision>(writer, value.revision));
  DCE_TRY(dce::codec::put<PlanGeneration>(writer, value.generation));
  DCE_TRY(dce::codec::put<CoordinatorEpoch>(writer, value.epoch));
  return Status{};
}

Result<PlanIdentity> Codec<PlanIdentity>::get(CanonicalReader& reader) {
  DCE_ASSIGN(id, dce::codec::get<EvolutionPlanId>(reader));
  DCE_ASSIGN(revision, dce::codec::get<PlanRevision>(reader));
  DCE_ASSIGN(generation, dce::codec::get<PlanGeneration>(reader));
  DCE_ASSIGN(epoch, dce::codec::get<CoordinatorEpoch>(reader));
  return PlanIdentity{.id = id,
                      .revision = revision,
                      .generation = generation,
                      .epoch = epoch};
}

Status Codec<EvolutionPlan>::put(CanonicalWriter& writer, const EvolutionPlan& value) {
  // The plan digest is derived and is deliberately not part of the layout.
  DCE_TRY(dce::codec::put<PlanIdentity>(writer, value.identity));
  DCE_TRY(put_text(writer, value.title, kMaxTextBytes));
  DCE_TRY(dce::codec::put<Version>(writer, value.source_version));
  DCE_TRY(dce::codec::put<Version>(writer, value.target_version));
  DCE_TRY(dce::codec::put<Digest256>(writer, value.source_state_digest));
  DCE_TRY(dce::codec::put<SiteMembershipSnapshot>(writer, value.membership));
  DCE_TRY(dce::codec::put<CapabilityMatrix>(writer, value.capabilities));
  DCE_TRY(dce::codec::put<CompatibilityGraph>(writer, value.compatibility));
  DCE_TRY(put_list<MigrationStep>(writer, value.steps, kMaxSteps));
  DCE_TRY(put_list<RolloutCohort>(writer, value.cohorts, kMaxCohorts));
  DCE_TRY(put_list<Precondition>(writer, value.preconditions, kMaxPreconditions));
  DCE_TRY(dce::codec::put<GateSet>(writer, value.gates));
  DCE_TRY(put_optional<PointOfNoReturn>(writer, value.point_of_no_return));
  DCE_TRY(dce::codec::put<RollbackPolicy>(writer, value.rollback));
  DCE_TRY(put_list<DeprecationGate>(writer, value.deprecations, kMaxDeprecations));
  DCE_TRY(put_list<ExceptionGrant>(writer, value.exceptions, kMaxExceptions));
  DCE_TRY(dce::codec::put<EvidenceSet>(writer, value.evidence));
  DCE_TRY(dce::codec::put<PolicyRequirement>(writer, value.policy));
  return Status{};
}

Result<EvolutionPlan> Codec<EvolutionPlan>::get(CanonicalReader& reader) {
  DCE_ASSIGN(identity, dce::codec::get<PlanIdentity>(reader));
  DCE_ASSIGN(title, reader.string(kMaxTextBytes));
  DCE_ASSIGN(source_version, dce::codec::get<Version>(reader));
  DCE_ASSIGN(target_version, dce::codec::get<Version>(reader));
  DCE_ASSIGN(source_state_digest, dce::codec::get<Digest256>(reader));
  DCE_ASSIGN(membership, dce::codec::get<SiteMembershipSnapshot>(reader));
  DCE_ASSIGN(capabilities, dce::codec::get<CapabilityMatrix>(reader));
  DCE_ASSIGN(compatibility, dce::codec::get<CompatibilityGraph>(reader));
  DCE_ASSIGN(steps, get_list<MigrationStep>(reader, kMaxSteps));
  DCE_ASSIGN(cohorts, get_list<RolloutCohort>(reader, kMaxCohorts));
  DCE_ASSIGN(preconditions, get_list<Precondition>(reader, kMaxPreconditions));
  DCE_ASSIGN(gates, dce::codec::get<GateSet>(reader));
  DCE_ASSIGN(point_of_no_return, get_optional<PointOfNoReturn>(reader));
  DCE_ASSIGN(rollback, dce::codec::get<RollbackPolicy>(reader));
  DCE_ASSIGN(deprecations, get_list<DeprecationGate>(reader, kMaxDeprecations));
  DCE_ASSIGN(exceptions, get_list<ExceptionGrant>(reader, kMaxExceptions));
  DCE_ASSIGN(evidence, dce::codec::get<EvidenceSet>(reader));
  DCE_ASSIGN(policy, dce::codec::get<PolicyRequirement>(reader));
  // The derived digest is left zero; the caller recomputes it.
  return EvolutionPlan{.identity = identity,
                       .title = title,
                       .source_version = source_version,
                       .target_version = target_version,
                       .source_state_digest = source_state_digest,
                       .membership = membership,
                       .capabilities = capabilities,
                       .compatibility = compatibility,
                       .steps = steps,
                       .cohorts = cohorts,
                       .preconditions = preconditions,
                       .gates = gates,
                       .point_of_no_return = point_of_no_return,
                       .rollback = rollback,
                       .deprecations = deprecations,
                       .exceptions = exceptions,
                       .evidence = evidence,
                       .policy = policy,
                       // The derived digest is left zero; the caller recomputes it.
                       .digest = Digest256{}};
}

Status Codec<AuthorityToken>::put(CanonicalWriter& writer, const AuthorityToken& value) {
  DCE_TRY(dce::codec::put<EvolutionPlanId>(writer, value.plan));
  DCE_TRY(dce::codec::put<PlanGeneration>(writer, value.plan_generation));
  DCE_TRY(dce::codec::put<CoordinatorEpoch>(writer, value.epoch));
  DCE_TRY(dce::codec::put<SiteId>(writer, value.site));
  DCE_TRY(dce::codec::put<CohortId>(writer, value.cohort));
  DCE_TRY(dce::codec::put<StageOrdinal>(writer, value.stage));
  DCE_TRY(dce::codec::put<Digest256>(writer, value.plan_digest));
  return Status{};
}

Result<AuthorityToken> Codec<AuthorityToken>::get(CanonicalReader& reader) {
  DCE_ASSIGN(plan, dce::codec::get<EvolutionPlanId>(reader));
  DCE_ASSIGN(plan_generation, dce::codec::get<PlanGeneration>(reader));
  DCE_ASSIGN(epoch, dce::codec::get<CoordinatorEpoch>(reader));
  DCE_ASSIGN(site, dce::codec::get<SiteId>(reader));
  DCE_ASSIGN(cohort, dce::codec::get<CohortId>(reader));
  DCE_ASSIGN(stage, dce::codec::get<StageOrdinal>(reader));
  DCE_ASSIGN(plan_digest, dce::codec::get<Digest256>(reader));
  return AuthorityToken{.plan = plan,
                        .plan_generation = plan_generation,
                        .epoch = epoch,
                        .site = site,
                        .cohort = cohort,
                        .stage = stage,
                        .plan_digest = plan_digest};
}

Status Codec<MigrationReceipt>::put(CanonicalWriter& writer, const MigrationReceipt& value) {
  DCE_TRY(dce::codec::put<ReceiptId>(writer, value.id));
  DCE_TRY(dce::codec::put<EvolutionPlanId>(writer, value.plan));
  DCE_TRY(dce::codec::put<PlanGeneration>(writer, value.plan_generation));
  DCE_TRY(dce::codec::put<CoordinatorEpoch>(writer, value.epoch));
  DCE_TRY(dce::codec::put<SiteId>(writer, value.site));
  DCE_TRY(dce::codec::put<StepId>(writer, value.step));
  DCE_TRY(dce::codec::put<StageOrdinal>(writer, value.stage));
  DCE_TRY(dce::codec::put<Version>(writer, value.from));
  DCE_TRY(dce::codec::put<Version>(writer, value.to));
  DCE_TRY(dce::codec::put<SiteGeneration>(writer, value.accepted_generation));
  DCE_TRY(dce::codec::put<Digest256>(writer, value.evidence_digest));
  DCE_TRY(put_text(writer, value.idempotency_key, kMaxTextBytes));
  return Status{};
}

Result<MigrationReceipt> Codec<MigrationReceipt>::get(CanonicalReader& reader) {
  DCE_ASSIGN(id, dce::codec::get<ReceiptId>(reader));
  DCE_ASSIGN(plan, dce::codec::get<EvolutionPlanId>(reader));
  DCE_ASSIGN(plan_generation, dce::codec::get<PlanGeneration>(reader));
  DCE_ASSIGN(epoch, dce::codec::get<CoordinatorEpoch>(reader));
  DCE_ASSIGN(site, dce::codec::get<SiteId>(reader));
  DCE_ASSIGN(step, dce::codec::get<StepId>(reader));
  DCE_ASSIGN(stage, dce::codec::get<StageOrdinal>(reader));
  DCE_ASSIGN(from, dce::codec::get<Version>(reader));
  DCE_ASSIGN(to, dce::codec::get<Version>(reader));
  DCE_ASSIGN(accepted_generation, dce::codec::get<SiteGeneration>(reader));
  DCE_ASSIGN(evidence_digest, dce::codec::get<Digest256>(reader));
  DCE_ASSIGN(idempotency_key, reader.string(kMaxTextBytes));
  return MigrationReceipt{.id = id,
                          .plan = plan,
                          .plan_generation = plan_generation,
                          .epoch = epoch,
                          .site = site,
                          .step = step,
                          .stage = stage,
                          .from = from,
                          .to = to,
                          .accepted_generation = accepted_generation,
                          .evidence_digest = evidence_digest,
                          .idempotency_key = idempotency_key};
}

Status Codec<StageCheckpoint>::put(CanonicalWriter& writer, const StageCheckpoint& value) {
  DCE_TRY(dce::codec::put<CheckpointId>(writer, value.id));
  DCE_TRY(dce::codec::put<EvolutionPlanId>(writer, value.plan));
  DCE_TRY(dce::codec::put<PlanGeneration>(writer, value.plan_generation));
  DCE_TRY(dce::codec::put<CoordinatorEpoch>(writer, value.epoch));
  DCE_TRY(dce::codec::put<SiteId>(writer, value.site));
  DCE_TRY(dce::codec::put<CohortId>(writer, value.cohort));
  DCE_TRY(dce::codec::put<CohortWave>(writer, value.wave));
  DCE_TRY(dce::codec::put<StageOrdinal>(writer, value.stage));
  DCE_TRY(dce::codec::put<SiteGeneration>(writer, value.site_generation));
  DCE_TRY(dce::codec::put<Digest256>(writer, value.stage_digest));
  DCE_TRY(dce::codec::put<ReceiptId>(writer, value.receipt));
  return Status{};
}

Result<StageCheckpoint> Codec<StageCheckpoint>::get(CanonicalReader& reader) {
  DCE_ASSIGN(id, dce::codec::get<CheckpointId>(reader));
  DCE_ASSIGN(plan, dce::codec::get<EvolutionPlanId>(reader));
  DCE_ASSIGN(plan_generation, dce::codec::get<PlanGeneration>(reader));
  DCE_ASSIGN(epoch, dce::codec::get<CoordinatorEpoch>(reader));
  DCE_ASSIGN(site, dce::codec::get<SiteId>(reader));
  DCE_ASSIGN(cohort, dce::codec::get<CohortId>(reader));
  DCE_ASSIGN(wave, dce::codec::get<CohortWave>(reader));
  DCE_ASSIGN(stage, dce::codec::get<StageOrdinal>(reader));
  DCE_ASSIGN(site_generation, dce::codec::get<SiteGeneration>(reader));
  DCE_ASSIGN(stage_digest, dce::codec::get<Digest256>(reader));
  DCE_ASSIGN(receipt, dce::codec::get<ReceiptId>(reader));
  return StageCheckpoint{.id = id,
                         .plan = plan,
                         .plan_generation = plan_generation,
                         .epoch = epoch,
                         .site = site,
                         .cohort = cohort,
                         .wave = wave,
                         .stage = stage,
                         .site_generation = site_generation,
                         .stage_digest = stage_digest,
                         .receipt = receipt};
}

Status Codec<RollbackMarker>::put(CanonicalWriter& writer, const RollbackMarker& value) {
  DCE_TRY(dce::codec::put<EvolutionPlanId>(writer, value.plan));
  DCE_TRY(dce::codec::put<PlanGeneration>(writer, value.plan_generation));
  DCE_TRY(dce::codec::put<CoordinatorEpoch>(writer, value.epoch));
  DCE_TRY(dce::codec::put<CohortWave>(writer, value.wave));
  DCE_TRY(dce::codec::put<StageOrdinal>(writer, value.stage));
  DCE_TRY(dce::codec::put<StepId>(writer, value.step));
  DCE_TRY(writer.put_bool(value.crossed_point_of_no_return));
  DCE_TRY(put_text(writer, value.justification, kMaxTextBytes));
  return Status{};
}

Result<RollbackMarker> Codec<RollbackMarker>::get(CanonicalReader& reader) {
  DCE_ASSIGN(plan, dce::codec::get<EvolutionPlanId>(reader));
  DCE_ASSIGN(plan_generation, dce::codec::get<PlanGeneration>(reader));
  DCE_ASSIGN(epoch, dce::codec::get<CoordinatorEpoch>(reader));
  DCE_ASSIGN(wave, dce::codec::get<CohortWave>(reader));
  DCE_ASSIGN(stage, dce::codec::get<StageOrdinal>(reader));
  DCE_ASSIGN(step, dce::codec::get<StepId>(reader));
  DCE_ASSIGN(crossed_point_of_no_return, reader.boolean());
  DCE_ASSIGN(justification, reader.string(kMaxTextBytes));
  return RollbackMarker{.plan = plan,
                        .plan_generation = plan_generation,
                        .epoch = epoch,
                        .wave = wave,
                        .stage = stage,
                        .step = step,
                        .crossed_point_of_no_return = crossed_point_of_no_return,
                        .justification = justification};
}

Status Codec<SiteProgress>::put(CanonicalWriter& writer, const SiteProgress& value) {
  DCE_TRY(dce::codec::put<SiteId>(writer, value.site));
  DCE_TRY(dce::codec::put<CohortId>(writer, value.cohort));
  DCE_TRY(dce::codec::put<CohortWave>(writer, value.wave));
  DCE_TRY(dce::codec::put<StageOrdinal>(writer, value.accepted_stage));
  DCE_TRY(dce::codec::put<SiteGeneration>(writer, value.generation));
  DCE_TRY(dce::codec::put<Digest256>(writer, value.stage_digest));
  DCE_TRY(writer.put_bool(value.partitioned));
  DCE_TRY(writer.put_bool(value.in_flight));
  return Status{};
}

Result<SiteProgress> Codec<SiteProgress>::get(CanonicalReader& reader) {
  DCE_ASSIGN(site, dce::codec::get<SiteId>(reader));
  DCE_ASSIGN(cohort, dce::codec::get<CohortId>(reader));
  DCE_ASSIGN(wave, dce::codec::get<CohortWave>(reader));
  DCE_ASSIGN(accepted_stage, dce::codec::get<StageOrdinal>(reader));
  DCE_ASSIGN(generation, dce::codec::get<SiteGeneration>(reader));
  DCE_ASSIGN(stage_digest, dce::codec::get<Digest256>(reader));
  DCE_ASSIGN(partitioned, reader.boolean());
  DCE_ASSIGN(in_flight, reader.boolean());
  return SiteProgress{.site = site,
                      .cohort = cohort,
                      .wave = wave,
                      .accepted_stage = accepted_stage,
                      .generation = generation,
                      .stage_digest = stage_digest,
                      .partitioned = partitioned,
                      .in_flight = in_flight};
}

Status Codec<SiteStageReport>::put(CanonicalWriter& writer, const SiteStageReport& value) {
  DCE_TRY(dce::codec::put<SiteId>(writer, value.site));
  DCE_TRY(dce::codec::put<SiteGeneration>(writer, value.generation));
  DCE_TRY(dce::codec::put<CoordinatorEpoch>(writer, value.epoch));
  DCE_TRY(dce::codec::put<StageOrdinal>(writer, value.accepted_stage));
  DCE_TRY(dce::codec::put<Digest256>(writer, value.accepted_stage_digest));
  DCE_TRY(put_list<ReceiptId>(writer, value.receipts, kMaxReceipts));
  DCE_TRY(writer.put_bool(value.partitioned));
  return Status{};
}

Result<SiteStageReport> Codec<SiteStageReport>::get(CanonicalReader& reader) {
  DCE_ASSIGN(site, dce::codec::get<SiteId>(reader));
  DCE_ASSIGN(generation, dce::codec::get<SiteGeneration>(reader));
  DCE_ASSIGN(epoch, dce::codec::get<CoordinatorEpoch>(reader));
  DCE_ASSIGN(accepted_stage, dce::codec::get<StageOrdinal>(reader));
  DCE_ASSIGN(accepted_stage_digest, dce::codec::get<Digest256>(reader));
  DCE_ASSIGN(receipts, get_list<ReceiptId>(reader, kMaxReceipts));
  DCE_ASSIGN(partitioned, reader.boolean());
  return SiteStageReport{.site = site,
                         .generation = generation,
                         .epoch = epoch,
                         .accepted_stage = accepted_stage,
                         .accepted_stage_digest = accepted_stage_digest,
                         .receipts = receipts,
                         .partitioned = partitioned};
}

}  // namespace dce::codec
