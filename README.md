# ReSearchify

**Find the code you need. Search once, or search a thousand times.**

ReSearchify indexes source code and lets you find identifiers, exact text, regular expressions, and combinations of terms without rescanning every file for every query. Written in C++20, with a small command-line tool named **`rs`**.

[Русский](README.ru.md) · [Download](#download-and-run) · [Commands](#commands) · [Benchmarks](#measured-performance) · [Agent setup](#use-with-an-ai-agent)

## Download and run

Pick the **tested release asset** for your operating system and CPU from [GitHub Releases](https://github.com/r1skme/ReSearchify/releases) and download its `SHA256SUMS.txt` from **the same release**:

| System | Archive | Step-by-step setup (Russian + English) |
| --- | --- | --- |
| Windows x64 | `ReSearchify-Windows-x64.zip` | [Windows guide](docs/INSTALL_WINDOWS.md) |
| macOS Apple Silicon (M1/M2/M3/M4+) | `ReSearchify-macOS-arm64.tar.gz` | [macOS guide](docs/INSTALL_MACOS.md) |
| macOS Intel | `ReSearchify-macOS-x64.tar.gz` | [macOS guide](docs/INSTALL_MACOS.md) |
| Linux x86-64 | `ReSearchify-Linux-x64.tar.gz` | [Linux guide](docs/INSTALL_LINUX.md) |
| Linux ARM64 | `ReSearchify-Linux-arm64.tar.gz` | [Linux guide](docs/INSTALL_LINUX.md) |

Prebuilt releases contain a portable `rs`/`rs.exe` CLI; no Python, Node.js, or compilation is required. **Code → Download ZIP** downloads source code, not an executable.

**Important:** `rs -i .` indexes the terminal's **current directory**. If you are still in Downloads after installing, it will index Downloads — not your source project. Each OS guide now includes a safe one-file test and explicit-path commands (`rs init "FULL_PROJECT_PATH"`, `rs status --root "FULL_PROJECT_PATH"`) so you can confirm the right folder before indexing real code. The index stores source-text copies in `.researchify/`; never automatically index secrets or unrelated folders.

Linux prebuilt packages are built on Ubuntu 22.04 and need compatible system libraries; macOS builds target 13.3+ and are not Apple-notarized. Published preview assets require successful native tests for all five platforms plus Linux sanitizer tests. See [installation overview](docs/INSTALL.md) and the [agent-specific checklist](docs/AGENT_SETUP.md).


## Commands

After indexing an explicitly selected project, these examples work from its root (or supply `--root "FULL_PROJECT_PATH"`). They assume `rs` is available on PATH; otherwise use the full executable path:

| Command | Meaning | When to use it |
| --- | --- | --- |
| `rs -i .` | Create an index of the current project | First use; only once per project |
| `rs -s RenderFrame` | Find the exact identifier `RenderFrame` | Function, class, variable names |
| `rs -s "RenderFrame AND NOT deprecated"` | Find files containing `RenderFrame` but not `deprecated` | Filter noisy results |
| `rs -rs "worker_[0-9]+"` | Match a regular expression | Numbered identifiers, patterns |
| `rs -l "std::vector<int>"` | Search for that exact text | Punctuation, calls, code fragments |
| `rs -f "*.cpp"` | List matching filenames | Find files by extension/name |
| `rs -r src/main.cpp --lines 20:45` | Read lines 20–45 from the indexed snapshot | Inspect a result without opening an editor |
| `rs -u` | Scan for changes and update the index | After editing files |
| `rs -st` | Show file, line, token and index-size statistics | Check index coverage and disk use |
| `rs verify` | Validate the saved index | Diagnose corruption |
| `rs --help` | List all commands and options | Discover anything else |

The short forms are optional: `rs init`, `rs find`, `rs read`, `rs update`, `rs status` and the other full commands work too.

**Search rules:** terms match whole identifiers and are case-sensitive. `AND`, `OR`, `NOT` and parentheses combine **files**, not adjacent words on the same line. Use `-l` for exact punctuation and `-rs` for patterns. An index lives in `.researchify/` at your project root; `-u` checks existing content for changes, while `--trust-mtime` opts into a faster, less strict check.

### Examples

**Find an implementation but exclude old code:**

```powershell
rs -s "(RenderFrame OR DrawFrame) AND NOT deprecated" --path "src/**"
```

Here, `OR` accepts either name, `AND NOT` excludes files containing `deprecated`, and `--path` limits results to `src/`.

**Find strings with punctuation:**

```powershell
rs -l "#include <vector>" --limit 10
```

`-l` searches the original characters rather than splitting a string into identifiers; `--limit 10` returns at most ten files. All matches are evaluated unless you also set an execution budget.

**Find a family of functions:**

```powershell
rs -rs "worker_[0-9]+" --path "src/**" --json
```

`[0-9]+` means one or more digits; `--json` prints structured output for scripts instead of formatted text.

For the complete query language, see [QUERY.md](docs/QUERY.md).

## Why use ReSearchify?

| | `grep` / `ripgrep` | Everything | ReSearchify |
| --- | --- | --- | --- |
| Primary approach | Scan files per command | Fast index of filenames and paths | Persistent index of **source content** |
| Search across many queries | Files are typically scanned again | Very fast filename lookups | Reuse existing postings and snapshots |
| `AND`, `OR`, `NOT` across files | Usually needs shell pipelines or extra logic | Different query model | Built into the query language |
| Exact text / regex | Excellent support, especially ripgrep | Content-search path differs from indexed filenames | Both, with a dedicated query API |
| Agent integration | Terminal output, scripting | Windows-oriented command line | JSON, JSONL sessions and stdio MCP |
| Initial setup / disk | No index needed | Requires Everything | One indexing step and disk space |

**Use `ripgrep` for a quick one-off search** in a small codebase. ReSearchify is useful when you're repeatedly exploring a large tree, combining conditions, or running many searches from an agent. Everything is excellent for finding filenames; ReSearchify's focus is source *content*. These are different tools, not a claim that one always beats the others.

### Measured performance

Results from the same **100,000-file / 5,000,000-line** synthetic source corpus (about **350 MiB** of text), comparing the previous CodeSearch v1 engine against the indexed v2 engine that ReSearchify builds on:

| Measurement | v1 | v2 / ReSearchify baseline |
| --- | ---: | ---: |
| Initial index size | 682 MiB | **483 MiB** |
| Index open | 300 ms | **7.14 ms** |
| Rare identifier, warm in-process query | 1.50 ms | **0.080 ms** |
| Term present in all 100,000 files, complete query | 4.87 s | **0.476 s** |
| Disk added by one edited file | about 11.7 MB | **about 16 KB** |

Index size is **29% smaller** on that corpus; a warm rare-term query was about **19× faster**. These figures compare **two versions of this project**, **not** against `rg`, `grep`, or Everything, and are **not** Windows measurements. They depend on CPU, disk cache, corpus and query. A broad regex can still be slow (about 19.9 s in this v2 baseline); full strict refreshes still read unchanged files to detect hidden edits. See [BENCHMARKS.md](docs/BENCHMARKS.md) for setup, raw results and limitations.

## Use with an AI agent

Share the repository link with your agent and ask:

> Set up ReSearchify on the machine where I want it to run. Read `docs/AGENT_SETUP.md` and `AGENTS.md`. Check the actual OS/CPU, select a completed tested release, verify the archive against `SHA256SUMS.txt`, and install the executable. First prove it works on an isolated one-file demo. **Ask me for the exact absolute source-project directory and permission before indexing it**; never use Downloads or assume `.` is correct. Confirm `root=`, run one known-term search and `verify`, and explain `update`. Use JSON for subsequent queries. Ask before any source build or security bypass.

The agent must be allowed to execute programs **on your target computer**. A web-only chat cannot perform an installation. For detailed OS-specific setup, safe command sequences, error handling, and MCP/JSONL rules, read [Agent setup](docs/AGENT_SETUP.md).

The optional Windows installer `scripts/install.ps1` is for users who have **already cloned this repository** and have GitHub CLI (`gh`) installed; ordinary users can follow [the manual Windows guide](docs/INSTALL_WINDOWS.md) without `gh`.

For agent integrations, using the confirmed project path:

```text
rs capabilities --json
rs schema find --json
rs find RenderFrame --root "FULL_PROJECT_PATH" --json --limit 8
rs serve --stdio --root "FULL_PROJECT_PATH"     # Persistent JSONL session
rs mcp --stdio --root "FULL_PROJECT_PATH"       # MCP transport (different protocol)
```

Prefer an absolute executable path if `rs` is not on PATH. MCP requires a compatible client; see [Agent API](docs/AGENT_API.md) and the [Windows](examples/mcp-client.json), [macOS](examples/mcp-client-macos.json), or [Linux](examples/mcp-client-linux.json) example.

## Build from source

Install CMake 3.20+ and Visual Studio 2022 with the MSVC C++ desktop workload (Windows), or a C++20 compiler and CMake 3.20+ (Linux/macOS).

**Windows — PowerShell:**

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DBUILD_TESTING=ON
cmake --build build --config Release --parallel 8
ctest --test-dir build -C Release --output-on-failure
.\build\Release\rs.exe --help
```

**Linux / macOS:**

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build --parallel 8
ctest --test-dir build --output-on-failure
./build/rs --help
```

The C++ core requires **no third-party libraries**. Windows builds use a static MSVC runtime by default. CI checks Windows x64, Linux x64/ARM64, macOS Intel/Apple Silicon and Linux sanitizers; see [the workflow](.github/workflows/windows.yml). An Actions workflow is not evidence of a successful run—check its actual result before sharing a binary.

## Documentation

[Installation](docs/INSTALL.md) · [Agent setup](docs/AGENT_SETUP.md) · [Agent instructions](AGENTS.md) · [Agent skill](skills/researchify/SKILL.md) · [Search syntax](docs/QUERY.md) · [API](docs/AGENT_API.md) · [Index format](docs/FORMAT_STORAGE.md) · [Benchmarks](docs/BENCHMARKS.md)

The index stores source text, so keep `.researchify/` out of Git and avoid indexing secrets. Indexes from the older v1 format must be rebuilt. Process-crash recovery does not guarantee durability after a sudden power failure.

No license has been granted yet. Contact the repository owner before redistributing the source or binary.