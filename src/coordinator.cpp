#include "dce/node.hpp"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "dce/codec.hpp"
#include "dce/platform.hpp"
#include "dce/protocol.hpp"
#include "dce/store.hpp"
#include "dce/text.hpp"

namespace dce {
namespace {

using persist::LogEntry;
using persist::OpenMode;
using persist::RecordType;
using persist::Store;
using persist::StoreOptions;

constexpr std::uint16_t kStateImageVersion = 1;

// A reconciliation is recorded with its outcome so that a restart can replay
// the exact belief the coordinator held, including a rewind.
struct ReconciliationRecord {
  ReconciliationAction action{ReconciliationAction::hold};
  SiteId site;
  StageOrdinal stage;
  SiteGeneration generation;
  Digest256 evidence_digest;
  std::string explanation;
};

template <class T>
Status get_into(CanonicalReader& reader, T& out) {
  Result<T> value = codec::get<T>(reader);
  if (!value.ok()) {
    return value.error();
  }
  out = std::move(*value);
  return Status{};
}

Status put_observations(CanonicalWriter& writer,
                        const std::vector<SiteObservation>& observations) {
  if (observations.size() > kMaxSites) {
    return Error{ErrorCode::limit_exceeded, "observed fleet exceeds its documented bound"};
  }
  DCE_TRY(writer.put_count(observations.size()));
  for (const SiteObservation& observation : observations) {
    DCE_TRY(codec::put(writer, observation.site));
    DCE_TRY(codec::put(writer, observation.dccp_version));
    DCE_TRY(codec::put_list(writer, observation.components, kMaxComponents));
    DCE_TRY(codec::put(writer, observation.accepted_generation));
    DCE_TRY(codec::put(writer, observation.capabilities));
    DCE_TRY(writer.put_bool(observation.policy.has_value()));
    if (observation.policy.has_value()) {
      DCE_TRY(codec::put(writer, *observation.policy));
    }
    DCE_TRY(codec::put(writer, observation.state_digest));
    DCE_TRY(codec::put(writer, observation.delegated_rollout_authority));
  }
  return Status{};
}

Result<std::vector<SiteObservation>> get_observations(CanonicalReader& reader) {
  DCE_ASSIGN(count, reader.count(kMaxSites));
  std::vector<SiteObservation> observations;
  observations.reserve(count);
  for (std::size_t index = 0; index < count; ++index) {
    SiteObservation observation;
    DCE_TRY(get_into(reader, observation.site));
    DCE_TRY(get_into(reader, observation.dccp_version));
    DCE_ASSIGN(components, codec::get_list<ComponentVersion>(reader, kMaxComponents));
    observation.components = std::move(components);
    DCE_TRY(get_into(reader, observation.accepted_generation));
    DCE_TRY(get_into(reader, observation.capabilities));
    DCE_ASSIGN(has_policy, reader.boolean());
    if (has_policy) {
      PolicyGeneration policy;
      DCE_TRY(get_into(reader, policy));
      observation.policy = policy;
    }
    DCE_TRY(get_into(reader, observation.state_digest));
    DCE_ASSIGN(delegated, reader.boolean());
    observation.delegated_rollout_authority = delegated;
    observations.push_back(std::move(observation));
  }
  return observations;
}

Status put_reconciliation(CanonicalWriter& writer, const ReconciliationRecord& record) {
  DCE_TRY(writer.put_u16(static_cast<std::uint16_t>(record.action)));
  DCE_TRY(codec::put(writer, record.site));
  DCE_TRY(codec::put(writer, record.stage));
  DCE_TRY(codec::put(writer, record.generation));
  DCE_TRY(codec::put(writer, record.evidence_digest));
  DCE_TRY(writer.put_string(record.explanation));
  return Status{};
}

Result<ReconciliationRecord> get_reconciliation(CanonicalReader& reader) {
  ReconciliationRecord record;
  DCE_ASSIGN(action, reader.u16());
  if (action > static_cast<std::uint16_t>(ReconciliationAction::reject)) {
    return Error{ErrorCode::malformed, "reconciliation action out of range"};
  }
  record.action = static_cast<ReconciliationAction>(action);
  DCE_TRY(get_into(reader, record.site));
  DCE_TRY(get_into(reader, record.stage));
  DCE_TRY(get_into(reader, record.generation));
  DCE_TRY(get_into(reader, record.evidence_digest));
  DCE_ASSIGN(explanation, reader.string(kMaxTextBytes));
  record.explanation = std::move(explanation);
  return record;
}

// The full coordinator state, as written into a snapshot image.
struct StateImage {
  bool has_plan{false};
  EvolutionPlan plan;
  PlanState state{PlanState::draft};
  PlanGeneration plan_generation;
  RolloutState rollout;
  std::vector<SiteObservation> observations;
  std::vector<CapabilityDependencyProof> removal_proofs;
  std::vector<MigrationReceipt> receipts;
  std::vector<StageCheckpoint> checkpoints;
  std::vector<RollbackMarker> rollback_markers;
  std::vector<ReconciliationRecord> reconciliations;
  Digest256 observed_state_digest;
  std::uint64_t refusals{0};
  std::uint64_t submissions{0};
  bool point_of_no_return_crossed{false};
};

Status put_state_image(CanonicalWriter& writer, const StateImage& image) {
  DCE_TRY(writer.put_u16(kStateImageVersion));
  DCE_TRY(writer.put_bool(image.has_plan));
  if (image.has_plan) {
    DCE_TRY(codec::put(writer, image.plan));
  }
  DCE_TRY(writer.put_u16(static_cast<std::uint16_t>(image.state)));
  DCE_TRY(codec::put(writer, image.plan_generation));
  DCE_TRY(codec::put_list(writer, image.rollout.sites, kMaxSites));
  DCE_TRY(codec::put(writer, image.rollout.current_wave));
  DCE_TRY(writer.put_bool(image.rollout.paused));
  DCE_TRY(put_observations(writer, image.observations));
  DCE_TRY(codec::put_list(writer, image.removal_proofs, kMaxDependencies));
  DCE_TRY(codec::put_list(writer, image.receipts, kMaxReceipts));
  DCE_TRY(codec::put_list(writer, image.checkpoints, kMaxCheckpoints));
  DCE_TRY(codec::put_list(writer, image.rollback_markers, kMaxGraphNodes));
  if (image.reconciliations.size() > kMaxReconciliations) {
    return Error{ErrorCode::limit_exceeded, "reconciliation history exceeds its documented bound"};
  }
  DCE_TRY(writer.put_count(image.reconciliations.size()));
  for (const ReconciliationRecord& record : image.reconciliations) {
    DCE_TRY(put_reconciliation(writer, record));
  }
  DCE_TRY(codec::put(writer, image.observed_state_digest));
  DCE_TRY(writer.put_u64(image.refusals));
  DCE_TRY(writer.put_u64(image.submissions));
  DCE_TRY(writer.put_bool(image.point_of_no_return_crossed));
  return Status{};
}

Result<StateImage> get_state_image(CanonicalReader& reader) {
  StateImage image;
  DCE_ASSIGN(version, reader.u16());
  if (version != kStateImageVersion) {
    return Error{ErrorCode::unsupported, "snapshot image version is not supported"};
  }
  DCE_ASSIGN(has_plan, reader.boolean());
  image.has_plan = has_plan;
  if (image.has_plan) {
    DCE_TRY(get_into(reader, image.plan));
  }
  DCE_ASSIGN(state, reader.u16());
  if (state > static_cast<std::uint16_t>(PlanState::reconciled)) {
    return Error{ErrorCode::malformed, "plan state out of range in snapshot"};
  }
  image.state = static_cast<PlanState>(state);
  DCE_TRY(get_into(reader, image.plan_generation));
  DCE_ASSIGN(sites, codec::get_list<SiteProgress>(reader, kMaxSites));
  image.rollout.sites = std::move(sites);
  DCE_TRY(get_into(reader, image.rollout.current_wave));
  DCE_ASSIGN(paused, reader.boolean());
  image.rollout.paused = paused;
  DCE_ASSIGN(observations, get_observations(reader));
  image.observations = std::move(observations);
  DCE_ASSIGN(proofs, codec::get_list<CapabilityDependencyProof>(reader, kMaxDependencies));
  image.removal_proofs = std::move(proofs);
  DCE_ASSIGN(receipts, codec::get_list<MigrationReceipt>(reader, kMaxReceipts));
  image.receipts = std::move(receipts);
  DCE_ASSIGN(checkpoints, codec::get_list<StageCheckpoint>(reader, kMaxCheckpoints));
  image.checkpoints = std::move(checkpoints);
  DCE_ASSIGN(markers, codec::get_list<RollbackMarker>(reader, kMaxGraphNodes));
  image.rollback_markers = std::move(markers);
  DCE_ASSIGN(reconciliation_count, reader.count(kMaxReconciliations));
  image.reconciliations.reserve(reconciliation_count);
  for (std::size_t index = 0; index < reconciliation_count; ++index) {
    DCE_ASSIGN(record, get_reconciliation(reader));
    image.reconciliations.push_back(std::move(record));
  }
  DCE_TRY(get_into(reader, image.observed_state_digest));
  DCE_ASSIGN(refusals, reader.u64());
  image.refusals = refusals;
  DCE_ASSIGN(submissions, reader.u64());
  image.submissions = submissions;
  DCE_ASSIGN(crossed, reader.boolean());
  image.point_of_no_return_crossed = crossed;
  return image;
}

}  // namespace

// ---------------------------------------------------------------------------
// Coordinator
// ---------------------------------------------------------------------------
struct Coordinator::Impl {
  mutable std::mutex mutex_;
  CoordinatorOptions options;
  Store store;
  bool closed{false};

