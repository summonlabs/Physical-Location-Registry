# Physical Location Registry

Physical Location Registry is the Data Center Control Plane (DCCP) Tranche 1
repository that owns **stable physical addressing**: what places exist in a
facility, where each one sits in the address hierarchy, which generation of each
place is current, and how a place moves, is renamed, retires or is replaced.

It is a portable C++20 library with a small inspection tool. It has no
third-party dependencies: the C++ standard library plus the operating-system
interfaces needed for durable publication and writer fencing.

## Systems boundary

This repository owns:

* stable location identities, independent of names and addresses;
* the containment hierarchy of facilities, buildings, halls, rooms, rows, racks,
  cages, rack units and zones, and the canonical address derived from it;
* the lifecycle of a location (active, retired, replaced) with a legal
  transition table;
* address changes under generation control: reparenting a node (moving a
  location), readdressing it, and relabeling it;
* replacement lineage: a location ceases to exist at an address and a successor
  with a new identity takes that address over;
* alias addresses that keep older references resolvable, with conflict and
  look-alike detection;
* provenance, move history and generation diffs;
* durable, integrity-checked, atomically published store state with conservative
  recovery, plus writer fencing between processes.

This repository deliberately does **not** implement, and does not store:

* rack occupancy, mounting, asset inventory or asset identity — a location is an
  addressable place, not the asset occupying it;
* facility topology edges (power feeds, cooling loops, cabling, network paths);
  those belong to the topology and dependency repositories;
* placement planning, scheduling or capacity reservations;
* geospatial mapping, coordinates in the physical world, or latitude/longitude;
* electrical actuation, cooling control, maintenance orchestration, tenancy
  policy, incident response or observability dashboards;
* multi-site federation.

Adjacent systems are expected to reference locations through the typed
`LocationId`, `LocationGeneration` and `LocationPath` values defined here rather
than by copying address strings. Zones in this registry are **containment nodes**
of the address hierarchy, not overlapping membership sets; overlapping groupings
belong to a failure-domain or topology repository.

## Addressing model

### Location kinds and the containment schema

| Kind | May be a root | Legal parents | Legal children |
| --- | --- | --- | --- |
| `facility` | yes | — | building, hall, room, cage, zone |
| `building` | no | facility | hall, room, cage, zone |
| `hall` | no | facility, building | room, row, cage, zone |
| `room` | no | facility, building, hall | row, rack, cage, zone |
| `row` | no | hall, room, zone | rack, cage |
| `rack` | no | room, row, cage, zone | rack-unit |
| `cage` | no | facility, building, hall, room, row, zone | rack |
| `rack-unit` | no | rack | — |
| `zone` | no | facility, building, hall, room | row, rack, cage |

A `facility` is the only parentless kind, and every other kind must be contained.
The schema is finite and acyclic, so a canonical address has **at most eight
components** (`facility / building / hall / room / zone / row / rack / rack-unit`
is the longest legal chain). `Limits::max_depth` can lower that bound; it cannot
raise it.

A `rack` may carry an *envelope*: the rack-unit extent it offers (for example
`1-48U`). A `rack-unit` carries a *coordinate*, which must lie inside its rack's
envelope, and no two rack units under one rack may claim the same coordinate.

### Canonical addresses

A location's canonical address is derived, never stored:

```
/FAC1/BLDG-A/ROOM-101/ROW-03/RACK-07/U12
```

