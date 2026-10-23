# Regex initial-transition follow-up on pinned real source

This is an isolated change measured against **456a5cf**, after the earlier
whole-product, verified-UTF-8, tiny-file planner and update work. It does not
replace the frozen tables in [PERFORMANCE_OPTIMIZATION.md](PERFORMANCE_OPTIMIZATION.md).
The earlier UTF-8 follow-up is documented separately in
[PERFORMANCE_REGEX_FOLLOWUP.md](PERFORMANCE_REGEX_FOLLOWUP.md).
Do not multiply speedups from these separate measurement windows.

## Measured bottleneck and implementation

A separately profiled search of CPython v3.13.0 C/H source with
`/(return|if|while)[ \t]/` spent 0.796 seconds of 0.925 seconds in matching in
compact mode (approximately 86%). This query has no mandatory literal prefix.
The NFA repeatedly built an initial epsilon closure and its consuming threads
at positions where no first transition could accept the next character.
That single profiled diagnostic is excluded from all latency distributions.

The accepted change computes a 128-bit necessary first-ASCII set and the exact
initial closure and consuming-thread costs once while compiling eligible
patterns. At a fresh NFA epoch, if no continuation accepted the preceding
character, no result was found, and the next scalar cannot start the pattern,
the matcher avoids constructing those impossible threads. Non-ASCII scalars
use the existing path. Initial assertions and nullable expressions disable this
shortcut; EOF always runs the original closure. No regex language, matching
order, Unicode definition or prefilter semantics change.

One per-line dispatch selects a compile-time specialization. Patterns with a
mandatory prefix, initial assertions or nullable initial closure use the
original loop with no rejection flags or per-character shortcut branches.

The optimization preserves the exact work-budget accounting of its baseline.
Skipped initial closures debit every original stack pop, including duplicate
IDs in epsilon cycles; skipped consuming threads are charged at the same
logical step. Bulk debits are used only when they succeed. An insufficient
budget takes the original one-unit debit path so the remaining value and
`E_BUDGET` behavior stay identical. Existing full-input validation charges,
mandatory-prefix charges and the final EOF closure charges are retained.

This change does not bypass `ContentReader`, CRC-32, fingerprints, UTF-8
validation, snapshot verification or the existing content-byte/candidate
limits. The public regex API still validates untrusted UTF-8. The already
verified internal entry point introduced earlier retains its contract.

## Controlled real-source comparison

The fixture contains the original bytes of 1005 regular UTF-8 `.c`/`.h` files,
27,501,615 bytes and 834,893 lines, captured from CPython v3.13.0 commit
`60403a5409ff2c3f3b07dd2ca91a7a3e096839c7`. Its full manifest hash is
`5539e8d9a04e1d3f1fe9b70a8ba927b6a54541e09eea8f032d0c0b2a0fda43ef`.

Both readers used the same immutable 456a5cf-built index in each profile.
Each row has 11 fresh-process before/current/ripgrep pairs, randomized with a
fixed seed, plus one untimed warmup. Search budget was 256 MiB. The OS page
cache was uncontrolled; these are not cold-cache measurements. Helper startup
is excluded, while target process startup/shutdown is included. Index creation
is excluded from this isolated matcher comparison and measured separately by
the broader external-source benchmark.

Regex counts mean matching lines per file. ReSearchify completed full query
evaluation with `limit=0`, `snippets=0`; ripgrep used `--count`, not early-exit
`--files-with-matches`. Before timing, the complete path/count maps of both
readers matched ripgrep. The first 16 sorted matching files also passed an
independent source-byte check of lines, counts, snippet text and byte columns.
Full correctness used 1024 MiB outside the timing samples. Source hashes were
checked before and after the experiment.

All times below are milliseconds. Delta is current/before minus one; negative
is faster. The p95 column is before/current. With 11 observations this nearest
rank statistic equals the maximum observation, not a stable tail estimate.

| Profile and query | Before median | Current median | Delta | p95 before / current | ripgrep median |
|---|---:|---:|---:|---:|---:|
| compact selective regex | 158.879 | 156.606 | -1.43% | 249.658 / 181.783 | 9.700 |
| compact wide regex | 1005.844 | 671.748 | -33.22% | 1109.013 / 734.899 | 16.283 |
| fast selective regex | 32.461 | 31.578 | -2.72% | 76.619 / 75.721 | 8.891 |
| fast wide regex | 1085.755 | 719.484 | -33.73% | 1670.222 / 822.949 | 17.501 |

The broad query improved in all 11 pairs for each profile. The selective query
is `/PyImport_[A-Za-z0-9_]+/`, which exercises the retained prefix path. The raw
report also preserves every CPU-time and peak-RSS sample, process order, exact
command and internal metric. RSS is the isolated child high-water observation;
it may include an inherited Python pre-exec floor.

A substantial ripgrep gap remains: current broad-query medians were about
0.672 and 0.719 seconds versus 0.0163 and 0.0175 seconds. This patch improves the
existing bounded Thompson NFA; it does not establish general parity with
ripgrep or eliminate wide-regex work.

## Prefix and anchored guards, including the observed regression

