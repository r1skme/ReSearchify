# Cross-platform performance implementation

Date: 2026-10-23. Baseline: `eb858348169157686e70104af6b64f9539a39fe8`.

**Measurement scope:** the original whole-product tables below are frozen at the
locally measured executable SHA-256
`86a12b278f1414c81beba973cb67387c34d09b91842f78de5a02af7758c05b4a`;
the native CI results refer to checkpoint
`dbcddf76708b614c26e212fa27b48efa33ddfd58`. They are not new measurements of the
final source after the later macOS-directed follow-ups. Those changes and their
separate Linux measurements are recorded in [literal-filter lookup and serial
setup](PERFORMANCE_LITERAL_FOLLOWUP.md) and [verified-content regex
execution](PERFORMANCE_REGEX_FOLLOWUP.md). Earlier intermediate experiments keep
their own recorded binary identities. Do not multiply speedup ratios from these
independent measurement windows. Native macOS paired measurements and validation
of the combined follow-up head are pending; the older green matrix does not
establish their outcome.

This report describes implemented code, measured tradeoffs and remaining limits. It is not a claim that ReSearchify beats ripgrep on every query. Historical v1/v2 figures in [BENCHMARKS.md](BENCHMARKS.md) describe different revisions and must not be mixed with these results.

## What changed

- **Compact literal filtering:** an optional checked per-document signature skips content that cannot contain a required literal. It covers every four-byte window in normalized lines, including substrings inside identifiers. Missing or saturated signatures fall back to exact verification. It is not a full trigram index.
- **Candidate planning:** safe fully enclosed token anchors, bounded adaptive streaming intersections, conservative Boolean/regex necessary conditions and rare-first choices. Edge tokens and approximate NOT conditions are not used to discard valid matches.
- **Content I/O:** bounded segment stream reuse, tracked offsets, block-backed line views, per-worker readers and bounded segment/offset-local dispatch instead of shared mutable seek positions. Snapshot pinning and all content identity checks remain enabled.
- **Parallel content scans:** a lazy, bounded worker pool for suitable non-regex scans. Candidate/read budgets are charged globally, while result reduction remains ordered and exact. Small, term-only and regex searches stay serial.
- **Regex execution:** mandatory straight-line literal-prefix seeking and ASCII character-class masks before/inside the existing Thompson NFA. Assertions, Unicode behavior and work-budget checks remain authoritative.
- **Checksum:** portable slicing-by-eight IEEE CRC-32 with identical seeds and output. This is not the incompatible x86 SSE4.2 CRC-32C instruction.
- **Ranking and metadata:** query-level IDF setup, reduced path/content-ID allocations for discarded hits, borrowed catalog iteration and active-segment exact-leaf lookup. Counts, scores and keyset pagination remain exact.
- **Index construction:** targeted producer/consumer notifications, reused aggregation storage, less repeated suffix work, bounded signature sorting and reusable compaction readers.
- **Instrumentation:** opt-in search/build stage counters; fresh-process, warm JSONL and update/find measurement tools; CPU/wall/RSS distributions and binary/index identity checks; native CI with rg smoke comparisons.
- **Later filter/serial follow-up:** a query-local weak cursor reuses checked literal-filter leaves without retaining decoded pages across calls; serial-only queries avoid unused CPU-count discovery. The separate [follow-up report](PERFORMANCE_LITERAL_FOLLOWUP.md) records both gains and remaining tiny-query regressions.
- **Later regex follow-up:** content callbacks reuse `stream_text`'s UTF-8 validation while public `Regex::find`, integrity checks and exact regex-budget charges remain strict. Its [separate report](PERFORMANCE_REGEX_FOLLOWUP.md) measures the additional improvement from the frozen optimized build, not from the original baseline.

The core stays C++20 and standard-library-only. No new runtime dependency, mandatory SIMD instruction or mandatory disk-format migration was introduced. See [ARCHITECTURE.md](ARCHITECTURE.md), [FORMAT_STORAGE.md](FORMAT_STORAGE.md) and [AGENT_API.md](AGENT_API.md) for contracts and field names.

## Measurement conditions

The full generated corpus has **100,000 files, 5,000,000 lines and 366,645,610 source bytes**. The host was Linux x86-64, GCC 13.3 Release, AMD EPYC 9V74, with an eight-CPU cgroup quota and approximately 9.7 GiB available RAM. Index construction used eight workers and a 256 MiB accounted workspace. ripgrep was 15.2.0.

