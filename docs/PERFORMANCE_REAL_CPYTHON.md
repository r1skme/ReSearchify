# Final integrated comparison on pinned CPython source

This report compares the original ReSearchify **eb858348** with the integrated
implementation **22822074**, including the final NFA initial-transition change.
It is a complete, independent measurement window. The earlier tables in
[PERFORMANCE_OPTIMIZATION.md](PERFORMANCE_OPTIMIZATION.md) and the isolated
[NFA follow-up](PERFORMANCE_NFA_FOLLOWUP.md) keep their original scopes; their
speedups must not be multiplied by this report's speedups.

On this fixture every current query/profile median was lower than its original
baseline median. Literal and regex searches still lagged ripgrep, sometimes
substantially. These measurements support improvements to the existing engine;
they do not establish general performance superiority or reproduce the user's
unknown macOS/Windows executable and fixture.

## Source, executable and measurement identities

The fixture is an unmodified subset of **CPython v3.13.0** at commit
`60403a5409ff2c3f3b07dd2ca91a7a3e096839c7`: **1005 regular UTF-8 C/H files,
27,501,615 bytes, 834,893 lines**. The capture retained original file bytes.
It excluded 414 directory entries and 3877 non-C/H entries; every exclusion and
every retained file's size, line count and SHA-256 is preserved in the full
manifest. This is a source-only subset, not a checkout including every CPython
asset, and it is not a synthetic workload.

