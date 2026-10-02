// The plan model: canonical ordering, the digest that identifies authoritative
// content, structural soundness, and the persisted form being lossless.
#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include "dce/codec.hpp"
#include "dce/synthetic.hpp"
#include "dce/validate.hpp"
#include "harness.hpp"

namespace {

dce::Result<dce::SyntheticFleet> small_fleet(std::uint64_t seed) {
  dce::SyntheticFleetOptions options;
  options.sites = 6;
  options.components = 2;
  options.cohorts = 2;
  options.seed = seed;
  return dce::make_synthetic_fleet(options);
}

}  // namespace

DCE_TEST(plan, canonicalization_is_idempotent_and_order_independent) {
  dce::Result<dce::SyntheticFleet> first_fleet = small_fleet(3);
  dce::Result<dce::SyntheticFleet> second_fleet = small_fleet(3);
  DCE_REQUIRE_OK(first_fleet);
  DCE_REQUIRE_OK(second_fleet);
  dce::SyntheticFleet first = std::move(*first_fleet);
  dce::SyntheticFleet second = std::move(*second_fleet);

  // Reorder every collection in the second plan without changing its content.
  std::reverse(second.plan.membership.sites.begin(), second.plan.membership.sites.end());
  std::reverse(second.plan.steps.begin(), second.plan.steps.end());
  std::reverse(second.plan.cohorts.begin(), second.plan.cohorts.end());
  DCE_REQUIRE_OK(dce::canonicalize(first.plan));
  DCE_REQUIRE_OK(dce::canonicalize(second.plan));
  DCE_CHECK_TRUE(first.plan.digest == second.plan.digest);

  const dce::Digest256 before = first.plan.digest;
  DCE_REQUIRE_OK(dce::canonicalize(first.plan));
  DCE_CHECK_TRUE(first.plan.digest == before);
}

DCE_TEST(plan, the_digest_changes_when_authoritative_content_changes) {
  dce::Result<dce::SyntheticFleet> built = small_fleet(4);
  DCE_REQUIRE_OK(built);
  dce::SyntheticFleet fleet = std::move(*built);
  const dce::Digest256 original = fleet.plan.digest;

  dce::EvolutionPlan changed = fleet.plan;
  changed.title = "a different title";
  DCE_REQUIRE_OK(dce::canonicalize(changed));
  DCE_CHECK_TRUE(!(changed.digest == original));

  dce::EvolutionPlan another = fleet.plan;
  another.membership.sites.front().accepted_generation = dce::SiteGeneration::from_value(7);
  DCE_REQUIRE_OK(dce::canonicalize(another));
  DCE_CHECK_TRUE(!(another.digest == original));
}

DCE_TEST(plan, computing_the_digest_does_not_mutate_the_plan) {
  dce::Result<dce::SyntheticFleet> built = small_fleet(5);
  DCE_REQUIRE_OK(built);
  dce::SyntheticFleet fleet = std::move(*built);
  const dce::EvolutionPlan snapshot = fleet.plan;
  const dce::Result<dce::Digest256> digest = dce::compute_plan_digest(fleet.plan);
  DCE_REQUIRE_OK(digest);
  DCE_CHECK_TRUE(*digest == snapshot.digest);
  DCE_CHECK_EQ(fleet.plan.steps.size(), snapshot.steps.size());
  for (std::size_t index = 0; index < snapshot.steps.size(); ++index) {
    DCE_CHECK_TRUE(fleet.plan.steps[index].id == snapshot.steps[index].id);
  }
  DCE_CHECK_TRUE(fleet.plan.membership.digest == snapshot.membership.digest);
}

DCE_TEST(plan, structural_check_accepts_sound_plans_and_names_every_defect) {
  dce::Result<dce::SyntheticFleet> built = small_fleet(6);
  DCE_REQUIRE_OK(built);
  dce::SyntheticFleet fleet = std::move(*built);
  DCE_CHECK_TRUE(dce::structural_check(fleet.plan).ok());

  {
    dce::EvolutionPlan plan = fleet.plan;
    plan.steps.push_back(plan.steps.front());
    const dce::Status status = dce::structural_check(plan);
    DCE_CHECK_EQ(status.code(), dce::ErrorCode::already_exists);
    DCE_CHECK_TRUE(status.message().find(plan.steps.front().id.str()) != std::string::npos);
  }
  {
    dce::EvolutionPlan plan = fleet.plan;
    plan.membership.sites.push_back(plan.membership.sites.front());
    DCE_CHECK_EQ(dce::structural_check(plan).code(), dce::ErrorCode::already_exists);
  }
  {
    dce::EvolutionPlan plan = fleet.plan;
    plan.steps.front().depends_on.push_back(*dce::StepId::parse("step.does.not.exist"));
    DCE_CHECK_EQ(dce::structural_check(plan).code(), dce::ErrorCode::not_found);
  }
  {
    dce::EvolutionPlan plan = fleet.plan;
    plan.steps.front().depends_on.push_back(plan.steps.front().id);
    DCE_CHECK_EQ(dce::structural_check(plan).code(), dce::ErrorCode::invalid_argument);
  }
  {
    dce::EvolutionPlan plan = fleet.plan;
    plan.cohorts.front().max_parallel = 0;
    DCE_CHECK_EQ(dce::structural_check(plan).code(), dce::ErrorCode::invalid_argument);
  }
  {
    dce::EvolutionPlan plan = fleet.plan;
    plan.identity.id = dce::EvolutionPlanId{};
    DCE_CHECK_EQ(dce::structural_check(plan).code(), dce::ErrorCode::invalid_argument);
  }
  {
    dce::EvolutionPlan plan = fleet.plan;
    plan.steps.front().to = plan.steps.front().from;
    DCE_CHECK_EQ(dce::structural_check(plan).code(), dce::ErrorCode::invalid_argument);
  }
  {
    // A two-step dependency cycle must be found by a real traversal.
    dce::EvolutionPlan plan = fleet.plan;
    DCE_REQUIRE(plan.steps.size() >= 2);
    plan.steps[0].depends_on.clear();
    plan.steps[1].depends_on.clear();
    plan.steps[0].depends_on.push_back(plan.steps[1].id);
    plan.steps[1].depends_on.push_back(plan.steps[0].id);
    DCE_CHECK_EQ(dce::structural_check(plan).code(), dce::ErrorCode::invalid_argument);
  }
  {
    dce::EvolutionPlan plan = fleet.plan;
    for (std::size_t index = 0; index <= dce::kMaxPreconditions; ++index) {
      dce::Precondition precondition;
      precondition.name = "precondition." + std::to_string(index);
      precondition.description = "generated";
      precondition.gate = plan.gates.entries().front().id;
      plan.preconditions.push_back(precondition);
    }
    DCE_CHECK_EQ(dce::structural_check(plan).code(), dce::ErrorCode::limit_exceeded);
  }
}

