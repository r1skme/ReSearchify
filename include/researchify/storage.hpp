#pragma once
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>
#include <functional>
#include <iosfwd>

namespace researchify::storage {
namespace fs = std::filesystem;
std::uint32_t crc32(std::string_view data, std::uint32_t previous = 0);
struct Fingerprint {
    std::uint64_t a = 0, b = 0;
    bool operator==(const Fingerprint&) const = default;
};
class Hasher {
public:
    Hasher();
    void add(std::string_view bytes);
    Fingerprint finish() const;
private:
    std::uint64_t a_, b_, count_ = 0;
};
std::uint64_t term_hash(std::string_view token);
void put_u32(std::ostream&, std::uint32_t);
void put_u64(std::ostream&, std::uint64_t);
std::uint32_t get_u32(std::istream&);
std::uint64_t get_u64(std::istream&);
void put_string(std::ostream&, const std::string&);
std::string get_string(std::istream&, std::size_t max_bytes);
std::uint32_t file_crc(const fs::path&);
std::uint64_t checked_file_size(const fs::path&);

struct Posting {
    std::uint64_t hash = 0, document = 0;
    std::uint64_t frequency = 0;
    std::uint32_t line = 0, column = 0;
};
// Bounded external sorting. Equal (hash,document) records are aggregated.
// merge_to accepts all final runs (possibly from multiple workers), uses <=32
// open inputs, and emits a block-checksummed postings file for binary search.
class ExternalSorter {
public:
    ExternalSorter(const fs::path& work_dir, std::string prefix, std::size_t buffer_bytes);
    ~ExternalSorter();
    ExternalSorter(const ExternalSorter&) = delete;
    ExternalSorter& operator=(const ExternalSorter&) = delete;
    void add(Posting);
    std::vector<fs::path> finish();
    static void merge_to(const std::vector<fs::path>& runs, const fs::path& output,
                         const fs::path& work_dir);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
class PostingReader {
public:
    explicit PostingReader(const fs::path& file);
    ~PostingReader();
    PostingReader(PostingReader&&) noexcept;
    PostingReader& operator=(PostingReader&&) noexcept;
    PostingReader(const PostingReader&) = delete;
    PostingReader& operator=(const PostingReader&) = delete;
    void lookup(std::uint64_t hash, const std::function<void(const Posting&)>& visitor);
    std::uint64_t verify(); // Checks all block CRCs, bounds and sort order.
    std::uint64_t count() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
