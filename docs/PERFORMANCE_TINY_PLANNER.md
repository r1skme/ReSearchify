# Tiny-document planner follow-up

Measured on 2026-10-23. This note isolates three search changes added after the earlier optimization pass. All timings below were executed on Linux x86-64 with GCC 13.3.0, Release, C++20 and LTO disabled. They are not native Windows or macOS measurements.

## Workloads and method

The new surrogate has **100,000 files, 800,000 lines and 19,034,450 source bytes**. It models the reported many-small-files workload; the user's exact corpus and original query commands were unavailable. The literal is `"needle_42 = 0;"`; broad regex is `/needle_[0-9]+/`; anchored regex is `/^int needle_[0-9]+ = 0;$/`. These are explicit surrogate queries, not a reconstruction of the user's commands.

The existing control corpus has 100,000 files, 5,000,000 lines and 366,645,610 source bytes. The scalability control has 1,000,000 files, 5,000,000 lines and 222,455,560 source bytes.

Both readers use the **same immutable generation-1 index** for each paired comparison. No index format, sidecar policy, source content or built index was changed by these patches. Tiny compact committed storage remained 45,103,447 bytes; tiny fast remained 129,596,742 bytes. Neither has literal sidecars because its documents are below the existing 512-byte threshold. The existing fast profile's larger storage cost still applies.

Every fresh-process comparison has one excluded warm-up per tool, randomized paired tool order with seed 1729, target-process wall/CPU time, process high-water RSS and per-request candidate/byte counters. OS cache was uncontrolled; these are **not cold-cache tests**. The 256 MiB setting applies to each ReSearchify reader. Process startup is included; the Python helper's startup is excluded. Index construction is excluded and unchanged here. `rg` performs complete occurrence scans with `--count-matches`, not early-exit `-l`; the RS timed request asks for total matching files without snippets. Output and operation differences remain documented in the raw command arrays. Matching-file totals agree for every timed sample.

RSS below the inherited Python pre-exec high-water floor cannot be resolved by this harness. The raw JSON retains all samples, min/max, p95, command lines, candidate/byte counts and exact executable identities. Seven-pair controls are regression checks, not high-confidence tail-latency estimates. Speedups from separate windows must not be multiplied.

## Exact executable provenance

| Stage | Source commit | Executable SHA-256 |
|---|---|---|
| Pre-follow-up | `349175274bb338fc5e39ca4ec417c296cc4f868e` | `c0046d12c64100b636bed378d8796a3d1c058b9ed692d3f8c8915adf6c80a4dd` |
| Single full-coverage driver | `56b1aa333b344434bdc682ef231155d45953cc42` | `7f4331d6f4ef2dfdb5bbb106c40a81f9a580fc25b9d04eb51dc912f0fbf6c6fa` |
| Raw same-segment membership | `905623a4a488e433160be94b60400a1ab20df00f` | `9da6e3f62f4143e166ba77430c51e185c22bdca05a01a5b407a442103168a857` |
| Combined with full-coverage trees | `499f5f64a9dbb514d043025f4fe3448646043ada` | `70f3b983b141dd3504df919119e956c64decd463f1264527be43b16d6f1b545a` |

The first binary is the preserved earlier build used by the integration base; documentation-only commits do not change its executable. The next three binaries were built in the isolated follow-up checkout. The integration checkout also contains independent indexing changes, so a separately measured original-baseline-to-final comparison must identify that final binary explicitly.

## 1. Skip a single posting driver that covers every live file

A content-only leaf sometimes chooses an exact token such as `0` whose posting count equals the complete corpus. Merging that list and repeatedly resolving its catalog records does no filtering. Use the ordered catalog directly when the necessary driver has exact full coverage.

The proof is deliberately conservative: every active segment must belong to the snapshot's current generation, and `df` must equal the live file count. Incremental snapshots with stale postings keep the previous strategy. Exact-term and Boolean query roots keep their existing paths. The segment-generation divisor is now one shared named constant used by generation creation, validation and this decision.

