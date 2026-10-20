# Disk format 2

All formats are implemented in `src/disk.cpp` and `src/engine.cpp`. Files are local, immutable after publication, and unencrypted. Integer offsets must fit a signed `std::streamoff`. CRC-32 detects accidental damage, not malicious forgery.

## Primitives

* Fixed u32/u64: little endian, explicit byte encoding; never serialized native structs.
* Ordered doc IDs: big endian eight bytes, so byte ordering is numeric ordering.
* Varint: unsigned base-128, at most10 bytes. Non-canonical encodings, truncation and overflow are rejected.
* Variable string: canonical varint byte length then UTF-8 or explicitly binary bytes. Size limits are validated before allocation.
* Checked file: payload plus little-endian CRC32(payload). The entire file length is bounded before reading.

## Commit

`commit-G.bin` is a checked file (maximum4 MiB payload). Magic `CSMAN2\r\n`. Fields in order:

1. Varints generation, next serial ID, live files, lines, source bytes, token occurrences.
2. Strings canonical absolute source root and scan-scope label.
3. 1024 catalog-generation varints; zero means absent bucket.
4. Varint live segment count (≤65536), followed by sorted `(segment ID, live document count)` varints.
5. Saved policy varints: threads, memory MiB, max file bytes, max line bytes, metadata-mode boolean, segment document limit.
6. Profile string, then directory exclusion count/strings and path exclusion count/strings.

Counts, sortedness, generations, UTF-8, root scope, options and totals are validated. Unknown format magic is not guessed as a compatible v1 index. The filename generation and encoded generation must agree.

New commits are written as `commit-G.bin.tmp` and renamed only after referenced immutable files are ready. Unpublished transaction directories are not part of the index view.

## Prefix-compressed table

Catalogs and term dictionaries use the same B+tree.

Header36 bytes: magic8 `CSTBL2\r\n`, root offset u64, end of contiguous leaf region u64, total entry count u64, CRC32 of preceding32 bytes.

A page begins with payload length u32 and CRC32(payload). Payload:

* type byte0 leaf /1 internal;
* count varint;
* repeated shared-prefix length, suffix string, value string.

Keys are strictly increasing. Each key reconstructs from the previous key in that page plus its suffix. Internal values are u64 offsets to earlier pages; the key equals the child's first key. Leaves occupy a contiguous region before internal levels. The root ends the file. Target page size16 KiB; maximum payload256 KiB and field64 KiB. Depth≤32. Oversized single entries can produce larger-than-target pages up to the hard limit.

`verify()` checks all leaf/internal pages, CRCs, root extent, separators, downward offsets, exact ordered leaf coverage and aggregate count, rejecting unreachable/repeated leaves and appended/truncated data. Lookup verifies touched pages, not all unused pages. A fresh cache is used for full engine verification.

Writer builds leaf pages then spills separator levels to temporary files; it never needs all dictionary entries in RAM. Cached raw and decoded nodes share a fixed overall cache budget split between two LRUs.

## Catalog

`catalog-G-B.tbl`, where B is0…1023:

* `I` + ordered docID → record;
* `P` + complete relative UTF-8 path → ordered docID.

A record contains ID, segment, content offset/size, line/token counts, encoded modification time, two fingerprint words, CRC32, maximum line length and the full path. ID→path and path→ID are cross-checked by verification. Full keys, not hashes, establish identity.

A normal update creates replacement tables for changed buckets only. The manifest may refer to bucket tables from many earlier generations. A full-manifest chain of duplicated document records is not written.

## Segment

`segment-S/`:

* `content.bin`: concatenated raw bytes of captured UTF-8 files; record offsets and checksums identify individual documents. BOM/CRLF remain in storage and are normalized only for matching/line display.
* `terms.tbl`: complete term key → posting descriptor.
* `postings.bin`: magic `CSPOST2\n`, followed by non-inline posting blocks.

A term key is prefix byte2 followed by a complete UTF-8 token, or byte1 followed by exactly3 raw trigram bytes. Source NUL is not indexed. Temporary sort keys append NUL and ordered docID. This makes term separation unambiguous without relying on hashes.

Descriptor tag0: varint document frequency1…4, then inline postings. Each posting is delta docID (first relative to0), frequency, first line, first column, all varints. The containing dictionary page CRC protects it. A segment containing only short lists may have an8-byte `postings.bin`.

Descriptor tag1: start offset, end offset, document frequency, all varints. Lists contain blocks of at most128 postings. Each32-byte header has payload length u32, count u32, first and last document u64, payload CRC32, header CRC32 of its first28 bytes. Payload repeats delta docID (reset from0 in each block), frequency, first line and first column. IDs are strictly increasing and all frequency/position fields positive.

Cursor seek can skip a block using its checked header bounds. Skipped payload bytes are not read by that query; full verification checks every block and exact descriptor counts/contiguity. Inline short lists avoid an I/O/header for every identifier unique to one file.

Fast-profile trigram frequency is1 because only document membership and an initial evidence position are required. Token frequencies preserve actual counts.

## Pins, locks and recovery

`writer.lock/`: one writer/compactor/collector.
`access.lock/`: short gate for snapshot pin publication and reclamation.
`pins/reader-.../generation`: checked decimal generation string.

Locks are exclusively created directories. Reader pin names combine random values, a sequence and time, with exclusive directory creation and retry. A pin is removed by snapshot destruction; it is not automatically expired after a crash.

Readers must be able to create/remove pins, so a truly read-only filesystem mount is not supported. Offline `recover --force --clear-pins` is an operator action after stopping all owners. No platform process-ID liveness test is hidden inside the implementation.

## v1 transition

Rebuild using `researchify create ROOT NEW_INDEX`. Keep v1 and its executable until the new index passes `verify` and representative queries. There is no in-place binary migration or claim that a v1 corruption test proves correctness of the v2 format; both old utility tests and new format tests are shipped.
