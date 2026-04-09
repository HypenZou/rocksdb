# Design: PeriodicCompactionChecker

## Overview

This document describes a small, callback-based enhancement to RocksDB's
existing `periodic_compaction_seconds` mechanism.

Today, periodic compaction is gated only by file age. This is useful because
it gives old SSTs another chance to go through compaction filters, but it is
coarse:

- files that are old but clean may still be rewritten;
- users cannot express file-level business logic such as "this old file has a
  high ratio of expired or stale data, so rewriting it is worth the write
  amplification."

The goal of this design is to keep the existing periodic compaction framework
and add one extra user-controlled eligibility check:

- `periodic_compaction_seconds` remains the first gate;
- a new file-level callback, `PeriodicCompactionChecker`, becomes the second
  gate;
- the same `periodic_compaction_seconds` interval is reused as the checker
  retry interval for files that remain live after a `false` decision.

This keeps the change small:

- no new compaction type;
- no new background thread;
- no changes to `CompactionFilter`;
- no requirement that RocksDB understand user value formats.

The callback is always invoked outside the DB mutex. The implementation keeps
temporary refs on the owning `ColumnFamilyData` and `Version` while the
callback is in flight, and revalidates the file by file number after
reacquiring the DB mutex.

## Problem Statement

Many users rely on `CompactionFilter` to discard expired or stale keys during
compaction rather than issuing explicit deletes. This avoids writing many
tombstones, but it has a structural limitation:

- a key can only be filtered when its SST participates in flush or compaction;
- bottommost or otherwise stable SSTs may not be compacted again for a long
  time.

`periodic_compaction_seconds` addresses this by periodically rewriting old
files. However, age alone is often an insufficient proxy for usefulness:

- some old files contain little reclaimable stale data;
- some old files contain a large amount of reclaimable stale data and are much
  better periodic compaction candidates.

Users already have a good way to encode file-level hints:

- `TablePropertiesCollector` can persist custom properties into SSTs;
- later reads of `TableProperties` can interpret these properties.

What is missing is a clean, small hook that lets users use these properties to
further refine periodic compaction eligibility.

## Goals

The design goals are:

- keep `periodic_compaction_seconds` as the top-level trigger;
- add a user callback for file-level eligibility decisions;
- guarantee the callback is not invoked while holding the DB mutex;
- reuse existing periodic compaction picker and execution paths;
- work naturally with `TablePropertiesCollector` and
  `UserCollectedProperties`;
- preserve current behavior when the new callback is not configured.

## Non-Goals

This design intentionally does not:

- introduce a new compaction reason or new compaction type;
- extend `CompactionFilter` with scheduling logic;
- make RocksDB parse application TTL or stale semantics;
- add a new daemon or dedicated SST scanning thread;
- persist checker results to SST or MANIFEST;
- replace `periodic_compaction_seconds` with a new framework.

## API Design

### New Public Types

Add a new public interface:

```cpp
class PeriodicCompactionChecker {
 public:
  struct Context {
    uint32_t column_family_id;
    int level;
    bool is_bottommost_level;
    uint64_t current_time;
    uint64_t periodic_compaction_seconds;
    uint64_t file_age;
    uint64_t file_number;
  };

  virtual ~PeriodicCompactionChecker() = default;

  virtual bool ShouldCompact(const Context& context,
                             const TableProperties& table_props) const = 0;

  virtual const char* Name() const = 0;
};

class PeriodicCompactionCheckerFactory : public Customizable {
 public:
  virtual ~PeriodicCompactionCheckerFactory() = default;

  virtual std::unique_ptr<PeriodicCompactionChecker>
  CreatePeriodicCompactionChecker() = 0;

  virtual const char* Name() const override = 0;
};
```

### New Option

Add a new column family option:

```cpp
std::shared_ptr<PeriodicCompactionCheckerFactory>
    periodic_compaction_checker_factory = nullptr;
```

Semantics:

- `nullptr`: current RocksDB behavior is preserved exactly;
- non-null: age-eligible periodic compaction candidates are additionally
  filtered by the checker.

There is no second timing knob. The single existing option
`periodic_compaction_seconds` controls both:

- when a file first becomes old enough to be considered for periodic
  compaction;
- how long RocksDB waits before rerunning the checker on a still-live file
  after a previous `false` decision.

## Relation to Existing Extensibility Points

### CompactionFilter

This design does not modify `CompactionFilter`.

Reason:

- `CompactionFilter` is a per-key transformation/removal hook;
- periodic compaction eligibility is a file-level scheduling decision;
- mixing the two would blur responsibilities and make scheduling dependent on
  an API that is only meaningful once compaction has already started.

### TablePropertiesCollector

This design is expected to compose with `TablePropertiesCollector`.

Typical user workflow:

1. `TablePropertiesCollector` writes custom per-file hints such as:
   - min/max expiration timestamp;
   - stale ratio estimate;
   - application-defined generation number.
2. `PeriodicCompactionChecker` reads these properties from
   `TableProperties::user_collected_properties`.
3. `ShouldCompact()` applies application-specific logic.

This keeps RocksDB generic while letting users express rich policies.

### TablePropertiesCollector::NeedCompact()

This design does not extend `NeedCompact()`.

Reason:

- `NeedCompact()` is evaluated during table creation;
- periodic compaction eligibility is inherently a later, time-dependent
  decision.

## High-Level Flow

Periodic compaction eligibility becomes a two-stage process.

### Stage 1: Age Eligibility

The existing periodic compaction logic still computes whether a file is old
enough.

Age-eligible means:

- `periodic_compaction_seconds > 0`;
- the file is not being compacted;
- the file age is greater than or equal to
  `periodic_compaction_seconds`.

This stage only uses cheap in-memory metadata and still runs while holding the
DB mutex.

### Stage 2: Checker Eligibility

If `periodic_compaction_checker_factory == nullptr`:

- all age-eligible files remain periodic compaction candidates.

If `periodic_compaction_checker_factory != nullptr`:

- age-eligible files must additionally pass `PeriodicCompactionChecker` before
  becoming periodic compaction candidates.

The checker runs outside the DB mutex and reads `TableProperties`.

## Locking and Execution Model

### Constraint

Current compaction score computation and compaction picking are generally done
while holding the DB mutex. Therefore the checker must not run inside:

- `VersionStorageInfo::ComputeCompactionScore()`;
- `VersionStorageInfo::ComputeFilesMarkedForPeriodicCompaction()`;
- `ColumnFamilyData::PickCompaction()`.

### Rule

`PeriodicCompactionChecker::ShouldCompact()` must always run:

- in a background compaction thread;
- outside the DB mutex.

### Refresh Workflow

The new logic is implemented as a background refresh pass before normal
`PickCompaction()` is attempted.

#### Step 1: Under DB mutex, collect check candidates

While holding the DB mutex:

- inspect the current `VersionStorageInfo`;
- identify age-eligible files that still need checker evaluation;
- skip files that:
  - are already being compacted;
  - already passed the checker;
  - are currently being checked by another background thread;
  - were previously rejected and have not yet waited another
    `periodic_compaction_seconds` interval.

For each selected file, capture a lightweight snapshot:

- referenced `ColumnFamilyData*`;
- current `Version*`;
- `FileMetaData*`;
- level;
- file number.

The implementation increments refs on both `ColumnFamilyData` and `Version`
before releasing the DB mutex. This guarantees:

- the checker callback can safely access the captured `Version`;
- `work->file` remains valid as part of that referenced `Version`;
- the file may become stale relative to the current DB state, but it does not
  become a dangling pointer.

Also mark the file as:

- `periodic_checker_in_progress = true`

so no other background thread checks it concurrently.

The temporary refs are owned by `PeriodicCompactionCheckerWork`, which releases
them via RAII in `Reset()` / its destructor.

#### Step 2: Outside DB mutex, load properties and invoke checker

