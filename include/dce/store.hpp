// Durable state: a crash-safe record log with snapshots.
//
// The store is the only thing in this runtime that claims durability, and the
// claim is precise: a record is durable once append() has returned ok with
// durable == true, which means the bytes reached stable storage through
// platform::File::sync(). Nothing before that point is promised to survive a
// crash, and nothing after it is allowed to disappear.
//
// Layout inside the store directory:
//   MANIFEST          two fixed-size slots, written alternately, each CRC- and
//                     SHA-256-checked, holding the epoch and the active
//                     snapshot and log identity
//   SNAPSHOT-<seq>    a full state image covering every record up to <seq>
//   LOG-<base>        an append-only segment of records starting at <base>
//   STORE.LOCK        the exclusive writer lock
//
// Each record carries a header CRC, a payload CRC, a SHA-256 of header and
// payload, and the digest of the previous record, so a torn tail and interior
// corruption are distinguishable: a torn tail is the last record and only the
// last record, and it is truncated; interior corruption is refused outright
// and never silently repaired.
#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "dce/digest.hpp"
#include "dce/ids.hpp"
#include "dce/status.hpp"

namespace dce::persist {

inline constexpr std::uint16_t kStoreFormatVersion = 1;
inline constexpr std::size_t kMaxRecordPayloadBytes = 4u * 1024u * 1024u;
inline constexpr std::size_t kManifestSlotBytes = 4096;
inline constexpr std::size_t kMaxSnapshotBytes = 64u * 1024u * 1024u;

enum class RecordType : std::uint16_t {
  epoch_bump = 1,
  plan_revision = 2,
  membership_snapshot = 3,
  validation_report = 4,
  plan_state_change = 5,
  stage_checkpoint = 6,
  migration_receipt = 7,
  rollback_marker = 8,
  reconciliation_record = 9,
  site_observation = 10,
  capability_dependency_proof = 11,
  exception_grant = 12,
};

[[nodiscard]] const char* to_string(RecordType type) noexcept;

// Opens a store for writing; read-only readers do not take the lock, because a
// reader that only ever looks at committed bytes cannot damage a writer, and a
// reader that meets a torn tail reports it rather than repairing it.
enum class OpenMode {
  read_only,
  read_write,
};

struct StoreOptions {
  std::size_t max_payload_bytes{kMaxRecordPayloadBytes};
  std::uint64_t max_log_bytes{64u * 1024u * 1024u};
  std::uint32_t max_segments{8};
  bool sync_on_commit{true};
};

struct LogEntry {
  RecordType type{RecordType::epoch_bump};
  LogSequence sequence;
  CoordinatorEpoch epoch;
  Digest256 digest;
  std::uint64_t offset{0};
  std::vector<std::byte> payload;
};

struct RecoveryReport {
  std::uint64_t records_recovered{0};
  std::uint64_t bytes_scanned{0};
  std::uint64_t torn_tail_bytes_discarded{0};
  bool torn_tail_recovered{false};
  LogSequence committed_sequence;
  CoordinatorEpoch epoch;
  std::uint32_t segments_scanned{0};
  bool snapshot_loaded{false};
  Digest256 snapshot_digest;
  std::string detail;
};

struct RecoveredState {
  std::vector<std::byte> snapshot;
  Digest256 snapshot_digest;
  LogSequence snapshot_sequence;
  std::vector<LogEntry> records;
};

struct CommitReceipt {
  LogSequence sequence;
  CoordinatorEpoch epoch;
  Digest256 record_digest;
  bool durable{false};
  bool duplicate{false};
};

class Store {
 public:
  Store() noexcept;
  Store(Store&& other) noexcept;
  Store& operator=(Store&& other) noexcept;
  Store(const Store&) = delete;
  Store& operator=(const Store&) = delete;
  ~Store();

  // Opens (creating when absent) and recovers. read_write takes the exclusive
  // lock and advances the coordinator epoch, which is what fences a stale
  // writer or a stale token after a restart. read_only takes no lock and does
  // not advance the epoch.
  [[nodiscard]] static Result<Store> open(std::string_view directory, const StoreOptions& options,
                                          OpenMode mode);

  // The transactional commit sequence: plan the record, reserve the next
  // sequence, write it, verify the bytes that were written, flush, and only
  // then publish it as the committed sequence.
  [[nodiscard]] Result<CommitReceipt> append(RecordType type, std::span<const std::byte> payload);

  // Replays an already-numbered record. Used to rebuild observed state from a
  // peer's journal. A sequence at or below the committed sequence is a
  // duplicate when its digest matches and a conflict when it does not.
  [[nodiscard]] Result<CommitReceipt> replay(RecordType type, LogSequence sequence,
                                             std::span<const std::byte> payload);

  [[nodiscard]] Result<RecoveredState> recover() const;

  [[nodiscard]] RecoveryReport recovery() const { return recovery_; }
  [[nodiscard]] CoordinatorEpoch epoch() const noexcept { return epoch_; }
  [[nodiscard]] LogSequence committed_sequence() const noexcept { return committed_sequence_; }
  [[nodiscard]] const std::string& directory() const noexcept { return directory_; }
  [[nodiscard]] bool is_open() const noexcept;

  // Writes a full state image covering the current committed sequence and then
  // retires only the segments that lie strictly below it. Records above the
  // snapshot sequence are never removed, so compaction cannot discard
  // uncommitted or post-snapshot state.
  [[nodiscard]] Status snapshot(std::span<const std::byte> image);

  [[nodiscard]] Status close();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  RecoveryReport recovery_;
  CoordinatorEpoch epoch_;
  LogSequence committed_sequence_;
  std::string directory_;
};

}  // namespace dce::persist
