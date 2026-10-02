// Seeded randomized tests.
//
// These assert the properties the boundary is required to hold for every fleet
// and every interleaving, not just for the examples in the other suites. Each
// case runs many seeded iterations and prints the seed on failure, so a failure
// is reproducible from the message alone.
#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "dce/authority.hpp"
#include "dce/lifecycle.hpp"
#include "dce/random.hpp"
#include "dce/reconcile.hpp"
#include "dce/rollout.hpp"
#include "dce/synthetic.hpp"
#include "dce/validate.hpp"
#include "harness.hpp"

namespace {

constexpr std::uint64_t kSeeds[] = {1, 2, 3, 5, 8, 13, 21, 34, 55, 89};

std::vector<dce::SiteObservation> observations_from(const dce::EvolutionPlan& plan) {
  std::vector<dce::SiteObservation> observations;
  for (const dce::SiteRecord& record : plan.membership.sites) {
    dce::SiteObservation observation;
    observation.site = record.id;
    observation.dccp_version = record.dccp_version;
    observation.components = record.components;
    observation.accepted_generation = record.accepted_generation;
    observation.capabilities = record.capabilities;
    observation.policy = record.policy;
    observation.state_digest = record.state_digest;
    observation.delegated_rollout_authority = record.delegated_rollout_authority;
    observations.push_back(std::move(observation));
  }
  return observations;
}

struct Generated {
  dce::EvolutionPlan plan;
  std::vector<dce::SiteObservation> observations;
  dce::RolloutState rollout;
};

bool generate(Generated& out, std::uint64_t seed, std::size_t sites, std::size_t components,
              std::size_t cohorts) {
  dce::SyntheticFleetOptions options;
  options.sites = sites;
  options.components = components;
  options.cohorts = cohorts;
  options.seed = seed;
  dce::Result<dce::SyntheticFleet> fleet = dce::make_synthetic_fleet(options);
  if (!fleet.ok()) {
    return false;
  }
  out.plan = fleet->plan;
  out.observations = fleet->initial_observations;
  dce::Result<dce::RolloutState> rollout = dce::initial_rollout_state(out.plan);
  if (!rollout.ok()) {
    return false;
  }
  out.rollout = std::move(*rollout);
  return true;
}

}  // namespace

DCE_TEST(property, a_valid_synthetic_fleet_is_always_accepted) {
  for (const std::uint64_t seed : kSeeds) {
    Generated generated;
    DCE_REQUIRE(generate(generated, seed, 8, 3, 3));
    dce::ValidationContext context;
    context.epoch = generated.plan.identity.epoch;
    context.current_generation = generated.plan.identity.generation;
    context.observed_sites = generated.observations;
    const dce::Result<dce::ValidationReport> report =
        dce::validate_plan(generated.plan, context);
    DCE_REQUIRE_OK(report);
    if (!report->accepted()) {
      DCE_FAIL("seed " + std::to_string(seed) + " was refused: " +
               std::string(dce::to_string(report->refusals.front().code)) + ": " +
               report->refusals.front().explanation);
    }
  }
}