  bool has_plan{false};
  EvolutionPlan plan;
  PlanState state{PlanState::draft};
  PlanGeneration plan_generation;
  RolloutState rollout;
  std::vector<SiteObservation> observations;
  std::vector<CapabilityDependencyProof> removal_proofs;
  std::vector<MigrationReceipt> receipts;
  std::vector<StageCheckpoint> checkpoints;
  std::vector<RollbackMarker> rollback_markers;
  std::vector<ReconciliationRecord> reconciliations;
  Digest256 observed_state_digest;
  std::uint64_t refusals{0};
  std::uint64_t submissions{0};
  std::uint64_t records_since_snapshot{0};
  bool point_of_no_return_crossed{false};

  [[nodiscard]] SiteProgress* find_progress(const SiteId& site) {
    for (SiteProgress& progress : rollout.sites) {
      if (progress.site == site) {
        return &progress;
      }
    }
    return nullptr;
  }

  void rebuild_rollout() {
    rollout.sites.clear();
    rollout.paused = false;
    if (!has_plan) {
      rollout.current_wave = CohortWave{};
      return;
    }
    for (const SiteRecord& record : plan.membership.sites) {
      SiteProgress progress;
      progress.site = record.id;
      progress.generation = record.accepted_generation;
      for (const RolloutCohort& cohort : plan.cohorts) {
        for (const SiteId& member : cohort.sites) {
          if (member == record.id) {
            progress.cohort = cohort.id;
            progress.wave = cohort.wave;
          }
        }
      }
      rollout.sites.push_back(std::move(progress));
    }
    rollout.canonicalize();
    refresh_wave();
  }

  // A re-authored revision keeps the durable progress of sites it still
  // contains, so re-affirming a plan after a restart never rewinds a fleet.
  void merge_rollout(const RolloutState& previous) {
    RolloutState merged;
    for (const SiteRecord& record : plan.membership.sites) {
      SiteProgress progress;
      progress.site = record.id;
      progress.generation = record.accepted_generation;
      for (const RolloutCohort& cohort : plan.cohorts) {
        for (const SiteId& member : cohort.sites) {
          if (member == record.id) {
            progress.cohort = cohort.id;
            progress.wave = cohort.wave;
          }
        }
      }
      for (const SiteProgress& prior : previous.sites) {
        if (prior.site == record.id) {
          progress.accepted_stage = prior.accepted_stage;
          progress.stage_digest = prior.stage_digest;
          progress.in_flight = false;
        }
      }
      merged.sites.push_back(std::move(progress));
    }
    merged.canonicalize();
    merged.paused = previous.paused;
    rollout = std::move(merged);
    refresh_wave();
  }

  void refresh_wave() {
    if (!has_plan) {
      rollout.current_wave = CohortWave{};
      return;
    }
    const std::size_t total = stage_count(plan);
    CohortWave lowest;
    bool found = false;
    for (const SiteProgress& progress : rollout.sites) {
      if (progress.accepted_stage.value() < total) {
        if (!found || progress.wave < lowest) {
          lowest = progress.wave;
          found = true;
        }
      }
    }
    if (!found && !plan.cohorts.empty()) {
      lowest = plan.cohorts.back().wave;
    }
    rollout.current_wave = lowest;
  }

  [[nodiscard]] Status commit_state(PlanState next) {
    CanonicalWriter writer;
    DCE_TRY(writer.put_u16(static_cast<std::uint16_t>(next)));
    DCE_TRY(codec::put(writer, plan_generation));
    DCE_TRY(codec::put(writer, store.epoch()));
    return append(RecordType::plan_state_change, writer);
  }

  [[nodiscard]] Status append(RecordType type, const CanonicalWriter& writer) {
    Result<persist::CommitReceipt> receipt = store.append(type, writer.bytes());
    if (!receipt.ok()) {
      return receipt.error();
    }
    ++records_since_snapshot;
    if (options.snapshot_every_records != 0 &&
        records_since_snapshot >= options.snapshot_every_records) {
      DCE_TRY(write_snapshot());
    }
    return Status{};
  }

  [[nodiscard]] Status write_snapshot() {
    StateImage image;
    image.has_plan = has_plan;
    image.plan = plan;
    image.state = state;
    image.plan_generation = plan_generation;
    image.rollout = rollout;
    image.observations = observations;
    image.removal_proofs = removal_proofs;
    image.receipts = receipts;
    image.checkpoints = checkpoints;
    image.rollback_markers = rollback_markers;
    image.reconciliations = reconciliations;
    image.observed_state_digest = observed_state_digest;
    image.refusals = refusals;
    image.submissions = submissions;
    image.point_of_no_return_crossed = point_of_no_return_crossed;

    CanonicalWriter writer(persist::kMaxSnapshotBytes);
    DCE_TRY(put_state_image(writer, image));
    DCE_TRY(store.snapshot(writer.bytes()));
    records_since_snapshot = 0;
    return Status{};
  }