Compact, before → single-driver change; 21 pairs:

| Query | Pairs | Wall p50, ms | Wall p95, ms | CPU p50, ms | CPU p95, ms | Candidates / scanned bytes |
|---|---:|---:|---:|---:|---:|---:|
| term | 21 | 5.956 → 5.661 | 6.781 → 8.190 | 5.751 → 5.478 | 6.547 → 7.932 | 1 / 0 |
| literal | 21 | 306.745 → 274.464 | 375.609 → 330.233 | 524.874 → 505.000 | 588.947 → 573.464 | 100,000 / 19,034,450 |
| no_match | 21 | 277.242 → 272.228 | 322.059 → 315.108 | 488.354 → 490.407 | 546.021 → 541.880 | 100,000 / 19,034,450 |
| broad_literal | 21 | 312.724 → 271.865 | 369.893 → 326.299 | 528.916 → 486.803 | 627.766 → 565.767 | 100,000 / 19,034,450 |

| Query | Pairs | Wall p50, ms | Wall p95, ms | CPU p50, ms | CPU p95, ms | Candidates / scanned bytes |
|---|---:|---:|---:|---:|---:|---:|
| broad_regex | 21 | 500.765 → 507.950 | 582.751 → 609.170 | 500.348 → 507.175 | 582.396 → 608.523 | 100,000 / 19,034,450 |
| anchored_regex | 21 | 561.784 → 529.077 | 628.650 → 597.570 | 561.249 → 528.644 | 628.311 → 597.027 | 100,000 / 19,034,450 |

Literal p50 improved 10.5%, broad literal 13.1%, and anchored regex 5.8% in these windows. Plain broad regex already used the catalog and changed by +1.4%; its path was not optimized by this stage. Candidates and content bytes are unchanged. Observed literal peak-RSS median fell from 53,145,600 to 51,650,560 bytes.

Evidence: [optimization-tiny-anchor-compact.json](../bench/results/optimization-tiny-anchor-compact.json), [optimization-tiny-anchor-regex.json](../bench/results/optimization-tiny-anchor-regex.json).

## 2. Check necessary same-segment postings before catalog lookup

For one positive content leaf whose prefilter is a pure conjunction, the driver can reject a raw `(segment, document)` posting using the existing membership cursors before resolving the catalog record. This avoids decoding metadata for documents that already fail a necessary posting condition. A retained candidate still passes the existing active-segment and posting line/column/frequency bounds checks, then the usual content verification and matching.

Each driver part advances only the corresponding segment's membership cursors. Old and new versions of the same document ID cannot supply different halves of an intersection. No additional posting cursor or materialized candidate set is allocated. The driver borrows a fixed query-local membership vector; member destruction order and deleted move operations preserve its lifetime. Cancellation remains polled in raw-rejection loops. Boolean roots, exact-term queries and generic OR-containing anchor trees retain their previous execution strategy.

Fast, single-driver binary → raw-membership binary; 21 pairs:

| Query | Pairs | Wall p50, ms | Wall p95, ms | CPU p50, ms | CPU p95, ms | Candidates / scanned bytes |
|---|---:|---:|---:|---:|---:|---:|
| term | 21 | 6.301 → 6.296 | 7.259 → 8.705 | 6.021 → 6.047 | 7.029 → 8.274 | 1 / 0 |
| literal | 21 | 58.039 → 10.797 | 64.818 → 14.012 | 57.659 → 10.621 | 64.538 → 13.767 | 12 / 2,272 |
| no_match | 21 | 6.201 → 6.263 | 8.597 → 10.491 | 6.038 → 6.080 | 8.197 → 10.236 | 0 / 0 |

The selective literal improved **58.039 → 10.797 ms, 5.38×**, with the same 12 final candidates and 2,272 content bytes. Its observed peak-RSS median fell from 29,327,360 bytes to the 15,728,640-byte measurement floor. This is metadata work avoided before the already existing intersection result, not a reduction in the reported final candidate count.

The raw-only change also exposed a real tradeoff: when every posting list covers the corpus, checking memberships twice adds work. The short guard retained this result rather than hiding it:

