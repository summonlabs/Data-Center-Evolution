// The site agent: one operating-system process that owns the local truth about
// exactly one site.
//
// A site never invents authority. It moves exactly one stage at a time, only
// where the coordinator presented a token that is current for the plan, the
// plan generation and the coordinator epoch it names, and only after the
// acceptance is durable in the site's own store. What it reports is what it
// actually holds: the stage it accepted, the generation it accepted it at, the
// digest of that stage and the receipts that prove it. A refused or cancelled
// offer is never later reported as accepted.
//
// Lock discipline, from include/dce/node.hpp: mutex_ protects the in-memory
// truth, the store is entered only while mutex_ is held, and no socket is
// written to while mutex_ is held. Each round therefore decodes first, then
// takes the lock, then releases it, and only then writes.
#include "dce/node.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "dce/canonical.hpp"
#include "dce/checked.hpp"
#include "dce/codec.hpp"
#include "dce/digest.hpp"
#include "dce/limits.hpp"
#include "dce/platform.hpp"
#include "dce/store.hpp"
#include "dce/text.hpp"

namespace dce {
namespace {

// The pause between rounds exists so that a site which cannot exchange anything
// - because it is partitioned, or because the operating system refuses the
// connection - does not spin on the CPU. It is never a timeout and never a
// success condition: every exit from run() is decided from state, not from
// elapsed time.
constexpr std::chrono::milliseconds kRoundPause{20};

// A round reads the coordinator's reconciliation result and then the directive
// that follows it. A peer that keeps talking without ever issuing a directive
// is a protocol defect rather than a reason to loop.
constexpr std::size_t kMaxRepliesPerRound = 4;

// Status counters saturate rather than wrap: a counter that wrapped would
// report a smaller number than it had already reported.
void bump(std::uint64_t& counter) { (void)accumulate(counter, std::uint64_t{1}); }

[[nodiscard]] std::string site_name(const SiteId& site) {
  return text::escape_for_output(site.view());
}

[[nodiscard]] std::string number32(std::uint32_t value) { return text::u32_to_string(value); }

[[nodiscard]] std::string number64(std::uint64_t value) { return text::u64_to_string(value); }

// The identity of one acceptance. The same plan, plan generation, site and
// stage always produce the same digest, which is what makes a replayed offer
// recognisable as the duplicate it is. The key is a fixed-width hex digest
// rather than a concatenation of identities, so it stays a valid, bounded
// identifier whatever the identities are called.
[[nodiscard]] Result<std::string> acceptance_digest(const EvolutionPlanId& plan,
                                                    const PlanGeneration& generation,
                                                    const SiteId& site,
                                                    const StageOrdinal& stage) {
  CanonicalWriter writer;
  DCE_TRY(codec::put(writer, plan));
  DCE_TRY(codec::put(writer, generation));
  DCE_TRY(codec::put(writer, site));
  DCE_TRY(codec::put(writer, stage));
  return writer.digest().to_hex();
}

// An authority verdict is reported with the closest outcome code this runtime
// owns, so a refusal says why without inventing a second vocabulary.
[[nodiscard]] ErrorCode authority_failure_code(AuthorityVerdict verdict) {
  switch (verdict) {
    case AuthorityVerdict::stale_epoch:
      return ErrorCode::stale;
    case AuthorityVerdict::fenced_generation:
      return ErrorCode::fenced;
    case AuthorityVerdict::plan_mismatch:
    case AuthorityVerdict::digest_mismatch:
      return ErrorCode::conflict;
    case AuthorityVerdict::not_delegated:
      return ErrorCode::denied;
    case AuthorityVerdict::unknown_site:
      return ErrorCode::not_found;
    case AuthorityVerdict::valid:
      break;
  }
  return ErrorCode::denied;
}

}  // namespace

struct SiteNode::Impl {
  explicit Impl(SiteNodeOptions options)
      : options_(std::move(options)), profile_(options_.profile) {}

  ~Impl();

  Status initialise();
  Status run();
  void request_stop();
  void set_partitioned(bool partitioned);
  [[nodiscard]] SiteNodeStatus status() const;
  [[nodiscard]] Result<SiteStageReport> report() const;
  Status close();

