# Verified-content regex follow-up

Measured on 23 October 2026. This change removes a duplicate UTF-8 validation
pass from regex searches over already validated snapshot lines. On the 100k
compact corpus, broad-regex fresh-process median decreased from **2,624.597 to
1,929.177 ms**, or **26.5% less time / 1.36x speedup** over the already optimized
checkpoint. ripgrep still completed that case faster.

These are **Linux x86-64 measurements**, prompted by the user's macOS results.
They do not reproduce an identified binary/query from the user's screenshots
and do not establish a macOS speedup. The original whole-product figures remain
in [PERFORMANCE_OPTIMIZATION.md](PERFORMANCE_OPTIMIZATION.md). The later
[literal-filter/serial-setup experiment](PERFORMANCE_LITERAL_FOLLOWUP.md) uses a
different measured executable and window; do not multiply their speedup ratios.

## Change and guarantees

`stream_text` strictly validates a complete normalized line before invoking its
content callback. Previously, each regex leaf called public `Regex::find`, which
validated every byte in that line again, even when a mandatory-prefix search
could immediately reject it. Multiple regex leaves repeated the pass again.

The two engine content callbacks, document matching and selected-hit snippets,
now use an internal `ValidatedRegexAccess` entry point. It consumes the same
line view while those validated bytes remain alive. Validation is not cached by
address or attached to a reusable workspace. The helper is in a source-internal
header; both public `Regex::find` overloads continue to validate the entire
input, including malformed bytes after an otherwise valid match.

The NFA, Unicode decoding, assertions, prefix search, result ordering and match
range semantics are unchanged. Both entry points still charge exactly
`text.size() + 1` work units before matching and retain all subsequent charges.
Avoiding a redundant pass does not increase the number of operations a query's
configured regex budget permits: the same valid input and initial budget
produce identical remaining work units, results or budget errors.

`stream_text` validation, CRC, fingerprint, snapshot pinning and read-budget
checks remain active. Invalid UTF-8 introduced into a snapshot after a warm
query is still rejected. Selected-hit content may still be reread for snippets.
This change adds no dependency, mandatory instruction set, platform API or
index-format migration. Regex execution remains serial to preserve the shared
query-wide work budget.

## Fresh-process measurement

The immutable corpus contains **100,000 files, 5,000,000 lines and 366,645,610
source bytes**. Both readers use the exact same existing compact index. The host
is Linux x86-64, GCC 13.3 Release (`-O3 -DNDEBUG`, LTO disabled), AMD EPYC 9V74,
with an eight-CPU cgroup quota. The requested accounted workspace is 256 MiB.
The comparator is ripgrep 15.2.0 with eight workers.

Each row has **11 paired repetitions**, deterministically shuffled tool order
and one excluded warm-up per tool. Every sample starts a fresh target process;
Python helper startup is outside the timed interval. OS caches are uncontrolled
and explicitly warmed, not cold. Index construction is excluded. ReSearchify
returns exact matching-file totals with `limit=0` and no snippets; ripgrep runs
complete `--count-matches` scans. The outputs are different projections, not a
comparison of ranked/snippet rendering. Matching-file totals agree on every
sample; all one-hit-per-line regex matches in the broad comparison also avoid a
line-versus-occurrence counting ambiguity.

Times are milliseconds. With 11 samples, nearest-rank p95 is the maximum and
does not establish a production tail distribution.

| Query | Before median | After median | Before p95 | After p95 | ripgrep median |
| --- | ---: | ---: | ---: | ---: | ---: |
| Broad regex `/needle_[0-9]+/` | 2,624.597 | 1,929.177 | 2,783.810 | 2,055.624 | 406.583 |
| `needle_42 AND /function_[0-9]+_[0-9]+/` | 5.653 | 5.286 | 6.265 | 5.746 | Not compared |
| Absent regex `/missing_[0-9]+/` | 294.042 | 289.688 | 369.600 | 351.962 | 96.008 |
| Literal control `needle_42 = 0;` | 292.794 | 296.388 | 363.933 | 327.293 | 99.366 |
| Exact-term control `needle_42` | 5.310 | 5.906 | 7.037 | 6.552 | 104.504 |

Broad-regex median CPU time decreased from **2,621.901 to 1,928.753 ms**. Both
readers still consider 100,000 candidates and scan 366,645,610 content bytes in
this count-only workload; the saving is CPU work, not reduced validation
coverage or fewer searched documents. Peak-RSS medians are 77,037,568 and
76,951,552 bytes, effectively unchanged. The POSIX high-water measurement can
include the helper's inherited pre-exec floor; the 15 MiB floor on tiny queries
is not an exact measurement of executable allocations.

The improved broad-regex median remains about **4.74x slower than ripgrep** in
this window. ripgrep itself varies substantially here (185.471–791.320 ms), so
this gap is a description of these medians, not a hardware-independent ratio.
The literal-control median increased **1.2%**, and the fresh exact-term median
increased **0.596 ms / 11.2%**. Neither control uses the modified content-regex
path; these observations are retained rather than presenting every row as a
win. The absent-regex result includes prefilter work and is not evidence that
the new entry point accelerates a query that does no content matching.

## Persistent exact-term control