| Query | Pairs | Wall p50, ms | Wall p95, ms | CPU p50, ms | CPU p95, ms | Candidates / scanned bytes |
|---|---:|---:|---:|---:|---:|---:|
| broad_regex | 7 | 594.239 → 598.811 | 650.967 → 620.204 | 593.740 → 598.332 | 650.385 → 619.882 | 100,000 / 19,034,450 |
| anchored_regex | 7 | 655.288 → 695.919 | 775.837 → 876.023 | 654.936 → 695.471 | 769.619 → 875.314 | 100,000 / 19,034,450 |

The anchored-regex median increased 6.2% in that seven-pair window. The next change handles that proven full-coverage case.

Evidence: [optimization-tiny-raw-fast.json](../bench/results/optimization-tiny-raw-fast.json), [optimization-tiny-raw-wide-guard.json](../bench/results/optimization-tiny-raw-wide-guard.json).

## 3. Remove complete full-coverage content anchor trees

Extend the same fresh-segment proof through the bounded anchor tree: a term covers all files only at exact `df == files`; AND covers all only when both children do; OR covers all when at least one child does. The decision still applies only to a single positive content leaf. Individually frequent lists with smaller counts continue to intersect because their joint membership can be selective.

Fast, raw-only binary → combined binary; 21 pairs:

| Query | Pairs | Wall p50, ms | Wall p95, ms | CPU p50, ms | CPU p95, ms | Candidates / scanned bytes |
|---|---:|---:|---:|---:|---:|---:|
| broad_literal | 21 | 366.198 → 259.025 | 440.926 → 344.705 | 587.283 → 471.075 | 709.413 → 578.514 | 100,000 / 19,034,450 |
| broad_regex | 21 | 577.803 → 474.806 | 641.488 → 587.289 | 577.487 → 474.452 | 641.132 → 586.908 | 100,000 / 19,034,450 |
| anchored_regex | 21 | 612.311 → 505.196 | 807.798 → 592.058 | 611.947 → 504.577 | 807.488 → 591.676 | 100,000 / 19,034,450 |

This removes the useless full-list intersections and their extra raw membership checks. Broad literal improved 29.3%, broad regex 17.8%, and anchored regex 17.5% in this separate measurement window. These percentages are not multiplied by the preceding stage's ratios.

Evidence: [optimization-tiny-full-coverage-fast.json](../bench/results/optimization-tiny-full-coverage-fast.json).

## Combined large-corpus and million-file guards

These controls compare the preserved pre-follow-up executable directly with the combined executable. No new index was built. The actual `rg` distributions are retained, including outliers.

| Corpus / profile | Query | RS before p50, ms | RS combined p50, ms | rg p50, ms | Pairs |
|---|---|---:|---:|---:|---:|
| 100k / 350 MiB compact | term | 5.252 | 5.823 | 118.302 | 7 |
| 100k / 350 MiB compact | literal | 245.005 | 216.700 | 108.389 | 7 |
| 100k / 350 MiB compact | no_match | 215.053 | 212.936 | 104.693 | 7 |
| 100k / 350 MiB compact | broad_literal | 653.929 | 531.068 | 533.924 | 7 |
| 100k / 350 MiB compact | broad_regex | 1949.768 | 1876.156 | 343.493 | 7 |
| 100k / 350 MiB fast | term | 5.580 | 5.523 | 114.787 | 7 |
| 100k / 350 MiB fast | literal | 73.740 | 24.938 | 121.893 | 7 |
| 100k / 350 MiB fast | no_match | 5.949 | 5.808 | 94.322 | 7 |
| 100k / 350 MiB fast | broad_literal | 731.223 | 577.379 | 480.871 | 7 |
| 100k / 350 MiB fast | broad_regex | 1962.915 | 1844.810 | 523.075 | 7 |
| 1M / compact | literal | 4505.579 | 3988.283 | 883.637 | 7 |

Detailed wall/CPU p50 and p95 plus invariant work counters:

### 100k larger documents, compact

| Query | Pairs | Wall p50, ms | Wall p95, ms | CPU p50, ms | CPU p95, ms | Candidates / scanned bytes |
|---|---:|---:|---:|---:|---:|---:|
| term | 7 | 5.252 → 5.823 | 6.152 → 6.368 | 5.087 → 5.466 | 5.861 → 6.027 | 1 / 0 |
| literal | 7 | 245.005 → 216.700 | 280.001 → 251.765 | 244.686 → 216.060 | 279.449 → 251.452 | 100,000 / 54,884 |
| no_match | 7 | 215.053 → 212.936 | 221.054 → 223.481 | 214.713 → 212.567 | 220.840 → 223.065 | 100,000 / 0 |
| broad_literal | 7 | 653.929 → 531.068 | 763.258 → 760.428 | 1928.429 → 1901.907 | 2043.812 → 1935.730 | 100,000 / 366,645,610 |
| broad_regex | 7 | 1949.768 → 1876.156 | 2016.243 → 2137.857 | 1949.299 → 1875.720 | 2015.910 → 2137.448 | 100,000 / 366,645,610 |

### 100k larger documents, fast

| Query | Pairs | Wall p50, ms | Wall p95, ms | CPU p50, ms | CPU p95, ms | Candidates / scanned bytes |
|---|---:|---:|---:|---:|---:|---:|
| term | 7 | 5.580 → 5.523 | 17.248 → 6.768 | 5.408 → 5.308 | 17.031 → 6.555 | 1 / 0 |
| literal | 7 | 73.740 → 24.938 | 94.823 → 27.562 | 73.429 → 24.612 | 94.540 → 27.216 | 112 / 410,529 |
| no_match | 7 | 5.949 → 5.808 | 6.881 → 7.169 | 5.786 → 5.640 | 6.725 → 7.007 | 0 / 0 |
| broad_literal | 7 | 731.223 → 577.379 | 836.559 → 707.886 | 2009.454 → 1873.502 | 2332.937 → 2085.485 | 100,000 / 366,645,610 |
| broad_regex | 7 | 1962.915 → 1844.810 | 2060.522 → 2051.922 | 1959.011 → 1842.922 | 2060.000 → 2051.466 | 100,000 / 366,645,610 |

### One million small documents, compact

| Query | Pairs | Wall p50, ms | Wall p95, ms | CPU p50, ms | CPU p95, ms | Candidates / scanned bytes |
|---|---:|---:|---:|---:|---:|---:|
| literal | 7 | 4505.579 → 3988.283 | 4804.231 → 4159.080 | 9098.165 → 8332.732 | 9334.049 → 8660.343 | 1,000,000 / 222,455,560 |

The million-file literal retained 1,000,000 candidates and 222,455,560 scanned bytes. Wall p50 improved 4,505.579 → 3,988.283 ms and CPU p50 9,098.165 → 8,332.732 ms. Maximum observed RS RSS was 92,860,416 bytes before and 89,325,568 bytes after, below 256 MiB in these runs. The `rg` median was 883.637 ms, with a retained 22,409.262 ms outlier; seven-sample p95 is consequently the maximum, not a stable estimate of its tail.

Evidence: [optimization-tiny-combined-large-compact-guard.json](../bench/results/optimization-tiny-combined-large-compact-guard.json), [optimization-tiny-combined-large-fast-guard.json](../bench/results/optimization-tiny-combined-large-fast-guard.json), [optimization-tiny-combined-million-guard.json](../bench/results/optimization-tiny-combined-million-guard.json).

## Rare-term regression check

The first seven-pair compact control reported a +0.570 ms fresh-process rare-term median. A targeted 21-pair repeat reduced, but did not erase, the observed difference:

| Query | Pairs | Wall p50, ms | Wall p95, ms | CPU p50, ms | CPU p95, ms | Candidates / scanned bytes |
|---|---:|---:|---:|---:|---:|---:|
| term | 21 | 5.537 → 5.746 | 9.437 → 6.875 | 5.365 → 5.562 | 9.124 → 6.699 | 1 / 0 |