 private:
  // Every method whose name ends in _locked is called with mutex_ already held.
  [[nodiscard]] Result<std::uint64_t> allocate_request_id_locked();
  [[nodiscard]] wire::Message hello_locked(std::uint64_t request_id) const;
  [[nodiscard]] wire::Message reconcile_locked(std::uint64_t request_id) const;
  [[nodiscard]] SiteStageReport report_locked() const;
  [[nodiscard]] Status decide_offer_locked(const wire::Message& offer, wire::Message& result);
  [[nodiscard]] Status accept_offer_locked(const wire::Message& offer, wire::Message& result);
  [[nodiscard]] Status replay_offer_locked(const wire::Message& offer, wire::Message& result);
  void refuse_locked(wire::Message& result, ErrorCode code, std::string explanation);
  void remember_locked(const MigrationReceipt& receipt);
  [[nodiscard]] const MigrationReceipt* find_record_locked(const EvolutionPlanId& plan,
                                                           const StageOrdinal& stage) const;

  // Taking mutex_ is their own business.
  Status open_and_recover();
  Status load_records(const persist::RecoveredState& recovered);
  [[nodiscard]] Result<std::uint64_t> allocate_request_id();
  Status run_loop();
  [[nodiscard]] Result<bool> run_once();
  [[nodiscard]] Result<bool> exchange(platform::Socket& socket);
  [[nodiscard]] Result<bool> handle_offer(platform::Socket& socket, const wire::Message& offer);
  [[nodiscard]] Result<bool> round_failure();
  void mark_disconnected();
  void count_reconciliation();
  [[nodiscard]] bool wait_between_rounds();

