# Benchmarks

## Current optimization implementation

Start with the [implementation and evidence map](PERFORMANCE_IMPLEMENTATION.md).
The [initial integrated report](PERFORMANCE_OPTIMIZATION.md) records its own
frozen checkpoint; the later [tiny-file planner](PERFORMANCE_TINY_PLANNER.md),
[known-path updates](PERFORMANCE_UPDATE_FOLLOWUP.md) and
[NFA execution](PERFORMANCE_NFA_FOLLOWUP.md) reports identify their separate
before/after binaries. Do not multiply gains from separate windows or relabel
an earlier checkpoint as the final executable. The v1/v2 results later on this
page are historical.

The development-only Python tools require Python 3.11+; the C++ engine and downloaded CLI do not require Python. `scripts/test_perf_benchmark.py` tests the measurement harness itself.

### Reproduce a paired comparison

Build the baseline and current source as separate Release executables. Use one **new** work directory. Do not modify its source corpus during a comparison:

```sh
git worktree add ../researchify-baseline eb858348169157686e70104af6b64f9539a39fe8
cmake -S ../researchify-baseline -B ../researchify-baseline/build -DCMAKE_BUILD_TYPE=Release
cmake --build ../researchify-baseline/build --config Release --parallel 4
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build --config Release --parallel 4
ctest --test-dir build -C Release --output-on-failure

./build/cs_bench --work bench-data-paired --phase generate --files 100000 --lines 50
../researchify-baseline/build/cs_bench --work bench-data-paired --phase create --index baseline-compact --profile compact --threads 8 --memory-mb 256
./build/cs_bench --work bench-data-paired --phase create --index current-compact --profile compact --threads 8 --memory-mb 256
python3 scripts/compare_benchmarks.py --baseline-executable ../researchify-baseline/build/rs --executable build/rs --baseline-index bench-data-paired/baseline-compact --index bench-data-paired/current-compact --corpus bench-data-paired/source --iterations 21 --output bench-data-paired/paired.json
```

For `fast`, build **both** comparison indexes with `--profile fast` and different index directory names. Never compare different profiles without labeling the difference. On Windows use the Release executable paths for **both** checkouts, for example `build/Release/rs.exe` and `../researchify-baseline/build/Release/rs.exe` (likewise for `cs_bench.exe`), and `python` instead of `python3` if appropriate. `--config Release` and CTest's `-C Release` are required with the Visual Studio multi-configuration generator; `CMAKE_BUILD_TYPE=Release` alone does not select its build configuration.

### Process startup, repeated requests and profiling

```sh
python3 scripts/perf_benchmark.py --executable build/rs --index bench-data-paired/current-compact --corpus bench-data-paired/source --query term --query literal --iterations 21 --output bench-data-paired/sessions.json
./build/cs_bench --work bench-data-paired --phase search --index current-compact --repeats 21 --stages 1
./build/rs -l 'needle_42 = 0;' --index bench-data-paired/current-compact --json --profile
python3 scripts/check_index_compatibility.py --old ../researchify-baseline/build/rs --new build/rs
python3 scripts/compare_sessions.py --baseline-executable ../researchify-baseline/build/rs --executable build/rs --index bench-data-paired/current-compact --corpus bench-data-paired/source --term-iterations 201 --literal-iterations 21 --output bench-data-paired/paired-sessions.json
```

The session runner reports fresh-process latency, the first persistent-process request, warmed JSONL requests and process CPU/RSS separately. `--interleave-update` additionally exercises an explicitly writable JSONL service with a validated strict **no-op** update between finds; it is not an edit/update benchmark and does not turn read-only MCP into a writer. Full untimed path/count comparison against rg is enabled by default. For very large broad-match corpora, `--skip-full-check` is explicit and its reduced correctness scope is recorded.

`compare_sessions.py` uses two persistent readers of the **same** immutable index with balanced baseline/current request ordering. First requests and warmups are separate; it preserves per-request client latency and engine metrics. It does not invent per-request CPU or RSS measurements. Reader compatibility with that index must be checked before comparing versions.

Search-stage profiling is a separate untimed diagnostic: timers can perturb short-line matching. Worker-stage totals overlap and cannot be added to end-to-end wall time. `candidates` counts examined document candidates; `filtered_documents` counts compact signatures that avoid content reads. These are different measurements.

