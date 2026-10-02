// Durable state: the append-only record log, its snapshot base and recovery.
//
// This translation unit is the only place in the runtime that claims
// durability, and the claim is deliberately narrow. A record is durable once
// append() has written it, read the bytes back, verified them and flushed them
// through platform::File::sync(). Nothing before that point is promised to
// survive a crash and nothing after it may disappear. Every operating-system
// entry point used here is reached through include/dce/platform.hpp, so
// durability has exactly one implementation.
//
// The recovery classification is the heart of the file:
//
//   * A record whose own bytes are incomplete or fail their integrity checks
//     (a short header, a bad magic or version, a header CRC, a payload digest,
//     a declared payload that runs past the end of the file) with no fully
//     valid, chain-continuing record after it is a TORN TAIL. The active
//     segment is truncated back to the last valid boundary and the discarded
//     byte count is reported.
//   * The same failure with a later record that parses and continues the
//     per-record digest chain behind it is INTERIOR CORRUPTION. Recovery names
//     the byte offset and the segment and refuses, and it never truncates and
//     never skips the damaged record.
//   * An intact record that breaks the sequence or the digest chain is
//     interior corruption as well, in the middle of the log and at its end: a
//     torn write cannot produce a complete record whose chain is wrong.
//   * A MANIFEST whose two slots are both invalid is corruption, never an
//     empty store, and recovery refuses rather than silently starting over.
//
// Only the active (highest-based) segment may ever be truncated, and only at
// its tail. A torn tail is repaired on a read-write open and merely reported on
// a read-only one, so a reader that only ever looks at committed bytes can
// never damage a writer.

// dce/store.hpp declares std::unique_ptr in its private section without
// including <memory>, so the declaration is completed before it is parsed.
#include <memory>

#include "dce/store.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "dce/checked.hpp"
#include "dce/platform.hpp"
#include "dce/text.hpp"