  mutable std::mutex mutex_;
  std::condition_variable wake_;
  SiteNodeOptions options_;
  SiteRecord profile_;
  persist::Store store_;
  SiteGeneration accepted_generation_;
  StageOrdinal accepted_stage_;
  Digest256 stage_digest_;
  CoordinatorEpoch epoch_;
  PlanGeneration plan_generation_;
  std::vector<ReceiptId> receipts_;
  std::vector<MigrationReceipt> records_;
  std::uint64_t accepted_stages_{0};
  std::uint64_t refused_offers_{0};
  std::uint64_t reconciliations_{0};
  std::uint64_t rounds_{0};
  std::uint64_t request_id_{0};
  std::uint32_t connect_failures_{0};
  bool partitioned_{false};
  bool connected_{false};
  bool stop_requested_{false};
  bool running_{false};
};

SiteNode::Impl::~Impl() {
  // The store closes itself as well; closing here makes the end of the node
  // explicit and is idempotent either way. No socket outlives a round, so
  // nothing here can block on a peer that will never write again.
  (void)store_.close();
}

// ---------------------------------------------------------------------------
// Startup: the durable truth comes back before anything else does
// ---------------------------------------------------------------------------
Status SiteNode::Impl::open_and_recover() {
  persist::StoreOptions store_options;
  store_options.sync_on_commit = true;
  Result<persist::Store> opened =
      persist::Store::open(options_.store_directory, store_options, persist::OpenMode::read_write);
  if (!opened.ok()) {
    return Error{opened.code(), "site " + site_name(profile_.id) + " could not open its store at " +
                                    text::escape_for_output(options_.store_directory) + ": " +
                                    opened.message()};
  }
  Result<persist::RecoveredState> recovered = opened->recover();
  if (!recovered.ok()) {
    return Error{recovered.code(), "site " + site_name(profile_.id) +
                                       " could not recover its store: " + recovered.message()};
  }
  DCE_TRY(load_records(*recovered));
  store_ = std::move(opened).value();
  return Status{};
}

Status SiteNode::Impl::load_records(const persist::RecoveredState& recovered) {
  std::vector<MigrationReceipt> accepted;
  for (const persist::LogEntry& entry : recovered.records) {
    if (entry.type != persist::RecordType::migration_receipt) {
      continue;
    }
    CanonicalReader reader(entry.payload);
    Result<MigrationReceipt> decoded = codec::get<MigrationReceipt>(reader);
    if (!decoded.ok()) {
      return Error{ErrorCode::corruption,
                   "site " + site_name(profile_.id) +
                       " holds a durable migration receipt that does not decode at log sequence " +
                       number64(entry.sequence.value()) + ": " + decoded.message()};
    }
    if (!reader.at_end()) {
      return Error{ErrorCode::corruption,
                   "durable migration receipt at log sequence " +
                       number64(entry.sequence.value()) +
                       " carries bytes the canonical codec did not consume"};
    }
    if (decoded->site != profile_.id) {
      return Error{ErrorCode::conflict,
                   "site " + site_name(profile_.id) + " opened a store whose records name site " +
                       site_name(decoded->site) +
                       "; the durable truth of one site is not another site's to recover"};
    }
    if (decoded->stage.is_zero()) {
      return Error{ErrorCode::corruption,
                   "durable migration receipt at log sequence " +
                       number64(entry.sequence.value()) +
                       " records stage 0, which is never an accepted stage"};
    }
    accepted.push_back(std::move(*decoded));
  }
  if (accepted.size() > kMaxReceipts) {
    return Error{ErrorCode::limit_exceeded,
                 "site " + site_name(profile_.id) + " holds " + number64(accepted.size()) +
                     " durable receipts, beyond the documented bound of " + number64(kMaxReceipts)};
  }
  std::stable_sort(accepted.begin(), accepted.end(),
                   [](const MigrationReceipt& lhs, const MigrationReceipt& rhs) {
                     return lhs.stage < rhs.stage;
                   });
  // At most one durable record exists per accepted stage, because a stage is
  // never applied twice. A later record for the same stage would supersede an
  // earlier one rather than being replayed on top of it.
  std::vector<MigrationReceipt> unique;
  unique.reserve(accepted.size());
  for (MigrationReceipt& receipt : accepted) {
    if (!unique.empty() && unique.back().stage == receipt.stage) {
      unique.back() = std::move(receipt);
      continue;
    }
    unique.push_back(std::move(receipt));
  }
  for (std::size_t index = 0; index < unique.size(); ++index) {
    const std::uint64_t expected = static_cast<std::uint64_t>(index) + 1u;
    if (static_cast<std::uint64_t>(unique[index].stage.value()) == expected) {
      continue;
    }
    const std::string previous =
        index == 0 ? std::string("no earlier stage")
                   : ("stage " + number32(unique[index - 1].stage.value()));
    return Error{ErrorCode::corruption,
                 "site " + site_name(profile_.id) +
                     " holds durable receipts that are not a contiguous prefix of stages: stage " +
                     number32(unique[index].stage.value()) + " follows " + previous};
  }
  std::lock_guard<std::mutex> lock(mutex_);
  records_ = std::move(unique);
  receipts_.reserve(records_.size());
  for (const MigrationReceipt& receipt : records_) {
    receipts_.push_back(receipt.id);
  }
  if (!records_.empty()) {
    const MigrationReceipt& last = records_.back();
    accepted_stage_ = last.stage;
    accepted_generation_ = last.accepted_generation;
    stage_digest_ = last.evidence_digest;
  }
  return Status{};
}

// ---------------------------------------------------------------------------
// Message construction
// ---------------------------------------------------------------------------
Result<std::uint64_t> SiteNode::Impl::allocate_request_id_locked() {
  const std::optional<std::uint64_t> next = checked_next(request_id_);
  if (!next.has_value()) {
    return Error{ErrorCode::overflow,
                 "site " + site_name(profile_.id) +
                     " exhausted its request identifier space and will not reuse one"};
  }
  request_id_ = *next;
  return *next;
}

Result<std::uint64_t> SiteNode::Impl::allocate_request_id() {
  std::lock_guard<std::mutex> lock(mutex_);
  return allocate_request_id_locked();
}

wire::Message SiteNode::Impl::hello_locked(std::uint64_t request_id) const {
  wire::Message hello;
  hello.kind = wire::MessageKind::hello;
  hello.request_id = request_id;
  hello.site = profile_.id;
  hello.epoch = epoch_;
  hello.plan_generation = plan_generation_;
  hello.stage = accepted_stage_;
  hello.site_generation = accepted_generation_;
  hello.stage_digest = stage_digest_;
  hello.profile = profile_;
  return hello;
}

wire::Message SiteNode::Impl::reconcile_locked(std::uint64_t request_id) const {
  wire::Message request;
  request.kind = wire::MessageKind::reconcile_request;
  request.request_id = request_id;
  request.site = profile_.id;
  request.epoch = epoch_;
  request.plan_generation = plan_generation_;
  request.report = report_locked();
  return request;
}

SiteStageReport SiteNode::Impl::report_locked() const {
  SiteStageReport report;
  report.site = profile_.id;
  report.generation = accepted_generation_;
  report.epoch = epoch_;
  report.accepted_stage = accepted_stage_;
  report.accepted_stage_digest = stage_digest_;
  report.receipts = receipts_;
  report.partitioned = partitioned_;
  return report;
}

Status SiteNode::Impl::decide_offer_locked(const wire::Message& offer, wire::Message& result) {
  // The site's own policy comes first: without it the site does not act under
  // delegated authority at all, whatever the offer carries.
  if (!options_.accept_delegated_authority) {
    refuse_locked(result, ErrorCode::denied,
                  "site " + site_name(profile_.id) +
                      " does not accept delegated rollout authority, so the offered stage is refused");
    return Status{};
  }
  if (!offer.token.has_value()) {
    refuse_locked(result, ErrorCode::denied,
                  "stage_offer carries no authority token, and authority that is not presented is "
                  "not held");
    return Status{};
  }
  const AuthorityToken& token = *offer.token;
  if (token.site != profile_.id) {
    refuse_locked(result, ErrorCode::denied,
                  "authority token names site " + site_name(token.site) + " but this node is site " +
                      site_name(profile_.id));
    return Status{};
  }
  if (token.stage != offer.stage) {
    refuse_locked(result, ErrorCode::denied,
                  "authority token authorises stage " + number32(token.stage.value()) +
                      " but the offer is for stage " + number32(offer.stage.value()));
    return Status{};
  }
  // The plan itself is not on the wire, so the token is checked against a view
  // reconstructed from exactly what the offer names: plan identity, digest,
  // plan generation and epoch. The delegation this site accepts is the option
  // checked above, so the view states it as held rather than re-litigating it
  // from the site's own description of itself.
  EvolutionPlan plan_view;
  plan_view.identity.id = offer.plan;
  plan_view.identity.generation = offer.plan_generation;
  plan_view.identity.epoch = offer.epoch;
  plan_view.digest = offer.plan_digest;
  SiteRecord member = profile_;
  member.delegated_rollout_authority = true;
  plan_view.membership.sites.push_back(std::move(member));
  const AuthorityCheck check = check_authority(token, plan_view, offer.epoch, offer.plan_generation);
  if (!check.valid()) {
    refuse_locked(result, authority_failure_code(check.verdict),
                  "authority token rejected: " + std::string(to_string(check.verdict)));
    return Status{};
  }
  // FENCING. The check above compares the token with what the offer itself
  // claims. It is not enough: the offer must also be current for the epoch and
  // the plan generation this site has already adopted from the coordinator's
  // own handshake, or an offer that is internally consistent but superseded
  // would be honoured. A site never acts under authority that has been replaced.
  if (offer.epoch != epoch_) {
    refuse_locked(result, ErrorCode::stale,
                  "offer was issued for epoch " + number64(offer.epoch.value()) +
                      " but the site has adopted epoch " + number64(epoch_.value()) +
                      "; a superseded epoch may no longer act");
    return Status{};
  }
  if (offer.plan_generation < plan_generation_) {
    refuse_locked(result, ErrorCode::fenced,
                  "offer carries plan generation " + number64(offer.plan_generation.value()) +
                      " but the site has adopted generation " + number64(plan_generation_.value()) +
                      "; a superseded plan generation may no longer act");
    return Status{};
  }
  const std::optional<std::uint32_t> next_stage = checked_next(accepted_stage_.value());
  if (!next_stage.has_value()) {
    refuse_locked(result, ErrorCode::overflow,
                  "the site stage ordinal is exhausted at its maximum value, so no further stage "
                  "can be accepted");
    return Status{};
  }
  if (offer.stage.value() == *next_stage) {
    return accept_offer_locked(offer, result);
  }
  if (!accepted_stage_.is_zero() && offer.stage == accepted_stage_) {
    return replay_offer_locked(offer, result);
  }
  if (offer.stage.value() > *next_stage) {
    refuse_locked(result, ErrorCode::out_of_range,
                  "offer would skip from stage " + number32(accepted_stage_.value()) + " to stage " +
                      number32(offer.stage.value()) + "; a site advances by exactly one stage");
    return Status{};
  }
  refuse_locked(result, ErrorCode::stale,
                "offer is for stage " + number32(offer.stage.value()) +
                    " but the site has already accepted stage " +
                    number32(accepted_stage_.value()));
  return Status{};
}

Status SiteNode::Impl::accept_offer_locked(const wire::Message& offer, wire::Message& result) {
  const std::optional<std::uint64_t> advanced = checked_next(accepted_generation_.value());
  if (!advanced.has_value()) {
    refuse_locked(result, ErrorCode::overflow,
                  "the site generation is exhausted at its maximum value, so the acceptance cannot "
                  "be described");
    return Status{};
  }
  // Generations are monotonic. The coordinator may have fenced the site forward
  // after a rewind, which is at or above the next local generation; an older
  // value is never adopted.
  SiteGeneration generation = SiteGeneration::from_value(*advanced);
  if (offer.site_generation > generation) {
    generation = offer.site_generation;
  }
  DCE_ASSIGN(digest_hex, acceptance_digest(offer.plan, offer.plan_generation, profile_.id, offer.stage));
  MigrationReceipt receipt;
  receipt.plan = offer.plan;
  receipt.plan_generation = offer.plan_generation;
  receipt.epoch = offer.epoch;
  receipt.site = profile_.id;
  receipt.step = offer.step;
  receipt.stage = offer.stage;
  receipt.from = offer.from_version;
  receipt.to = offer.to_version;
  receipt.accepted_generation = generation;
  receipt.evidence_digest = offer.stage_digest;
  receipt.idempotency_key = "accept-" + digest_hex;
  DCE_ASSIGN(receipt_id, ReceiptId::parse("receipt-" + digest_hex));
  receipt.id = receipt_id;

  CanonicalWriter writer;
  DCE_TRY(codec::put(writer, receipt));
  const Result<persist::CommitReceipt> appended =
      store_.append(persist::RecordType::migration_receipt, writer.bytes());
  if (!appended.ok()) {
    refuse_locked(result, appended.code(),
                  "the acceptance of stage " + number32(offer.stage.value()) +
                      " could not be recorded durably, so it is not accepted: " + appended.message());
    return Error{appended.code(), "site " + site_name(profile_.id) +
                                      " could not durably record the acceptance of stage " +
                                      number32(offer.stage.value()) + ": " + appended.message()};
  }
  if (!appended->durable) {
    refuse_locked(result, ErrorCode::io_error,
                  "the store did not confirm that the acceptance record is durable, so the stage is "
                  "not accepted");
    return Error{ErrorCode::io_error,
                 "site " + site_name(profile_.id) + " recorded the acceptance of stage " +
                     number32(offer.stage.value()) +
                     " but the store did not report the record durable"};
  }
  // The record is durable; only now is the stage published in memory.
  accepted_stage_ = offer.stage;
  accepted_generation_ = generation;
  stage_digest_ = offer.stage_digest;
  epoch_ = offer.epoch;
  plan_generation_ = offer.plan_generation;
  remember_locked(receipt);
  bump(accepted_stages_);
  result.status = ErrorCode::ok;
  result.receipt = receipt;
  result.text = "stage " + number32(offer.stage.value()) + " accepted at generation " +
                number64(generation.value());
  return Status{};
}

Status SiteNode::Impl::replay_offer_locked(const wire::Message& offer, wire::Message& result) {
  const MigrationReceipt* stored = find_record_locked(offer.plan, offer.stage);
  if (stored == nullptr) {
    refuse_locked(result, ErrorCode::conflict,
                  "offer repeats stage " + number32(offer.stage.value()) +
                      " but no durable receipt for that acceptance is held");
    return Status{};
  }
  MigrationReceipt replay = *stored;
  // The recorded acceptance is durable and is never applied twice. Only the
  // epoch, and a plan generation that has moved on, are refreshed: a receipt
  // from a superseded epoch or generation would be fenced by the coordinator
  // instead of recognised as the duplicate it is.
  replay.epoch = offer.epoch;
  if (replay.plan_generation != offer.plan_generation) {
    replay.plan_generation = offer.plan_generation;
    DCE_ASSIGN(digest_hex,
               acceptance_digest(offer.plan, offer.plan_generation, profile_.id, offer.stage));
    replay.idempotency_key = "accept-" + digest_hex;
    DCE_ASSIGN(receipt_id, ReceiptId::parse("receipt-" + digest_hex));
    replay.id = receipt_id;
  }
  result.status = ErrorCode::ok;
  result.receipt = std::move(replay);
  result.text = "stage " + number32(offer.stage.value()) +
                " was already accepted and durably recorded; the recorded receipt is re-presented";
  return Status{};
}

void SiteNode::Impl::refuse_locked(wire::Message& result, ErrorCode code, std::string explanation) {
  result.status = code;
  result.text = std::move(explanation);
  result.receipt.reset();
  bump(refused_offers_);
}

void SiteNode::Impl::remember_locked(const MigrationReceipt& receipt) {
  records_.push_back(receipt);
  receipts_.push_back(receipt.id);
  if (records_.size() > kMaxReceipts) {
    records_.erase(records_.begin());
    receipts_.erase(receipts_.begin());
  }
}

const MigrationReceipt* SiteNode::Impl::find_record_locked(const EvolutionPlanId& plan,
                                                           const StageOrdinal& stage) const {
  for (const MigrationReceipt& receipt : records_) {
    if (receipt.plan == plan && receipt.stage == stage) {
      return &receipt;
    }
  }
  return nullptr;
}

// ---------------------------------------------------------------------------
// Rounds
// ---------------------------------------------------------------------------
Status SiteNode::Impl::run() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (running_) {
      return Error{ErrorCode::busy, "site " + site_name(profile_.id) +
                                        " is already running; run() is entered from one thread"};
    }
    running_ = true;
  }
  const Status outcome = run_loop();
  {
    std::lock_guard<std::mutex> lock(mutex_);
    running_ = false;
    connected_ = false;
  }
  return outcome;
}

