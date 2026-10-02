# Data Center Evolution

Data Center Evolution (DCE) is the DCCP boundary that governs **live evolution**
of a running multi-site data-center control plane: moving a fleet from one
compatible system state to another across versions, capabilities, sites, policy
generations and lower-layer dependencies, without a global shutdown.

It is the final boundary of the Data Center Control Plane. It owns evolution
plans, compatibility staging, rollout generations, gates, rollback eligibility,
migration authority and the evidence that a heterogeneous fleet can move
between control-plane versions safely.

**It is not a software updater.** It governs eligibility and proof; the
mechanism of any individual runtime's upgrade belongs to the runtime that owns
that runtime.

---

## Contents

- [What this boundary owns](#what-this-boundary-owns)
- [What it does not own](#what-it-does-not-own)
- [Build](#build)
- [Test](#test)
- [Install and consume](#install-and-consume)
- [Command line](#command-line)
- [A complete worked run](#a-complete-worked-run)
- [Using the library](#using-the-library)
- [The model](#the-model)
- [Authority and generations](#authority-and-generations)
- [Persistence and recovery](#persistence-and-recovery)
- [Platform support and limitations](#platform-support-and-limitations)
- [Validation actually performed](#validation-actually-performed)
- [Benchmarks](#benchmarks)
- [Adjacent boundaries](#adjacent-boundaries)

---

## What this boundary owns

* **Evolution plans** and their deterministic digest.
* **Compatibility staging**: the capability matrix and the compatibility graph.
* **Rollout generations**: cohorts, waves, canary subsets, per-site stage.
* **Gates** and the evidence that satisfies them.
* **Migration receipts**, checkpoints, and the durable record of what was accepted.
* **Rollback eligibility** and the point of no return.
* **Reconciliation** of a reconnecting site against what it actually accepted.
* **Exception authority**, bounded and explicit.
* **Evidence provenance**: who asserted what, about which generation.

## What it does not own

* the internals of any individual runtime's upgrade;
* firmware rollout;
* facility-change execution;
* policy semantics (it consumes a policy generation identity and checks the
  requirement; it does not interpret policy);
* federation membership;
* ASI or DFI evolution internals.

Every reference to a neighbouring runtime is an **explicit contract**: an
identifier the neighbour published, a version range, a capability set, an
observed generation, and the evidence that vouches for the observation. A
dependency that was never observed is reported as *indeterminate*, which is
deliberately not the same as compatible. Nothing is inferred from a version
string.

---

## Build

Requires CMake 3.20 or newer and a C++20 compiler. There are **no third-party
dependencies**: the standard library and the operating system only.

`@
cmake -S . -B build/release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/release

cmake -S . -B build/debug -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build/debug
`@

First-party code builds with **zero warnings** under
`-Wall -Wextra -Wpedantic -Wshadow -Wnon-virtual-dtor -Wcast-qual -Wunused
-Woverloaded-virtual -Wold-style-cast -Wdouble-promotion -Wformat=2 -Wconversion
-Wsign-conversion -Werror` and under MSVC `/W4 /permissive- /WX`. Warnings are
fixed at the cause; there is no blanket suppression anywhere in the tree.

Options:

| Option | Default | Meaning |
|---|---|---|
| `DCE_BUILD_TESTS` | ON | the test suites and `dce_test_*` executables |
| `DCE_BUILD_TOOLS` | ON | the `dce` command line executable |
| `DCE_BUILD_EXAMPLES` | ON | the worked examples |
| `DCE_BUILD_BENCHMARKS` | ON | the `dce_bench` measurement tool |
| `DCE_WARNINGS_AS_ERRORS` | ON | first-party warnings are errors |
| `DCE_ENABLE_ASAN` | OFF | AddressSanitizer where the toolchain ships it |
| `DCE_ENABLE_UBSAN` | OFF | UndefinedBehaviorSanitizer where supported |

## Test

`@
ctest --test-dir build/debug --output-on-failure
`@

Tests are run plainly. There are **no timeouts of any kind** anywhere in this
repository: no CTest `TIMEOUT` property, no `timeout` wrapper, no
`timeout-minutes` in CI, and no logic that kills a process and calls the result
a pass. A test that hangs is a defect to diagnose.

Each suite is one executable whose cases register themselves before `main` runs,
so registration order is deterministic and there is no discovery step. A suite
accepts `--list`, `--filter <substring>` and `--repeat <n>`.

## Install and consume

`@
cmake --install build/release --prefix /some/prefix
`@

An independent consumer uses the installed package and nothing from the build
tree:

`@cmake
find_package(DataCenterEvolution CONFIG REQUIRED)
add_executable(consumer main.cpp)
target_link_libraries(consumer PRIVATE DataCenterEvolution::dce)
`@

`examples/downstream-consumer` is exactly such a project. It is deliberately
**not** part of this build: it is configured separately with its own source and
binary directories and `-DCMAKE_PREFIX_PATH=<prefix>`, so that it can only ever
link the installed library. The package config propagates the transitive
`Threads::Threads` dependency itself, so the consumer needs no other
`find_package` call.

---

## Command line

The `dce` executable is a real operator surface. Every decision it reports
comes from the runtime, never from the tool.

`@
dce version
dce coordinator serve --store DIR [--port N]
dce site serve --store DIR --profile FILE --coordinator-port N [--delegated] [--rounds N]
dce synthetic --seed S [--sites N] [--components N] [--cohorts N] [--versions N]
              --plan-out FILE --profiles-out DIR
dce plan show --plan FILE
dce plan digest --plan FILE
dce plan submit --port N --plan FILE
dce plan validate --port N
dce status --port N
dce observe --port N --profiles DIR
dce event --port N --event NAME [--gates-satisfied] [--all-cohorts] [--rollback-eligible]
dce rollback-eligibility --port N --wave W
`@

`coordinator serve` prints `LISTEN <port>` on stdout once it is accepting, and
`site serve` prints `SITE <id> ready`. Those two lines are how a supervising
process knows a node is up without guessing at timing.

A plan file is the canonical encoding of an `EvolutionPlan` — the same bytes a
coordinator commits under its plan-revision record. `dce plan digest` prints the
digest of that content; `dce plan show` prints the whole model.

## A complete worked run

This sequence is the whole boundary, and it is what the multi-process suite
performs with real child processes.

`@
# 1. Generate a fleet and a plan written against it.
dce synthetic --seed 20260101 --sites 6 --components 2 --cohorts 2 \
    --plan-out plan.dce --profiles-out profiles

# 2. Start the coordinator. It prints the port it bound.
dce coordinator serve --store coordinator-store --port 0
#   LISTEN 52341

# 3. Start one site process per profile, each with its own durable store.
dce site serve --store site-0-store --profile profiles/site-0.site \
    --coordinator-port 52341 --delegated --rounds 500

# 4. The coordinator observes each site from its own report as it connects.
#    Validate the plan against the fleet that actually exists.
dce plan submit --port 52341 --plan plan.dce
dce plan validate --port 52341
#   verdict         accepted
#   checked         sites=6 steps=12 edges=19 gates=2

# 5. Drive the lifecycle. Each event is explicit; none is implied.
dce event --port 52341 --event validate       --gates-satisfied
dce event --port 52341 --event stage          --gates-satisfied
dce event --port 52341 --event begin_rollout  --gates-satisfied

# 6. Watch progress. The sites advance one stage at a time, in cohort order.
dce status --port 52341
#   state           rolling_out
#   sites           6 complete 6
#   receipts        24
#   checkpoints     24
#   refusals        0
`@

`dce rollback-eligibility --port 52341 --wave 0` answers whether the rollout may
still return to wave 0, and exits non-zero when it may not.

## Using the library

`@cpp
#include "dce/synthetic.hpp"
#include "dce/validate.hpp"

dce::SyntheticFleetOptions options;
options.sites = 6;
options.components = 2;
options.cohorts = 2;
options.seed = 20260101;
dce::SyntheticFleet fleet = *dce::make_synthetic_fleet(options);

// A plan is validated against the fleet it was written for. The context carries
// the coordinator's epoch and generation and what it currently observes.
dce::ValidationContext context;
context.epoch = fleet.plan.identity.epoch;
context.current_generation = fleet.plan.identity.generation;
context.observed_sites = fleet.initial_observations;

dce::Result<dce::ValidationReport> report = dce::validate_plan(fleet.plan, context);
if (!report.ok() || !report->accepted()) {
  for (const dce::Refusal& refusal : report->refusals) {
    // Every refusal names its subject and explains itself.
    std::printf("%s [%s] %s\n", dce::to_string(refusal.code), refusal.subject.c_str(),
                refusal.explanation.c_str());
  }
}

// Rollout decisions are a pure function of the plan and the observed state.
dce::RolloutState state = *dce::initial_rollout_state(fleet.plan);
dce::RolloutAction action = *dce::next_rollout_action(fleet.plan, state);
`@

The examples directory contains two complete programs: `evolve_fleet` drives a
synthetic fleet through validation, the lifecycle and a full deterministic
rollout sweep, and `inspect_plan` decodes a plan file, verifies its digest and
lists its contents.

---

## The model

### Identity, generations and stages

Every authoritative object has a stable, validated identity. The accepted
spelling is deliberately narrow, because identities are also used as file names:
no path separators, no `..`, no colon, no control bytes, no Windows reserved
device names, bounded length.

Counters are distinct types, never bare integers: `PlanRevision`,
`PlanGeneration`, `CoordinatorEpoch`, `SiteGeneration`, `StageOrdinal`,
`CohortWave`, `LogSequence`.

A site's `StageOrdinal` is **the number of migration steps it has durably
accepted**. `0` means nothing accepted; `stage_count(plan)` means fully
evolved. The step that moves a site from ordinal *n* to *n+1* is `plan.steps[n]`
in canonical step order, and a site may only ever advance by exactly one stage.
That is how "sites cannot skip gates" is enforced structurally rather than by
convention.

### Compatibility is evidence-driven

The capability matrix states, for every `(component, version)`, what that
version provides and requires. Two versions interoperate only when **both**
directions of the requirement hold; an undescribed version is `unknown`, never
assumed compatible.

The compatibility graph has `(component, version)` nodes and evidence-backed
edges carrying the capabilities that must already be present, the capabilities
that appear afterwards, reversibility, and a provenance identity. An edge
without provenance is a claim, not a permission. Analysis reports self loops,
cycles, orphan nodes, unsupported downgrades, missing provenance and capability
regressions; path search is a deterministic breadth-first walk that honours
capability preconditions, so an impossible plan is refused with the capability
that was missing, the version it was needed at, and the frontier that was
actually reachable.

### Lifecycle

`@
draft -> validated -> staged -> rolling_out -> validating -> completed
`@

with `paused`, `blocked`, `reconciled`, `rolling_back`, `rolled_back`,
`aborted` and `failed` reachable where they are legal.
`dce::legal_transitions()` is the single source of truth that the runtime,
the documentation and the test suite all read, so there is no undocumented
transition. Rollback is available only before the recorded irreversible
boundary, and `abort` is refused once the point of no return has been crossed.

## Authority and generations

A site is **sovereign by default**. It acts on evolution only when the plan
explicitly delegates rollout authority to it *and* it presents an authority
token current for the coordinator's epoch and the plan's generation. The verdict
vocabulary is explicit: `valid`, `stale_epoch`, `fenced_generation`,
`plan_mismatch`, `digest_mismatch`, `unknown_site`, `not_delegated`.

Opening a store for writing advances the coordinator epoch, so a coordinator
that takes over automatically fences everything the previous one issued. A plan
recorded under a previous epoch is refused with `stale_plan_epoch` until it is
re-affirmed, and re-affirming it keeps the durable rollout progress of every
site it still contains.

Migration receipts are idempotent by idempotency key: replaying the same effect
is a `duplicate` that changes nothing, while the same key carrying different
content is a `conflicting` that is refused. A receipt whose stage does not
correspond to its step is refused rather than recorded.

### Partition and reconciliation

A partitioned site keeps serving locally — local sovereignty is not suspended by
losing the coordinator. What it loses is the right to advance. When it
reconnects it reports what it actually accepted, and the coordinator moves its
own belief to match reality **before** any new authority is issued.

| Reported | Action |
|---|---|
| matches | `agree` |
| ahead, with delegated authority | `advance` |
| ahead, without delegated authority | `reject` |
| behind | `rewind` |
| same stage, different stage digest | `reject` |
| same stage, earlier generation | `hold` (authority withheld) |

Whenever reconciliation **revises** the coordinator's belief - on `advance`,
`rewind` or `hold` - it issues `next_generation = recorded_generation + 1`
rather than adopting the site's counter, because a generation it may already
have published must never be reused. Generations therefore stay monotonic across
a rewind, and a stale completion from before the rewind cannot mutate the newer
generation. Agreement revises nothing and keeps the current generation; a
rejection revises nothing and issues nothing at all.

## Persistence and recovery

The durable store is a crash-safe record log with snapshots, a double-buffered
manifest, per-record CRC and SHA-256 integrity, and a digest chain that makes a
deleted, reordered or substituted record detectable.

**The durability claim is precise**: a record is durable once
`persist::Store::append` has returned `ok` with `CommitReceipt::durable ==
true`, which means `platform::File::sync()` (FlushFileBuffers or fsync)
returned success. Nothing before that point is promised to survive a crash;
nothing after it is allowed to disappear. When `sync_on_commit` is disabled,
`append` reports `durable == false` rather than claiming a durability it did
not perform.

Recovery distinguishes two failures that must never be conflated:

* a **torn tail** — an incomplete final record with nothing valid after it — is
  truncated to the last valid boundary and reported;
* **interior corruption** — damage with a valid record after it, or a gap in the
  sequence — is refused outright and **never truncated**, because silently
  discarding committed state is worse than refusing to open.

Snapshots are written to a temporary file in the store directory, synced, read
back and verified, atomically replaced, and only then published through the
manifest. Retirement removes only segments whose every record lies at or below
the snapshot sequence, so compaction can never discard post-snapshot state.
The full format is documented in [docs/PERSISTENCE.md](docs/PERSISTENCE.md).

## Platform support and limitations

| Platform | Status |
|---|---|
| Windows, MinGW-w64 GCC 14 | built and tested in this environment |
| Windows, MSVC | supported by the build; CI configures and builds it |
| Linux, GCC and Clang | supported by the build; CI configures and builds it |
| AddressSanitizer + UndefinedBehaviorSanitizer | enabled on Linux CI |
| Windows sanitizers | not claimed: the CI does not run them |

Honest limitations:

* `sync_directory` is a real `fsync` on POSIX and a **deliberate no-op on
  Windows**, where NTFS handles metadata ordering and there is no portable
  directory handle to flush. Rename durability across a power loss on Windows is
  therefore an untested capability, not a proven one.
* `File::sync()` on a read-only handle is refused on Windows, because
  `FlushFileBuffers` requires write access and reporting success without
  flushing would be a false durability claim.
* The durable-operation costs below were measured on one Windows machine with
  MinGW-w64 GCC 14.2.0. They are measurements, not specifications, and no
  before/after comparison is published because there is no baseline in this
  repository to compare against.

---

## Validation actually performed

Everything in this section was executed; nothing is projected.

### Test suites

`ctest` runs one executable per suite. Every suite below passes.

| Suite | What it proves |
|---|---|
| `dce_test_primitives` | checked arithmetic, identifier and text rules, version parsing, SHA-256 and CRC-32 against published vectors, canonical round-trips and adversarial decoding |
| `dce_test_primitives_adversarial` | 20 009 malformed payloads under a fixed seed: decode either succeeds or is refused, never crashes, never reads out of bounds |
| `dce_test_capability` | sets, matrix, interoperation, policy, dependency and removal verdicts |
| `dce_test_compatibility` | graph structure, defect analysis, path search and every refusal kind |
| `dce_test_plan` | canonical ordering, digest stability, structural checks, lossless round-trip |
| `dce_test_lifecycle` | every legal transition driven from the table, and every illegal pair refused |
| `dce_test_rollout` | one stage at a time, cohort order, strategies, partitions, determinism |
| `dce_test_authority` | token verdicts, receipt idempotency, rollback eligibility |
| `dce_test_reconcile` | every reconciliation action and its generation rule |
| `dce_test_validate` | every refusal the validator can produce, one broken thing at a time |
| `dce_test_property` | seeded randomized invariants over many fleet shapes |
| `dce_test_adversarial` | hostile input: path-shaped identities, absurd counts, hostile lengths |
| `dce_test_persistence` | commit, recovery, torn tail, interior corruption, locking, epochs, snapshots, and a **real child process killed mid-write** |
| `dce_test_cli` | the shipped executable's commands, output and exit codes |
| `dce_test_multiprocess` | **real independent site processes** rolling forward, a coordinator killed with `TerminateProcess` and restarted on the same store, and a stopped site reconciled when it returns |

The randomized suites print their seed on failure, so a failure is reproducible
from the message alone.

### Defects found and fixed during this work

These were found by building and running, not by inspection, and each is fixed
in the tree:

1. `requires` as a struct member name — a C++20 keyword, so the header did not
   parse at all. Renamed to `requires_capabilities`.
2. Missing direct includes in five headers, so each was not self-contained.
3. The cohort contiguity rule compared each wave against the last wave seen
   rather than the last wave plus one, so **every** plan with two or more cohorts
   was refused.
4. An inline-defaulted constructor on a pimpl type instantiated the incomplete
   type's deleter in every translation unit that default-constructed it.
5. The wire codec could not represent an **absent** identity, so no site could
   ever send a hello and no administrative request could be decoded.
6. The durable codec could not round-trip a plan whose facility had published no
   policy generation identity.
7. A migration stage offer did not carry the step's versions, so every receipt
   built from it was refused as the wrong stage.
8. A recovered plan revision did not have its derived digest recomputed, so a
   restarted coordinator refused its own recovered plan.
9. The snapshot temporary file was created in the process working directory, so
   publishing it failed across volumes and left debris behind.
10. `:` was accepted in identities that become filename components, which is
    an NTFS alternate-data-stream separator.
11. On POSIX the pipe helper cleared the close-on-exec flag on a pipe that
    `pipe()` had just returned without it, so the exec status pipe's write end
    survived `execv` into the child. Starting a process therefore blocked until
    that process exited: on Linux the crash test took exactly as long as the
    child's own sleep, and the multi-process suite never finished at all. The
    accepted-socket path inverted the same flag, so every accepted connection
    leaked into any child process. Windows was never affected, which is why the
    whole Windows matrix was green throughout.
12. Both suites that drive the shipped executable preferred a `Release` CLI over
    a `Debug` one. A multi-configuration build tree holds both at once, so under
    the Visual Studio generator a Debug test run exercised whichever
    configuration had been built most recently. Each suite now tries the
    configuration it was itself built as first.
13. The multi-process suite validated its plan as soon as the site processes had
    printed their readiness lines, but a site is observable only once its
    process has connected. On a Release build the coordinator could therefore
    observe part of the fleet, and it correctly refused the plan with
    `source_state_stale` and `site_not_observed`. The suite now retries those
    two refusals alone, bounded by attempts rather than by a clock.

### Real multi-process proof

`dce_test_multiprocess` spawns the shipped `dce` executable as real child
processes. It proves, with no simulation:

* four independent site processes at differing versions roll forward through
  every stage under a live coordinator, each accepting exactly one stage at a
  time, with receipts and checkpoints recorded;
* a site that stops while the fleet moves on is **refused** progress until it
  returns, and is then reconciled against what it actually accepted;
* the coordinator is killed with `TerminateProcess` (a genuine hard kill, no
  graceful shutdown) and restarted on the same durable store: the epoch strictly
  increases, the recovered plan is fenced until re-affirmed, and the durable
  progress survives.

`dce_test_persistence` spawns the test binary itself as a crash writer,
waits for it to report that its records are durable, kills it, and re-opens the
store: every record whose append reported `durable == true` is present, and
recovery reports either a clean end or a torn tail — never interior corruption.

### Package and downstream proof

The library installs with CMake export, a namespaced target
`DataCenterEvolution::dce`, and a config usable through `find_package`. An
independent consumer configured from outside the repository against the
installed prefix — never the build tree — finds the package, links the installed
archive, runs, and prints its own checks. CI repeats this on both Linux and
Windows.

### Continuous integration

`.github/workflows/ci.yml` runs on every push. Every job below completed
successfully on the released commit:

| Job | Configuration | Result |
|---|---|---|
| `windows-msvc` (Debug, Release) | Ninja, MSVC, `/W4 /permissive- /WX` | passed |
| `ubuntu-gcc` (Debug, Release) | Ninja, gcc, `-Werror` | passed |
| `ubuntu-clang` (Debug, Release) | Ninja, clang, `-Werror` | passed |
| `ubuntu-sanitizers` | clang, Debug, AddressSanitizer + UndefinedBehaviorSanitizer, no suppression files | passed |
| `package-downstream` (ubuntu-gcc) | install, then configure, build and run the consumer from outside the repository, asserting it linked the installed archive | passed |
| `package-downstream` (windows-msvc) | the same, on the discovered MSVC toolset | passed |
| `gate` | the single required status check | passed |

Every leg runs the same fifteen suites, so the Linux persistence, process and
socket paths are exercised exactly as the Windows ones are. The Windows jobs
find the installed Visual Studio through `vswhere` and export its environment
rather than naming a generator, so no job depends on a particular Visual Studio
release being present in the runner image. No job sets `timeout-minutes` and no
test is wrapped in a timeout: a hang is a defect, and one was found and fixed
this way.

## Benchmarks

`dce_bench` measures the boundary's two kinds of cost separately and never
conflates them: pure decision cost, and the cost of a durable commit. Every
figure is measured during the run with `platform::monotonic_nanos()`; nothing is
projected, and no before/after comparison is published because there is no
baseline in this repository.

Measured on Windows with MinGW-w64 GCC 14.2.0, 16 hardware threads, synthetic
fleets from a fixed seed.

**Plan validation** (`validate_plan` against the fleet's own observations), Release:

| sites | components | steps | checked sites/edges | mean |
|---|---|---|---|---|
| 8 | 1 | 2 | 8 / 12 | 0.67 ms |
| 64 | 2 | 4 | 64 / 193 | 6.1 ms |
| 256 | 4 | 8 | 256 / 1522 | 82 ms |
| 512 | 8 | 16 | 512 / 6135 | 512 ms |

**Rollout decisions** (a full deterministic sweep), Release:

| sites | steps | decisions per sweep | per decision |
|---|---|---|---|
| 8 | 2 | 18 | 1.8 µs |
| 64 | 4 | 258 | 14 µs |
| 256 | 8 | 2052 | 77 µs |
| 512 | 16 | 8196 | 334 µs |

A rollout decision rescans the rollout state, so a sweep is quadratic in the
fleet size. The table shows that growth rather than hiding it, which is why the
sweep stops at 512 sites; a 4096-site sweep is roughly three orders of magnitude
more work and was **not run**.

**Durable stage commit** (`Store::append` of a real 172-byte migration-receipt
record), 512 appends each:

| mode | `sync_on_commit` | mean | min | median |
|---|---|---|---|---|
| durable | true | 4559 µs | 1541 µs | 2039 µs |
| non-durable | false | 1621 µs | 750 µs | 862 µs |

Only the durable row makes a durability claim: it is the one that includes
`platform::File::sync()`. The non-durable row is a cost comparison and is
explicitly **not** a durability claim. In the Debug build the same rows measured
2185 µs and 1067 µs.

**Checkpoint then recover**: after a snapshot over 384 records and 128 further
appends, 32 re-opens recovered the snapshot (5050 bytes) plus the 128
post-snapshot records, with a median recovery of 0.82 ms in Release and 2.17 ms
in Debug, and no torn tail.

Run it yourself:

`@
cmake -S . -B build/release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/release --target dce_bench
build/release/benchmarks/dce_bench
`@

## Adjacent boundaries

`@
facility runtimes  ->  complete sites  ->  federation  ->  DCCP evolution
   (identity,            (Site Control        (Data Center     (this boundary)
    topology, assets,     Plane)               Federation)
    capacity, power,
    cooling, policy,
    tenancy, ...)
`@

Independently governed facility runtimes compose into complete sites;
federation composes sites; evolution changes versions and capabilities without
collapsing authority boundaries. This boundary consumes explicit compatibility
contracts from ASI (accelerator execution, memory, serving, scheduling, state)
and DFI (network topology, paths, transport, congestion, failure, recovery,
federation) and never reaches into their internals. It records what it consumed,
including the provenance of the evidence, and it reports a dependency it has not
observed as indeterminate rather than guessing.

See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) for the full model and
[docs/PERSISTENCE.md](docs/PERSISTENCE.md) for the on-disk format.

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md). Contributions are accepted under the
Apache License 2.0; there is no CLA.

## License

Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.
