#pragma once
#include <utility>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace researchify {
namespace fs = std::filesystem;
class Error : public std::runtime_error {
public:
    explicit Error(const std::string& message) : std::runtime_error(message) {}
    Error(std::string code, const std::string& message, bool retryable = false)
        : std::runtime_error(message), code_(std::move(code)), retryable_(retryable) {}
    const std::string& code() const noexcept { return code_; }
    bool retryable() const noexcept { return retryable_; }
private:
    std::string code_ = "E_IO";
    bool retryable_ = false;
};
struct BuildOptions {
    std::size_t threads = 16, memory_mb = 256;
    std::uint64_t max_file_bytes = 1024ULL * 1024 * 1024;
    std::size_t max_line_bytes = 1024 * 1024;
    bool trust_mtime = false;
    std::vector<std::string> exclude_dirs = {".git", ".svn", ".hg", ".researchify"};
    std::vector<std::string> exclude_paths;
    // compact: exact terms + snapshots; fast additionally indexes document trigrams.
    std::string profile = "compact";
    std::size_t segment_documents = 8192;
    bool changed_only = false;
    std::vector<std::string> changed_files;
    std::function<void(const std::string&)> checkpoint;
    bool profile_stages = false;
};
struct BuildProfile {
    bool enabled = false;
    // Thread-stage sums can overlap. Capture includes tokenization and writes;
    // enumeration includes producer backpressure. Do not add them to wall time.
    double enumeration_seconds = 0, queue_wait_seconds = 0, metadata_lookup_seconds = 0, validation_seconds = 0;
    double capture_seconds = 0, posting_write_seconds = 0, catalog_seconds = 0, commit_seconds = 0;
};
struct BuildReport {
    std::uint64_t generation = 0, files = 0, lines = 0, bytes = 0;
    std::uint64_t added = 0, changed = 0, unchanged = 0, removed = 0, skipped = 0;
    std::uint64_t hashed = 0, tokenized = 0, segments = 0;
    std::size_t workers = 0, memory_budget_bytes = 0;
    double seconds = 0;
    bool global_scan = true;
    std::vector<std::string> warnings;
    BuildProfile profile;
};
struct SearchOptions {
    std::size_t limit = 20, offset = 0;
    std::size_t snippets_per_file = 3, snippet_bytes = 240, memory_mb = 256;
    std::uint64_t max_scan_bytes = 0, max_regex_steps = 100000000, max_candidates = 0;
    std::vector<std::string> paths;
    bool sort_path = false;
    // 0 selects a bounded adaptive worker count; 1 forces serial execution.
    std::size_t threads = 0;
    // Stage timing is opt-in; disabled searches do not time each input line.
    bool profile = false;
    // Optional thread-safe cooperative cancellation predicate (C++ API).
    std::function<bool()> cancelled;
    // Keyset continuation keeps memory bounded independently of the page number.
    bool after = false;
    double after_score = 0;
    std::string after_path;
};
struct Snippet { std::uint64_t line = 0, column = 0; std::string text; };
struct SearchHit {
    std::string path, content_id;
    double score = 0;
    std::uint64_t matches = 0;
    std::vector<Snippet> snippets;
};
struct SearchProfile {
    bool enabled = false;
    std::size_t workers = 1;
    double planning_seconds = 0, candidate_seconds = 0, content_open_seconds = 0;
    double content_read_seconds = 0, verification_seconds = 0, line_processing_seconds = 0;
    double matching_seconds = 0, snippet_seconds = 0;
    std::uint64_t content_opens = 0, content_reads = 0, content_bytes = 0, regex_steps = 0;
    std::uint64_t filtered_documents = 0;
};
struct SearchReport {
    std::uint64_t generation = 0, total_matches = 0, remaining_matches = 0, candidates = 0, scanned_bytes = 0;
    double seconds = 0;
    std::vector<SearchHit> hits;
    SearchProfile profile;
};
struct Statistics {
    std::uint64_t generation = 0, files = 0, lines = 0, bytes = 0, tokens = 0;
    std::uint64_t segments = 0, index_bytes = 0, retained_generations = 0;
    std::string root, profile;
    bool recovered = false;
    std::vector<std::string> warnings;
};
struct VerifyReport { std::uint64_t generation = 0, segments = 0, files = 0, bytes_checked = 0; };
struct ReadReport {
    std::uint64_t generation = 0, first_line = 0, last_line = 0;
    std::string path, content_id, source, text;
    bool truncated = false;
};
struct FileInfo { std::string path, content_id; std::uint64_t bytes = 0, lines = 0; };
struct FileList { std::vector<FileInfo> files; std::uint64_t total = 0, generation = 0; };
struct GarbageReport { std::uint64_t generations_removed = 0, objects_removed = 0, bytes_removed = 0, pinned = 0; };
struct Query;
// Immutable snapshot. Reader pins protect its files from concurrent GC.
class Index {
public:
    explicit Index(const fs::path& directory, std::size_t memory_mb = 256, std::uint64_t generation = 0);
    SearchReport search(const std::string& query, const SearchOptions& options = {}) const;
    SearchReport search(const Query& query, const SearchOptions& options = {}) const;
    std::vector<std::string> warnings() const;
    std::uint64_t generation() const noexcept;
    Statistics stats(bool disk_usage = true) const;
    VerifyReport verify() const;
    BuildOptions policy() const;
    FileList files(const std::string& pattern = "**", std::size_t limit = 100,
                   const std::string& after = "") const;
    ReadReport read(const std::string& path, std::uint64_t first = 1, std::uint64_t last = 200,
                    const std::string& source = "snapshot", const std::string& expected_content = "",
                    std::size_t max_bytes = 65536) const;
private:
    struct Impl;
    std::shared_ptr<const Impl> impl_;
};
BuildReport create_index(const fs::path& root, const fs::path& directory, const BuildOptions& options = {});
BuildReport update_index(const fs::path& directory);
BuildReport update_index(const fs::path& directory, const BuildOptions& options);
BuildReport compact_index(const fs::path& directory, std::size_t threads = 16, std::size_t memory_mb = 256);
GarbageReport garbage_collect(const fs::path& directory, std::size_t keep_generations = 2);
void recover_writer_lock(const fs::path& directory);
// Only after ALL readers and writers have stopped. Never called automatically.
void recover_index(const fs::path& directory, bool clear_reader_pins = false);
std::string path_to_utf8(const fs::path& path);
fs::path path_from_utf8(const std::string& path);
bool match_path(const std::string& pattern, const std::string& path);
}