  void apply_image(StateImage image) {
    has_plan = image.has_plan;
    plan = std::move(image.plan);
    state = image.state;
    plan_generation = image.plan_generation;
    rollout = std::move(image.rollout);
    observations = std::move(image.observations);
    removal_proofs = std::move(image.removal_proofs);
    receipts = std::move(image.receipts);
    checkpoints = std::move(image.checkpoints);
    rollback_markers = std::move(image.rollback_markers);
    reconciliations = std::move(image.reconciliations);
    observed_state_digest = image.observed_state_digest;
    refusals = image.refusals;
    submissions = image.submissions;
    point_of_no_return_crossed = image.point_of_no_return_crossed;
  }

  [[nodiscard]] Status apply_record(const LogEntry& entry) {
    CanonicalReader reader(entry.payload);
    switch (entry.type) {
      case RecordType::plan_revision: {
        DCE_TRY(get_into(reader, plan));
        DCE_TRY(reader.expect_end());
        // The plan digest and the membership digest are derived and therefore
        // not stored, so recovering a revision means recomputing both from the
        // content that was recovered. A recovered plan that carried no digest
        // would fail every later validation for the wrong reason.
        DCE_TRY(canonicalize(plan));
        has_plan = true;
        plan_generation = plan.identity.generation;
        return Status{};
      }
      case RecordType::plan_state_change: {
        DCE_ASSIGN(state_code, reader.u16());
        if (state_code > static_cast<std::uint16_t>(PlanState::reconciled)) {
          return Error{ErrorCode::corruption, "plan state record carries an out-of-range state"};
        }
        state = static_cast<PlanState>(state_code);
        DCE_TRY(get_into(reader, plan_generation));
        CoordinatorEpoch recorded_epoch;
        DCE_TRY(get_into(reader, recorded_epoch));
        DCE_TRY(reader.expect_end());
        rollout.paused = state == PlanState::paused;
        return Status{};
      }
      case RecordType::site_observation: {
        DCE_ASSIGN(restored, get_observations(reader));
        DCE_TRY(reader.expect_end());
        observations = std::move(restored);
        Result<Digest256> digest = compute_observation_digest(observations);
        if (!digest.ok()) {
          return digest.error();
        }
        observed_state_digest = *digest;
        return Status{};
      }
      case RecordType::migration_receipt: {
        MigrationReceipt receipt;
        DCE_TRY(get_into(reader, receipt));
        DCE_TRY(reader.expect_end());
        for (const MigrationReceipt& existing : receipts) {
          if (existing.id == receipt.id) {
            return Status{};  // replaying a receipt changes nothing
          }
        }
        receipts.push_back(receipt);
        if (SiteProgress* progress = find_progress(receipt.site); progress != nullptr) {
          if (progress->accepted_stage < receipt.stage) {
            progress->accepted_stage = receipt.stage;
          }
          progress->generation = receipt.accepted_generation;
          progress->in_flight = false;
        }
        return Status{};
      }
      case RecordType::stage_checkpoint: {
        StageCheckpoint checkpoint;
        DCE_TRY(get_into(reader, checkpoint));
        DCE_TRY(reader.expect_end());
        checkpoints.push_back(checkpoint);
        return Status{};
      }
      case RecordType::rollback_marker: {
        RollbackMarker marker;
        DCE_TRY(get_into(reader, marker));
        DCE_TRY(reader.expect_end());
        if (marker.crossed_point_of_no_return) {
          point_of_no_return_crossed = true;
        }
        rollback_markers.push_back(marker);
        return Status{};
      }
      case RecordType::reconciliation_record: {
        DCE_ASSIGN(record, get_reconciliation(reader));
        DCE_TRY(reader.expect_end());
        if (SiteProgress* progress = find_progress(record.site); progress != nullptr) {
          progress->accepted_stage = record.stage;
          progress->generation = record.generation;
          progress->in_flight = false;
        }
        reconciliations.push_back(std::move(record));
        return Status{};
      }
      case RecordType::capability_dependency_proof: {
        CapabilityDependencyProof proof;
        DCE_TRY(get_into(reader, proof));
        DCE_TRY(reader.expect_end());
        removal_proofs.push_back(std::move(proof));
        return Status{};
      }
      case RecordType::epoch_bump:
        return Status{};  // the manifest owns the epoch; nothing else to restore
      case RecordType::membership_snapshot:
      case RecordType::validation_report:
      case RecordType::exception_grant:
        return Error{ErrorCode::unsupported,
                     std::string("the coordinator does not write ") + persist::to_string(entry.type) +
                         " records, so one found in the log came from an incompatible writer"};
    }
    return Error{ErrorCode::corruption, "unknown durable record type"};
  }