DCE_TEST(property, sites_never_skip_a_stage_and_never_exceed_the_target) {
  for (const std::uint64_t seed : kSeeds) {
    Generated generated;
    DCE_REQUIRE(generate(generated, seed, 6, 2, 2));
    const std::size_t total = dce::stage_count(generated.plan);
    DCE_REQUIRE(total > 0);

    std::uint64_t decisions = 0;
    while (decisions < 10000) {
      const dce::Result<dce::RolloutAction> action =
          dce::next_rollout_action(generated.plan, generated.rollout);
      DCE_REQUIRE_OK(action);
      if (action->kind == dce::RolloutActionKind::completed) {
        break;
      }
      if (action->kind == dce::RolloutActionKind::advance_wave) {
        // Advancing the cursor is part of the progression, never a stall.
        generated.rollout.current_wave = action->wave;
        ++decisions;
        continue;
      }
      if (action->kind != dce::RolloutActionKind::advance_site) {
        DCE_FAIL("seed " + std::to_string(seed) + " stalled with action " +
                 std::string(dce::to_string(action->kind)) + ": " + action->explanation);
        return;
      }
      // The site exists, is not in flight, and advances by exactly one stage.
      bool found = false;
      for (dce::SiteProgress& progress : generated.rollout.sites) {
        if (!(progress.site == action->site)) {
          continue;
        }
        found = true;
        DCE_CHECK_EQ(action->target_stage.value(), progress.accepted_stage.value() + 1);
        DCE_CHECK_TRUE(action->target_stage.value() <= total);
        DCE_CHECK_TRUE(!progress.in_flight);
        progress.accepted_stage = action->target_stage;
        progress.stage_digest = dce::Digest256::of(action->site.str() + ":" +
                                                  std::to_string(progress.accepted_stage.value()));
        progress.in_flight = false;
      }
      DCE_CHECK_TRUE(found);
      if (!found) {
        return;
      }
      ++decisions;
    }
    DCE_CHECK_TRUE(dce::rollout_complete(generated.plan, generated.rollout));
    // A completed plan reaches its target: every site sits exactly at the
    // number of steps, never beyond it.
    for (const dce::SiteProgress& progress : generated.rollout.sites) {
      DCE_CHECK_EQ(progress.accepted_stage.value(), total);
    }
  }
}

DCE_TEST(property, cohort_waves_never_go_backwards_and_progression_is_deterministic) {
  for (const std::uint64_t seed : kSeeds) {
    Generated first;
    Generated second;
    DCE_REQUIRE(generate(first, seed, 6, 2, 3));
    DCE_REQUIRE(generate(second, seed, 6, 2, 3));

    dce::CohortWave highest;
    std::vector<std::string> first_trace;
    std::vector<std::string> second_trace;
    for (std::size_t step = 0; step < 2000; ++step) {
      const dce::Result<dce::RolloutAction> a =
          dce::next_rollout_action(first.plan, first.rollout);
      const dce::Result<dce::RolloutAction> b =
          dce::next_rollout_action(second.plan, second.rollout);
      DCE_REQUIRE_OK(a);
      DCE_REQUIRE_OK(b);
      // Two independent runs of the same input produce identical decisions.
      DCE_CHECK_EQ(std::string(dce::to_string(a->kind)), std::string(dce::to_string(b->kind)));
      DCE_CHECK_TRUE(a->site == b->site);
      DCE_CHECK_EQ(a->target_stage.value(), b->target_stage.value());
      first_trace.push_back(std::string(dce::to_string(a->kind)) + ":" + a->site.str());
      second_trace.push_back(std::string(dce::to_string(b->kind)) + ":" + b->site.str());
      if (a->kind == dce::RolloutActionKind::completed) {
        break;
      }
      if (a->kind == dce::RolloutActionKind::advance_wave) {
        DCE_CHECK_EQ(a->wave.value(), b->wave.value());
        first.rollout.current_wave = a->wave;
        second.rollout.current_wave = b->wave;
        continue;
      }
      if (a->kind != dce::RolloutActionKind::advance_site) {
        DCE_FAIL("seed " + std::to_string(seed) + " stalled: " + a->explanation);
        return;
      }
      // The wave of the site being advanced never regresses.
      for (dce::SiteProgress& progress : first.rollout.sites) {
        if (progress.site == a->site) {
          DCE_CHECK_TRUE(!(progress.wave < highest));
          highest = progress.wave;
          progress.accepted_stage = a->target_stage;
        }
      }
      for (dce::SiteProgress& progress : second.rollout.sites) {
        if (progress.site == b->site) {
          progress.accepted_stage = b->target_stage;
        }
      }
    }
    DCE_CHECK_TRUE(first_trace == second_trace);
  }
}

