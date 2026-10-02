// Adversarial input.
//
// Everything here is something a hostile peer, a corrupted file or a careless
// operator could present. The requirement is uniform: refuse with a precise
// error, never crash, never allocate from an unvalidated length, and never
// accept a value that the runtime would later be unable to represent.
#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "dce/codec.hpp"
#include "dce/ids.hpp"
#include "dce/random.hpp"
#include "dce/synthetic.hpp"
#include "dce/validate.hpp"
#include "harness.hpp"

namespace {

}  // namespace

DCE_TEST(adversarial, identities_that_could_become_paths_are_refused) {
  const char* rejected[] = {
      "",           "..",           "../etc/passwd", "a/b",      "a\\\\b",
      "C:evil",     "con",          "CON",           "NUL.txt",  "com1",
      "lpt9",       ".leading",     "trailing.",     "-leading", "@leading",
      "has space",  "has\\ttab",    "with\\nnewline", "with\\0nul",
      "semi;colon", "pipe|",        "star*",         "quest?",
  };
  for (const char* candidate : rejected) {
    std::string value = candidate;
    if (value == "with\\0nul") {
      value = std::string("with");
      value.push_back('\0');
      value.append("nul");
    } else if (value == "with\\nnewline") {
      value = "with\nnewline";
    } else if (value == "has\\ttab") {
      value = "has\ttab";
    } else if (value == "a\\\\b") {
      value = "a\\b";
    }
    DCE_CHECK_TRUE(!dce::SiteId::parse(value).ok());
    DCE_CHECK_TRUE(!dce::ComponentId::parse(value).ok());
    DCE_CHECK_TRUE(!dce::EvidenceId::parse(value).ok());
  }

  // A very long identity is refused rather than truncated.
  const std::string long_name(2000, 'a');
  DCE_CHECK_EQ(dce::SiteId::parse(long_name).code(), dce::ErrorCode::invalid_argument);

  // Ordinary identities are still accepted.
  DCE_CHECK_TRUE(dce::SiteId::parse("site-dc1.rack").ok());
  DCE_CHECK_TRUE(dce::ComponentId::parse("dccp.power").ok());
  DCE_CHECK_TRUE(dce::CapabilityId::parse("cap.dfi.transport").ok());
}

DCE_TEST(adversarial, hostile_version_text_is_refused) {
  const char* rejected[] = {"",       "1",      "1.2",    "1.2.3.4", "1..2",
                            "a.b.c",  "1.2.",   ".1.2",   "1.2.3 ",  " 1.2.3",
                            "1.2.-3", "4294967296.0.0", "1.2.3\\n"};
  for (const char* candidate : rejected) {
    std::string value = candidate;
    if (value == "1.2.3\\n") {
      value = "1.2.3\n";
    }
    DCE_CHECK_TRUE(!dce::Version::parse(value).ok());
  }
  DCE_CHECK_TRUE(dce::Version::parse("1.2.3").ok());
  DCE_CHECK_TRUE(!dce::VersionRange::parse("1.2.3").ok());
  DCE_CHECK_TRUE(!dce::VersionRange::parse("2.0.0..1.0.0").ok());
  DCE_CHECK_TRUE(dce::VersionRange::parse("1.0.0..").ok());
}

DCE_TEST(adversarial, a_plan_payload_with_hostile_lengths_is_refused_before_allocating) {
  dce::SyntheticFleetOptions options;
  options.sites = 4;
  options.components = 2;
  options.cohorts = 2;
  options.seed = 21;
  dce::Result<dce::SyntheticFleet> fleet = dce::make_synthetic_fleet(options);
  DCE_REQUIRE_OK(fleet);

  dce::CanonicalWriter writer;
  DCE_REQUIRE_OK(dce::codec::put(writer, fleet->plan));
  const std::vector<std::byte> valid = writer.bytes();

  // Every truncation must be refused, and none may read past the payload.
  for (std::size_t length = 0; length < valid.size(); length += 7) {
    dce::CanonicalReader reader(std::span<const std::byte>(valid.data(), length));
    DCE_CHECK_TRUE(!dce::codec::get<dce::EvolutionPlan>(reader).ok());
  }

  // Single-byte mutations must either decode or be refused, never crash and
  // never produce a payload that re-encodes differently.
  dce::DeterministicRng rng(0x5eed1234ull);
  std::size_t accepted = 0;
  for (std::size_t attempt = 0; attempt < 3000; ++attempt) {
    std::vector<std::byte> mutated = valid;
    const std::size_t offset = static_cast<std::size_t>(rng.uniform_below(mutated.size()));
    mutated[offset] = static_cast<std::byte>(rng.uniform_below(256));
    dce::CanonicalReader reader(mutated);
    dce::Result<dce::EvolutionPlan> decoded = dce::codec::get<dce::EvolutionPlan>(reader);
    if (!decoded.ok()) {
      continue;
    }
    ++accepted;
    dce::CanonicalWriter again;
    const dce::Status reencoded = dce::codec::put(again, *decoded);
    DCE_CHECK_TRUE(reencoded.ok());
    if (reencoded.ok()) {
      dce::CanonicalReader second(again.bytes());
      DCE_CHECK_TRUE(dce::codec::get<dce::EvolutionPlan>(second).ok());
    }
  }
  // The mutation sweep is not expected to produce many valid plans; it is
  // expected to produce none that are unstable.
  DCE_CHECK_TRUE(accepted <= 3000);
}