DCE_TEST(plan, lookups_find_the_declared_object_and_report_absence) {
  dce::Result<dce::SyntheticFleet> built = small_fleet(7);
  DCE_REQUIRE_OK(built);
  dce::SyntheticFleet fleet = std::move(*built);
  const dce::EvolutionPlan& plan = fleet.plan;

  DCE_REQUIRE(!plan.steps.empty());
  DCE_REQUIRE(dce::find_step(plan, plan.steps.front().id) != nullptr);
  DCE_CHECK_EQ(dce::find_step(plan, plan.steps.front().id)->id.str(), plan.steps.front().id.str());
  DCE_CHECK_TRUE(dce::find_step(plan, *dce::StepId::parse("step.absent")) == nullptr);

  DCE_REQUIRE(!plan.cohorts.empty());
  DCE_CHECK_TRUE(dce::find_cohort(plan, plan.cohorts.front().id) != nullptr);
  DCE_CHECK_TRUE(dce::find_cohort(plan, *dce::CohortId::parse("cohort.absent")) == nullptr);

  DCE_REQUIRE(!plan.membership.sites.empty());
  DCE_CHECK_TRUE(dce::find_site(plan, plan.membership.sites.front().id) != nullptr);
  DCE_CHECK_TRUE(dce::find_site(plan, *dce::SiteId::parse("site.absent")) == nullptr);
}

DCE_TEST(plan, the_persisted_form_is_lossless) {
  dce::Result<dce::SyntheticFleet> built = small_fleet(8);
  DCE_REQUIRE_OK(built);
  dce::SyntheticFleet fleet = std::move(*built);
  dce::CanonicalWriter writer;
  DCE_REQUIRE_OK(dce::codec::put(writer, fleet.plan));

  dce::CanonicalReader reader(writer.bytes());
  dce::Result<dce::EvolutionPlan> decoded = dce::codec::get<dce::EvolutionPlan>(reader);
  DCE_REQUIRE_OK(decoded);
  DCE_CHECK_OK(reader.expect_end());

  // The digest is derived, so it must come back zero and then recompute to the
  // same value from the decoded content alone.
  DCE_CHECK_TRUE(decoded->digest.is_zero());
  DCE_REQUIRE_OK(dce::canonicalize(*decoded));
  DCE_CHECK_TRUE(decoded->digest == fleet.plan.digest);

  DCE_CHECK_EQ(decoded->steps.size(), fleet.plan.steps.size());
  DCE_CHECK_EQ(decoded->cohorts.size(), fleet.plan.cohorts.size());
  DCE_CHECK_EQ(decoded->membership.sites.size(), fleet.plan.membership.sites.size());
  DCE_CHECK_EQ(decoded->evidence.size(), fleet.plan.evidence.size());
  DCE_CHECK_EQ(decoded->gates.size(), fleet.plan.gates.size());
  DCE_CHECK_TRUE(decoded->source_version == fleet.plan.source_version);
  DCE_CHECK_TRUE(decoded->target_version == fleet.plan.target_version);
  DCE_CHECK_TRUE(decoded->policy == fleet.plan.policy);
  DCE_CHECK_TRUE(decoded->rollback == fleet.plan.rollback);
  DCE_CHECK_TRUE(decoded->point_of_no_return.has_value() ==
                 fleet.plan.point_of_no_return.has_value());
  for (std::size_t index = 0; index < fleet.plan.membership.sites.size(); ++index) {
    const dce::SiteRecord& expected = fleet.plan.membership.sites[index];
    const dce::SiteRecord& actual = decoded->membership.sites[index];
    DCE_CHECK_TRUE(actual.id == expected.id);
    DCE_CHECK_TRUE(actual.components == expected.components);
    DCE_CHECK_TRUE(actual.capabilities == expected.capabilities);
    DCE_CHECK_TRUE(actual.state_digest == expected.state_digest);
  }
  // Re-encoding the decoded plan must produce exactly the same bytes.
  dce::CanonicalWriter again;
  DCE_REQUIRE_OK(dce::codec::put(again, *decoded));
  DCE_CHECK_EQ(again.size(), writer.size());
  DCE_CHECK_TRUE(again.digest() == writer.digest());
}