  [[nodiscard]] Status recover() {
    Result<persist::RecoveredState> recovered = store.recover();
    if (!recovered.ok()) {
      return recovered.error();
    }
    if (!recovered->snapshot.empty()) {
      CanonicalReader reader(recovered->snapshot);
      DCE_ASSIGN(image, get_state_image(reader));
      DCE_TRY(reader.expect_end());
      apply_image(std::move(image));
      if (has_plan) {
        // Snapshots carry content, never derived digests, for the same reason
        // log records do not.
        DCE_TRY(canonicalize(plan));
      }
    }
    for (const LogEntry& entry : recovered->records) {
      DCE_TRY(apply_record(entry));
    }
    if (has_plan) {
      refresh_wave();
    }
    return Status{};
  }
};

Coordinator::~Coordinator() = default;

Result<std::unique_ptr<Coordinator>> Coordinator::open(const CoordinatorOptions& options) {
  if (options.store_directory.empty()) {
    return Error{ErrorCode::invalid_argument, "a coordinator needs a store directory"};
  }
  if (options.max_worker_threads == 0) {
    return Error{ErrorCode::invalid_argument, "a coordinator needs at least one worker thread"};
  }
  auto state = std::make_unique<Impl>();
  state->options = options;
  StoreOptions store_options;
  store_options.sync_on_commit = options.sync_on_commit;
  Result<Store> store =
      Store::open(options.store_directory, store_options, OpenMode::read_write);
  if (!store.ok()) {
    return store.error();
  }
  state->store = std::move(*store);
  const Status recovered = state->recover();
  if (!recovered.ok()) {
    return recovered.error();
  }
  auto coordinator = std::unique_ptr<Coordinator>(new Coordinator());
  coordinator->impl_ = std::move(state);
  return coordinator;
}

Result<PlanGeneration> Coordinator::submit_plan(EvolutionPlan plan) {
  if (impl_ == nullptr) {
    return Error{ErrorCode::closed, "the coordinator is not open"};
  }
  std::lock_guard<std::mutex> guard(impl_->mutex_);
  if (impl_->closed) {
    return Error{ErrorCode::closed, "the coordinator is closed"};
  }
  if (!plan.identity.id.valid()) {
    return Error{ErrorCode::invalid_argument, "an evolution plan needs a stable identity"};
  }
  if (impl_->submissions >= impl_->options.max_submissions) {
    return Error{ErrorCode::limit_exceeded, "the coordinator has reached its submission bound"};
  }
  DCE_TRY(structural_check(plan));

  const bool same_plan = impl_->has_plan && impl_->plan.identity.id == plan.identity.id;
  PlanGeneration next_generation{1};
  if (same_plan) {
    Result<PlanGeneration> advanced = impl_->plan_generation.next();
    if (!advanced.ok()) {
      return advanced.error();
    }
    next_generation = *advanced;
    Result<PlanRevision> revision = impl_->plan.identity.revision.next();
    plan.identity.revision = revision.ok() ? *revision : PlanRevision{1};
  } else {
    plan.identity.revision = PlanRevision{1};
  }
  plan.identity.generation = next_generation;
  plan.identity.epoch = impl_->store.epoch();
  if (plan.source_state_digest.is_zero()) {
    Result<Digest256> digest = compute_observation_digest(impl_->observations);
    if (!digest.ok()) {
      return digest.error();
    }
    plan.source_state_digest = *digest;
  }
  const Status canonical = canonicalize(plan);
  if (!canonical.ok()) {
    return canonical.error();
  }

  CanonicalWriter writer;
  DCE_TRY(codec::put(writer, plan));
  DCE_TRY(impl_->append(RecordType::plan_revision, writer));

  const RolloutState previous = impl_->rollout;
  impl_->plan = std::move(plan);
  impl_->has_plan = true;
  impl_->plan_generation = next_generation;
  impl_->state = PlanState::draft;
  ++impl_->submissions;
  if (same_plan) {
    impl_->merge_rollout(previous);
  } else {
    impl_->rebuild_rollout();
  }
  DCE_TRY(impl_->commit_state(PlanState::draft));
  return next_generation;
}

Result<ValidationReport> Coordinator::validate(const ValidationContext& context) {
  if (impl_ == nullptr) {
    return Error{ErrorCode::closed, "the coordinator is not open"};
  }
  std::lock_guard<std::mutex> guard(impl_->mutex_);
  if (impl_->closed) {
    return Error{ErrorCode::closed, "the coordinator is closed"};
  }
  if (!impl_->has_plan) {
    return Error{ErrorCode::not_found, "no plan has been submitted"};
  }
  ValidationContext effective = context;
  effective.epoch = impl_->store.epoch();
  effective.current_generation = impl_->plan_generation;
  if (effective.observed_sites.empty()) {
    effective.observed_sites = impl_->observations;
  }
  if (effective.removal_proofs.empty()) {
    effective.removal_proofs = impl_->removal_proofs;
  }
  return validate_plan(impl_->plan, effective);
}

Status Coordinator::record_observation(std::vector<SiteObservation> observations) {
  if (impl_ == nullptr) {
    return Error{ErrorCode::closed, "the coordinator is not open"};
  }
  std::lock_guard<std::mutex> guard(impl_->mutex_);
  if (impl_->closed) {
    return Error{ErrorCode::closed, "the coordinator is closed"};
  }
  if (observations.size() > kMaxSites) {
    return Error{ErrorCode::limit_exceeded, "observed fleet exceeds its documented bound"};
  }
  std::sort(observations.begin(), observations.end(),
            [](const SiteObservation& left, const SiteObservation& right) {
              return left.site < right.site;
            });
  for (std::size_t index = 1; index < observations.size(); ++index) {
    if (observations[index - 1].site == observations[index].site) {
      return Error{ErrorCode::conflict, "the same site was observed twice in one report"};
    }
  }
  Result<Digest256> digest = compute_observation_digest(observations);
  if (!digest.ok()) {
    return digest.error();
  }
  CanonicalWriter writer;
  DCE_TRY(put_observations(writer, observations));
  DCE_TRY(impl_->append(RecordType::site_observation, writer));
  impl_->observations = std::move(observations);
  impl_->observed_state_digest = *digest;
  return Status{};
}

Status Coordinator::observe_site(SiteObservation observation) {
  if (impl_ == nullptr) {
    return Error{ErrorCode::closed, "the coordinator is not open"};
  }
  std::lock_guard<std::mutex> guard(impl_->mutex_);
  if (impl_->closed) {
    return Error{ErrorCode::closed, "the coordinator is closed"};
  }
  std::vector<SiteObservation> merged = impl_->observations;
  bool replaced = false;
  for (SiteObservation& existing : merged) {
    if (existing.site == observation.site) {
      existing = observation;
      replaced = true;
      break;
    }
  }
  if (!replaced) {
    if (merged.size() >= kMaxSites) {
      return Error{ErrorCode::limit_exceeded, "observed fleet exceeds its documented bound"};
    }
    merged.push_back(std::move(observation));
  }
  std::sort(merged.begin(), merged.end(),
            [](const SiteObservation& left, const SiteObservation& right) {
              return left.site < right.site;
            });
  Result<Digest256> digest = compute_observation_digest(merged);
  if (!digest.ok()) {
    return digest.error();
  }
  CanonicalWriter writer;
  DCE_TRY(put_observations(writer, merged));
  DCE_TRY(impl_->append(RecordType::site_observation, writer));
  impl_->observations = std::move(merged);
  impl_->observed_state_digest = *digest;
  return Status{};
}

Result<PlanState> Coordinator::apply(LifecycleEvent event, const TransitionContext& context) {
  if (impl_ == nullptr) {
    return Error{ErrorCode::closed, "the coordinator is not open"};
  }
  std::lock_guard<std::mutex> guard(impl_->mutex_);
  if (impl_->closed) {
    return Error{ErrorCode::closed, "the coordinator is closed"};
  }
  if (!impl_->has_plan) {
    return Error{ErrorCode::not_found, "no plan has been submitted"};
  }
  TransitionContext effective = context;
  if (effective.point_of_no_return_crossed != impl_->point_of_no_return_crossed) {
    effective.point_of_no_return_crossed = impl_->point_of_no_return_crossed;
  }
  Result<TransitionOutcome> outcome = transition(impl_->state, event, effective);
  if (!outcome.ok()) {
    ++impl_->refusals;
    return outcome.error();
  }
  if (outcome->state == PlanState::rolling_out || outcome->state == PlanState::paused) {
    // Validate before publishing: an event that reaches a new state must have
    // its record durable first.
  }
  DCE_TRY(impl_->commit_state(outcome->state));
  impl_->state = outcome->state;
  impl_->rollout.paused = outcome->state == PlanState::paused;
  return outcome->state;
}

Result<RolloutAction> Coordinator::next_action() {
  if (impl_ == nullptr) {
    return Error{ErrorCode::closed, "the coordinator is not open"};
  }
  std::lock_guard<std::mutex> guard(impl_->mutex_);
  if (!impl_->has_plan) {
    return Error{ErrorCode::not_found, "no plan has been submitted"};
  }
  RolloutState view = impl_->rollout;
  view.paused = view.paused || impl_->state == PlanState::paused ||
                is_terminal(impl_->state) || impl_->state == PlanState::draft ||
                impl_->state == PlanState::validated || impl_->state == PlanState::staged;
  return next_rollout_action(impl_->plan, view);
}

Result<ReceiptOutcome> Coordinator::accept_migration(const MigrationReceipt& receipt) {
  if (impl_ == nullptr) {
    return Error{ErrorCode::closed, "the coordinator is not open"};
  }
  std::lock_guard<std::mutex> guard(impl_->mutex_);
  if (impl_->closed) {
    return Error{ErrorCode::closed, "the coordinator is closed"};
  }
  if (!impl_->has_plan) {
    return Error{ErrorCode::not_found, "no plan has been submitted"};
  }
  Result<ReceiptOutcome> outcome =
      accept_receipt(receipt, impl_->plan, impl_->receipts, impl_->store.epoch(),
                     impl_->plan_generation);
  if (!outcome.ok()) {
    ++impl_->refusals;
    return outcome.error();
  }
  if (outcome->verdict != ReceiptVerdict::accepted) {
    if (outcome->verdict != ReceiptVerdict::duplicate) {
      ++impl_->refusals;
    }
    return *outcome;
  }

  CanonicalWriter receipt_writer;
  DCE_TRY(codec::put(receipt_writer, receipt));
  DCE_TRY(impl_->append(RecordType::migration_receipt, receipt_writer));

  StageCheckpoint checkpoint;
  const std::string checkpoint_name =
      "chk." + text::u64_to_string(impl_->store.committed_sequence().value());
  Result<CheckpointId> checkpoint_id = CheckpointId::parse(checkpoint_name);
  if (!checkpoint_id.ok()) {
    return checkpoint_id.error();
  }
  checkpoint.id = *checkpoint_id;
  checkpoint.plan = receipt.plan;
  checkpoint.plan_generation = receipt.plan_generation;
  checkpoint.epoch = receipt.epoch;
  checkpoint.site = receipt.site;
  checkpoint.stage = receipt.stage;
  checkpoint.site_generation = receipt.accepted_generation;
  checkpoint.receipt = receipt.id;
  for (const SiteProgress& progress : impl_->rollout.sites) {
    if (progress.site == receipt.site) {
      checkpoint.cohort = progress.cohort;
      checkpoint.wave = progress.wave;
      checkpoint.stage_digest = progress.stage_digest;
    }
  }
  CanonicalWriter checkpoint_writer;
  DCE_TRY(codec::put(checkpoint_writer, checkpoint));
  DCE_TRY(impl_->append(RecordType::stage_checkpoint, checkpoint_writer));

  impl_->receipts.push_back(receipt);
  impl_->checkpoints.push_back(checkpoint);
  if (SiteProgress* progress = impl_->find_progress(receipt.site); progress != nullptr) {
    if (progress->accepted_stage < receipt.stage) {
      progress->accepted_stage = receipt.stage;
    }
    progress->generation = receipt.accepted_generation;
    progress->in_flight = false;
  }
  impl_->refresh_wave();
  return *outcome;
}

Result<ReconciliationOutcome> Coordinator::reconcile_site(const SiteStageReport& report) {
  if (impl_ == nullptr) {
    return Error{ErrorCode::closed, "the coordinator is not open"};
  }
  std::lock_guard<std::mutex> guard(impl_->mutex_);
  if (impl_->closed) {
    return Error{ErrorCode::closed, "the coordinator is closed"};
  }
  if (!impl_->has_plan) {
    return Error{ErrorCode::not_found, "no plan has been submitted"};
  }
  SiteProgress* progress = impl_->find_progress(report.site);
  if (progress == nullptr) {
    ++impl_->refusals;
    return Error{ErrorCode::not_found, "the reporting site is not part of this plan"};
  }
  const SiteRecord* record = find_site(impl_->plan, report.site);
  const bool delegated = record != nullptr && record->delegated_rollout_authority;
  Result<ReconciliationOutcome> outcome = reconcile(*progress, report, impl_->plan_generation,
                                                    impl_->store.epoch(), delegated);
  if (!outcome.ok()) {
    ++impl_->refusals;
    return outcome.error();
  }
  if (outcome->action == ReconciliationAction::reject) {
    ++impl_->refusals;
    return *outcome;
  }
  if (outcome->action != ReconciliationAction::agree) {
    ReconciliationRecord record_out;
    record_out.action = outcome->action;
    record_out.site = report.site;
    record_out.stage = outcome->agreed_stage;
    record_out.generation = outcome->next_generation;
    record_out.evidence_digest = outcome->evidence_digest;
    record_out.explanation = outcome->explanation;
    CanonicalWriter writer;
    DCE_TRY(put_reconciliation(writer, record_out));
    DCE_TRY(impl_->append(RecordType::reconciliation_record, writer));
    impl_->reconciliations.push_back(std::move(record_out));
    progress->accepted_stage = outcome->agreed_stage;
    progress->generation = outcome->next_generation;
    progress->stage_digest = report.accepted_stage_digest;
    progress->in_flight = false;
    progress->partitioned = report.partitioned;
    impl_->refresh_wave();
  } else {
    progress->partitioned = report.partitioned;
    progress->in_flight = false;
  }
  return *outcome;
}

Result<AuthorityToken> Coordinator::issue_authority(const SiteId& site, StageOrdinal stage) {
  if (impl_ == nullptr) {
    return Error{ErrorCode::closed, "the coordinator is not open"};
  }
  std::lock_guard<std::mutex> guard(impl_->mutex_);
  if (!impl_->has_plan) {
    return Error{ErrorCode::not_found, "no plan has been submitted"};
  }
  SiteProgress* progress = impl_->find_progress(site);
  if (progress == nullptr) {
    return Error{ErrorCode::not_found, "the site is not part of this plan"};
  }
  AuthorityToken token;
  token.plan = impl_->plan.identity.id;
  token.plan_generation = impl_->plan_generation;
  token.epoch = impl_->store.epoch();
  token.site = site;
  token.cohort = progress->cohort;
  token.stage = stage;
  token.plan_digest = impl_->plan.digest;
  const AuthorityCheck check =
      check_authority(token, impl_->plan, impl_->store.epoch(), impl_->plan_generation);
  if (!check.valid()) {
    return Error{ErrorCode::denied, check.explanation};
  }
  progress->in_flight = true;
  return token;
}

Result<RollbackEligibility> Coordinator::rollback_eligibility(CohortWave target_wave) {
  if (impl_ == nullptr) {
    return Error{ErrorCode::closed, "the coordinator is not open"};
  }
  std::lock_guard<std::mutex> guard(impl_->mutex_);
  if (!impl_->has_plan) {
    return Error{ErrorCode::not_found, "no plan has been submitted"};
  }
  RollbackMarker marker;
  marker.plan = impl_->plan.identity.id;
  marker.plan_generation = impl_->plan_generation;
  marker.epoch = impl_->store.epoch();
  marker.wave = impl_->rollout.current_wave;
  marker.crossed_point_of_no_return = impl_->point_of_no_return_crossed;
  marker.justification = "derived from the current rollout position";
  if (!impl_->rollback_markers.empty()) {
    marker = impl_->rollback_markers.back();
    marker.crossed_point_of_no_return =
        marker.crossed_point_of_no_return || impl_->point_of_no_return_crossed;
  }
  std::uint32_t highest_stage = 0;
  for (const SiteProgress& progress : impl_->rollout.sites) {
    highest_stage = std::max(highest_stage, progress.accepted_stage.value());
  }
  marker.stage = StageOrdinal{highest_stage};
  return assess_rollback_eligibility(impl_->plan, marker, target_wave);
}

Status Coordinator::record_rollback(const RollbackMarker& marker) {
  if (impl_ == nullptr) {
    return Error{ErrorCode::closed, "the coordinator is not open"};
  }
  std::lock_guard<std::mutex> guard(impl_->mutex_);
  if (impl_->closed) {
    return Error{ErrorCode::closed, "the coordinator is closed"};
  }
  if (!impl_->has_plan || !(marker.plan == impl_->plan.identity.id)) {
    return Error{ErrorCode::invalid_argument, "the marker names a plan this coordinator does not hold"};
  }
  CanonicalWriter writer;
  DCE_TRY(codec::put(writer, marker));
  DCE_TRY(impl_->append(RecordType::rollback_marker, writer));
  if (marker.crossed_point_of_no_return) {
    impl_->point_of_no_return_crossed = true;
  }
  impl_->rollback_markers.push_back(marker);
  return Status{};
}

Status Coordinator::compact() {
  if (impl_ == nullptr) {
    return Error{ErrorCode::closed, "the coordinator is not open"};
  }
  std::lock_guard<std::mutex> guard(impl_->mutex_);
  if (impl_->closed) {
    return Error{ErrorCode::closed, "the coordinator is closed"};
  }
  return impl_->write_snapshot();
}

CoordinatorStatus Coordinator::status() const {
  CoordinatorStatus status;
  if (impl_ == nullptr) {
    return status;
  }
  std::lock_guard<std::mutex> guard(impl_->mutex_);
  status.has_plan = impl_->has_plan;
  status.plan = impl_->plan.identity.id;
  status.state = impl_->state;
  status.plan_generation = impl_->plan_generation;
  status.epoch = impl_->store.epoch();
  status.sites = impl_->plan.membership.sites.size();
  status.current_wave = impl_->rollout.current_wave;
  status.receipts = impl_->receipts.size();
  status.checkpoints = impl_->checkpoints.size();
  status.reconciliations = impl_->reconciliations.size();
  status.refusals = impl_->refusals;
  status.submissions = impl_->submissions;
  status.point_of_no_return_crossed = impl_->point_of_no_return_crossed;
  status.plan_digest = impl_->plan.digest;
  status.observed_state_digest = impl_->observed_state_digest;
  if (impl_->has_plan) {
    const std::size_t total = stage_count(impl_->plan);
    for (const SiteProgress& progress : impl_->rollout.sites) {
      if (progress.accepted_stage.value() >= total) {
        ++status.sites_complete;
      }
    }
  }
  return status;
}

Result<EvolutionPlan> Coordinator::plan() const {
  if (impl_ == nullptr) {
    return Error{ErrorCode::closed, "the coordinator is not open"};
  }
  std::lock_guard<std::mutex> guard(impl_->mutex_);
  if (!impl_->has_plan) {
    return Error{ErrorCode::not_found, "no plan has been submitted"};
  }
  return impl_->plan;
}

Result<RolloutState> Coordinator::rollout_state() const {
  if (impl_ == nullptr) {
    return Error{ErrorCode::closed, "the coordinator is not open"};
  }
  std::lock_guard<std::mutex> guard(impl_->mutex_);
  return impl_->rollout;
}

Result<std::vector<SiteObservation>> Coordinator::observations() const {
  if (impl_ == nullptr) {
    return Error{ErrorCode::closed, "the coordinator is not open"};
  }
  std::lock_guard<std::mutex> guard(impl_->mutex_);
  return impl_->observations;
}

CoordinatorEpoch Coordinator::epoch() const {
  if (impl_ == nullptr) {
    return CoordinatorEpoch{};
  }
  std::lock_guard<std::mutex> guard(impl_->mutex_);
  return impl_->store.epoch();
}

Status Coordinator::close() {
  if (impl_ == nullptr) {
    return Status{};
  }
  std::lock_guard<std::mutex> guard(impl_->mutex_);
  if (impl_->closed) {
    return Status{};
  }
  impl_->closed = true;
  return impl_->store.close();
}

// ---------------------------------------------------------------------------
// CoordinatorServer
// ---------------------------------------------------------------------------
namespace {

void send_failure(platform::Socket& socket, const Error& error) {
  wire::Message message;
  message.kind = wire::MessageKind::failure;
  message.status = error.code;
  message.text = error.message;
  (void)wire::write_message(socket, message);
}

void fill_routing(wire::Message& message, const CoordinatorStatus& status) {
  message.epoch = status.epoch;
  message.plan_generation = status.plan_generation;
  message.plan = status.plan;
  message.plan_digest = status.plan_digest;
}

// The administrative surface, translated into coordinator calls. It holds no
// authority of its own: every answer comes from the Coordinator under its own
// lock.
wire::AdminResponse handle_admin_request(Coordinator& coordinator,
                                         const wire::AdminRequest& request) {
  wire::AdminResponse response;
  response.request_id = request.request_id;
  const CoordinatorStatus status = coordinator.status();
  response.epoch = status.epoch;
  response.plan_generation = status.plan_generation;
  response.plan_digest = status.plan_digest;
  response.sites = status.sites;
  response.sites_complete = status.sites_complete;
  response.receipts = status.receipts;
  response.checkpoints = status.checkpoints;
  response.reconciliations = status.reconciliations;
  response.refusals = status.refusals;

  switch (request.op) {
    case wire::AdminOp::status: {
      response.status = ErrorCode::ok;
      response.state = status.state;
      response.text = std::string("state=") + to_string(status.state) +
                      " generation=" + text::u64_to_string(status.plan_generation.value()) +
                      " epoch=" + text::u64_to_string(status.epoch.value()) +
                      " sites=" + text::u64_to_string(status.sites) +
                      " complete=" + text::u64_to_string(status.sites_complete);
      return response;
    }
    case wire::AdminOp::submit_plan: {
      if (!request.plan_body.has_value()) {
        response.status = ErrorCode::invalid_argument;
        response.text = "a submission must carry a plan";
        return response;
      }
      Result<PlanGeneration> generation = coordinator.submit_plan(*request.plan_body);
      if (!generation.ok()) {
        response.status = generation.code();
        response.text = generation.message();
        return response;
      }
      response.status = ErrorCode::ok;
      response.plan_generation = *generation;
      response.text = "plan committed at generation " + text::u64_to_string(generation->value());
      return response;
    }
    case wire::AdminOp::validate: {
      ValidationContext context;
      if (request.context.has_value()) {
        context = *request.context;
      }
      Result<ValidationReport> report = coordinator.validate(context);
      if (!report.ok()) {
        response.status = report.code();
        response.text = report.message();
        return response;
      }
      response.status = ErrorCode::ok;
      response.text = report->accepted() ? "accepted" : "refused";
      response.report = *report;
      return response;
    }
    case wire::AdminOp::event: {
      Result<PlanState> state = coordinator.apply(request.event, request.transition);
      if (!state.ok()) {
        response.status = state.code();
        response.text = state.message();
        return response;
      }
      response.status = ErrorCode::ok;
      response.state = *state;
      response.text = std::string("state is now ") + to_string(*state);
      return response;
    }
    case wire::AdminOp::observe: {
      std::vector<SiteObservation> observations;
      if (request.context.has_value()) {
        observations = request.context->observed_sites;
      }
      const Status recorded = coordinator.record_observation(std::move(observations));
      if (!recorded.ok()) {
        response.status = recorded.code();
        response.text = recorded.message();
        return response;
      }
      response.status = ErrorCode::ok;
      response.text = "observation recorded";
      return response;
    }
    case wire::AdminOp::next_action: {
      Result<RolloutAction> action = coordinator.next_action();
      if (!action.ok()) {
        response.status = action.code();
        response.text = action.message();
        return response;
      }
      response.status = ErrorCode::ok;
      response.action = *action;
      response.text = action->explanation;
      return response;
    }
    case wire::AdminOp::rollback_eligibility: {
      Result<RollbackEligibility> eligibility =
          coordinator.rollback_eligibility(request.target_wave);
      if (!eligibility.ok()) {
        response.status = eligibility.code();
        response.text = eligibility.message();
        return response;
      }
      response.status = ErrorCode::ok;
      response.rollback = *eligibility;
      response.text = eligibility->explanation;
      return response;
    }
    case wire::AdminOp::stop: {
      response.status = ErrorCode::ok;
      response.text = "stop acknowledged";
      return response;
    }
  }
  response.status = ErrorCode::unsupported;
  response.text = "unsupported administrative operation";
  return response;
}

}  // namespace

struct CoordinatorServer::Impl {
  CoordinatorServerOptions options;
  std::unique_ptr<Coordinator> coordinator;
  platform::Socket listener;
  std::thread acceptor;
  std::vector<std::thread> workers;
  std::mutex queue_mutex;
  std::condition_variable queue_cv;
  std::deque<platform::Socket> queue;
  std::atomic<bool> stop_requested{false};
  std::atomic<bool> stopping_workers{false};
  std::atomic<std::size_t> connected{0};
  std::uint16_t port{0};

