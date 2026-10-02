// Benchmarks for the DCE evolution boundary.
//
// This program is a measurement tool, not a test. It prints timing figures that
// were taken on the machine that runs it, in the build that produced it, during
// the run itself, and it is not registered with CTest so that a timing number
// can never fail a correctness build.
//
// Two different claims are measured and are never conflated:
//
//   A. PURE DECISION COST. validate_plan over a synthetic fleet, and a full
//      deterministic rollout sweep through next_rollout_action until the action
//      is completed. No bytes are written anywhere: this is CPU work with no
//      durability in it.
//
//   B. DURABLE STAGE COMMIT COST. persist::Store::append of a real
//      migration-receipt record into a real store directory under the system
//      temporary directory. The row with sync_on_commit = true is the only
//      durability claim this output makes, because it is the row that includes
//      platform::File::sync(). The row with sync_on_commit = false is a cost
//      comparison and is not a durability claim of any kind.
//
// A third, separate measurement covers a checkpoint-then-recover cycle.
//
// Every printed figure is computed from a measurement taken during this run.
// There is no baseline in this repository, so no before/after comparison is
// printed anywhere.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "dce/authority.hpp"
#include "dce/canonical.hpp"
#include "dce/codec.hpp"
#include "dce/ids.hpp"
#include "dce/plan.hpp"
#include "dce/platform.hpp"
#include "dce/rollout.hpp"
#include "dce/status.hpp"
#include "dce/store.hpp"
#include "dce/synthetic.hpp"
#include "dce/validate.hpp"

