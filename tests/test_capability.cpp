// The capability model: what a version offers, what it needs, and what may
// therefore interoperate. The behaviours that matter most here are the ones
// where a wrong answer would be optimistic: an undescribed version must be
// unknown, and an unobserved dependency must be indeterminate.
#include <string>
#include <vector>

#include "dce/capability.hpp"
#include "harness.hpp"

namespace {

dce::CapabilityId cap(const std::string& name) { return *dce::CapabilityId::parse(name); }
dce::ComponentId comp(const std::string& name) { return *dce::ComponentId::parse(name); }

dce::ComponentCapabilities entry(const std::string& component, dce::Version version,
                                 const std::vector<std::string>& provides,
                                 const std::vector<std::string>& required) {
  dce::ComponentCapabilities declared;
  declared.component = comp(component);
  declared.version = version;
  // A recorded assertion rather than a fatal one: the helper still returns a
  // value, and a failure here is reported by the harness like any other.
  for (const std::string& name : provides) {
    DCE_CHECK_OK(declared.provides.insert(cap(name)));
  }
  for (const std::string& name : required) {
    DCE_CHECK_OK(declared.requires_capabilities.insert(cap(name)));
  }
  return declared;
}

}  // namespace

DCE_TEST(capability, a_set_is_ordered_and_idempotent) {
  dce::CapabilitySet first;
  DCE_REQUIRE_OK(first.insert(cap("z.last")));
  DCE_REQUIRE_OK(first.insert(cap("a.first")));
  DCE_REQUIRE_OK(first.insert(cap("m.middle")));
  DCE_REQUIRE_OK(first.insert(cap("a.first")));  // idempotent

  DCE_CHECK_EQ(first.size(), static_cast<std::size_t>(3));
  DCE_CHECK_TRUE(first.contains(cap("a.first")));
  DCE_CHECK_TRUE(!first.contains(cap("not.present")));
  // Iteration order is canonical, so equality never depends on insertion order.
  DCE_CHECK_EQ(first.entries().front().str(), std::string("a.first"));
  DCE_CHECK_EQ(first.entries().back().str(), std::string("z.last"));

  dce::CapabilitySet second;
  DCE_REQUIRE_OK(second.insert(cap("m.middle")));
  DCE_REQUIRE_OK(second.insert(cap("a.first")));
  DCE_REQUIRE_OK(second.insert(cap("z.last")));
  DCE_CHECK_TRUE(first == second);

  const std::vector<dce::CapabilityId> missing = first.missing_from(second);
  DCE_CHECK_EQ(missing.size(), static_cast<std::size_t>(0));
  dce::CapabilitySet empty;
  DCE_CHECK_EQ(first.missing_from(empty).size(), static_cast<std::size_t>(3));
  DCE_CHECK_EQ(empty.missing_from(first).size(), static_cast<std::size_t>(0));
}

DCE_TEST(capability, a_set_enforces_its_bound) {
  dce::CapabilitySet set;
  for (std::size_t index = 0; index < dce::CapabilitySet::kMaxCapabilities; ++index) {
    DCE_REQUIRE_OK(set.insert(cap("cap." + std::to_string(index))));
  }
  const dce::Status overflow = set.insert(cap("cap.one.too.many"));
  DCE_CHECK_EQ(overflow.code(), dce::ErrorCode::limit_exceeded);
}

DCE_TEST(capability, the_matrix_is_idempotent_and_conflicts_on_disagreement) {
  dce::CapabilityMatrix matrix;
  const dce::Version one{1, 0, 0};
  DCE_REQUIRE_OK(matrix.add(entry("dccp.power", one, {"cap.power.read"}, {})));
  DCE_REQUIRE_OK(matrix.add(entry("dccp.power", one, {"cap.power.read"}, {})));
  DCE_CHECK_EQ(matrix.entries().size(), static_cast<std::size_t>(1));

  const dce::Status conflict =
      matrix.add(entry("dccp.power", one, {"cap.power.write"}, {}));
  DCE_CHECK_EQ(conflict.code(), dce::ErrorCode::conflict);

  DCE_CHECK_TRUE(matrix.describes(comp("dccp.power"), one));
  DCE_CHECK_TRUE(!matrix.describes(comp("dccp.power"), dce::Version{9, 0, 0}));
  const dce::Result<dce::ComponentCapabilities> found = matrix.lookup(comp("dccp.power"), one);
  DCE_REQUIRE_OK(found);
  DCE_CHECK_TRUE(found->provides.contains(cap("cap.power.read")));
  DCE_CHECK_EQ(matrix.lookup(comp("dccp.power"), dce::Version{9, 0, 0}).code(),
               dce::ErrorCode::not_found);
}

DCE_TEST(capability, interoperability_requires_both_directions_and_is_unknown_when_undescribed) {
  dce::CapabilityMatrix matrix;
  const dce::Version a{1, 0, 0};
  const dce::Version b{2, 0, 0};
  DCE_REQUIRE_OK(matrix.add(entry("dccp.fabric", a, {"cap.a"}, {})));
  DCE_REQUIRE_OK(matrix.add(entry("dccp.fabric", b, {"cap.a", "cap.b"}, {"cap.a"})));

  const dce::InteropVerdict compatible = matrix.interoperate(comp("dccp.fabric"), a, b);
  DCE_CHECK_EQ(compatible.outcome, dce::InteropOutcome::interoperable);
  DCE_CHECK_TRUE(compatible.missing_from_first.empty());
  DCE_CHECK_TRUE(compatible.missing_from_second.empty());

  // Now make the older version require something the newer one does not offer.
  dce::CapabilityMatrix asymmetric;
  DCE_REQUIRE_OK(asymmetric.add(entry("dccp.fabric", a, {"cap.a"}, {"cap.b"})));
  DCE_REQUIRE_OK(asymmetric.add(entry("dccp.fabric", b, {"cap.a"}, {})));
  const dce::InteropVerdict broken = asymmetric.interoperate(comp("dccp.fabric"), a, b);
  DCE_CHECK_EQ(broken.outcome, dce::InteropOutcome::incompatible);
  DCE_CHECK_EQ(broken.missing_from_first.size(), static_cast<std::size_t>(1));

  const dce::InteropVerdict unknown =
      matrix.interoperate(comp("dccp.fabric"), a, dce::Version{7, 7, 7});
  DCE_CHECK_EQ(unknown.outcome, dce::InteropOutcome::unknown);
  DCE_CHECK_TRUE(!unknown.explanation.empty());
}