  // A self-connection wakes a blocking accept() without a signal handler and
  // without any assumption about how a shutdown unblocks a listener.
  [[nodiscard]] Status wake_listener() {
    Result<platform::Socket> wake = platform::Socket::connect_loopback(port);
    if (!wake.ok()) {
      return wake.error();
    }
    return wake->close();
  }

  void accept_loop() {
    while (!stop_requested.load()) {
      Result<platform::Socket> accepted = listener.accept();
      if (!accepted.ok()) {
        if (stop_requested.load()) {
          break;
        }
        continue;
      }
      std::lock_guard<std::mutex> guard(queue_mutex);
      if (stop_requested.load()) {
        break;
      }
      if (queue.size() >= options.coordinator.max_connections) {
        // Bounded resources: refuse the connection rather than queueing without
        // limit. The peer observes a clean disconnect.
        (void)accepted->close();
        continue;
      }
      queue.push_back(std::move(*accepted));
      queue_cv.notify_one();
    }
  }

  void worker_loop() {
    while (true) {
      platform::Socket socket;
      {
        std::unique_lock<std::mutex> lock(queue_mutex);
        queue_cv.wait(lock, [this] { return !queue.empty() || stopping_workers.load(); });
        if (queue.empty()) {
          if (stopping_workers.load()) {
            return;
          }
          continue;
        }
        socket = std::move(queue.front());
        queue.pop_front();
      }
      serve_connection(std::move(socket));
    }
  }