Search comparisons use **21 paired repetitions**, a deterministic shuffled baseline/current/rg order and one excluded warmup per tool. Each sample starts a fresh target process. OS cache was uncontrolled/warmed, **not explicitly cold**. The timed search has `limit=0`, exact totals and no snippets; the rg comparator performs complete occurrence scans, not `-l` early termination. Aggregate JSON and rg's per-file count output are different projections. These figures do not measure ranked-result rendering or snippet generation.

Index construction is excluded from search timing and reported separately. Raw reports include exact commands, immutable binary hashes, individual samples, min/max, p95, CPU time, RSS and engine counters. Correctness checks beyond matching-file totals run separately. An independently changed source tree is not a valid comparator for a pinned index snapshot.

## Compact profile: full-corpus process measurements

Wall times in milliseconds; speedup is the ratio of the two medians, not the median of paired ratios.

| Query | Baseline median | Current median | Current p95 | Baseline/current | rg median |
| --- | ---: | ---: | ---: | ---: | ---: |
| Exact term `needle_42` | 5.494 | 5.629 | 6.543 | 0.98x | 109.551 |
| Literal `needle_42 = 0;` | 2,995.938 | 315.523 | 391.772 | 9.50x | 110.337 |
| Absent literal | 2,774.590 | 251.243 | 298.788 | 11.04x | 106.882 |
| Frequent term `common` | 212.792 | 180.892 | 227.793 | 1.18x | 511.458 |
| Frequent literal `return common;` | 2,699.407 | 659.323 | 1,061.018 | 4.09x | 510.805 |
| Broad regex `/needle_[0-9]+/` | 11,793.074 | 2,631.644 | 2,774.911 | 4.48x | 367.874 |

All samples returned the same matching-file totals. The literal case still examines 100,000 metadata candidates, but content reads fall from **366,645,610 to 54,884 bytes**. The missing-literal case also has 100,000 metadata candidates and reads zero content bytes. A separate final-binary profiled literal pass rejected 99,985 documents and retained 15 content checks for one real hit. This is a reduction in content verification, **not** a claim that the candidate counter fell to one.

The remaining selective-compact cost includes catalog/signature work. ripgrep remains about **2.86x faster** for the literal and **7.15x faster** for the broad regex in this process-level run. Exact indexed terms win against rg here, but that advantage excludes index creation and maintenance.

Broad-literal median CPU time changed from 2,698.824 to 1,933.019 ms while wall time fell much more: the latency improvement combines reduced CPU work with parallel execution, not a fourfold reduction in CPU work. Broad-regex CPU fell from 11,791.727 to 2,631.103 ms while remaining serial. The rare-term process median is 0.135 ms slower (+2.4%); this startup-scale row is not an improvement claim.

The broad-regex scenario also appears in the retained historical v1/v2 logs (15,870.89 versus 19,852.14 ms). This work measures the current baseline against the new implementation on the same corpus and host, rather than dividing today's timing by those older single-run values. It does not claim a new controlled rerun of the original v1 binary.

The largest observed current RSS in these compact query samples was **79,167,488 bytes (75.50 MiB)**. The old broad-content path used approximately 56.6 MiB: faster filtering and worker workspaces are not free memory improvements. Small-target RSS near the helper's inherited pre-exec floor is not executable-only RSS.

Final frozen-binary raw data: [optimization-compact-release.json](../bench/results/optimization-compact-release.json). The earlier [optimization-compact.json](../bench/results/optimization-compact.json) is retained as a checkpoint, not the source of the final table.

An excluded final profiled broad-literal pass used eight workers, 100,000 content reads and only 30 content opens. The rare literal stayed serial with eight opens. These diagnostics are in [optimization-diagnostic-compact-release.json](../bench/results/optimization-diagnostic-compact-release.json); they are not samples in the latency distributions, and summed worker-stage durations are not additive wall time.

## Fast profile

The same 100k corpus and 21 paired fresh-process samples, with separate fast indexes built by each writer. Times are milliseconds:

| Query | Baseline median | Current median | Current p95 | Baseline/current | rg median |
| --- | ---: | ---: | ---: | ---: | ---: |
| Exact term `needle_42` | 5.356 | 5.244 | 7.016 | 1.02x | 111.096 |
| Literal `needle_42 = 0;` | 97.362 | 56.558 | 63.416 | 1.72x | 99.858 |
| Absent literal | 5.846 | 5.558 | 6.848 | 1.05x | 102.012 |
| Frequent term `common` | 189.313 | 168.433 | 192.647 | 1.12x | 625.433 |

