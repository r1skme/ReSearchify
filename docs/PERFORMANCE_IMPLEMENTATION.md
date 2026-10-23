# Performance implementation and review map

This document maps the implemented changes to their correctness constraints and
retained evidence. It describes the optimization branch relative to original
commit `eb858348169157686e70104af6b64f9539a39fe8`. Individual measurement reports
identify their own exact commits, executables, corpora and sampling windows;
their speedup ratios must not be multiplied.

The integrated Linux executable after the NFA follow-up has SHA-256
`701c8a34f6578e212336e80628bdd711df6413d6c8f1e0edf7d8b58f8a4b1293`.
Its source was built at local commit
`228220743f205c370964fcb4475c8daee1f4b3b7`. Subsequent measurement-tool and report
changes are separate from that executable. Native CI records actual PR merge
checkout commits as well as the branch head; those are different commit IDs
and their relevant source trees must be checked before comparing them.

## Implemented work

| Area | Change | Correctness or resource constraint |
|---|---|---|
| Compact literal filtering | Optional bounded document signatures reject impossible four-byte windows before reading content. Safe enclosed-token conditions also use existing postings. | Filters are necessary conditions only. Short, saturated, unsupported or missing signatures fall back to verification. Small files below the existing 512-byte admission threshold keep the scan path. |
| Fast literal and regex filters | Bounded rare-anchor selection and streaming intersections; same-segment posting membership can reject a raw candidate before expensive catalog lookup. | No materialized corpus-sized candidate set. Accepted candidates still pass active-record identity, posting bounds and content checks. Old and new document versions cannot satisfy opposite halves of an intersection. |
| Full-coverage anchors | Content-only anchor drivers and trees that provably cover every live file can use the ordered catalog directly. | Exact coverage is used only with the conservative current-generation segment proof. Frequent but selective intersections and stale counts keep their previous strategy. |
| Content I/O | Query/worker-local bounded LRU of seekable segment readers; lines wholly inside an I/O block are consumed as views. | No concurrent shared `seekg`. Snapshots remain pinned, handles are bounded and errors are propagated. CRC, fingerprint, UTF-8, line-size and byte-count checks remain active. |
| Parallel content search | Bounded adaptive workers handle sufficiently large eligible non-regex content queries; small requests stay sequential. | Admission accounts for worker memory. Candidate/byte budgets are global to the request, reduction is ordered, and counts, scores, tie-breaking, snippets and pagination remain deterministic. |
| Posting and Boolean evaluation | Rare-first AND traversal, fewer repeated exact-term lookups and IDF computations, bounded posting membership cursors. | Query truth, live-document checks, cancellation and ranking contracts are preserved. |
| Regex execution | Mandatory-prefix seeking, compiled ASCII class masks, reuse of verified UTF-8, and rejection of impossible fresh initial ASCII transitions. | Assertions, nullable patterns, non-ASCII scalars and EOF retain conservative paths. The latest initial-transition shortcut preserves its reference executor's exact work charges and exhausted-budget behavior. |
| CRC-32 | Portable slicing-by-eight implementation of the existing IEEE CRC-32. | Polynomial, initialization, chaining and final checksums are unchanged. x86 SSE4.2 CRC-32C is not substituted for IEEE CRC-32. No new ISA requirement is introduced. |
| Indexing and storage | Less repeated key/allocation work, bounded worker queues and reusable catalog lookup state; fast-profile trigram scratch is allocated lazily. | The core remains C++20 with standard-library dependencies. Existing commit/recovery paths, file validation and immutable-reader snapshots remain in force. |
| Known-path updates | Worker count is capped by supplied paths; zero/one-file work avoids idle pools and unused fast-profile scratch. Checked catalog cursors are reused per worker. | `changed_files` is an explicit scope, not automatic change discovery. Full strict scans still detect same-size, same-timestamp changes outside a supplied list. |
| Measurement and CI | Opt-in stage counters, isolated target CPU/RSS sampling, paired comparisons, real-source capture, full-result oracles and native platform jobs. | Diagnostic profiling is excluded from latency distributions. Failed/incomplete runs and observed regressions are retained. Cross-compilation alone is not recorded as a native test. |

## Update scopes are intentionally distinct

For an editor or agent that knows exactly what it changed:

```sh
rs update --changed-file src/main.cpp --strict
rs update --changed-file src/old.cpp --changed-file src/new.cpp --strict
```

The second example supplies both names of a rename. A complete known-path list
can contain edits, additions and deletions. The JSON response reports
`global_scan:false`; unrelated paths have not been checked. If changes outside
the list are possible, run a full strict update:

```sh
rs update --strict
```

The targeted interface existed before this work. The branch improves its
execution and documents its scope; it does not claim to have invented a new
filesystem notification service. Comparing a full 100,000-file strict scan
with one supplied path is a workload comparison, not an algorithm speedup.
The retained update report compares old/new binaries within each scope.

## Reading the evidence

| Report | What it establishes |
|---|---|
| [Initial integrated optimization](PERFORMANCE_OPTIMIZATION.md) | Original-baseline comparisons, index sizes, profiling, integrity and compatibility checks. |
| [Literal follow-up](PERFORMANCE_LITERAL_FOLLOWUP.md) | Content scheduling/locality and the costs of selective versus broad scans. |
| [Earlier regex follow-up](PERFORMANCE_REGEX_FOLLOWUP.md) | Verified UTF-8 reuse and its focused correctness and performance controls. |
| [Tiny-file planner](PERFORMANCE_TINY_PLANNER.md) | Full-coverage and raw-membership changes, rare-term regressions, 350 MiB and million-file guards. |
| [Update follow-up](PERFORMANCE_UPDATE_FOLLOWUP.md) | Paired strict/known-path updates, memory tradeoffs, source mutation audits and invalidated experiments. |
| [NFA initial-transition follow-up](PERFORMANCE_NFA_FOLLOWUP.md) | Accepted real-source gains, exact-budget differential checks, and a rejected first implementation. |
| [Native checkpoint measurements](PERFORMANCE_NATIVE.md) | Earlier actual macOS ARM64/Intel measurement windows and their limitations. |
| [Scoped cache benchmark method](CACHE_BENCHMARK.md) | Per-file Linux page-residency observation, cache-advice scope and conservative labels. |

Raw evidence is stored under `bench/results/`. Commands, fixed seeds, binary
hashes, profiles and corpus identities accompany the reports. Index build
timing is separate from search timing. A single diagnostic build is explicitly
descriptive, not a stable p95 or a paired throughput claim.

## Measurement contracts

Fresh CLI timing includes the native process startup and shutdown. It does not
by itself establish a cold OS page cache. Resident JSONL measurements separate
engine time from client request wall time; 1,000 resident requests and 1,000
fresh CLI processes are different experiments.

Literal/term occurrence counts and regex matching-line counts are different
contracts. The comparison tools select the corresponding ripgrep count mode.
Full correctness runs compare the actual path/count maps, check pagination
progress and ordering where applicable, and compare returned source snippets
and byte columns. Same-snapshot ranking is compared between ReSearchify
versions; ripgrep has no equivalent ReSearchify score.

Timing uses count-only ReSearchify responses and complete ripgrep per-file
count scans. Output volume differs: ripgrep serializes per-file counts while
ReSearchify reports the total and internal work metrics. Exact commands and
this limitation are retained; these are not claims of identical serialization
cost or universal engine superiority. Correctness output is checked separately
outside the timed samples. Full-result validation can use a larger memory
budget than the timed 256 MiB search and is labeled accordingly.

CPU time is target-process time, not Python orchestration time. POSIX high-water
RSS may include a small inherited pre-exec Python floor; values at that floor
are not precise ReSearchify allocations. The algorithmic memory setting is not
a hard operating-system RSS limit. Reported RSS observations, requested memory
and index storage sizes are kept separate.

Seven- and eleven-sample nearest-rank p95 values equal the maximum observed
sample. They are useful short controls, not robust estimates of a tail-latency
distribution. Paired orders and all observed outliers remain in the raw data.

## Deliberate limits

- Compact tiny-file edge literals can still require a corpus scan. The fast
  profile trades a larger index for more selective posting filters.
- Wide regex retains serial execution under its finite global work budget and
  can remain substantially slower than ripgrep, despite the measured gains.
- Full strict updates still inspect the complete source set. Known-path input
  is efficient when the caller can supply a complete change list.
- The index format remains compatible with the tested readers. Optional
  signature storage has a measured space cost; it is not a free speedup.
- ISA-specific search kernels were not added merely to satisfy an optimization
  checklist. The portable implementation and its fallback behavior remain the
  supported path.
- Snapshot publication and recovery checks do not turn standard stream flushes
  into a new guarantee of power-loss durability.

The branch preserves both successful and rejected experiments. The rejected
callback-template change, the first NFA shortcut, the raw-only anchored-regex
regression and residual rare-term differences are documented in their reports.
No aggregate claim that every query became faster is supported by this work.