namespace dce::persist {
namespace {

// ---------------------------------------------------------------------------
// On-disk constants
// ---------------------------------------------------------------------------
// "DECM", "DECR" and "SNAP" as they appear on the wire, little endian.
constexpr std::uint32_t kManifestMagic = 0x4D434544U;
constexpr std::uint32_t kRecordMagic = 0x52454344U;
constexpr std::uint32_t kSnapshotMagic = 0x50414E53U;

constexpr std::size_t kManifestSlotCount = 2;
constexpr std::size_t kRecordHeaderBytes = 100;
constexpr std::size_t kRecordPrefixBytes = 64;  // header bytes hashed with the payload
constexpr std::size_t kRecordCrcBytes = 96;     // header bytes covered by the header CRC
constexpr std::size_t kManifestCrcBytes = 88;
constexpr std::size_t kManifestDigestBytes = 92;
constexpr std::size_t kSnapshotHeaderBytes = 56;

constexpr std::string_view kManifestFileName = "MANIFEST";
constexpr std::string_view kLockFileName = "STORE.LOCK";
constexpr std::string_view kLogPrefix = "LOG-";
constexpr std::string_view kSnapshotPrefix = "SNAPSHOT-";
constexpr std::string_view kSnapshotTempSuffix = ".tmp";

// Bytes moved per read when a damaged region is searched for the next record
// that continues the digest chain.
constexpr std::size_t kScanWindowBytes = 64U * 1024U;

// ---------------------------------------------------------------------------
// Little-endian field access
//
// Fields are addressed byte by byte rather than by overlaying a struct: the
// format is fixed on the wire, and no host layout or alignment assumption
// belongs in a durability claim.
// ---------------------------------------------------------------------------
void put_u16(std::span<std::byte> out, std::size_t offset, std::uint16_t value) noexcept {
  out[offset] = static_cast<std::byte>(value & 0xFFU);
  out[offset + 1] = static_cast<std::byte>((value >> 8U) & 0xFFU);
}

void put_u32(std::span<std::byte> out, std::size_t offset, std::uint32_t value) noexcept {
  out[offset] = static_cast<std::byte>(value & 0xFFU);
  out[offset + 1] = static_cast<std::byte>((value >> 8U) & 0xFFU);
  out[offset + 2] = static_cast<std::byte>((value >> 16U) & 0xFFU);
  out[offset + 3] = static_cast<std::byte>((value >> 24U) & 0xFFU);
}

void put_u64(std::span<std::byte> out, std::size_t offset, std::uint64_t value) noexcept {
  for (std::size_t index = 0; index < 8; ++index) {
    out[offset + index] = static_cast<std::byte>((value >> (8U * static_cast<unsigned>(index))) & 0xFFU);
  }
}

std::uint16_t get_u16(std::span<const std::byte> in, std::size_t offset) noexcept {
  return static_cast<std::uint16_t>(std::to_integer<std::uint16_t>(in[offset]) |
                                    static_cast<std::uint16_t>(std::to_integer<std::uint16_t>(in[offset + 1]) << 8U));
}

std::uint32_t get_u32(std::span<const std::byte> in, std::size_t offset) noexcept {
  std::uint32_t value = 0;
  for (std::size_t index = 0; index < 4; ++index) {
    value |= static_cast<std::uint32_t>(std::to_integer<std::uint32_t>(in[offset + index]))
             << (8U * static_cast<unsigned>(index));
  }
  return value;
}

std::uint64_t get_u64(std::span<const std::byte> in, std::size_t offset) noexcept {
  std::uint64_t value = 0;
  for (std::size_t index = 0; index < 8; ++index) {
    value |= static_cast<std::uint64_t>(std::to_integer<std::uint64_t>(in[offset + index]))
             << (8U * static_cast<unsigned>(index));
  }
  return value;
}

void put_digest(std::span<std::byte> out, std::size_t offset, const Digest256& value) noexcept {
  const std::span<std::byte> target = out.subspan(offset, kDigestBytes);
  std::copy(value.bytes.begin(), value.bytes.end(), target.begin());
}

Digest256 get_digest(std::span<const std::byte> in, std::size_t offset) noexcept {
  Digest256 value;
  const std::span<const std::byte> source = in.subspan(offset, kDigestBytes);
  std::copy(source.begin(), source.end(), value.bytes.begin());
  return value;
}

// The record digest covers the header bytes up to the digest field itself and
// the payload that follows the header.
Digest256 record_self_digest(std::span<const std::byte> header_prefix,
                             std::span<const std::byte> payload) noexcept {
  Sha256 hash;
  hash.update(header_prefix);
  hash.update(payload);
  Digest256 digest;
  digest.bytes = hash.finish();
  return digest;
}

// ---------------------------------------------------------------------------
// On-disk names
// ---------------------------------------------------------------------------
std::string log_name(std::uint64_t base) { return std::string(kLogPrefix) + text::u64_to_string(base); }

std::string snapshot_name(std::uint64_t sequence) {
  return std::string(kSnapshotPrefix) + text::u64_to_string(sequence);
}

std::string snapshot_temp_name(std::uint64_t sequence) {
  return snapshot_name(sequence) + std::string(kSnapshotTempSuffix);
}

// Parses <prefix><digits> exactly. A name with any other shape is not part of
// the format and is ignored, and sequence zero is never a valid base because
// the first record of a store carries sequence one.
bool parse_numbered_name(std::string_view name, std::string_view prefix, std::uint64_t& value) {
  if (name.size() <= prefix.size() || name.substr(0, prefix.size()) != prefix) {
    return false;
  }
  const std::string_view digits = name.substr(prefix.size());
  for (const char character : digits) {
    if (character < '0' || character > '9') {
      return false;
    }
  }
  const Result<std::uint64_t> parsed = text::parse_u64(digits);
  if (!parsed.ok() || parsed.value() == 0) {
    return false;
  }
  value = parsed.value();
  return true;
}

bool is_log_name(std::string_view name, std::uint64_t& base) {
  return parse_numbered_name(name, kLogPrefix, base);
}

bool is_snapshot_name(std::string_view name, std::uint64_t& sequence) {
  return parse_numbered_name(name, kSnapshotPrefix, sequence);
}

bool is_snapshot_temp_name(std::string_view name) {
  if (name.size() <= kSnapshotTempSuffix.size() ||
      name.substr(name.size() - kSnapshotTempSuffix.size()) != kSnapshotTempSuffix) {
    return false;
  }
  const std::string_view stem = name.substr(0, name.size() - kSnapshotTempSuffix.size());
  std::uint64_t sequence = 0;
  return is_snapshot_name(stem, sequence);
}

// A store identity is written once, when the directory is first initialised,
// and never changes. It names the directory this manifest belongs to; it is not
// a secret and it is not verified against the path, because a store may be
// restored from a backup under a different name.
Digest256 make_store_identity(std::string_view directory) {
  Sha256 hash;
  hash.update(std::string_view("dce.persist.store/1"));
  hash.update(std::string_view("\0", 1));
  hash.update(directory);
  std::array<std::byte, 24> tail{};
  put_u64(tail, 0, platform::process_id());
  put_u64(tail, 8, platform::monotonic_nanos());
  put_u64(tail, 16, static_cast<std::uint64_t>(directory.size()));
  hash.update(tail);
  Digest256 digest;
  digest.bytes = hash.finish();
  return digest;
}

// ---------------------------------------------------------------------------
// MANIFEST slot
// ---------------------------------------------------------------------------
// The in-memory image of one manifest slot. Slots are written alternately and
// the newest valid one (highest generation) is authoritative, so a crash in the
// middle of a manifest write leaves the previous slot intact.
struct Manifest {
  std::uint16_t slot{0};
  std::uint64_t generation{0};
  std::uint64_t epoch{0};
  std::uint64_t snapshot_sequence{0};
  std::uint64_t snapshot_length{0};
  std::uint64_t active_log_base{1};
  std::uint64_t next_sequence{1};
  Digest256 identity;
};

std::array<std::byte, kManifestSlotBytes> encode_manifest(const Manifest& state) {
  std::array<std::byte, kManifestSlotBytes> slot{};
  const std::span<std::byte> out(slot);
  put_u32(out, 0, kManifestMagic);
  put_u16(out, 4, kStoreFormatVersion);
  put_u16(out, 6, state.slot);
  put_u64(out, 8, state.generation);
  put_u64(out, 16, state.epoch);
  put_u64(out, 24, state.snapshot_sequence);
  put_u64(out, 32, state.active_log_base);
  put_u64(out, 40, state.next_sequence);
  put_digest(out, 48, state.identity);
  put_u64(out, 80, state.snapshot_length);
  put_u32(out, 88, crc32(out.first(kManifestCrcBytes)));
  put_digest(out, 92, Digest256::of(out.first(kManifestDigestBytes)));
  return slot;
}

// Every rejection is reported with its reason so that a manifest whose two
// slots are both invalid can name both reasons.
Result<Manifest> decode_manifest(std::span<const std::byte> slot, std::uint16_t expected_slot) {
  const std::string where = "manifest slot " + text::u32_to_string(expected_slot);
  if (slot.size() < kManifestSlotBytes) {
    return Error{ErrorCode::malformed, where + " is shorter than " + text::u64_to_string(kManifestSlotBytes) + " bytes"};
  }
  if (get_u32(slot, 0) != kManifestMagic) {
    return Error{ErrorCode::malformed, where + " does not start with the DECM magic"};
  }
  if (get_u16(slot, 4) != kStoreFormatVersion) {
    return Error{ErrorCode::unsupported, where + " carries format version " +
                                              text::u32_to_string(get_u16(slot, 4)) +
                                              ", which this build does not implement"};
  }
  if (get_u16(slot, 6) != expected_slot) {
    return Error{ErrorCode::malformed, where + " claims to be slot " + text::u32_to_string(get_u16(slot, 6))};
  }
  if (get_u32(slot, kManifestCrcBytes) != crc32(slot.first(kManifestCrcBytes))) {
    return Error{ErrorCode::malformed, where + " fails its header crc32"};
  }
  if (get_digest(slot, kManifestDigestBytes) != Digest256::of(slot.first(kManifestDigestBytes))) {
    return Error{ErrorCode::malformed, where + " fails its header sha256"};
  }

  Manifest state;
  state.slot = expected_slot;
  state.generation = get_u64(slot, 8);
  state.epoch = get_u64(slot, 16);
  state.snapshot_sequence = get_u64(slot, 24);
  state.active_log_base = get_u64(slot, 32);
  state.next_sequence = get_u64(slot, 40);
  state.identity = get_digest(slot, 48);
  state.snapshot_length = get_u64(slot, 80);

  if (state.generation == 0) {
    return Error{ErrorCode::malformed, where + " carries generation zero"};
  }
  if (state.active_log_base == 0) {
    return Error{ErrorCode::malformed, where + " carries active log base zero"};
  }
  if (state.next_sequence < state.active_log_base) {
    return Error{ErrorCode::malformed, where + " expects the next record below the active log base"};
  }
  if (state.snapshot_sequence == 0) {
    if (state.snapshot_length != 0) {
      return Error{ErrorCode::malformed, where + " names a snapshot length without a snapshot sequence"};
    }
  } else {
    if (state.snapshot_length == 0 ||
        state.snapshot_length > static_cast<std::uint64_t>(kMaxSnapshotBytes)) {
      return Error{ErrorCode::malformed, where + " names a snapshot length outside the documented bound"};
    }
    if (state.snapshot_sequence >= state.next_sequence) {
      return Error{ErrorCode::malformed, where + " covers a snapshot at or beyond its next sequence"};
    }
  }
  return state;
}

// ---------------------------------------------------------------------------
// LOG record header
// ---------------------------------------------------------------------------
struct RecordFields {
  std::uint16_t type{0};
  std::uint64_t sequence{0};
  std::uint64_t epoch{0};
  std::uint32_t payload_length{0};
  Digest256 prev_digest;
  Digest256 self_digest;
};

std::array<std::byte, kRecordHeaderBytes> encode_record_header(const RecordFields& fields) {
  std::array<std::byte, kRecordHeaderBytes> header{};
  const std::span<std::byte> out(header);
  put_u32(out, 0, kRecordMagic);
  put_u16(out, 4, kStoreFormatVersion);
  put_u16(out, 6, fields.type);
  put_u64(out, 8, fields.sequence);
  put_u64(out, 16, fields.epoch);
  put_u32(out, 24, fields.payload_length);
  put_u32(out, 28, 0);
  put_digest(out, 32, fields.prev_digest);
  put_digest(out, 64, fields.self_digest);
  put_u32(out, kRecordCrcBytes, crc32(out.first(kRecordCrcBytes)));
  return header;
}

// Verifies everything a 100-byte header can be checked against on its own: the
// magic, the format version, the reserved field and the header CRC. The
// sequence, the payload length and the digest chain are checked by the caller
// against the position in the log and the bytes that follow.
bool decode_record_header(std::span<const std::byte> header, RecordFields& fields, std::string& reason) {
  if (header.size() < kRecordHeaderBytes) {
    reason = "a short record header";
    return false;
  }
  if (get_u32(header, 0) != kRecordMagic) {
    reason = "a record header without the DECR magic";
    return false;
  }
  if (get_u16(header, 4) != kStoreFormatVersion) {
    reason = "a record header with format version " + text::u32_to_string(get_u16(header, 4));
    return false;
  }
  if (get_u32(header, 28) != 0) {
    reason = "a record header whose reserved field is not zero";
    return false;
  }
  if (get_u32(header, kRecordCrcBytes) != crc32(header.first(kRecordCrcBytes))) {
    reason = "a record header that fails its crc32";
    return false;
  }
  fields.type = get_u16(header, 6);
  fields.sequence = get_u64(header, 8);
  fields.epoch = get_u64(header, 16);
  fields.payload_length = get_u32(header, 24);
  fields.prev_digest = get_digest(header, 32);
  fields.self_digest = get_digest(header, 64);
  return true;
}

// ---------------------------------------------------------------------------
// Recovery scan
// ---------------------------------------------------------------------------
struct SegmentInfo {
  std::string name;
  std::uint64_t base{0};
  std::uint64_t size{0};
  std::uint64_t last_sequence{0};
  bool has_records{false};
};

struct ScanRequest {
  const StoreOptions* options{nullptr};
  std::string_view directory;
  std::uint64_t snapshot_sequence{0};
  std::uint64_t manifest_next_sequence{1};
  bool repair{false};           // truncate a torn tail (read-write open only)
  bool collect_entries{false};  // also decode the payloads above the snapshot
};

struct ScanResult {
  std::vector<SegmentInfo> segments;
  std::vector<LogEntry> entries;
  std::uint64_t committed{0};
  std::uint64_t last_sequence{0};
  Digest256 last_digest;
  std::uint64_t records_validated{0};
  std::uint64_t bytes_scanned{0};
  std::uint64_t discarded_bytes{0};
  std::uint32_t segments_scanned{0};
  bool torn_tail{false};
  bool manifest_ahead{false};
  std::uint64_t manifest_expected{0};
};

// One hypothesis about the record that should follow a damaged region.
struct ChainCheck {
  std::uint64_t sequence{0};
  Digest256 prev_digest;
  bool prev_verifiable{false};
};

// A damaged record is interior corruption when a later record exists that both
// parses and continues the digest chain. Three chains are compatible with the
// damage, and any of them proves that valid data follows:
//
//   * the chain from the last record that verified, which holds when the
//     damaged bytes were never part of the log;
//   * the chain through the damaged record itself, which holds when its header
//     survived and only its payload or tail is damaged;
//   * a record numbered exactly one above the damaged one, which holds when the
//     damaged header is unreadable and its record digest can no longer be read.
[[nodiscard]] Result<bool> chain_continues_after(platform::File& file, std::uint64_t file_size,
                                                 std::uint64_t damaged_offset, const ChainCheck& from_last_valid,
                                                 const ChainCheck& through_damaged, bool through_damaged_valid,
                                                 std::uint64_t one_past_damaged, bool one_past_damaged_valid,
                                                 std::size_t payload_bound) {
  const std::uint64_t header_bytes = static_cast<std::uint64_t>(kRecordHeaderBytes);
  if (file_size < header_bytes) {
    return false;
  }
  const std::uint64_t last_offset = file_size - header_bytes;
  std::vector<std::byte> window(kScanWindowBytes);
  std::uint64_t window_start = 0;
  std::size_t window_length = 0;
  std::uint64_t cursor = damaged_offset;
  while (cursor < last_offset) {
    cursor += 1;
    if (cursor > last_offset) {
      break;
    }
    if (cursor < window_start || cursor + header_bytes > window_start + static_cast<std::uint64_t>(window_length)) {
      window_start = cursor;
      const std::uint64_t remaining = file_size - cursor;
      const std::size_t want =
          static_cast<std::size_t>(std::min<std::uint64_t>(remaining, static_cast<std::uint64_t>(kScanWindowBytes)));
      DCE_ASSIGN(window_read, file.read_at(window_start, std::span<std::byte>(window).first(want)));
      if (window_read != want) {
        return Error{ErrorCode::io_error, "the damaged region could not be read back in full"};
      }
      window_length = window_read;
    }
    const std::size_t start = static_cast<std::size_t>(cursor - window_start);
    const std::span<const std::byte> header = std::span<const std::byte>(window).subspan(start, kRecordHeaderBytes);
    if (get_u32(header, 0) != kRecordMagic) {
      continue;
    }
    RecordFields fields;
    std::string reason;
    if (!decode_record_header(header, fields, reason)) {
      continue;
    }
    const bool matches_last = fields.sequence == from_last_valid.sequence &&
                              (!from_last_valid.prev_verifiable || fields.prev_digest == from_last_valid.prev_digest);
    const bool matches_damaged = through_damaged_valid && fields.sequence == through_damaged.sequence &&
                                 fields.prev_digest == through_damaged.prev_digest;
    const bool matches_fallback = one_past_damaged_valid && fields.sequence == one_past_damaged;
    if (!matches_last && !matches_damaged && !matches_fallback) {
      continue;
    }
    // A record that only matches the weak fallback still has to be complete on
    // its own before it can be evidence of anything.
    const std::uint64_t payload_length = static_cast<std::uint64_t>(fields.payload_length);
    if (payload_length > static_cast<std::uint64_t>(payload_bound)) {
      continue;
    }
    const std::optional<std::uint64_t> total = checked_add<std::uint64_t>(header_bytes, payload_length);
    if (!total.has_value() || *total > file_size - cursor) {
      continue;
    }
    std::vector<std::byte> payload(static_cast<std::size_t>(payload_length));
    if (!payload.empty()) {
      DCE_ASSIGN(payload_read, file.read_at(cursor + header_bytes, std::span<std::byte>(payload)));
      if (payload_read != payload.size()) {
        continue;
      }
    }
    if (record_self_digest(header.first(kRecordPrefixBytes), payload) != fields.self_digest) {
      continue;
    }
    return true;
  }
  return false;
}

[[nodiscard]] Result<ScanResult> scan_log(const ScanRequest& request) {
  if (request.options == nullptr) {
    return Error{ErrorCode::invalid_argument, "the recovery scan needs store options"};
  }
  const StoreOptions& options = *request.options;
  const std::uint64_t snapshot = request.snapshot_sequence;
  ScanResult result;

  DCE_ASSIGN(names, platform::list_directory(request.directory));

  struct SegmentFile {
    std::string name;
    std::uint64_t base{0};
  };
  std::vector<SegmentFile> files;
  for (const std::string& name : names) {
    std::uint64_t base = 0;
    if (is_log_name(name, base)) {
      SegmentFile entry;
      entry.name = name;
      entry.base = base;
      files.push_back(std::move(entry));
    }
  }
  std::sort(files.begin(), files.end(),
            [](const SegmentFile& left, const SegmentFile& right) { return left.base < right.base; });

  bool have_any = false;
  std::uint64_t last_sequence = 0;
  Digest256 last_digest;

  for (std::size_t index = 0; index < files.size(); ++index) {
    const SegmentFile& entry = files[index];
    // Only the segment that physically ends the log may be truncated: it is the
    // last one in ascending base order.
    const bool is_head = index + 1 == files.size();
    // Only the head segment may ever be truncated, and a read-only handle
    // cannot truncate on every platform, so write access is asked for exactly
    // when a repair is possible.
    const platform::OpenMode file_mode =
        request.repair && is_head ? platform::OpenMode::read_write : platform::OpenMode::read_only;
    DCE_ASSIGN(file, platform::File::open(platform::join_path(request.directory, entry.name), file_mode));
    DCE_ASSIGN(file_size, file.size());
    if (!accumulate(result.bytes_scanned, file_size)) {
      return Error{ErrorCode::overflow, "the recovery scan byte count overflowed"};
    }
    if (result.segments_scanned == std::numeric_limits<std::uint32_t>::max()) {
      return Error{ErrorCode::limit_exceeded, "the recovery scan found more log segments than it can count"};
    }
    result.segments_scanned += 1;

    SegmentInfo info;
    info.name = entry.name;
    info.base = entry.base;
    info.size = file_size;

    std::uint64_t offset = 0;
    bool first_in_segment = true;
    bool failed = false;
    bool continuity_failure = false;
    std::uint64_t failure_offset = 0;
    std::string failure_reason;

    while (offset < file_size) {
      const std::uint64_t remaining = file_size - offset;
      if (remaining < static_cast<std::uint64_t>(kRecordHeaderBytes)) {
        failed = true;
        failure_offset = offset;
        failure_reason = "a record header cut short by the end of the segment";
        break;
      }
      std::array<std::byte, kRecordHeaderBytes> header{};
      DCE_ASSIGN(header_read, file.read_at(offset, std::span<std::byte>(header)));
      if (header_read != kRecordHeaderBytes) {
        failed = true;
        failure_offset = offset;
        failure_reason = "a record header cut short by the end of the segment";
        break;
      }
      RecordFields fields;
      std::string reason;
      if (!decode_record_header(header, fields, reason)) {
        failed = true;
        failure_offset = offset;
        failure_reason = reason;
        break;
      }
      // No allocation is ever driven by a length read from disk: the declared
      // payload must fit the configured bound and the bytes that remain.
      if (static_cast<std::uint64_t>(fields.payload_length) >
          static_cast<std::uint64_t>(options.max_payload_bytes)) {
        failed = true;
        failure_offset = offset;
        failure_reason = "a payload length above the configured bound";
        break;
      }
      const std::optional<std::uint64_t> record_bytes =
          checked_add<std::uint64_t>(static_cast<std::uint64_t>(kRecordHeaderBytes),
                                     static_cast<std::uint64_t>(fields.payload_length));
      if (!record_bytes.has_value() || *record_bytes > remaining) {
        failed = true;
        failure_offset = offset;
        failure_reason = "a record whose declared payload runs past the end of the segment";
        break;
      }

      // The first record of a segment carries the sequence the segment name
      // promises, and the log continues one sequence at a time from there. The
      // only discontinuity the format allows is the restart one past the
      // snapshot, which is also where the digest chain restarts.
      if (first_in_segment && fields.sequence != entry.base) {
        failed = true;
        continuity_failure = true;
        failure_offset = offset;
        failure_reason = "a first record for sequence " + text::u64_to_string(fields.sequence) +
                         " in a segment whose base is " + text::u64_to_string(entry.base);
        break;
      }
      bool sequence_ok = false;
      bool prev_ok = false;
      bool anchor = false;
      if (!have_any) {
        sequence_ok = snapshot == 0 ? fields.sequence == 1 : fields.sequence <= snapshot + 1;
        anchor = fields.sequence == 1 || (snapshot > 0 && fields.sequence == snapshot + 1);
        prev_ok = !anchor || fields.prev_digest.is_zero();
      } else {
        const std::optional<std::uint64_t> expected = checked_next<std::uint64_t>(last_sequence);
        if (expected.has_value() && fields.sequence == *expected) {
          sequence_ok = true;
          prev_ok = fields.prev_digest == last_digest;
        } else if (snapshot > 0 && fields.sequence == snapshot + 1 && last_sequence <= snapshot) {
          sequence_ok = true;
          prev_ok = fields.prev_digest.is_zero();
        }
      }
      if (!sequence_ok) {
        failed = true;
        continuity_failure = true;
        failure_offset = offset;
        failure_reason = have_any
                             ? "a record for sequence " + text::u64_to_string(fields.sequence) +
                                   " where sequence " + text::u64_to_string(last_sequence + 1) + " was required"
                             : "a log that does not begin at sequence 1";
        break;
      }
      if (!prev_ok) {
        failed = true;
        continuity_failure = true;
        failure_offset = offset;
        failure_reason = "a record whose previous-record digest does not continue the chain";
        break;
      }

      std::vector<std::byte> payload(static_cast<std::size_t>(fields.payload_length));
      if (!payload.empty()) {
        DCE_ASSIGN(payload_read, file.read_at(offset + static_cast<std::uint64_t>(kRecordHeaderBytes),
                                              std::span<std::byte>(payload)));
        if (payload_read != payload.size()) {
          failed = true;
          failure_offset = offset;
          failure_reason = "a payload cut short by the end of the segment";
          break;
        }
      }
      if (record_self_digest(std::span<const std::byte>(header).first(kRecordPrefixBytes), payload) !=
          fields.self_digest) {
        failed = true;
        failure_offset = offset;
        failure_reason = "a record whose payload does not hash to its stored digest";
        break;
      }

      if (!accumulate(result.records_validated, static_cast<std::uint64_t>(1))) {
        return Error{ErrorCode::overflow, "the recovery scan record count overflowed"};
      }
      if (request.collect_entries && fields.sequence > snapshot) {
        LogEntry log_entry;
        log_entry.type = static_cast<RecordType>(fields.type);
        log_entry.sequence = LogSequence{fields.sequence};
        log_entry.epoch = CoordinatorEpoch{fields.epoch};
        log_entry.digest = fields.self_digest;
        log_entry.offset = offset;
        log_entry.payload = std::move(payload);
        result.entries.push_back(std::move(log_entry));
      }
      have_any = true;
      last_sequence = fields.sequence;
      last_digest = fields.self_digest;
      info.last_sequence = fields.sequence;
      info.has_records = true;
      first_in_segment = false;
      offset += *record_bytes;
    }

    if (!failed) {
      result.segments.push_back(std::move(info));
      continue;
    }

    const std::string where = "LOG-" + text::u64_to_string(entry.base) + " at byte offset " +
                              text::u64_to_string(failure_offset);
    if (continuity_failure) {
      // The record's own bytes verify; only its place in the sequence or in the
      // digest chain is wrong. A torn write cannot produce that, so this is
      // interior corruption even at the end of the log.
      return Error{ErrorCode::corruption, where + ": " + failure_reason};
    }

    ChainCheck from_last_valid;
    from_last_valid.prev_verifiable = have_any;
    if (have_any) {
      const std::optional<std::uint64_t> expected = checked_next<std::uint64_t>(last_sequence);
      if (!expected.has_value()) {
        return Error{ErrorCode::overflow, "the recovery scan cannot continue past the last sequence"};
      }
      from_last_valid.sequence = *expected;
      if (snapshot > 0 && *expected == snapshot + 1) {
        from_last_valid.prev_digest = Digest256::zero();
      } else {
        from_last_valid.prev_digest = last_digest;
      }
    } else {
      from_last_valid.sequence = entry.base;
      from_last_valid.prev_verifiable = entry.base == 1 || (snapshot > 0 && entry.base == snapshot + 1);
      from_last_valid.prev_digest = Digest256::zero();
    }

    // The damaged header may still be readable: if it is, the record that
    // follows it must carry its digest, and that is the strongest evidence of
    // interior corruption available.
    ChainCheck through_damaged;
    bool through_damaged_valid = false;
    std::uint64_t one_past_damaged = 0;
    bool one_past_damaged_valid = false;
    if (failure_offset + static_cast<std::uint64_t>(kRecordHeaderBytes) <= file_size) {
      std::array<std::byte, kRecordHeaderBytes> damaged{};
      const Result<std::size_t> damaged_read =
          file.read_at(failure_offset, std::span<std::byte>(damaged));
      if (!damaged_read.ok()) {
        return damaged_read.error();
      }
      RecordFields damaged_fields;
      std::string damaged_reason;
      if (*damaged_read == kRecordHeaderBytes && decode_record_header(damaged, damaged_fields, damaged_reason)) {
        const std::optional<std::uint64_t> after = checked_next<std::uint64_t>(damaged_fields.sequence);
        const std::optional<std::uint64_t> beyond = checked_next<std::uint64_t>(from_last_valid.sequence);
        if (after.has_value()) {
          through_damaged.sequence = *after;
          through_damaged.prev_digest = damaged_fields.self_digest;
          through_damaged_valid = true;
        }
        if (beyond.has_value()) {
          one_past_damaged = *beyond;
          one_past_damaged_valid = true;
        }
      }
    }

    DCE_ASSIGN(follows, chain_continues_after(file, file_size, failure_offset, from_last_valid, through_damaged,
                                              through_damaged_valid, one_past_damaged, one_past_damaged_valid,
                                              options.max_payload_bytes));
    if (follows) {
      return Error{ErrorCode::corruption,
                   where + ": " + failure_reason +
                       ", and a later record continues the digest chain, so the damage is interior"};
    }
    if (!is_head) {
      return Error{ErrorCode::corruption,
                   where + ": " + failure_reason +
                       ", and this is not the active segment, so it may not be truncated"};
    }

    result.torn_tail = true;
    result.discarded_bytes = file_size - failure_offset;
    if (request.repair) {
      DCE_TRY(file.truncate(failure_offset));
      DCE_TRY(file.sync());
      info.size = failure_offset;
    }
    result.segments.push_back(std::move(info));
    break;
  }

  result.last_sequence = last_sequence;
  result.last_digest = last_digest;
  result.committed = std::max(snapshot, last_sequence);
  const std::optional<std::uint64_t> next = checked_next<std::uint64_t>(result.committed);
  if (next.has_value() && request.manifest_next_sequence > *next) {
    // The manifest is written after the record it describes, so it can never
    // lead the log across a clean end: whole committed records would have had
    // to disappear. A damaged tail is the one explanation that keeps the log
    // honest, and it is already counted, so that discrepancy is reported
    // rather than turned into a refusal.
    if (!result.torn_tail) {
      return Error{ErrorCode::corruption,
                   "the manifest expects the next record to be sequence " +
                       text::u64_to_string(request.manifest_next_sequence) +
                       " but the log ends at sequence " + text::u64_to_string(result.committed) +
                       ", so committed records are missing"};
    }
    result.manifest_ahead = true;
    result.manifest_expected = request.manifest_next_sequence;
  }
  return result;
}

// ---------------------------------------------------------------------------
// SNAPSHOT file
// ---------------------------------------------------------------------------
// Reads and verifies a snapshot image. The declared length is checked against
// the documented bound and the file size before a single byte is allocated.
Result<std::vector<std::byte>> load_snapshot_file(std::string_view path, std::uint64_t sequence,
                                                  std::uint64_t expected_length, Digest256& digest_out) {
  DCE_ASSIGN(file, platform::File::open(path, platform::OpenMode::read_only));
  DCE_ASSIGN(file_size, file.size());
  const std::uint64_t header_bytes = static_cast<std::uint64_t>(kSnapshotHeaderBytes);
  if (file_size < header_bytes) {
    return Error{ErrorCode::corruption, "the snapshot file is shorter than its header"};
  }
  std::array<std::byte, kSnapshotHeaderBytes> header{};
  DCE_ASSIGN(header_read, file.read_at(0, std::span<std::byte>(header)));
  if (header_read != kSnapshotHeaderBytes) {
    return Error{ErrorCode::corruption, "the snapshot header could not be read in full"};
  }
  if (get_u32(header, 0) != kSnapshotMagic) {
    return Error{ErrorCode::corruption, "the snapshot file does not start with the SNAP magic"};
  }
  if (get_u16(header, 4) != kStoreFormatVersion) {
    return Error{ErrorCode::corruption, "the snapshot carries format version " +
                                            text::u32_to_string(get_u16(header, 4))};
  }
  if (get_u16(header, 6) != 0) {
    return Error{ErrorCode::corruption, "the snapshot reserved field is not zero"};
  }
  if (get_u64(header, 8) != sequence) {
    return Error{ErrorCode::corruption, "the snapshot covers sequence " + text::u64_to_string(get_u64(header, 8)) +
                                            " rather than " + text::u64_to_string(sequence)};
  }
  const std::uint64_t length = static_cast<std::uint64_t>(get_u32(header, 16));
  if (length > static_cast<std::uint64_t>(kMaxSnapshotBytes)) {
    return Error{ErrorCode::corruption, "the snapshot declares an image above the documented bound"};
  }
  if (length != expected_length) {
    return Error{ErrorCode::corruption, "the snapshot length does not match the manifest"};
  }
  const std::optional<std::uint64_t> total = checked_add<std::uint64_t>(header_bytes, length);
  if (!total.has_value() || *total != file_size) {
    return Error{ErrorCode::corruption, "the snapshot file is not exactly its header and its image"};
  }
  std::vector<std::byte> image(static_cast<std::size_t>(length));
  if (!image.empty()) {
    DCE_ASSIGN(image_read, file.read_at(header_bytes, std::span<std::byte>(image)));
    if (image_read != image.size()) {
      return Error{ErrorCode::corruption, "the snapshot image could not be read in full"};
    }
  }
  if (get_u32(header, 20) != crc32(image)) {
    return Error{ErrorCode::corruption, "the snapshot image fails its crc32"};
  }
  const Digest256 digest = Digest256::of(image);
  if (digest != get_digest(header, 24)) {
    return Error{ErrorCode::corruption, "the snapshot image fails its sha256"};
  }
  digest_out = digest;
  return image;
}

}  // namespace

// ---------------------------------------------------------------------------
// Store::Impl
// ---------------------------------------------------------------------------
struct Store::Impl {
  StoreOptions options;
  std::string directory;
  bool read_only{false};
  bool opened{false};