namespace {

using dce::Error;
using dce::ErrorCode;
using dce::Result;
using dce::Status;

// ---------------------------------------------------------------------------
// Outcomes and small formatting helpers
// ---------------------------------------------------------------------------

[[nodiscard]] Status failure(ErrorCode code, std::string message) {
  return Status{Error{code, std::move(message)}};
}

// Moves a successful result into out and turns a failed one into an error that
// names the operation it came from.
template <class T>
[[nodiscard]] Status take(Result<T>& result, T& out, std::string_view operation) {
  if (!result.ok()) {
    return failure(result.code(), std::string(operation) + ": " + result.message());
  }
  out = std::move(result).value();
  return Status{};
}

// The only clock this program reads. Monotonic nanoseconds, never a wall clock.
[[nodiscard]] std::uint64_t now_nanos() noexcept { return dce::platform::monotonic_nanos(); }

struct Timing {
  std::size_t samples{0};
  std::uint64_t total_nanos{0};
  std::uint64_t min_nanos{0};
  std::uint64_t median_nanos{0};
  std::uint64_t mean_nanos{0};
};

// Sorts the per-iteration samples and reduces them to the reported figures.
// Every value here is derived from measured nanoseconds by integer arithmetic.
[[nodiscard]] Timing summarise(std::vector<std::uint64_t>& samples) {
  Timing timing;
  timing.samples = samples.size();
  if (samples.empty()) {
    return timing;
  }
  std::sort(samples.begin(), samples.end());
  for (const std::uint64_t sample : samples) {
    timing.total_nanos += sample;
  }
  timing.min_nanos = samples.front();
  const std::size_t middle = samples.size() / 2;
  timing.median_nanos = (samples.size() % 2 == 0)
                            ? (samples[middle - 1] + samples[middle]) / 2
                            : samples[middle];
  timing.mean_nanos = timing.total_nanos / static_cast<std::uint64_t>(samples.size());
  return timing;
}

[[nodiscard]] std::string pad3(std::uint64_t value) {
  std::string text = std::to_string(value);
  while (text.size() < 3) {
    text.insert(text.begin(), '0');
  }
  return text;
}

// Nanoseconds as milliseconds with three decimals.
[[nodiscard]] std::string millis_text(std::uint64_t nanos) {
  const std::uint64_t micros = nanos / 1000;
  return std::to_string(micros / 1000) + "." + pad3(micros % 1000);
}

// Nanoseconds as microseconds with three decimals.
[[nodiscard]] std::string micros_text(std::uint64_t nanos) {
  return std::to_string(nanos / 1000) + "." + pad3(nanos % 1000);
}

void print_cells(const std::vector<std::string>& cells, const std::vector<std::size_t>& widths) {
  for (std::size_t index = 0; index < cells.size(); ++index) {
    std::cout << std::left << std::setw(static_cast<int>(widths[index])) << cells[index];
  }
  std::cout << '\n';
}

void print_table_head(const std::vector<std::string>& headers,
                      const std::vector<std::size_t>& widths) {
  print_cells(headers, widths);
  std::vector<std::string> rule;
  rule.reserve(widths.size());
  for (const std::size_t width : widths) {
    rule.push_back(std::string(width - 1, '-') + " ");
  }
  print_cells(rule, widths);
}

void print_field(const std::string& name, const std::string& value) {
  std::cout << "  " << std::left << std::setw(20) << name << ": " << value << '\n';
}

// ---------------------------------------------------------------------------
// Environment
// ---------------------------------------------------------------------------

[[nodiscard]] std::string compiler_text() {
  std::string text;
#if defined(__clang__)
  text = "Clang " + std::to_string(__clang_major__) + "." + std::to_string(__clang_minor__) + "." +
         std::to_string(__clang_patchlevel__);
#elif defined(_MSC_VER)
  text = "MSVC _MSC_VER=" + std::to_string(_MSC_VER);
#elif defined(__GNUC__)
  text = "GCC " + std::to_string(__GNUC__) + "." + std::to_string(__GNUC_MINOR__) + "." +
         std::to_string(__GNUC_PATCHLEVEL__);
#else
  text = "unknown compiler";
#endif
  text += " (__VERSION__: ";
  text += __VERSION__;
  text += ")";
  return text;
}

[[nodiscard]] const char* platform_text() noexcept {
#if defined(__MINGW64__)
  return "Windows (MinGW-w64)";
#elif defined(_WIN32)
  return "Windows";
#elif defined(__linux__)
  return "Linux";
#else
  return "unknown platform";
#endif
}

[[nodiscard]] const char* build_text() noexcept {
#if defined(NDEBUG)
  return "Release-style (NDEBUG is defined)";
#else
  return "Debug-style (NDEBUG is not defined)";
#endif
}

// ---------------------------------------------------------------------------
// Section A: pure decision cost
// ---------------------------------------------------------------------------

constexpr std::uint64_t kFleetSeed = 20240607;

struct FleetShape {
  std::size_t sites;
  std::size_t components;
  std::size_t versions_per_component;
  std::size_t cohorts;
  std::size_t validate_samples;
  std::size_t sweep_samples;
};

// The size sweep. Components grow with the fleet, versions per component stay
// fixed, and the seed is fixed, so every row is reproducible from this table.
constexpr FleetShape kFleetShapes[] = {
    {8, 1, 3, 2, 400, 100},
    {64, 2, 3, 2, 150, 20},
    {256, 4, 3, 4, 40, 4},
    {512, 8, 3, 4, 20, 3},
};

struct DecisionResult {
  FleetShape shape{};
  std::size_t steps{0};
  std::size_t cohorts{0};
  Timing validate{};
  std::uint64_t refusals{0};
  std::uint64_t checked_sites{0};
  std::uint64_t checked_edges{0};
  Timing sweeps{};
  std::uint64_t decisions_per_sweep{0};
  std::uint64_t total_decisions{0};
};

[[nodiscard]] dce::SiteProgress* find_progress(dce::RolloutState& state, const dce::SiteId& id) {
  for (dce::SiteProgress& progress : state.sites) {
    if (progress.site == id) {
      return &progress;
    }
  }
  return nullptr;
}

// One full sweep: ask for the next decision and apply the acceptance that an
// advance_site decision describes, until the decision is completed. The site
// accepts its target stage immediately, so the sweep measures the cost of the
// decisions themselves and not the cost of waiting for receipts.
[[nodiscard]] Status run_rollout_sweeps(const dce::EvolutionPlan& plan,
                                        const dce::RolloutState& initial, std::size_t sweeps,
                                        std::vector<std::uint64_t>& samples,
                                        std::uint64_t& decisions_per_sweep,
                                        std::uint64_t& total_decisions) {
  samples.clear();
  samples.reserve(sweeps);
  for (std::size_t sweep = 0; sweep < sweeps; ++sweep) {
    dce::RolloutState state = initial;
    std::uint64_t decisions = 0;
    bool finished = false;
    const std::uint64_t begin = now_nanos();
    while (!finished) {
      Result<dce::RolloutAction> action_result = dce::next_rollout_action(plan, state);
      if (!action_result.ok()) {
        return failure(action_result.code(), "next_rollout_action: " + action_result.message());
      }
      const dce::RolloutAction& action = action_result.value();
      ++decisions;
      switch (action.kind) {
        case dce::RolloutActionKind::completed:
          finished = true;
          break;
        case dce::RolloutActionKind::advance_site: {
          dce::SiteProgress* progress = find_progress(state, action.site);
          if (progress == nullptr) {
            return failure(ErrorCode::internal,
                           "the rollout chose a site that the rollout state does not describe");
          }
          progress->accepted_stage = action.target_stage;
          break;
        }
        case dce::RolloutActionKind::advance_wave:
          state.current_wave = action.wave;
          break;
        default:
          return failure(ErrorCode::internal, std::string("the rollout could not complete: ") +
                                                  dce::to_string(action.kind) + ": " +
                                                  action.explanation);
      }
    }
    samples.push_back(now_nanos() - begin);
    if (sweep == 0) {
      decisions_per_sweep = decisions;
    } else if (decisions != decisions_per_sweep) {
      return failure(ErrorCode::internal,
                     "the rollout sweep is not deterministic: one sweep took " +
                         std::to_string(decisions) + " decisions and an earlier sweep took " +
                         std::to_string(decisions_per_sweep));
    }
    total_decisions += decisions;
  }
  return Status{};
}

[[nodiscard]] Status measure_decisions(std::vector<DecisionResult>& results) {
  results.clear();
  results.reserve(std::size(kFleetShapes));
  for (const FleetShape& shape : kFleetShapes) {
    dce::SyntheticFleetOptions options;
    options.sites = shape.sites;
    options.components = shape.components;
    options.versions_per_component = shape.versions_per_component;
    options.cohorts = shape.cohorts;
    options.seed = kFleetSeed;

    Result<dce::SyntheticFleet> fleet_result = dce::make_synthetic_fleet(options);
    dce::SyntheticFleet fleet;
    const Status built = take(fleet_result, fleet, "make_synthetic_fleet");
    if (!built.ok()) {
      return failure(built.code(), built.message() + " (sites " + std::to_string(shape.sites) + ")");
    }

    DecisionResult result;
    result.shape = shape;
    result.steps = fleet.plan.steps.size();
    result.cohorts = fleet.plan.cohorts.size();

    dce::ValidationContext context;
    context.epoch = fleet.plan.identity.epoch;
    context.current_generation = fleet.plan.identity.generation;
    context.observed_sites = fleet.initial_observations;

    std::vector<std::uint64_t> validate_samples;
    validate_samples.reserve(shape.validate_samples);
    for (std::size_t sample = 0; sample < shape.validate_samples; ++sample) {
      const std::uint64_t begin = now_nanos();
      Result<dce::ValidationReport> report_result = dce::validate_plan(fleet.plan, context);
      const std::uint64_t end = now_nanos();
      if (!report_result.ok()) {
        return failure(report_result.code(), "validate_plan: " + report_result.message());
      }
      const dce::ValidationReport& report = report_result.value();
      if (sample == 0) {
        result.checked_sites = report.checked_sites;
        result.checked_edges = report.checked_edges;
        if (!report.accepted()) {
          const dce::Refusal& first = report.refusals.front();
          return failure(ErrorCode::internal,
                         "the synthetic fleet of " + std::to_string(shape.sites) +
                             " sites did not validate against its own initial observations (" +
                             std::to_string(report.refusals.size()) + " refusals, first: " +
                             dce::to_string(first.code) + " at " + first.subject + ": " +
                             first.explanation +
                             "); the validation timing would not be a full decision sweep");
        }
      }
      result.refusals += static_cast<std::uint64_t>(report.refusals.size());
      validate_samples.push_back(end - begin);
    }
    result.validate = summarise(validate_samples);

    Result<dce::RolloutState> initial_result = dce::initial_rollout_state(fleet.plan);
    dce::RolloutState initial;
    const Status initialised = take(initial_result, initial, "initial_rollout_state");
    if (!initialised.ok()) {
      return failure(initialised.code(), initialised.message());
    }

    std::vector<std::uint64_t> sweep_samples;
    const Status swept = run_rollout_sweeps(fleet.plan, initial, shape.sweep_samples, sweep_samples,
                                            result.decisions_per_sweep, result.total_decisions);
    if (!swept.ok()) {
      return failure(swept.code(), swept.message() + " (sites " + std::to_string(shape.sites) + ")");
    }
    result.sweeps = summarise(sweep_samples);
    results.push_back(result);
  }
  return Status{};
}

void print_decision_tables(const std::vector<DecisionResult>& results) {
  std::cout << "A. PURE DECISION COST (no durable write is performed in this section)\n";
  std::cout << "Legend: this section measures the cost of the boundary's decisions and nothing else.\n";
  std::cout << "No store is opened and no byte is written to disk. Fleets are generated by\n";
  std::cout << "make_synthetic_fleet with the fixed seed printed above, so every row is reproducible.\n";
  std::cout << "min and median are the smallest and the middle per-iteration sample; mean is the\n";
  std::cout << "arithmetic mean of the same samples. 'total' is the elapsed time of all samples\n";
  std::cout << "together, measured with platform::monotonic_nanos().\n\n";

  std::cout << "A1. validate_plan against the fleet's own initial observations\n";
  std::cout << "    One sample is one validate_plan call on the generated fleet.\n";
  const std::vector<std::string> validate_headers = {
      "sites",    "comps",   "vers",    "cohorts",  "steps",   "samples",   "total ms",
      "mean us",  "min us",  "median us", "checked sites/edges", "refusals"};
  const std::vector<std::size_t> validate_widths = {8, 7, 6, 9, 7, 9, 11, 11, 11, 11, 22, 10};
  print_table_head(validate_headers, validate_widths);
  for (const DecisionResult& result : results) {
    print_cells({std::to_string(result.shape.sites),
                 std::to_string(result.shape.components),
                 std::to_string(result.shape.versions_per_component),
                 std::to_string(result.cohorts),
                 std::to_string(result.steps),
                 std::to_string(result.validate.samples),
                 millis_text(result.validate.total_nanos),
                 micros_text(result.validate.mean_nanos),
                 micros_text(result.validate.min_nanos),
                 micros_text(result.validate.median_nanos),
                 std::to_string(result.checked_sites) + "/" + std::to_string(result.checked_edges),
                 std::to_string(result.refusals)},
                validate_widths);
  }
  std::cout << "\n";

  std::cout << "A2. full deterministic rollout sweep\n";
  std::cout << "    One sample is one complete sweep: next_rollout_action is called until the\n";
  std::cout << "    action is completed, and every advance_site decision is applied before the next\n";
  std::cout << "    decision is requested. 'decisions' counts those calls; 'us/decision' is the\n";
  std::cout << "    measured total time divided by the measured number of decisions. The per-decision\n";
  std::cout << "    cost rises with the fleet size in the table itself, because a decision rescans the\n";
  std::cout << "    rollout state; that is why the sweep stops at the largest row shown.\n";
  const std::vector<std::string> sweep_headers = {"sites",   "steps",       "cohorts",
                                                  "sweeps",  "decisions",   "total decisions",
                                                  "total ms", "mean ms",    "min ms",
                                                  "median ms", "us/decision"};
  const std::vector<std::size_t> sweep_widths = {8, 7, 9, 8, 11, 16, 11, 11, 11, 11, 13};
  print_table_head(sweep_headers, sweep_widths);
  for (const DecisionResult& result : results) {
    const std::uint64_t per_decision =
        result.total_decisions == 0 ? 0 : result.sweeps.total_nanos / result.total_decisions;
    print_cells({std::to_string(result.shape.sites),
                 std::to_string(result.steps),
                 std::to_string(result.cohorts),
                 std::to_string(result.sweeps.samples),
                 std::to_string(result.decisions_per_sweep),
                 std::to_string(result.total_decisions),
                 millis_text(result.sweeps.total_nanos),
                 millis_text(result.sweeps.mean_nanos),
                 millis_text(result.sweeps.min_nanos),
                 millis_text(result.sweeps.median_nanos),
                 micros_text(per_decision)},
                sweep_widths);
  }
  std::cout << "\n";
}

// ---------------------------------------------------------------------------
// Section B and C: real store directories under the system temporary directory
// ---------------------------------------------------------------------------

constexpr std::size_t kStoreSites = 64;
constexpr std::size_t kStoreComponents = 4;
constexpr std::size_t kStoreVersions = 3;
constexpr std::size_t kStoreCohorts = 4;
constexpr std::size_t kStoreAppends = 512;
constexpr std::size_t kRecoveryBeforeSnapshot = 384;
constexpr std::size_t kRecoveryAfterSnapshot = 128;
constexpr std::size_t kRecoverySamples = 32;

// Builds one real migration receipt per (site, step) pair of the synthetic plan
// and encodes each with codec::put. Every encoded record is decoded again and
// compared before it is used, so a payload that is not a faithful receipt
// encoding stops the run instead of being timed.
[[nodiscard]] Status build_receipt_pool(const dce::EvolutionPlan& plan, std::size_t receipts,
                                        std::vector<std::vector<std::byte>>& payloads) {
  if (plan.membership.sites.empty() || plan.steps.empty()) {
    return failure(ErrorCode::invalid_argument,
                   "the synthetic plan has no sites or no steps to build receipts from");
  }
  payloads.clear();
  payloads.reserve(receipts);
  const std::size_t site_count = plan.membership.sites.size();
  for (std::size_t index = 0; index < receipts; ++index) {
    const std::size_t step_index = (index / site_count) % plan.steps.size();
    const dce::SiteRecord& site = plan.membership.sites[index % site_count];
    const dce::MigrationStep& step = plan.steps[step_index];

    dce::MigrationReceipt receipt;
    Result<dce::ReceiptId> identifier = dce::ReceiptId::parse("receipt." + std::to_string(index));
    if (!identifier.ok()) {
      return failure(identifier.code(), "ReceiptId::parse: " + identifier.message());
    }
    receipt.id = std::move(identifier).value();
    receipt.plan = plan.identity.id;
    receipt.plan_generation = plan.identity.generation;
    receipt.epoch = plan.identity.epoch;
    receipt.site = site.id;
    receipt.step = step.id;
    receipt.stage = dce::StageOrdinal::from_value(static_cast<std::uint32_t>(step_index) + 1u);
    receipt.from = step.from;
    receipt.to = step.to;
    receipt.accepted_generation = site.accepted_generation;
    receipt.idempotency_key = "idempotency." + std::to_string(index);

    dce::CanonicalWriter evidence;
    DCE_TRY(dce::codec::put(evidence, receipt.id));
    DCE_TRY(dce::codec::put(evidence, receipt.step));
    receipt.evidence_digest = evidence.digest();

    dce::CanonicalWriter writer;
    DCE_TRY(dce::codec::put(writer, receipt));

    dce::CanonicalReader reader(writer.bytes());
    Result<dce::MigrationReceipt> decoded = dce::codec::get<dce::MigrationReceipt>(reader);
    if (!decoded.ok()) {
      return failure(decoded.code(), "the encoded receipt does not decode: " + decoded.message());
    }
    const Status consumed = reader.expect_end();
    if (!consumed.ok()) {
      return failure(consumed.code(), "the encoded receipt has trailing bytes: " + consumed.message());
    }
    if (!(decoded.value() == receipt)) {
      return failure(ErrorCode::internal, "the encoded receipt does not survive a codec round trip");
    }
    payloads.push_back(writer.bytes());
  }
  return Status{};
}

// A full state image of the kind a coordinator checkpoints: the canonical
// encoding of one SiteProgress per member site.
[[nodiscard]] Status build_state_image(const dce::EvolutionPlan& plan, std::vector<std::byte>& image) {
  dce::CanonicalWriter writer;
  DCE_TRY(writer.put_count(plan.membership.sites.size()));
  for (const dce::SiteRecord& record : plan.membership.sites) {
    dce::SiteProgress progress;
    progress.site = record.id;
    progress.generation = record.accepted_generation;
    progress.stage_digest = record.state_digest;
    for (const dce::RolloutCohort& cohort : plan.cohorts) {
      for (const dce::SiteId& member : cohort.sites) {
        if (member == record.id) {
          progress.cohort = cohort.id;
          progress.wave = cohort.wave;
        }
      }
    }
    DCE_TRY(dce::codec::put(writer, progress));
  }
  image = writer.bytes();
  return Status{};
}

struct AppendResult {
  bool sync_on_commit{false};
  std::size_t appends{0};
  std::size_t payload_bytes{0};
  std::uint64_t receipts_reporting_durable{0};
  dce::LogSequence last_sequence{};
  Timing timing{};
};

[[nodiscard]] Status measure_appends(const std::string& directory, bool sync_on_commit,
                                     const std::vector<std::vector<std::byte>>& payloads,
                                     std::size_t appends, AppendResult& result) {
  if (payloads.empty() || appends == 0) {
    return failure(ErrorCode::invalid_argument, "the append measurement needs receipts to append");
  }
  const Status created = dce::platform::make_directories(directory);
  if (!created.ok()) {
    return failure(created.code(), "make_directories(" + directory + "): " + created.message());
  }

  dce::persist::StoreOptions options;
  options.sync_on_commit = sync_on_commit;
  Result<dce::persist::Store> opened =
      dce::persist::Store::open(directory, options, dce::persist::OpenMode::read_write);
  if (!opened.ok()) {
    return failure(opened.code(), "Store::open(" + directory + ", read_write): " + opened.message());
  }
  dce::persist::Store store = std::move(opened).value();

  std::vector<std::uint64_t> samples;
  samples.reserve(appends);
  for (std::size_t index = 0; index < appends; ++index) {
    const std::vector<std::byte>& payload = payloads[index % payloads.size()];
    const std::uint64_t begin = now_nanos();
    Result<dce::persist::CommitReceipt> receipt =
        store.append(dce::persist::RecordType::migration_receipt, payload);
    const std::uint64_t end = now_nanos();
    if (!receipt.ok()) {
      return failure(receipt.code(), "Store::append(migration_receipt): " + receipt.message());
    }
    if (receipt->durable) {
      ++result.receipts_reporting_durable;
    }
    result.last_sequence = receipt->sequence;
    samples.push_back(end - begin);
  }

  result.sync_on_commit = sync_on_commit;
  result.appends = appends;
  result.payload_bytes = payloads.front().size();
  result.timing = summarise(samples);

  const Status closed = store.close();
  if (!closed.ok()) {
    return failure(closed.code(), "Store::close(" + directory + "): " + closed.message());
  }
  return Status{};
}

struct RecoveryResult {
  std::size_t appends_before_snapshot{0};
  std::size_t appends_after_snapshot{0};
  std::size_t snapshot_bytes{0};
  std::uint64_t records_recovered{0};
  std::size_t records_replayed{0};
  std::size_t snapshot_bytes_replayed{0};
  std::uint64_t bytes_scanned{0};
  std::uint64_t segments_scanned{0};
  std::uint64_t torn_tail_bytes{0};
  bool snapshot_loaded{false};
  dce::LogSequence last_sequence{};
  Timing timing{};
};

[[nodiscard]] Status measure_checkpoint_recovery(const std::string& directory,
                                                 const dce::EvolutionPlan& plan,
                                                 const std::vector<std::vector<std::byte>>& payloads,
                                                 RecoveryResult& result) {
  if (payloads.empty()) {
    return failure(ErrorCode::invalid_argument, "the recovery measurement needs receipts to append");
  }
  const Status created = dce::platform::make_directories(directory);
  if (!created.ok()) {
    return failure(created.code(), "make_directories(" + directory + "): " + created.message());
  }

  dce::persist::StoreOptions options;
  Result<dce::persist::Store> opened =
      dce::persist::Store::open(directory, options, dce::persist::OpenMode::read_write);
  if (!opened.ok()) {
    return failure(opened.code(), "Store::open(" + directory + ", read_write): " + opened.message());
  }
  dce::persist::Store store = std::move(opened).value();

  const std::size_t total = kRecoveryBeforeSnapshot + kRecoveryAfterSnapshot;
  for (std::size_t index = 0; index < total; ++index) {
    if (index == kRecoveryBeforeSnapshot) {
      std::vector<std::byte> image;
      DCE_TRY(build_state_image(plan, image));
      result.snapshot_bytes = image.size();
      const Status snapshotted = store.snapshot(image);
      if (!snapshotted.ok()) {
        return failure(snapshotted.code(), "Store::snapshot: " + snapshotted.message());
      }
    }
    Result<dce::persist::CommitReceipt> receipt =
        store.append(dce::persist::RecordType::migration_receipt, payloads[index % payloads.size()]);
    if (!receipt.ok()) {
      return failure(receipt.code(), "Store::append(migration_receipt): " + receipt.message());
    }
    result.last_sequence = receipt->sequence;
  }
  result.appends_before_snapshot = kRecoveryBeforeSnapshot;
  result.appends_after_snapshot = kRecoveryAfterSnapshot;

  const Status closed = store.close();
  if (!closed.ok()) {
    return failure(closed.code(), "Store::close(" + directory + "): " + closed.message());
  }

  std::vector<std::uint64_t> samples;
  samples.reserve(kRecoverySamples);
  for (std::size_t sample = 0; sample < kRecoverySamples; ++sample) {
    const std::uint64_t begin = now_nanos();
    Result<dce::persist::Store> reopened =
        dce::persist::Store::open(directory, options, dce::persist::OpenMode::read_only);
    const std::uint64_t end = now_nanos();
    if (!reopened.ok()) {
      return failure(reopened.code(),
                     "Store::open(" + directory + ", read_only) recovery: " + reopened.message());
    }
    dce::persist::Store reader = std::move(reopened).value();
    const dce::persist::RecoveryReport report = reader.recovery();
    if (sample == 0) {
      result.records_recovered = report.records_recovered;
      result.bytes_scanned = report.bytes_scanned;
      result.segments_scanned = report.segments_scanned;
      result.torn_tail_bytes = report.torn_tail_bytes_discarded;
      result.snapshot_loaded = report.snapshot_loaded;
      if (!report.snapshot_loaded) {
        return failure(ErrorCode::internal,
                       "recovery did not load the checkpoint that was written before the restart");
      }
      if (!(report.committed_sequence == result.last_sequence)) {
        return failure(ErrorCode::internal,
                       "recovery reported committed sequence " +
                           std::to_string(report.committed_sequence.value()) +
                           " but the last appended sequence was " +
                           std::to_string(result.last_sequence.value()));
      }
      // One explicit replay, outside the timed region, so the number of
      // recovered records is checked against the records themselves.
      Result<dce::persist::RecoveredState> replayed = reader.recover();
      if (!replayed.ok()) {
        return failure(replayed.code(), "Store::recover(" + directory + "): " + replayed.message());
      }
      result.records_replayed = replayed->records.size();
      result.snapshot_bytes_replayed = replayed->snapshot.size();
    }
    const Status closed_reader = reader.close();
    if (!closed_reader.ok()) {
      return failure(closed_reader.code(),
                     "Store::close(" + directory + ", read_only): " + closed_reader.message());
    }
    samples.push_back(end - begin);
  }
  result.timing = summarise(samples);
  return Status{};
}

void print_store_tables(const AppendResult& durable, const AppendResult& non_durable,
                        const RecoveryResult& recovery) {
  std::cout << "B. DURABLE STAGE COMMIT COST (a real store directory, real bytes on disk)\n";
  std::cout << "Legend: each row appends one real migration-receipt record per call with\n";
  std::cout << "persist::Store::append into its own directory under the store root listed above.\n";
  std::cout << "The payload is a codec::put encoding of a MigrationReceipt built from the synthetic\n";
  std::cout << "plan, encoded before the timed region: only append() is timed. One sample is one\n";
  std::cout << "append. min and median are the smallest and the middle sample, mean is their mean.\n";
  std::cout << "  sync_on_commit = true  : includes platform::File::sync(). This is the only row in\n";
  std::cout << "                           this output that makes a durability claim.\n";
  std::cout << "  sync_on_commit = false : the same append without the commit sync. It is a cost\n";
  std::cout << "                           comparison and is NOT a durability claim.\n";
  const std::vector<std::string> headers = {
      "mode",       "sync_on_commit", "appends",  "samples",   "payload B",        "total ms",
      "mean us",    "min us",         "median us", "last sequence", "receipts durable=true"};
  const std::vector<std::size_t> widths = {12, 16, 8, 9, 10, 11, 11, 11, 11, 15, 22};
  print_table_head(headers, widths);
  const auto row = [&widths](const char* mode, const AppendResult& result) {
    print_cells({mode,
                 result.sync_on_commit ? "true" : "false",
                 std::to_string(result.appends),
                 std::to_string(result.timing.samples),
                 std::to_string(result.payload_bytes),
                 millis_text(result.timing.total_nanos),
                 micros_text(result.timing.mean_nanos),
                 micros_text(result.timing.min_nanos),
                 micros_text(result.timing.median_nanos),
                 std::to_string(result.last_sequence.value()),
                 std::to_string(result.receipts_reporting_durable)},
                widths);
  };
  row("durable", durable);
  row("non-durable", non_durable);
  std::cout << "\n";

  std::cout << "C. CHECKPOINT THEN RECOVER (compaction and restart cost)\n";
  std::cout << "Legend: a store is filled with receipts, a full state image is written with\n";
  std::cout << "Store::snapshot, more receipts are appended after it, the store is closed, and the\n";
  std::cout << "cost of re-opening it is measured. One sample is one re-open. Re-opens use\n";
  std::cout << "OpenMode::read_only, so the timed work is the recovery itself: the epoch-advance\n";
  std::cout << "write that a read_write open performs is deliberately not part of this number.\n";
  const std::vector<std::string> recovery_headers = {"samples", "total ms", "mean ms", "min ms",
                                                     "median ms", "appended before/after snapshot",
                                                     "records recovered", "snapshot loaded"};
  const std::vector<std::size_t> recovery_widths = {9, 11, 11, 11, 11, 31, 18, 17};
  print_table_head(recovery_headers, recovery_widths);
  print_cells({std::to_string(recovery.timing.samples),
               millis_text(recovery.timing.total_nanos),
               millis_text(recovery.timing.mean_nanos),
               millis_text(recovery.timing.min_nanos),
               millis_text(recovery.timing.median_nanos),
               std::to_string(recovery.appends_before_snapshot) + " / " +
                   std::to_string(recovery.appends_after_snapshot),
               std::to_string(recovery.records_recovered),
               recovery.snapshot_loaded ? "yes" : "no"},
              recovery_widths);
  std::cout << "    checkpoint image bytes        : " << recovery.snapshot_bytes << "\n";
  std::cout << "    records reported by recovery  : " << recovery.records_recovered << "\n";
  std::cout << "    records replayed by recover() : " << recovery.records_replayed
            << " (replayed once, outside the timed region)\n";
  std::cout << "    checkpoint bytes replayed     : " << recovery.snapshot_bytes_replayed << "\n";
  std::cout << "    recovery bytes scanned        : " << recovery.bytes_scanned << "\n";
  std::cout << "    log segments scanned          : " << recovery.segments_scanned << "\n";
  std::cout << "    torn tail bytes discarded     : " << recovery.torn_tail_bytes << "\n";
  std::cout << "    committed sequence recovered  : " << recovery.last_sequence.value()
            << " (verified equal to the last appended sequence)\n\n";
}

}  // namespace