* components are joined with a single `/` and the whole address is absolute;
* address component grammar: 1–64 bytes, first and last byte ASCII
  alphanumeric, interior bytes `[A-Za-z0-9._-]`. There is no way to express
  `/`, `\`, `:`, whitespace, control characters, or a bare `.`/`..`, so an
  address can never be mistaken for a filesystem path and persistence never
  derives a file name from one;
* sibling components must be unique byte-for-byte **and** must not be ASCII
  case-insensitive look-alikes, so `RACK-07` and `rack-07` cannot coexist as
  siblings and create a human-ambiguous address space;
* components are compared byte for byte. No Unicode normalization is performed
  anywhere: two labels that differ only by normalization form are different
  labels, and this is deliberate rather than accidental.

### Identity, labels and addresses are separate

* `LocationId` is the stable identity. It never changes, it is never recycled,
  and it is the only thing a consumer should persist.
* `label` is human display text (valid UTF-8, no control characters or
  noncharacters, at most 256 bytes). Changing it is a `relabel`: it does **not**
  change the address, and it therefore does not restamp descendants.
* the **address component** is the addressable token that appears in the
  canonical path. Changing it is a `readdress`: the node and every descendant
  get a new canonical address and their generations advance.

### Iteration order is defined

Children are enumerated in ascending byte order of their address component.
Listings are ordered by canonical address text, also byte-wise. Canonical
serialization orders locations by `LocationId` bytes. These orders are identical
on every platform and in every locale; nothing depends on hash iteration order.

### Aliases

An alias is an additional absolute address bound to a location. Aliases exist so
that a reference written against an older addressing scheme keeps resolving to
the same place. The rules are strict, because an ambiguous alias is worse than no
alias:

* an alias may not equal any location's current canonical address (including the
  owner's own, which is reported as redundant);
* an alias may not collide with another alias, exactly or modulo ASCII case;
* an alias may not be assigned to a location that is retired or replaced;
* a mutation may never create an address that an alias already claims — the
  conflict is reported instead.

## Identity, generation, revision and authority

Four independent, strongly typed counters exist, and none of them converts
implicitly into another:

| Type | Scope | Meaning |
| --- | --- | --- |
| `LocationGeneration` | one location | version of that location's authoritative record; starts at 1 and advances on every accepted mutation that touches it, including one caused by a mutation of an ancestor |
| `LocationRevision` | one store | monotonic revision of committed state; every committed mutation advances it by exactly one |
| `StateSequence` | one store | monotonic counter of *publications*, which also advance for authority-only publications |
| `WriterEpoch` | one store | durable writer-incarnation counter, advanced and published by every session that opens the store for writing |

A mutation may carry three optional preconditions:

* `expected_generation` — rejected with `STALE_GENERATION` when the target moved on;
* `expected_revision` — rejected with `STALE_REVISION` when the committed state moved on;
* `authority` (store identity plus writer epoch) — rejected with
  `STALE_AUTHORITY_EPOCH` or `STORE_MISMATCH` when the session no longer owns the
  store.

A rejected mutation publishes nothing: the committed revision, the published
generation, the state bytes and the head pointer are all unchanged, which the
test suite asserts directly.

### Idempotency

A mutation may carry an `operation_id`. If that operation was already applied,
the stored receipt is returned with `replayed = true` and nothing is published;
if the same operation id arrives with a different request, it is rejected with
`OPERATION_ID_CONFLICT`. Receipts are part of the durable state, so a retry after
a process restart still replays instead of applying twice. The receipt ring is
bounded by `Limits::max_operation_receipts`; the oldest receipts are evicted in
order, and the bound is documented rather than silent.

## Lifecycle

| From | Legal verbs | Result |
| --- | --- | --- |
| `active` | `retire` | `retired` — not resolvable as current |
| `active` | `replace` | `replaced` — terminal, successor created |
| `retired` | `reactivate` | `active` again |
| `retired` | `replace` | `replaced` — terminal |
| `replaced` | — | terminal; no mutation of any kind is accepted |

Enforced consequences:

* an active location may not sit under a non-active parent, so retiring is
  bottom-up (retire the subtree, or retire the descendants first) and
  reactivating is top-down;
* a retired location keeps its address in the hierarchy (so it can be
  reactivated) but does not resolve as current. `resolve(..., CurrentOnly)`
  refuses it; `resolve(..., IncludeRetired)` returns it with its lifecycle;
* a replaced location keeps its place in the hierarchy for lineage but leaves
  the address space entirely: its successor holds the address, and the
  predecessor never resolves, not even with `IncludeRetired`;
* a retired location may be relabeled (the label is descriptive metadata) but not
  moved or readdressed (the address space is authority);
* replacing a location requires that it has no children, that its successor
  identity is unused, and that the successor inherits its coordinate and
  envelope. Replacement lineage is acyclic by construction and re-verified when
  stored state is loaded.

## Moves, move history and diffs

A *move* is a reparenting. Its identity, label and component are preserved; the
node and every addressable descendant get a new canonical address and advance
their generation. A *readdress* is the same mechanics with the component
changed. Both are atomic: the whole subtree is validated first, applied as one
change set, and published in one durable publication, or nothing happens.

The moved node records one `MoveRecord` (kind, revision, generation before and
after, previous and new parent and component, previous and new canonical path,
the number of descendants that followed, actor, instant, reason). Descendants do
not each get a record: their generations advance, which is what makes a stale
cached address detectable. Total move history is bounded per location and per
store.

Generation diffs are available two ways:

* `Registry::diff(from, to)` uses the bounded in-memory revision journal of a
  live session;
* `diff_snapshots(a, b)` compares any two snapshots, including ones decoded from
  published state files, which is how durable diffs across processes and
  restarts are produced (`plr-cli diff --from-file/--to-file`, or
  `plr-cli diff --from-revision N` over the generations the store still retains).

## Persistence and recovery

A store is one directory:

```
store.lock          writer lock file (advisory operating-system lock)
store.id            durable store identity anchor, written once at creation
head                current publication pointer, replaced atomically
state.<sequence>.plr immutable canonical state publications
```

The identity anchor is not authoritative state; the state carries the identity it
was written with. It exists so that recovery never has to guess which store a
publication belongs to: a renamed, stray or foreign publication cannot be
adopted, and a directory whose files were swapped with another store's files is
refused instead of being silently accepted under a new identity.

Publication is a protocol, not an append:

1. plan and validate the change against the committed state;
2. reserve the next state sequence;
3. write the new generation to a temporary file and flush it;
4. rename it to its permanent name;
5. read it back and verify its SHA-256 integrity trailer (unless explicitly
   disabled when the store is opened);
6. **commit** by atomically replacing `head` — this single directory operation is
   the commit point;
7. retire superseded publications beyond `max_publications_retained`.

A crash before step 6 leaves the previous generation authoritative; a crash after
it leaves the new one authoritative, because the new file was durable and
verified before the pointer moved. Both outcomes are exercised by killing real
child processes at each stage.

A canonical publication is a deterministic binary encoding: a fixed header,
a payload whose field order is fixed, and a SHA-256 trailer over everything
preceding it. Two stores built from the same sequence of requests with the same
store identity produce byte-identical state. Decoding validates framing, format
version, flags, declared lengths, every enumeration, every identity, every text
grammar, every bound and the integrity digest **before** trusting anything, and
then re-validates the whole structure: containment legality, acyclicity,
lifecycle consistency, alias uniqueness, replacement symmetry and revision
monotonicity.

Recovery is conservative:

* `head` is read first. If it is valid, the state it names must decode, pass its
  digest, and agree with the head's own metadata; otherwise the head is treated
  as unusable.
* Only then is the directory scanned: the newest publication that decodes and
  verifies end to end is adopted, and the recovery report records exactly what
  happened, including how many candidates were skipped.
* If nothing verifies, the store refuses to open
  (`RECOVERY_UNAVAILABLE`) rather than presenting unverified bytes as
  authoritative.
* A writer session heals an unusable head in the same publication that advances
  the writer epoch.

Readers take no lock and never block writers; they always see a complete
published generation because publication is an atomic pointer replacement.
Readers never observe a partially applied mutation.

## Deterministic rejection semantics

Every rejection is a stable `ErrorCode` with a category, a human message and,
where one exists, the subject (identity, address or component). Codes are
appended to, never renumbered. Examples:

| Situation | Code | Category |
| --- | --- | --- |
| component already used by a sibling | `ADDRESS_IN_USE` | structure |
| sibling differs only by ASCII case | `ADDRESS_LOOK_ALIKE` | structure |
| kind cannot be contained by that parent | `INVALID_KIND_FOR_PARENT` | structure |
| parent is retired or replaced | `PARENT_NOT_ACTIVE` | structure |
| moved into its own subtree | `MOVE_INTO_DESCENDANT` | structure |
| illegal lifecycle transition | `LIFECYCLE_TRANSITION_ILLEGAL` | lifecycle |
| stale `expected_generation` | `STALE_GENERATION` | authority |
| stale `expected_revision` | `STALE_REVISION` | authority |
| superseded writer epoch | `STALE_AUTHORITY_EPOCH` | authority |
| operation id reused for a different request | `OPERATION_ID_CONFLICT` | authority |
| a configured bound was exceeded | `LIMIT_EXCEEDED` | limit |
| integrity digest mismatch | `DIGEST_MISMATCH` | argument |
| store owned by another writer | `STORE_LOCKED` | persistence |
| cancelled before publication | `CANCELLED` | cancelled |

`explain_resolve`, `explain_create` and `explain_move` run the same structural
rules without mutating anything and return a machine-readable code plus a human
summary and hints, so a caller can ask "would this be accepted, and why not?"
before committing to it. Explanations report the structural verdict; generation,
revision and authority preconditions are evaluated when the mutation is
submitted.

## Concurrency model

The model is explicit and small:

* one `Registry` instance may be used from many threads;
* readers take a shared lock and see exactly one committed revision;
* a mutation takes the exclusive lock, validates, applies, publishes durably and
  only then reports success;
* no callback is ever invoked, no background thread is created, and no lock is
  ever held across a call back into the registry, so re-entrancy and
  lock-upgrade hazards are structurally absent;
* the durable writer lock is acquired at open — before the in-memory lock is ever
  needed — and released at close, so file lock and memory lock are never nested
  in both orders;
* two writer sessions cannot own the same store directory, in this process or any
  other: the second open is rejected with `STORE_LOCKED`;
* closing is a clean shutdown: it stops new work, waits for in-flight mutations
  to finish (they hold the lock), commits only what already crossed the
  publication boundary, releases the writer lock, and leaves the accounting
  exact — the on-disk revision always equals the number of mutations that
  reported success;
* cancellation is cooperative through `std::stop_token` and is checked before
  publication. A cancelled mutation publishes nothing. Once durable publication
  has started it runs to completion and reports success; there is no outcome in
  which a mutation reports failure after committing.

## Limits and resource bounds

`Limits` bounds locations, depth, children per location, component/label/reason/
source text, canonical path length, aliases per location and in total, move
records per location and in total, replacement records, operation receipts,
retained revisions, retained publications, traversal nodes and total state bytes.
Limits are fixed when a store is created, persisted inside the state, validated
against compile-time hard ceilings when that state is read back, and a mismatch
between requested and persisted limits is rejected. Exceeding a bound is a
deterministic `LIMIT_EXCEEDED` rejection; nothing is ever silently truncated.

## API

```cpp
#include <dccp/physical_location_registry/physical_location_registry.hpp>