DCE_TEST(capability, policy_requirements_distinguish_too_low_from_unknown) {
  dce::PolicyRequirement requirement;
  requirement.minimum_ordinal = 4;
  const dce::PolicyGenerationId identity = *dce::PolicyGenerationId::parse("policy.facility");

  DCE_CHECK_EQ(dce::evaluate_policy(requirement, std::nullopt), dce::PolicyVerdict::unknown);
  DCE_CHECK_EQ(dce::evaluate_policy(requirement, dce::PolicyGeneration{identity, 3}),
               dce::PolicyVerdict::too_low);
  DCE_CHECK_EQ(dce::evaluate_policy(requirement, dce::PolicyGeneration{identity, 4}),
               dce::PolicyVerdict::satisfied);
  DCE_CHECK_EQ(dce::evaluate_policy(requirement, dce::PolicyGeneration{identity, 9}),
               dce::PolicyVerdict::satisfied);

  requirement.exact = *dce::PolicyGenerationId::parse("policy.other");
  DCE_CHECK_EQ(dce::evaluate_policy(requirement, dce::PolicyGeneration{identity, 9}),
               dce::PolicyVerdict::identity_mismatch);
}

DCE_TEST(capability, an_unobserved_dependency_is_never_treated_as_compatible) {
  dce::DependencyCompatibilityRef reference;
  reference.reference = *dce::BoundaryRefId::parse("dfi.fabric.contract");
  reference.boundary = "dfi";
  reference.supported = *dce::VersionRange::parse("1.0.0..2.0.0");
  DCE_REQUIRE_OK(reference.required.insert(cap("cap.dfi.transport")));
  reference.provenance = *dce::EvidenceId::parse("ev.dfi.attestation");

  // Never observed at all.
  const dce::DependencyAssessment indeterminate = dce::assess_dependency(reference);
  DCE_CHECK_EQ(indeterminate.verdict, dce::DependencyVerdict::indeterminate);
  DCE_CHECK_TRUE(!indeterminate.explanation.empty());

  // Observed, but the version is outside the supported range.
  reference.observed = true;
  reference.observed_version = dce::Version{3, 0, 0};
  DCE_REQUIRE_OK(reference.observed_capabilities.insert(cap("cap.dfi.transport")));
  DCE_CHECK_EQ(dce::assess_dependency(reference).verdict,
               dce::DependencyVerdict::version_out_of_range);

  // Observed and in range, but missing a capability we consume.
  reference.observed_version = dce::Version{1, 4, 0};
  reference.observed_capabilities = dce::CapabilitySet{};
  const dce::DependencyAssessment missing = dce::assess_dependency(reference);
  DCE_CHECK_EQ(missing.verdict, dce::DependencyVerdict::capability_missing);
  DCE_CHECK_EQ(missing.missing_capabilities.size(), static_cast<std::size_t>(1));

  // Fully observed.
  DCE_REQUIRE_OK(reference.observed_capabilities.insert(cap("cap.dfi.transport")));
  DCE_CHECK_EQ(dce::assess_dependency(reference).verdict, dce::DependencyVerdict::satisfied);
}

DCE_TEST(capability, removal_needs_a_current_proof_with_nothing_outstanding) {
  const dce::CapabilityId capability = cap("cap.legacy.path");
  const dce::CoordinatorEpoch epoch = dce::CoordinatorEpoch::from_value(5);

  DCE_CHECK_EQ(dce::assess_removal(capability, nullptr, epoch).verdict,
               dce::RemovalVerdict::proof_missing);

  dce::CapabilityDependencyProof proof;
  proof.capability = capability;
  proof.provenance = *dce::EvidenceId::parse("ev.removal.legacy");
  proof.observed_epoch = dce::CoordinatorEpoch::from_value(9);
  DCE_CHECK_EQ(dce::assess_removal(capability, &proof, epoch).verdict,
               dce::RemovalVerdict::proof_stale);

  proof.observed_epoch = epoch;
  DCE_CHECK_EQ(dce::assess_removal(capability, &proof, epoch).verdict,
               dce::RemovalVerdict::permitted);

  proof.remaining_dependents.push_back(*dce::SiteId::parse("site-a"));
  DCE_CHECK_EQ(dce::assess_removal(capability, &proof, epoch).verdict,
               dce::RemovalVerdict::dependents_remain);

  proof.remaining_dependents.clear();
  proof.obligations.push_back(*dce::ObligationId::parse("obligation.1"));
  DCE_CHECK_EQ(dce::assess_removal(capability, &proof, epoch).verdict,
               dce::RemovalVerdict::dependents_remain);

  // A proof for a different capability proves nothing about this one.
  dce::CapabilityDependencyProof other = proof;
  other.capability = cap("cap.other");
  DCE_CHECK_EQ(dce::assess_removal(capability, &other, epoch).verdict,
               dce::RemovalVerdict::proof_missing);
}