After releasing the DB mutex:

- load the file's `TableProperties`;
- build the `PeriodicCompactionChecker::Context`;
- create a checker instance via the factory;
- invoke `ShouldCompact()`.

This is the only phase where user code runs.

The properties are loaded from the captured `Version` using the captured
`FileMetaData*`. Therefore the checker evaluates a consistent old-version view
even if the current DB state changes concurrently.

#### Step 3: Reacquire DB mutex and publish the result

After reacquiring the DB mutex:

- verify the file is still live in the current version by looking it up again
  using `file_number`;
- clear `periodic_checker_in_progress`;
- set:
  - `periodic_checker_passed = true/false`;
  - `last_periodic_checker_time = now`.

If the checker returned `true` and the file is still valid:

- recompute compaction score for the column family;
- the file can now enter the periodic compaction candidate set;
- the normal `PickCompaction()` path can subsequently choose it.

If the file became invalid while checked:

- discard the result silently;
- clear the in-progress state;
- do not mark the file as passed.

If the file is still live but is already `being_compacted` by the time the DB
mutex is reacquired:

- clear the in-progress state;
- do not publish `periodic_checker_passed` or update the check timestamp.

## Internal State Changes

### FileMetaData Runtime Fields

Add runtime-only fields to `FileMetaData`:

```cpp
bool periodic_checker_passed = false;
bool periodic_checker_in_progress = false;
uint64_t last_periodic_checker_time = 0;
```

These fields are:

- in-memory only;
- not stored in SST;
- not persisted to MANIFEST.

This is deliberate:

- the checker is an optimization to candidate selection;
- results can be recomputed after restart.

### VersionStorageInfo Sets

Logically distinguish:

- age-eligible files;
- final periodic compaction candidates.

The current implementation uses two vectors plus runtime file flags. The
behavior is:

- age eligibility is decided by existing periodic compaction age logic;
- final inclusion in `FilesMarkedForPeriodicCompaction()` additionally requires
  checker approval when a checker is configured.
- files that are age-eligible but still need checker evaluation are tracked in
  `FilesPendingPeriodicCompactionCheck()`.

## Detailed Changes in Periodic Candidate Computation

`VersionStorageInfo::ComputeFilesMarkedForPeriodicCompaction()` is reworked as
follows:

1. Clear the periodic candidate list.
2. For each file:
   - compute file age exactly as current periodic compaction does;
   - if the file is not age-eligible, skip it;
   - if no checker factory is configured, add it directly to the periodic
     candidate list;
   - if a checker factory is configured, only add it if
     `periodic_checker_passed == true`;
   - otherwise, if enough time has elapsed since the last rejected check,
     add it to the pending-check list.

This preserves existing behavior when the checker is not configured.

## Integration Point in Background Compaction

The checker refresh runs in background compaction before normal automatic
compaction picking.

Recommended placement:

- in `DBImpl::BackgroundCompaction()`;
- after enough context is available to identify the target column family;
- before calling `cfd->PickCompaction(...)`.

This placement is appropriate because:

- it runs on an existing background thread;
- it can perform property IO outside the DB mutex;
- it naturally precedes compaction candidate selection.

The refresh is bounded:

- the implementation checks at most one file per background compaction pass;
- this avoids turning the background compaction worker into a long-running
  property scanner.

## Failure Handling

### TableProperties Load Failure

If loading `TableProperties` fails:

- treat the checker evaluation as failed for that file;
- clear `periodic_checker_in_progress`;
- update `last_periodic_checker_time`;
- do not include the file in periodic compaction candidates;
- continue the background compaction flow.

### User Callback Failure

As with other RocksDB callbacks:

- exceptions must not escape user code;
- callback code is expected to be well-behaved and non-throwing.

The new API documentation must explicitly state this.

### Concurrent File State Changes

If the file is replaced, compacted, or otherwise invalidated while the checker
is running:

- the result is ignored when publishing;
- no error is reported.

More precisely, the implementation distinguishes:

- file no longer live in the current version:
  - drop the result;