### Real source and million-file smoke

Use new output/work directories. The real-source script captures only the named baseline checkout's `src`, `include`, `tests` and `docs`, verifies the bytes with a manifest, and compares full results independently of timings:

```sh
python3 scripts/bench_real_source.py --source-checkout ../researchify-baseline --output-dir bench-real-source --baseline ../researchify-baseline/build/rs --current build/rs --iterations 21 --build-iterations 3
python3 scripts/scale_smoke.py generate --bench build/cs_bench --work bench-million --files 1000000 --output million-generation.json
python3 scripts/scale_smoke.py run --bench build/cs_bench --rs build/rs --work bench-million --files 1000000 --memory-mb 256 --output million-smoke.json
```

The million-file smoke uses five lines per file. Generation is deliberately separate from measured build/search/verify, and the report lives outside the source/index. It validates one scale point and measured memory use, not a repeated before/after speed claim. Do not run corpus generation, builds or another benchmark concurrently with a timed comparison.

For pinned external projects, `prepare_external_source.py` captures original
regular UTF-8 C/H files from an explicitly identified source archive and records
every exclusion plus per-file SHA-256. `external_source_benchmark.py` separates
build, full-result validation and measurement phases; its `--resume` contract
checks source, binary and snapshot identities. It freezes per-query sample
counts before timing, including an explicit `--wide-regex-iterations` override
for expensive broad queries. At most two independent readers may overlap during
full correctness validation; both finish before any timed target or warmup.
All measured baseline/current/rg target processes run sequentially.

### Reproduce the tiny-file comparison

The reported Windows source fixture was not attached. The generator below is
an explicit eight-line surrogate: 100,000 files, 800,000 lines and 19,034,450
source bytes. Its identity is recorded in every result; it is not the reported
800,101-line source tree.

```sh
python3 scripts/tiny_comparison.py \
  --baseline ../researchify-baseline/build/rs --current build/rs \
  --baseline-revision eb858348169157686e70104af6b64f9539a39fe8 \
  --current-revision CURRENT_FULL_COMMIT_SHA \
  --work NEW_DISPOSABLE_WORK_DIRECTORY --files 100000 --iterations 21 \
  --session-iterations 1000 --fresh-processes 1000 \
  --update-iterations 7 --index-threads 4 --rg rg
```

Use the actual full current commit SHA. Windows uses `python` and the native
`build/Release/rs.exe` paths for both versions. The output directory must be new.
The driver checks full path/count maps and real rg JSON snippets before timing,
records 1,000 fresh process searches separately from 1,000 resident JSONL
requests, then measures strict and known-path updates on its owned fixture.
Update mutations are restored and independently checked; failure evidence is
retained. Index creation is a separate single observation for each writer and
profile, not a stable build-latency distribution.

### File-data cache experiments

The ordinary comparison scripts retain uncontrolled/warmed OS-cache labels.
The separate [scoped cache driver](CACHE_BENCHMARK.md) can observe residency of
explicitly owned disposable source/index files on 64-bit Linux. Successful
`DONTNEED` advice alone is not a cold-cache result: the driver requires a
complete zero-residency observation immediately before a target to use the
`verified-cold-file-data-pages` label. Otherwise it reports the failed or
unverified observation. It never claims a cold executable, metadata cache,
device cache or entire machine, and does not reset global OS caches.

### Incremental maintenance and exact-planner regression checks

```sh
python3 scripts/maintenance_benchmark.py --baseline ../researchify-baseline/build/rs --candidate build/rs --work bench-maintenance --output maintenance-results.json --iterations 7 --files 2000 --threads 4 --memory-mb 256
python3 scripts/check_anchor_equivalence.py --before CONTROL_RS --after build/rs --output anchor-equivalence.json
```

Maintenance uses a new fixture directory and an external report path. Each profile/iteration constructs separate indexes, performs a strict no-op update before touching source timestamps, then runs two full strict updates, one known-path update and compaction. The targeted generation supplies all 14 affected paths and is not reported as a global freshness check. Saved readers stay pinned while fresh readers check the new snapshots. Cross-reader scores are compared on the same physical snapshot, since independently segmented indexes can have different stale document-frequency estimates before compaction. Retained generations are not silently counted as removable scratch; GC is a separate operation. Reported maintenance-process RSS excludes the two otherwise idle pinned reader processes.

