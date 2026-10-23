# Native performance: final core and retained checkpoint

The main tables use [run 39976554801](https://github.com/r1skme/ReSearchify/actions/runs/39976554801), containing the accepted NFA V2 implementation. Earlier runs are retained separately. These are native GitHub runner experiments on generated surrogate corpora; they do not reproduce the user's computer or unsupplied corpus.

## Final source identity

- Workflow head: `8a917c53a090b3cc3b75a9bc7e085b2c2d0adf57`.
- Actual pull-request merge checkout: `547b8da292e90b34b22a73a9e46125db52617372`.
- Equivalent local source commit: `c0522628da1300d2c01637c5276ca5628d409b5b`.
- All three complete Git trees are `be76b16b61a1e5879ad2daf050bdf54aa551bc14`.
- Original baseline: `eb858348169157686e70104af6b64f9539a39fe8`; macOS warm sessions use previous optimized `dbcddf76708b614c26e212fa27b48efa33ddfd58` as explicitly labelled.

[Final provenance](../bench/results/native-tiny-39976554801-provenance.json) contains exact artifact metadata, checksums, per-member hashes and native job outcomes. ZIP files retain the exact downloaded bytes. Paired search comparisons stay within their recorded windows; results from different runs or corpora are not pooled.

Later head `708b46f54fdfd3d571b1145e36253866dd1a1bc6` / local `78fcf5701762888349b04642a020db8f5bbf04c7` has tree `d72e3bad31a4db37a5028d90d145dec9b91fb9cb`. Its only changed path relative to the measured local commit is `scripts/external_source_benchmark.py`; native C++, CMake, benchmark generation/drivers and workflow inputs are unchanged. This input-equivalence check preserves the measured run's exact source identity rather than replacing it with the latest commit.

## Windows Server 2022 x64: 100k tiny files

The final native tiny-file job is still running. No timing or update result is inferred from its smoke artifact. The 1000 fresh CLI launches will remain separate from the 1000 warm JSONL requests.

## macOS Apple Silicon: separate 20k-file corpus

Host: `macOS-15.7.9-arm64-arm-64bit`, 3 logical CPUs; Apple clang 17.0.0 and ripgrep 15.2.0. Native executable architecture was checked. The generated corpus contains **20,000 files, 1,000,000 lines and 72,893,610 bytes** (approximately 3.6 KB/file). This is separate from the Windows 100k tiny-file surrogate.

Native CTest passed 13/13 in 13.48 s; the Python harness passed 41 tests in 0.403 s. The full path/count correctness gate passed 24 reader/profile/query comparisons.

### Fresh process search

21 paired samples per tool/query, with excluded warmups. Target wall milliseconds include startup and shutdown. OS cache is uncontrolled; these are not cold-cache measurements. RS count-only requests and complete rg per-file count output are measured as implemented; their serialization costs differ.

| Profile/query | Original median | Current median | Current p95 | rg median |
| --- | --- | --- | --- | --- |
| compact/term | 5.185 | 5.503 | 13.528 | 136.162 |
| compact/literal | 843.156 | 88.149 | 105.305 | 151.186 |
| compact/no_match | 964.372 | 103.862 | 174.988 | 214.126 |
| compact/broad_literal | 1007.394 | 353.967 | 458.793 | 475.270 |
| compact/broad_regex | 2637.059 | 608.953 | 795.228 | 422.044 |
| fast/term | 7.197 | 7.561 | 11.954 | 186.664 |
| fast/literal | 26.001 | 8.243 | 13.804 | 165.355 |
| fast/no_match | 6.491 | 7.274 | 9.633 | 166.260 |

Compact selective literal is 9.57× faster than the original and 1.72× faster than rg by median within this paired window.

Fast selective literal is 3.15× faster than the original and 20.06× faster than rg by median within this paired window.

Broad regex remains slower than rg on this generated fixture. This native workload does not isolate the prefix-free NFA first-set optimization; its regex has a literal prefix. Speedups from different runs are not multiplied.

Observed median regressions versus the original: compact/term 5.185 → 5.503 ms (6.1% higher); fast/term 7.197 → 7.561 ms (5.0% higher); fast/no_match 6.491 → 7.274 ms (12.1% higher). The raw distributions are retained; this shared-runner experiment does not isolate their causes.

### CPU and memory

CPU values are target-process median milliseconds; memory is the maximum observed target peak RSS MiB across the 21 samples. POSIX RSS may include the Python pre-exec floor. These are not simultaneous whole-service memory measurements.

| Profile/query | Original CPU | Current CPU | rg CPU | Original max RSS MiB | Current max RSS MiB | rg max RSS MiB |
| --- | --- | --- | --- | --- | --- | --- |
| compact/term | 4.120 | 4.125 | 361.985 | 2.53 | 2.36 | 13.42 |
| compact/literal | 824.383 | 86.040 | 355.910 | 15.02 | 22.80 | 12.25 |
| compact/no_match | 936.230 | 99.940 | 558.893 | 15.02 | 22.61 | 12.67 |
| compact/broad_literal | 998.635 | 544.051 | 1127.596 | 15.02 | 23.14 | 13.27 |
| compact/broad_regex | 2589.549 | 587.453 | 1142.915 | 15.06 | 22.64 | 13.80 |
| fast/term | 4.771 | 4.685 | 475.276 | 2.06 | 2.38 | 12.11 |
| fast/literal | 24.237 | 6.734 | 436.620 | 4.45 | 3.05 | 11.58 |
| fast/no_match | 4.659 | 4.692 | 450.426 | 2.59 | 2.41 | 12.31 |

### Separate index construction

One descriptive sample per writer/profile, 3 workers, 256 MiB configured budget. These are not repeated build medians or p95 estimates. Index bytes are committed logical bytes excluding coordination and scratch.

| Writer/profile | Wall s | CPU s | Peak RSS MiB | Committed index bytes | Literal sidecar bytes |
| --- | --- | --- | --- | --- | --- |
| original/compact | 5.034 | 10.521 | 22.69 | 100,485,432 | 0 |
| current/compact | 3.787 | 9.670 | 22.53 | 103,767,220 | 3,283,584 |
| current/fast | 7.123 | 14.763 | 31.73 | 140,459,963 | 0 |
| original/fast | 8.113 | 19.332 | 31.53 | 140,455,437 | 0 |

Compact committed index growth is 3,281,788 bytes (3.27%), principally the content signature sidecar. The tiny Windows fixture remains a distinct size case.

### Warm JSONL

This session comparison uses the previous optimized `dbcddf7` reader and current on the same current index. It does not use the original baseline. Term has 201 exchanges; literal has 21; three warmups are excluded. Engine milliseconds are internal wall time, not CPU. No per-reader CPU/RSS or 1000-request result is claimed for these macOS sessions.

| Profile/query | N | Previous client median ms | Current client median ms | Current client p95 ms | Previous engine median ms | Current engine median ms |
| --- | --- | --- | --- | --- | --- | --- |
| compact/term | 201 | 0.064958 | 0.063958 | 0.091209 | 0.013791 | 0.013083 |
| compact/literal | 21 | 26.329875 | 16.159583 | 25.309666 | 25.521542 | 15.275250 |
| fast/term | 201 | 0.067333 | 0.066292 | 0.184417 | 0.013542 | 0.012958 |
| fast/literal | 21 | 0.530708 | 0.491000 | 0.616708 | 0.442708 | 0.405584 |

The full path/count checks cover original/current own indexes and the previous reader on the current index. This macOS driver does not include the Windows update workflow or independent snippet checks; neither is claimed here.

[Exact ZIP](../bench/results/native-tiny-39976554801-macos-arm64.zip), SHA256 `055ba8a8a7683d248d63d795e1e10d7df649a149cae7e089e5e25770e0c46b19`; [unchanged summary](../bench/results/native-tiny-39976554801-macos-arm64-summary.json); [native validation excerpts](../bench/results/native-tiny-39976554801-macos-arm64-validation.json).

## macOS Intel: separate 20k-file corpus

Host: `macOS-15.7.9-x86_64-i386-64bit`, 4 logical CPUs; Apple clang 17.0.0 and ripgrep 15.2.0. Native executable architecture was checked. The generated corpus contains **20,000 files, 1,000,000 lines and 72,893,610 bytes** (approximately 3.6 KB/file). This is separate from the Windows 100k tiny-file surrogate.

Native CTest passed 13/13 in 30.92 s; the Python harness passed 41 tests in 1.199 s. The full path/count correctness gate passed 24 reader/profile/query comparisons.

### Fresh process search

21 paired samples per tool/query, with excluded warmups. Target wall milliseconds include startup and shutdown. OS cache is uncontrolled; these are not cold-cache measurements. RS count-only requests and complete rg per-file count output are measured as implemented; their serialization costs differ.

| Profile/query | Original median | Current median | Current p95 | rg median |
| --- | --- | --- | --- | --- |
| compact/term | 15.971 | 17.163 | 77.495 | 203.825 |
| compact/literal | 1492.934 | 251.142 | 474.208 | 238.975 |
| compact/no_match | 1613.648 | 250.891 | 369.209 | 223.755 |
| compact/broad_literal | 1222.219 | 369.385 | 436.343 | 597.571 |
| compact/broad_regex | 3045.724 | 680.810 | 877.617 | 502.010 |
| fast/term | 12.202 | 13.272 | 56.844 | 234.716 |
| fast/literal | 40.056 | 15.036 | 18.773 | 208.075 |
| fast/no_match | 11.892 | 12.219 | 19.646 | 226.898 |

Compact selective literal is 5.94× faster than the original and 5.1% slower than rg by median within this paired window.

Fast selective literal is 2.66× faster than the original and 13.84× faster than rg by median within this paired window.

Broad regex remains slower than rg on this generated fixture. This native workload does not isolate the prefix-free NFA first-set optimization; its regex has a literal prefix. Speedups from different runs are not multiplied.

Observed median regressions versus the original: compact/term 15.971 → 17.163 ms (7.5% higher); fast/term 12.202 → 13.272 ms (8.8% higher); fast/no_match 11.892 → 12.219 ms (2.8% higher). The raw distributions are retained; this shared-runner experiment does not isolate their causes.

### CPU and memory

CPU values are target-process median milliseconds; memory is the maximum observed target peak RSS MiB across the 21 samples. POSIX RSS may include the Python pre-exec floor. These are not simultaneous whole-service memory measurements.

| Profile/query | Original CPU | Current CPU | rg CPU | Original max RSS MiB | Current max RSS MiB | rg max RSS MiB |
| --- | --- | --- | --- | --- | --- | --- |
| compact/term | 11.534 | 12.131 | 728.781 | 1.30 | 1.34 | 13.11 |
| compact/literal | 1480.637 | 243.092 | 804.517 | 14.01 | 21.68 | 16.07 |
| compact/no_match | 1505.028 | 243.934 | 736.169 | 14.02 | 21.52 | 12.56 |
| compact/broad_literal | 1217.416 | 647.576 | 1964.977 | 14.02 | 21.98 | 16.02 |
| compact/broad_regex | 3040.116 | 676.968 | 1606.172 | 14.04 | 21.59 | 13.01 |
| fast/term | 9.765 | 10.301 | 808.730 | 1.39 | 1.38 | 14.99 |
| fast/literal | 37.731 | 12.714 | 732.187 | 3.28 | 1.86 | 12.75 |
| fast/no_match | 9.527 | 9.454 | 784.588 | 1.31 | 1.36 | 15.61 |

### Separate index construction

One descriptive sample per writer/profile, 3 workers, 256 MiB configured budget. These are not repeated build medians or p95 estimates. Index bytes are committed logical bytes excluding coordination and scratch.

| Writer/profile | Wall s | CPU s | Peak RSS MiB | Committed index bytes | Literal sidecar bytes |
| --- | --- | --- | --- | --- | --- |
| original/compact | 9.371 | 17.756 | 13.05 | 100,481,232 | 0 |
| current/compact | 7.765 | 16.644 | 13.97 | 103,762,661 | 3,283,494 |
| current/fast | 18.081 | 34.535 | 18.56 | 140,437,402 | 0 |
| original/fast | 17.928 | 35.419 | 19.19 | 140,458,372 | 0 |

Compact committed index growth is 3,281,429 bytes (3.27%), principally the content signature sidecar. The tiny Windows fixture remains a distinct size case.

Fast create is slower in this single sample (17.928 → 18.081 s); it is retained as an observed cost, not treated as a stable repeated-build regression.

### Warm JSONL

This session comparison uses the previous optimized `dbcddf7` reader and current on the same current index. It does not use the original baseline. Term has 201 exchanges; literal has 21; three warmups are excluded. Engine milliseconds are internal wall time, not CPU. No per-reader CPU/RSS or 1000-request result is claimed for these macOS sessions.

| Profile/query | N | Previous client median ms | Current client median ms | Current client p95 ms | Previous engine median ms | Current engine median ms |
| --- | --- | --- | --- | --- | --- | --- |
| compact/term | 201 | 0.237620 | 0.235061 | 0.301799 | 0.037195 | 0.033459 |
| compact/literal | 21 | 63.360810 | 36.543096 | 40.590459 | 61.446806 | 34.362956 |
| fast/term | 201 | 0.250222 | 0.243860 | 0.312335 | 0.038856 | 0.035142 |
| fast/literal | 21 | 1.013134 | 0.966758 | 1.371901 | 0.744532 | 0.697186 |

The full path/count checks cover original/current own indexes and the previous reader on the current index. This macOS driver does not include the Windows update workflow or independent snippet checks; neither is claimed here.

[Exact ZIP](../bench/results/native-tiny-39976554801-macos-x64.zip), SHA256 `0caf1196605f1d2d7e9a2108af52621aaabea673e3badc3ac0b492dcdb1eec5b`; [unchanged summary](../bench/results/native-tiny-39976554801-macos-x64-summary.json); [native validation excerpts](../bench/results/native-tiny-39976554801-macos-x64-validation.json).

## Earlier final-core repeat and interpretation limits

The [Apple Silicon archive from run 39975881618](../bench/results/native-tiny-39975881618-macos-arm64.zip) and [Intel archive](../bench/results/native-tiny-39975881618-macos-x64.zip) are retained as a separate final-core repeat, with [ARM summary](../bench/results/native-tiny-39975881618-macos-arm64-summary.json), [Intel summary](../bench/results/native-tiny-39975881618-macos-x64-summary.json) and [provenance](../bench/results/native-tiny-39975881618-provenance.json). Its head `5a358ec4`, merge checkout `db43130c` and local `361aac6` share tree `241109e7b76993c1c1a75556a2a10755d790ee9d`. Both current native binary SHA256 values exactly match the latest run on their respective architectures. No cross-run timing ratio is presented as a controlled NFA speedup.

Fresh-query tables preserve baseline/current/rg from the same workload and comparison window; persistent-session baselines and sample counts are labelled separately. Fresh process startup, OS file-cache state, index construction and persistent service exchanges are distinct measurements. Shared-runner p95 and one-shot create samples are not hardware guarantees. Min/max and full raw samples remain inside each ZIP.

The generated regex cases have one match per matching line, so rg occurrence counts and RS matching-line counts agree on these fixtures. Full per-file checks validate that agreement; this does not establish general Unicode or regex count equivalence for arbitrary repositories. JSONL internal engine time is wall time, not CPU. These runs make no whole-machine cold-cache claim.

---

## Native checkpoint: tiny Windows and separate macOS corpus

This report preserves native GitHub Actions evidence from [run 39972113138](https://github.com/r1skme/ReSearchify/actions/runs/39972113138). It is an intermediate checkpoint before the subsequent NFA first-set experiment. It does not reproduce the user's computer or unsupplied source corpus.

### Source and artifact identity

- Workflow head: `d71cbea1e6c71a9e3f07a4fed0f4d19fedd8f3b5`.
- Actual pull-request merge checkout: `0babf2521230df09791fd2e61ab04dc90af53854`.
- Equivalent local source commit: `456a5cf88c15330eaf8ae8c752e351e09e0f5ceb`.
- All three have exactly the same Git tree: `a017f6c242fb0d71c13a4efc422c9de09d68b28f`.
- Original baseline: `eb858348169157686e70104af6b64f9539a39fe8`; additional macOS previous-checkpoint comparisons use `dbcddf76708b614c26e212fa27b48efa33ddfd58`.

[Machine-readable provenance and per-member SHA256](../bench/results/native-tiny-39972113138-provenance.json) records the run, jobs, artifact identities, verified ZIP digests and source-tree equality. ZIPs are the exact downloaded bytes, not repacked archives. Full raw samples and commands remain inside them.

### Windows Server 2022 x64: 100k tiny files

Native host `Windows-2022Server-10.0.20348-SP0`, 4 logical CPUs; MSVC 19.44.35229.0, Release/static runtime, native PE x86-64 architecture checked, ripgrep 15.2.0. This is **Windows Server 2022 on a GitHub runner**, not the user's Windows 11 PC.

The generated surrogate has **100,000 files, 800,000 lines and 19,034,450 bytes**, about 190 bytes per file. Its path/content digest is `d99f3d069ea6e58538a16f3080656292ac29bc4f325162ea5576ffa255e3d2dd`. No equivalence to the unsupplied user corpus is assumed.

Current binary SHA256 is `2439da5da735b1bfa5ec94da0d8fba117e91188557cc1eb9c4c6c0fa13420840`; original binary SHA256 is `316159425a65a0f837f3cfb8e5bd2f0d38a6e3c4368b22e52335d40b0f884cb0`. The artifact records actual checkout `0babf2521230df09791fd2e61ab04dc90af53854`.

Native CTest passed 13/13 in 45.80 s; Python harness tests passed 28 in 0.570 s. The separate search oracle passed **24/24 comparisons**: two readers, two profiles and six cases. It compared every matching path and per-file count with rg, then rg JSON line/UTF-8-byte-column/text for the first 16 matching files per case. These ASCII/LF fixture checks do not independently establish all Unicode/BOM/CR edge cases.

**This complete workflow did not pass:** the search and session reports below completed, but the enclosing 70-minute step timed out during the fast-profile update workflow. The unchanged summary retains `complete: false`. Completed stage evidence is reported without promoting the interrupted suite to a pass.

#### Fresh process search: 21 paired samples

Wall values are median / current p95 milliseconds, measured over complete child lifetimes with excluded warmups, shuffled pairing and uncontrolled OS cache. They are not cold-cache samples. RS count-only output and rg complete per-file counts have different serialization costs. Index building is excluded and reported separately.

| Profile/query | Original median | Current median | Current p95 | rg median |
| --- | --- | --- | --- | --- |
| compact/term | 10.965 | 10.909 | 11.161 | 1831.045 |
| compact/literal | 2784.176 | 512.932 | 718.882 | 1827.690 |
| compact/broad_term | 388.923 | 367.376 | 445.459 | 3593.604 |
| compact/broad_regex | 3291.108 | 692.827 | 775.593 | 3598.903 |
| compact/anchored_regex | 3312.899 | 711.944 | 783.534 | 3575.850 |
| compact/missing_term | 11.033 | 10.918 | 11.466 | 1816.433 |
| fast/term | 11.194 | 11.025 | 11.318 | 1823.224 |
| fast/literal | 168.786 | 23.195 | 24.441 | 1824.896 |
| fast/broad_term | 394.341 | 371.664 | 468.026 | 3609.858 |
| fast/broad_regex | 3404.245 | 697.379 | 806.062 | 3596.158 |
| fast/anchored_regex | 3412.231 | 716.396 | 795.327 | 3572.980 |
| fast/missing_term | 11.151 | 10.951 | 11.718 | 1831.735 |

Compact literal is 5.43x faster than the original and 3.56x faster than rg in this window.

Fast literal is 7.28x faster than the original and 78.67x faster than rg in this window.

The cases are `needle_42`, `common`, `"needle_42 = 0;"`, `/needle_[0-9]+/`, `/^int needle_[0-9]+ = 0;$/`, and missing exact term `ReSearchify_missing_2026`. Anchored and unanchored regex are separate rows.

#### CPU, memory and content work

CPU is median target-process milliseconds; RSS is maximum observed target peak MiB across 21 samples. Windows process counters are used. Process CPU values may be coarse for the shortest queries; no CPU values are inferred from wall time.

| Profile/query | Original CPU ms | Current CPU ms | rg CPU ms | Original max RSS MiB | Current max RSS MiB | rg max RSS MiB |
| --- | --- | --- | --- | --- | --- | --- |
| compact/term | 15.625 | 15.625 | 46.875 | 4.543 | 4.602 | 13.316 |
| compact/literal | 2765.625 | 843.750 | 31.250 | 58.621 | 52.301 | 13.301 |
| compact/broad_term | 390.625 | 375.000 | 31.250 | 61.086 | 53.082 | 13.309 |
| compact/broad_regex | 3281.250 | 687.500 | 31.250 | 58.617 | 50.793 | 13.301 |
| compact/anchored_regex | 3296.875 | 718.750 | 31.250 | 58.699 | 51.316 | 13.320 |
| compact/missing_term | 15.625 | 15.625 | 31.250 | 4.594 | 4.648 | 13.293 |
| fast/term | 15.625 | 15.625 | 31.250 | 4.531 | 4.598 | 13.297 |
| fast/literal | 171.875 | 15.625 | 31.250 | 29.320 | 6.852 | 13.297 |
| fast/broad_term | 390.625 | 375.000 | 31.250 | 60.980 | 52.832 | 13.297 |
| fast/broad_regex | 3406.250 | 703.125 | 31.250 | 60.750 | 51.207 | 13.316 |
| fast/anchored_regex | 3406.250 | 718.750 | 31.250 | 61.387 | 52.102 | 13.301 |
| fast/missing_term | 15.625 | 15.625 | 31.250 | 4.543 | 4.613 | 13.297 |

| Profile/query | Original candidates / content bytes | Current candidates / content bytes |
| --- | --- | --- |
| compact/term | 1 / 0 | 1 / 0 |
| compact/literal | 100,000 / 19,034,450 | 100,000 / 19,034,450 |
| compact/broad_term | 100,000 / 0 | 100,000 / 0 |
| compact/broad_regex | 100,000 / 19,034,450 | 100,000 / 19,034,450 |
| compact/anchored_regex | 100,000 / 19,034,450 | 100,000 / 19,034,450 |
| compact/missing_term | 0 / 0 | 0 / 0 |
| fast/term | 1 / 0 | 1 / 0 |
| fast/literal | 1,000 / 190,445 | 12 / 2,272 |
| fast/broad_term | 100,000 / 0 | 100,000 / 0 |
| fast/broad_regex | 100,000 / 19,034,450 | 100,000 / 19,034,450 |
| fast/anchored_regex | 100,000 / 19,034,450 | 100,000 / 19,034,450 |
| fast/missing_term | 0 / 0 | 0 / 0 |

These engine content counters do not represent total metadata reads or physical-device bytes. In this tiny compact fixture the literal still examines all files and reads all 19 MB; its gain comes from reduced processing/data-access cost. The compact signature sidecar deliberately omits these sub-512-byte documents, so no added sidecar size or signature-filter benefit is claimed here.

#### 1000 fresh CLI processes

Each row is one sequential block of 1000 rare-term searches. It is separate from the 21-pair experiment and from warm JSONL. Batch wall includes Python orchestration and post-exit result checks; individual process wall excludes those checks but includes target startup. The five blocks were not interleaved as paired samples; no precise causal ratio is inferred between the blocks.

| Tool/profile | N | Batch wall s | Individual median ms | Individual p95 ms | Summed target CPU s | Max target RSS MiB |
| --- | --- | --- | --- | --- | --- | --- |
| baseline/compact | 1000 | 10.038 | 9.719 | 10.296 | 8.828 | 4.543 |
| current/compact | 1000 | 9.852 | 9.550 | 10.062 | 8.891 | 4.598 |
| rg/unindexed | 1000 | 1946.536 | 1851.256 | 2149.169 | 35.469 | 13.312 |
| current/fast | 1000 | 10.222 | 9.910 | 10.546 | 9.125 | 4.605 |
| baseline/fast | 1000 | 10.468 | 10.127 | 10.908 | 9.234 | 4.539 |

#### Warm JSONL: 1000 term requests, 21 literal requests

Both original and current readers remain open on the same current-writer index. Requests execute sequentially in balanced alternating pair order; three warmups and the first request are excluded. Unlike the macOS session experiment, this baseline is the original `eb858348` reader. Client and internal engine values are wall milliseconds, not CPU. This session harness validates exact count-only response fields on every exchange and makes no independent per-reader RSS/CPU claim.

| Profile/query | N | Original client median | Current client median | Current client p95 | Original engine median | Current engine median |
| --- | --- | --- | --- | --- | --- | --- |
| compact/term | 1000 | 0.262800 | 0.265900 | 0.292800 | 0.118000 | 0.118900 |
| compact/literal | 21 | 2561.173800 | 325.219500 | 327.561600 | 2553.749700 | 321.141700 |
| fast/term | 1000 | 0.262100 | 0.264900 | 0.296400 | 0.117400 | 0.118500 |
| fast/literal | 21 | 26.456900 | 2.575600 | 2.716700 | 26.204900 | 2.291000 |

#### Separate construction and storage

One descriptive create sample per writer/profile, 4 workers, 256 MiB configured budget. The full search oracle uses 1024 MiB outside timing; timed search and index operations use 256 MiB. These single build samples are not median/p95 estimates. Committed bytes exclude scratch and coordination.

| Writer/profile | Wall s | CPU s | Peak RSS MiB | Committed index bytes | Literal sidecar bytes |
| --- | --- | --- | --- | --- | --- |
| baseline/compact | 19.701 | 67.828 | 16.969 | 44,755,997 | 0 |
| current/compact | 19.818 | 65.906 | 16.379 | 44,752,816 | 0 |
| current/fast | 26.331 | 95.953 | 23.438 | 125,806,121 | 0 |
| baseline/fast | 26.676 | 97.938 | 22.883 | 125,817,596 | 0 |

Compact current index difference: -3,181 bytes (-0.007%). Small byte differences between separately built indexes also depend on segment layout; they are not presented as a new compression guarantee.

The compact build was slower in its single sample (19.701 to 19.818 s); that result is retained without a repeated-build claim.

Fast current index difference: -11,475 bytes (-0.009%). Small byte differences between separately built indexes also depend on segment layout; they are not presented as a new compression guarantee.

#### Strict and targeted updates

Both readers begin with separate copies of the same immutable current-writer index. Global strict no-op, global one-file edit, and known-path edit/add/delete are distinct scopes; their speedups are not compared across scopes. Seven pairs per stage are intended. Generations and segments accumulate across edits, so observations are not independently reset snapshots.

Compact: `complete: true`, 70 completed timed operations, 46 post-operation checks, `source_restored: true`.

| Stage | Original median ms | Current median ms | Current p95 ms | Original CPU ms | Current CPU ms | Current max RSS MiB |
| --- | --- | --- | --- | --- | --- | --- |
| strict_noop | 9725.821 | 9525.629 | 9613.800 | 33453.125 | 32781.250 | 51.930 |
| global_one_edit | 9752.018 | 9534.324 | 9613.422 | 33562.500 | 32640.625 | 51.828 |
| targeted_one_edit | 13.777 | 13.216 | 13.441 | 15.625 | 15.625 | 4.180 |
| targeted_one_add | 13.382 | 13.206 | 13.394 | 15.625 | 15.625 | 4.172 |
| targeted_one_delete | 12.364 | 11.674 | 12.320 | 15.625 | 0.000 | 4.109 |

Checks include matching marker results, exact scores/snippets, targeted additions/removals, rename handling and final verification. The parent driver independently checked the restored source hash. Seven-sample p95 equals the maximum and is not a stable tail estimate.

Fast: `complete: false`, 13 completed timed operations, 0 post-operation checks, `source_restored: true`.

Partial operation counts: `{'strict_noop': 13}`. No completed-stage update performance table is inferred from this interrupted series; remaining mutation, rename and verification checks did not complete.

The job log explicitly records a 70-minute Actions step timeout at `2026-10-23T15:57:19Z`. All five 1000-process blocks and both paired search/session reports had already finished. The 1000-rg block consumed 1946.536 s (32.44 minutes). The interruption occurred in the last fast-profile strict no-op pair, after 13 normal operations of about 9.5–9.7 s each. This evidence identifies an overall harness wall-time limit, not a failed search assertion or a demonstrated engine deadlock.

[Exact unchanged ZIP](../bench/results/native-tiny-39972113138-windows-x64.zip), SHA256 `27d9ed2b48b3d6ecdde50af512630f05bd31bfe03142c19b9c3e072d954ff839`; [unchanged summary](../bench/results/native-tiny-39972113138-windows-x64-summary.json); [native validation and timeout excerpts](../bench/results/native-tiny-39972113138-windows-x64-validation.json). All min/max and raw samples remain in the ZIP.

### macOS Apple Silicon: separate 20k-file corpus

Host: macOS 15.7.9 arm64, 3 logical CPUs; Apple clang 17.0.0; ripgrep 15.2.0. Native executable architecture was checked. The fixture contains **20,000 files, 1,000,000 lines and 72,893,610 bytes**, approximately 3.6 KB/file. It is a different fixture from the Windows 100k tiny-file experiment and its results must not be merged with that corpus.

21 paired fresh-process samples per tool/query follow excluded warmups. OS cache is uncontrolled; these are not cold-cache results. Startup/shutdown is included. RS returns matching-file totals while rg performs complete scans and emits per-file counts. Index construction is measured separately. Native tests passed 13/13 in 19.49 s; the Python harness passed 28 tests in 0.442 s.

#### Fresh search: original baseline / current / ripgrep

All values below are target wall milliseconds. The original/current/rg medians are from the same paired comparison window.

| Profile/query | Original median | Current median | Current p95 | rg median |
|---|---:|---:|---:|---:|
| compact/term | 5.965 | 5.516 | 11.355 | 165.608 |
| compact/literal | 965.301 | 91.722 | 111.035 | 173.444 |
| compact/no_match | 1017.939 | 116.511 | 143.915 | 197.554 |
| compact/broad_literal | 968.255 | 304.295 | 376.707 | 407.124 |
| compact/broad_regex | 2385.025 | 488.461 | 563.225 | 351.504 |
| fast/term | 5.873 | 6.151 | 10.876 | 145.439 |
| fast/literal | 23.167 | 8.962 | 15.020 | 135.491 |
| fast/no_match | 6.442 | 7.126 | 13.964 | 128.497 |

Current compact selective literal reads 7,148 content bytes versus 72,893,610 before, while still examining 20,000 metadata candidates. Compact no-match reads zero content bytes. The fast literal has its own trigram filtering path. Broad regex remains slower than rg at this checkpoint despite improving substantially over the original implementation.

The fast fresh-process term median rises from 5.873 to 6.151 ms and fast no-match rises from 6.442 to 7.126 ms. Their CPU medians decrease, but the wall differences are retained as observed regressions; this run does not isolate their cause. Comparisons against the earlier native run are not treated as controlled speedup measurements.

#### Target CPU and memory

CPU medians are milliseconds; RSS is each target's maximum observed high-water MiB across its 21 samples. POSIX counters may include the inherited pre-exec Python floor. They are not simultaneous total service memory.

| Profile/query | Original CPU | Current CPU | rg CPU | Original max RSS MiB | Current max RSS MiB | rg max RSS MiB |
|---|---:|---:|---:|---:|---:|---:|
| compact/term | 4.295 | 4.092 | 414.793 | 2.33 | 2.56 | 12.91 |
| compact/literal | 940.591 | 86.249 | 451.253 | 15.03 | 22.78 | 13.30 |
| compact/no_match | 1006.428 | 113.563 | 526.103 | 15.03 | 22.59 | 12.52 |
| compact/broad_literal | 928.018 | 501.310 | 1064.886 | 15.02 | 23.09 | 12.50 |
| compact/broad_regex | 2362.058 | 482.582 | 936.535 | 15.06 | 22.64 | 12.36 |
| fast/term | 4.315 | 4.152 | 373.363 | 2.50 | 2.39 | 11.92 |
| fast/literal | 21.444 | 6.157 | 335.938 | 4.45 | 2.98 | 12.23 |
| fast/no_match | 4.385 | 4.197 | 329.135 | 2.34 | 2.41 | 11.72 |

#### Separate index construction

Each entry is one descriptive create sample with 3 workers and a 256 MiB configured memory budget. These are not repeated build medians or stable p95 estimates. Committed logical bytes exclude coordination and scratch.

| Writer/profile | Wall s | CPU s | Peak RSS MiB | Committed index bytes | Literal sidecar bytes |
|---|---:|---:|---:|---:|---:|
| original/compact | 3.553 | 7.783 | 23.47 | 100,485,913 | 0 |
| current/compact | 3.911 | 9.394 | 23.94 | 103,770,479 | 3,283,573 |
| current/fast | 5.908 | 15.227 | 33.95 | 140,470,819 | 0 |
| original/fast | 6.999 | 16.856 | 31.66 | 140,461,057 | 0 |

Compact create is slower in this single sample (3.553 → 3.911 s), and its committed index grows from 100,485,913 to 103,770,479 bytes, principally the literal sidecar. Fast create is 6.999 → 5.908 s and its index is essentially unchanged. These costs are not hidden inside the search tables.

#### Warm JSONL: previous optimized checkpoint versus current

These use the same current-writer immutable index and compare `dbcddf7` with current, not the original `eb85834`. Term has 201 exchanges; literal has 21; three excluded warmups precede measurement. Engine time is internal wall time, not CPU. No per-reader CPU/RSS or 1000-request claim is made for these macOS sessions.

| Profile/query | N | Previous client median ms | Current client median ms | Current client p95 ms | Previous engine median ms | Current engine median ms |
|---|---:|---:|---:|---:|---:|---:|
| compact/term | 201 | 0.064833 | 0.064083 | 0.087292 | 0.013709 | 0.013042 |
| compact/literal | 21 | 30.478333 | 17.025792 | 25.112833 | 29.333084 | 16.037958 |
| fast/term | 201 | 0.068917 | 0.068292 | 0.130458 | 0.013666 | 0.013208 |
| fast/literal | 21 | 0.492875 | 0.462416 | 0.561208 | 0.418916 | 0.399625 |

24 complete per-file path/count comparisons passed across the reader/profile/query combinations recorded in `correctness.json`. They cover original/current own indexes and the previous reader on the current index. The macOS driver does not perform the Windows tiny driver's update workflow or independent snippet check; those are not claimed here.

[Exact Apple Silicon ZIP](../bench/results/native-tiny-39972113138-macos-arm64.zip), SHA256 `da2c915db14c8c00ae1a74763df4102e958db40f17716ad1e0615b5d79ab8ea0`; [unchanged extracted summary](../bench/results/native-tiny-39972113138-macos-arm64-summary.json).

### macOS Intel: separate 20k-file corpus

Host: macOS 15.7.9 x86-64, 4 logical CPUs; Apple clang 17.0.0; ripgrep 15.2.0. The same generated 20,000-file / 1,000,000-line / 72,893,610-byte corpus shape is used here. Measurements stay within this runner and its paired windows; they are not pooled with ARM or Windows. Native tests passed 13/13 in 28.45 s, and the Python harness passed 28 tests in 0.658 s.

#### Fresh search: original baseline / current / ripgrep

21 paired samples; target wall milliseconds.

| Profile/query | Original median | Current median | Current p95 | rg median |
|---|---:|---:|---:|---:|
| compact/term | 15.403 | 14.794 | 18.420 | 290.361 |
| compact/literal | 1489.845 | 240.289 | 424.441 | 272.992 |
| compact/no_match | 1269.593 | 204.187 | 282.859 | 235.007 |
| compact/broad_literal | 1580.489 | 447.878 | 522.676 | 763.301 |
| compact/broad_regex | 4933.240 | 1033.745 | 1155.355 | 751.351 |
| fast/term | 16.916 | 17.351 | 18.943 | 332.290 |
| fast/literal | 67.701 | 24.964 | 29.779 | 339.353 |
| fast/no_match | 14.176 | 14.158 | 19.863 | 281.619 |

Both literal profiles beat rg by median in this paired window. Broad regex is still slower than rg. The fast rare-term median rises from 16.916 to 17.351 ms; no general absence of regressions is claimed.

#### Target CPU and memory

CPU median milliseconds and maximum target RSS MiB across the 21 samples.

| Profile/query | Original CPU | Current CPU | rg CPU | Original max RSS MiB | Current max RSS MiB | rg max RSS MiB |
|---|---:|---:|---:|---:|---:|---:|
| compact/term | 12.182 | 11.423 | 1070.247 | 1.23 | 1.26 | 15.79 |
| compact/literal | 1485.852 | 236.419 | 974.468 | 14.04 | 21.68 | 13.62 |
| compact/no_match | 1245.579 | 200.683 | 834.799 | 14.01 | 21.37 | 14.57 |
| compact/broad_literal | 1571.938 | 812.995 | 2727.645 | 14.03 | 21.99 | 15.62 |
| compact/broad_regex | 4928.152 | 1028.933 | 2704.988 | 14.03 | 21.54 | 14.15 |
| fast/term | 13.898 | 14.043 | 1189.521 | 1.31 | 1.27 | 13.95 |
| fast/literal | 64.053 | 21.800 | 1236.864 | 3.36 | 1.83 | 15.86 |
| fast/no_match | 11.650 | 11.349 | 1024.286 | 1.23 | 1.28 | 15.45 |

#### Separate index construction

One sample per writer/profile, 3 workers and 256 MiB configured budget. No repeated-build timing claim.

| Writer/profile | Wall s | CPU s | Peak RSS MiB | Committed index bytes | Literal sidecar bytes |
|---|---:|---:|---:|---:|---:|
| original/compact | 16.306 | 23.649 | 13.18 | 100,478,489 | 0 |
| current/compact | 13.701 | 23.304 | 13.02 | 103,765,431 | 3,283,461 |
| current/fast | 28.701 | 47.807 | 18.84 | 140,435,773 | 0 |
| original/fast | 30.344 | 51.854 | 18.71 | 140,451,672 | 0 |

#### Warm JSONL: previous optimized checkpoint versus current

The comparison uses `dbcddf7`, the same current index, 201 term exchanges or 21 literal exchanges and three excluded warmups. It does not measure 1000 fresh launches.

| Profile/query | N | Previous client median ms | Current client median ms | Current client p95 ms | Previous engine median ms | Current engine median ms |
|---|---:|---:|---:|---:|---:|---:|
| compact/term | 201 | 0.378150 | 0.375038 | 0.559560 | 0.062741 | 0.056806 |
| compact/literal | 21 | 86.039450 | 45.629450 | 54.019955 | 84.063639 | 43.704688 |
| fast/term | 201 | 0.293568 | 0.291519 | 0.403481 | 0.042290 | 0.038057 |
| fast/literal | 21 | 1.411568 | 1.360490 | 1.671727 | 1.057507 | 0.959633 |

24 full per-file path/count comparisons passed. The archive preserves all samples, additional same-index comparisons and per-query candidates/bytes. This macOS workload does not contain the Windows update experiment.

[Exact Intel ZIP](../bench/results/native-tiny-39972113138-macos-x64.zip), SHA256 `a33a42e8468fc94475d2473962aa1313ff8ad4f86bf2dacc025f74e58e78b49a`; [unchanged extracted summary](../bench/results/native-tiny-39972113138-macos-x64-summary.json).

### Interpretation limits

Fresh-query tables retain the original baseline, current build and rg from the same workload/window; session comparisons explicitly name their different baseline. The runner measurements are not measurements on the user's Windows 11 PC or Mac mini. Warm OS cache, process startup, index construction and persistent service exchanges remain separate concepts. Shared-runner p95 values and one-shot build timings should not be interpreted as hardware guarantees.

The generated regex cases have one match per matching line, so the raw rg occurrence counts and RS matching-line counts agree on these fixtures. Full per-file comparisons verify that agreement; it does not establish general Unicode or regex count equivalence for arbitrary repositories. CPU is target process CPU, while JSONL engine times are wall times. No new-NFA or whole-machine cold-cache performance claim is supported by this run.