  platform::FileLock lock;
  platform::File manifest_file;
  platform::File log_file;

  Manifest manifest;
  std::vector<SegmentInfo> segments;
  std::uint64_t active_base{1};
  std::uint64_t active_size{0};
  Digest256 last_digest;
  bool tail_dirty{false};

  [[nodiscard]] std::string manifest_path() const {
    return platform::join_path(directory, kManifestFileName);
  }
  [[nodiscard]] std::string lock_path() const {
    return platform::join_path(directory, kLockFileName);
  }
  [[nodiscard]] std::string log_path(std::uint64_t base) const {
    return platform::join_path(directory, log_name(base));
  }
  [[nodiscard]] std::uint64_t segment_budget() const {
    const std::uint64_t budget = options.max_log_bytes / static_cast<std::uint64_t>(options.max_segments);
    return budget == 0 ? 1 : budget;
  }

  Status refuse_uninitialised_directory() const;
  Status create_initial_manifest();
  Status load_manifest();
  Status write_manifest(const Manifest& next_state);
  Status open_log();
  Status close_log();
  Status retire_superseded();
  Status ensure_capacity(std::uint64_t record_bytes, std::uint64_t sequence);
  Status verify_written(std::uint64_t offset, std::span<const std::byte> header, const RecordFields& fields,
                        std::span<const std::byte> payload);
  [[nodiscard]] Result<CommitReceipt> commit(RecordType type, LogSequence sequence,
                                             std::span<const std::byte> payload, std::uint64_t epoch);
  [[nodiscard]] Result<std::optional<std::pair<RecordFields, std::vector<std::byte>>>> find_record(
      std::uint64_t sequence);
  Status write_snapshot(std::span<const std::byte> image, std::uint64_t committed);
  Status shutdown();

