#include "researchify/engine.hpp"
#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <ctime>
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
    std::size_t files = 100000, lines = 50, threads = 16, memory_mb = 256, repeats = 21;
    std::size_t search_threads = 0;
    bool stages = false;
    std::string query_name;
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
double cpu_elapsed(std::clock_t start) {
#ifdef _WIN32
    // The Windows CRT clock() measures wall time, not process CPU time.
    // scripts/perf_benchmark.py uses GetProcessTimes for Windows CPU telemetry.
    (void)start;
    return -1;
#else
    const auto end = std::clock();
    if (start == std::clock_t(-1) || end == std::clock_t(-1)) return -1;
    return static_cast<double>(end - start) / CLOCKS_PER_SEC;
#endif
}
void build_report(const std::string& phase, const BuildReport& r, double cpu_seconds) {
    const auto rate = r.seconds > 0 ? static_cast<double>(r.bytes) / (1024.0 * 1024.0 * r.seconds) : 0;
    std::cout << "BUILD phase=" << phase << " generation=" << r.generation
              << " files=" << r.files << " lines=" << r.lines << " bytes=" << r.bytes
              << " added=" << r.added << " changed=" << r.changed
              << " removed=" << r.removed << " unchanged=" << r.unchanged
              << " skipped=" << r.skipped << " hashed=" << r.hashed
              << " tokenized=" << r.tokenized << " workers=" << r.workers
              << " memory_budget_bytes=" << r.memory_budget_bytes
              << " profiled=" << (r.profile.enabled ? 1 : 0)
              << " seconds=" << r.seconds << " cpu_available=" << (cpu_seconds >= 0 ? 1 : 0)
              << " cpu_seconds=" << cpu_seconds
              << " corpus_mib_per_second=" << rate << '\n';
    if (r.profile.enabled) {
        const auto& p = r.profile;
        std::cout << "BUILD_PROFILE phase=" << phase << " overlapping_thread_sums=true instrumentation_overhead=true"
                  << " enumeration_seconds=" << p.enumeration_seconds << " queue_wait_seconds=" << p.queue_wait_seconds
                  << " validation_seconds=" << p.validation_seconds << " capture_seconds=" << p.capture_seconds
                  << " posting_write_seconds=" << p.posting_write_seconds << " catalog_seconds=" << p.catalog_seconds
                  << " commit_seconds=" << p.commit_seconds << '\n';
    }
}
struct Distribution { double median, p95, minimum, maximum; };
Distribution distribution(std::vector<double> values) {
    std::sort(values.begin(), values.end());
    const auto p95 = static_cast<std::size_t>(std::ceil(0.95 * static_cast<double>(values.size()))) - 1;
    const auto median = values.size() % 2 ? values[values.size() / 2]
        : (values[values.size() / 2 - 1] + values[values.size() / 2]) / 2;
    return {median, values[p95], values.front(), values.back()};
}
bool same_results(const SearchReport& a, const SearchReport& b) {
    if (a.total_matches != b.total_matches || a.hits.size() != b.hits.size()) return false;
    for (std::size_t i = 0; i < a.hits.size(); ++i) {
        const auto& x = a.hits[i];
        const auto& y = b.hits[i];
        if (x.path != y.path || x.content_id != y.content_id || x.score != y.score
            || x.matches != y.matches || x.snippets.size() != y.snippets.size()) return false;
        for (std::size_t j = 0; j < x.snippets.size(); ++j) {
            const auto& p = x.snippets[j];
            const auto& q = y.snippets[j];
            if (p.line != q.line || p.column != q.column || p.text != q.text) return false;
        }
    }
    return true;
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
    struct QueryCase { std::string name, query; };
    const std::vector<QueryCase> queries = {
        {"term", needle},
        {"literal", "\"" + needle + " = 0;\""},
        {"short_literal", "\"0;\""},
        {"no_match", "\"ReSearchify_missing_2026\""},
        {"and", "bucket" + std::to_string(id % 100) + " AND common"},
        {"or", needle + " OR needle_" + std::to_string(id2)},
        {"not", needle + " AND NOT absent_symbol"},
        {"phrase", needle + " AND \"return common;\""},
        {"regex", needle + " AND /function_[0-9]+_[0-9]+/"},
        {"broad_term", "common"},
        {"broad_literal", "\"return common;\""},
        {"broad_regex", "/needle_[0-9]+/"},
        {"regex_no_match", "/missing_[0-9]+/"}
    };
    SearchOptions so;
    so.limit = 20;
    so.memory_mb = o.memory_mb;
    so.threads = o.search_threads;
    // Large corpus scans are intentional. Their work is reported separately.
    so.max_regex_steps = std::numeric_limits<std::uint64_t>::max();
    bool selected = false;
    for (const auto& q : queries) {
        if (!o.query_name.empty() && o.query_name != q.name) continue;
        selected = true;
        std::vector<double> values, cpu_values;
        SearchReport last;
        // This warm-up is explicit, and measured results below are warm-cache.
        const auto reference = index.search(q.query, so);
        for (std::size_t iteration = 0; iteration < o.repeats; ++iteration) {
            const auto cpu_start = std::clock();
            const auto start = Clock::now();
            last = index.search(q.query, so);
            values.push_back(elapsed(start) * 1000.0);
            const auto cpu_seconds = cpu_elapsed(cpu_start);
            if (cpu_seconds >= 0) cpu_values.push_back(cpu_seconds * 1000.0);
            if (!same_results(reference, last)) throw Error("Nondeterministic results in benchmark: " + q.name);
        }
        const auto wall = distribution(values);
        const auto cpu_available = cpu_values.size() == values.size();
        const auto cpu = cpu_available ? distribution(cpu_values) : Distribution{-1, -1, -1, -1};
        std::cout << "QUERY name=" << q.name << " iterations=" << values.size()
                  << " warmup=1 cache=warm process=reused p50_ms=" << wall.median
                  << " p95_ms=" << wall.p95 << " min_ms=" << wall.minimum
                  << " max_ms=" << wall.maximum << " cpu_available=" << (cpu_available ? 1 : 0)
                  << " cpu_p50_ms=" << cpu.median
                  << " cpu_p95_ms=" << cpu.p95 << " cpu_min_ms=" << cpu.minimum
                  << " cpu_max_ms=" << cpu.maximum << " matches=" << last.total_matches
                  << " candidates=" << last.candidates << " scanned_bytes=" << last.scanned_bytes << '\n';
        // Instrumentation has overhead: never mix this sample into latency statistics.
        if (o.stages) {
            so.profile = true;
            const auto profiled = index.search(q.query, so);
            so.profile = false;
            if (!same_results(reference, profiled)) throw Error("Profiling changed results: " + q.name);
            const auto& p = profiled.profile;
            std::cout << "PROFILE name=" << q.name << " timing_sample=false"
                      << " planning_seconds=" << p.planning_seconds << " candidate_seconds=" << p.candidate_seconds
                      << " content_open_seconds=" << p.content_open_seconds << " content_read_seconds=" << p.content_read_seconds
                      << " verification_seconds=" << p.verification_seconds << " line_processing_seconds=" << p.line_processing_seconds
                      << " matching_seconds=" << p.matching_seconds << " snippet_seconds=" << p.snippet_seconds
                      << " content_opens=" << p.content_opens << " content_reads=" << p.content_reads
                      << " content_bytes=" << p.content_bytes << " filtered_documents=" << p.filtered_documents
                      << " regex_steps=" << p.regex_steps << " workers=" << p.workers << '\n';
        }
    }
    if (!selected) throw Error("Unknown --query benchmark case: " + o.query_name);
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
    const auto cpu_start = std::clock();
    const auto report = update_index(directory, options);
    build_report("update_one_percent", report, cpu_elapsed(cpu_start));
    if (report.tokenized != changed) throw Error("Incremental benchmark unexpectedly re-tokenized files");
}
int run(const std::vector<std::string>& args) {
    Options o;
    for (std::size_t i = 1; i < args.size(); ++i) {
        const std::string arg = args[i];
        if (arg == "--help") {
            std::cout << "cs_bench [--work DIR] [--phase all|generate|create|update|search|verify]\n"
                      << "         [--index NAME] [--files 100000] [--lines 50]\n"
                      << "         [--threads 16] [--memory-mb 256] [--repeats 21] [--profile compact|fast]\n"
                      << "         [--search-threads 0] [--query CASE] [--stages 0|1]\n"
                      << "Default all: generate, create, strict no-op update, update 1%, searches, verify.\n"
                      << "Generation refuses existing corpus. Other phases read benchmark.meta.\n"
                      << "Use separate --index names to compare thread counts on the same corpus.\n"
                      << "Every query, including broad regex, has one warmup and --repeats samples.\n"
                      << "--stages adds a separate profiled search, excluded from the timing distribution.\n"
                      << "Builds with --stages 1 are explicitly profiled; repeat with 0 for headline build timing.\n"
                      << "Use scripts/perf_benchmark.py for fresh-process/JSONL latency, CPU and peak RSS.\n";
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
        else if (arg == "--query") o.query_name = value;
        else if (arg == "--search-threads") o.search_threads = value == "0" ? 0 : positive(value);
        else if (arg == "--stages") {
            if (value != "0" && value != "1") throw Error("--stages expects 0 or 1");
            o.stages = value == "1";
        }
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
    bo.profile_stages = o.stages;
    if (o.phase == "all" || o.phase == "create") {
        const auto cpu_start = std::clock();
        const auto report = create_index(o.work / "source", directory, bo);
        build_report("create", report, cpu_elapsed(cpu_start));
        if (report.files != o.files || report.lines != o.files * o.lines)
            throw Error("Corpus count mismatch in benchmark index");
    }
    if (o.phase == "all") {
        const auto cpu_start = std::clock();
        const auto report = update_index(directory, bo);
        build_report("strict_noop", report, cpu_elapsed(cpu_start));
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