using namespace dccp::physical_location_registry;

RegistryOpenOptions options;
options.mode = OpenMode::ReadWrite;
options.create_if_missing = true;

auto registry = Registry::create("/var/lib/dccp/plr", options).value();

MutationContext context;
context.actor = ActorId::parse("planner").value();
context.at    = Timestamp::parse_text("2026-01-01T00:00:00Z").value();
context.authority = registry->authority();   // writer fencing

auto request = CreateLocationRequest::make(
    "loc-rack-07", LocationKind::Rack, "loc-row-03", "RACK-07", "Rack 7", context).value();
request.envelope = RackEnvelope::with_height(48).value();
auto receipt = registry->create_location(request);

auto path = registry->path_of(LocationId::parse("loc-rack-07").value());
auto resolved = registry->resolve(LocationPath::parse("/FAC1/ROOM-101/ROW-03/RACK-07",
                                                      registry->limits()).value());
auto snapshot = registry->copy_snapshot();   // immutable, lock-free queries
```

Public headers: `result.hpp` (codes, categories, `Error`, `Result`),
`strong_id.hpp` (identities and counters), `text.hpp` (text grammars),
`kind.hpp`, `address.hpp`, `rack_unit.hpp`, `lifecycle.hpp`, `provenance.hpp`,
`limits.hpp`, `location.hpp` (immutable views), `snapshot.hpp` (a complete
immutable state plus canonical encode/decode), `requests.hpp` (typed requests),
`registry.hpp` (session and mutations), `store.hpp` (durable store),
`diff.hpp`, `explanation.hpp`, `digest.hpp`, `version.hpp`.

### Authority surface

* `Registry::authority()` returns the token a caller stamps on mutations.
* `Registry::copy_snapshot()` returns a `std::shared_ptr<const Snapshot>`: a
  detached value whose queries never take a lock and never change.
* Mutations return `MutationReceipt` (revision, sequence, generation,
  successor generation, affected counts, replayed flag).
* Nothing in the public API exposes the persistence layout or mutable records;
  snapshots are immutable and the store directory is an implementation detail
  documented above for operators, not an interface.

## Inspection tool

```
plr-cli [--store DIR] [--format text|json] <command> [options]

