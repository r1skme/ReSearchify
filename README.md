# ReSearchify

**Local code search. C++20. No runtime dependencies.**

[Русский](README.ru.md) · [CLI](#commands) · [For agents](#for-code-agents) · [Build](#build)

ReSearchify indexes source files on your machine. Search by token, exact text, or regex. Keep the index between runs; update only changed files.

> **Status:** source build. The Windows/MSVC workflow is configured, but a Windows release binary has not been verified here. No installer or prebuilt EXE is included.

## Quick start

Build `rs`, open a terminal in your project, then run:

```powershell
rs -i .
rs -s RenderFrame
rs -rs "Render[A-Za-z_]*Frame"
rs -l "std::vector<int>"
rs -u
```

On Windows, run `rs.exe` from your build directory or add that directory to `PATH`.

## Commands

| Command | Action |
| --- | --- |
| `rs -i .` | Index the current project |
| `rs -s "foo AND NOT bar"` | Search indexed tokens |
| `rs -rs "foo[0-9]+"` | Regex search |
| `rs -l "a->b"` | Find exact text |
| `rs -f "*.cpp"` | List indexed files |
| `rs -r src/main.cpp --lines 10:35` | Read indexed lines |
| `rs -u` | Update the index |
| `rs -st` | Index statistics |
| `rs --help` | Full CLI help |

Long forms (`rs init`, `rs find`, `rs read`, etc.) work too. `AND`, `OR`, `NOT`, parentheses and quoted phrases are supported. Search is case-sensitive. By default the index lives in `.researchify/` in your workspace.

## Build

**Windows 11 — Visual Studio 2022 / MSVC** with C++ tools and CMake:

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DBUILD_TESTING=ON
cmake --build build --config Release --parallel 8
ctest --test-dir build -C Release --output-on-failure
.\build\Release\rs.exe --help
```

**Linux / macOS:**

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build -j 8
ctest --test-dir build --output-on-failure
./build/rs --help
```

Requires CMake 3.20+ (Visual Studio generator: 3.21+) and a C++20 compiler. No external C++ packages.

## For code agents

Start with [`AGENTS.md`](AGENTS.md) and [`skills/researchify/SKILL.md`](skills/researchify/SKILL.md).

```powershell
rs capabilities --json
rs schema find --json
rs -s RenderFrame --json --limit 8 --max-output-bytes 16384
rs serve --stdio --root .
rs mcp --stdio --root .
```

JSON output is intended for scripts. `serve` keeps an index open across JSONL requests. `mcp` implements the stdio MCP interface with read-only tools. See [agent API](docs/AGENT_API.md).

## Notes

- Default indexing policy checks file contents during a full update; `--mode metadata` is faster but can miss unchanged-size/mtime edits.
- `compact` uses less index space; `fast` adds trigrams to filter phrase and regex candidates.
- Index data contains source text. Keep `.researchify/` out of version control and do not index secrets.
- Index format v2 is **not compatible** with v1. Build a new index; do not overwrite the old one.
- Protection against process crashes is not a guarantee against sudden power loss.

[Query syntax](docs/QUERY.md) · [Architecture](docs/ARCHITECTURE.md) · [Storage format](docs/FORMAT_STORAGE.md)

**License:** not specified yet. All rights reserved until a license is added.
