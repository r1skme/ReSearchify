#include "researchify/engine.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <future>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {
using namespace researchify;
using Test = void (*)();
std::string executable;
std::atomic<unsigned> serial{0};

[[noreturn]] void fail(const std::string& message, const char* file, int line) {
    throw std::runtime_error(std::string(file) + ':' + std::to_string(line) + ": " + message);
}
#define CHECK(expression) do { if (!(expression)) fail(#expression, __FILE__, __LINE__); } while (false)
template<class F> void throws(F&& operation) {
    bool did_throw = false;
    try { operation(); } catch (const std::exception&) { did_throw = true; }
    CHECK(did_throw);
}
std::string utf8(const std::u8string& s) {
    return std::string(reinterpret_cast<const char*>(s.data()), s.size());
}
struct Workspace {
    fs::path base, root, index;
    explicit Workspace(bool in_current_directory = false) {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        for (unsigned attempt = 0; attempt != 100; ++attempt) {
            base = (in_current_directory ? fs::current_path() : fs::temp_directory_path())
                   / ("cs_engine_test_" + std::to_string(stamp)
                   + '_' + std::to_string(serial.fetch_add(1)));
            if (fs::create_directory(base)) break;
            if (attempt == 99) throw Error("Cannot allocate test workspace");
        }
        root = base / "source";
        index = base / "index";
        fs::create_directory(root);
    }
    ~Workspace() { std::error_code ec; fs::remove_all(base, ec); }
    Workspace(const Workspace&) = delete;
    Workspace& operator=(const Workspace&) = delete;
};
BuildOptions options() {
    BuildOptions result;
    result.threads = 4;
    result.memory_mb = 64;
    return result;
}
void write(const fs::path& path, const std::string& text) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    CHECK(out.good());
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    out.flush();
    CHECK(out.good());
    out.close();
    CHECK(out.good());
}
void flip(const fs::path& path, std::uint64_t offset) {
    std::fstream file(path, std::ios::binary | std::ios::in | std::ios::out);
    CHECK(file.good());
    file.seekg(static_cast<std::streamoff>(offset));
    char c = 0;
    file.get(c);
    CHECK(file.good());
    file.seekp(static_cast<std::streamoff>(offset));
    c = static_cast<char>(static_cast<unsigned char>(c) ^ 0x5Au);
    file.put(c);
    file.flush();
    CHECK(file.good());
}
fs::path generation_file(const fs::path& index, const std::string& prefix, std::uint64_t generation) {
    return index / (prefix + '-' + std::to_string(generation) + ".bin");
}
fs::path segment_file(const fs::path& index, const std::string& filename) {
    for (const auto& entry : fs::recursive_directory_iterator(index))
        if (entry.is_regular_file() && entry.path().filename() == filename
            && entry.path().parent_path().filename().string().starts_with("segment-")) return entry.path();
    throw Error("Test segment file missing: " + filename);
}
bool contains_path(const SearchReport& report, const std::string& suffix) {
    return std::any_of(report.hits.begin(), report.hits.end(), [&](const auto& h) { return h.path.ends_with(suffix); });
}
void basic_create_stats_verify() {
    Workspace w;
    write(w.root / "a.cpp", "alpha beta\nsecond\n");
    write(w.root / "b.cpp", "beta gamma\n");
    const auto b = create_index(w.root, w.index, options());
    CHECK(b.files == 2 && b.lines == 3 && b.added == 2 && b.tokenized == 2);
    const Index index(w.index);
    CHECK(index.stats().files == 2);
    CHECK(index.stats().lines == 3);
    CHECK(index.stats().tokens == 5);
    CHECK(index.stats().bytes == 29);
    CHECK(index.search("alpha").total_matches == 1);
    CHECK(index.search("beta").total_matches == 2);
    CHECK(index.verify().files == 2);
}
void empty_root() {
    Workspace w;
    const auto b = create_index(w.root, w.index, options());
    CHECK(b.files == 0 && b.lines == 0);
    const Index i(w.index);
    CHECK(i.search("absent").total_matches == 0);
    CHECK(i.search("NOT absent").total_matches == 0);
    CHECK(i.verify().files == 0);
}
void empty_file() {
    Workspace w;
    write(w.root / "empty.cpp", "");
    const auto b = create_index(w.root, w.index, options());
    CHECK(b.files == 1 && b.lines == 0);
    CHECK(Index(w.index).search("NOT absent").total_matches == 1);
}
void whole_tokens_and_case() {
    Workspace w;
    write(w.root / "a", "foo\n");
    write(w.root / "b", "foobar foo_bar Foo\n");
    create_index(w.root, w.index, options());
    const Index i(w.index);
    CHECK(i.search("foo").total_matches == 1);
    CHECK(i.search("Foo").total_matches == 1);
    CHECK(i.search("FOO").total_matches == 0);
    CHECK(i.search("bar").total_matches == 0);
}
void boolean_precedence() {
    Workspace w;
    write(w.root / "a", "alpha beta\n");
    write(w.root / "b", "beta gamma\n");
    write(w.root / "c", "gamma\n");
    create_index(w.root, w.index, options());
    const Index i(w.index);
    CHECK(i.search("alpha OR beta AND gamma").total_matches == 2);
    CHECK(i.search("(alpha OR beta) AND gamma").total_matches == 1);
    CHECK(i.search("NOT beta").total_matches == 1);
    CHECK(i.search("NOT NOT beta").total_matches == 2);
    CHECK(i.search("beta NOT alpha").total_matches == 1);
    CHECK(i.search("alpha OR NOT gamma").total_matches == 1);
}
void exact_phrase_and_regex() {
    Workspace w;
    write(w.root / "a", "std::vector<int> values;\nworker123 /path\n");
    write(w.root / "b", "std::vector< int > values;\nworkerABC\n");
    create_index(w.root, w.index, options());
    const Index i(w.index);
    CHECK(i.search("\"std::vector<int>\"").total_matches == 1);
    CHECK(i.search("/worker[0-9]+/").total_matches == 1);
    CHECK(i.search("/\\/path/").total_matches == 1);
    CHECK(i.search("values AND /worker[A-Z]+/").total_matches == 1);
    CHECK(i.search("NOT /worker[0-9]+/").total_matches == 1);
}
void file_scope_line_phrases() {
    Workspace w;
    write(w.root / "a", "alpha\nbeta\n");
    write(w.root / "b", "alpha beta\n");
    create_index(w.root, w.index, options());
    const Index i(w.index);
    CHECK(i.search("alpha AND beta").total_matches == 2);
    CHECK(i.search("\"alpha beta\"").total_matches == 1);
    CHECK(i.search("/alpha.*beta/").total_matches == 1);
}
void ranking_and_stability() {
    Workspace w;
    write(w.root / "early", "needle\nx\nx\nx\n");
    write(w.root / "late", "x\nx\nx\nneedle\n");
    create_index(w.root, w.index, options());
    const Index i(w.index);
    const auto a = i.search("needle"), b = i.search("needle");
    CHECK(a.hits.size() == 2);
    CHECK(a.hits[0].path.ends_with("early"));
    CHECK(a.hits[0].score > a.hits[1].score);
    CHECK(a.hits[0].score == b.hits[0].score);
    CHECK(a.hits[0].snippets.at(0).line == 1);
    CHECK(a.hits[1].snippets.at(0).line == 4);
}
void incremental_noop() {
    Workspace w;
    write(w.root / "a", "alpha\n");
    write(w.root / "b", "beta\n");
    create_index(w.root, w.index, options());
    const auto b = update_index(w.index, options());
    CHECK(b.tokenized == 0 && b.unchanged == 2 && b.added == 0 && b.changed == 0);
    CHECK(Index(w.index).search("alpha OR beta").total_matches == 2);
}
void incremental_add_change_delete() {
    Workspace w;
    write(w.root / "a", "alpha\n");
    write(w.root / "b", "beta\n");
    create_index(w.root, w.index, options());
    write(w.root / "a", "gamma\n");
    write(w.root / "c", "delta\n");
    fs::remove(w.root / "b");
    const auto b = update_index(w.index, options());
    CHECK(b.files == 2 && b.added == 1 && b.changed == 1 && b.removed == 1 && b.tokenized == 2);
    const Index i(w.index);
    CHECK(i.search("alpha OR beta").total_matches == 0);
    CHECK(i.search("gamma OR delta").total_matches == 2);
    CHECK(i.verify().files == 2);
}
void strict_same_timestamp_same_size() {
    Workspace w;
    write(w.root / "a", "alpha\n");
    const auto timestamp = fs::last_write_time(w.root / "a");
    create_index(w.root, w.index, options());
    write(w.root / "a", "omega\n");
    fs::last_write_time(w.root / "a", timestamp);
    const auto b = update_index(w.index, options());
    CHECK(b.changed == 1 && b.tokenized == 1);
    CHECK(Index(w.index).search("omega").total_matches == 1);
    CHECK(Index(w.index).search("alpha").total_matches == 0);
}
void trust_mtime_is_explicit() {
    Workspace w;
    write(w.root / "a", "alpha\n");
    const auto timestamp = fs::last_write_time(w.root / "a");
    create_index(w.root, w.index, options());
    write(w.root / "a", "omega\n");
    fs::last_write_time(w.root / "a", timestamp);
    auto fast = options();
    fast.trust_mtime = true;
    CHECK(update_index(w.index, fast).tokenized == 0);
    CHECK(Index(w.index).search("alpha").total_matches == 1);
    CHECK(update_index(w.index, options()).tokenized == 1);
    CHECK(Index(w.index).search("omega").total_matches == 1);
}
void remove_all_files() {
    Workspace w;
    write(w.root / "a", "alpha\n");
    create_index(w.root, w.index, options());
    fs::remove(w.root / "a");
    const auto b = update_index(w.index, options());
    CHECK(b.files == 0 && b.removed == 1 && b.tokenized == 0);
    CHECK(Index(w.index).search("NOT alpha").total_matches == 0);
}
void unicode_paths_and_contents() {
    Workspace w;
    const auto relative = utf8(u8"директория/исходник_猫.cpp");
    write(w.root / path_from_utf8(relative), utf8(u8"int число = 42;\n// привет мир\n"));
    create_index(w.root, w.index, options());
    const Index i(w.index);
    const auto result = i.search(utf8(u8"число"));
    CHECK(result.total_matches == 1);
    CHECK(contains_path(result, relative));
    CHECK(result.hits[0].snippets[0].column == 5);
    CHECK(i.search(utf8(u8"\"привет мир\"")).total_matches == 1);
    CHECK(i.search(utf8(u8"/привет/" )).total_matches == 1);
    CHECK(path_to_utf8(path_from_utf8(relative)) == relative);
}
void bom_crlf_and_final_line() {
    Workspace w;
    write(w.root / "bom", "\xEF\xBB\xBF" "alpha\r\nbeta\r\n");
    write(w.root / "final", "gamma");
    const auto b = create_index(w.root, w.index, options());
    CHECK(b.lines == 3);
    CHECK(Index(w.index).search("alpha").total_matches == 1);
    CHECK(Index(w.index).search("beta").hits.at(0).snippets.at(0).line == 2);
    CHECK(Index(w.index).search("gamma").total_matches == 1);
}
void binary_and_invalid_utf8() {
    Workspace w;
    write(w.root / "valid", "valid_token\n");
    write(w.root / "nul", std::string("alpha\0beta", 10));
    write(w.root / "invalid", "\xC0\xAF");
    write(w.root / "surrogate", "\xED\xA0\x80");
    const auto b = create_index(w.root, w.index, options());
    CHECK(b.files == 1 && b.skipped == 3);
    CHECK(Index(w.index).search("valid_token").total_matches == 1);
    CHECK(Index(w.index).verify().files == 1);
}
void size_and_line_limits() {
    Workspace w;
    write(w.root / "valid", "valid\n");
    write(w.root / "large", std::string(1000, 'x'));
    write(w.root / "long", std::string(100, 'y'));
    auto o = options();
    o.max_file_bytes = 200;
    o.max_line_bytes = 32;
    const auto b = create_index(w.root, w.index, o);
    CHECK(b.files == 1 && b.skipped == 2);
    CHECK(Index(w.index).search("valid").total_matches == 1);
}
void excluded_directories_and_embedded_index() {
    Workspace w;
    write(w.root / "a", "alpha\n");
    write(w.root / ".git" / "secret", "hidden\n");
    write(w.root / "generated" / "b", "generated_token\n");
    w.index = w.root / "my_index";
    auto o = options();
    o.exclude_dirs.push_back("generated");
    CHECK(create_index(w.root, w.index, o).files == 1);
    CHECK(update_index(w.index, o).files == 1);
    CHECK(Index(w.index).search("hidden OR generated_token").total_matches == 0);
}
void immutable_snapshot() {
    Workspace w;
    write(w.root / "a", "old_token\n");
    create_index(w.root, w.index, options());
    const Index old(w.index);
    write(w.root / "a", "new_token\n");
    update_index(w.index, options());
    fs::remove(w.root / "a");
    CHECK(old.search("old_token").total_matches == 1);
    CHECK(old.search("new_token").total_matches == 0);
    const Index current(w.index);
    CHECK(current.search("new_token").total_matches == 1);
    CHECK(current.search("old_token").total_matches == 0);
    CHECK(current.stats().generation > old.stats().generation);
}
void top_k_total_matches() {
    Workspace w;
    for (int n = 0; n != 10; ++n) write(w.root / ("a" + std::to_string(n)), "common\n");
    create_index(w.root, w.index, options());
    SearchOptions s;
    s.limit = 3;
    const auto r = Index(w.index).search("common", s);
    CHECK(r.total_matches == 10 && r.hits.size() == 3);
    CHECK(r.hits[0].path < r.hits[1].path && r.hits[1].path < r.hits[2].path);
}
void scan_budget_is_an_error() {
    Workspace w;
    write(w.root / "a", "alpha beta gamma\n");
    create_index(w.root, w.index, options());
    SearchOptions s;
    s.max_scan_bytes = 2;
    throws([&] { Index(w.index).search("\"alpha\"", s); });
}
void invalid_queries_and_options() {
    Workspace w;
    write(w.root / "a", "alpha\n");
    auto o = options();
    o.threads = 0;
    throws([&] { create_index(w.root, w.index, o); });
    o = options();
    o.memory_mb = 0;
    throws([&] { create_index(w.root, w.index, o); });
    create_index(w.root, w.index, options());
    const Index i(w.index);
    for (const auto& query : {"", "alpha AND", "(alpha", "alpha OR )", "/[a-/"})
        throws([&] { i.search(query); });
}
void duplicate_create_and_missing_index() {
    Workspace w;
    throws([&] { Index unused(w.index); });
    write(w.root / "a", "alpha\n");
    create_index(w.root, w.index, options());
    throws([&] { create_index(w.root, w.index, options()); });
    CHECK(Index(w.index).search("alpha").total_matches == 1);
}
void corrupted_latest_commit_falls_back() {
    Workspace w;
    write(w.root / "a", "alpha\n");
    const auto first = create_index(w.root, w.index, options());
    write(w.root / "a", "omega\n");
    const auto latest = update_index(w.index, options());
    flip(generation_file(w.index, "commit", latest.generation), 0);
    const Index i(w.index);
    CHECK(i.stats().generation == first.generation && i.stats().recovered);
    CHECK(i.search("alpha").total_matches == 1);
}
void corrupted_only_commit_is_rejected() {
    Workspace w;
    write(w.root / "a", "alpha\n");
    const auto report = create_index(w.root, w.index, options());
    flip(generation_file(w.index, "commit", report.generation), 0);
    throws([&] { Index unused(w.index); });
}
void truncated_latest_catalog_is_detected() {
    Workspace w;
    write(w.root / "a", "alpha\n");
    const auto first = create_index(w.root, w.index, options());
    write(w.root / "a", "omega\n");
    const auto latest = update_index(w.index, options());
    bool changed = false;
    for (const auto& item : fs::directory_iterator(w.index)) {
        const auto name = path_to_utf8(item.path().filename());
        if (name.starts_with("catalog-" + std::to_string(latest.generation) + "-") && name.ends_with(".tbl")) {
            fs::resize_file(item.path(), 7); changed = true; break;
        }
    }
    CHECK(changed);
    const Index i(w.index);
    CHECK(i.generation() == latest.generation);
    throws([&] { i.verify(); });
    throws([&] { i.search("omega"); });
    CHECK(Index(w.index, 64, first.generation).search("alpha").total_matches == 1);
}
void corrupted_postings_detected() {
    Workspace w;
    write(w.root / "a", "alpha beta\n");
    create_index(w.root, w.index, options());
    const auto file = segment_file(w.index, "postings.bin");
    flip(file, fs::file_size(file) - 1);
    throws([&] { Index(w.index).verify(); });
}
void corrupted_content_detected() {
    Workspace w;
    write(w.root / "a", "alpha beta\n");
    create_index(w.root, w.index, options());
    fs::path blob;
    for (const auto& e : fs::recursive_directory_iterator(w.index))
        if (e.is_regular_file() && e.path().filename() == "content.bin" && e.file_size() != 0) {
            blob = e.path(); break;
        }
    CHECK(!blob.empty());
    flip(blob, fs::file_size(blob) - 1);
    throws([&] { Index(w.index).verify(); });
    throws([&] { Index(w.index).search("alpha"); });
}
void temporary_commit_is_ignored() {
    Workspace w;
    write(w.root / "a", "alpha\n");
    const auto first = create_index(w.root, w.index, options());
    write(w.index / "commit-999999.bin.tmp", "not a committed file");
    CHECK(Index(w.index).stats().generation == first.generation);
}
void concurrent_search_same_object() {
    Workspace w;
    for (int n = 0; n != 24; ++n)
        write(w.root / ("a" + std::to_string(n)), n % 2 ? "alpha beta\n" : "beta gamma\n");
    create_index(w.root, w.index, options());
    const Index i(w.index);
    std::atomic<bool> bad{false};
    std::vector<std::thread> readers;
    for (int n = 0; n != 8; ++n) readers.emplace_back([&] {
        try {
            for (int k = 0; k != 8; ++k)
                if (i.search("beta AND (alpha OR gamma)").total_matches != 24) bad = true;
        } catch (...) { bad = true; }
    });
    for (auto& t : readers) t.join();
    CHECK(!bad);
}
void short_path_catalog_regression() {
    Workspace w;
    for (int n = 0; n != 128; ++n) write(w.root / std::to_string(n), "common\n");
    create_index(w.root, w.index, options());
    const Index index(w.index);
    CHECK(index.stats().files == 128);
    CHECK(index.search("common").total_matches == 128);
    CHECK(index.verify().files == 128);
}
void concurrent_writer_lock_and_readers() {
    Workspace w;
    write(w.root / "a", "alpha\n");
    create_index(w.root, w.index, options());
    const Index before(w.index);
    write(w.root / "a", "omega\n");
    std::promise<void> entered, proceed;
    auto entered_future = entered.get_future();
    auto proceed_future = proceed.get_future();
    auto o = options();
    o.checkpoint = [&](const std::string& point) {
        if (point == "after_lock") { entered.set_value(); proceed_future.wait(); }
    };
    std::exception_ptr failure;
    std::thread writer([&] { try { update_index(w.index, o); } catch (...) { failure = std::current_exception(); } });
    const auto status = entered_future.wait_for(std::chrono::seconds(10));
    if (status != std::future_status::ready) {
        proceed.set_value(); writer.join();
        CHECK(status == std::future_status::ready);
    }
    bool rejected = false, readable = false;
    try { update_index(w.index, options()); } catch (const std::exception&) { rejected = true; }
    try { readable = Index(w.index).search("alpha").total_matches == 1; } catch (...) { readable = false; }
    proceed.set_value();
    writer.join();
    if (failure) std::rethrow_exception(failure);
    CHECK(rejected && readable);
    CHECK(before.search("alpha").total_matches == 1);
    CHECK(Index(w.index).search("omega").total_matches == 1);
}
void thrown_failpoints_preserve_generation() {
    const std::vector<std::string> checkpoints = {
        "after_lock", "after_scan", "after_segments", "after_catalog", "before_commit"
    };
    for (const auto& checkpoint : checkpoints) {
        Workspace w;
        write(w.root / "a", "alpha\n");
        const auto first = create_index(w.root, w.index, options());
        write(w.root / "a", "omega\n");
        auto o = options();
        o.checkpoint = [&](const auto& point) { if (point == checkpoint) throw Error("injected failure"); };
        throws([&] { update_index(w.index, o); });
        CHECK(Index(w.index).stats().generation == first.generation);
        CHECK(Index(w.index).search("alpha").total_matches == 1);
        CHECK(!fs::exists(w.index / "writer.lock"));
        CHECK(update_index(w.index, options()).tokenized == 1);
        CHECK(Index(w.index).search("omega").total_matches == 1);
    }
}
void thrown_after_commit_is_published() {
    Workspace w;
    write(w.root / "a", "alpha\n");
    create_index(w.root, w.index, options());
    write(w.root / "a", "omega\n");
    auto o = options();
    o.checkpoint = [](const auto& point) { if (point == "after_commit") throw Error("injected acknowledgement failure"); };
    throws([&] { update_index(w.index, o); });
    CHECK(Index(w.index).search("omega").total_matches == 1);
    CHECK(!fs::exists(w.index / "writer.lock"));
}
std::string shell_quote(const std::string& text) {
#ifdef _WIN32
    std::string result = "\"";
    std::size_t slashes = 0;
    for (const auto c : text) {
        if (c == '\\') { ++slashes; continue; }
        if (c == '"') { result.append(slashes * 2 + 1, '\\'); result += c; }
        else { result.append(slashes, '\\'); result += c; }
        slashes = 0;
    }
    result.append(slashes * 2, '\\');
    return result + '"';
#else
    std::string result = "'";
    for (const auto c : text) result += c == '\'' ? "'\\''" : std::string(1, c);
    return result + '\'';
#endif
}
std::string child_command(const Workspace& workspace, const std::string& mode,
                          const std::string& checkpoint = {}) {
    // ASCII relative arguments avoid std::system's narrow-character Windows
    // boundary even when the actual current directory or user name is Unicode.
#ifdef _WIN32
    const auto child_name = "crash_child.exe";
#else
    const auto child_name = "crash_child";
#endif
    const auto child = workspace.base / child_name;
    fs::copy_file(path_from_utf8(executable), child, fs::copy_options::overwrite_existing);
    const auto relative_child = fs::path(".") / workspace.base.filename() / child_name;
    const auto relative_index = fs::path(".") / workspace.base.filename() / "index";
    auto command = shell_quote(path_to_utf8(relative_child)) + ' ' + mode + ' '
                   + shell_quote(path_to_utf8(relative_index));
    if (!checkpoint.empty()) command += ' ' + shell_quote(checkpoint);
#ifdef _WIN32
    command = '"' + command + '"';
#endif
    return command;
}
void process_crash(const std::string& point, bool published) {
    Workspace w(true);
    write(w.root / "a", "alpha\n");
    const auto first = create_index(w.root, w.index, options());
    write(w.root / "a", "omega\n");
    const auto command = child_command(w, "--crash-child", point);
    const auto status = std::system(command.c_str());
    CHECK(status != -1 && status != 0);
    CHECK(fs::is_directory(w.index / "writer.lock"));
    const Index after(w.index);
    CHECK(after.search(published ? "omega" : "alpha").total_matches == 1);
    CHECK((after.stats().generation > first.generation) == published);
    throws([&] { update_index(w.index, options()); });
    recover_writer_lock(w.index);
    CHECK(!fs::exists(w.index / "writer.lock"));
    update_index(w.index, options());
    CHECK(Index(w.index).search("omega").total_matches == 1);
}
void process_crash_before_commit() { process_crash("before_commit", false); }
void process_crash_after_commit() { process_crash("after_commit", true); }
void independent_process_writer_exclusion() {
    Workspace w(true);
    write(w.root / "a", "alpha\n");
    create_index(w.root, w.index, options());
    write(w.root / "a", "omega\n");
    const auto command = child_command(w, "--expect-lock-child");
    auto o = options();
    bool probed = false;
    o.checkpoint = [&](const auto& stage) {
        if (stage == "after_lock") {
            CHECK(std::system(command.c_str()) == 0);
            probed = true;
        }
    };
    update_index(w.index, o);
    CHECK(probed);
    CHECK(Index(w.index).search("omega").total_matches == 1);
}
void source_disappears_after_enumeration() {
    Workspace w;
    write(w.root / "a", "alpha\n");
    const auto first = create_index(w.root, w.index, options());
    auto o = options();
    o.checkpoint = [&](const auto& stage) { if (stage == "before_source_read") fs::remove(w.root / "a"); };
    throws([&] { update_index(w.index, o); });
    CHECK(Index(w.index).stats().generation == first.generation);
    CHECK(Index(w.index).search("alpha").total_matches == 1);
}
void rename_file() {
    Workspace w;
    write(w.root / "a", "alpha\n");
    create_index(w.root, w.index, options());
    fs::rename(w.root / "a", w.root / "b");
    const auto report = update_index(w.index, options());
    CHECK(report.added == 1 && report.removed == 1);
    const auto found = Index(w.index).search("alpha");
    CHECK(found.total_matches == 1 && contains_path(found, "b"));
}
void many_lines_streamed() {
    Workspace w;
    std::ostringstream content;
    for (int i = 1; i < 10000; ++i) content << "int value_" << i << " = " << i << ";\n";
    content << "last_unique_token\n";
    write(w.root / "large.cpp", content.str());
    const auto report = create_index(w.root, w.index, options());
    CHECK(report.lines == 10000);
    const auto found = Index(w.index).search("last_unique_token");
    CHECK(found.total_matches == 1 && found.hits[0].snippets[0].line == 10000);
    CHECK(Index(w.index).verify().files == 1);
}
void modified_file_becomes_binary() {
    Workspace w;
    write(w.root / "a", "alpha\n");
    create_index(w.root, w.index, options());
    write(w.root / "a", std::string("x\0y", 3));
    const auto b = update_index(w.index, options());
    CHECK(b.files == 0 && b.skipped == 1);
    CHECK(Index(w.index).search("alpha").total_matches == 0);
}
void two_line_boundaries() {
    Workspace w;
    write(w.root / "one", "\n");
    write(w.root / "two", "\n\n");
    write(w.root / "three", "a\n\nb");
    const auto b = create_index(w.root, w.index, options());
    CHECK(b.lines == 6);
    CHECK(Index(w.index).search("b").hits.at(0).snippets.at(0).line == 3);
}
void indexed_data_survives_source_removal() {
    Workspace w;
    write(w.root / "a", "alpha beta\n");
    create_index(w.root, w.index, options());
    fs::remove_all(w.root);
    CHECK(Index(w.index).search("alpha AND beta").total_matches == 1);
    CHECK(Index(w.index).verify().files == 1);
    throws([&] { update_index(w.index, options()); });
    CHECK(Index(w.index).stats().files == 1);
}
void root_index_overlap_rejected() {
    Workspace w;
    write(w.root / "a", "alpha\n");
    throws([&] { create_index(w.root, w.root, options()); });
    CHECK(fs::exists(w.root / "a"));
}
}