The rare literal decreases from 1,000 to **112 candidates**, and from 3,666,561 to **410,529 content bytes**; all matching-file counts remain identical. Its p95 improves from 117.445 to 63.416 ms and median CPU from 97.072 to 56.040 ms. Unlike compact, fast beats rg on this literal in this experiment, with the separate index-build/storage costs described below. Tiny-term median is slightly lower but p95 rises from 6.340 to 7.016 ms; startup-level changes are not robust universal wins.

The excluded final [fast profile diagnostic](../bench/results/optimization-diagnostic-fast-release.json) used one worker and 16 content opens for the 112 verified candidates. Fewer candidates do not automatically justify starting parallel workers.

An earlier optimization checkpoint regressed this literal from 95.431 to 125.351 ms despite reducing candidates. That regression led to active-segment exact-leaf lookup and adaptive handling of pure AND anchors; the final paired table above supersedes it, rather than deleting it from the evidence. Raw data: [final fast](../bench/results/optimization-fast-release.json), [retained regression](../bench/results/optimization-fast-regression.json).

## Persistent sessions

Both readers use the **same immutable current-writer index**, with two resident JSONL processes but only one request executing at a time. AB/BA pair order is balanced and alternating. The first request and three additional warmups are recorded separately, not mixed into the distributions. Term rows have 201 pairs, literal rows 21. These are client write/flush-to-response latencies in milliseconds, including transport but excluding client JSON parsing:

| Profile / query | Baseline median | Current median | Baseline p95 | Current p95 |
| --- | ---: | ---: | ---: | ---: |
| compact / `needle_42` | 0.12646 | 0.15086 | 0.20272 | 0.26315 |
| fast / `needle_42` | 0.21299 | 0.23136 | 0.47548 | 0.50445 |
| compact / literal `needle_42 = 0;` | 2,764.180 | 272.820 | 2,896.337 | 342.750 |
| fast / literal `needle_42 = 0;` | 32.496 | 5.646 | 42.778 | 7.656 |

Literal medians improve **10.13x compact / 5.76x fast**. Tiny term requests regress by **24.4 microseconds compact (+19.3%) / 18.4 microseconds fast (+8.6%)**. Internal unprofiled search medians also increase, from 46.13 to 54.86 microseconds compact and 60.43 to 70.15 microseconds fast; this is not dismissed as pipe overhead alone. The absolute increase is small, but it matters when interpreting repeated agent requests. There is no claim that every query improved.

All count-only responses agree. This harness does not establish full ranked-result/snippet equivalence, CPU time or per-reader RSS; those are tested or measured separately. Reuse here is **find-only**, not the cost of mutation plus `Service` invalidation/reopening. Earlier sequential sessions did not isolate host drift and are not used as the final comparison.

Raw first-request, warmup, ordering and measured-response records: [compact](../bench/results/optimization-jsonl-compact-release.json), [fast](../bench/results/optimization-jsonl-fast-release.json), [small integration check](../bench/results/optimization-jsonl-integration-release.json).

## Boolean planning: avoid trading one regression for another

Active-segment membership made selective fast anchors cheaper, but repeatedly probing non-overlapping AND lists created a separate regression. A bounded, amortized **global** next-posting hint was added after sustained misses. A segment-local next ID is not used as a global skip: another live segment can still contain a match. Cancellation is polled inside the skip/probe loops.

An isolated 20,000-file compact fixture measured 11 interleaved repetitions per variant on one immutable index. These are development variants with identical non-planner changes, **not** the unmodified historical baseline. Median fresh-process milliseconds:

| AND shape | Generic streaming planner | Active-segment membership | Membership plus global hint |
| --- | ---: | ---: | ---: |
| Disjoint ranges, zero matches | 9.701 | 25.001 | 10.606 |
| Interleaved disjoint IDs | 51.444 | 52.185 | 51.160 |
| Selective intersection | 45.916 | 44.156 | 41.553 |
| Dense intersection | 24.965 | 25.326 | 25.005 |

