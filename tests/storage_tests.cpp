#include "researchify/engine.hpp"
#include "researchify/storage.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
#include <map>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace fs = std::filesystem;
using researchify::Error;
using namespace researchify::storage;

namespace {
void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

template<class Function> void rejects(Function&& function) {
    bool rejected = false;
    try { function(); } catch (const Error&) { rejected = true; }
    require(rejected, "Expected researchify::Error");
}

struct Temporary {
    fs::path path;
    Temporary() {
        const auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
        for (unsigned attempt = 0; attempt < 100; ++attempt) {
            path = fs::temp_directory_path() / ("researchify-storage-tests-" +
                std::to_string(tick) + "-" + std::to_string(attempt));
            if (fs::create_directory(path)) return;
        }
        throw std::runtime_error("Cannot allocate test directory");
    }
    ~Temporary() { std::error_code ignored; fs::remove_all(path, ignored); }
};

std::string read_file(const fs::path& path) {
    std::ifstream stream(path, std::ios::binary);
    require(static_cast<bool>(stream), "Cannot open test data");
    return std::string(std::istreambuf_iterator<char>(stream), {});
}

void write_file(const fs::path& path, const std::string& bytes) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    stream.close();
    require(!stream.fail(), "Cannot write test data");
}

void set32(std::string& bytes, std::size_t offset, std::uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) bytes.at(offset + i) = static_cast<char>((value >> (8 * i)) & 255U);
}

void set64(std::string& bytes, std::size_t offset, std::uint64_t value) {
    for (unsigned i = 0; i < 8; ++i) bytes.at(offset + i) = static_cast<char>((value >> (8 * i)) & 255U);
}

void repair_header_crc(std::string& bytes) { set32(bytes, 56, crc32(std::string_view(bytes.data(), 56))); }
void repair_block_crc(std::string& bytes, std::size_t block = 0) {
    const auto offset = 64 + block * 8200;
    set32(bytes, offset + 8196, crc32(std::string_view(bytes.data() + offset, 8196)));
}

fs::path make_postings(const fs::path& root, const std::vector<Posting>& postings,
                        std::size_t memory = 1024, std::string name = "postings.bin") {
    ExternalSorter sorter(root, "test", memory);
    for (const auto& posting : postings) sorter.add(posting);
    const auto runs = sorter.finish();
    const auto result = root / name;
    ExternalSorter::merge_to(runs, result, root);
    return result;
}

std::vector<Posting> lookup(const fs::path& path, std::uint64_t hash) {
    PostingReader reader(path);
    std::vector<Posting> result;
    reader.lookup(hash, [&](const Posting& posting) { result.push_back(posting); });
    return result;
}

void same_posting(const Posting& a, const Posting& b) {
    require(a.hash == b.hash && a.document == b.document && a.frequency == b.frequency &&
            a.line == b.line && a.column == b.column, "Posting mismatch");
}
}