DCE_TEST(property, a_partitioned_site_is_never_advanced_and_never_skipped) {
  for (const std::uint64_t seed : kSeeds) {
    Generated generated;
    DCE_REQUIRE(generate(generated, seed, 5, 2, 2));
    // Partition exactly one site and freeze the rest at their current stage.
    const dce::SiteId partitioned = generated.rollout.sites.front().site;
    for (dce::SiteProgress& progress : generated.rollout.sites) {
      progress.partitioned = progress.site == partitioned;
    }
    for (std::size_t step = 0; step < 500; ++step) {
      const dce::Result<dce::RolloutAction> action =
          dce::next_rollout_action(generated.plan, generated.rollout);
      DCE_REQUIRE_OK(action);
      if (action->kind == dce::RolloutActionKind::completed) {
        break;
      }
      if (action->kind == dce::RolloutActionKind::advance_wave) {
        generated.rollout.current_wave = action->wave;
        continue;
      }
      if (action->kind == dce::RolloutActionKind::gate_blocked) {
        // The only thing that may block progress is the partition itself, and
        // only within the lowest wave that is still incomplete.
        std::uint32_t lowest = 0xffffffffu;
        const std::size_t total = dce::stage_count(generated.plan);
        for (const dce::SiteProgress& progress : generated.rollout.sites) {
          if (progress.accepted_stage.value() < total && progress.wave.value() < lowest) {
            lowest = progress.wave.value();
          }
        }
        for (const dce::SiteProgress& progress : generated.rollout.sites) {
          if (progress.accepted_stage.value() < total && progress.wave.value() == lowest) {
            DCE_CHECK_TRUE(progress.partitioned);
          }
        }
        break;
      }
      DCE_REQUIRE(action->kind == dce::RolloutActionKind::advance_site);
      // A partitioned site is never the one advanced.
      DCE_CHECK_TRUE(!(action->site == partitioned));
      for (dce::SiteProgress& progress : generated.rollout.sites) {
        if (progress.site == action->site) {
          progress.accepted_stage = action->target_stage;
        }
      }
    }
    // The partitioned site's stage is exactly what it started at.
    for (const dce::SiteProgress& progress : generated.rollout.sites) {
      if (progress.site == partitioned) {
        DCE_CHECK_EQ(progress.accepted_stage.value(), static_cast<std::uint32_t>(0));
      }
    }
  }
}

DCE_TEST(property, migration_acceptance_is_never_duplicated) {
  for (const std::uint64_t seed : kSeeds) {
    Generated generated;
    DCE_REQUIRE(generate(generated, seed, 4, 2, 2));
    std::vector<dce::MigrationReceipt> accepted;
    std::uint64_t accepted_count = 0;

    dce::DeterministicRng rng(seed * 7919u + 13u);
    for (std::size_t round = 0; round < 200; ++round) {
      dce::MigrationReceipt receipt;
      receipt.id = *dce::ReceiptId::parse("receipt-" + std::to_string(round));
      receipt.plan = generated.plan.identity.id;
      receipt.plan_generation = generated.plan.identity.generation;
      receipt.epoch = generated.plan.identity.epoch;
      receipt.site = generated.plan.membership.sites[rng.uniform_below(
                                                         generated.plan.membership.sites.size())]
                         .id;
      const std::size_t step_index = static_cast<std::size_t>(
          rng.uniform_below(generated.plan.steps.size()));
      receipt.step = generated.plan.steps[step_index].id;
      receipt.stage = dce::StageOrdinal::from_value(static_cast<std::uint32_t>(step_index + 1));
      receipt.from = generated.plan.steps[step_index].from;
      receipt.to = generated.plan.steps[step_index].to;
      receipt.accepted_generation =
          dce::SiteGeneration::from_value(rng.uniform_below(8) + 1);
      receipt.evidence_digest = dce::Digest256::of(std::to_string(round));
      receipt.idempotency_key = "key-" + std::to_string(step_index) + "-" +
                                std::to_string(rng.uniform_below(3));

      const dce::Result<dce::ReceiptOutcome> outcome =
          dce::accept_receipt(receipt, generated.plan, accepted, generated.plan.identity.epoch,
                              generated.plan.identity.generation);
      DCE_REQUIRE_OK(outcome);
      // Acceptance is idempotent by key, so the comparison is against what was
      // already recorded BEFORE this outcome: an accepted outcome must not share
      // its key with anything, a duplicate must match exactly, and a conflict
      // must share the key but differ in content.
      std::size_t same_key = 0;
      bool identical = false;
      for (const dce::MigrationReceipt& existing : accepted) {
        if (existing.idempotency_key != receipt.idempotency_key) {
          continue;
        }
        ++same_key;
        if (existing.site == receipt.site && existing.step == receipt.step &&
            existing.stage == receipt.stage && existing.from == receipt.from &&
            existing.to == receipt.to &&
            existing.accepted_generation == receipt.accepted_generation &&
            existing.evidence_digest == receipt.evidence_digest) {
          identical = true;
        }
      }
      if (outcome->verdict == dce::ReceiptVerdict::accepted) {
        DCE_CHECK_EQ(same_key, static_cast<std::size_t>(0));
        ++accepted_count;
        accepted.push_back(receipt);
      } else if (outcome->verdict == dce::ReceiptVerdict::duplicate) {
        DCE_CHECK_EQ(same_key, static_cast<std::size_t>(1));
        DCE_CHECK_TRUE(identical);
      } else if (outcome->verdict == dce::ReceiptVerdict::conflicting) {
        DCE_CHECK_EQ(same_key, static_cast<std::size_t>(1));
        DCE_CHECK_TRUE(!identical);
      }
    }
    DCE_CHECK_EQ(accepted.size(), static_cast<std::size_t>(accepted_count));
    // Replaying every accepted receipt again changes nothing at all.
    const std::size_t size_before = accepted.size();
    for (const dce::MigrationReceipt& receipt : accepted) {
      const dce::Result<dce::ReceiptOutcome> replay =
          dce::accept_receipt(receipt, generated.plan, accepted,
                              generated.plan.identity.epoch, generated.plan.identity.generation);
      DCE_REQUIRE_OK(replay);
      DCE_CHECK_EQ(replay->verdict, dce::ReceiptVerdict::duplicate);
    }
    DCE_CHECK_EQ(accepted.size(), size_before);
  }
}

