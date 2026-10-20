# Benchmarks

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
