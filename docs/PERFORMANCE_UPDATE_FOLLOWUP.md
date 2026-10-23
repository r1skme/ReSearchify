# Strict and known-path update follow-up

Measured on 23 October 2026 on Linux x86-64, GCC 13.3 Release (`-O3 -DNDEBUG`).
The source fixture contains 100,000 generated C++ files, 800,000 lines and
19,034,450 bytes. It exercises tiny documents; it is **not** the user's Windows
100,000-file project and does not establish Windows or macOS latency.

## Implemented changes

- Each update worker reuses checked, worker-local path and record cursors for
  the immutable catalog shards. The two namespaces have separate cursors.
  Decoded pages remain in the shared `PageCache`; cursors hold weak page owners
  and retain no decoded entries after cache eviction. The per-worker budget
  also includes cursor slots and conservative expired weak-owner overhead.
  Full document ID, path identity, bucket and segment checks remain active.
- A supplied known-path list caps the worker count to the number of input
  paths. A zero/one-path request performs any queued file job on the caller's
  thread. Delete/excluded/empty-list requests avoid constructing idle workers.
- Fast-profile workers allocate the 2 MiB trigram bitmap only when capturing
  their first changed or added document. Strict no-op workers do not allocate
  it, and a one-file update no longer allocates it in 15 idle workers.
- Opt-in build profiling now reports `metadata_lookup_seconds` separately from
  source validation and final catalog publication. Profiling remains disabled
  in all latency samples below.

The strict default still reads and fingerprints the complete included source
set. The validation and capture passes, CRC-32 and fingerprint algorithms,
crash-safe commit path and immutable snapshots are unchanged. Existing
`--changed-file` / JSON `changed_files` inputs now have more explicit CLI/API
instructions; this is an existing operation whose execution was improved.

## Work scope and methodology

Before is commit `349175274bb338fc5e39ca4ec417c296cc4f868e`, binary SHA-256
`c0046d12c64100b636bed378d8796a3d1c058b9ed692d3f8c8915adf6c80a4dd`.
After is the implementation in `f765f71dea037cce4ecd07680af52e66eff71e78`,
binary SHA-256
`3522d3b11f1b1971a05f9600b6a9726c3199410152fd9a11c62ec1cfb7dd79f1`.
The runtime code was unchanged between its successful tests and measurement.

Both binaries begin each profile with separate copies of the same immutable
index written by the before binary. Setup normalizes the persisted policy with
an excluded strict no-op scan. Every row has seven deterministically shuffled
AB/BA pairs, requested threads 16, memory budget 256 MiB. A fresh CLI process is
measured from launch through exit. Copying, mutations, fsync of owned fixture
files, correctness queries, verification and restoration are outside timing.
The OS cache is uncontrolled and warmed by setup; no cold-cache result is
claimed. No other local agent ran target workloads during these measurements.

The operations below are compared **within the same scope**. Full strict scans
hash all 100,000 files; known-path edits/additions hash one file, and known-path
deletions hash none. Comparing a full scan with a known-path update would not be
an algorithm speedup measurement. Missing notifications require a full strict
scan. The targeted path does not promise constant latency: metadata, snapshot
and commit costs remain.

Generations and retained on-disk commits grow across the identical change
sequence in both tools. These are real consecutive updates, not independent
reset snapshots. Source edits preserve the original modification timestamp;
after the first edit, subsequent marker replacements also preserve file size.
Full strict scans still detect every changed marker. The driver restores the
original source bytes and timestamp in `finally` and fails on unexpected source
contents. An independent parent process verified the original SHA after each
profile and again after the complete run.

## Fresh CLI latency

Times are milliseconds. The p95 is the maximum of seven samples, not a precise
tail-latency estimate. Full distributions and paired ratios are in the raw JSON.

