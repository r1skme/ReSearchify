#include "test_support.hpp"
#include "researchify/disk.hpp"
#include <algorithm>
#include <limits>
#include <map>
#include <random>
using namespace test;
namespace d = researchify::disk;
std::string key(std::uint64_t n) { return "prefix/shared/" + d::ordered(n);
}
int main() {
    return run({
        {"canonical_varints_boundaries", [] {
            for (auto value : {std::uint64_t(0), std::uint64_t(1), std::uint64_t(127), std::uint64_t(128), std::uint64_t(16383), std::uint64_t(16384), (std::numeric_limits<std::uint64_t>::max)()}) {
                std::string data;
                d::varint(data, value);
                std::size_t at = 0;
                REQUIRE(d::get_varint(data, at) == value);
                REQUIRE(at == data.size());
                REQUIRE(d::ordered(d::ordered(value)) == value);
                for (std::size_t n = 0; n < data.size(); ++n) rejects([&] { std::size_t pos = 0; d::get_varint(std::string_view(data).substr(0, n), pos); }, "E_INDEX_CORRUPT");
            }
            rejects([] { std::size_t p = 0; d::get_varint(std::string("\x80\x00", 2), p); }, "E_INDEX_CORRUPT");
            rejects([] { std::size_t p = 0; d::get_varint(std::string(10, char(0xff)), p); }, "E_INDEX_CORRUPT");
        }},
        {"checked_file_corruption", [] {
            Workspace w;
            auto p = w.base / "checked";
            d::write_file(p, "payload");
            REQUIRE(d::read_file(p, 100) == "payload");
            rejects([&] { d::read_file(p, 2); }, "E_INDEX_CORRUPT");
            flip(p, 0);
            rejects([&] { d::read_file(p, 100); }, "E_INDEX_CORRUPT");
        }},
        {"prefix_btree_many_levels_and_streaming", [] {
            Workspace w;
            auto file = w.base / "tree";
            d::TableWriter writer(file, 1024);
            for (std::uint64_t i = 0; i < 20000; ++i) writer.add(key(i * 2), "value-" + std::to_string(i));
            writer.finish();
            auto cache = std::make_shared<d::PageCache>(8192);
            d::Table table(file, cache);
            REQUIRE(table.count() == 20000);
            REQUIRE(table.verify() == fs::file_size(file));
            for (std::uint64_t i = 0; i < 20000; i += 47) {
                REQUIRE(table.get(key(i * 2)) == "value-" + std::to_string(i));
                REQUIRE(!table.get(key(i * 2 + 1)));
            }
            REQUIRE(!table.get("absent"));
            std::uint64_t count = 0;
            table.each([&](auto k, auto v) { REQUIRE(k == key(count * 2)); REQUIRE(v == "value-" + std::to_string(count)); ++count; });
            REQUIRE(count == table.count());
        }},
        {"btree_empty_prefix_and_order_rejection", [] {
            Workspace w;
            auto p = w.base / "tree";
            { d::TableWriter t(p);
            t.finish();
            }
            auto cache = std::make_shared<d::PageCache>(4096);
            d::Table empty(p, cache);
            REQUIRE(empty.count() == 0);
            REQUIRE(!empty.get("a"));
            empty.verify();
            auto q = w.base / "nonempty";
            d::TableWriter t(q);
            t.add("I1", "a");
            t.add("I2", "b");
            t.add("P1", "c");
            rejects([&] { t.add("A", "d"); });
            t.finish();
            d::Table table(q, cache);
            unsigned n = 0;
            table.each([&](auto k, auto) { REQUIRE(k.starts_with("I")); ++n; }, "I");
            REQUIRE(n == 2);
        }},
        {"btree_checksum_and_truncation", [] {
            Workspace w;
            auto p = w.base / "tree";
            { d::TableWriter t(p);
            t.add("a", std::string(4000, 'v'));
            t.finish();
            }
            const auto original = read(p);
            flip(p, 44);
            auto cache = std::make_shared<d::PageCache>(4096);
            rejects([&] { d::Table(p, cache).verify(); }, "E_INDEX_CORRUPT");
            for (std::size_t n : {std::size_t(0), std::size_t(8), std::size_t(35), original.size() - 1}) {
                write(p, original.substr(0, n));
                cache->clear();
                rejects([&] { d::Table(p, cache).verify(); });
            }
        }},
        {"external_sort_many_runs_and_duplicate_combine", [] {
            Workspace w;
            auto dir = w.base / "sort";
            fs::create_directory(dir);
            d::Sorter sorter(dir, 262144, [](std::string& a, const std::string& b) { a = std::to_string(std::stoull(a) + std::stoull(b)); });
            std::map<std::string, std::uint64_t> reference;
            std::mt19937 rng(91);
            for (unsigned i = 0; i < 50000; ++i) { auto k = key(rng() % 1500);
            sorter.add(k, "1");
            ++reference[k];
            }
            auto it = reference.begin();
            sorter.finish([&](auto k, auto v) { REQUIRE(it != reference.end()); REQUIRE(k == it->first); REQUIRE(v == std::to_string(it->second)); ++it; });
            REQUIRE(it == reference.end());
        }},
        {"postings_delta_compression_seek_and_validation", [] {
            Workspace w;
            auto segment = w.base / "segment";
            fs::create_directory(segment);
            d::PostingWriter writer(segment);
            for (std::uint64_t t = 0; t < 4; ++t) for (std::uint64_t i = 1; i <= 2000; ++i) {
                auto term = "term" + std::to_string(t);
                auto k = term + std::string(1, '\0') + d::ordered(i * 9);
                writer.add(k, d::posting_value({i * 9, 3, i, 1}));
            }
            writer.finish();
            auto cache = std::make_shared<d::PageCache>(32768);
            d::Table dict(segment / "terms.tbl", cache);
            REQUIRE(dict.count() == 4);
            REQUIRE(d::verify_postings(segment, cache) > 0);
            d::PostingCursor cursor(segment, cache, *dict.get("term2"));
            REQUIRE(cursor.count() == 2000);
            for (std::uint64_t i = 1; i <= 2000; i += 39) {
                REQUIRE(cursor.seek(i * 9 - 1));
                REQUIRE(cursor.current().document == i * 9);
                REQUIRE(cursor.current().line == i);
                REQUIRE(cursor.current().frequency == 3);
            }
            REQUIRE(!cursor.seek(18001));
            auto p = segment / "postings.bin";
            flip(p, fs::file_size(p) - 1);
            cache->clear();
            rejects([&] { d::verify_postings(segment, cache); }, "E_INDEX_CORRUPT");
        }},
        {"inline_postings_one_four_and_blocked_five", [] {
            Workspace w;
            auto segment = w.base / "segment";
            fs::create_directory(segment);
            d::PostingWriter writer(segment);
            for (std::uint64_t count : {1ULL, 4ULL, 5ULL, 128ULL, 129ULL}) {
                auto term = d::ordered(count);
                for (std::uint64_t i = 1; i <= count; ++i)
                    writer.add(term + std::string(1, '\0') + d::ordered(i * 4000), d::posting_value({i * 4000, 2, 3, 4}));
            }
            writer.finish();
            auto cache = std::make_shared<d::PageCache>(32768);
            d::Table table(segment / "terms.tbl", cache);
            d::verify_postings(segment, cache);
            for (std::uint64_t count : {1ULL, 4ULL, 5ULL, 128ULL, 129ULL}) {
                auto descriptor = *table.get(d::ordered(count));
                REQUIRE(descriptor.front() == (count <= 4 ? 0 : 1));
                REQUIRE(d::posting_count(descriptor) == count);
                d::PostingCursor cursor(segment, cache, descriptor);
                for (std::uint64_t i = 1; i <= count; ++i) {
                    REQUIRE(cursor.seek(i * 4000 - 1));
                    REQUIRE(cursor.current().document == i * 4000);
                }
                REQUIRE(!cursor.seek(count * 4000 + 1));
                if (count <= 4) {
                    for (std::size_t n = 0; n < descriptor.size(); ++n)
                        rejects([&] { d::PostingCursor bad(segment, cache, std::string_view(descriptor).substr(0, n)); });
                }
            }
            auto small = w.base / "small";
            fs::create_directory(small);
            d::PostingWriter only_inline(small);
            only_inline.add("token" + std::string(1, '\0') + d::ordered(1), d::posting_value({1, 1, 1, 1}));
            only_inline.finish();
            REQUIRE(fs::file_size(small / "postings.bin") == 8);
            d::verify_postings(small, cache);
        }},
        {"posting_combiner_overflow_and_earliest_position", [] {
            auto a = d::posting_value({1, 4, 9, 4}), b = d::posting_value({1, 7, 3, 20});
            d::combine_posting(a, b);
            auto p = d::posting_value(a);
            REQUIRE(p.frequency == 11 && p.line == 3 && p.column == 20);
            a = d::posting_value({1, (std::numeric_limits<std::uint64_t>::max)(), 1, 1});
            rejects([&] { d::combine_posting(a, b); });
        }}
    });
}
