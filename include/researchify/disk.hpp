#pragma once
#include "researchify/engine.hpp"
#include <functional>
#include <iosfwd>
#include <optional>
#include <string_view>
#include <utility>

namespace researchify::disk {
using Entry = std::pair<std::string, std::string>;
void varint(std::string& out, std::uint64_t value);
std::uint64_t get_varint(std::string_view data, std::size_t& at);
void string(std::string& out, std::string_view value);
std::string get_string(std::string_view data, std::size_t& at, std::size_t maximum = 1U << 20);
void u64(std::string& out, std::uint64_t value);
std::uint64_t get_u64(std::string_view data, std::size_t& at);
std::string ordered(std::uint64_t value);
std::uint64_t ordered(std::string_view value);
void write_file(const fs::path&, std::string_view payload);
std::string read_file(const fs::path&, std::size_t max_bytes);
void finish(std::ofstream&, const fs::path&);
struct DecodedPage;
class PageCache {
public:
    explicit PageCache(std::size_t bytes);
    ~PageCache();
    std::shared_ptr<const std::string> read(const fs::path&, std::uint64_t offset, std::size_t length) const;
    std::shared_ptr<const DecodedPage> node(const fs::path&, std::uint64_t offset, std::uint64_t file_size) const;
    void clear();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
// Prefix-compressed, CRC protected, immutable B+tree; no mmap or third-party code.
class TableWriter {
public:
    explicit TableWriter(const fs::path&, std::size_t page_bytes = 16384);
    ~TableWriter();
    void add(std::string_view key, std::string_view value);
    void finish();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
class Table {
    struct Impl;
public:
    // One consumer owns each cursor. It only remembers a weak reference to a
    // checked leaf: cache eviction releases its memory without waiting for the
    // cursor or the search to finish. The immutable table identity prevents a
    // cursor reused with another Table from serving values from its old leaf.
    class Cursor {
        friend class Table;
        std::shared_ptr<const Impl> owner_;
        std::weak_ptr<const DecodedPage> leaf_;
    };
    Table(const fs::path&, std::shared_ptr<PageCache>);
    std::optional<std::string> get(std::string_view key) const;
    // The value is borrowed only for the duration of visitor. Non-monotone
    // lookups and expired/out-of-range leaves use the normal root traversal.
    bool get(std::string_view key, Cursor&,
             const std::function<void(std::string_view)>& visitor) const;
    void each(const std::function<void(std::string_view, std::string_view)>& visitor,
              std::string_view prefix = {}) const;
    std::uint64_t verify() const;
    std::uint64_t count() const;
private:
    std::shared_ptr<Impl> impl_;
};
// Runs are private to one transaction. Duplicate keys are combined on every pass.
class Sorter {
public:
    using Combine = std::function<void(std::string&, const std::string&)>;
    Sorter(const fs::path& work, std::size_t budget, Combine combine);
    ~Sorter();
    void add(std::string key, std::string value);
    void finish(const std::function<void(std::string_view, std::string_view)>& visitor);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
struct Posting { std::uint64_t document = 0, frequency = 0, line = 0, column = 0; };
std::string posting_value(const Posting&);
Posting posting_value(std::string_view);
void combine_posting(std::string& a, const std::string& b);
// Sorted input keys = full term bytes + NUL + big-endian docID. No hash collisions.
class PostingWriter {
public:
    explicit PostingWriter(const fs::path& directory);
    ~PostingWriter();
    void add(std::string_view key, std::string_view value);
    void finish();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
class PostingCursor {
public:
    PostingCursor(const fs::path& directory, std::shared_ptr<PageCache> cache, std::string_view descriptor);
    ~PostingCursor();
    PostingCursor(PostingCursor&&) noexcept;
    PostingCursor& operator=(PostingCursor&&) noexcept;
    bool seek(std::uint64_t document);
    const Posting& current() const;
    std::uint64_t count() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
std::uint64_t posting_count(std::string_view descriptor);
std::uint64_t verify_postings(const fs::path&, std::shared_ptr<PageCache>);
}
