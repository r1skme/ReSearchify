# ReSearchify

**Find the code you need. Search once, or search a thousand times.**

ReSearchify indexes source code and lets you find identifiers, exact text, regular expressions, and combinations of terms without rescanning every file for every query. Written in C++20, with a small command-line tool named **`rs`**.

[Русский](README.ru.md) · [Download](#download-and-run) · [Commands](#commands) · [Benchmarks](#measured-performance) · [Agent setup](#use-with-an-ai-agent)

## Download and run

**Windows 11 (x64):**

1. Open **[Releases](https://github.com/r1skme/ReSearchify/releases)**. Download `ReSearchify-Windows-x64.zip` from the newest preview release.
2. Extract it, for example to `C:\Tools\ReSearchify`. You should see `rs.exe`. No Python, Node.js, service, or installer is needed.
3. Open PowerShell **in the project you want to search**, then run:

```powershell
# Make 'rs' available in this PowerShell window (adjust the folder if needed)
$env:Path += ';C:\Tools\ReSearchify'

rs -i .                             # Index this project once
rs -s "RenderFrame"                # Find an identifier
rs -rs "Render[A-Za-z]*Frame"      # Regex search
rs -u                               # Update the existing index
```

A release appears only after its Windows build and automated tests pass. If the Releases page is still empty or a run failed, see [build from source](#build-from-source). This repository is **private**: GitHub access is required to download it.

Alternatively, download the repo as a ZIP (**Code → Download ZIP**) and build it yourself. Detailed steps: [INSTALL.md](docs/INSTALL.md).

## Commands

Once `rs.exe` is in your `PATH`, run these commands from any folder inside an indexed project:

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

Give your agent the repository link and this request:

> Set up ReSearchify for my project. Read `AGENTS.md` and `skills/researchify/SKILL.md`. Download the latest Windows x64 archive from this repository's Releases, check its SHA-256 against `SHA256SUMS.txt`, and install `rs.exe`. Index my project only after I approve the target folder. Demonstrate one search and show me how to update the index. Prefer JSON for subsequent searches. If no release is available, tell me before attempting a source build.

A private GitHub repository requires an authenticated account with access. The agent also needs permission to execute programs on your computer; a web-only chat cannot install software by itself.

For an already authenticated terminal with GitHub CLI, there is an optional installer:

```powershell
.\scripts\install.ps1     # Download a checked release to your user profile
rs -s RenderFrame --json --limit 8
```

To integrate directly, use:

```powershell
rs capabilities --json             # Discover features and limits
rs schema find --json             # Exact argument schema for 'find'
rs serve --stdio --root .         # One process, many JSONL requests
rs mcp --stdio --root .           # MCP tools for compatible agents
```

No separate plugin is required for a terminal-capable agent. MCP requires a compatible client configuration; see [AGENT_API.md](docs/AGENT_API.md) and [MCP example](examples/mcp-client.json).

## Build from source

Install CMake 3.21+ and Visual Studio 2022 with the MSVC C++ desktop workload (Windows), or a C++20 compiler and CMake 3.20+ (Linux/macOS).

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

The C++ core requires **no third-party libraries**. Windows builds use a static MSVC runtime by default. CI checks Windows (MSVC), Linux and sanitizer builds; see [the workflow](.github/workflows/windows.yml). An Actions workflow is not evidence of a successful run—check its actual result before sharing a binary.

## Documentation

[Windows install](docs/INSTALL.md) · [Agent instructions](AGENTS.md) · [Agent skill](skills/researchify/SKILL.md) · [Search syntax](docs/QUERY.md) · [API](docs/AGENT_API.md) · [Index format](docs/FORMAT_STORAGE.md) · [Benchmarks](docs/BENCHMARKS.md)

The index stores source text, so keep `.researchify/` out of Git and avoid indexing secrets. Indexes from the older v1 format must be rebuilt. Process-crash recovery does not guarantee durability after a sudden power failure.

No license has been granted yet. Contact the repository owner before redistributing the source or binary.