int main() {
    using Test = std::pair<std::string, std::function<void()>>;
    const std::vector<Test> tests = {
        {"crc32_standard_and_incremental", [] {
            require(crc32("") == 0, "Empty CRC");
            require(crc32("123456789") == 0xcbf43926U, "CRC reference vector");
            std::uint32_t value = 0;
            for (const auto byte : std::string("123456789")) value = crc32(std::string_view(&byte, 1), value);
            require(value == 0xcbf43926U, "Incremental CRC");
            require(crc32("", value) == value, "Empty incremental CRC");
        }},
        {"fingerprint_chunk_independence_and_stability", [] {
            Hasher full, chunked;
            std::string text(200000, '\0');
            for (std::size_t i = 0; i < text.size(); ++i) text[i] = static_cast<char>(i & 255U);
            full.add(text);
            for (std::size_t i = 0; i < text.size(); i += 97)
                chunked.add(std::string_view(text).substr(i, 97));
            require(full.finish() == chunked.finish(), "Hash changes with chunk boundaries");
            require(full.finish() == full.finish(), "finish mutates state");
            Hasher vector;
            vector.add("123456789");
            require(vector.finish() == Fingerprint{0xcf5ac12168adf70fULL, 0xbb0a0033f56ef3bfULL},
                    "Fingerprint stable format reference");
            require(term_hash("hello") == 0x32b37ad5d165220fULL, "Term hash format reference");
            require(term_hash("hello") != term_hash("Hello"), "Term matching is case sensitive");
        }},
        {"integer_and_string_wire_roundtrip", [] {
            std::stringstream stream(std::ios::in | std::ios::out | std::ios::binary);
            put_u32(stream, 0x12345678U);
            put_u64(stream, 0xfedcba9876543210ULL);
            put_string(stream, std::string("a\0b", 3));
            put_string(stream, "");
            require(static_cast<unsigned char>(stream.str()[0]) == 0x78, "Little-endian encoding");
            require(get_u32(stream) == 0x12345678U, "u32 roundtrip");
            require(get_u64(stream) == 0xfedcba9876543210ULL, "u64 roundtrip");
            require(get_string(stream, 3) == std::string("a\0b", 3), "Binary string roundtrip");
            require(get_string(stream, 0).empty(), "Empty string roundtrip");
        }},
        {"wire_truncation_and_untrusted_lengths", [] {
            std::istringstream short32("abc");
            rejects([&] { static_cast<void>(get_u32(short32)); });
            std::istringstream short64("1234567");
            rejects([&] { static_cast<void>(get_u64(short64)); });
            std::stringstream huge;
            put_u64(huge, (std::numeric_limits<std::uint64_t>::max)());
            rejects([&] { static_cast<void>(get_string(huge, 1024)); });
            std::stringstream truncated;
            put_u64(truncated, 4);
            truncated << "abc";
            rejects([&] { static_cast<void>(get_string(truncated, 4)); });
            std::ostringstream failed;
            failed.setstate(std::ios::badbit);
            rejects([&] { put_u64(failed, 42); });
        }},
        {"streaming_file_crc_and_missing_files", [] {
            Temporary temporary;
            const auto path = temporary.path / "bytes.bin";
            std::string data(200001, 'x');
            data[65535] = '\0';
            data[65536] = static_cast<char>(255);
            write_file(path, data);
            require(file_crc(path) == crc32(data), "File CRC differs from byte CRC");
            require(checked_file_size(path) == data.size(), "File size mismatch");
            rejects([&] { static_cast<void>(file_crc(temporary.path / "missing")); });
            rejects([&] { static_cast<void>(checked_file_size(temporary.path / "missing")); });
        }},
        {"empty_index_and_empty_lookup", [] {
            Temporary temporary;
            const auto path = make_postings(temporary.path, {});
            PostingReader reader(path);
            require(reader.count() == 0, "Empty count");
            require(reader.verify() == 64, "Empty physical size");
            bool called = false;
            reader.lookup(0, [&](const Posting&) { called = true; });
            require(!called, "Empty lookup called visitor");
        }},
        {"sort_aggregate_and_earliest_position", [] {
            Temporary temporary;
            const auto path = make_postings(temporary.path, {
                {40, 8, 2, 5, 4}, {7, 9, 1, 2, 20}, {40, 8, 5, 3, 8},
                {40, 2, 9, 6, 1}, {40, 8, 4, 3, 2}, {0, 0, 1, 0, 0}
            }, 64);
            const auto found = lookup(path, 40);
            require(found.size() == 2, "Aggregated document count");
            same_posting(found[0], {40, 2, 9, 6, 1});
            same_posting(found[1], {40, 8, 11, 3, 2});
            require(PostingReader(path).count() == 4, "Unique key count");
        }},
        {"block_boundaries_and_uint64_hash_limits", [] {
            Temporary temporary;
            std::vector<Posting> data;
            for (std::uint64_t document = 1; document <= 1100; ++document)
                data.push_back({3, document, 1, 1, 1});
            data.push_back({0, 1, 1, 1, 1});
            data.push_back({(std::numeric_limits<std::uint64_t>::max)(), 1, 1, 1, 1});
            std::reverse(data.begin(), data.end());
            const auto path = make_postings(temporary.path, data, 1024);
            const auto found = lookup(path, 3);
            require(found.size() == 1100, "Lookup stopped at block boundary");
            require(found.front().document == 1 && found.back().document == 1100, "Lookup order");
            require(lookup(path, 0).size() == 1, "Minimum hash lookup");
            require(lookup(path, (std::numeric_limits<std::uint64_t>::max)()).size() == 1, "Maximum hash lookup");
            require(lookup(path, 1).empty() && lookup(path, 4).empty(), "Missing hash lookup");
            require(PostingReader(path).verify() == checked_file_size(path), "Verify byte accounting");
        }},
        {"external_sort_against_independent_map", [] {
            Temporary temporary;
            std::vector<Posting> data;
            std::map<std::pair<std::uint64_t, std::uint64_t>, Posting> expected;
            std::uint64_t state = 42;
            for (std::uint32_t i = 0; i < 15000; ++i) {
                state = state * 6364136223846793005ULL + 1442695040888963407ULL;
                Posting posting{(state >> 20) % 23, (state >> 30) % 91, 1,
                                1 + i % 29, 1 + i % 31};
                data.push_back(posting);
                const auto key = std::make_pair(posting.hash, posting.document);
                const auto iterator = expected.find(key);
                if (iterator == expected.end()) expected[key] = posting;
                else {
                    ++iterator->second.frequency;
                    if (std::make_pair(posting.line, posting.column) <
                        std::make_pair(iterator->second.line, iterator->second.column)) {
                        iterator->second.line = posting.line;
                        iterator->second.column = posting.column;
                    }
                }
            }
            const auto path = make_postings(temporary.path, data, 160);
            PostingReader reader(path);
            require(reader.count() == expected.size(), "Map unique count");
            reader.verify();
            for (std::uint64_t hash = 0; hash < 23; ++hash) {
                const auto found = lookup(path, hash);
                std::vector<Posting> reference;
                for (const auto& [key, posting] : expected) if (key.first == hash) reference.push_back(posting);
                require(found.size() == reference.size(), "Map per-key count");
                for (std::size_t i = 0; i < found.size(); ++i) same_posting(found[i], reference[i]);
            }
        }},
        {"multipass_merge_more_than_32_inputs", [] {
            Temporary temporary;
            std::vector<fs::path> runs;
            for (unsigned i = 0; i < 70; ++i) {
                ExternalSorter sorter(temporary.path, "worker", sizeof(Posting));
                sorter.add({42, 7, 1, 70 - i, 1});
                const auto part = sorter.finish();
                runs.insert(runs.end(), part.begin(), part.end());
            }
            require(runs.size() == 70, "Expected independent runs");
            const auto path = temporary.path / "merged.bin";
            ExternalSorter::merge_to(runs, path, temporary.path);
            const auto found = lookup(path, 42);
            require(found.size() == 1, "Multipass duplicate reduction");
            same_posting(found[0], {42, 7, 70, 1, 1});
        }},
        {"online_compaction_bounds_number_of_run_paths", [] {
            Temporary temporary;
            ExternalSorter sorter(temporary.path, "tiny", 0);
            for (std::uint64_t i = 0; i < 1100; ++i) sorter.add({i, i, 1, 1, 1});
            const auto runs = sorter.finish();
            require(runs.size() <= 62, "Online carry failed to bound run metadata");
            const auto path = temporary.path / "all.bin";
            ExternalSorter::merge_to(runs, path, temporary.path);
            require(PostingReader(path).count() == 1100, "Compaction lost keys");
            PostingReader(path).verify();
        }},
        {"frequency_maximum_and_overflow", [] {
            Temporary temporary;
            const auto maximum = (std::numeric_limits<std::uint64_t>::max)();
            const auto valid = make_postings(temporary.path, {{1, 1, maximum - 1, 1, 1}, {1, 1, 1, 1, 1}});
            require(lookup(valid, 1)[0].frequency == maximum, "Maximum frequency rejected");
            ExternalSorter overflow(temporary.path, "overflow", 1024);
            overflow.add({1, 1, maximum, 1, 1});
            overflow.add({1, 1, 1, 1, 1});
            rejects([&] { static_cast<void>(overflow.finish()); });
            ExternalSorter split(temporary.path, "overflowmerge", sizeof(Posting));
            split.add({1, 1, maximum, 1, 1});
            split.add({1, 1, 1, 1, 1});
            const auto runs = split.finish();
            rejects([&] { ExternalSorter::merge_to(runs, temporary.path / "overflow.bin", temporary.path); });
        }},
        {"sorter_lifecycle_and_output_protection", [] {
            Temporary temporary;
            ExternalSorter sorter(temporary.path, "lifecycle", 64);
            rejects([&] { sorter.add({1, 1, 0, 1, 1}); });
            sorter.add({1, 1, 1, 1, 1});
            const auto runs = sorter.finish();
            require(sorter.finish() == runs, "finish is not idempotent");
            rejects([&] { sorter.add({2, 1, 1, 1, 1}); });
            rejects([&] { ExternalSorter invalid(temporary.path, "../outside", 64); });
            const auto existing = temporary.path / "existing.bin";
            write_file(existing, "preserve me");
            rejects([&] { ExternalSorter::merge_to(runs, existing, temporary.path); });
            require(read_file(existing) == "preserve me", "Existing output overwritten");
        }},
        {"header_corruption_truncation_and_trailing_bytes", [] {
            Temporary temporary;
            const auto valid = make_postings(temporary.path, {{1, 1, 1, 1, 1}});
            const auto bytes = read_file(valid);
            const auto bad = temporary.path / "bad.bin";
            for (const auto length : {std::size_t{0}, std::size_t{1}, std::size_t{63},
                                      std::size_t{64}, bytes.size() - 1}) {
                write_file(bad, bytes.substr(0, length));
                rejects([&] { PostingReader reader(bad); });
            }
            auto mutation = bytes;
            mutation[8] ^= 1;
            write_file(bad, mutation);
            rejects([&] { PostingReader reader(bad); });
            write_file(bad, bytes + "x");
            rejects([&] { PostingReader reader(bad); });
        }},
        {"header_valid_crc_invalid_semantics", [] {
            Temporary temporary;
            const auto valid = make_postings(temporary.path, {{1, 1, 1, 1, 1}});
            const auto original = read_file(valid);
            const auto bad = temporary.path / "bad.bin";
            for (const auto offset : {std::size_t{8}, std::size_t{12}, std::size_t{16},
                                      std::size_t{20}, std::size_t{32}, std::size_t{40},
                                      std::size_t{48}, std::size_t{60}}) {
                auto bytes = original;
                bytes[offset] ^= 1;
                repair_header_crc(bytes);
                write_file(bad, bytes);
                rejects([&] { PostingReader reader(bad); });
            }
            auto oversized = original;
            set64(oversized, 24, (std::numeric_limits<std::uint64_t>::max)());
            set64(oversized, 32, (std::numeric_limits<std::uint64_t>::max)() / 256 + 1);
            repair_header_crc(oversized);
            write_file(bad, oversized);
            rejects([&] { PostingReader reader(bad); });
        }},
        {"block_bit_flip_rejected_by_lookup_and_verify", [] {
            Temporary temporary;
            const auto path = make_postings(temporary.path, {{1, 1, 1, 1, 1}});
            auto bytes = read_file(path);
            bytes[90] ^= 64;
            write_file(path, bytes);
            rejects([&] { static_cast<void>(lookup(path, 1)); });
            rejects([&] { PostingReader(path).verify(); });
        }},
        {"block_valid_crc_invalid_semantics", [] {
            Temporary temporary;
            const auto original_path = make_postings(temporary.path, {{1, 1, 1, 1, 1}, {1, 2, 1, 1, 1}});
            const auto original = read_file(original_path);
            const auto bad = temporary.path / "bad.bin";
            auto bytes = original;
            set32(bytes, 64, 0);
            repair_block_crc(bytes);
            write_file(bad, bytes);
            rejects([&] { PostingReader(bad).verify(); });
            bytes = original;
            set64(bytes, 64 + 4 + 16, 0);
            repair_block_crc(bytes);
            write_file(bad, bytes);
            rejects([&] { PostingReader(bad).verify(); });
            bytes = original;
            set64(bytes, 64 + 4 + 32 + 8, 1);
            repair_block_crc(bytes);
            write_file(bad, bytes);
            rejects([&] { PostingReader(bad).verify(); });
            bytes = original;
            bytes[64 + 4 + 64] = 'x';
            repair_block_crc(bytes);
            write_file(bad, bytes);
            rejects([&] { PostingReader(bad).verify(); });
        }},
        {"global_order_corruption_across_blocks", [] {
            Temporary temporary;
            std::vector<Posting> postings;
            for (std::uint64_t i = 0; i < 300; ++i) postings.push_back({4, i, 1, 1, 1});
            const auto path = make_postings(temporary.path, postings);
            auto bytes = read_file(path);
            set64(bytes, 64 + 8200 + 4 + 8, 0);
            repair_block_crc(bytes, 1);
            write_file(path, bytes);
            rejects([&] { PostingReader(path).verify(); });
            rejects([&] { static_cast<void>(lookup(path, 4)); });
            rejects([&] { ExternalSorter::merge_to({path}, temporary.path / "merged.bin", temporary.path); });
        }},
        {"moved_reader_and_missing_index", [] {
            Temporary temporary;
            const auto path = make_postings(temporary.path, {{1, 1, 1, 1, 1}});
            PostingReader first(path);
            PostingReader second(std::move(first));
            require(second.count() == 1, "Move construction lost reader");
            rejects([&] { static_cast<void>(first.count()); });
            PostingReader third(path);
            third = std::move(second);
            require(third.verify() == checked_file_size(path), "Move assignment lost reader");
            rejects([&] { PostingReader missing(temporary.path / "missing.bin"); });
        }},
        {"unicode_paths", [] {
            Temporary temporary;
            const auto directory = temporary.path / fs::path(u8"каталог_日本_😀");
            fs::create_directories(directory);
            const auto path = make_postings(directory, {{term_hash("abc"), 1, 2, 3, 4}},
                                            64, "postings.bin");
            require(lookup(path, term_hash("abc")).size() == 1, "Unicode path lookup");
            require(PostingReader(path).verify() == checked_file_size(path), "Unicode path verification");
        }},
        {"parallel_sorters_and_independent_readers", [] {
            Temporary temporary;
            std::mutex mutex;
            std::vector<fs::path> runs;
            std::vector<std::exception_ptr> errors;
            std::vector<std::thread> workers;
            for (std::uint64_t worker = 0; worker < 8; ++worker) workers.emplace_back([&, worker] {
                try {
                    ExternalSorter sorter(temporary.path, "parallel", 160);
                    for (std::uint64_t i = 0; i < 500; ++i) sorter.add({i % 7, worker, 1, 1, 1});
                    const auto part = sorter.finish();
                    std::lock_guard lock(mutex);
                    runs.insert(runs.end(), part.begin(), part.end());
                } catch (...) { std::lock_guard lock(mutex); errors.push_back(std::current_exception()); }
            });
            for (auto& worker : workers) worker.join();
            if (!errors.empty()) std::rethrow_exception(errors.front());
            const auto path = temporary.path / "parallel.bin";
            ExternalSorter::merge_to(runs, path, temporary.path);
            require(PostingReader(path).count() == 56, "Parallel sort lost keys");
            workers.clear();
            for (unsigned i = 0; i < 8; ++i) workers.emplace_back([&] {
                try {
                    PostingReader reader(path);
                    for (unsigned repeat = 0; repeat < 10; ++repeat) {
                        std::size_t count = 0;
                        reader.lookup(3, [&](const Posting&) { ++count; });
                        require(count == 8, "Parallel lookup mismatch");
                    }
                } catch (...) { std::lock_guard lock(mutex); errors.push_back(std::current_exception()); }
            });
            for (auto& worker : workers) worker.join();
            if (!errors.empty()) std::rethrow_exception(errors.front());
        }}
    };
    unsigned failed = 0;
    for (const auto& [name, test] : tests) {
        try { test(); std::cout << "[ OK ] " << name << '\n'; }
        catch (const std::exception& error) { ++failed; std::cerr << "[FAIL] " << name << ": " << error.what() << '\n'; }
    }
    std::cout << tests.size() - failed << '/' << tests.size() << " storage tests passed\n";
    return failed == 0 ? 0 : 1;
}