The hint removes most of the 25 ms disjoint-range regression without adding an unconditional full-list intersection to every candidate. The generic planner remains 0.905 ms faster on that isolated shape; no universal per-shape win is claimed. A separate 21-pair fast-literal check changed 64.866 to 64.004 ms when enabling the hint. Raw distributions and hashes: [AND shapes](../bench/results/optimization-and-ranges.json), [fast-literal guard](../bench/results/optimization-fast-anchor-c.json). Exact full-result and resource-budget comparisons are listed under correctness below.

## Real-source workload

A separate immutable capture of the baseline's `src/include/tests/docs` contains **29 actual files, 425,562 bytes and 8,746 lines**. It is not generated benchmark text. Its SHA-256 manifest is embedded in [optimization-real-source-release.json](../bench/results/optimization-real-source-release.json). The earlier [optimization-real-source.json](../bench/results/optimization-real-source.json) is retained as a pre-final checkpoint.

All 20 same-index full-hit comparisons passed: five queries, two profiles and both index writers. Ordered paths, counts, scores and content IDs agreed between readers; both readers also verified all four retained index combinations. Independent Python scanning and rg agreed on every matching path/count. `E_[A-Z_]+` used matching-line counts (`rg --count`), rather than occurrence counts, to match RS semantics.

Twenty-one shuffled paired fresh-process repetitions, two excluded warmups; median / p95 milliseconds:

| Profile | Query | Baseline | Current | rg |
| --- | --- | ---: | ---: | ---: |
| compact | Term `return` | 3.635 / 4.247 | 3.874 / 5.020 | 5.266 / 6.545 |
| fast | Term `return` | 3.580 / 5.468 | 3.639 / 5.152 | 4.734 / 6.231 |
| compact | Literal `std::string` | 6.985 / 20.593 | 6.222 / 8.267 | 5.124 / 6.967 |
| fast | Literal `std::string` | 7.915 / 10.106 | 6.943 / 8.160 | 4.863 / 6.543 |
| compact | Literal `std::` | 6.735 / 7.509 | 5.904 / 7.038 | 5.224 / 6.162 |
| fast | Literal `std::` | 7.184 / 8.247 | 6.157 / 7.840 | 4.771 / 6.482 |
| compact | Absent literal | 7.292 / 9.440 | 5.710 / 7.632 | 4.939 / 7.179 |
| fast | Absent literal | 5.069 / 7.427 | 4.831 / 8.261 | 4.817 / 9.248 |
| compact | Regex `E_[A-Z_]+` | 16.443 / 18.900 | 6.999 / 8.719 | 4.952 / 5.822 |
| fast | Regex `E_[A-Z_]+` | 16.422 / 22.016 | 6.314 / 7.167 | 4.694 / 6.366 |

Regex latency improves approximately 2.35x/2.60x with unchanged 425,562 content bytes and 29 candidates. rg still wins the median literal/regex cases. Compact `return` is 0.239 ms slower (+6.6%), fast `return` 0.059 ms slower (+1.6%); the compact-term and fast absent-literal p95 tails also worsen. Tiny-process startup/scheduling variance is large, so these rows are not a universal improvement claim. Query RSS hits the inherited Python floor of 15,204,352 bytes and establishes no target-memory reduction.

Three separate builds per writer/profile gave compact median 10.336 to 9.900 ms, and fast 23.858 to 23.836 ms (effectively unchanged). Median committed bytes were 625,363 to 626,297 compact and 1,660,707 to 1,655,830 fast; small size variation also reflects segment partitioning. Sidecars occupied 1,705 bytes; no transaction or nested sort-spill bytes remained in these builds. Three small-corpus build samples are limited evidence, not a stable throughput or p95 guarantee.

## Indexing and storage

The smaller repeated indexing experiment uses seven before/after pairs for each of four cases: **56 independent builds**, four workers, 256 MiB, same generated source and uncontrolled OS cache. These measurements were collected before the last search-only metadata refinements; raw executable hashes identify that checkpoint. They measure the combined changes, not isolated attribution to the queue or CRC.

| Corpus/profile | Baseline median wall ms | Current median wall ms | Ratio of medians |
| --- | ---: | ---: | ---: |
| 10,000 short files, compact | 272.308 | 259.549 | 1.05x |
| 10,000 short files, fast | 550.024 | 485.798 | 1.13x |
| 2,000 regular files, compact | 170.305 | 158.837 | 1.07x |
| 2,000 regular files, fast | 332.683 | 306.154 | 1.09x |

