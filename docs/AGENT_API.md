# Agent interface, API version 1

Application2.0.0, disk format2, ranking version2. Obtain actual command schemas with `researchify schema --json` or `schema COMMAND --json`; help, schema and validation use one registry. Unknown fields are errors. Defaults described in schemas are applied by execution rather than mutating incoming JSON. An update retains saved build settings unless explicitly overridden.

## Common contract

Requests are objects with `command`, optional `api_version:1`, and an optional short string/number/null `id`. A result echoes a valid ID. Generic limits: input≤1 MiB, depth≤64, parsed nodes≤100000. Duplicate keys, malformed Unicode, NaN/Infinity and malformed numeric syntax are rejected. Unsigned64-bit integer text is preserved exactly by the parser. Generation and cursors use strings so clients do not lose64-bit precision in JavaScript numbers.

Success: `status:"ok", execution_complete:true, warnings:[]`. Failure: `status:"error", execution_complete:false, error:{code,message,retryable}`. Errors remain small even when a malicious filename/query contains thousands of control characters. Do not interpret errors as zero matches.

Every normal result is bounded by `max_output_bytes`, including serialized escapes, envelope and newline. Limit range1024…1048576, default65536. Windows CRLF is allowed for in-byte accounting. Human output escapes terminal control bytes. JSON output escapes controls but preserves proper UTF-8 strings.

Exit codes:0 success;1 human search with no matches;2 error. JSON search with zero matches exits0. A JSONL batch continues after a bad line, emits an error for that line and exits2 after EOF if any request failed.

## Find

`query` is textual boolean syntax or a tree with one key per node: `term`, `phrase` (`literal` alias), `regex`, `and:[...]`, `or:[...]`, `not:{...}`. AND/OR need2…128 children; tree depth≤64 and total text/leaf limits match the ordinary parser.

Request options: `path` list; `limit`; `cursor`; `count` exact/none; `sort` score/path; `snippets`; `snippet_bytes`; `max_scan_bytes`; `max_candidates`; `regex_steps`; `fields`. Command schema is authoritative for numeric limits. Zero read/candidate budget means unlimited; regex steps must be positive. Unknown projection fields are rejected.

Response includes `snapshot:{generation,source:"index",freshness:"unknown"}`, `position_units`, `results`, `returned`, `total` (or null), `has_more`, `next_cursor`, `output_truncated`, `metrics:{candidates,scanned_bytes,seconds}`. Hits normally contain path, content_id, score, matches, snippets. Projection can reduce fields; snippets may be omitted to fit the smallest possible paginated record.

A continuation includes generation, last score/path and a signature of index/query/path filters/sort/rank version, encoded as a bounded base64url token. It is validated but not cryptographically authenticated or intended as a security credential. The server recalculates ranks deterministically in the same snapshot and excludes keys already emitted. Page size/output fields may be changed, semantic query inputs may not. A removed snapshot cannot be restored from the cursor.

Top-k and count are exact for successfully completed execution. Neither `limit` nor `count:none` implies early termination. On a work budget error no partially evaluated result is presented as complete.

## Files and read

`files` lists metadata with `pattern`, `limit`, `after`. For a repeatable pagination sequence send the returned generation explicitly on subsequent calls; `after` is the exclusive path key. Unlike find, this simple listing continuation is not opaque. Globs are byte-sensitive: `?` is one non-slash byte, `*` excludes slash, `**` includes it, `**/` may match zero directories. A pattern without slash applies to basenames. Unicode bytes are preserved but wildcard character classes and normalization are not implemented.

`read` takes relative `file`, `first`, `last`, `source`, optional expected `content_id`, and `text_bytes`. Ranges are1-based inclusive, bounded to100001 requested lines. Text truncation preserves whole lines; if the next line is too large, it is not silently sliced. Empty returned range uses first/last0. Snapshot reads verify the whole saved file's content checksum. Workspace reads validate the complete live file against the indexed identity before returning the requested range. The original file's final newline status is not represented in the returned normalized line text.

`content_id` is a noncryptographic128-bit fingerprint plus CRC32. It detects accidental/stale content changes; it is not an authenticity guarantee or atomic filesystem edit token.

## Sessions

`request --stdin` reads one JSON object until EOF. `--input FILE` avoids shell quoting. `batch` and `serve --stdio` accept newline-delimited objects and reuse a snapshot. They are read-only unless `--allow-write` is explicitly supplied. A fixed `--root`/`--index` rejects requests attempting to switch to another index. `refresh` releases the cached snapshot; next/current refresh command opens latest. One request is processed at a time.

MCP uses JSON-RPC2.0, not this request shape directly. Supported versions:2025-11-25,2025-06-18,2025-03-26,2024-11-05. Unknown requested versions negotiate the server's supported default; the client decides whether to continue. Lifecycle, invalid JSON/requests, unknown methods, unknown tools and application errors are separately handled. Notifications have no response.

Tools: researchify_find/files/read/status. The registry supplies inputSchema with fixed-index fields removed. Server-scoped clients cannot supply command/index/root/id overrides or write operations. Output budget4096…1048576 applies to the entire RPC response; the inner record is allocated a smaller conservative budget because both text content and structuredContent may be sent. For protocol2024-11-05/2025-03-26 the response does not publish the later structuredContent field;2024-11-05 tools also omit later annotations. No HTTP, subscriptions, tasks, in-flight cancellation or sampling capability is advertised.

## Operational errors

E_ARGUMENT / E_JSON / E_API_VERSION: fix request.
E_QUERY_SYNTAX: fix query or unsupported regex syntax.
E_INDEX_NOT_FOUND: select an existing index, or explicitly authorize init.
E_FORMAT: v1/incompatible format; rebuild in a new directory.
E_INDEX_CORRUPT: report corruption; verify/rebuild, do not edit index internals.
E_BUDGET / E_OUTPUT_LIMIT: narrow work or explicitly change a supported budget.
E_CURSOR / E_SNAPSHOT_GONE: restart pagination with a consistent retained snapshot.
E_STALE_READ / E_SOURCE_CHANGED: re-read/update before editing or committing.
E_READ_ONLY / E_SCOPE: respect permissions/workspace boundaries.
E_LOCKED: another owner or stale lock; never force-unlock without stopping owners.
E_IO: filesystem/stream failure; inspect underlying message.

## External protocol references

MCP stdio transport: https://modelcontextprotocol.io/specification/2025-11-25/basic/transports
MCP tools: https://modelcontextprotocol.io/specification/2025-11-25/server/tools
Earlier tools schema: https://modelcontextprotocol.io/specification/2025-03-26/server/tools
Agent Skills: https://agentskills.io/specification

These references define external interoperability; they are not claims of tests in every agent application. Actual local protocol tests are listed in VALIDATION.md.