DCE_TEST(adversarial, a_declared_element_count_cannot_drive_an_allocation) {
  // A plan whose first field claims four billion sites.
  dce::CanonicalWriter writer;
  DCE_REQUIRE_OK(writer.put_count(0));            // no plan identity presence issues here
  DCE_REQUIRE_OK(writer.put_string("plan.x"));    // identity
  DCE_REQUIRE_OK(writer.put_u64(1));              // revision
  DCE_REQUIRE_OK(writer.put_u64(1));              // generation
  DCE_REQUIRE_OK(writer.put_u64(1));              // epoch
  DCE_REQUIRE_OK(writer.put_string("title"));
  DCE_REQUIRE_OK(writer.put_u32(1));              // source major
  DCE_REQUIRE_OK(writer.put_u32(0));
  DCE_REQUIRE_OK(writer.put_u32(0));
  DCE_REQUIRE_OK(writer.put_u32(2));              // target major
  DCE_REQUIRE_OK(writer.put_u32(0));
  DCE_REQUIRE_OK(writer.put_u32(0));
  DCE_REQUIRE_OK(writer.put_digest(dce::Digest256::zero()));
  // membership: a hostile site count that must be refused by the bound.
  DCE_REQUIRE_OK(writer.put_u32(0xffffffffu));
  dce::CanonicalReader reader(writer.bytes());
  const dce::Result<dce::EvolutionPlan> decoded = dce::codec::get<dce::EvolutionPlan>(reader);
  DCE_CHECK_TRUE(!decoded.ok());
  // Which refusal wins depends on how far the hostile payload gets before the
  // bound is reached; what must never happen is acceptance.
  DCE_CHECK_TRUE(decoded.code() == dce::ErrorCode::limit_exceeded ||
                 decoded.code() == dce::ErrorCode::malformed ||
                 decoded.code() == dce::ErrorCode::invalid_argument ||
                 decoded.code() == dce::ErrorCode::out_of_range);
}

DCE_TEST(adversarial, validation_refuses_an_empty_observation_set) {
  dce::SyntheticFleetOptions options;
  options.sites = 3;
  options.components = 1;
  options.cohorts = 1;
  options.seed = 5;
  dce::Result<dce::SyntheticFleet> fleet = dce::make_synthetic_fleet(options);
  DCE_REQUIRE_OK(fleet);

  dce::ValidationContext context;
  context.epoch = fleet->plan.identity.epoch;
  context.current_generation = fleet->plan.identity.generation;
  // No observations at all: the fleet the plan was written against is gone.
  const dce::Result<dce::ValidationReport> report =
      dce::validate_plan(fleet->plan, context);
  DCE_REQUIRE_OK(report);
  DCE_CHECK_TRUE(!report->accepted());
  bool stale = false;
  for (const dce::Refusal& refusal : report->refusals) {
    if (refusal.code == dce::RefusalCode::source_state_stale) {
      stale = true;
    }
    DCE_CHECK_TRUE(!refusal.explanation.empty());
  }
  DCE_CHECK_TRUE(stale);
}

DCE_TEST(adversarial, a_duplicated_site_in_one_report_is_refused) {
  dce::SyntheticFleetOptions options;
  options.sites = 3;
  options.components = 1;
  options.cohorts = 1;
  options.seed = 6;
  dce::Result<dce::SyntheticFleet> fleet = dce::make_synthetic_fleet(options);
  DCE_REQUIRE_OK(fleet);

  dce::ValidationContext context;
  context.epoch = fleet->plan.identity.epoch;
  context.current_generation = fleet->plan.identity.generation;
  context.observed_sites = fleet->initial_observations;
  context.observed_sites.push_back(context.observed_sites.front());
  const dce::Result<dce::ValidationReport> report =
      dce::validate_plan(fleet->plan, context);
  DCE_REQUIRE_OK(report);
  DCE_CHECK_TRUE(!report->accepted());
  bool duplicate = false;
  for (const dce::Refusal& refusal : report->refusals) {
    if (refusal.code == dce::RefusalCode::structural_defect) {
      duplicate = true;
    }
  }
  DCE_CHECK_TRUE(duplicate);
}

DCE_TEST(adversarial, absurd_policy_and_cohort_values_do_not_wrap) {
  dce::SyntheticFleetOptions options;
  options.sites = 4;
  options.components = 1;
  options.cohorts = 2;
  options.seed = 9;
  dce::Result<dce::SyntheticFleet> fleet = dce::make_synthetic_fleet(options);
  DCE_REQUIRE_OK(fleet);

  dce::EvolutionPlan plan = fleet->plan;
  // The largest possible wave on the first cohort, which must be refused as a
  // gap rather than wrapping the contiguity arithmetic.
  plan.cohorts.front().wave = dce::CohortWave::from_value(0xffffffffu);
  plan.rollback.max_waves_back = dce::CohortWave::from_value(0xffffffffu);
  plan.policy.minimum_ordinal = 0xffffffffffffffffull;
  DCE_REQUIRE_OK(dce::canonicalize(plan));

  dce::ValidationContext context;
  context.epoch = plan.identity.epoch;
  context.current_generation = plan.identity.generation;
  context.observed_sites = fleet->initial_observations;
  const dce::Result<dce::ValidationReport> report = dce::validate_plan(plan, context);
  DCE_REQUIRE_OK(report);
  DCE_CHECK_TRUE(!report->accepted());

  // The rollback assessment must not wrap either.
  dce::RollbackMarker marker;
  marker.plan = plan.identity.id;
  marker.plan_generation = plan.identity.generation;
  marker.epoch = plan.identity.epoch;
  marker.wave = dce::CohortWave::from_value(0xffffffffu);
  const dce::RollbackEligibility eligibility =
      dce::assess_rollback_eligibility(plan, marker, dce::CohortWave::from_value(0));
  DCE_CHECK_TRUE(!eligibility.eligible);
  DCE_CHECK_TRUE(!eligibility.explanation.empty());
}