read-only:  status, verify, roots, list, show, path, resolve, children, tree,
            aliases, diff, snapshot, explain-resolve, explain-move, recover
mutation:   init, create, readdress, relabel, move, retire, reactivate, replace,
            alias-add, alias-remove, set-rack

mutation options: --actor A [--at TIME] [--expected-generation N]
                  [--expected-revision N] [--op-id ID] [--reason TEXT]
```

The tool has no privileged path into the store: mutations go through the same
public API, including authority, generation, revision and lifecycle checks that a
library consumer faces. Options are validated per command, so a misspelled option
is a usage error rather than something silently ignored. Exit codes are stable:

| Code | Meaning |
| --- | --- |
| 0 | success |
| 1 | internal error |
| 2 | usage error (malformed argument) |
| 3 | store unavailable (missing, locked, closed, read-only, I/O) |
| 4 | integrity failure (digest, corruption, unsupported format) |
| 5 | operation rejected (structure, lifecycle, authority, limit, cancelled) |

Example session:

```
$ plr-cli --store /srv/plr init
$ plr-cli --store /srv/plr create --id loc-fac --kind facility --component FAC1 --actor ops
$ plr-cli --store /srv/plr create --id loc-rack --kind rack --parent loc-row \
      --component RACK-07 --u-height 48 --actor ops