Status SiteNode::Impl::run_loop() {
  while (true) {
    const Result<bool> round = run_once();
    if (!round.ok()) {
      return round.error();
    }
    if (!*round) {
      return Status{};
    }
    bool finished = false;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      bump(rounds_);
      finished = stop_requested_ ||
                 (options_.exit_after_rounds != 0 && rounds_ >= options_.exit_after_rounds);
    }
    if (finished) {
      return Status{};
    }
    if (wait_between_rounds()) {
      return Status{};
    }
  }
}

Result<bool> SiteNode::Impl::run_once() {
  bool partitioned = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stop_requested_) {
      return false;
    }
    if (!store_.is_open()) {
      return Error{ErrorCode::closed, "site " + site_name(profile_.id) +
                                          " has no open store, so it can neither hold nor record an "
                                          "acceptance"};
    }
    partitioned = partitioned_;
  }
  if (partitioned) {
    return true;
  }

  Result<platform::Socket> connected = platform::Socket::connect_loopback(options_.coordinator_port);
  if (!connected.ok()) {
    std::lock_guard<std::mutex> lock(mutex_);
    connected_ = false;
    if (!accumulate(connect_failures_, std::uint32_t{1})) {
      return Error{ErrorCode::overflow,
                   "site " + site_name(profile_.id) + " exhausted its connection attempt counter"};
    }
    if (connect_failures_ >= options_.max_reconnect_attempts) {
      return Error{ErrorCode::io_error,
                   "site " + site_name(profile_.id) +
                       " could not reach the coordinator on 127.0.0.1:" +
                       number32(options_.coordinator_port) + " within " +
                       number32(options_.max_reconnect_attempts) +
                       " bounded connection attempts: " + connected.message()};
    }
    return true;
  }
  {
    std::lock_guard<std::mutex> lock(mutex_);
    connect_failures_ = 0;
  }
  platform::Socket socket = std::move(connected).value();
  return exchange(socket);
}