DCE_TEST(property, reconciliation_is_deterministic_and_generations_stay_monotonic) {
  for (const std::uint64_t seed : kSeeds) {
    Generated generated;
    DCE_REQUIRE(generate(generated, seed, 4, 2, 2));
    dce::DeterministicRng rng(seed * 104729u + 7u);

    dce::SiteProgress view = generated.rollout.sites.front();
    view.accepted_stage = dce::StageOrdinal::from_value(2);
    view.generation = dce::SiteGeneration::from_value(10);
    view.stage_digest = dce::Digest256::of("agreed");

    dce::SiteGeneration highest = view.generation;
    for (std::size_t round = 0; round < 500; ++round) {
      dce::SiteStageReport report;
      report.site = view.site;
      report.generation = dce::SiteGeneration::from_value(rng.uniform_below(20) + 1);
      report.epoch = generated.plan.identity.epoch;
      report.accepted_stage =
          dce::StageOrdinal::from_value(static_cast<std::uint32_t>(rng.uniform_below(6)));
      report.accepted_stage_digest =
          rng.chance(1, 2) ? dce::Digest256::of("agreed") : dce::Digest256::of("other");
      report.partitioned = rng.chance(1, 4);
      const bool delegated = rng.chance(1, 2);

      const dce::Result<dce::ReconciliationOutcome> outcome =
          dce::reconcile(view, report, generated.plan.identity.generation,
                         generated.plan.identity.epoch, delegated);
      DCE_REQUIRE_OK(outcome);
      // Determinism: the same inputs produce the same outcome.
      const dce::Result<dce::ReconciliationOutcome> repeated =
          dce::reconcile(view, report, generated.plan.identity.generation,
                         generated.plan.identity.epoch, delegated);
      DCE_REQUIRE_OK(repeated);
      DCE_CHECK_EQ(std::string(dce::to_string(outcome->action)),
                   std::string(dce::to_string(repeated->action)));
      DCE_CHECK_TRUE(outcome->evidence_digest == repeated->evidence_digest);

      if (outcome->action == dce::ReconciliationAction::reject) {
        // A rejection never advances the coordinator's belief.
        DCE_CHECK_TRUE(!(view.accepted_stage < outcome->agreed_stage));
        continue;
      }
      if (outcome->action == dce::ReconciliationAction::advance) {
        DCE_CHECK_TRUE(delegated);
        DCE_CHECK_TRUE(view.accepted_stage < outcome->agreed_stage);
      }
      if (outcome->action == dce::ReconciliationAction::rewind) {
        DCE_CHECK_TRUE(outcome->agreed_stage < view.accepted_stage);
      }
      // Generations never go backwards and are never reused.
      if (outcome->action != dce::ReconciliationAction::agree) {
        DCE_CHECK_TRUE(view.generation < outcome->next_generation);
      }
      DCE_CHECK_TRUE(!(outcome->next_generation < highest));
      highest = outcome->next_generation;
      view.accepted_stage = outcome->agreed_stage;
      view.generation = outcome->next_generation;
      view.stage_digest = report.accepted_stage_digest;
    }
  }
}