- Upstream archive: [pinned CPython source](https://github.com/python/cpython/archive/60403a5409ff2c3f3b07dd2ca91a7a3e096839c7.tar.gz).
- Archive SHA-256: `37c8e18b35e659da6ff4db74caea50ed4170cf66c6d51f97d22e436e338d7b5b`.
- Sorted retained-content manifest SHA-256: `676198eeeee0efa583187b692df25c3860ca76d89293582a16fc9697fa528323`.
- Canonical full-manifest identity: `5539e8d9a04e1d3f1fe9b70a8ba927b6a54541e09eea8f032d0c0b2a0fda43ef`.
  This hashes sorted compact ASCII JSON after removing the self-checksum field;
  it is the `fixture_manifest_sha256` recorded by the benchmark.
- Original manifest file-byte SHA-256: `34ca9b7d0f965345fa0e4a05b5fd6d7e5e35f26116d6ea29119544ff8a69f1bc`.
- Original source revision: `eb858348169157686e70104af6b64f9539a39fe8`.
- Integrated source revision: `228220743f205c370964fcb4475c8daee1f4b3b7`.
- Original executable SHA-256: `b0a2c90c2dcaa3a71993751ec895807f32e8959e7439397971880564087e9154`.
- Integrated executable SHA-256: `701c8a34f6578e212336e80628bdd711df6413d6c8f1e0edf7d8b58f8a4b1293`.
- Comparator: ripgrep 15.2.0, revision `e89fff89ac`, executable SHA-256
  `e62198eb19b136b88c330af83647b5a962cb99b6b1f066758568f12de1974849`.
- Measured host: Linux 6.18.55, x86-64, glibc 2.39; GCC 13.3 Release C++20,
  `-O3 -DNDEBUG`, LTO disabled. The harness used Python 3.12.14.

## Protocol and equivalent operations

Each of the seven cases was measured in both `compact` and `fast`: **21 paired
fresh-process samples per tool per row**, with baseline/current/ripgrep order
randomized using seed 7727 and one untimed warmup per tool/case/profile. This is
882 timed processes in total. All warmups and timed processes ran serially with
no other local benchmark running. Target startup and shutdown are included;
the isolated Python measurement helper's startup is excluded. The OS page
cache was uncontrolled: these are **not cold-cache results**, and a fresh
process does not imply a cold filesystem cache.

Searches used a 256 MiB budget, `limit=0`, `snippets=0`, a 1 MiB output envelope,
and the same explicit regex work limit of `18446744073709551615`. A zero result
limit suppresses result objects; it does not stop complete matching/counting.
ReSearchify returned the total matching-file count. Ripgrep performed full
per-file counts, with eight threads, and its per-file output cost is included
in timing. It did not use early-exit `--files-with-matches` or quiet mode.

Term and literal cases used ripgrep `--count-matches --fixed-strings`; terms
also used `--word-regexp`. The term results were checked against a capture-time
oracle with ReSearchify's exact byte-level token boundaries, so the comparison
does not assume general Unicode equivalence between the two products. Regex
counts mean **matching lines**, using ripgrep `--count`, not the number of regex
occurrences. The portable ASCII expressions and all exact commands are in the
raw report.

Each binary read its own immutable profile index. Index construction was
measured separately below. Full correctness, source hashing, profiling and
build times are excluded from search distributions. Both `compact` and `fast`
retained integrity verification, including CRC-32, fingerprint and UTF-8 checks.

## Search results

Times are milliseconds. `Before/current` p95 uses the nearest-rank definition;
with 21 samples it is the 20th sorted observation, still a limited tail sample.
Speedup is the original median divided by the current median. Every raw
wall/CPU/RSS sample, minimum, maximum, p95, execution order and internal counter
is retained rather than removing outliers.

| Profile / case | Original median | Current median | Speedup | p95 original / current | ripgrep median |
|---|---:|---:|---:|---:|---:|
| compact selective term | 4.828 | 4.803 | 1.01x | 6.074 / 5.721 | 8.911 |
| fast selective term | 4.974 | 4.462 | 1.11x | 6.194 / 6.306 | 8.787 |
| compact common term | 17.921 | 17.688 | 1.01x | 23.121 / 21.722 | 14.705 |
| fast common term | 18.061 | 17.176 | 1.05x | 31.487 / 24.283 | 14.132 |
| compact selective literal | 197.056 | 81.569 | 2.42x | 216.238 / 169.294 | 8.823 |
| fast selective literal | 16.103 | 14.195 | 1.13x | 21.325 / 17.778 | 8.213 |
| compact common literal | 217.334 | 68.810 | 3.16x | 251.493 / 110.707 | 14.420 |
| fast common literal | 179.451 | 64.087 | 2.80x | 197.708 / 129.383 | 12.454 |
| compact negative literal | 200.869 | 70.875 | 2.83x | 216.420 / 147.260 | 8.741 |
| fast negative literal | 20.443 | 16.279 | 1.26x | 22.928 / 24.531 | 7.700 |
| compact selective regex | 887.205 | 147.210 | 6.03x | 947.717 / 159.868 | 9.138 |
| fast selective regex | 170.717 | 32.661 | 5.23x | 207.308 / 39.742 | 8.956 |
| compact wide regex | 1237.376 | 679.277 | 1.82x | 1430.650 / 827.766 | 16.456 |
| fast wide regex | 1210.299 | 704.940 | 1.72x | 1375.478 / 832.526 | 15.298 |

The complete query strings are:

| Case | Pattern and interpretation |
|---|---|
| Selective term | Exact token `PyImport_ImportModuleLevelObject` |
| Common term | Exact token `return` |
| Selective literal | `PyImport_ImportModuleLevelObject(` |
| Common literal | `return `, including the trailing space |
| Negative literal | `researchify_absent_6e9ae0ce70a44e78bdf088c197fb7134` |
| Selective regex | `PyImport_[A-Za-z0-9_]+` |
| Wide regex | `(return\|if\|while)[ \t]` |

Rare exact-token lookup was faster than ripgrep here. Common-term and all
literal/regex medians remained slower. For example, current compact wide regex
took about 679 ms against ripgrep's 16.5 ms, and fast wide regex took about
705 ms against 15.3 ms. The largest original/current reductions therefore must
not be presented as victories over ripgrep.

No query/profile median regressed in this particular completed window.
Not every tail statistic improved: fast selective-term p95 rose from 6.194 to
6.306 ms, and fast negative-literal p95 rose from 22.928 to 24.531 ms.
That does not erase the separate **+1.70% prefix-regex guard regression** recorded
in the isolated NFA report or establish a universal absence of regressions.

## CPU, retained work and remaining limits

The following current counters were identical across all 21 samples of each
row. `candidates` counts candidates before later document-signature rejection;
it is not the number of files whose content was actually read. `scanned_bytes`
counts verified content bytes, not filesystem/device I/O or all index reads.

| Profile / case | Current CPU median, ms | Candidates | Verified content bytes |
|---|---:|---:|---:|
| compact selective term | 4.584 | 6 | 0 |
| fast selective term | 4.300 | 6 | 0 |
| compact common term | 17.311 | 728 | 0 |
| fast common term | 16.947 | 728 | 0 |
| compact selective literal | 145.605 | 1,005 | 26,613,214 |
| fast selective literal | 13.992 | 8 | 408,035 |
| compact common literal | 178.701 | 1,005 | 27,278,185 |
| fast common literal | 142.074 | 745 | 23,360,683 |
| compact negative literal | 141.849 | 1,005 | 26,606,300 |
| fast negative literal | 16.098 | 11 | 1,362,274 |
| compact selective regex | 146.800 | 1,005 | 27,069,243 |
| fast selective regex | 32.384 | 79 | 5,046,210 |
| compact wide regex | 678.966 | 1,005 | 27,501,615 |
| fast wide regex | 704.589 | 1,005 | 27,501,615 |

Compact still reads most of this corpus for literals and prefix regex; its
document-signature filter does not eliminate most large-file reads here.
Fast candidate filtering substantially reduces selective work but is much less
helpful on common queries. Both profiles read every source byte for wide regex.
The 679 ms compact wide-regex wall median and 679 ms CPU median identify a
remaining CPU-heavy path. Current compact common-literal CPU time was about
179 ms against 69 ms wall time, reflecting parallel verification rather than
an equivalent reduction in total CPU work.

The measured peak RSS for every current search sample was 15,728,640 bytes
(15 MiB). This is the isolated child high-water measurement and can include an
inherited Python pre-exec floor. It cannot support precise comparisons of the
executables' low memory consumption. The raw reports preserve this limitation;
no per-query or cross-platform memory savings are inferred from that floor.

## Index creation and storage

Builds used eight workers and a 256 MiB budget. There was **one observed build
per writer/profile**, in randomized writer order, which happened to place the
baseline first in both profiles. Cache state was uncontrolled. These are
descriptive single observations, not robust build medians or tail estimates.

| Profile / writer | Wall, ms | CPU, ms | Peak RSS, bytes | Committed index, bytes |
|---|---:|---:|---:|---:|
| compact baseline | 326.909 | 1902.820 | 39,792,640 | 38,958,952 |
| compact current | 254.721 | 1551.828 | 38,961,152 | 39,082,413 |
| fast baseline | 563.010 | 3807.086 | 55,603,200 | 68,759,194 |
| fast current | 540.431 | 3341.888 | 55,193,600 | 68,578,654 |

All four builds reported exactly 1005 indexed files, 27,501,615 bytes and
834,893 lines, with zero skipped source files. Completed transaction/spill
scratch and reader/writer coordination files occupied zero bytes. Index size
is the sum of committed logical file sizes, not allocated disk blocks or peak
temporary disk usage. The current compact index is 123,461 bytes larger
(about 0.32%); 60,455 bytes belong to the literal sidecar. The current fast
index is 180,540 bytes smaller (about 0.26%). Existing functionality and
verification were retained; these small size differences are not evidence of
a new compressed storage format.

The observed wall/CPU build reductions are encouraging, especially for compact,
but require repeated platform-specific measurements before broad claims.
Search timings exclude these construction costs; amortization depends on the
number and type of subsequent queries and updates.

## Complete correctness checks

Both readers' complete per-file result/count maps matched ripgrep for all seven
cases in both profiles. Checks used the real JSONL API, 1024 MiB, a 10,000-result
page limit and 1 MiB response envelope, outside the timed budget. They rejected
budget exhaustion, incomplete execution, duplicate paths, non-monotonic path
ordering and inconsistent continuation cursors. On this 1005-file fixture each
full result set fit one page; multi-page coverage belongs to larger fixtures
and the regression suite, not this result.

| Case | Matching files | Total occurrences / matching lines |
|---|---:|---:|
| selective term | 6 | 8 |
| common term | 728 | 45,893 |
| selective literal | 5 | 7 |
| common literal | 722 | 44,912 |
| negative literal | 0 | 0 |
| selective regex | 77 | 488 |
| wide regex | 934 | 111,106 |

For each case/profile, both executables also read the **same current snapshot**
and produced identical ranked top-16 result objects, including scores and
counts and content identities. This avoids treating writer-specific document IDs as ranking
differences. Independently, the first 16 sorted matching files (or every match
when fewer) were checked against original source bytes for full file counts,
line numbers, byte columns and snippet text, including BOM/CRLF normalization
and UTF-8-safe clipping. An empty query result performed the empty-result check.
The manifest, every source file, executable hashes and index state were checked
at phase boundaries. No source or index was updated during the experiment.

CPython full-map correctness ran sequentially. A later tooling-only change
allows at most two independent readers for full-map validation on Linux's much
larger fixture; they are joined before ranking checks, warmups or timing.
No concurrent correctness process is included in either performance series.

## Reproduction and evidence

- [Complete raw final report](../bench/results/optimization-real-cpython-final.json):
  every sample/command, frozen timing plan, oracle hash, correctness result,
  build report and storage measurement. Its `complete` field and all three
  phases are true.
- [Original full capture manifest, gzip-compressed](../bench/results/optimization-real-cpython-manifest.json.gz):
  decompresses to the exact manifest bytes whose hash is given above, including
  every source-file hash and every excluded archive member. The archive and
  source contents are not duplicated in the repository.

Create fresh directories and use executables built from the named revisions:

```sh
python scripts/prepare_external_source.py \
  --project cpython --archive /absolute/path/cpython-60403a5.tar.gz \
  --output-dir /absolute/path/new-cpython-capture

python scripts/external_source_benchmark.py \
  --fixture-dir /absolute/path/new-cpython-capture \
  --output-dir /absolute/path/new-cpython-results \
  --baseline /absolute/path/rs-original --current /absolute/path/rs-integrated \
  --baseline-commit eb858348169157686e70104af6b64f9539a39fe8 \
  --current-commit 228220743f205c370964fcb4475c8daee1f4b3b7 \
  --phase all --iterations 21 --build-iterations 1 --warmups 1 \
  --index-threads 8 --rg-threads 8 --memory-mb 256 --seed 7727
```

The raw file preserves the actual absolute commands used here. A fresh capture
has different capture-time/path metadata and therefore a different canonical
full-manifest identity; its pinned archive identity and retained-content
manifest identity must still match. Reusing a report requires `--resume` and
the exact original identities/options; no sample count can change after its
timing plan is frozen.

This report establishes runtime behavior only on the stated Linux x86-64
environment. Native CI results for Windows, macOS and ARM are separate build
and correctness evidence, not performance measurements on those platforms.
Cold-cache tests, updates, simultaneous agents and million-file scaling are
also separate workloads and are not claimed by this source-only comparison.
