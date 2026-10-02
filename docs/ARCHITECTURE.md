# Architecture

Data Center Evolution (DCE) is the boundary of the Data Center Control Plane
(DCCP) that governs **live evolution** of a multi-site control plane: moving a
running fleet from one compatible system state to another across versions,
capabilities, sites, policy generations and lower-layer dependencies, without a
global shutdown.

## What this boundary owns

* evolution plans and their deterministic digest;
* compatibility staging: the capability matrix and the compatibility graph;
* rollout generations: cohorts, waves, canary subsets and per-site stage;
* gates and the evidence that satisfies them;
* migration receipts, checkpoints and the durable record of what was accepted;
* rollback eligibility and the point of no return;
* reconciliation of a reconnecting site against what it actually accepted;
* exception authority, bounded and explicit;
* evidence provenance: who asserted what, about which generation.

## What this boundary does not own

* the internals of any individual runtime's upgrade (it governs eligibility and
  proof, not mechanism);
* firmware rollout;
* facility-change execution;
* policy semantics (it consumes a policy generation and checks the requirement);
* federation membership;
* ASI or DFI evolution internals.

References to ASI, DFI or any other runtime are **explicit contracts**: an
identifier the neighbour published, a version range, a capability set, an
observed generation and the evidence that vouches for the observation. Nothing
about a neighbour is inferred from a version string. A dependency that was never
observed is reported as *indeterminate*, which is deliberately not the same as
compatible.

## Layering

`@
                     +-------------------------------+
                     |  tools/dce  (operator surface)|
                     +---------------+---------------+
                                     |
      +------------------------------+------------------------------+
      |                                                             |
+-----v-----------------+                                 +---------v---------+
| dce::CoordinatorServer|                                 | dce::SiteNode     |
|  transport shell only |                                 |  one site process |
+-----+-----------------+                                 +---------+---------+
      |                                                             |
+-----v-----------------+   wire::Message   +---------------------v---------+
| dce::Coordinator      |<----------------->| dce::SiteAgent (site_node)    |
| authoritative state   |                   | local accepted truth          |
+-----+-----------------+                   +---------------------+---------+
      |                                                             |
+-----v-------------------------------------------------------------v---------+
| dce::persist::Store  - record log, manifest, snapshots, epoch fencing       |
+-----------------------------------------------------------------------------+
      |
+-----v-----------------------------------------------------------------------+
| dce::platform  - files, durable sync, locks, child processes, loopback sockets|
+-----------------------------------------------------------------------------+
`@

Deterministic core, with no I/O and no clock: ids, version, capability,
compatibility, evidence, plan, lifecycle, rollout, authority, reconcile,
validate, codec, digest, random.

The deterministic core never includes `dce/platform.hpp`. That is what makes the
whole model testable without a filesystem, a network or a clock, and it is why
rollout decisions are reproducible byte for byte.

## The model

### Identity and generation

Every authoritative object has a stable, validated identity (`BasicId`). The
accepted spelling is deliberately narrow because identities are also used as
file names: no path separators, no `..`, no Windows reserved device names, no
control bytes, bounded length.

Counters are distinct types, never bare integers:

| Type | Meaning |
|---|---|
| `PlanRevision` | how many times a plan identity has been re-authored |
| `PlanGeneration` | the monotonic authority generation of a plan |
| `CoordinatorEpoch` | increments every time a coordinator opens its store for writing |
| `SiteGeneration` | the generation a site accepted a stage at |
| `StageOrdinal` | how many migration steps a site has durably accepted |
| `CohortWave` | the position of a cohort in the rollout |
| `LogSequence` | the durable record sequence |

`CoordinatorEpoch` is the fencing mechanism. Opening a store for writing advances
it, so every token, receipt and plan revision issued under a previous epoch is
fenced the moment a new coordinator takes over. A stale writer cannot act, and a
stale plan cannot silently continue.

### Stage semantics

A site's `StageOrdinal` is the number of migration steps it has durably
accepted. `0` means nothing accepted; `stage_count(plan)` means fully evolved.
The step that moves a site from ordinal *n* to *n+1* is `plan.steps[n]` in
canonical step order. A site may only ever advance by exactly one stage, which
is how "sites cannot skip gates" is enforced structurally rather than by
convention.

### Compatibility

Two structures, both persisted and both digested:

* **Capability matrix** - what each `(component, version)` provides and
  requires. Interoperability between two versions is decided by checking *both*
  directions of the requirement. An undescribed version yields `unknown`, never
  `interoperable`.