The residual fresh-process median is **+0.209 ms, +3.8%**. Internal first-query wall median was 1.615 → 1.729 ms. The change is small in absolute time; its cause was not isolated and it is not described as a proven speedup or dismissed as definitely noise.

A separate 201-pair warm JSONL comparison used two resident services, sequential requests, three excluded warmups and balanced alternating AB/BA order:

| Metric | p50, microseconds, before → after | p95, microseconds, before → after |
|---|---:|---:|
| Engine wall | 48.964 → 49.655 | 77.938 → 78.548 |
| JSONL request wall | 141.102 → 134.362 | 212.360 → 232.670 |

The warm internal median changed by +0.691 microseconds (+1.4%), while client request-wall median improved. Internal/request wall values are not CPU time. The harness also records a single literal pair as an interface smoke check; that single pair supports no performance conclusion.

Evidence: [optimization-tiny-term-recheck.json](../bench/results/optimization-tiny-term-recheck.json), [optimization-tiny-term-warm-recheck.json](../bench/results/optimization-tiny-term-warm-recheck.json).

## Correctness and validation

The combined source passed `search_perf` and `engine`: **2/2 CTest suites in 5.30 s**, including 9/9 search cases with 2,498,482 assertions and 44/44 engine tests. The three new focused cases verify:

- Exact counts, scores, snippet lines/columns/text, ordering, offset and continuation pagination against the equivalent legacy-plan query and independent literal/Boolean source oracle.
- ASCII/UTF-8 content, a Unicode path, and false candidates whose old and new versions share a document ID across different segments.
- Stale `df` after partial updates, the selective eight-candidate fallback, pinned old snapshots and full compaction.
- Candidate/byte budgets and cancellation in posting runs that reject every document before content evaluation.
- Corrupt content, corrupt required posting payloads, corrupt catalog data and **valid-CRC postings whose line exceeds the accepted document's bounds**.
- Exact-term, OR/NOT and mixed Boolean query roots retaining correct results.

A separate read-only review found no blocker in cursor monotonicity, same-segment liveness, borrowed-state lifetime or cursor budget accounting. This is not a new ThreadSanitizer run. Full integration CI/native-platform validation is recorded separately by the integration workflow; these local measurements alone do not establish Windows, ARM64 or macOS performance.

Validation provenance: [optimization-tiny-planner-validation.json](../bench/results/optimization-tiny-planner-validation.json).

## Remaining limits

Compact still scans tiny edge-only literals when no safe selective token exists; it does not gain a new small-file signature. Existing compact signatures still omit documents below 512 bytes. The million-file full scan remains slower than `rg`, and broad regex on the larger corpus remains slower as well. Regex retains its serial finite-global-budget execution contract. Mixed-generation snapshots conservatively keep posting drivers rather than treating stale counts as exact live statistics. `fast` keeps its existing larger index size; these search changes add no index bytes.

The raw-only anchored-regex regression and residual fresh rare-term difference are preserved above. No claim is made that every workload or platform is faster. Native hardware runs, cold-cache behavior and concurrent multi-client throughput require their own measurements.

## Reproduction

Build the indicated source revisions with the same Release toolchain and LTO setting. Preserve each executable before changing revisions. Use one existing immutable index for both compatible readers; do not rebuild it between paired readers. For example, with explicit task-specific paths:

```bash
python3 scripts/compare_benchmarks.py \
  --baseline-executable "$RS_BEFORE_BIN" --executable "$RS_AFTER_BIN" \
  --baseline-index "$RS_INDEX" --index "$RS_INDEX" --corpus "$RS_CORPUS" \
  --iterations 21 --query literal --query broad_literal --query broad_regex \
  --output bench/results/my-paired-run.json
```

The tiny surrogate and `anchored_regex` case come from the integration harness. The retained raw reports identify the exact queries, source sizes, profiles and binary hashes actually measured; do not compare these numbers with different per-query counts or an unrecorded cold-cache claim.
