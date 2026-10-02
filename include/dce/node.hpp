// The coordinator and the site agent: the two roles this boundary runs.
//
// The coordinator owns evolution authority for a fleet. A site agent owns the
// local truth about one site: which stage it actually accepted, at which
// generation. Neither reaches into the other's internals; they exchange
// explicit authority tokens, stage offers, receipts and stage reports.
//
// Concurrency and lock order, stated once and enforced everywhere:
//   state_mutex_  protects the authoritative in-memory model
//   store         has its own internal lock and is only ever entered while
//                 state_mutex_ is held, never the other way round
//   sockets       are never written to while state_mutex_ is held
// A worker decodes a request, takes state_mutex_, mutates and commits, releases
// it, and only then writes the response. Shutdown closes the listener, joins
// the accept thread, closes client sockets, joins the workers, and only then
// closes the store, so no worker is ever joined while it still needs state that
// the shutdown path holds.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "dce/authority.hpp"
#include "dce/ids.hpp"
#include "dce/lifecycle.hpp"
#include "dce/plan.hpp"
#include "dce/protocol.hpp"
#include "dce/reconcile.hpp"
#include "dce/rollout.hpp"
#include "dce/status.hpp"
#include "dce/validate.hpp"

namespace dce {

struct CoordinatorOptions {
  std::string store_directory;
  bool sync_on_commit{true};
  std::uint32_t max_worker_threads{4};
  std::size_t max_connections{64};
  std::size_t max_submissions{4096};
  // A snapshot is written once this many records have accumulated since the
  // last one, which bounds both recovery time and log growth.
  std::uint64_t snapshot_every_records{1024};
};

struct CoordinatorStatus {
  bool has_plan{false};
  EvolutionPlanId plan;
  PlanState state{PlanState::draft};
  PlanGeneration plan_generation;
  CoordinatorEpoch epoch;
  std::size_t sites{0};
  std::size_t sites_complete{0};
  CohortWave current_wave;
  std::uint64_t receipts{0};
  std::uint64_t checkpoints{0};
  std::uint64_t reconciliations{0};
  std::uint64_t refusals{0};
  std::uint64_t submissions{0};
  bool point_of_no_return_crossed{false};
  Digest256 plan_digest;
  Digest256 observed_state_digest;
};

class Coordinator {
 public:
  Coordinator() = default;
  Coordinator(Coordinator&&) = delete;
  Coordinator& operator=(Coordinator&&) = delete;
  Coordinator(const Coordinator&) = delete;
  Coordinator& operator=(const Coordinator&) = delete;
  ~Coordinator();

  [[nodiscard]] static Result<std::unique_ptr<Coordinator>> open(const CoordinatorOptions& options);

  // Accepts an authored plan: it is canonicalised, structurally checked, the
  // revision is assigned, and the revision is committed durably before it is
  // published as the authoritative plan.
  [[nodiscard]] Result<PlanGeneration> submit_plan(EvolutionPlan plan);

  [[nodiscard]] Result<ValidationReport> validate(const ValidationContext& context);

  [[nodiscard]] Status record_observation(std::vector<SiteObservation> observations);

  // Merges one site's explicit self-report into the observed fleet. A site that
  // reports a different profile replaces what was known about it; a site that
  // has never been observed is added.
  [[nodiscard]] Status observe_site(SiteObservation observation);

  [[nodiscard]] Result<PlanState> apply(LifecycleEvent event, const TransitionContext& context);

  [[nodiscard]] Result<RolloutAction> next_action();

  [[nodiscard]] Result<ReceiptOutcome> accept_migration(const MigrationReceipt& receipt);

  [[nodiscard]] Result<ReconciliationOutcome> reconcile_site(const SiteStageReport& report);

  [[nodiscard]] Result<AuthorityToken> issue_authority(const SiteId& site, StageOrdinal stage);

  [[nodiscard]] Result<RollbackEligibility> rollback_eligibility(CohortWave target_wave);