Result<bool> SiteNode::Impl::exchange(platform::Socket& socket) {
  const Result<std::uint64_t> hello_id = allocate_request_id();
  if (!hello_id.ok()) {
    return Error{hello_id.code(), hello_id.message()};
  }
  wire::Message hello;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    hello = hello_locked(*hello_id);
  }
  if (!wire::write_message(socket, hello).ok()) {
    return round_failure();
  }
  const Result<wire::Message> acknowledgement = wire::read_message(socket);
  if (!acknowledgement.ok()) {
    return round_failure();
  }
  if (acknowledgement->kind != wire::MessageKind::hello_ack ||
      acknowledgement->status != ErrorCode::ok) {
    return round_failure();
  }
  {
    std::lock_guard<std::mutex> lock(mutex_);
    epoch_ = acknowledgement->epoch;
    plan_generation_ = acknowledgement->plan_generation;
    connected_ = true;
  }

  const Result<std::uint64_t> request_id = allocate_request_id();
  if (!request_id.ok()) {
    return Error{request_id.code(), request_id.message()};
  }
  wire::Message request;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    request = reconcile_locked(*request_id);
  }
  if (!wire::write_message(socket, request).ok()) {
    return round_failure();
  }

  // A coordinator answers the site's report with its reconciliation result and
  // then with the directive that follows from it. The reconciliation result
  // carries no stage authority; the directive does.
  bool answered = false;
  for (std::size_t index = 0; index < kMaxRepliesPerRound; ++index) {
    const Result<wire::Message> reply = wire::read_message(socket);
    if (!reply.ok()) {
      return round_failure();
    }
    switch (reply->kind) {
      case wire::MessageKind::reconcile_result:
        if (!answered) {
          count_reconciliation();
          answered = true;
        }
        continue;
      case wire::MessageKind::stage_offer:
        if (!answered) {
          count_reconciliation();
          answered = true;
        }
        return handle_offer(socket, *reply);
      case wire::MessageKind::pause:
        if (!answered) {
          count_reconciliation();
          answered = true;
        }
        return true;
      case wire::MessageKind::stop:
        if (!answered) {
          count_reconciliation();
          answered = true;
        }
        return false;
      default:
        return round_failure();
    }
  }
  return round_failure();
}

