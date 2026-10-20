# ReSearchify — notes for agents

Read [`README.md`](README.md) for the quick start, or [`README.ru.md`](README.ru.md) for Russian. Use `rs capabilities --json` to discover the current interface and `rs schema find --json` for exact parameters.

# Working on ReSearchify

Runtime implementation is C++20 standard-library only. Do not add SQLite, a search engine, a package-manager dependency, subprocess helpers, or platform APIs to the core without changing the stated requirements.

CLI shortcuts: `rs -s` token/boolean search, `rs -rs` regex, `rs -l` literal, `rs -i` init, `rs -u` update. Never index folders automatically without permission.

Build: `cmake -S . -B build -DCMAKE_BUILD_TYPE=Release`; `cmake --build build --parallel 4`; `ctest --test-dir build --output-on-failure`.

Keep CLI help, JSON schemas and request validation consistent through the registry in `src/api.cpp`. Machine stdout must remain protocol-only. Preserve bounded output, explicit error codes and immutable snapshot identity.

Storage changes require corruption, truncation, varint-overflow, independent-reader/writer and process-exit tests. Never reclaim a pinned snapshot. Publish data before commit; unpublish obsolete commits before reclaiming data. Stdlib stream flush is not power-loss durability.

Query prefilters may admit extra candidates but must never remove a real match, especially under OR, optional repeats and NOT. Test against an exact full-scan oracle in both profiles. Terms must be compared as complete strings, not only hashes.

Run benchmarks on the same corpus and report regressions as well as gains. Keep logs tied to the final source. Do not claim MSVC, sanitizer, million-file, or power-loss validation without actual evidence.