To distinguish a small fresh-process timing change from sustained search cost,
two resident JSONL services executed the same term request in strictly
sequential, balanced AB/BA order for **201 measured pairs**. First requests and
three additional warm-ups are excluded. Both services read the same compact
index. Engine time is unprofiled internal wall time; request time also includes
the JSONL exchange and excludes client JSON parsing.

| Metric | Before median, ms | After median, ms | Before p95, ms | After p95, ms |
| --- | ---: | ---: | ---: | ---: |
| Engine | 0.055433 | 0.055594 | 0.086009 | 0.136685 |
| Request | 0.150496 | 0.148413 | 0.238058 | 0.355566 |

The engine-median difference is 0.161 microseconds and request median is slightly
lower. The fresh-process 0.596 ms increase is not reproduced as a comparable
warm-search median increase. However, both warm p95 values are higher, so this
control does not establish a universal regression-free latency distribution.
It measures the regex follow-up alone; the later serial-setup change has its own
controls in the literal follow-up report.

## Correctness and separate diagnostics

The modified source passed all **13 CTest suites** on Linux x86-64. Added tests
compare strict and internally validated execution for ASCII/Unicode, assertions,
alternation, nullable expressions and start offsets. They compare acceptance,
exact byte ranges and remaining budgets. Selected executions cover every budget
from **0 through 255**, including errors after workspace reuse and offsets
inside a multibyte character. Both public overloads reject malformed UTF-8 before
and after potential matches. Content tests cover long/block-boundary lines,
BOM/CRLF normalization, Unicode snippets and corruption after a warm read.

An additional CLI comparison runs ten queries on a three-file Unicode fixture
at seven budgets (0, 1, 2, 10, 30, 100, 10,000): **70 before/after pairs**. It
checks matching or identical errors, all returned first-page result fields,
counts, scores, snippets, cursors and exact work counters. The raw record stores
each successful reference response or expected error. This is not a comparison
of every page of an arbitrary large result set.

Two additional profiled calls per reader check broad and selective regex on the
100k index with `limit=20` and up to two snippets per hit. The returned pages,
total counts, scores, snippets, cursors, candidate/read counters and work units
agree. The broad case returns 20 of 100,000 matching files and charges
**735,945,224 regex work units** in both versions; the selective case returns its
single matching file and charges **7,574** in both. This establishes equality
of those complete returned pages, not all 100,000 ranked rows.

The broad diagnostic's `matching_seconds` changes from **1.020027 to 0.415867**.
Verification time is 0.692942 versus 0.706555 seconds; it remains enabled. Both
calls read 366,716,784 bytes in 100,020 reads, including snippet rereads, and use
one worker. These are separate instrumented observations, not samples in the
unprofiled latency table. Do not add overlapping stage times, compare their
extra snippet bytes directly with count-only bytes, or treat one profile pair
as a timing distribution.

## Scope and platform status

The isolated test and timing evidence in this report covers Linux x86-64 only.
The combined later checkpoint subsequently passed the native Windows/Linux/
ARM64/macOS matrix and paired macOS Intel/Apple Silicon measurements in
[run 39961942957](https://github.com/r1skme/ReSearchify/actions/runs/39961942957).
See [native measurements](PERFORMANCE_NATIVE.md) for that checkpoint's separate
results and binary identities. They do not change the scope of this isolated
experiment or establish the outcome of subsequent C++ changes. Cross-compilation
would not substitute for runtime tests.

No new cold-OS-cache, million-file, fast-profile regex or production multi-agent
latency distribution is claimed here. The unchanged validation/budget contracts
are checked directly; this experiment does not justify removing other content
checks or changing the query's work budget.

## Evidence and reproduction

Before executable SHA-256:
`86a12b278f1414c81beba973cb67387c34d09b91842f78de5a02af7758c05b4a`.

After executable SHA-256:
`4497cc63d9c1e2e2c43141d4b1c4bd52501ff3644bc14d8df7c69aaa69fe3eae`.

Measured source commit: `cecfbac62c51966c86e8f80eede0e468e82d5e2e`, based on
`bfac3761ee7d0004684adbf643fa7982bd3d9870`; integrated as `dc518f9`.
The base includes help/documentation changes after the frozen before binary,
which do not change the search algorithms. Later filter-cursor/CPU-setup changes
are outside this measured after binary.

- [11-pair timing distributions, commands and hashes](../bench/results/optimization-regex-followup.json)
- [201-pair control, profile responses and 70 differential checks](../bench/results/optimization-regex-followup-validation.json)

Use an existing compatible `cs_bench` generated compact index for both readers:

```sh
python3 scripts/compare_benchmarks.py \
  --baseline-executable "$RS_BEFORE" --executable "$RS_AFTER" \
  --baseline-index "$BENCH_ROOT/optimized-compact" \
  --index "$BENCH_ROOT/optimized-compact" --corpus "$BENCH_ROOT/source" \
  --iterations 11 --query broad_regex --query regex --query term \
  --query literal --query regex_no_match --rg-threads 8 \
  --output regex-followup-paired.json
```

`RS_BEFORE` and `RS_AFTER` identify the immutable executables above; `BENCH_ROOT`
is the explicitly generated fixture directory. The persistent control uses
`scripts/compare_sessions.py::compare_case` with the exact-term case, 201 pairs,
three warm-ups and seed 1729. The stored requests, results and order make the
control inspectable independently of the aggregate table. Keep profiling and
correctness probes outside measured search distributions.