Result<bool> SiteNode::Impl::handle_offer(platform::Socket& socket, const wire::Message& offer) {
  wire::Message result;
  Status durability;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    const Result<std::uint64_t> request_id = allocate_request_id_locked();
    if (!request_id.ok()) {
      return Error{request_id.code(), request_id.message()};
    }
    result.kind = wire::MessageKind::stage_result;
    result.request_id = *request_id;
    result.site = profile_.id;
    result.plan = offer.plan;
    result.plan_generation = offer.plan_generation;
    result.epoch = offer.epoch;
    result.stage = offer.stage;
    durability = decide_offer_locked(offer, result);
  }
  const Status sent = wire::write_message(socket, result);
  if (!sent.ok() || !durability.ok()) {
    mark_disconnected();
  }
  if (!durability.ok()) {
    // The refusal has been answered; the site cannot serve its durability
    // contract, so the run ends rather than pretending otherwise.
    return Error{durability.code(), durability.message()};
  }
  return true;
}

Result<bool> SiteNode::Impl::round_failure() {
  mark_disconnected();
  return true;
}

void SiteNode::Impl::mark_disconnected() {
  std::lock_guard<std::mutex> lock(mutex_);
  connected_ = false;
}

void SiteNode::Impl::count_reconciliation() {
  std::lock_guard<std::mutex> lock(mutex_);
  bump(reconciliations_);
}