* **Compatibility graph** - nodes are `(component, version)`, edges are
  evidence-backed transitions carrying the capabilities that must already be
  present, the capabilities that appear afterwards, reversibility, and a
  provenance identity. An edge without provenance is a claim, not a permission.

Graph analysis reports self loops, cycles between distinct versions of one
component, orphan nodes, unsupported downgrades, edges with no provenance and
capability regressions. Path search is a deterministic breadth-first walk in
canonical edge order that honours capability preconditions, so an impossible
plan is refused with the capability that was missing, the version it was needed
at, and the frontier that was actually reachable. That is what makes a refusal
explainable instead of a bare "no".

### Lifecycle

`draft -> validated -> staged -> rolling_out -> validating -> completed`, with
`paused`, `blocked`, `reconciled`, `rolling_back`, `rolled_back`, `aborted` and
`failed` reachable where they are legal. `legal_transitions()` is the single
source of truth: documentation, tests and the runtime all read the same table.
A transition that is not in the table is refused, and a guard that does not hold
is refused with the guard named.

Rollback is available only before the recorded irreversible boundary. `abort`
is refused once the point of no return has been crossed, because a fleet that
has passed it cannot be abandoned; it can only complete or fail.

### Authority and fencing

A site is sovereign by default. It acts on evolution only when the plan
explicitly delegates rollout authority to it **and** it presents an authority
token current for the coordinator's epoch and the plan's generation. The verdict
vocabulary is explicit: `valid`, `stale_epoch`, `fenced_generation`,
`plan_mismatch`, `digest_mismatch`, `unknown_site`, `not_delegated`.

Migration receipts are idempotent by idempotency key. Replaying the same effect
is a `duplicate` that changes nothing; the same key carrying different content
is a `conflicting` that is refused. A receipt whose stage does not correspond to
its step, or whose versions do not match the step, is refused rather than
recorded.

### Partition and reconciliation

A partitioned site keeps serving locally: local sovereignty is not suspended by
losing the coordinator. What it loses is the right to advance. When it
reconnects it reports what it actually accepted, and the coordinator moves its
own belief to match reality **before** any new authority is issued.

| Reported | Coordinator action |
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

### Validation

`validate_plan` is the authoritative answer to "may this plan proceed?". It is
evidence-driven and refuses with a typed, explainable reason. The checks, in
order:

1. structural soundness, then the attached digest must match the content;
2. fencing: the plan's epoch must be the coordinator's, and its generation must
   not be behind the authoritative one;
3. the fleet the plan was written against must still be the fleet that exists
   (site set, versions, per-component versions, accepted generations, state
   digests, and the source state digest itself);
4. the target must be ahead of the source;
5. every observed component version must be described by the capability matrix;
6. every evolved component must converge on exactly one destination, and every
   site must have an evidenced, capability-complete route to it;
7. every blocking gate must be satisfied by evidence that is present and
   current, and an unevaluated gate is never treated as passing;
8. the policy generation requirement;
9. every stated ASI/DFI dependency must have been observed explicitly;
10. capability removal and deprecation require a proof that no site and no
    obligation still depends on the capability;
11. cohorts must be complete, contiguous and cover every site exactly once;
12. irreversibility must be justified, and a rollback policy that can never be
    used is an inconsistency rather than a safety margin;
13. mixed-version interoperability across the whole observed fleet, with
    `split_brain` reported when two observed versions cannot interoperate and no
    step in the plan moves either of them.

### Evidence currency

An evidence record carries the coordinator epoch it was observed in. Zero means
the record is **not epoch-scoped**, which is the honest description of a static
property of a version pair such as a compatibility certification. An
epoch-scoped record that predates the plan revision it accompanies is stale; a
record from an epoch the coordinator has not reached is indeterminate.

## Concurrency and lock order

The rule, stated once and enforced everywhere:

`@
state_mutex_   protects the authoritative in-memory model
store          has its own internal lock and is only ever entered while
               state_mutex_ is held, never the other way round
sockets        are never written to while state_mutex_ is held
`@

A worker decodes a request, takes `state_mutex_`, mutates and commits durably,
releases it, and only then writes the response. No callback is invoked under the
state lock, no lock is held across a blocking socket operation, and no callback
can re-enter mutable state. Shutdown closes the listener, joins the accept
thread, closes client sockets, joins the workers, and only then closes the
store, so no worker is ever joined while it still needs state the shutdown path
holds.

## Bounded everything

Every collection that can arrive from a peer, from disk or from an operator is
bounded in `dce/limits.hpp` and the bound is enforced **before** memory is
reserved for it. A hostile length prefix cannot become a large allocation, and
an over-long collection is refused rather than truncated.
