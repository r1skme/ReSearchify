# Literal-filter lookup follow-up

Measured on 23 October 2026. This comparison starts from the already optimized
build used before this follow-up. The immutable corpus has 100,000 files,
5,000,000 lines and 366,645,610 source bytes. All measurements below ran on the
available Linux x86-64 host with GCC 13.3 Release (`-O3 -DNDEBUG`). They are not
macOS performance measurements.

## Changes

1. Serial-only searches no longer ask `std::thread::hardware_concurrency()` for
   an unused worker count. The adaptive worker calculation now runs only for a
   large eligible content query. Explicit worker limits keep their existing
   behavior.
2. Compact literal-filter lookup reuses a query-local weak cursor to the last
   checked B+tree leaf in each of its at most 32 open table entries. Adjacent
   document IDs in the same leaf avoid another root traversal, cache key
   construction, cache mutex acquisition and copied 157-byte filter value.
   Values are borrowed only inside a callback that owns the page for that call.

The cursor does not retain decoded pages between calls. Cache eviction or clear
expires the weak pointer; another table identity, a non-monotone key outside the
leaf, or any other cache miss uses the checked root traversal. A leaf's full key
range must fit its root-route separator bounds before the cursor can reuse it.
The existing page CRC, filter schema and content-identity checks remain active.
The index format, index size and missing-sidecar content fallback are unchanged.

The measured current build also contains the separate verified-content regex
change. None of the query cases in the tables below is a regex query.

## Fresh process results

Each row contains 21 deterministically shuffled paired repetitions and one
excluded warm-up per tool. Both ReSearchify executables read the same immutable
index. No OS caches were flushed: these are fresh processes with an uncontrolled
but explicitly warmed OS cache. Index construction is excluded. ReSearchify
uses count-only output; ripgrep uses complete occurrence scans through
`--count-matches`, with matching-file counts checked on every sample.

All times are milliseconds; p95 uses the recorded 21 samples.

| Compact query | Before median | After median | Change | Before p95 | After p95 | ripgrep median |
|---|---:|---:|---:|---:|---:|---:|
| Rare exact term `needle_42` | 5.560 | 5.636 | +1.4% | 7.174 | 7.591 | 97.244 |
| Selective literal `needle_42 = 0;` | 306.820 | 243.756 | -20.6% | 342.122 | 277.497 | 108.502 |
| Short literal `0;` | 458.181 | 462.793 | +1.0% | 635.757 | 622.167 | 372.164 |
| Absent literal `ReSearchify_missing_2026` | 261.251 | 211.198 | -19.2% | 325.728 | 255.784 | 101.815 |
| Broad literal `return common;` | 675.448 | 630.155 | -6.7% | 824.700 | 753.070 | 492.972 |

Selective literal still examines 100,000 candidate metadata records and reads
54,884 content bytes in both builds. The absent literal examines 100,000 records
and reads zero content bytes in both builds. The improvement therefore comes
from cheaper metadata/filter access, not weaker verification or fewer reported
matches. The two broad literal cases still read all 366,645,610 content bytes.

The selective-literal process CPU median decreased from 306.296 to 242.952 ms.
Its peak-RSS median was 78,675,968 before and 78,503,936 bytes after; decoded leaf
reuse did not increase measured RSS. POSIX peak RSS can include the helper's
inherited pre-exec high-water floor, so the identical 15 MiB floor on tiny
processes must not be interpreted as precise target allocation measurements.

Compact selective/no-match scans remain slower than ripgrep in wall time in
this run. The short-literal median increased 1.0% and the fresh exact-term median
increased 1.4%; these outcomes are retained, including their full distributions.
The broad-literal result has substantial run-to-run variation.

## Persistent JSONL results

Two persistent services read one index. Request execution is strictly sequential
and alternates balanced AB/BA order. The first requests and three additional
warm-ups are excluded. Term cases contain 201 measured pairs; literal cases
contain 21. `engine` is internal unprofiled wall time, and `request` additionally
includes the JSONL exchange. These are different metrics.