These are modest, noisy gains: medians of paired speedup ratios range from roughly 1.05x to 1.12x and differ from ratios of medians. With seven samples, nearest-rank p95 is the maximum; it does not establish a production tail. No non-empty transaction scratch was observed immediately after these 56 builds. Raw data: [optimization-indexing.json](../bench/results/optimization-indexing.json).

On the full 100k corpus, committed compact data changed from **505,425,112 to 521,926,014 bytes (+3.26%)**, including 16,424,945 bytes of literal sidecars. Recognized fast data changed from **711,060,495 to 710,777,848 bytes (-0.04%, effectively unchanged)**. In the fast build's single pair, wall time was 11.359 versus 11.396 seconds: no convincing build speedup is claimed for that case.

Some old scratch directories contained transaction or nested sort-spill files. They are disclosed separately, not counted as a compression win. The source of those retained artifacts was not reproduced; a code lifetime review found synchronous cleanup before publication and did not justify a speculative production fix. Comparing the raw directory totals would produce misleading size improvements.

The optional sidecar keeps mandatory format 2 unchanged. Both readers were tested against both writers for both profiles, including exact results, content IDs, snapshots and verification. See [optimization-compatibility.json](../bench/results/optimization-compatibility.json).

## Updates and compaction with pinned readers

The final measured core passed **14 paired maintenance series and 168 measured operations**, using independent 2,000-file, 50-line fixtures, four workers and 256 MiB. Each pair creates indexes, opens two readers pinned to generation 1, performs a real strict no-op before modifying the fixture, runs three mutation generations, then compacts. Each mutation changes eight files, deletes three and adds three; the middle update receives exactly fourteen paths, while the other two use full strict scans.

Fresh-process median wall times, milliseconds:

| Profile | Stage | Baseline | Current | Ratio of medians | Current faster pairs |
| --- | --- | ---: | ---: | ---: | ---: |
| compact | Create | 179.797 | 181.197 | 0.992x | 4/7 |
| compact | Strict no-op | 71.399 | 68.056 | 1.049x | 6/7 |
| compact | First global update | 83.070 | 72.301 | 1.149x | 5/7 |
| compact | Targeted update | 9.323 | 8.467 | 1.101x | 5/7 |
| compact | Third global update | 70.748 | 72.124 | 0.981x | 3/7 |
| compact | Compact | 173.327 | 151.929 | 1.141x | 5/7 |
| fast | Create | 303.431 | 290.655 | 1.044x | 3/7 |
| fast | Strict no-op | 76.437 | 77.268 | 0.989x | 4/7 |
| fast | First global update | 88.031 | 81.388 | 1.082x | 4/7 |
| fast | Targeted update | 10.544 | 10.595 | 0.995x | 3/7 |
| fast | Third global update | 76.982 | 72.365 | 1.064x | 6/7 |
| fast | Compact | 297.414 | 285.960 | 1.040x | 3/7 |

Performance is modest and mixed. Compact create/third update and fast no-op/targeted update medians regress by 0.78%, 1.94%, 1.09% and 0.48%. The fast create/compact ratios of medians look better, but median paired speedups are **0.982x/0.993x**, with only three faster pairs each; these do not establish consistent gains. Fast compact p95/max rises from 327.137 to 334.972 ms. With seven samples, nearest-rank p95 equals the maximum and is not a reliable production-tail estimate.

All **70 validation checkpoints** passed: independent totals/counts for ten queries, exact top-three ordered hits including scores/snippets on the same snapshot, unchanged original-reader results/content and full index verification. Full path sets are compared for queries returning at most three hits; the frequent term is a total/top-three check, not a comparison of every ranked hit. The series contains 1,400 old/new-reader comparisons on the same snapshots. Updates run in separate CLI processes, so this does not measure one persistent Service doing update/find invalidation cycles.

Maximum current maintenance-child RSS was **32,043,008 bytes (30.56 MiB)**, versus 31,420,416 baseline. The two idle pinned readers are excluded, and small-child values can include the Python pre-exec floor. All 168 completed operations left zero transaction/nested-sort scratch. Committed sizes include retained generations; GC is intentionally absent, so post-compaction retained bytes are not a compression ratio or a leak claim.

The full report has `status: passed` (this schema has no `complete` flag): [optimization-maintenance-release.json](../bench/results/optimization-maintenance-release.json). These are small-corpus measurements, not million-file maintenance-throughput evidence.

## Million-file scale