bool SiteNode::Impl::wait_between_rounds() {
  std::unique_lock<std::mutex> lock(mutex_);
  if (stop_requested_) {
    return true;
  }
  wake_.wait_for(lock, kRoundPause, [this] { return stop_requested_; });
  return stop_requested_;
}

void SiteNode::Impl::request_stop() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stop_requested_ = true;
  }
  wake_.notify_all();
}

void SiteNode::Impl::set_partitioned(bool partitioned) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    partitioned_ = partitioned;
    if (partitioned) {
      // A partitioned site is not connected to anything; it keeps serving
      // locally and keeps its accepted stage exactly as it was.
      connected_ = false;
    }
  }
  wake_.notify_all();
}

SiteNodeStatus SiteNode::Impl::status() const {
  std::lock_guard<std::mutex> lock(mutex_);
  SiteNodeStatus snapshot;
  snapshot.site = profile_.id;
  snapshot.accepted_generation = accepted_generation_;
  snapshot.accepted_stage = accepted_stage_;
  snapshot.stage_digest = stage_digest_;
  snapshot.connected = connected_;
  snapshot.partitioned = partitioned_;
  snapshot.accepted_stages = accepted_stages_;
  snapshot.refused_offers = refused_offers_;
  snapshot.reconciliations = reconciliations_;
  snapshot.rounds = rounds_;
  snapshot.epoch = epoch_;
  return snapshot;
}

Result<SiteStageReport> SiteNode::Impl::report() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return report_locked();
}

Status SiteNode::Impl::close() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!store_.is_open()) {
    return Status{};
  }
  return store_.close();
}