  [[nodiscard]] Status record_rollback(const RollbackMarker& marker);

  // Writes a full state image covering the committed sequence and retires the
  // segments below it. Records above the snapshot are never removed.
  [[nodiscard]] Status compact();

  [[nodiscard]] CoordinatorStatus status() const;
  [[nodiscard]] Result<EvolutionPlan> plan() const;
  [[nodiscard]] Result<RolloutState> rollout_state() const;
  [[nodiscard]] Result<std::vector<SiteObservation>> observations() const;
  [[nodiscard]] CoordinatorEpoch epoch() const;
  [[nodiscard]] Status close();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

struct CoordinatorServerOptions {
  CoordinatorOptions coordinator;
  std::uint16_t port{0};
};

// A transport shell around a Coordinator. It holds no authority of its own:
// every decision is taken by the Coordinator under its own lock.
class CoordinatorServer {
 public:
  CoordinatorServer() = default;
  CoordinatorServer(CoordinatorServer&&) = delete;
  CoordinatorServer& operator=(CoordinatorServer&&) = delete;
  CoordinatorServer(const CoordinatorServer&) = delete;
  CoordinatorServer& operator=(const CoordinatorServer&) = delete;
  ~CoordinatorServer();

  [[nodiscard]] static Result<std::unique_ptr<CoordinatorServer>> start(
      const CoordinatorServerOptions& options);

  [[nodiscard]] Result<std::uint16_t> port() const;

  // Blocks until stop is requested or the listener fails.
  [[nodiscard]] Status serve();

  void request_stop();

  [[nodiscard]] Coordinator& coordinator();
  [[nodiscard]] std::size_t connected_sites() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

struct SiteNodeOptions {
  std::string store_directory;
  SiteRecord profile;
  std::uint16_t coordinator_port{0};
  bool accept_delegated_authority{false};
  std::uint32_t max_reconnect_attempts{256};
  // Number of completed synchronisation rounds after which run() returns.
  // Zero means "until stopped", which is what a long-lived site process uses.
  std::uint64_t exit_after_rounds{0};
};

struct SiteNodeStatus {
  SiteId site;
  SiteGeneration accepted_generation;
  StageOrdinal accepted_stage;
  Digest256 stage_digest;
  bool connected{false};
  bool partitioned{false};
  std::uint64_t accepted_stages{0};
  std::uint64_t refused_offers{0};
  std::uint64_t reconciliations{0};
  std::uint64_t rounds{0};
  CoordinatorEpoch epoch;
};

class SiteNode {
 public:
  SiteNode() = default;
  SiteNode(SiteNode&&) = delete;
  SiteNode& operator=(SiteNode&&) = delete;
  SiteNode(const SiteNode&) = delete;
  SiteNode& operator=(const SiteNode&) = delete;
  ~SiteNode();

  [[nodiscard]] static Result<std::unique_ptr<SiteNode>> start(const SiteNodeOptions& options);

  [[nodiscard]] Status run();
  void request_stop();

  // A partitioned site keeps serving locally. It stops advancing, because
  // advancing without the coordinator's authority would be a claim it cannot
  // support, and it keeps its accepted stage exactly as it was.
  void set_partitioned(bool partitioned);

  [[nodiscard]] SiteNodeStatus status() const;
  [[nodiscard]] Result<SiteStageReport> report() const;
  [[nodiscard]] Status close();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// A one-shot administrative client, used by the command line tool and by the
// end-to-end suites.
class AdminClient {
 public:
  AdminClient() = default;
  AdminClient(AdminClient&&) = delete;
  AdminClient& operator=(AdminClient&&) = delete;
  AdminClient(const AdminClient&) = delete;
  AdminClient& operator=(const AdminClient&) = delete;
  ~AdminClient();

  [[nodiscard]] static Result<std::unique_ptr<AdminClient>> connect(std::uint16_t port);
  [[nodiscard]] Result<wire::AdminResponse> call(const wire::AdminRequest& request);
  [[nodiscard]] Status close();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace dce