int main() {
  std::error_code error;
  const std::filesystem::path base = std::filesystem::temp_directory_path(error);
  if (error) {
    std::cerr << "benchmark failed: the system temporary directory is unavailable: " << error.message()
              << '\n';
    return 1;
  }

  const std::string store_root = dce::platform::join_path(
      base.string(), "dce-bench-" + std::to_string(dce::platform::process_id()));

  std::cout << "Data Center Evolution boundary benchmark\n";
  std::cout << "========================================\n";
  std::cout << "Every figure below is a measurement taken during this run, on this machine, in this\n";
  std::cout << "build. Nothing here is a baseline and nothing here is a comparison against another\n";
  std::cout << "run: this repository contains no previous measurement to compare against.\n\n";
  std::cout << "environment\n";
  print_field("compiler", compiler_text());
  print_field("build type", build_text());
  print_field("platform", platform_text());
  print_field("hardware threads", std::to_string(std::thread::hardware_concurrency()));
  print_field("timer", "dce::platform::monotonic_nanos() (monotonic, never a wall clock)");
  print_field("synthetic seed", std::to_string(kFleetSeed) + " (fixed, so every fleet is reproducible)");
  print_field("store root", store_root + " (temporary; removed before this program exits)");
  print_field("store durable", dce::platform::join_path(store_root, "store-durable"));
  print_field("store non-durable", dce::platform::join_path(store_root, "store-non-durable"));
  print_field("store recovery", dce::platform::join_path(store_root, "store-recovery"));
  std::cout << '\n';

  const Status created = dce::platform::make_directories(store_root);
  if (!created.ok()) {
    std::cerr << "benchmark failed: make_directories(" << store_root
              << "): " << created.message() << '\n';
    return 1;
  }

  Status outcome = Status{};
  Status removed = Status{};
  bool present_after_removal = true;
  {
    std::vector<DecisionResult> decisions;
    outcome = measure_decisions(decisions);
    if (outcome.ok()) {
      print_decision_tables(decisions);

      dce::SyntheticFleetOptions store_options;
      store_options.sites = kStoreSites;
      store_options.components = kStoreComponents;
      store_options.versions_per_component = kStoreVersions;
      store_options.cohorts = kStoreCohorts;
      store_options.seed = kFleetSeed;

      dce::SyntheticFleet store_fleet;
      if (outcome.ok()) {
        Result<dce::SyntheticFleet> fleet_result = dce::make_synthetic_fleet(store_options);
        outcome = take(fleet_result, store_fleet, "make_synthetic_fleet (store fleet)");
      }

      std::vector<std::vector<std::byte>> payloads;
      if (outcome.ok()) {
        outcome = build_receipt_pool(store_fleet.plan, kStoreAppends, payloads);
      }
      if (outcome.ok()) {
        std::cout << "store fleet for sections B and C\n";
        print_field("sites", std::to_string(store_fleet.plan.membership.sites.size()));
        print_field("steps", std::to_string(store_fleet.plan.steps.size()));
        print_field("receipt records", std::to_string(payloads.size()) +
                                           " (one per (site, step) pair, each codec round-tripped)");
        print_field("record payload bytes", std::to_string(payloads.front().size()));
        std::cout << '\n';
      }

      AppendResult durable;
      if (outcome.ok()) {
        outcome = measure_appends(dce::platform::join_path(store_root, "store-durable"), true, payloads,
                                  kStoreAppends, durable);
      }
      AppendResult non_durable;
      if (outcome.ok()) {
        outcome = measure_appends(dce::platform::join_path(store_root, "store-non-durable"), false,
                                  payloads, kStoreAppends, non_durable);
      }
      RecoveryResult recovery;
      if (outcome.ok()) {
        outcome = measure_checkpoint_recovery(dce::platform::join_path(store_root, "store-recovery"),
                                              store_fleet.plan, payloads, recovery);
      }
      if (outcome.ok()) {
        print_store_tables(durable, non_durable, recovery);
      }
    }

    removed = dce::platform::remove_tree(store_root);
    present_after_removal = dce::platform::path_exists(store_root).value_or(true);
  }

  std::cout << "cleanup\n";
  print_field("temporary store root", store_root);
  print_field("removed", removed.ok() ? "yes" : "no: " + removed.message());
  print_field("still present", present_after_removal ? "yes" : "no");
  std::cout << '\n';

  if (!outcome.ok()) {
    std::cerr << "benchmark failed: " << outcome.message() << '\n';
    return 1;
  }
  if (!removed.ok() || present_after_removal) {
    std::cerr << "benchmark failed: the temporary store root " << store_root
              << " could not be removed" << '\n';
    return 1;
  }
  std::cout << "all measurements completed\n";
  return 0;
}