The A+B planner/worker checkpoint was exercised on **1,000,000 files, 5,000,000 lines and 222,455,560 source bytes**. These are smaller, five-line files than the 100k performance corpus. Generation was a separate, untimed preparation step. Eight workers and a 256 MiB accounted workspace were requested.

Each operation below has **one sample**: this establishes scalability and correctness, not a repeated before/after speedup.

| Operation | Wall seconds | CPU seconds | Peak RSS MiB |
| --- | ---: | ---: | ---: |
| Compact index construction | 21.438 | 115.705 | 42.61 |
| Frequent term, 1,000,000 matching files | 1.658 | 1.658 | 88.18 |
| Frequent AND NOT rare, 999,999 matching files | 1.386 | 1.386 | 88.13 |
| Literal full-content scan, profiling enabled | 8.340 | 18.478 | 88.43 |
| Full verification of all files | 13.641 | 13.639 | 83.21 |

All ten query cases passed exact total/path/count checks, including rare terms near both ends, misses, AND/OR/NOT, an anchored literal and regex, and the full-content literal. Rare-term line positions and the last file's exact five-line stored snapshot also passed. The maximum measured RSS over every operation was **92,729,344 bytes (88.43 MiB)**. Small-target RSS can include the helper's inherited pre-exec floor.

The committed index occupied **482,530,403 logical bytes in 127 segments**. No transaction or nested sort-spill files remained immediately after construction. No literal sidecars were emitted, as expected for documents smaller than 512 bytes. This is not a claim that the index is smaller than the 222 MB source corpus.

The full-content query read 222,455,767 bytes including its selected snippet, used eight workers and performed 653,962 content opens. Summed worker open time was 14.142 seconds versus 0.371 seconds of matching. These overlapping stage times are not additive wall time; they exposed segment-handle locality as a remaining bottleneck for follow-up measurement.

Raw data, exact binary hashes and all operation outputs: [optimization-million.json](../bench/results/optimization-million.json). This artifact records the measured checkpoint, not an unmeasured extrapolation from the 100k case.

## Follow-up: segment locality and bounded batches

The million-file diagnostic justified two isolated, paired experiments on identical immutable indexes. The first changed only dispatch locality over the original bounded batch. It sorted auxiliary indices by segment/content offset and assigned balanced contiguous slices, leaving candidate charging and result reduction in their original order. The second increased the batch from 16 to 128 documents per worker, retaining the same pending-work pressure flush and stream-handle bounds.

| Workload | Pairs | First experiment: striped to local, median ms | Second experiment: small to larger local batch, median ms |
| --- | ---: | ---: | ---: |
| Million-file compact full-content literal | 7 | 6,873.42 to 5,379.47 | 5,577.63 to 4,302.44 |
| 100k compact broad literal | 21 | 928.32 to 814.36 | 863.87 to 712.20 |
| 100k fast broad literal | 21 | 852.00 to 755.41 | 721.41 to 551.57 |

These columns are **separate measurement windows**, not one combined distribution. They use explicit warmups and uncontrolled warm OS caches. The million-file second experiment reduced median CPU from 10,965.67 to 8,694.73 ms and its separate profiled content-open count from 217,407 to 127,346. In the first experiment, opens had fallen from 653,962 to 217,407. The 100k larger-batch profiled calls opened only 30 content streams in each profile; counters do not represent the number of distinct files in the index or the peak simultaneously open handle count.

Every private variant passed 67,688 mixed-size/empty-file, serial/parallel, exact-result and budget assertions. Full million-file results, 100k ranked/path-sorted top-k results, snippets and budget errors were unchanged. Profiling calls were excluded from the latency distributions. The final integrated build additionally polls cancellation at content-scan entry, covering zero-line documents; its final whole-product measurements are recorded separately above.

Both experiments and binary hashes are retained: [locality](../bench/results/optimization-locality.json), [larger batch](../bench/results/optimization-locality-batch.json). The maximum auxiliary order vector contains 2048 indices. Its live payload is accounted, but vector spare capacity and transient allocator behavior are not a hard physical `budget / 64` limit.

## Checkpoint correctness and native-platform checks