An independent 100,000-file, 5,000,000-line synthetic fixture
(366,645,610 bytes) was checked with seven randomized process pairs per query.
The same compact index was used by both readers. Broad regex here means
`/needle_[0-9]+/`, which has a mandatory prefix; the anchored guard is
`/^int needle_[0-9]+ = 0;$/`.

| Profile and query | Before median | Current median | Delta | p95 before / current | ripgrep median |
|---|---:|---:|---:|---:|---:|
| compact broad regex | 2026.027 | 2060.482 | +1.70% | 2277.542 / 2116.300 | 532.777 |
| compact anchored regex | 1944.293 | 1932.826 | -0.59% | 2101.428 / 2061.464 | 138.611 |

The **+1.70% broad-query median regression is retained as an observed cost**.
The median paired delta was +2.73%; five of seven current samples were slower.
The anchored median improved 0.59%, while its median paired delta was +1.39%.
These small-sample guards are not evidence that every prefix query improves.
With seven samples, p95 again equals the maximum observed sample.

The first implementation kept shortcut bookkeeping in the common loop. It
improved CPython wide regex by about 39% but regressed this synthetic broad
median by **12.54%**, so it was rejected. Its exact raw reports and patch remain
in the repository. V2 isolates the added work in a separate compile-time
specialization and trades some of that broad-query gain for a much smaller
guard cost. The V1 source patch is evidence only and is not applied.

A separately profiled V1 guard had identical 100,000 candidates, 366,645,610
content bytes and 735,802,330 regex work units before/after. Most observed delta
was outside the matcher. Compiler layout effects are a possible explanation,
not a proven cause; no instruction-alignment or native-ISA tuning was added.

## Correctness and portability gate

- GCC 13.3 Release, C++20, `-O3 -DNDEBUG`, LTO disabled: 13/13 CTest tests passed.
- The independent `bench/regex_budget_probe.cpp`, linked separately against
  before/after libraries, produced **937,118 byte-for-byte identical rows**.
  It compares success/failure, match offsets, remaining budget and complete
  errors across 28 patterns, fixed/seeded-random inputs, invalid UTF-8, every
  byte start offset, boundary budgets, both APIs, workspace reuse and epoch wrap.
- Additional unit tests fix the exact closure charge for `(a|b)` and `(a?)*b`;
  the latter includes a repeated epsilon-loop ID. ASCII, CRLF, NUL, Unicode,
  empty text and exhausted budgets are covered.
- No new dependency, native ISA requirement or index-format change was added.
- Binary size increased from 1,131,216 to 1,131,968 bytes (+752); ELF `.text`
  increased from 933,028 to 934,936 bytes (+1908) in this build configuration.
- The performance measurements are **Linux x86-64 only**. Native CI for the
  eventual integrated commit must be reported separately; this report does
  not claim measured Windows, macOS or ARM acceleration.

The differential stdout SHA-256 is identical for both libraries:
`659ed249ab5b74ba0981a2271d3d44373e3b94d8e1f01f4e8a3c5be96bc38ec9`.
Full output remains reproducible from the committed probe; it is not embedded
as hundreds of thousands of repetitive text rows in the repository.

## Immutable identities and raw evidence

- Before source: `456a5cf88c15330eaf8ae8c752e351e09e0f5ceb`.
- Accepted implementation: `a7df68610eb88e3a3a5b3de39403ad68c1b48bea`.
- Before executable SHA-256: `444681365d904f513be7669bb9eb77ed4501ef1800f481f83714ca316fa9486f`.
- Measured V2 executable SHA-256: `701c8a34f6578e212336e80628bdd711df6413d6c8f1e0edf7d8b58f8a4b1293`.
- [11-pair real-source distributions and complete oracle checks](../bench/results/optimization-nfa-initial-cpython.json).
- [7-pair synthetic guards](../bench/results/optimization-nfa-initial-guard.json).
- [Build/probe metadata and excluded diagnostic responses](../bench/results/optimization-nfa-initial-validation.json).
- [Rejected V1 real-source samples](../bench/results/optimization-nfa-initial-v1-cpython-rejected.json).
- [Rejected V1 synthetic guard samples](../bench/results/optimization-nfa-initial-v1-guard-rejected.json).
- [Rejected V1 patch, never applied to the accepted implementation](../bench/results/optimization-nfa-initial-v1-rejected.patch).

To reproduce the differential check, compile the same probe against each
revision's core library and compare complete output:

```sh
g++ -std=c++20 -O2 -I include bench/regex_budget_probe.cpp "$BEFORE_LIBRARY" -pthread -o budget-before
g++ -std=c++20 -O2 -I include bench/regex_budget_probe.cpp "$AFTER_LIBRARY" -pthread -o budget-after
./budget-before > budget-before.tsv
./budget-after > budget-after.tsv
cmp budget-before.tsv budget-after.tsv
```

All benchmark process commands, fixed random seeds and execution orders are
preserved in the JSON files. The full-source benchmark tooling
`prepare_external_source.py` and `external_source_benchmark.py` captures pinned
sources and runs independent build, full correctness and paired timing phases.
