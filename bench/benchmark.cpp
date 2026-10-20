#include "researchify/engine.hpp"
#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <numeric>
#include <string>
#include <thread>
#include <vector>

namespace {
using namespace researchify;
using Clock = std::chrono::steady_clock;
struct Options {
    fs::path work = "benchmark-data";
    std::string phase = "all", index_name = "index", profile = "compact";
    std::size_t files = 100000, lines = 50, threads = 16, memory_mb = 256, repeats = 7;
};
double elapsed(Clock::time_point start) {
    return std::chrono::duration<double>(Clock::now() - start).count();
}
std::size_t positive(const std::string& text) {
    std::size_t n = 0;
    auto r = std::from_chars(text.data(), text.data() + text.size(), n);
    if (r.ec != std::errc{} || r.ptr != text.data() + text.size() || n == 0)
        throw Error("Expected positive integer: " + text);
    return n;
}
fs::path file_path(const fs::path& source, std::size_t i) {
    return source / ("group_" + std::to_string(i / 1000)) / ("module_" + std::to_string(i) + ".cpp");
}
void close_checked(std::ofstream& out) {
    out.flush();
    if (!out) throw Error("Benchmark file write failed");
    out.close();
    if (!out) throw Error("Benchmark file close failed");
}
void generate(const Options& o) {
    const auto start = Clock::now();
    fs::create_directories(o.work);
    const auto source = o.work / "source";
    if (fs::exists(source) || fs::exists(o.work / "benchmark.meta"))
        throw Error("Benchmark corpus already exists; use another --work or a non-generate --phase");
    if (o.files > std::numeric_limits<std::uint64_t>::max() / o.lines)
        throw Error("Corpus line count overflows uint64_t");
    fs::create_directory(source);
    std::uint64_t total_bytes = 0;
    for (std::size_t i = 0; i < o.files; ++i) {
        const auto file = file_path(source, i);
        if (i % 1000 == 0) fs::create_directory(file.parent_path());
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        if (!out) throw Error("Cannot create benchmark source file");
        out << "// generated file " << i << '\n';
        out << "namespace bucket" << (i % 100) << " { int needle_" << i << " = 0;\n";
        for (std::size_t line = 2; line + 1 < o.lines; ++line)
            out << "int function_" << i << '_' << line
                << "(int value) { int common = value + " << line << "; return common; }\n";
        out << "}\n";
        close_checked(out);
        total_bytes += fs::file_size(file);
    }
    std::ofstream meta(o.work / "benchmark.meta", std::ios::binary | std::ios::trunc);
    if (!meta) throw Error("Cannot create benchmark metadata");
    meta << "RESEARCHIFY_BENCH_1\n" << o.files << ' ' << o.lines << '\n';
    close_checked(meta);
    std::cout << "GENERATE files=" << o.files << " lines=" << o.files * o.lines
              << " bytes=" << total_bytes << " seconds=" << elapsed(start) << '\n';
}
void read_meta(Options& o) {
    std::ifstream meta(o.work / "benchmark.meta", std::ios::binary);
    std::string magic;
    if (!(std::getline(meta, magic)) || magic != "RESEARCHIFY_BENCH_1"
        || !(meta >> o.files >> o.lines) || o.files == 0 || o.lines < 4)
        throw Error("--work is not a completed benchmark corpus");
}
void build_report(const std::string& phase, const BuildReport& r) {
    const auto rate = r.seconds > 0 ? static_cast<double>(r.bytes) / (1024.0 * 1024.0 * r.seconds) : 0;
    std::cout << "BUILD phase=" << phase << " generation=" << r.generation
              << " files=" << r.files << " lines=" << r.lines << " bytes=" << r.bytes
              << " added=" << r.added << " changed=" << r.changed
              << " removed=" << r.removed << " unchanged=" << r.unchanged
              << " skipped=" << r.skipped << " hashed=" << r.hashed
              << " tokenized=" << r.tokenized << " workers=" << r.workers
              << " memory_budget_bytes=" << r.memory_budget_bytes
              << " seconds=" << r.seconds << " corpus_mib_per_second=" << rate << '\n';
}
void run_searches(const Options& o, const fs::path& directory) {
    const auto opened = Clock::now();
    const Index index(directory, o.memory_mb);
    const auto open_seconds = elapsed(opened);
    const auto s = index.stats();
    std::cout << "OPEN seconds=" << open_seconds << " files=" << s.files
              << " lines=" << s.lines << " active_segments=" << s.segments
              << " retained_generations=" << s.retained_generations
              << " index_bytes=" << s.index_bytes << '\n';
    const auto id = std::min<std::size_t>(42, o.files - 1);
    const auto id2 = std::min<std::size_t>(43, o.files - 1);
    const auto needle = "needle_" + std::to_string(id);
    struct QueryCase { std::string name, query; bool broad; };
    const std::vector<QueryCase> queries = {
        {"term", needle, false},
        {"and", "bucket" + std::to_string(id % 100) + " AND common", false},
        {"or", needle + " OR needle_" + std::to_string(id2), false},
        {"not", needle + " AND NOT absent_symbol", false},
        {"phrase", needle + " AND \"return common;\"", false},
        {"regex", needle + " AND /function_[0-9]+_[0-9]+/", false},
        {"broad_term", "common", true},
        {"broad_regex", "/needle_[0-9]+/", true}
    };
    SearchOptions so;
    so.limit = 20;
    so.memory_mb = o.memory_mb;
    // Large corpus scans are intentional. Their work is reported separately.
    so.max_regex_steps = std::numeric_limits<std::uint64_t>::max();
    for (const auto& q : queries) {
        const auto count = q.broad ? std::size_t{1} : o.repeats;
        std::vector<double> values;
        SearchReport last;
        // This warm-up is explicit, and measured results below are warm-cache.
        if (!q.broad) static_cast<void>(index.search(q.query, so));
        for (std::size_t iteration = 0; iteration < count; ++iteration) {
            const auto start = Clock::now();
            last = index.search(q.query, so);
            values.push_back(elapsed(start) * 1000.0);
        }
        std::sort(values.begin(), values.end());
        const auto p95 = static_cast<std::size_t>(std::ceil(0.95 * static_cast<double>(values.size()))) - 1;
        const auto median = values.size() % 2 ? values[values.size() / 2]
            : (values[values.size() / 2 - 1] + values[values.size() / 2]) / 2;
        std::cout << "QUERY name=" << q.name << " iterations=" << values.size()
                  << " warmup=" << (q.broad ? 0 : 1) << " p50_ms=" << median
                  << " p95_ms=" << values[p95] << " min_ms=" << values.front()
                  << " max_ms=" << values.back() << " matches=" << last.total_matches
                  << " candidates=" << last.candidates << " scanned_bytes=" << last.scanned_bytes << '\n';
    }
}
void update_corpus(const Options& o, const fs::path& directory, const BuildOptions& options) {
    const auto source = o.work / "source";
    const auto changed = (o.files + 99) / 100;
    for (std::size_t i = 0; i < o.files; i += 100) {
        std::ofstream out(file_path(source, i), std::ios::binary | std::ios::app);
        if (!out) throw Error("Cannot modify benchmark source file");
        out << "int updated_marker_" << i << " = 1;\n";
        close_checked(out);
    }
    const auto report = update_index(directory, options);
    build_report("update_one_percent", report);
    if (report.tokenized != changed) throw Error("Incremental benchmark unexpectedly re-tokenized files");
}
int run(const std::vector<std::string>& args) {
    Options o;
    for (std::size_t i = 1; i < args.size(); ++i) {
        const std::string arg = args[i];
        if (arg == "--help") {
            std::cout << "cs_bench [--work DIR] [--phase all|generate|create|update|search|verify]\n"
                      << "         [--index NAME] [--files 100000] [--lines 50]\n"
                      << "         [--threads 16] [--memory-mb 256] [--repeats 7] [--profile compact|fast]\n"
                      << "Default all: generate, create, strict no-op update, update 1%, searches, verify.\n"
                      << "Generation refuses existing corpus. Other phases read benchmark.meta.\n"
                      << "Use separate --index names to compare thread counts on the same corpus.\n"
                      << "Use scripts/benchmark.ps1 on Windows to sample process memory.\n";
            return 0;
        }
        if (i + 1 == args.size()) throw Error("Missing benchmark option value");
        const std::string value = args[++i];
        if (arg == "--work") o.work = path_from_utf8(value);
        else if (arg == "--phase") o.phase = value;
        else if (arg == "--profile") o.profile = value;
        else if (arg == "--index") {
            if (value.empty() || value == "." || value == ".." || value.find_first_of("/\\:") != std::string::npos)
                throw Error("--index must be one simple directory name");
            if (value == "source") throw Error("--index cannot be source");
            o.index_name = value;
        } else if (arg == "--files") o.files = positive(value);
        else if (arg == "--lines") o.lines = positive(value);
        else if (arg == "--threads") o.threads = positive(value);
        else if (arg == "--memory-mb") o.memory_mb = positive(value);
        else if (arg == "--repeats") o.repeats = positive(value);
        else throw Error("Unknown benchmark option: " + arg);
    }
    if (o.lines < 4) throw Error("--lines must be at least 4");
    if (o.phase != "all" && o.phase != "generate" && o.phase != "create"
        && o.phase != "update" && o.phase != "search" && o.phase != "verify")
        throw Error("Unknown benchmark phase");
    std::cout << std::fixed << std::setprecision(6);
    std::cout << "CONFIG cpp=20 threads=" << o.threads << " memory_mb=" << o.memory_mb
              << " profile=" << o.profile << " hardware_concurrency_hint=" << std::thread::hardware_concurrency() << '\n';
    if (o.phase == "all" || o.phase == "generate") generate(o);
    if (o.phase == "generate") return 0;
    read_meta(o);
    const auto directory = o.work / o.index_name;
    BuildOptions bo;
    bo.threads = o.threads;
    bo.memory_mb = o.memory_mb;
    bo.profile = o.profile;
    if (o.phase == "all" || o.phase == "create") {
        const auto report = create_index(o.work / "source", directory, bo);
        build_report("create", report);
        if (report.files != o.files || report.lines != o.files * o.lines)
            throw Error("Corpus count mismatch in benchmark index");
    }
    if (o.phase == "all") {
        const auto report = update_index(directory, bo);
        build_report("strict_noop", report);
        if (report.tokenized != 0) throw Error("No-op benchmark re-tokenized unchanged files");
    }
    if (o.phase == "all" || o.phase == "update") update_corpus(o, directory, bo);
    if (o.phase == "all" || o.phase == "search") run_searches(o, directory);
    if (o.phase == "all" || o.phase == "verify") {
        const auto start = Clock::now();
        const auto verified = Index(directory, o.memory_mb).verify();
        std::cout << "VERIFY seconds=" << elapsed(start) << " files=" << verified.files
                  << " segments=" << verified.segments << " bytes_checked=" << verified.bytes_checked << '\n';
    }
    return 0;
}
}
#ifdef _MSC_VER
int wmain(int argc, wchar_t* argv[]) {
    try {
        std::vector<std::string> args;
        for (int i = 0; i < argc; ++i) args.push_back(researchify::path_to_utf8(researchify::fs::path(argv[i])));
        return run(args);
    }
    catch (const std::exception& error) { std::cerr << "benchmark error: " << error.what() << '\n'; return 2; }
}
#else
int main(int argc, char* argv[]) {
    try { return run(std::vector<std::string>(argv, argv + argc)); }
    catch (const std::exception& error) { std::cerr << "benchmark error: " << error.what() << '\n'; return 2; }
}
#endif