| Profile / operation | Before median | After median | Median change | Before p95 | After p95 | After faster pairs |
|---|---:|---:|---:|---:|---:|---:|
| compact / Full strict no-op | 1174.730 | 1111.620 | -5.4% | 1320.111 | 1238.705 | 4/7 |
| compact / Full strict scan, one edit | 1110.573 | 1103.669 | -0.6% | 1200.384 | 1218.913 | 5/7 |
| compact / Known-path edit | 6.677 | 5.850 | -12.4% | 7.436 | 7.964 | 5/7 |
| compact / Known-path add | 7.669 | 6.388 | -16.7% | 8.605 | 6.882 | 7/7 |
| compact / Known-path delete | 7.008 | 5.859 | -16.4% | 7.699 | 6.361 | 7/7 |
| fast / Full strict no-op | 1109.138 | 1067.335 | -3.8% | 1141.482 | 1156.414 | 4/7 |
| fast / Full strict scan, one edit | 1104.651 | 1044.383 | -5.5% | 1192.301 | 1126.781 | 7/7 |
| fast / Known-path edit | 11.173 | 7.249 | -35.1% | 12.594 | 9.382 | 7/7 |
| fast / Known-path add | 10.699 | 6.713 | -37.3% | 12.599 | 6.966 | 7/7 |
| fast / Known-path delete | 11.373 | 5.411 | -52.4% | 15.334 | 5.858 | 7/7 |

Fast targeted changes improve in all seven pairs for each operation. Compact
add/delete also improve in all seven pairs; the compact one-file edit improves
in five pairs and its p95 increases from 7.436 to 7.964 ms. The full strict
compact one-file median is effectively flat. Global strict results retain
scheduler/IO variation; their modest gains do not justify claims of a large
full-scan acceleration. Fast no-op p95 increases from 1141.482 to 1156.414 ms.

## CPU and memory

CPU medians are milliseconds; RSS values are MiB. RSS is a process high-water
mark obtained through an isolated child. On this POSIX setup it may include a
Python pre-exec floor, observed as exactly 15 MiB on small processes. Values at
that floor are upper-bound observations, not precise ReSearchify allocations.

| Profile / operation | Before CPU median | After CPU median | Before RSS median | After RSS median | Before RSS p95 | After RSS p95 |
|---|---:|---:|---:|---:|---:|---:|
| compact / Full strict no-op | 5114.874 | 4806.768 | 47.18 | 48.35 | 47.33 | 48.48 |
| compact / Full strict scan, one edit | 4983.232 | 4745.874 | 47.06 | 48.46 | 47.35 | 48.52 |
| compact / Known-path edit | 7.480 | 5.664 | 15.00 | 15.00 | 15.00 | 15.00 |
| compact / Known-path add | 8.442 | 6.220 | 15.00 | 15.00 | 15.00 | 15.00 |
| compact / Known-path delete | 8.097 | 5.679 | 15.00 | 15.00 | 15.00 | 15.00 |
| fast / Full strict no-op | 4960.902 | 4631.606 | 79.33 | 48.36 | 79.38 | 48.66 |
| fast / Full strict scan, one edit | 4955.767 | 4521.954 | 79.22 | 50.48 | 79.33 | 50.69 |
| fast / Known-path edit | 22.258 | 7.087 | 30.39 | 15.00 | 32.56 | 15.00 |
| fast / Known-path add | 21.353 | 6.544 | 23.02 | 15.00 | 30.16 | 15.00 |
| fast / Known-path delete | 23.863 | 5.248 | 27.16 | 15.00 | 28.06 | 15.00 |

Fast strict no-op RSS drops by about 31 MiB because idle workers no longer create
trigram bitmaps. Compact full-update RSS rises by about 1.2–1.4 MiB as worker
catalog cursors are initialized. Their bounded worst-case allowance is included
in worker admission and sorter budgets; it is not an unaccounted page cache.
The reported `memory_mb` remains an algorithmic workspace budget rather than a
hard OS RSS cap, as in the existing implementation.

## Index size and primary-build diagnostics

These are **single instrumented builds**, not paired performance claims. Source
bytes and the on-disk format are unchanged. Concurrent file assignment and
segment packing make independently built index byte sizes slightly different;
no size reduction is claimed.

| Profile | Before committed index bytes | After committed index bytes | Before diagnostic build, s | After diagnostic build, s |
|---|---:|---:|---:|---:|
| compact | 45,373,530 | 45,407,238 | 1.933 | 1.684 |
| fast | 132,053,135 | 132,041,255 | 3.340 | 3.259 |