int run_tests(const std::vector<std::string>& args) {
    if (args.size() == 4 && args[1] == "--crash-child") {
        auto o = options();
        const std::string point = args[3];
        o.checkpoint = [point](const auto& stage) { if (stage == point) std::_Exit(77); };
        try { update_index(path_from_utf8(args[2]), o); }
        catch (const std::exception& e) { std::cerr << "child error: " << e.what() << '\n'; return 91; }
        return 92;
    }
    if (args.size() == 3 && args[1] == "--expect-lock-child") {
        const auto directory = path_from_utf8(args[2]);
        try {
            if (Index(directory).search("alpha").total_matches != 1) return 93;
            update_index(directory, options());
        } catch (const Error& error) {
            return error.code() == "E_LOCKED" ? 0 : 94;
        }
        return 95;
    }
    executable = path_to_utf8(fs::absolute(path_from_utf8(args[0])));
    const std::vector<std::pair<std::string, Test>> tests = {
        {"basic_create_stats_verify", basic_create_stats_verify},
        {"empty_root", empty_root}, {"empty_file", empty_file},
        {"whole_tokens_and_case", whole_tokens_and_case},
        {"boolean_precedence", boolean_precedence},
        {"exact_phrase_and_regex", exact_phrase_and_regex},
        {"file_scope_line_phrases", file_scope_line_phrases},
        {"ranking_and_stability", ranking_and_stability},
        {"incremental_noop", incremental_noop},
        {"incremental_add_change_delete", incremental_add_change_delete},
        {"strict_same_timestamp_same_size", strict_same_timestamp_same_size},
        {"trust_mtime_is_explicit", trust_mtime_is_explicit},
        {"remove_all_files", remove_all_files},
        {"unicode_paths_and_contents", unicode_paths_and_contents},
        {"bom_crlf_and_final_line", bom_crlf_and_final_line},
        {"binary_and_invalid_utf8", binary_and_invalid_utf8},
        {"size_and_line_limits", size_and_line_limits},
        {"excluded_directories_and_embedded_index", excluded_directories_and_embedded_index},
        {"immutable_snapshot", immutable_snapshot},
        {"top_k_total_matches", top_k_total_matches},
        {"scan_budget_is_an_error", scan_budget_is_an_error},
        {"invalid_queries_and_options", invalid_queries_and_options},
        {"duplicate_create_and_missing_index", duplicate_create_and_missing_index},
        {"corrupted_latest_commit_falls_back", corrupted_latest_commit_falls_back},
        {"corrupted_only_commit_is_rejected", corrupted_only_commit_is_rejected},
        {"truncated_latest_catalog_is_detected", truncated_latest_catalog_is_detected},
        {"corrupted_postings_detected", corrupted_postings_detected},
        {"corrupted_content_detected", corrupted_content_detected},
        {"temporary_commit_is_ignored", temporary_commit_is_ignored},
        {"concurrent_search_same_object", concurrent_search_same_object},
        {"short_path_catalog_regression", short_path_catalog_regression},
        {"concurrent_writer_lock_and_readers", concurrent_writer_lock_and_readers},
        {"thrown_failpoints_preserve_generation", thrown_failpoints_preserve_generation},
        {"thrown_after_commit_is_published", thrown_after_commit_is_published},
        {"process_crash_before_commit", process_crash_before_commit},
        {"process_crash_after_commit", process_crash_after_commit},
        {"independent_process_writer_exclusion", independent_process_writer_exclusion},
        {"source_disappears_after_enumeration", source_disappears_after_enumeration},
        {"rename_file", rename_file}, {"many_lines_streamed", many_lines_streamed},
        {"modified_file_becomes_binary", modified_file_becomes_binary},
        {"two_line_boundaries", two_line_boundaries},
        {"indexed_data_survives_source_removal", indexed_data_survives_source_removal},
        {"root_index_overlap_rejected", root_index_overlap_rejected}
    };
    std::size_t failed = 0;
    for (const auto& [name, test] : tests) {
        std::cout << "[RUN ] " << name << std::endl;
        try { test(); std::cout << "[ OK ] " << name << '\n'; }
        catch (const std::exception& error) { ++failed; std::cerr << "[FAIL] " << name << ": " << error.what() << '\n'; }
    }
    std::cout << "engine tests=" << tests.size() << " passed=" << tests.size() - failed << " failed=" << failed << '\n';
    return failed == 0 ? 0 : 1;
}
#ifdef _MSC_VER
int wmain(int argc, wchar_t* argv[]) {
    try {
        std::vector<std::string> args;
        for (int i = 0; i < argc; ++i) args.push_back(path_to_utf8(fs::path(argv[i])));
        return run_tests(args);
    } catch (const std::exception& e) { std::cerr << "test startup error: " << e.what() << '\n'; return 2; }
}
#else
int main(int argc, char* argv[]) {
    try { return run_tests(std::vector<std::string>(argv, argv + argc)); }
    catch (const std::exception& e) { std::cerr << "test startup error: " << e.what() << '\n'; return 2; }
}
#endif
