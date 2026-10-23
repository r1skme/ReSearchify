# Final integrated tiny-file comparison

Measured on **23 October 2026, native Linux x86-64**. This is the complete
original-version versus final-integrated-version run on the generated
**100,000-file, 800,000-line, 19,034,450-byte** tiny-file surrogate. It is not the
user's Windows corpus reported as 18.7 MB and 800,101 lines, and it does not
establish performance on the user's Windows PC or Mac.

The accepted changes improve the broad search and update paths in this run.
The compact literal still scans every tiny document and remains slower than
ripgrep; the fast-profile resident rare-term latency regresses. Both results
are retained below. These measurements compare the integrated product with
the original version, so they must not be multiplied by isolated follow-up
speedups reported elsewhere.

## Evidence and exact provenance

- [Exact raw archive](../bench/results/optimization-tiny-final.zip): all 14 result JSON/log files, original fixture metadata, seven measured-revision drivers, independent recomputation script and source/build identity records. The archive has 29 entries; it contains neither the source corpus nor indexes nor executables.
- [Recomputed compact summary](../bench/results/optimization-tiny-final-summary.json): all query/update distributions, conditions, commands, counters, build records and correctness samples, without repeated timing arrays.
- [Provenance and SHA-256 manifest](../bench/results/optimization-tiny-final-provenance.json): archive and every entry's hash, compiler settings, local/public tree equivalence and a reproducible invocation.

Every one of **199 retained distributions** was independently recomputed from
its samples, with exact agreement for median, p95, min/max and sample count.
JSONL response timings and update child samples were also checked against the
corresponding distributions. No samples or outliers were removed. The raw
fresh-process files retain per-sample wall/CPU/RSS arrays and engine counters;
they do not retain each process's complete stdout. Their matching-file checks
were performed by the driver before accepting each sample.

| Version | Source commit | Executable SHA-256 |
|---|---|---|
| Original | eb858348169157686e70104af6b64f9539a39fe8 | b0a2c90c2dcaa3a71993751ec895807f32e8959e7439397971880564087e9154 |
| Final local build | 228220743f205c370964fcb4475c8daee1f4b3b7 | 701c8a34f6578e212336e80628bdd711df6413d6c8f1e0edf7d8b58f8a4b1293 |