  void remember_segment(std::uint64_t sequence);
};

// A directory that holds log or snapshot files but no manifest was not created
// by this store, and initialising it would silently start over on top of
// durable bytes.
Status Store::Impl::refuse_uninitialised_directory() const {
  DCE_ASSIGN(names, platform::list_directory(directory));
  for (const std::string& name : names) {
    if (name == kLockFileName) {
      continue;
    }
    std::uint64_t number = 0;
    if (is_log_name(name, number) || is_snapshot_name(name, number) || is_snapshot_temp_name(name)) {
      return Error{ErrorCode::corruption,
                   "the directory holds " + text::escape_for_output(name) +
                       " but no MANIFEST, so it is not a store this build may initialise"};
    }
  }
  return Status{};
}

Status Store::Impl::create_initial_manifest() {
  DCE_ASSIGN(file, platform::File::open(manifest_path(), platform::OpenMode::read_write));
  manifest_file = std::move(file);
  Manifest state;
  state.slot = 1;  // the first written slot is slot 0
  state.epoch = 0;
  state.snapshot_sequence = 0;
  state.snapshot_length = 0;
  state.active_log_base = 1;
  state.next_sequence = 1;
  state.identity = make_store_identity(directory);
  DCE_TRY(write_manifest(state));
  return platform::sync_directory(directory);
}

Status Store::Impl::load_manifest() {
  const platform::OpenMode file_mode =
      read_only ? platform::OpenMode::read_only : platform::OpenMode::read_write;
  DCE_ASSIGN(file, platform::File::open(manifest_path(), file_mode));
  DCE_ASSIGN(file_size, file.size());
  std::array<std::byte, kManifestSlotBytes * kManifestSlotCount> raw{};
  const std::uint64_t wanted =
      std::min<std::uint64_t>(file_size, static_cast<std::uint64_t>(raw.size()));
  std::size_t available = 0;
  if (wanted != 0) {
    DCE_ASSIGN(bytes_read, file.read_at(0, std::span<std::byte>(raw).first(static_cast<std::size_t>(wanted))));
    available = bytes_read;
  }
  std::optional<Manifest> best;
  std::array<std::string, kManifestSlotCount> reasons;
  for (std::size_t index = 0; index < kManifestSlotCount; ++index) {
    const std::uint16_t slot = static_cast<std::uint16_t>(index);
    const std::size_t offset = index * kManifestSlotBytes;
    if (static_cast<std::uint64_t>(offset) + static_cast<std::uint64_t>(kManifestSlotBytes) >
        static_cast<std::uint64_t>(available)) {
      reasons[index] = "the slot lies beyond the end of the file";
      continue;
    }
    Result<Manifest> parsed =
        decode_manifest(std::span<const std::byte>(raw).subspan(offset, kManifestSlotBytes), slot);
    if (!parsed.ok()) {
      reasons[index] = parsed.error().message;
      continue;
    }
    // The newest valid slot wins; on an identical generation the later slot is
    // taken so that the choice is deterministic.
    if (!best.has_value() || parsed->generation > best->generation ||
        (parsed->generation == best->generation && parsed->slot > best->slot)) {
      best = parsed.value();
    }
  }
  if (!best.has_value()) {
    return Error{ErrorCode::corruption, "both MANIFEST slots are invalid (slot 0: " + reasons[0] +
                                            "; slot 1: " + reasons[1] + ")"};
  }
  manifest = *best;
  manifest_file = std::move(file);
  return Status{};
}

Status Store::Impl::write_manifest(const Manifest& next_state) {
  if (read_only) {
    return Error{ErrorCode::denied, "a read-only store never writes the manifest"};
  }
  if (!manifest_file.is_open()) {
    return Error{ErrorCode::closed, "the manifest file is not open"};
  }
  const std::optional<std::uint64_t> generation = checked_next<std::uint64_t>(manifest.generation);
  if (!generation.has_value()) {
    return Error{ErrorCode::overflow, "the manifest slot generation is exhausted"};
  }
  Manifest written = next_state;
  written.slot = static_cast<std::uint16_t>((manifest.slot + 1U) % 2U);
  written.generation = *generation;
  const std::array<std::byte, kManifestSlotBytes> bytes = encode_manifest(written);
  const std::uint64_t offset = static_cast<std::uint64_t>(written.slot) * kManifestSlotBytes;
  DCE_TRY(manifest_file.write_at(offset, bytes));
  DCE_TRY(manifest_file.sync());
  manifest = written;
  return Status{};
}

Status Store::Impl::open_log() {
  if (log_file.is_open()) {
    return Status{};
  }
  const std::string path = log_path(active_base);
  DCE_ASSIGN(exists, platform::path_exists(path));
  DCE_ASSIGN(file, platform::File::open(path, platform::OpenMode::read_write));
  DCE_ASSIGN(file_size, file.size());
  if (file_size < active_size) {
    return Error{ErrorCode::corruption,
                 "the active segment " + log_name(active_base) + " is shorter than its committed end"};
  }
  if (file_size > active_size) {
    // Bytes past the last committed record were never verified: they are the
    // remains of a failed write and are discarded before anything is appended.
    DCE_TRY(file.truncate(active_size));
  }
  log_file = std::move(file);
  tail_dirty = false;
  if (!exists) {
    DCE_TRY(platform::sync_directory(directory));
  }
  bool known = false;
  for (const SegmentInfo& segment : segments) {
    if (segment.base == active_base) {
      known = true;
      break;
    }
  }
  if (!known) {
    SegmentInfo segment;
    segment.name = log_name(active_base);
    segment.base = active_base;
    segment.size = active_size;
    segments.push_back(std::move(segment));
  }
  return Status{};
}

Status Store::Impl::close_log() {
  if (!log_file.is_open()) {
    return Status{};
  }
  const Status synced = log_file.sync();
  const Status closed = log_file.close();
  if (!synced.ok()) {
    return synced;
  }
  return closed;
}

void Store::Impl::remember_segment(std::uint64_t sequence) {
  for (SegmentInfo& segment : segments) {
    if (segment.base == active_base) {
      segment.size = active_size;
      segment.last_sequence = sequence;
      segment.has_records = true;
      return;
    }
  }
  SegmentInfo segment;
  segment.name = log_name(active_base);
  segment.base = active_base;
  segment.size = active_size;
  segment.last_sequence = sequence;
  segment.has_records = true;
  segments.push_back(std::move(segment));
}

// A segment is fully superseded when every record it holds is covered by the
// snapshot. Only such a segment may be retired, which is what stops compaction
// from discarding uncommitted or post-snapshot state.
Status Store::Impl::retire_superseded() {
  if (read_only) {
    return Status{};
  }
  const std::uint64_t snapshot = manifest.snapshot_sequence;
  Status first_failure;
  std::vector<SegmentInfo> kept;
  kept.reserve(segments.size());
  for (const SegmentInfo& segment : segments) {
    const bool superseded = segment.has_records ? segment.last_sequence <= snapshot : segment.base <= snapshot;
    if (!superseded) {
      kept.push_back(segment);
      continue;
    }
    const Status removed = platform::remove_file(platform::join_path(directory, segment.name));
    if (!removed.ok()) {
      if (first_failure.ok()) {
        first_failure = removed;
      }
      kept.push_back(segment);
    }
  }
  segments = std::move(kept);
  if (snapshot == 0) {
    return first_failure;
  }
  DCE_ASSIGN(names, platform::list_directory(directory));
  for (const std::string& name : names) {
    bool remove = false;
    std::uint64_t sequence = 0;
    if (is_snapshot_name(name, sequence)) {
      remove = sequence < snapshot;
    } else if (is_snapshot_temp_name(name)) {
      remove = true;  // an interrupted snapshot of any sequence
    }
    if (!remove) {
      continue;
    }
    const Status removed = platform::remove_file(platform::join_path(directory, name));
    if (!removed.ok() && first_failure.ok()) {
      first_failure = removed;
    }
  }
  return first_failure;
}

Status Store::Impl::ensure_capacity(std::uint64_t record_bytes, std::uint64_t sequence) {
  if (active_size == 0) {
    return Status{};  // an empty active segment accepts any single record
  }
  const std::optional<std::uint64_t> projected = checked_add<std::uint64_t>(active_size, record_bytes);
  if (!projected.has_value()) {
    return Error{ErrorCode::overflow, "the active segment length overflowed"};
  }
  if (*projected <= segment_budget()) {
    return Status{};
  }
  DCE_TRY(close_log());
  DCE_TRY(retire_superseded());
  if (segments.size() + 1 > static_cast<std::size_t>(options.max_segments)) {
    return Error{ErrorCode::limit_exceeded,
                 "the log already holds " + text::u64_to_string(static_cast<std::uint64_t>(segments.size())) +
                     " of at most " + text::u32_to_string(options.max_segments) +
                     " segments and none of them is fully superseded by the snapshot"};
  }
  // The next record starts a fresh segment whose base is its own sequence.
  active_base = sequence;
  active_size = 0;
  return Status{};
}

Status Store::Impl::verify_written(std::uint64_t offset, std::span<const std::byte> header,
                                   const RecordFields& fields, std::span<const std::byte> payload) {
  std::array<std::byte, kRecordHeaderBytes> read_header{};
  DCE_ASSIGN(header_read, log_file.read_at(offset, std::span<std::byte>(read_header)));
  if (header_read != kRecordHeaderBytes) {
    return Error{ErrorCode::io_error, "the record header did not read back in full"};
  }
  if (std::memcmp(read_header.data(), header.data(), kRecordHeaderBytes) != 0) {
    return Error{ErrorCode::io_error, "the record header did not read back identically"};
  }
  RecordFields parsed;
  std::string reason;
  if (!decode_record_header(read_header, parsed, reason)) {
    return Error{ErrorCode::io_error, "the record header read back but fails its own " + reason};
  }
  std::vector<std::byte> read_payload(payload.size());
  if (!payload.empty()) {
    DCE_ASSIGN(payload_read, log_file.read_at(offset + static_cast<std::uint64_t>(kRecordHeaderBytes),
                                              std::span<std::byte>(read_payload)));
    if (payload_read != payload.size()) {
      return Error{ErrorCode::io_error, "the record payload did not read back in full"};
    }
    if (std::memcmp(read_payload.data(), payload.data(), payload.size()) != 0) {
      return Error{ErrorCode::io_error, "the record payload did not read back identically"};
    }
  }
  if (record_self_digest(std::span<const std::byte>(read_header).first(kRecordPrefixBytes), read_payload) !=
      fields.self_digest) {
    return Error{ErrorCode::io_error, "the bytes read back do not hash to the digest that was written"};
  }
  return Status{};
}

Result<CommitReceipt> Store::Impl::commit(RecordType type, LogSequence sequence,
                                          std::span<const std::byte> payload, std::uint64_t epoch) {
  if (read_only) {
    return Error{ErrorCode::denied, "a read-only store cannot append"};
  }
  if (payload.size() > options.max_payload_bytes) {
    return Error{ErrorCode::limit_exceeded, "the payload exceeds the configured bound"};
  }
  const std::uint64_t seq = sequence.value();
  const std::optional<std::uint64_t> next = checked_next<std::uint64_t>(seq);
  if (!next.has_value()) {
    return Error{ErrorCode::overflow, "the log sequence is exhausted"};
  }
  const std::optional<std::uint64_t> record_bytes =
      checked_add<std::uint64_t>(static_cast<std::uint64_t>(kRecordHeaderBytes),
                                 static_cast<std::uint64_t>(payload.size()));
  if (!record_bytes.has_value()) {
    return Error{ErrorCode::overflow, "the record length overflowed"};
  }
  DCE_TRY(ensure_capacity(*record_bytes, seq));

  RecordFields fields;
  fields.type = static_cast<std::uint16_t>(type);
  fields.sequence = seq;
  fields.epoch = epoch;
  fields.payload_length = static_cast<std::uint32_t>(payload.size());
  fields.prev_digest = last_digest;
  std::array<std::byte, kRecordHeaderBytes> header = encode_record_header(fields);
  fields.self_digest = record_self_digest(std::span<const std::byte>(header).first(kRecordPrefixBytes), payload);
  header = encode_record_header(fields);

  std::vector<std::byte> bytes;
  bytes.reserve(static_cast<std::size_t>(*record_bytes));
  bytes.insert(bytes.end(), header.begin(), header.end());
  bytes.insert(bytes.end(), payload.begin(), payload.end());

  DCE_TRY(open_log());
  if (tail_dirty) {
    DCE_TRY(log_file.truncate(active_size));
    tail_dirty = false;
  }
  DCE_TRY(log_file.write_at(active_size, bytes));

  // The verify step is not optional: the bytes that were written are read back
  // and checked against the record that was planned before anything is
  // published.
  const Status verified = verify_written(active_size, header, fields, payload);
  if (!verified.ok()) {
    tail_dirty = true;
    return verified.error();
  }
  bool durable = false;
  if (options.sync_on_commit) {
    const Status synced = log_file.sync();
    if (!synced.ok()) {
      tail_dirty = true;
      return synced.error();
    }
    durable = true;
  }

  // Publish: the record is in the log and the manifest records it. The manifest
  // is written last, so a crash before this point leaves a log the next
  // recovery reads further than the manifest expected, never the other way
  // round.
  Manifest updated = manifest;
  updated.next_sequence = *next;
  updated.active_log_base = active_base;
  DCE_TRY(write_manifest(updated));

  active_size += *record_bytes;
  last_digest = fields.self_digest;
  remember_segment(seq);

  CommitReceipt receipt;
  receipt.sequence = LogSequence{seq};
  receipt.epoch = CoordinatorEpoch{epoch};
  receipt.record_digest = fields.self_digest;
  receipt.durable = durable;
  receipt.duplicate = false;
  return receipt;
}

Result<std::optional<std::pair<RecordFields, std::vector<std::byte>>>> Store::Impl::find_record(
    std::uint64_t sequence) {
  const std::uint64_t header_bytes = static_cast<std::uint64_t>(kRecordHeaderBytes);
  for (const SegmentInfo& segment : segments) {
    DCE_ASSIGN(file, platform::File::open(platform::join_path(directory, segment.name),
                                          platform::OpenMode::read_only));
    DCE_ASSIGN(file_size, file.size());
    std::uint64_t offset = 0;
    while (offset + header_bytes <= file_size) {
      std::array<std::byte, kRecordHeaderBytes> header{};
      DCE_ASSIGN(header_read, file.read_at(offset, std::span<std::byte>(header)));
      if (header_read != kRecordHeaderBytes) {
        break;
      }
      RecordFields fields;
      std::string reason;
      if (!decode_record_header(header, fields, reason)) {
        break;
      }
      if (fields.sequence > sequence) {
        return std::optional<std::pair<RecordFields, std::vector<std::byte>>>{};
      }
      if (fields.sequence == sequence) {
        if (static_cast<std::uint64_t>(fields.payload_length) >
            static_cast<std::uint64_t>(options.max_payload_bytes)) {
          return Error{ErrorCode::corruption, "a stored record declares a payload above the configured bound"};
        }
        std::vector<std::byte> payload(static_cast<std::size_t>(fields.payload_length));
        if (!payload.empty()) {
          DCE_ASSIGN(payload_read, file.read_at(offset + header_bytes, std::span<std::byte>(payload)));
          if (payload_read != payload.size()) {
            return Error{ErrorCode::corruption, "a stored record is truncated"};
          }
        }
        if (record_self_digest(std::span<const std::byte>(header).first(kRecordPrefixBytes), payload) !=
            fields.self_digest) {
          return Error{ErrorCode::corruption, "a stored record fails its own digest"};
        }
        return std::optional<std::pair<RecordFields, std::vector<std::byte>>>(
            std::make_pair(fields, std::move(payload)));
      }
      offset += header_bytes + static_cast<std::uint64_t>(fields.payload_length);
    }
  }
  return std::optional<std::pair<RecordFields, std::vector<std::byte>>>{};
}

Status Store::Impl::write_snapshot(std::span<const std::byte> image, std::uint64_t committed) {
  if (read_only) {
    return Error{ErrorCode::denied, "a read-only store cannot write a snapshot"};
  }
  if (image.size() > kMaxSnapshotBytes) {
    return Error{ErrorCode::limit_exceeded, "the snapshot image exceeds the documented bound"};
  }
  const std::optional<std::uint64_t> after = checked_next<std::uint64_t>(committed);
  if (!after.has_value()) {
    return Error{ErrorCode::overflow, "the committed sequence cannot be advanced past a snapshot"};
  }
  const std::string final_path = platform::join_path(directory, snapshot_name(committed));
  // The temporary file must live in the store directory rather than in the
  // process working directory: publishing it is an atomic replacement, and a
  // rename is only atomic within one volume - across volumes it fails outright.
  const std::string temp_path = platform::join_path(directory, snapshot_temp_name(committed));
  DCE_ASSIGN(stale, platform::path_exists(temp_path));
  if (stale) {
    DCE_TRY(platform::remove_file(temp_path));
  }
  {
    DCE_ASSIGN(temp, platform::File::open(temp_path, platform::OpenMode::create_new));
    std::array<std::byte, kSnapshotHeaderBytes> header{};
    put_u32(header, 0, kSnapshotMagic);
    put_u16(header, 4, kStoreFormatVersion);
    put_u16(header, 6, 0);
    put_u64(header, 8, committed);
    put_u32(header, 16, static_cast<std::uint32_t>(image.size()));
    put_u32(header, 20, crc32(image));
    put_digest(header, 24, Digest256::of(image));
    DCE_TRY(temp.write_at(0, header));
    if (!image.empty()) {
      DCE_TRY(temp.write_at(static_cast<std::uint64_t>(kSnapshotHeaderBytes), image));
    }
    DCE_TRY(temp.sync());
    Digest256 written_digest;
    DCE_ASSIGN(read_back, load_snapshot_file(temp_path, committed, static_cast<std::uint64_t>(image.size()),
                                             written_digest));
    if (read_back.size() != image.size() ||
        (!image.empty() && std::memcmp(read_back.data(), image.data(), image.size()) != 0)) {
      return Error{ErrorCode::io_error, "the snapshot image did not read back identically"};
    }
    DCE_TRY(temp.close());
  }
  DCE_TRY(platform::atomic_replace(temp_path, final_path));
  DCE_TRY(platform::sync_directory(directory));

  const std::uint64_t base = *after;
  Manifest updated = manifest;
  updated.snapshot_sequence = committed;
  updated.snapshot_length = static_cast<std::uint64_t>(image.size());
  updated.active_log_base = base;
  updated.next_sequence = base;
  DCE_TRY(write_manifest(updated));
  DCE_TRY(platform::sync_directory(directory));

  // Only now, with the snapshot and its manifest durable, is the log rolled and
  // the superseded prefix retired.
  DCE_TRY(close_log());
  active_base = base;
  active_size = 0;
  last_digest = Digest256::zero();
  tail_dirty = false;
  DCE_TRY(retire_superseded());
  return Status{};
}

Status Store::Impl::shutdown() {
  Status first_failure;
  if (log_file.is_open()) {
    const Status closed = close_log();
    if (!closed.ok()) {
      first_failure = closed;
    }
  }
  if (manifest_file.is_open()) {
    const Status closed = manifest_file.close();
    if (first_failure.ok() && !closed.ok()) {
      first_failure = closed;
    }
  }
  if (lock.held()) {
    const Status released = lock.release();
    if (first_failure.ok() && !released.ok()) {
      first_failure = released;
    }
  }
  opened = false;
  return first_failure;
}

// ---------------------------------------------------------------------------
// Store
// ---------------------------------------------------------------------------
Store::Store() noexcept = default;

Store::Store(Store&& other) noexcept = default;

Store& Store::operator=(Store&& other) noexcept = default;

Store::~Store() {
  if (impl_ != nullptr) {
    (void)impl_->shutdown();
  }
}

Result<Store> Store::open(std::string_view directory, const StoreOptions& options, OpenMode mode) {
  if (directory.empty()) {
    return Error{ErrorCode::invalid_argument, "a store needs a directory"};
  }
  if (directory.find('\0') != std::string_view::npos) {
    return Error{ErrorCode::invalid_argument, "the store directory contains an embedded NUL byte"};
  }
  if (options.max_payload_bytes == 0 || options.max_payload_bytes > kMaxRecordPayloadBytes) {
    return Error{ErrorCode::invalid_argument, "max_payload_bytes is outside the documented record bound"};
  }
  if (options.max_segments == 0) {
    return Error{ErrorCode::invalid_argument, "a store needs at least one log segment"};
  }
  if (options.max_log_bytes == 0) {
    return Error{ErrorCode::invalid_argument, "a store needs a non-zero log byte budget"};
  }

  auto impl = std::make_unique<Store::Impl>();
  impl->options = options;
  impl->directory = std::string(directory);
  impl->read_only = mode == OpenMode::read_only;

  // A writer creates the directory; a reader that finds none has nothing to
  // read and never creates anything.
  if (!impl->read_only) {
    DCE_TRY(platform::make_directories(impl->directory));
  }
  DCE_ASSIGN(directory_stat, platform::stat_path(impl->directory));
  if (!directory_stat.is_directory) {
    return Error{ErrorCode::invalid_argument, "the store path is not a directory"};
  }

  // The exclusive lock is the only thing that makes a live stale writer
  // impossible on one host, and it is taken before any byte is read.
  if (!impl->read_only) {
    DCE_ASSIGN(lock, platform::FileLock::acquire(impl->lock_path(), true));
    impl->lock = std::move(lock);
  }

  DCE_ASSIGN(has_manifest, platform::path_exists(impl->manifest_path()));
  if (has_manifest) {
    DCE_TRY(impl->load_manifest());
  } else if (impl->read_only) {
    return Error{ErrorCode::not_found, "the store directory holds no MANIFEST"};
  } else {
    DCE_TRY(impl->refuse_uninitialised_directory());
    DCE_TRY(impl->create_initial_manifest());
  }

  // The epoch that recovery ran under, before a read-write open advances it.
  const std::uint64_t recovered_epoch = impl->manifest.epoch;
  bool snapshot_loaded = false;
  Digest256 snapshot_digest;
  if (impl->manifest.snapshot_sequence > 0) {
    Digest256 digest;
    Result<std::vector<std::byte>> loaded =
        load_snapshot_file(platform::join_path(impl->directory, snapshot_name(impl->manifest.snapshot_sequence)),
                           impl->manifest.snapshot_sequence, impl->manifest.snapshot_length, digest);
    if (!loaded.ok()) {
      return loaded.error();
    }
    snapshot_loaded = true;
    snapshot_digest = digest;
  }

  ScanRequest request;
  request.options = &impl->options;
  request.directory = impl->directory;
  request.snapshot_sequence = impl->manifest.snapshot_sequence;
  request.manifest_next_sequence = impl->manifest.next_sequence;
  request.repair = !impl->read_only;
  request.collect_entries = false;
  DCE_ASSIGN(scan, scan_log(request));

  const std::optional<std::uint64_t> next = checked_next<std::uint64_t>(scan.committed);
  if (!next.has_value()) {
    return Error{ErrorCode::overflow, "the committed sequence is exhausted"};
  }
  impl->segments = scan.segments;
  // The active segment is the one the manifest names when it exists, and
  // otherwise the highest-based segment on disk; a manifest that names a
  // segment which was never created is the roll that a crash interrupted, and
  // the next record starts it.
  const std::uint64_t greatest = impl->segments.empty() ? 0 : impl->segments.back().base;
  const std::uint64_t candidate = std::max(impl->manifest.active_log_base, greatest);
  const bool candidate_exists = !impl->segments.empty() && impl->segments.back().base == candidate;
  impl->active_base = candidate_exists ? candidate : *next;
  impl->active_size = candidate_exists ? impl->segments.back().size : 0;
  if (impl->active_base == 0 || impl->active_base > *next) {
    return Error{ErrorCode::corruption,
                 "the active log base " + text::u64_to_string(impl->active_base) +
                     " is beyond the next sequence " + text::u64_to_string(*next)};
  }
  impl->last_digest = (impl->manifest.snapshot_sequence > 0 && scan.committed == impl->manifest.snapshot_sequence)
                          ? Digest256::zero()
                          : scan.last_digest;

  std::uint64_t epoch = recovered_epoch;
  if (!impl->read_only) {
    const std::optional<std::uint64_t> bumped = checked_next<std::uint64_t>(recovered_epoch);
    if (!bumped.has_value()) {
      return Error{ErrorCode::overflow, "the coordinator epoch is exhausted"};
    }
    epoch = *bumped;
    Manifest updated = impl->manifest;
    updated.epoch = epoch;
    updated.active_log_base = impl->active_base;
    updated.next_sequence = *next;
    DCE_TRY(impl->write_manifest(updated));
  }
  impl->opened = true;

  RecoveryReport report;
  report.records_recovered = scan.records_validated;
  report.bytes_scanned = scan.bytes_scanned;
  report.torn_tail_bytes_discarded = scan.discarded_bytes;
  report.torn_tail_recovered = scan.torn_tail;
  report.committed_sequence = LogSequence{scan.committed};
  report.epoch = CoordinatorEpoch{recovered_epoch};
  report.segments_scanned = scan.segments_scanned;
  report.snapshot_loaded = snapshot_loaded;
  report.snapshot_digest = snapshot_digest;
  std::string detail = text::u64_to_string(scan.records_validated) + " record(s) validated across " +
                       text::u32_to_string(scan.segments_scanned) + " log segment(s)";
  if (scan.torn_tail) {
    detail += impl->read_only ? "; a torn tail was found and left in place ("
                              : "; a torn tail was truncated (";
    detail += text::u64_to_string(scan.discarded_bytes) + " trailing byte(s))";
  }
  if (scan.manifest_ahead) {
    detail += "; the manifest expected the next record to be sequence " +
              text::u64_to_string(scan.manifest_expected) + " while the log ends at sequence " +
              text::u64_to_string(scan.committed) + ", which the discarded tail accounts for";
  }
  if (snapshot_loaded) {
    detail += "; snapshot at sequence " + text::u64_to_string(impl->manifest.snapshot_sequence) + " loaded";
  }
  report.detail = std::move(detail);

  Store store;
  store.impl_ = std::move(impl);
  store.recovery_ = std::move(report);
  store.epoch_ = CoordinatorEpoch{epoch};
  store.committed_sequence_ = LogSequence{scan.committed};
  store.directory_ = std::string(directory);
  return store;
}

Result<CommitReceipt> Store::append(RecordType type, std::span<const std::byte> payload) {
  if (impl_ == nullptr || !impl_->opened) {
    return Error{ErrorCode::closed, "append: the store is not open"};
  }
  if (impl_->read_only) {
    return Error{ErrorCode::denied, "append: a read-only store cannot be written"};
  }
  if (payload.size() > impl_->options.max_payload_bytes) {
    return Error{ErrorCode::limit_exceeded,
                 "append: the payload exceeds the configured bound of " +
                     text::u64_to_string(static_cast<std::uint64_t>(impl_->options.max_payload_bytes)) + " bytes"};
  }
  const std::optional<std::uint64_t> next = checked_next<std::uint64_t>(committed_sequence_.value());
  if (!next.has_value()) {
    return Error{ErrorCode::overflow, "append: the log sequence is exhausted"};
  }
  DCE_ASSIGN(receipt, impl_->commit(type, LogSequence{*next}, payload, epoch_.value()));
  committed_sequence_ = receipt.sequence;
  return receipt;
}

Result<CommitReceipt> Store::replay(RecordType type, LogSequence sequence, std::span<const std::byte> payload) {
  if (impl_ == nullptr || !impl_->opened) {
    return Error{ErrorCode::closed, "replay: the store is not open"};
  }
  if (impl_->read_only) {
    return Error{ErrorCode::denied, "replay: a read-only store cannot be written"};
  }
  if (payload.size() > impl_->options.max_payload_bytes) {
    return Error{ErrorCode::limit_exceeded, "replay: the payload exceeds the configured bound"};
  }
  const std::uint64_t seq = sequence.value();
  if (seq == 0) {
    return Error{ErrorCode::out_of_range, "replay: sequence zero is not a valid log sequence"};
  }
  const std::uint64_t committed = committed_sequence_.value();
  if (seq > committed) {
    const std::optional<std::uint64_t> next = checked_next<std::uint64_t>(committed);
    if (!next.has_value()) {
      return Error{ErrorCode::overflow, "replay: the log sequence is exhausted"};
    }
    if (seq != *next) {
      // The sequence is well formed but ahead of the only sequence this store
      // accepts next, so it is out of range rather than stale: stale would say
      // the request is older than the authority it targets, and this one is
      // newer.
      return Error{ErrorCode::out_of_range,
                   "replay: sequence " + text::u64_to_string(seq) + " is ahead of the log, which ends at " +
                       text::u64_to_string(committed) + " and accepts only " + text::u64_to_string(*next) + " next"};
    }
    DCE_ASSIGN(receipt, impl_->commit(type, sequence, payload, epoch_.value()));
    committed_sequence_ = receipt.sequence;
    return receipt;
  }

  DCE_ASSIGN(found, impl_->find_record(seq));
  if (!found.has_value()) {
    if (impl_->manifest.snapshot_sequence >= seq) {
      return Error{ErrorCode::stale,
                   "replay: sequence " + text::u64_to_string(seq) +
                       " is covered by the snapshot at sequence " +
                       text::u64_to_string(impl_->manifest.snapshot_sequence) +
                       " and its record is no longer retained"};
    }
    return Error{ErrorCode::corruption, "replay: sequence " + text::u64_to_string(seq) +
                                            " is committed but no record for it exists in the log"};
  }

  const RecordFields& stored = found->first;
  RecordFields candidate;
  candidate.type = static_cast<std::uint16_t>(type);
  candidate.sequence = seq;
  candidate.epoch = epoch_.value();
  candidate.payload_length = static_cast<std::uint32_t>(payload.size());
  candidate.prev_digest = stored.prev_digest;
  const std::array<std::byte, kRecordHeaderBytes> header = encode_record_header(candidate);
  const Digest256 recomputed = record_self_digest(std::span<const std::byte>(header).first(kRecordPrefixBytes), payload);
  if (recomputed != stored.self_digest) {
    return Error{ErrorCode::conflict,
                 "replay: sequence " + text::u64_to_string(seq) +
                     " is already committed with different bytes or a different type"};
  }
  CommitReceipt receipt;
  receipt.sequence = sequence;
  receipt.epoch = CoordinatorEpoch{stored.epoch};
  receipt.record_digest = stored.self_digest;
  // No sync happened in this call, so no durability is claimed for it; the
  // duplicate flag says the record is already in the log.
  receipt.durable = false;
  receipt.duplicate = true;
  return receipt;
}

Result<RecoveredState> Store::recover() const {
  if (impl_ == nullptr || !impl_->opened) {
    return Error{ErrorCode::closed, "recover: the store is not open"};
  }
  ScanRequest request;
  request.options = &impl_->options;
  request.directory = impl_->directory;
  request.snapshot_sequence = impl_->manifest.snapshot_sequence;
  request.manifest_next_sequence = impl_->manifest.next_sequence;
  request.repair = false;  // a reader never repairs
  request.collect_entries = true;
  DCE_ASSIGN(scan, scan_log(request));

  RecoveredState state;
  state.snapshot_sequence = LogSequence{impl_->manifest.snapshot_sequence};
  if (impl_->manifest.snapshot_sequence > 0) {
    Digest256 digest;
    DCE_ASSIGN(image, load_snapshot_file(platform::join_path(impl_->directory, snapshot_name(impl_->manifest.snapshot_sequence)),
                                         impl_->manifest.snapshot_sequence, impl_->manifest.snapshot_length, digest));
    state.snapshot = std::move(image);
    state.snapshot_digest = digest;
  }
  state.records = std::move(scan.entries);
  return state;
}

Status Store::snapshot(std::span<const std::byte> image) {
  if (impl_ == nullptr || !impl_->opened) {
    return Error{ErrorCode::closed, "snapshot: the store is not open"};
  }
  if (impl_->read_only) {
    return Error{ErrorCode::denied, "snapshot: a read-only store cannot be written"};
  }
  if (image.size() > kMaxSnapshotBytes) {
    return Error{ErrorCode::limit_exceeded, "snapshot: the image exceeds the documented bound"};
  }
  if (committed_sequence_.is_zero()) {
    // Sequence zero means "no snapshot" in the manifest, so an image that
    // covers no record cannot be represented and is refused rather than
    // silently discarded.
    return Error{ErrorCode::refused, "snapshot: the store has no committed record for an image to cover"};
  }
  return impl_->write_snapshot(image, committed_sequence_.value());
}

Status Store::close() {
  if (impl_ == nullptr || !impl_->opened) {
    return Status{};
  }
  return impl_->shutdown();
}

bool Store::is_open() const noexcept { return impl_ != nullptr && impl_->opened; }

const char* to_string(RecordType type) noexcept {
  switch (type) {
    case RecordType::epoch_bump:
      return "epoch_bump";
    case RecordType::plan_revision:
      return "plan_revision";
    case RecordType::membership_snapshot:
      return "membership_snapshot";
    case RecordType::validation_report:
      return "validation_report";
    case RecordType::plan_state_change:
      return "plan_state_change";
    case RecordType::stage_checkpoint:
      return "stage_checkpoint";
    case RecordType::migration_receipt:
      return "migration_receipt";
    case RecordType::rollback_marker:
      return "rollback_marker";
    case RecordType::reconciliation_record:
      return "reconciliation_record";
    case RecordType::site_observation:
      return "site_observation";
    case RecordType::capability_dependency_proof:
      return "capability_dependency_proof";
    case RecordType::exception_grant:
      return "exception_grant";
  }
  return "unknown";
}

}  // namespace dce::persist