For the anchor regression script, `CONTROL_RS` is a compatible generic-planner checkpoint, not an arbitrary old binary. It checks ordered results and candidate/read-budget boundaries on compact/fast indexes with update/deletion churn and zero-token punctuation documents. Binary hashes identify the exact compared revisions. It is a correctness test, not a performance benchmark.

### Measurement rules

- Fresh executable startup is **not cold filesystem cache**. Ordinary search/session comparisons record uncontrolled/warmed OS cache conditions. The separate scoped cache driver has the narrower file-data residency contract described above; it does not reset global OS caches.
- Paired comparisons shuffle baseline/current/rg order with a recorded seed and preserve raw samples, binary hashes, exact commands, CPU/wall distributions, candidates and scanned bytes. A final identity check rejects changed binaries or index generations.
- Median, nearest-rank p95, minimum and maximum are reported. Seven samples yield p95 equal to the maximum; that is not enough to characterize a real production tail.
- RS count-only and `rg --count-matches` both complete occurrence scans, but the output projections differ: aggregate JSON versus per-file counts. Do not describe these as a ranked/snippet workload or compare them with `rg -l` early exits.
- The generated regex cases have at most one relevant match per line. For other corpora RS regex counts matching **lines**, so use the equivalent rg line-count operation when checking counts.
- Index build/update time is separate from query latency. Published size comparisons must separate recognized index data, transaction/spill scratch and reader/writer coordination files. Never hide retained scratch as a compression improvement.
- POSIX RSS uses an isolated child high-water mark and may include the Python parent's inherited pre-exec floor. Small-process peaks near that floor are not executable-only RSS. Windows uses process CPU and working-set counters; unavailable values remain null.
- A configured 256 MiB workspace budget is not proof of a strict whole-process RSS cap. Measure actual peaks, and include shared caches, allocator overhead, thread stacks and overlapping searches in deployment sizing.

## Historical v1/v2 measurements

Numbers below compare **CodeSearch v1 and the v2 engine behind ReSearchify**, not ReSearchify against grep, ripgrep or Everything. They do not represent a completed MSVC/Windows benchmark.

## Test corpus

| Property | Value |
| --- | ---: |
| Source files | 100,000 |
| Total lines | 5,000,000 |
| Original UTF-8 text | 349.66 MiB |
| Worker setting | 16 |
| Environment | Linux/GCC, approximately 4 CPU quota |
| Cache | Not explicitly cleared between every run |

## Before / after

| Measurement | v1 | v2 |
| --- | ---: | ---: |
| Initial index size | 681.55 MiB | **483.10 MiB** |
| Index open | 299.99 ms | **7.14 ms** |
| Rare term p50, open index (7 trials) | 1.504 ms | **0.080 ms** |
| `bucket42 AND common` p50, open index (7 trials) | 67.47 ms | **9.29 ms** |
| Term found in every file (one run) | 4868.82 ms | **475.92 ms** |
| Wide regex, 100,000 candidates (one run) | **15870.89 ms** | 19852.14 ms |
| Added index data for one edit | ~11.69 MB | **~16.4 KB** |

On this corpus, the new index uses about 29% less disk, while a repeated rare-term query inside the open process is about 19× faster. **A wide regex became slower.** ReSearchify should not be described as faster for every query. Neither table contains direct timings for ripgrep or Everything.

A second experiment used **1,000,000 files × 5 lines**. Indexing was measured at 75.05 s with 16 worker settings, 77.32 MiB peak process RSS, 463.03 MiB index. This validates one million *short* files, not fifty million lines.

Strict updates still read unchanged file contents, to detect edits even when size and timestamps stay the same. Targeted updates only examine explicitly supplied paths.

## Reproduce

Run the same CMake Release and tests as in the [README](../README.md), then on Windows:

```powershell
.\build\Release\cs_bench.exe --work .\bench-data --files 100000 --lines 50 --threads 16 --memory-mb 256 --repeats 7
```

On Linux use `./build/cs_bench`. Use a new work directory and compare equivalent corpora, storage, CPU budgets and cache conditions. Warm in-process timings must not be presented as complete CLI startup times.

Original measurement output is included in [bench/results](../bench/results) for verification. The logs describe earlier source revisions and are not a substitute for the current CI run.