The opt-in profile identifies work still present in a full scan. In the after
compact strict no-op, the worker sums are 3.816 s for metadata lookup and 4.417 s
for source validation, while end-to-end time is 1.132 s. They overlap across
16 workers and must not be added to wall time. Fast no-op gives 3.810 s metadata
lookup and 4.272 s validation, with 1.062 s wall time. Enumeration includes
bounded-queue backpressure, so it is not an isolated filesystem-walk timer.

Fast primary indexing still spends substantial aggregate worker time in posting
serialization: 14.617 s in the after diagnostic build, with 3.259 s wall time.
This change makes no claim to eliminate that cost or to accelerate primary
indexing by the ratio of these single observations. Profiling perturbs work and
is recorded separately from the seven-pair update measurements.

## Correctness and resource checks

The accepted two-profile run completed 140 timed updates and 92 additional
correctness records, with 98 source mutation audit events. Checks include exact
paths, occurrence counts, scores, snippet positions/text and the absence of
previous markers. Added files appear, deleted files disappear, and a rename
supplied with both old and new names preserves results. Both final indexes pass
full `verify` for all 100,000 files. Both source restorations pass independent
SHA checks. The original fixture identity is
`d99f3d069ea6e58538a16f3080656292ac29bc4f325162ea5576ffa255e3d2dd`.

The GCC Release suite passed all 13 CTest suites. Additional indexing tests
passed 8 cases and 1,824 assertions, covering caller-thread cancellation with
no commit/lock leak, a pinned old snapshot, complete strict detection of an
unreported same-size/timestamp edit, additions/deletions/exclusions/UTF-8 rename,
empty known-path lists, and a valid-CRC catalog with a false document identity.
Small-memory tests exercise compact known-path updates at 16 MiB, compact full
scans at 24 MiB, fast known-path updates at 24 MiB and fast full scans at 32 MiB,
with a 16 KiB line limit. They correctly reduce the requested 16 workers to one.

No new native Windows/macOS/ARM performance result is claimed by this report.
No ripgrep comparison appears here because updating a persistent index is not
the same operation as a ripgrep source search.

## Reproduction and evidence

Use `scripts/update_benchmark.py` with two trusted local executables, an immutable
base index and an explicitly marked disposable fixture. The fixture manifest
must live outside its source tree and contain this contract:

```json
{
  "format": "researchify-update-fixture-v1",
  "source": "/absolute/disposable/source",
  "edit_file": "group_0/module_42.cpp",
  "edit_sha256": "SHA256_OF_ORIGINAL_EDIT_FILE",
  "files": 100000,
  "description": "Owned benchmark fixture with recorded provenance"
}
```

```sh
python3 scripts/update_benchmark.py \
  --baseline /absolute/before/rs --candidate /absolute/after/rs \
  --base-index /absolute/immutable/index \
  --fixture /absolute/update-fixture.json \
  --work /absolute/new-results-directory --iterations 7 --threads 16
```

Repeat with compact and fast base indexes. The driver refuses to overwrite an
existing result directory or an unexpected source edit. Use an isolated local
fixture; never point it at an actively edited user checkout. It records raw
commands, target output, distributions, binary SHA-256 identities, source audit
and all completion checks.

Accepted raw evidence:

- [Compact paired updates](../bench/results/optimization-update-compact.json).
- [Fast paired updates](../bench/results/optimization-update-fast.json).
- [Isolated runner provenance and separate build profiles](../bench/results/optimization-update-profiles.json).

### Invalidated workspace runs

Two earlier runs on the shared workspace fixture are deliberately excluded.
After the first reported restoration, a new invocation observed the older
`g_0006` marker. The instrumented retry later observed its `t_0005` source
replaced by `g_0006` on a different inode before its correctness query; the
semantic and restoration guards stopped the run. No actor or platform cause
was established. No timing from either run is used above.

The fixture was restored only after checking byte-for-byte that the unexpected
contents were the known original plus that known marker. The accepted rerun
created a new `/tmp` fixture, rebuilt its own immutable baseline indexes with
the correct source root, ran both profiles sequentially, and independently
verified restoration. The parent separately confirmed the shared fixture's
original SHA before later read-only search measurements.

[Both invalidated raw runs and reasons](../bench/results/optimization-update-invalidated.json)
are preserved for audit rather than silently discarded.