DCE_TEST(property, rollback_is_never_available_past_the_point_of_no_return) {
  for (const std::uint64_t seed : kSeeds) {
    Generated generated;
    DCE_REQUIRE(generate(generated, seed, 6, 2, 3));
    DCE_REQUIRE(generated.plan.point_of_no_return.has_value());
    const dce::CohortWave boundary = generated.plan.point_of_no_return->wave;
    const std::uint32_t waves = static_cast<std::uint32_t>(generated.plan.cohorts.size());

    for (std::uint32_t marker_wave = 0; marker_wave < waves; ++marker_wave) {
      for (std::uint32_t target = 0; target <= marker_wave; ++target) {
        dce::RollbackMarker marker;
        marker.plan = generated.plan.identity.id;
        marker.plan_generation = generated.plan.identity.generation;
        marker.epoch = generated.plan.identity.epoch;
        marker.wave = dce::CohortWave::from_value(marker_wave);
        marker.crossed_point_of_no_return = false;
        marker.justification = "seeded property sweep";
        const dce::RollbackEligibility eligibility = dce::assess_rollback_eligibility(
            generated.plan, marker, dce::CohortWave::from_value(target));
        if (marker_wave > boundary.value()) {
          DCE_CHECK_TRUE(!eligibility.eligible);
        }
        // Crossing the point of no return retires rollback entirely.
        dce::RollbackMarker crossed = marker;
        crossed.crossed_point_of_no_return = true;
        const dce::RollbackEligibility after = dce::assess_rollback_eligibility(
            generated.plan, crossed, dce::CohortWave::from_value(target));
        DCE_CHECK_TRUE(!after.eligible);
        DCE_CHECK_TRUE(!after.explanation.empty());
      }
    }
  }
}

DCE_TEST(property, impossible_plans_are_explained_rather_than_crashing) {
  for (const std::uint64_t seed : kSeeds) {
    Generated generated;
    DCE_REQUIRE(generate(generated, seed, 5, 2, 2));
    dce::DeterministicRng rng(seed * 31u + 17u);
    for (std::size_t attempt = 0; attempt < 50; ++attempt) {
      dce::EvolutionPlan plan = generated.plan;
      // Break something at random, then reseal so the failure under test is the
      // semantic one rather than a stale digest.
      switch (rng.uniform_below(6)) {
        case 0:
          if (!plan.steps.empty()) {
            plan.steps.erase(plan.steps.begin());
          }
          break;
        case 1:
          if (!plan.cohorts.empty()) {
            plan.cohorts.front().sites.clear();
          }
          break;
        case 2:
          plan.evidence = dce::EvidenceSet{};
          break;
        case 3:
          plan.compatibility = dce::CompatibilityGraph{};
          plan.compatibility.canonicalize();
          break;
        case 4:
          plan.rollback.permitted = false;
          break;
        default:
          plan.policy.minimum_ordinal = 1000000;
          break;
      }
      std::vector<dce::SiteObservation> observations = observations_from(plan);
      const dce::Result<dce::Digest256> digest = dce::compute_observation_digest(observations);
      DCE_REQUIRE_OK(digest);
      plan.source_state_digest = *digest;
      DCE_REQUIRE_OK(dce::canonicalize(plan));
      dce::ValidationContext context;
      context.epoch = plan.identity.epoch;
      context.current_generation = plan.identity.generation;
      context.observed_sites = observations;
      const dce::Result<dce::ValidationReport> report = dce::validate_plan(plan, context);
      DCE_REQUIRE_OK(report);
      if (!report->accepted()) {
        // Every refusal names its subject and explains itself.
        for (const dce::Refusal& refusal : report->refusals) {
          DCE_CHECK_TRUE(!refusal.explanation.empty());
        }
      }
    }
  }
}
