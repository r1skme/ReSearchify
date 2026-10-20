# Architecture, version 2

## Runtime boundaries

`researchify_core` contains six implementation modules. `engine` owns the indexing/search/snapshot lifecycle; `disk` owns paged dictionaries, bounded external sorting and compressed postings; `query` owns lexing, parsing, UTF-8 tokenization, Thompson NFA and conservative regex filters; `json` owns strict JSON parsing/writing; `api` owns one command registry, schemas, CLI, JSONL and MCP. `storage` retains tested portable serialization/checksum/fingerprint utilities and the original v1 storage helpers. Those helpers do not make v1 a readable production index in v2.

The executable does not launch subprocesses, contact services, load models, or require a language runtime. Test executables use `std::system` to exercise independent processes. Optional benchmark scripts use Python/PowerShell only as development tooling.

## Index construction

One producer enumerates the tree while a bounded byte queue feeds workers. Existing-index global scans group a bounded (up to8192 jobs / about2 MiB accounted payload) window by catalog bucket to improve locality; they still do not retain the entire file list. Each worker hashes/validates a file before deciding whether capture and tokenization are needed. Changed/new files are captured in a segment content stream and checked against the initial fingerprint, CRC, size and timestamps. Existing unchanged documents are not tokenized again.

Each worker accumulates term frequency and earliest position for the current document, spills bounded aggregates into a multi-pass external sorter, and periodically finalizes an independent segment. The sorter merges no more than 24 runs at a time. The fast profile additionally remembers distinct 24-bit byte trigrams with a 2 MiB bitmap and a bounded touched-word list, preventing repeated occurrences from generating repeated temporary records.

Text is scanned with fixed I/O blocks and a bounded line buffer. An invalid file is skipped with diagnostics; a file that changes after validation aborts the transaction. Filesystem enumeration errors are not interpreted as successful deletions. A repository is not an atomic snapshot of the entire filesystem: files can change after their individual stable capture. Global freshness remains unknown unless verified again.

## Metadata and generations

Stable document IDs contain a 10-bit path-hash bucket and a 54-bit monotonically allocated serial. A bucket hash is only routing: full relative UTF-8 paths are keys and collisions do not merge files. There are 1024 independent catalog B+trees. Each stores both ID→record and path→ID. An update rewrites only affected catalog buckets and writes a small new manifest. Highly skewed buckets have a budget guard, not an unbounded allocation.

A manifest references catalog generations and immutable segments with live document counts, aggregate statistics, next ID, source root and saved build policy. Updated documents point to their newest segment; older postings are filtered by the current record's segment identity. A no-op with identical policy/metadata does not publish a generation. Changing stored timestamps or policy can legitimately publish metadata without retokenizing content.

New segments are made final before catalogs and commit publication. The fully closed checked commit is renamed to `commit-G.bin` last. Before that point, readers continue opening the old commit. Failed transactions may leave unreachable temporary/orphan objects, which are harmless and reclaimed by GC. Stream flush/close and rename give a tested process-exit protocol, not an fsync-based power-loss guarantee.

## Snapshots and concurrent maintenance

Opening a snapshot takes the short `access.lock`, selects/validates a manifest and creates a checked reader pin before releasing the gate. Writers take `writer.lock`; ordinary searches do not acquire the writer lock. Each `Index` shares an immutable snapshot and can be searched concurrently. A `Service` is a sequential session, not a concurrent RPC dispatcher.

GC takes both writer lock and gate, retains the requested recent commits plus every pinned generation, and computes reachable catalogs/segments. It first removes every unretained commit, then removes unreachable data. Thus a process exit during deletion cannot leave an advertised old commit whose objects were already removed. Pin removal also uses the access gate, preventing a last reader's destructor from removing the generation file while GC reads it. If the destructor cannot acquire the gate, it leaves a conservative stale pin. A crashed reader's pin is retained indefinitely until explicit offline recovery. No timer guesses whether a slow reader is dead.

Compaction reindexes current saved document content into fresh segments, retaining stable IDs and publishing a new snapshot. It does not implicitly refresh from the live workspace or delete old snapshots. GC is separate. Pins protect an independent reader across both operations, which is exercised by a subprocess test.

## Search planning

Full terms look up exact dictionary strings. Sparse terms are streamed through compressed, sorted postings; active-version filtering uses the catalog. AND chooses the rarest safe covering anchor. OR unions covering anchors. NOT never subtracts approximate trigram candidates and may require enumeration of all live documents. Path filters apply before content verification.

For fast-profile phrases/regex, an AST-derived necessary condition extracts mandatory literal fragments. Optional or unknown portions weaken the condition to the universe; disjunction is a union. A bounded sample of a literal's trigrams selects a rare covering list. The precise NFA/substr matcher still verifies candidate text. The filter can admit false positives but must not exclude actual matches. There is no general full-text speed guarantee for expressions such as `/.*/`.

Exact term truth, frequency and first position come from the index. Once a candidate record is known, other exact leaves seek only its active segment rather than merging every segment merely to test that one document. Term-only search ranks candidates without opening their content; snippets are read after selecting top-k. Regex/phrase candidates are read for exact verification, then top results may be read again to produce evidence. Regex workspaces are reused within a query.

Ranking uses term frequency, a BM25-like length normalization, inverse frequency, earlier positions, path matches and proximity of the first occurrences of positive leaves. Segment frequency is an upper estimate while obsolete versions remain; compaction can change that estimate and therefore scores. Ranking version is 2. A continuation is bound to generation, query, filters and sort so it cannot accidentally mix rankings across updates. No approximate early termination is claimed.

Dense result candidates use ordered shard decoding; sparse candidates use direct B+tree lookup and a bounded shared record LRU. Raw and decoded page LRUs are budgeted separately. Each search holds bounded dictionaries/cursors and a top-k heap. Keyset pagination keeps heap size independent of page number but still evaluates candidates for each page.

## What was deliberately not added

No native filesystem watcher, USN journal, mmap, platform fsync, SIMD intrinsics, content compressor, vector database, semantic embedding service or background daemon. Native APIs violate the unchanged standard-library-only requirement. Snapshot content is stored uncompressed. WAND upper-bound pruning and dense compressed bitmap postings were not necessary to implement the delivered exact streamed search and are not claimed. The selected design is validated by the included measurements, not by extrapolating a maximum document constant.

A raw memory budget is not an OS-enforced RSS limit. Arbitrary concurrent queries require their own per-request budgets in addition to shared snapshot caches; the sequential stdio frontends avoid multiplicative unbounded work.