  void serve_connection(platform::Socket socket) {
    std::uint16_t kind = 0;
    Result<std::vector<std::byte>> frame = wire::read_frame(socket, kind);
    if (!frame.ok()) {
      return;
    }
    if (kind == wire::kAdminFrameKind) {
      handle_admin(socket, *frame);
      return;
    }
    Result<wire::Message> message = wire::decode_message(*frame);
    if (!message.ok()) {
      send_failure(socket, message.error());
      return;
    }
    if (message->kind != wire::MessageKind::hello) {
      send_failure(socket, Error{ErrorCode::malformed, "a session must begin with hello"});
      return;
    }
    handle_site(socket, *message);
  }

  void handle_admin(platform::Socket& socket, const std::vector<std::byte>& first) {
    std::vector<std::byte> buffer = first;
    while (!stop_requested.load()) {
      Result<wire::AdminRequest> request = wire::decode_admin_request(buffer);
      if (!request.ok()) {
        return;
      }
      const wire::AdminResponse response = handle_admin_request(*coordinator, *request);
      Result<std::vector<std::byte>> body = wire::encode_admin_response(response);
      if (!body.ok()) {
        return;
      }
      if (!wire::write_frame(socket, wire::kAdminFrameKind, *body).ok()) {
        return;
      }
      std::uint16_t kind = 0;
      Result<std::vector<std::byte>> next = wire::read_frame(socket, kind);
      if (!next.ok() || kind != wire::kAdminFrameKind) {
        return;
      }
      buffer = std::move(*next);
    }
  }

