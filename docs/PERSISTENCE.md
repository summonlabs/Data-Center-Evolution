# Persistence and recovery

This document describes the on-disk format, the durability boundary, and what
recovery does and does not repair. It is normative: a change to any of it
requires a format version bump and an explicit compatibility statement.

## The durability claim

The store is the only thing in this runtime that claims durability, and the
claim is precise:

> A record is durable once `Store::append` has returned `ok` with
> `CommitReceipt::durable == true`. That means `platform::File::sync()` returned
> success for the segment file, which is `FlushFileBuffers` on Windows and
> `fsync` on POSIX.

Nothing before that point is promised to survive a crash. Nothing after it is
allowed to disappear. When `sync_on_commit` is disabled, `append` reports
`durable == false` rather than claiming a durability it did not perform.

## Layout

`@
<store directory>/
  MANIFEST            two fixed 4096-byte slots, written alternately
  SNAPSHOT-<seq>      a full state image covering every record up to <seq>
  LOG-<base>          an append-only segment whose first record is <base>
  STORE.LOCK          the exclusive writer lock
`@

### MANIFEST slot

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | magic `0x4D434544` |
| 4 | 2 | format version |
| 6 | 2 | slot index (0 or 1) |
| 8 | 8 | slot generation, increments on every manifest write |
| 16 | 8 | coordinator epoch |
| 24 | 8 | snapshot sequence (0 when none) |
| 32 | 8 | active log base sequence |
| 40 | 8 | next sequence |
| 48 | 32 | store identity digest |
| 80 | 8 | snapshot length in bytes |
| 88 | 4 | CRC-32 over bytes 0..87 |
| 92 | 32 | SHA-256 over bytes 0..91 |

A slot is valid when the magic, the version, the CRC and the SHA-256 all check
out and the fields are internally consistent. The newest valid slot wins. The
writer always writes the slot it is not currently using, so a crash in the
middle of a manifest write leaves the previous slot intact and the store
readable.

### Record

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | magic `0x52454344` |
| 4 | 2 | format version |
| 6 | 2 | record type |
| 8 | 8 | sequence |
| 16 | 8 | epoch |
| 24 | 4 | payload length |
| 28 | 4 | reserved, zero |
| 32 | 32 | `prev_digest`, the `self_digest` of the previous record |
| 64 | 32 | `self_digest`, SHA-256 over header bytes 0..63 and the payload |
| 96 | 4 | CRC-32 over header bytes 0..95 |
| 100 | *n* | payload |

There is no padding and no alignment. The chain of `prev_digest` values makes a
deleted, reordered or substituted record detectable rather than merely unlikely.

## Write path

`@
plan the record
  -> validate the payload against the configured bound
  -> reserve the next sequence with a checked increment
  -> build the bytes with prev_digest taken from the last record
  -> write at the current end of the active segment
  -> read the bytes back and verify them
  -> flush (sync) when the option is enabled
  -> publish the committed sequence
`@

The read-back verification step is not optional: a write that the operating
system accepted but that did not reach the disk as written is exactly the
failure this step catches before it is published as committed.

## Recovery

Recovery walks the manifest, then the snapshot, then the segments in ascending
base order, then the records within each segment in order. Every record is
checked for its header CRC, its magic, its format version, sequence continuity,
the `prev_digest` chain and its recomputed `self_digest`.

The classification that matters:

* **Torn tail** - a short or partial record at the very end of the log, or a
  final record whose checks fail with nothing valid after it. The active
  segment is truncated to the last valid boundary, `torn_tail_recovered` is set,
  and the discarded byte count is reported. Only the active segment may be
  truncated, and only at its tail.
* **Interior corruption** - a record whose checks fail while a later record with
  a valid chain exists, or a gap or regression in sequence numbers. Recovery
  returns `ErrorCode::corruption`, names the segment and the byte offset, and
  **never truncates**. A truncated log would silently discard committed state,
  which is worse than refusing to open.
* **Invalid manifest** - both slots invalid is corruption, not an empty store.
  The store refuses to open rather than starting over.

The distinction is made by scanning forward for the next record that both parses
and continues the digest chain. If one exists, the damage is interior.

A read-only open never repairs anything: a reader that meets a torn tail reports
it.

## Fencing

Opening a store for writing advances the coordinator epoch and persists it
**before** returning. Because every authority token, receipt and plan revision
carries the epoch it was issued under, a coordinator that takes over
automatically fences everything the previous one issued. A second writer on the
same directory is refused with `busy` by the exclusive lock rather than being
allowed to interleave.

A read-only open takes no lock and does not touch the epoch, so an operator can
inspect a store while a coordinator is running.

## Idempotency and replay

`Store::replay` accepts an already-numbered record. A sequence at or below the
committed sequence is a `duplicate` when the stored record's digest matches and
a `conflict` when it does not; the next sequence behaves exactly like `append`;
anything further ahead is a gap and is refused.

## Snapshots and compaction

`@
write SNAPSHOT-<seq>.tmp
  -> sync it
  -> read it back and verify magic, version, length, CRC and SHA-256
  -> atomic replace into SNAPSHOT-<seq>
  -> sync the directory
  -> write the MANIFEST slot pointing at it
  -> sync the manifest and the directory
  -> only now retire
`@

Retirement removes snapshot files with a strictly lower sequence and log
segments in which **every** record has a sequence at or below the snapshot
sequence. A segment that still holds a record above the snapshot sequence is
never removed, which is what stops compaction from discarding uncommitted or
newer state. After a successful snapshot the active segment is rolled, so the
next append starts a fresh `LOG-<base>`.

The coordinator writes a snapshot once `snapshot_every_records` records have
accumulated since the previous one, which bounds both recovery time and log
growth without ever discarding post-snapshot state.

## Platform limitation, stated honestly

`sync_directory` is a real `fsync` of the directory on POSIX. On Windows it is
a deliberate no-op: NTFS metadata ordering is handled by the filesystem and
there is no portable directory handle to flush. Consumers relying on rename
durability across a power loss on Windows should treat that as an untested
capability rather than a proven one.