$ plr-cli --store /srv/plr tree
$ plr-cli --store /srv/plr resolve --path /FAC1/ROOM-101/ROW-03/RACK-07
$ plr-cli --store /srv/plr explain-move --id loc-rack --to-parent loc-row-04
```

Each CLI invocation opens its own writer session for mutations, which advances the
durable writer epoch: a mutation command costs one publication for the epoch and
one for the change. A long-lived library consumer pays the epoch publication once
per session instead.

`plr-store-agent` is a second, deliberately small process helper used for
cross-process proof and diagnosis: `hold-lock` (hold the writer lock until a
release file appears), `crash-hold` (die while holding the lock), `write` and
`read` (mutate or inspect from another process), `crash-publish` (die at a named
publication stage), `verify`.

## Build, test, install

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
cmake --install build --prefix /opt/plr
```

CMake options: `PLR_BUILD_TESTS`, `PLR_BUILD_TOOLS`, `PLR_BUILD_EXAMPLES`,
`PLR_BUILD_BENCHMARKS`, `PLR_WARNINGS_AS_ERRORS` (default `ON`),
`PLR_SANITIZERS` (AddressSanitizer, plus UndefinedBehaviorSanitizer on GCC and
Clang), `PLR_ANALYZE` (MSVC static analyzer), `BUILD_SHARED_LIBS`.

The installed package provides `SummonSoftwareLabs::PhysicalLocationRegistry`
with generated config and version files. An independent consumer outside the
source tree:

```cmake
find_package(PhysicalLocationRegistry CONFIG REQUIRED)
target_link_libraries(app PRIVATE SummonSoftwareLabs::PhysicalLocationRegistry)
```

`tests/package_consumer` is exactly such a project; it builds and runs against an
installed tree, exercising create, resolve, generation fencing, reopen and
verification.

## Validation performed

Environment: Windows 11 Pro 26200, AMD Ryzen 7 9800X3D (8 cores / 16 threads),
61.6 GB RAM, MSVC 19.44.35207 (Visual Studio 2022 Build Tools), Ninja, CMake
4.3.2. Store directories were placed in the system temporary directory on a
local filesystem. Every row below was run in this environment.