  // The site's own report is the only source of truth about what it runs, so it
  // is consumed as an observation rather than inferred.
  [[nodiscard]] Status observe_site_from_profile(const SiteRecord& profile) {
    SiteObservation observation;
    observation.site = profile.id;
    observation.dccp_version = profile.dccp_version;
    observation.components = profile.components;
    observation.accepted_generation = profile.accepted_generation;
    observation.capabilities = profile.capabilities;
    observation.policy = profile.policy;
    observation.state_digest = profile.state_digest;
    observation.delegated_rollout_authority = profile.delegated_rollout_authority;
    return coordinator->observe_site(std::move(observation));
  }

  void handle_site(platform::Socket& socket, const wire::Message& hello) {
    if (hello.profile.has_value()) {
      (void)observe_site_from_profile(*hello.profile);
    }
    const CoordinatorStatus status = coordinator->status();
    wire::Message ack;
    ack.kind = wire::MessageKind::hello_ack;
    ack.request_id = hello.request_id;
    ack.site = hello.site;
    fill_routing(ack, status);
    if (!status.has_plan) {
      ack.status = ErrorCode::not_found;
      ack.text = "no plan has been submitted";
      (void)wire::write_message(socket, ack);
      return;
    }
    ack.status = ErrorCode::ok;
    ack.text = "ready";
    if (!wire::write_message(socket, ack).ok()) {
      return;
    }

    connected.fetch_add(1);
    std::uint16_t kind = 0;
    Result<std::vector<std::byte>> frame = wire::read_frame(socket, kind);
    if (frame.ok() && kind != wire::kAdminFrameKind) {
      Result<wire::Message> request = wire::decode_message(*frame);
      if (request.ok() && request->kind == wire::MessageKind::reconcile_request &&
          request->report.has_value()) {
        run_round(socket, hello, *request);
      }
    }
    connected.fetch_sub(1);
  }