The pre-follow-up core checkpoint `dbcddf76708b614c26e212fa27b48efa33ddfd58` passed [native CI run 39937206527](https://github.com/r1skme/ReSearchify/actions/runs/39937206527):

| Target | Build/test status |
| --- | --- |
| Windows x86-64, MSVC | Native build, 13/13 CTest, harness tests and rg smoke passed |
| Linux x86-64, GCC | Native build, 13/13 CTest, harness tests and rg smoke passed |
| Linux ARM64, GCC | Native build, 13/13 CTest, harness tests and rg smoke passed |
| macOS Intel, Apple Clang | Native build, 13/13 CTest, harness tests and rg smoke passed |
| macOS Apple Silicon, Apple Clang | Native build, 13/13 CTest, harness tests and rg smoke passed |
| Linux x86-64, Clang | Separate native build and CTest passed |
| Linux x86-64, ASan/UBSan | Full CTest passed |

In that run, Linux/macOS jobs checked the host architecture and ran the extracted archive's CLI; macOS additionally checked the Mach-O architecture. Windows used an explicit MSVC x64 build and ran the built CLI before packaging. A follow-up workflow change adds PE-machine validation and execution after extracting the Windows ZIP; its completion must be checked on the final PR head, not inferred from this earlier run. Benchmark smoke is a functional check, not a statistically controlled cross-platform speed comparison. The detailed timing tables in this report are **Linux x86-64 only**.

Additional local checks include 484 baseline/current exact result comparisons on shared indexes (scores, counts, snippets, content identities, order and pagination), old/new writer/reader compatibility, and focused ThreadSanitizer concurrent search/cancellation/update/GC tests. CRC tests compare against an independent bitwise oracle over seeds, alignments, tails and chunk boundaries. Regex and content tests include Unicode, first-line BOM, CRLF, line boundaries, NUL and corrupt snapshots/sidecars.

The frozen locally measured binary (SHA-256 `86a12b278f1414c81beba973cb67387c34d09b91842f78de5a02af7758c05b4a`) additionally passed **852 exact result and candidate/read-budget comparisons** against the generic planner on compact/fast indexes across updates/deletions, including punctuation-only documents with zero indexed tokens. All six search cases at that checkpoint also passed ThreadSanitizer with **2,415,676 assertions** and no reported races. Cancellation is checked before search preparation and periodically inside rejected-candidate, stale-posting, generic intersection and global range-hint loops, not only after finding an evaluable candidate. A deterministic native test covers serial/parallel pending empty-file cancellation and successful retries. A compact command/hash/result record is retained in [optimization-validation.json](../bench/results/optimization-validation.json).

The first Windows run found a test fixture named `nul.txt`, a reserved Windows device name. It was renamed to `binary_nul.txt`; the binary-content assertion remains intact. The next complete native matrix passed. This is recorded rather than silently describing the first run as successful.

## Explicit limits and rejected shortcuts

- No false-negative prefilter, approximate count, early-exit ranking or disabled integrity check is used to obtain these numbers.
- The 256 MiB option budgets accounted workspaces, **not a hard whole-process RSS cap**. Shared caches, per-query workspaces, allocator overhead and thread stacks need headroom. Multiple C++ callers add their own workspaces; one stdio Service dispatches sequentially.
- `Service` already cached its `Index` before this work. No new index-cache claim is made. Write/refresh invalidation is preserved; find-only reuse differs from update/refresh/find workloads.
- Regex parallelism remains disabled to retain the established query-wide step budget. C++ cooperative cancellation is implemented; no new JSONL/MCP in-flight cancellation protocol is claimed.
- Selected hits may still reread their content for snippets, with integrity checks. No claim of eliminating all snippet I/O is made.
- A portable CRC improvement was retained. A word-at-a-time UTF-8 experiment was rejected after a Unicode regression. No unmeasured SSE2/AVX2/NEON backend, memory mapping or native filesystem API was added; such changes need separate evidence and must respect the repository's standard-library-only contract.
- Compaction and update correctness are covered, but there is no statistically powered end-to-end million-file maintenance throughput claim. Cold OS-cache latency, other filesystems/hardware and production multi-agent load distributions remain unmeasured.
- Recovery preserves the existing process-crash contract; this work does not add OS-level power-loss durability guarantees.

## Reproduce

See the paired commands and measurement rules in [BENCHMARKS.md](BENCHMARKS.md). `scripts/perf_benchmark.py` measures generated-corpus CLI/JSONL behavior, `scripts/compare_benchmarks.py` pairs baseline/current/rg, `scripts/bench_real_source.py` captures and verifies real source, and `scripts/scale_smoke.py` separates million-file generation from measured build/query/verify. The Python tools are development-only; the C++ executable does not require Python.