| Profile/query | Engine median before, ms | Engine median after, ms | Request median before, ms | Request median after, ms |
|---|---:|---:|---:|---:|
| Compact exact term | 0.055263 | 0.058708 | 0.143777 | 0.172750 |
| Compact selective literal | 267.658388 | 206.197104 | 277.490342 | 222.039452 |
| Fast exact term | 0.058088 | 0.050467 | 0.173283 | 0.149176 |
| Fast selective literal | 3.414587 | 3.307496 | 3.630385 | 3.539777 |

The compact literal internal median improved 23.0%, with request latency down
20.0%. The fast exact-term internal median improved 13.1%. The compact exact-term
median did not improve in this run. Its internal p95 changed from 0.134513 to
0.089935 ms.

A recorded confirmation with 20 warm-ups and another 201 term pairs likewise
did not establish a compact term median improvement: 0.055634 to 0.057717 ms;
p95 0.101523 to 0.096956 ms. This confirmation's one literal sample is only an
execution/control sample, not evidence for a literal latency distribution.
Do not use the serial-setup change to claim an overall compact-term speedup.

### Isolated worker-setup control

To check the specific source of worker setup cost, each build also ran one
persistent service alternating `threads:0` and `threads:1` for the same exact-term
query, with 20 warm-ups and 201 pairs. Both options execute the serial posting
path. In the before build the internal medians were 73.929 and 67.630 microseconds
respectively, a 6.299-microsecond difference. In the current build they were
62.062 and 62.002 microseconds, a 0.060-microsecond difference. This control shows
that the unnecessary worker-discovery cost was removed; it does not resolve all
other sources of variation in end-to-end tiny-query latency.

## Correctness and limits

The complete 13-suite CTest run passed on Linux x86-64. Added cases exercise
sequential, reverse and random cursor lookups, missing and exact-edge keys,
empty tables, table-identity changes, cache expiration, borrowed value lifetime,
CRC corruption after cache clear, and an invalid sibling separator with valid
page checksums. Existing tests cover filter schema/content identity, missing
sidecar fallback, query results and snapshots.

Every benchmark sample checked matching-file counts. The persistent comparison
also compared the complete count-only response fields. Those benchmark requests
do not independently validate ranking, positions or snippets; the ordinary
engine and search test suites cover those contracts. No new runtime dependency
or platform-specific core API was added. No Windows, macOS or ARM64 runtime
test is claimed by this local follow-up.

## Evidence and reproduction

Before executable SHA-256:
`86a12b278f1414c81beba973cb67387c34d09b91842f78de5a02af7758c05b4a`.

Current executable SHA-256:
`c0046d12c64100b636bed378d8796a3d1c058b9ed692d3f8c8915adf6c80a4dd`.

Measured source worktree commit: `fa4eae4` (combines `e9a06af`, `8574541` and the
verified-content regex change). The worker-count and cursor changes are separate
commits. Raw files retain executable identity, exact commands/requests, ordering,
per-sample durations and correctness counters:

- [Fresh compact pairs](../bench/results/optimization-literal-followup-compact.json)
- [Persistent compact pairs](../bench/results/optimization-literal-followup-jsonl-compact.json)
- [Persistent fast pairs](../bench/results/optimization-literal-followup-jsonl-fast.json)
- [Compact term confirmation](../bench/results/optimization-literal-followup-jsonl-compact-confirmation.json)
- [Serial worker-setup control](../bench/results/optimization-serial-setup-control.json)

Run `scripts/compare_benchmarks.py` with the same compact index as both
`--baseline-index` and `--index`, `--iterations 21`, and cases `term`, `literal`,
`short_literal`, `no_match`, `broad_literal`. Run `scripts/compare_sessions.py`
on each compact/fast index with `--term-iterations 201 --literal-iterations 21`.
The control alternates the two requests recorded in its JSON inside a single
`Service` per executable. Preserve both successes and regressions when comparing
results on another host.