| Check | Result |
| --- | --- |
| Release build (`/W4 /WX`): library, 2 tools, 7 examples, 1 benchmark, 11 test binaries | zero warnings, zero errors |
| Debug build (`/W4 /WX`), same targets | zero warnings, zero errors |
| Release test suite (11 binaries, 108 test cases) | 11/11 passed, no skips |
| Debug test suite | 11/11 passed |
| AddressSanitizer build and suite (`PLR_SANITIZERS=ON`) | 11/11 passed, no sanitizer report |
| Multi-process suite | real child processes: exclusive writer lock, reader isolation under a held lock, OS lock release after an abrupt exit, epoch fencing across incarnations, and a crash at each of the six publication stages with both permitted outcomes verified |
| Crash injection | writers killed (`_Exit`, exit code 70) before and after the commit point; the reopened store verified and matched the expected revision in every case |
| Property suites | 4 seeds × 120 valid operations and 4 seeds × 120 rejected operations per run, checked against an independent reference model |
| Corruption suite | byte-flip at header, payload and trailer, truncation at every prefix, oversized input, 200 random byte strings, hostile head files, hostile components and labels |
| Adversarial suite | store path that is a file, directories and junk files in the store, renamed publications, contradictory payloads with a *valid* digest (self-parent cycle, duplicate identities, inflated counts), swapped store identity, replaced lock file, command-line typos |
| Install + downstream consumer | `cmake --install` to a prefix, then `tests/package_consumer` configured, built and run from outside the source tree; it printed `consumer ok` |
| Examples | all seven example programs compiled and ran to completion with exit code 0 |
| Fresh clone | build, test, install and downstream consumer run from a clean clone of the committed sources |

## Benchmarks

`plr-benchmarks` measures completed operations. Mutations include their durable
publication (write, flush, read-back verification, atomic head switch), and reads
measure the finished answer. Numbers below are a Release build on the machine
described above, seed 20260101, shape `rooms=10 rows/room=10 racks/row=10
units/rack=8`, that is **9,112 locations** and a **1.9 MB** published state.
Reproduce with:

```
plr-benchmarks --rooms 10 --rows 10 --racks 10 --units 8 --mutations 200 --resolves 20000
```

| Operation (completed work) | Count | Per operation |
| --- | --- | --- |
| create location, durably committed | 9,111 | 20.5 ms |
| resolve by identity (`find`) | 20,000 | 0.79 µs |
| resolve by canonical address | 20,000 | 1.19 µs |
| subtree traversal | 1,831,311 nodes visited | 0.72 µs per node |
| full listing in canonical order | 191,352 locations returned | 1.85 µs per location |
| relabel, durably committed | 200 | 27.8 ms |
| subtree readdress, durably committed | 8 changes, 728 locations restamped | 28.1 ms |
| encode + decode + validate state | 5 round trips of 1.9 MB | 35.0 ms (≈56 MB/s) |
| open + verify published state | 1 | 70.4 ms |

The benchmark reported `locations=9112 (expected 9112)` and that the reopened
state verified against its digest and matched the revision it was closed at, so
the measured runs are integrity-checked results rather than timing of unverified
work.

Read in context: a mutation rewrites, flushes and re-verifies the *whole*
published state, so per-mutation cost grows with store size — this is the direct
price of whole-state atomic publication and is the dominant term above. Reads are
index-bound and stay sub-microsecond per identity at this scale. These are
single-machine, single-flush results; they are not a claim about accelerator,
network or facility hardware, none of which is involved.

## Limitations

* **Validated on Windows/MSVC only.** The POSIX code paths for durable writes,
  advisory locking and directory flushing are implemented and reviewed, but no
  POSIX toolchain was available on this machine, so they are unexercised here and
  must be validated before deployment on Linux.
* **One writer per store directory.** Serialization between writers is
  coarse-grained by design: no two writer sessions may own a store at once, in
  one process or across processes.
* **One store holds its own state.** This repository has no federation, gossip or
  replication; multi-site composition is a later DCCP concern.
* **The revision journal is bounded and in-memory.** `Registry::diff` compares
  revisions retained by a live session. Durable comparison uses published state
  files with `diff_snapshots`, and only generations the store still retains are
  available.
* **Labels are not normalized.** No Unicode normalization or case folding is
  applied to labels; comparison is byte-exact. Address components are restricted
  to an ASCII grammar precisely so that this cannot create ambiguity, but two
  labels that a human would read as the same may be distinct.
* **Zones are containment nodes**, not overlapping sets of locations.
* **No hardware integration is claimed or exercised.** The store is a local
  directory; no PDU, UPS, cooling controller, switch, accelerator or network
  fabric was involved in any validation.
* **Timestamps are caller-supplied.** The library never reads the system clock,
  so ordering of history is only as good as the instants its callers provide.

## License

Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.