- file still live but already being compacted:
  - clear in-progress only and do not publish the result.

## Performance Considerations

The design intentionally keeps expensive work off the DB mutex:

- loading `TableProperties` is done outside the lock;
- user checker logic is done outside the lock.

The remaining cost is:

- extra state bookkeeping;
- one extra bounded refresh phase in the background compaction thread.

To keep overhead predictable:

- only age-eligible files are checked;
- rejected files are retried only after another
  `periodic_compaction_seconds` interval;
- multiple background threads do not check the same file concurrently.

## Compatibility

This proposal is backward compatible:

- no SST format change;
- no MANIFEST format change;
- no behavior change when the checker is absent.

It is also naturally compatible with existing user infrastructure:

- users already using `TablePropertiesCollector` can reuse their file metadata;
- users not using collectors can still implement simple policies from standard
  `TableProperties`.

## Alternatives Considered

### Extending CompactionFilter

Rejected because:

- it conflates per-key filtering with file-level scheduling;
- it still does not naturally solve the "how do we decide to compact this SST"
  problem.

### Extending TablePropertiesCollector::NeedCompact()

Rejected because:

- it only runs at table creation time;
- periodic compaction eligibility is explicitly time-dependent and may change
  later.

### Introducing a New Stale Compaction Framework

Rejected for now because:

- it adds a new scheduling path;
- it is much more invasive;
- it is unnecessary for the narrower goal of reducing false positives in
  periodic compaction.

## Test Plan

### Unit Tests

Add tests covering:

- no checker configured:
  - periodic compaction behavior remains unchanged.
- checker configured and returns `false`:
  - age-eligible file does not become a periodic compaction candidate.
- checker configured and returns `true`:
  - age-eligible file becomes a periodic compaction candidate.
- concurrent refresh:
  - the same file is not checked twice at the same time.
- invalidation during checker execution:
  - file result is dropped safely.
- checker retry cadence:
  - reuses `periodic_compaction_seconds`;
  - a rejected live file is retried only after another full interval.

### Integration Tests

Add a test scenario where:

1. a custom `TablePropertiesCollector` writes user properties such as
   `user.stale_ratio`;
2. a `PeriodicCompactionChecker` reads this property;
3. only old files with `stale_ratio` above a threshold are selected for
   periodic compaction;
4. `CompactionFilter` then removes stale keys during actual compaction.

Also compare:

- with checker: only a subset of old files are rewritten;
- without checker: all old files remain eligible by age alone.

### Concurrency / Regression Tests

Add tests to ensure:

- the checker does not run under DB mutex;
- front-end write paths are not blocked by user checker execution;
- existing periodic compaction tests continue to pass unchanged when no checker
  is configured.
- lifetime and invalidation handling remain safe when the file disappears from
  the current version while the checker is running.

## Documentation Requirements

Public API documentation must clearly state:

- `PeriodicCompactionChecker` is only used for periodic compaction;
- `ShouldCompact()` is called outside the DB mutex;
- `ShouldCompact()` may be called concurrently on different files;
- callback code must not throw exceptions;
- callback code should not call RocksDB APIs that mutate DB state;
- callback code is expected to be lightweight even though it does not hold the
  DB mutex.

Internal implementation notes should also state:

- `PeriodicCompactionCheckerWork` owns the temporary `ColumnFamilyData` and
  `Version` refs via RAII;
- the checker evaluates a file against the captured `Version`, not necessarily
  the latest version;
- result publication must always revalidate the file by `file_number` in the
  current version before updating runtime flags.

## Summary

This design keeps RocksDB's periodic compaction model intact and adds a small,
generic extension point:

- age remains the first gate;
- user-defined file-level logic becomes the second gate;
- user code never runs under DB mutex;
- existing `TablePropertiesCollector` becomes the natural source of
  application-specific hints.

The result is a narrowly scoped, upstream-friendly improvement that gives users
precise control over periodic compaction usefulness without introducing a new
compaction subsystem.
