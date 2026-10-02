// Capability model: what a component version offers, what it needs, and what a
// mixed-version fleet can therefore interoperate with.
//
// Compatibility is never inferred from version numbers here. A version is
// interoperable with another only when the capability matrix says so, and an
// absent matrix entry is reported as unknown rather than assumed compatible.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "dce/ids.hpp"
#include "dce/status.hpp"
#include "dce/version.hpp"

namespace dce {

// Bounded, canonically ordered set of capability identities.
class CapabilitySet {
 public:
  static constexpr std::size_t kMaxCapabilities = 1024;

  // Inserting a capability that is already present is an idempotent success:
  // a set has no notion of a duplicate member.
  [[nodiscard]] Status insert(CapabilityId capability);
  [[nodiscard]] bool contains(const CapabilityId& capability) const;
  [[nodiscard]] std::size_t size() const noexcept { return capabilities_.size(); }
  [[nodiscard]] bool empty() const noexcept { return capabilities_.empty(); }

  [[nodiscard]] const std::vector<CapabilityId>& entries() const noexcept { return capabilities_; }

  // Capabilities present in this set and absent from the other.
  [[nodiscard]] std::vector<CapabilityId> missing_from(const CapabilitySet& other) const;

  [[nodiscard]] std::vector<CapabilityId>::const_iterator begin() const noexcept { return capabilities_.begin(); }
  [[nodiscard]] std::vector<CapabilityId>::const_iterator end() const noexcept { return capabilities_.end(); }

  auto operator<=>(const CapabilitySet&) const = default;

 private:
  // Kept sorted by identity so that equality, digest and iteration order are
  // canonical regardless of insertion order.
  std::vector<CapabilityId> capabilities_;
};

// Declared capabilities of one component at one exact version.
struct ComponentCapabilities {
  ComponentId component;
  Version version;
  CapabilitySet provides;
  // Named to avoid the C++20 keyword; the matching field on CompatEdge is
  // requires_capabilities.
  CapabilitySet requires_capabilities;

  auto operator<=>(const ComponentCapabilities&) const = default;
};

enum class InteropOutcome : std::uint8_t {
  interoperable,   // both directions of the requirement are satisfied
  incompatible,    // at least one direction is not satisfied
  unknown,         // the matrix does not describe one of the two versions
};

struct InteropVerdict {
  InteropOutcome outcome{InteropOutcome::unknown};
  std::vector<CapabilityId> missing_from_first;
  std::vector<CapabilityId> missing_from_second;
  std::string explanation;
};

// The capability matrix: the formal statement of what every component version
// in this evolution offers and consumes.
class CapabilityMatrix {
 public:
  static constexpr std::size_t kMaxEntries = 4096;

  // Adding the same (component, version) twice with identical content is
  // idempotent; adding it twice with different content is a conflict.
  [[nodiscard]] Status add(ComponentCapabilities entry);

  [[nodiscard]] Result<ComponentCapabilities> lookup(const ComponentId& component,
                                                     const Version& version) const;

  [[nodiscard]] bool describes(const ComponentId& component, const Version& version) const;

  // Canonical order: component identity, then version.
  void canonicalize();
  [[nodiscard]] const std::vector<ComponentCapabilities>& entries() const noexcept { return entries_; }

  [[nodiscard]] InteropVerdict interoperate(const ComponentId& component, const Version& first,
                                            const Version& second) const;

 private:
  std::vector<ComponentCapabilities> entries_;
};

// A generation of facility-wide policy, as published by the policy boundary.
// The ordinal is the only part this runtime orders; the identity is what it
// records and reports.
struct PolicyGeneration {
  PolicyGenerationId id;
  std::uint64_t ordinal{0};

  auto operator<=>(const PolicyGeneration&) const = default;
};

struct PolicyRequirement {
  std::uint64_t minimum_ordinal{0};
  std::optional<PolicyGenerationId> exact;

  auto operator<=>(const PolicyRequirement&) const = default;
};

enum class PolicyVerdict : std::uint8_t { satisfied, too_low, identity_mismatch, unknown };

[[nodiscard]] PolicyVerdict evaluate_policy(const PolicyRequirement& requirement,
                                            const std::optional<PolicyGeneration>& observed);

// An explicit compatibility contract consumed from a neighbouring DCCP
// runtime or from ASI/DFI. Nothing here is guessed: an unobserved dependency
// is reported as indeterminate, which is not the same as compatible.
struct DependencyCompatibilityRef {
  BoundaryRefId reference;
  std::string boundary;  // "asi", "dfi", "dccp.facility", "dccp.federation", ...
  VersionRange supported;
  CapabilitySet required;
  Version observed_version;
  CapabilitySet observed_capabilities;
  SiteGeneration observed_generation;
  EvidenceId provenance;
  bool observed{false};

  auto operator<=>(const DependencyCompatibilityRef&) const = default;
};

enum class DependencyVerdict : std::uint8_t {
  satisfied,
  version_out_of_range,
  capability_missing,
  indeterminate,  // never observed: the truth is not knowable from what we hold
};

struct DependencyAssessment {
  DependencyVerdict verdict{DependencyVerdict::indeterminate};
  std::vector<CapabilityId> missing_capabilities;
  std::string explanation;
};

[[nodiscard]] DependencyAssessment assess_dependency(const DependencyCompatibilityRef& reference);

// Proof that nothing still depends on a capability, required before that
// capability may be removed or its compatibility path deprecated.
struct CapabilityDependencyProof {
  CapabilityId capability;
  std::vector<SiteId> remaining_dependents;
  std::vector<ObligationId> obligations;
  EvidenceId provenance;
  CoordinatorEpoch observed_epoch;

  auto operator<=>(const CapabilityDependencyProof&) const = default;
};

enum class RemovalVerdict : std::uint8_t { permitted, dependents_remain, proof_missing, proof_stale };

struct RemovalAssessment {
  RemovalVerdict verdict{RemovalVerdict::proof_missing};
  std::string explanation;
};

[[nodiscard]] RemovalAssessment assess_removal(const CapabilityId& capability,
                                               const CapabilityDependencyProof* proof,
                                               CoordinatorEpoch current_epoch);

}  // namespace dce