  void run_round(platform::Socket& socket, const wire::Message& hello,
                 const wire::Message& request) {
    Result<ReconciliationOutcome> outcome =
        coordinator->reconcile_site(*request.report);
    const CoordinatorStatus after = coordinator->status();

    wire::Message reply;
    reply.kind = wire::MessageKind::reconcile_result;
    reply.request_id = request.request_id;
    reply.site = hello.site;
    fill_routing(reply, after);
    if (outcome.ok()) {
      reply.status = ErrorCode::ok;
      reply.stage = outcome->agreed_stage;
      reply.text = outcome->explanation;
    } else {
      reply.status = outcome.code();
      reply.text = outcome.message();
    }
    if (!wire::write_message(socket, reply).ok()) {
      return;
    }

    Result<RolloutAction> action = coordinator->next_action();
    if (!action.ok()) {
      send_failure(socket, action.error());
      return;
    }

    if (action->kind == RolloutActionKind::advance_site && action->site == hello.site) {
      offer_stage(socket, hello, request, *action);
      return;
    }

    wire::Message directive;
    directive.site = hello.site;
    directive.request_id = request.request_id;
    fill_routing(directive, after);
    switch (action->kind) {
      case RolloutActionKind::completed:
        directive.kind = wire::MessageKind::stop;
        directive.status = ErrorCode::ok;
        break;
      case RolloutActionKind::paused:
      case RolloutActionKind::gate_blocked:
      case RolloutActionKind::await_receipt:
      case RolloutActionKind::advance_wave:
        directive.kind = wire::MessageKind::pause;
        directive.status = ErrorCode::ok;
        break;
      case RolloutActionKind::refused:
        directive.kind = wire::MessageKind::pause;
        directive.status = ErrorCode::refused;
        break;
      case RolloutActionKind::advance_site:
        directive.kind = wire::MessageKind::pause;
        directive.status = ErrorCode::ok;
        break;
    }
    directive.text = action->explanation;
    (void)wire::write_message(socket, directive);
  }

  void offer_stage(platform::Socket& socket, const wire::Message& hello,
                   const wire::Message& request, const RolloutAction& action) {
    Result<AuthorityToken> token = coordinator->issue_authority(hello.site, action.target_stage);
    const CoordinatorStatus after = coordinator->status();
    wire::Message offer;
    offer.kind = wire::MessageKind::stage_offer;
    offer.request_id = request.request_id;
    offer.site = hello.site;
    offer.stage = action.target_stage;
    offer.cohort = action.cohort;
    offer.step = action.step;
    // The offer carries the exact transition it authorises. A site does not
    // hold the plan, so a receipt built from anything but these versions would
    // be a guess, and a guessed receipt is refused as the wrong stage.
    Result<EvolutionPlan> current = coordinator->plan();
    if (current.ok()) {
      const MigrationStep* step = find_step(*current, action.step);
      if (step != nullptr) {
        offer.from_version = step->from;
        offer.to_version = step->to;
      }
    }
    fill_routing(offer, after);
    if (token.ok()) {
      offer.status = ErrorCode::ok;
      offer.token = *token;
      offer.text = action.explanation;
    } else {
      offer.status = token.code();
      offer.text = token.message();
    }
    if (!wire::write_message(socket, offer).ok() || !token.ok()) {
      return;
    }

    std::uint16_t kind = 0;
    Result<std::vector<std::byte>> frame = wire::read_frame(socket, kind);
    if (!frame.ok() || kind == wire::kAdminFrameKind) {
      return;
    }
    Result<wire::Message> result = wire::decode_message(*frame);
    if (!result.ok() || result->kind != wire::MessageKind::stage_result ||
        !result->receipt.has_value()) {
      return;
    }
    // A refused or cancelled stage never becomes an accepted one: the outcome
    // is recorded exactly as the coordinator judged it.
    (void)coordinator->accept_migration(*result->receipt);
  }
};

CoordinatorServer::~CoordinatorServer() {
  if (impl_ == nullptr) {
    return;
  }
  request_stop();
  if (impl_->acceptor.joinable()) {
    impl_->acceptor.join();
  }
  {
    std::lock_guard<std::mutex> guard(impl_->queue_mutex);
    impl_->stopping_workers.store(true);
  }
  impl_->queue_cv.notify_all();
  for (std::thread& worker : impl_->workers) {
    if (worker.joinable()) {
      worker.join();
    }
  }
  impl_->workers.clear();
  (void)impl_->listener.close();
  if (impl_->coordinator != nullptr) {
    (void)impl_->coordinator->close();
  }
}

Result<std::unique_ptr<CoordinatorServer>> CoordinatorServer::start(
    const CoordinatorServerOptions& options) {
  auto server = std::unique_ptr<CoordinatorServer>(new CoordinatorServer());
  server->impl_ = std::make_unique<Impl>();
  Impl& impl = *server->impl_;
  impl.options = options;

  Result<std::unique_ptr<Coordinator>> coordinator = Coordinator::open(options.coordinator);
  if (!coordinator.ok()) {
    return coordinator.error();
  }
  impl.coordinator = std::move(*coordinator);

  Result<platform::Socket> listener = platform::Socket::listen_loopback(options.port);
  if (!listener.ok()) {
    return listener.error();
  }
  Result<std::uint16_t> port = listener->local_port();
  if (!port.ok()) {
    return port.error();
  }
  impl.port = *port;
  impl.listener = std::move(*listener);
  return server;
}

Result<std::uint16_t> CoordinatorServer::port() const {
  if (impl_ == nullptr) {
    return Error{ErrorCode::closed, "the server is not started"};
  }
  return impl_->port;
}

Status CoordinatorServer::serve() {
  if (impl_ == nullptr) {
    return Error{ErrorCode::closed, "the server is not started"};
  }
  Impl& impl = *impl_;
  impl.acceptor = std::thread([&impl] { impl.accept_loop(); });
  const std::uint32_t count =
      impl.options.coordinator.max_worker_threads == 0 ? 1u : impl.options.coordinator.max_worker_threads;
  impl.workers.reserve(count);
  for (std::uint32_t index = 0; index < count; ++index) {
    impl.workers.emplace_back([&impl] { impl.worker_loop(); });
  }
  impl.acceptor.join();
  {
    std::lock_guard<std::mutex> guard(impl.queue_mutex);
    impl.stopping_workers.store(true);
  }
  impl.queue_cv.notify_all();
  for (std::thread& worker : impl.workers) {
    worker.join();
  }
  impl.workers.clear();
  return Status{};
}

void CoordinatorServer::request_stop() {
  if (impl_ == nullptr) {
    return;
  }
  const bool already = impl_->stop_requested.exchange(true);
  if (already) {
    return;
  }
  (void)impl_->wake_listener();
}

Coordinator& CoordinatorServer::coordinator() { return *impl_->coordinator; }

std::size_t CoordinatorServer::connected_sites() const {
  return impl_ == nullptr ? 0 : impl_->connected.load();
}

}  // namespace dce
