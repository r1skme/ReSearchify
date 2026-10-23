# Linux file-data-cache measurements

`scripts/cache_benchmark.py` supplements the ordinary fresh-process and warm
JSONL measurements. It uses only Python's standard library and Linux libc via
`ctypes`; it does not add an OS dependency to the C++ runtime.

## Scope and labels

For each cold attempt the driver applies `posix_fadvise(POSIX_FADV_DONTNEED)`
to the complete ranges of explicitly inventoried fixture files. It never
writes global `drop_caches`, flushes a device cache, or resets system-wide
caches. Source and committed index files remain byte-for-byte immutable.
Normal reader pins are coordination state and are excluded.

`mmap(PROT_NONE)` plus `mincore` observes page residency without reading or
faulting in file data. Each range is mapped, inspected in bounded chunks, and
unmapped. Both pre-advice and post-advice observations are recorded, followed
by another observation after the search. A sample receives the label:

- `verified-cold-file-data-pages` only when every range was successfully
  inspected, at least one page was inspected, and all inspected pages were
  nonresident before the target was launched.
- `advice-only` when DONTNEED succeeded but zero residency could not be proven.
- `unverified-cache-state` when the advice failed and zero residency could not
  be proven.
- `warm-query-replay` after one identical fresh-process query warmup. This
  describes query-relevant warm pages, not a claim that every file is resident.

The proof concerns individual file ranges at their recorded observation
times. Inspection is sequential; another process or readahead could populate
an earlier range before target launch. The report records the observation
window and resource-helper launch timestamp. Directory/inode metadata is
touched by setup and the probes and is not evicted. Executables, shared
libraries, and storage-device caches remain uncontrolled.
These results must never be presented as a cold whole machine or cold SSD.

## Explicit disposable fixture

Create a new owned fixture directory, generate its source, and build the
indexes with the desired binaries outside search timings. The standard
benchmark generator or `scripts/generate_tiny_corpus.py` is suitable. Both
indexes must refer to this exact source root. Copied indexes with a different
source root are rejected. The driver checks complete source counts/bytes,
index profile, ownership, absence of symlinks/hard links, and hashes all data
before and after the experiment. It rejects unfinished transaction/sort
scratch rather than deleting it.

Place this marker **outside** the source and index trees, in their common
owned disposable parent:

```json
{
  "format": "researchify-cache-fixture-v1",
  "disposable": true,
  "description": "Generated 100000-file fixture; not the user's original corpus",
  "source": "source",
  "indexes": {
    "baseline": "baseline-compact",
    "current": "current-compact"
  }
}
```

Compatible readers may explicitly use the same immutable index directory.
Use separate invocations/markers for compact and fast. The driver uses the
standard generated-fixture literal, broad-regex and term queries. It first
compares complete matching paths and counts against ripgrep,
outside the timed measurements. Every measured response must be complete;
ripgrep uses occurrence counts for terms/literals and matching-line counts
for broad regex, matching the respective engine contracts. It scans completely
rather than exiting on the first matching file occurrence.

```sh
python3 scripts/cache_benchmark.py \
  --fixture /tmp/owned-fixture/cache-fixture.json \
  --baseline /absolute/rs-before --current /absolute/rs-after \
  --rg /absolute/rg --iterations 7 \
  --work /tmp/new-cache-results
```

Do not run other benchmark targets or fixture writers concurrently. Each
query/cache mode has seven deterministically shuffled baseline/current/rg
repetitions. With seven samples, nearest-rank p95 is the maximum, so tail
estimates are weak. The timed work is a fresh search process; cache preparation,
correctness passes, the Python helper, and index construction are excluded.

Raw JSON includes commands, binary hashes, inventory hashes, file identities,
residency observations, target wall/CPU/RSS, engine stdout/stderr, and Linux
`getrusage` input/output block-operation and page-fault counters. These are OS
counters, not direct device-byte measurements. RSS can include Python's
inherited pre-exec floor; the resource helper never loads large inventories.
Successful ripgrep output is stored as its byte count/SHA and the complete
path/count digest, with one complete compressed JSON oracle per query; a failed
sample retains its full offending output.
Verified-cold and advice-only samples have separate distributions. The driver
fails on semantic differences or changed data and preserves an incomplete
report. Unsupported residency verification is recorded rather than silently
converted into a cold-cache claim.