The public equivalent of the final local source is
[`9713ff706d94e0af6a606a5cdc1bed39e03afe11`](https://github.com/r1skme/ReSearchify/commit/9713ff706d94e0af6a606a5cdc1bed39e03afe11).
Its GitHub Git-commit API response and the local Git object both name the exact
tree `f723c7ed25c3b487e7f121c95865d05049ee5258`. Publication changed commit
metadata, not source files. The API response is included in the archive.

Both Linux builds use **GNU C++ 13.3.0, Release, C++20, `-O3 -DNDEBUG`**, with
no LTO. The host reports Linux 6.18.55 x86-64, glibc 2.39 and nine logical CPUs.
The measured files still matched their recorded SHA-256 identities during the
read-only evidence packaging; each also matched its corresponding build output.
The seven archived drivers equal both the measured source revision and the
current files read during packaging. This is a post-run capture, not a claim
that the original driver recorded contemporaneous driver hashes.

Ripgrep is **15.2.0**, with runtime SSE2/SSSE3/AVX2 available. Its executable hash
and full version output are retained in provenance. The fixture's canonical
ordered path/content SHA-256 is
`d99f3d069ea6e58538a16f3080656292ac29bc4f325162ea5576ffa255e3d2dd`.

## Workload, scope and correctness

The generator writes eight ASCII/UTF-8 LF lines per file, averaging 190.34 bytes.
This surrogate isolates many tiny documents; it does not replace real-source,
large-document or million-file results. All search/update timings use the
256 MiB algorithmic memory setting. Primary builds and updates request eight
workers. Search commands retain each executable's defaults; they do not force
matching thread counts for RS and ripgrep. Full-map correctness requests use a
separate 1 GiB budget outside timing to avoid repeatedly scanning 100,000
matches in small pages.

| Case | ReSearchify query | Matching files | Occurrences or matching lines per matching file |
|---|---|---|---|
| term | `needle_42` | 1 | 2 occurrences |
| literal | `"needle_42 = 0;"` | 1 | 1 occurrence |
| broad_term | `common` | 100,000 | 2 occurrences |
| broad_regex | `/needle_[0-9]+/` | 100,000 | 2 matching lines, each with one occurrence |
| anchored_regex | `/^int needle_[0-9]+ = 0;$/` | 100,000 | 1 matching line |
| missing_term | `ReSearchify_missing_2026` | 0 | 0 |

All **24 full matching-path/count comparisons** passed: six cases, two
profiles and two executables. The same per-case path/count digest agrees across
all four combinations. Real ripgrep JSON matches independently verified snippet
line numbers, UTF-8 byte columns and text for the first 16 matching files in
path order, or all files when fewer match: **200 sampled file-result records**
across reader/profile/case checks, including repeat checks of the same paths.
The raw report preserves those snippets and full-map digests. It does not
preserve every full map, and this check is not a claim of exhaustive snippet
or score-ranked-order validation for every matching document.

Timed RS commands use `--limit 0 --snippets 0`, returning a matching-file total.
Ripgrep uses `--count-matches`, traverses complete matches, and prints a path
and count for each matching file. Regex counts agree on this fixture because
there is at most one match per matching line. The operations have equivalent
matching sets here, but **their output volume and result contracts differ**;
especially for 100,000-match queries, these are end-to-end command comparisons,
not proof that one bare matcher is faster. Ripgrep never uses `-l` early exit.

Fresh-process measurements include target startup and exclude Python-helper
startup. One explicit warmup per tool precedes 21 repetitions with recorded,
seeded shuffled baseline/current/rg order. Baseline and current use separately
built indexes from their respective writers. **OS cache is uncontrolled and
warmed by preparation; none of these measurements is cold-cache.** The
separately verified file-cache experiment is documented in
[PERFORMANCE_CACHE_FOLLOWUP.md](PERFORMANCE_CACHE_FOLLOWUP.md).

## Fresh-process search latency

Times are milliseconds. Every cell is **median / p95** from 21 samples. The
speedup column is the ratio of medians; independently paired-ratio statistics
are separately retained in the compact summary. A p95 from 21 samples is the
20th ordered observation, not a high-confidence tail estimate.

### compact

| Query | Original wall | Final wall | Original / final median | ripgrep wall |
|---|---|---|---|---|
| term | 5.495 / 6.535 | 5.067 / 5.971 | 1.08× | 98.594 / 103.578 |
| literal | 974.244 / 1148.676 | 291.136 / 364.707 | 3.35× | 93.768 / 118.464 |
| broad_term | 196.301 / 233.427 | 167.852 / 217.168 | 1.17× | 603.001 / 1010.357 |
| broad_regex | 1412.375 / 1622.468 | 504.587 / 624.681 | 2.80× | 579.665 / 724.013 |
| anchored_regex | 1253.534 / 1366.538 | 493.530 / 518.849 | 2.54× | 759.726 / 911.522 |
| missing_term | 5.101 / 7.804 | 4.716 / 5.850 | 1.08× | 92.126 / 97.990 |

### fast

| Query | Original wall | Final wall | Original / final median | ripgrep wall |
|---|---|---|---|---|
| term | 5.164 / 7.445 | 5.206 / 9.116 | 0.99× | 89.170 / 96.888 |
| literal | 78.566 / 93.670 | 10.976 / 12.301 | 7.16× | 94.604 / 112.248 |
| broad_term | 190.091 / 208.870 | 164.303 / 187.388 | 1.16× | 601.371 / 717.152 |
| broad_regex | 1422.052 / 1523.413 | 486.227 / 562.192 | 2.92× | 716.256 / 809.204 |
| anchored_regex | 1339.562 / 1432.021 | 495.372 / 560.784 | 2.70× | 619.351 / 808.530 |
| missing_term | 5.519 / 8.613 | 4.948 / 6.954 | 1.12× | 87.493 / 92.291 |

Compact literal improves **3.35×**, but its final 291.136 ms median is still
**3.10× slower** than ripgrep's 93.768 ms. Fast literal improves **7.16×**, from
78.566 to 10.976 ms, with a measured 94.604 ms ripgrep median. Broad and anchored
regex improve in both profiles in this workload; the output-volume caveat
above still applies to the comparison with ripgrep.

The **fast rare term regresses by 0.042 ms, +0.81%** in the fresh-process median
(5.164 → 5.206 ms); its p95 also increases (7.445 → 9.116 ms). This observation
is retained without claiming either a proven systematic regression or that it
is definitely noise. The resident-session regression below is a separate
measurement with a larger relative change.

### CPU and process memory

CPU times are milliseconds; RSS values are MiB. Again each cell is
**median / p95**, with all 21 raw samples retained. CPU sums work across threads
and can exceed elapsed wall time. POSIX RSS comes from isolated-child
`getrusage` and may inherit the Python process's pre-exec high-water mark.
An observed **15.000 MiB floor** is therefore not an accurate allocation or
resident-size measurement for small RS/rg processes.

#### compact

| Query | Original CPU | Final CPU | rg CPU | Original RSS | Final RSS | rg RSS |
|---|---|---|---|---|---|---|
| term | 5.271 / 6.362 | 4.884 / 5.800 | 741.655 / 777.836 | 15.000 / 15.000 | 15.000 / 15.000 | 15.000 / 15.000 |
| literal | 973.831 / 1147.060 | 510.920 / 591.673 | 692.834 / 867.523 | 56.438 / 56.496 | 49.027 / 49.090 | 15.000 / 15.000 |
| broad_term | 195.936 / 233.141 | 167.544 / 216.639 | 1646.324 / 2230.690 | 58.598 / 58.621 | 50.105 / 50.121 | 15.000 / 15.000 |
| broad_regex | 1411.766 / 1621.973 | 503.902 / 624.313 | 1633.221 / 1803.435 | 56.496 / 56.621 | 48.152 / 48.191 | 15.000 / 15.000 |
| anchored_regex | 1253.141 / 1366.177 | 492.979 / 518.516 | 1771.924 / 2046.354 | 56.605 / 56.621 | 48.762 / 48.812 | 15.000 / 15.000 |
| missing_term | 4.881 / 7.615 | 4.549 / 5.532 | 677.339 / 740.026 | 15.000 / 15.000 | 15.000 / 15.000 | 15.000 / 15.000 |

#### fast

| Query | Original CPU | Final CPU | rg CPU | Original RSS | Final RSS | rg RSS |
|---|---|---|---|---|---|---|
| term | 5.000 / 7.261 | 5.045 / 8.907 | 663.624 / 710.873 | 15.000 / 15.000 | 15.000 / 15.000 | 15.000 / 15.000 |
| literal | 78.177 / 93.325 | 10.760 / 12.082 | 703.563 / 855.160 | 27.312 / 27.371 | 15.000 / 15.000 | 15.000 / 15.000 |
| broad_term | 189.706 / 208.325 | 163.841 / 186.935 | 1617.528 / 1764.763 | 58.602 / 58.621 | 50.066 / 50.113 | 15.000 / 15.000 |
| broad_regex | 1420.607 / 1521.744 | 485.911 / 561.900 | 1754.206 / 1975.513 | 58.453 / 58.496 | 48.883 / 48.938 | 15.000 / 15.000 |
| anchored_regex | 1338.068 / 1431.535 | 494.805 / 560.454 | 1612.947 / 2007.223 | 59.227 / 59.246 | 49.621 / 49.652 | 15.000 / 15.000 |
| missing_term | 5.255 / 8.420 | 4.768 / 6.784 | 650.678 / 693.790 | 15.000 / 15.000 | 15.000 / 15.000 | 15.000 / 15.000 |

### Candidates and logical content bytes

Each entry is **candidate documents / scanned content bytes**. Values are invariant across the 21 samples. These counters do not represent physical disk I/O or all index-page traffic.

| Query | Compact original | Compact final | Fast original | Fast final |
|---|---|---|---|---|
| term | 1 / 0 | 1 / 0 | 1 / 0 | 1 / 0 |
| literal | 100,000 / 19,034,450 | 100,000 / 19,034,450 | 1,000 / 190,445 | 12 / 2,272 |
| broad_term | 100,000 / 0 | 100,000 / 0 | 100,000 / 0 | 100,000 / 0 |
| broad_regex | 100,000 / 19,034,450 | 100,000 / 19,034,450 | 100,000 / 19,034,450 | 100,000 / 19,034,450 |
| anchored_regex | 100,000 / 19,034,450 | 100,000 / 19,034,450 | 100,000 / 19,034,450 | 100,000 / 19,034,450 |
| missing_term | 0 / 0 | 0 / 0 | 0 / 0 | 0 / 0 |

The compact tiny-file literal still checks all 100,000 candidates and scans
all 19,034,450 content bytes. It must not be described as a candidate-filtering
win. These tiny documents are below the existing 512-byte compact-signature
threshold, so **both compact indexes have zero literal-sidecar bytes**. In fast,
the literal falls from 1,000 candidates / 190,445 bytes to 12 candidates /
2,272 bytes, while retaining the one actual matching file. Broad regex still
scans the complete corpus in both versions.

## Persistent JSONL sessions

For each profile both binaries read the **same immutable current-writer index**.
Two services remain resident, but requests execute strictly sequentially with
balanced alternating AB/BA order. Startup/first request and three warmups are
excluded and retained separately in raw data. There are **1,000 term pairs**
and **21 literal pairs** per profile. Client wall time runs from write/flush to
a complete response line, excluding JSON parsing. Internal engine time is wall
time, not CPU. This harness makes no per-process CPU/RSS, concurrent-throughput
or multi-agent scalability claim.

All values below are milliseconds, **median / p95**.

| Profile / query | Pairs | Original client | Final client | Original engine | Final engine |
|---|---|---|---|---|---|
| compact / term | 1000 | 0.155710 / 0.209436 | 0.123977 / 0.194723 | 0.055369 / 0.081563 | 0.039139 / 0.071137 |
| compact / literal | 21 | 920.406562 / 1043.118968 | 253.886418 / 332.087926 | 910.424840 / 1031.120747 | 246.395675 / 324.588212 |
| fast / term | 1000 | 0.120301 / 0.191079 | 0.163617 / 0.205831 | 0.038413 / 0.068023 | 0.062414 / 0.081423 |
| fast / literal | 21 | 8.994378 / 10.379118 | 1.336949 / 2.501647 | 8.755947 / 10.075600 | 1.145089 / 1.569910 |

The fast resident term median increases from **0.120301 to 0.163617 ms
(+36.0%)** at the client and from **0.038413 to 0.062414 ms (+62.5%)** inside the
engine. Every response in that series still has one candidate and zero scanned
content bytes. Its cause was not isolated. The raw series includes changes
within the measurement window; that observation neither explains nor erases
the regression. Compact resident term improves in the same protocol. Literal
sessions improve in both profiles.

## Five sequential blocks of 1,000 fresh CLI processes

These are **five separate blocks, one per tool/profile**, in the retained order
below. They are not 1,000 resident requests and not five paired statistical
replicates. Batch wall includes Python orchestration/result checks; individual
target wall excludes post-exit result checks. POSIX target CPU is the final
cumulative child CPU counter, not the sum of its intermediate cumulative
values. OS cache remains uncontrolled.

| Tool / profile | Whole block wall, s | Individual wall median / p95, ms | Target CPU total, s | Maximum RSS, MiB |
|---|---|---|---|---|
| baseline / compact | 4.723476 | 4.331 / 6.801 | 4.507771 | 15.125 |
| current / compact | 4.130128 | 3.899 / 5.286 | 3.960001 | 15.125 |
| rg / unindexed | 103.361746 | 100.430 / 124.104 | 737.753874 | 15.125 |
| current / fast | 4.222471 | 3.921 / 5.520 | 4.039385 | 15.125 |
| baseline / fast | 4.722616 | 4.324 / 6.588 | 4.446924 | 15.125 |

Final fast's whole CLI block improves from 4.723 to 4.222 seconds, while its
separate resident term series regresses. Neither result substitutes for the
other protocol. These single ordered blocks support no claim of a repeatable
batch-level p95 or identical CPU scheduling between blocks.

## Primary indexing and stored size

There is exactly **one independent create sample per writer/profile**, with
eight requested/actual workers, 16 segments and a 256 MiB memory setting. Build
time is excluded from every search number. The table retains actual execution
order and reports CLI wall, target CPU and process high-water RSS.

| Writer / profile | Wall, s | CPU, s | RSS, MiB | Committed bytes | Committed MiB | Literal sidecar bytes |
|---|---|---|---|---|---|---|
| baseline / compact | 1.814693 | 10.845883 | 40.203 | 45,097,944 | 43.009 | 0 |
| current / compact | 1.389187 | 9.415815 | 39.332 | 45,105,825 | 43.016 | 0 |
| current / fast | 4.053212 | 25.828160 | 54.020 | 129,394,348 | 123.400 | 0 |
| baseline / fast | 3.830347 | 25.597634 | 52.398 | 129,550,293 | 123.549 | 0 |

Compact create is observed at 1.815 → 1.389 seconds. Fast create is observed
at **3.830 → 4.053 seconds (+5.82%)**. With one sample each, neither is a stable
indexing speedup/regression estimate and no build p95 is claimed. Fast index
storage remains roughly 2.87 times compact storage; it is a material profile
tradeoff on this corpus.

Committed index sizes change by only +7,881 bytes (compact) and −155,945 bytes
(fast). Independent multiworker assignment and segment packing can change
those sizes; these small differences are not claimed as a compression gain.
Transaction, spill and coordination bytes are measured separately. All four
build records show zero such leftovers at the storage capture. Source files
are excluded from index sizes.

## Strict-global and supplied-path updates

The two versions begin each profile from separate copies of the **same
current-writer base index**. There are seven paired repetitions per stage,
five stages, two binaries and two profiles: **140 timed update operations**.
Each operation uses strict source validation; global scope and supplied-path
scope are compared only with their own equivalent operation. Copies, mutations,
correctness queries and restoration are outside timing. Setup/no-op warmups
are excluded. The requested count is eight workers; the final implementation
uses one worker for a supplied single path.

Full strict no-op and full strict one-edit scans hash **all 100,000 files**.
Known-path edit/add hash one; known-path delete hashes none. The supplied list
must include the complete change set, including both names of a rename. A
missing notification requires a full scan. Comparing the roughly 1-second
global operation with a roughly 6–7-millisecond targeted operation would be
a change in scope, not a speedup from the code patch.

Generations and retained commits grow through identical consecutive updates;
the seven repetitions are not independent reset snapshots. Edits retain the
original timestamp; subsequent replacements also preserve size. Strict-global
scans still detect those changes. Source bytes and timestamp are restored in
the child's `finally`, and the parent independently checks the edited file's
SHA after each profile. Mutation audit records confirm the original and final
SHA/size/mtime match. This is an edited-file restoration claim, not a new
post-run full-corpus scan.

### Update wall time

Times are milliseconds. Each cell is **median / p95**; with seven samples,
nearest-rank p95 is the maximum and is not a precise tail estimate. Faster-pair
counts compare matching before/after iterations, not ratios of medians.

| Profile / operation | Original wall | Final wall | Median change | Final faster pairs |
|---|---|---|---|---|
| compact / Full strict no-op | 1180.318 / 1222.277 | 1062.661 / 1128.189 | -10.0% | 7/7 |
| compact / Full strict, one edit | 1175.448 / 1310.177 | 1036.186 / 1069.988 | -11.8% | 7/7 |
| compact / Known-path edit | 7.493 / 7.794 | 6.623 / 7.233 | -11.6% | 6/7 |
| compact / Known-path add | 7.869 / 8.771 | 6.447 / 9.367 | -18.1% | 6/7 |
| compact / Known-path delete | 6.707 / 11.518 | 6.413 / 6.553 | -4.4% | 7/7 |
| fast / Full strict no-op | 1217.954 / 1345.919 | 1065.229 / 1224.075 | -12.5% | 6/7 |
| fast / Full strict, one edit | 1234.903 / 1315.428 | 1096.824 / 1321.760 | -11.2% | 6/7 |
| fast / Known-path edit | 8.873 / 10.265 | 6.952 / 9.751 | -21.7% | 5/7 |
| fast / Known-path add | 9.500 / 11.075 | 7.452 / 10.523 | -21.6% | 6/7 |
| fast / Known-path delete | 9.574 / 14.521 | 5.908 / 6.332 | -38.3% | 7/7 |

All ten stage medians improve in this run. However, compact known-path-add
p95 increases **8.771 → 9.367 ms**, and fast full one-edit p95 increases
**1,315.428 → 1,321.760 ms**. Those samples remain in the distributions. These
seven-pair findings do not establish Windows one-file-update latency.

### Update CPU and memory

CPU is milliseconds and RSS is MiB, **median / p95**. The same inherited
15 MiB RSS floor applies to small targeted operations.

| Profile / operation | Original CPU | Final CPU | Original RSS | Final RSS |
|---|---|---|---|---|
| compact / Full strict no-op | 5374.990 / 5561.220 | 4649.755 / 5027.251 | 46.117 / 46.414 | 46.730 / 46.824 |
| compact / Full strict, one edit | 5369.939 / 5957.309 | 4550.079 / 4670.423 | 46.289 / 46.414 | 46.715 / 46.785 |
| compact / Known-path edit | 7.289 / 8.100 | 6.469 / 7.076 | 15.000 / 15.000 | 15.000 / 15.000 |
| compact / Known-path add | 8.401 / 8.786 | 6.296 / 9.166 | 15.000 / 15.000 | 15.000 / 15.000 |
| compact / Known-path delete | 7.478 / 12.465 | 6.234 / 6.401 | 15.000 / 15.000 | 15.000 / 15.000 |
| fast / Full strict no-op | 5392.234 / 5714.278 | 4575.466 / 4844.358 | 62.164 / 62.348 | 46.641 / 46.855 |
| fast / Full strict, one edit | 5472.749 / 5700.950 | 4714.598 / 5092.447 | 62.172 / 62.477 | 48.766 / 48.988 |
| fast / Known-path edit | 15.578 / 17.993 | 6.796 / 9.561 | 15.355 / 15.965 | 15.000 / 15.000 |
| fast / Known-path add | 15.568 / 16.660 | 7.268 / 10.306 | 15.000 / 15.492 | 15.000 / 15.000 |
| fast / Known-path delete | 16.574 / 27.798 | 5.748 / 6.167 | 15.000 / 16.258 | 15.000 / 15.000 |

The fast global no-op RSS median falls from 62.164 to 46.641 MiB; compact
global no-op rises slightly, 46.117 → 46.730 MiB. The highest observed final
RSS among these search, build and update samples is **56,643,584 bytes
(54.020 MiB)**, from the fast create sample. This demonstrates the measured
run stayed below 256 MiB; it is not a hard OS RSS guarantee for arbitrary
projects or multiple agents.

The update run also passes **92 correctness records** and retains **98 mutation
audit events**. Marker-query results agree exactly between binaries, including
paths, counts, scores and snippet positions/text. Old markers disappear;
added/deleted paths and supplied two-name renames behave correctly. Four final
full-index `verify` calls pass (both tools, both profiles), each with 100,000
files. These correctness operations are excluded from update timing.

## Reproduction and limits

Use the original and tree-equivalent final source revisions above, build
Release executables with comparable compiler settings, and run the archived
`tiny_comparison.py` against a new owned directory. The complete reconstructed
invocation and exact measured child commands are in provenance/raw JSON. It
uses `--files 100000 --iterations 21 --session-iterations 1000
--fresh-processes 1000 --update-iterations 7 --index-threads 8`. The selected
search cases, warmups, samples and update scopes must stay unchanged when
comparing runs. Rebuilding promises neither identical executable SHA nor
identical multithreaded index packing; record the new identities.

For a read-only arithmetic check, extract the ZIP and run:

```sh
python3 verification/recompute.py results --output recomputed-summary.json
```

It uses only Python's standard library, reads the archived evidence and writes
one summary. It invokes no engine, ripgrep, compiler or cache control. All raw
archive entries were read back and compared byte-for-byte after packaging.

The data establishes completion and the stated measurements for this native
Linux surrogate. It does not reproduce the user's unpublished fixture or
query commands; test Unicode or non-ASCII corpus behavior here; measure
millions of files here; establish true cold-cache latency; or measure concurrent
agent throughput. The project has separate tests/reports for those applicable
areas. The observed regressions and remaining compact-literal cost are part
of the accepted evidence, not omitted exceptions.