Status SiteNode::Impl::initialise() {
  if (options_.store_directory.empty()) {
    return Error{ErrorCode::invalid_argument,
                 "site " + site_name(profile_.id) + " was given no store directory"};
  }
  if (!profile_.id.valid()) {
    return Error{ErrorCode::invalid_argument,
                 "a site node requires a valid site identity in its profile"};
  }
  return open_and_recover();
}

// ---------------------------------------------------------------------------
// SiteNode
// ---------------------------------------------------------------------------
SiteNode::~SiteNode() = default;

Result<std::unique_ptr<SiteNode>> SiteNode::start(const SiteNodeOptions& options) {
  auto node = std::make_unique<SiteNode>();
  node->impl_ = std::make_unique<Impl>(options);
  DCE_TRY(node->impl_->initialise());
  return node;
}

Status SiteNode::run() {
  if (impl_ == nullptr) {
    return Error{ErrorCode::closed, "the site node is not started"};
  }
  return impl_->run();
}

void SiteNode::request_stop() {
  if (impl_ != nullptr) {
    impl_->request_stop();
  }
}

void SiteNode::set_partitioned(bool partitioned) {
  if (impl_ != nullptr) {
    impl_->set_partitioned(partitioned);
  }
}

SiteNodeStatus SiteNode::status() const {
  if (impl_ == nullptr) {
    return SiteNodeStatus{};
  }
  return impl_->status();
}

Result<SiteStageReport> SiteNode::report() const {
  if (impl_ == nullptr) {
    return Error{ErrorCode::closed, "the site node is not started"};
  }
  return impl_->report();
}

Status SiteNode::close() {
  if (impl_ == nullptr) {
    return Status{};
  }
  return impl_->close();
}

// ---------------------------------------------------------------------------
// AdminClient
// ---------------------------------------------------------------------------
struct AdminClient::Impl {
  [[nodiscard]] Result<wire::AdminResponse> call(const wire::AdminRequest& request);
  Status close();

  platform::Socket socket;
  mutable std::mutex mutex;
  bool closed{false};
};

Result<wire::AdminResponse> AdminClient::Impl::call(const wire::AdminRequest& request) {
  std::lock_guard<std::mutex> lock(mutex);
  if (closed || !socket.is_open()) {
    return Error{ErrorCode::closed, "the admin client is not connected"};
  }
  DCE_ASSIGN(body, wire::encode_admin_request(request));
  const Status sent = wire::write_frame(socket, wire::kAdminFrameKind, body);
  if (!sent.ok()) {
    return Error{sent.code(), "admin request " + number64(request.request_id) +
                                  " could not be sent: " + sent.message()};
  }
  std::uint16_t kind = 0;
  DCE_ASSIGN(frame, wire::read_frame(socket, kind));
  if (kind != wire::kAdminFrameKind) {
    return Error{ErrorCode::malformed, "the admin reply arrived as frame kind " +
                                           number32(kind) +
                                           ", which is not the administrative frame kind"};
  }
  DCE_ASSIGN(response, wire::decode_admin_response(frame));
  if (response.request_id != request.request_id) {
    return Error{ErrorCode::conflict,
                 "the admin reply carries request id " + number64(response.request_id) +
                     " but the request was " + number64(request.request_id)};
  }
  return response;
}

Status AdminClient::Impl::close() {
  std::lock_guard<std::mutex> lock(mutex);
  if (closed) {
    return Status{};
  }
  closed = true;
  if (!socket.is_open()) {
    return Status{};
  }
  return socket.close();
}

AdminClient::~AdminClient() = default;

Result<std::unique_ptr<AdminClient>> AdminClient::connect(std::uint16_t port) {
  Result<platform::Socket> socket = platform::Socket::connect_loopback(port);
  if (!socket.ok()) {
    return Error{socket.code(), "the admin client could not reach the coordinator on 127.0.0.1:" +
                                    number32(port) + ": " + socket.message()};
  }
  auto client = std::make_unique<AdminClient>();
  client->impl_ = std::make_unique<Impl>();
  client->impl_->socket = std::move(socket).value();
  return client;
}

Result<wire::AdminResponse> AdminClient::call(const wire::AdminRequest& request) {
  if (impl_ == nullptr) {
    return Error{ErrorCode::closed, "the admin client is not connected"};
  }
  return impl_->call(request);
}

Status AdminClient::close() {
  if (impl_ == nullptr) {
    return Status{};
  }
  return impl_->close();
}

}  // namespace dce
