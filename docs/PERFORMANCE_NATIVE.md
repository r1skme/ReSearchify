# Native macOS measurements at the c5a22f0 checkpoint

These are native GitHub Actions measurements from [run 39961942957](https://github.com/r1skme/ReSearchify/actions/runs/39961942957), associated with PR head `c5a22f0b9b4bae421f3334dce621640a8e65a96d`. They precede the later tiny-file/update changes. They are not a recreation of the user’s macOS screenshots or Windows measurements.

Both architectures ran the binaries, tests, complete result comparisons, and timing loops on their native host. Every table uses 21 paired fresh processes with deterministically shuffled tool order after excluded warmups. OS cache was uncontrolled; these are not cold-cache results. RS uses exact count-only search, while rg scans all occurrences. Index creation is separate. Full matching paths and per-file counts were compared 24 times per architecture before timing.

The fixture is 20,000 generated files with 50 lines each. The original reader/writer is `eb858348169157686e70104af6b64f9539a39fe8`; additional same-index comparisons against the previous optimized `dbcddf76708b614c26e212fa27b48efa33ddfd58` checkpoint and warm JSONL measurements are in each raw archive. The archives contain full samples, commands, executable hashes, compiler/host identity, checks and index-build reports.

## Apple Silicon

Host: `macOS-15.7.9-arm64-arm-64bit`, 3 logical CPUs. Current checked-out revision: `ade04cb1853a790e60068c6a6c7788714a8f4f77`.

| Profile/query | Original median ms | Current median ms | Current p95 ms | rg median ms |
|---|---:|---:|---:|---:|
| compact/term | 4.322 | 4.353 | 6.867 | 121.895 |
| compact/literal | 699.198 | 77.878 | 84.486 | 123.316 |
| compact/no_match | 666.026 | 70.687 | 77.173 | 119.848 |
| compact/broad_literal | 694.908 | 197.214 | 239.543 | 272.943 |
| compact/broad_regex | 2055.594 | 420.565 | 490.845 | 264.876 |
| fast/term | 5.561 | 4.841 | 10.149 | 128.390 |
| fast/literal | 21.595 | 16.743 | 18.099 | 125.242 |
| fast/no_match | 4.245 | 4.320 | 7.026 | 125.378 |

[Complete raw archive](../bench/results/optimization-native-macos-arm64-c5a22f0.zip).

## Intel

Host: `macOS-15.7.9-x86_64-i386-64bit`, 4 logical CPUs. Current checked-out revision: `ade04cb1853a790e60068c6a6c7788714a8f4f77`.

| Profile/query | Original median ms | Current median ms | Current p95 ms | rg median ms |
|---|---:|---:|---:|---:|
| compact/term | 17.964 | 19.358 | 28.017 | 309.812 |
| compact/literal | 1826.115 | 293.951 | 376.459 | 285.615 |
| compact/no_match | 1666.772 | 266.999 | 341.356 | 308.199 |
| compact/broad_literal | 1586.157 | 457.984 | 871.236 | 738.201 |
| compact/broad_regex | 4056.490 | 869.732 | 916.575 | 676.179 |
| fast/term | 12.212 | 13.002 | 14.845 | 238.270 |
| fast/literal | 50.269 | 38.449 | 50.513 | 259.182 |
| fast/no_match | 14.469 | 13.485 | 16.361 | 306.620 |

[Complete raw archive](../bench/results/optimization-native-macos-x64-c5a22f0.zip).

## Interpretation and limits

The selective literal path improves substantially on both hosts. On ARM64 this compact literal workload is faster than rg; on Intel its median remains slightly slower. Broad literal improves and beats rg on both hosts. Broad regex is substantially faster than the original implementation but still slower than rg on both hosts.

Some short fresh-process queries fluctuate or regress: the Intel compact rare-term median rises from 17.964 to 19.358 ms, and the fast rare-term median rises from 12.212 to 13.002 ms. The complete warm-session distributions and p95 values are retained rather than substituting an unrelated Linux measurement. These results do not prove that every query or repository is faster.

Windows x64 MSVC, Linux x64 GCC/Clang, Linux ARM64 GCC, both macOS Clang targets and Address/UB sanitizers passed their build/test jobs in this run. The release job was skipped. These checks cover this checkpoint; subsequent C++ changes require a new native run.
