#include "test_support.hpp"
#include <algorithm>
#include <future>
#include <limits>
#include <sstream>

using namespace test;

namespace {
std::string normalized(const std::string& raw) {
    std::istringstream input(raw);
    std::string line, result;
    bool first = true;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (first && line.starts_with("\xef\xbb\xbf")) line.erase(0, 3);
        first = false;
        result += line;
        result += '\n';
    }
    return result;
}
fs::path content_file(const fs::path& directory) {
    for (const auto& item : fs::recursive_directory_iterator(directory))
        if (item.is_regular_file() && item.path().filename() == "content.bin") return item.path();
    throw std::runtime_error("Missing content.bin fixture");
}
SearchOptions search_options() {
    SearchOptions result;
    result.memory_mb = 64;
    result.limit = 100;
    result.sort_path = true;
    result.max_regex_steps = (std::numeric_limits<std::uint64_t>::max)();
    return result;
}
}

int main() {
    return run({
        {"stream_preserves_empty_crlf_bom_and_boundary_lines", [] {
            Workspace w;
            std::vector<std::string> samples{
                "", "\n", "\n\n", "\r", "\r\n", "a\r\nb\nc", "\xef\xbb\xbf",
                "\xef\xbb\xbf" "MARK!\r\n\xef\xbb\xbf" "second\n",
                std::string(65535, 'a') + "\nMARK!\n",
                std::string(65536, 'b') + "\nMARK!",
                std::string(65537, 'c') + "\r\nMARK!\r\n",
                std::string(65534, 'd') + "\r\nMARK!\n",
                std::string(65535, 'e') + "\xc3\xa9" "MARK!\n",
                std::string(131072, 'f'),
                std::string(65532, 'g') + "MARK!\n" + std::string(65536, 'h') + "\n"
            };
            for (std::size_t n = 0; n < samples.size(); ++n)
                write(w.root / (std::to_string(n) + ".txt"), samples[n]);
            auto o = options();
            o.max_line_bytes = 131072;
            o.segment_documents = 3;
            auto built = create_index(w.root, w.index, o);
            REQUIRE(built.files == samples.size());
            Index index(w.index, 64);
            for (std::size_t n = 0; n < samples.size(); ++n) {
                auto got = index.read(std::to_string(n) + ".txt", 1, 100, "snapshot", "", 1024 * 1024);
                REQUIRE(!got.truncated);
                REQUIRE(got.text == normalized(samples[n]));
            }
            REQUIRE(index.verify().files == samples.size());
            auto result = index.search("\"MARK!\"", search_options());
            REQUIRE(result.total_matches == 7);
            REQUIRE(result.hits.size() == 7);
            for (const auto& hit : result.hits) {
                REQUIRE(hit.matches == 1);
                REQUIRE(hit.snippets.size() == 1);
                REQUIRE(hit.snippets[0].text.find("MARK!") != std::string::npos);
            }
        }},
        {"stream_keeps_validation_and_maximum_line_contract", [] {
            Workspace w;
            write(w.root / "exact.txt", std::string(65536, 'x'));
            write(w.root / "too_long.txt", std::string(65537, 'x'));
            write(w.root / "invalid_utf8.txt", std::string(65535, 'x') + "\xc3\n");
            // NUL (also with an extension) is a Windows device name, not a
            // regular fixture file. Keep the embedded NUL, not that filename.
            write(w.root / "binary_nul.txt", std::string("text\0tail\n", 10));
            auto o = options();
            o.max_line_bytes = 65536;
            auto result = create_index(w.root, w.index, o);
            REQUIRE(result.files == 1);
            REQUIRE(result.skipped == 3);
            REQUIRE(Index(w.index).verify().files == 1);
        }},
        {"content_reader_eviction_preserves_counts_snippets_and_identity", [] {
            Workspace w;
            for (unsigned n = 0; n < 48; ++n)
                write(w.root / (std::to_string(n) + ".txt"), "match!\nline\nmatch!\n");
            auto o = options();
            o.threads = 1;
            o.segment_documents = 2;
            create_index(w.root, w.index, o);
            Index index(w.index, 64);
            REQUIRE(index.stats(false).segments == 24);
            auto so = search_options();
            so.profile = true;
            auto first = index.search("\"match!\"", so);
            auto second = index.search("\"match!\"", so);
            REQUIRE(first.total_matches == 48);
            REQUIRE(first.hits.size() == 48);
            REQUIRE(first.profile.enabled);
            REQUIRE(first.profile.content_opens > 0);
            REQUIRE(first.profile.content_bytes == first.scanned_bytes);
            REQUIRE(first.profile.content_reads > 0);
            for (std::size_t n = 0; n < first.hits.size(); ++n) {
                const auto& a = first.hits[n];
                const auto& b = second.hits[n];
                REQUIRE(a.path == b.path);
                REQUIRE(a.content_id == b.content_id);
                REQUIRE(a.score == b.score);
                REQUIRE(a.matches == 2);
                REQUIRE(a.snippets.size() == 2);
                REQUIRE(a.snippets[0].line == 1 && a.snippets[0].column == 1);
                REQUIRE(a.snippets[1].line == 3 && a.snippets[1].column == 1);
                REQUIRE(a.snippets[0].text == "match!");
            }
        }},
        {"content_corruption_after_warm_query_is_not_hidden", [] {
            Workspace w;
            write(w.root / "a.txt", "first\nmatch!\n");
            create_index(w.root, w.index, options());
            Index index(w.index, 64);
            REQUIRE(index.search("\"!\"").total_matches == 1);
            flip(content_file(w.index), 0);
            rejects([&] { index.search("\"!\""); }, "E_INDEX_CORRUPT");
        }},
        {"serial_queries_reuse_the_segment_handle_without_changing_results", [] {
            Workspace w;
            for (unsigned n = 0; n < 40; ++n) write(w.root / (std::to_string(n) + ".txt"), "match!\n");
            auto o = options();
            o.threads = 1;
            o.segment_documents = 100;
            create_index(w.root, w.index, o);
            Index index(w.index, 64);
            auto so = search_options();
            so.threads = 1;
            so.snippets_per_file = 0;
            const auto plain = index.search("\"!\"", so);
            so.profile = true;
            const auto profiled = index.search("\"!\"", so);
            REQUIRE(profiled.total_matches == plain.total_matches);
            REQUIRE(profiled.hits.size() == plain.hits.size());
            REQUIRE(profiled.scanned_bytes == plain.scanned_bytes);
            REQUIRE(profiled.profile.content_opens == 1);
            REQUIRE(profiled.profile.content_reads == 40);
            REQUIRE(profiled.profile.content_bytes == 280);
            REQUIRE(!plain.profile.enabled);
            for (std::size_t n = 0; n < plain.hits.size(); ++n) {
                REQUIRE(profiled.hits[n].path == plain.hits[n].path);
                REQUIRE(profiled.hits[n].score == plain.hits[n].score);
                REQUIRE(profiled.hits[n].matches == plain.hits[n].matches);
            }
        }},
        {"content_truncation_after_warm_query_is_rejected", [] {
            Workspace w;
            write(w.root / "a.txt", "first\nmatch!\n");
            create_index(w.root, w.index, options());
            Index index(w.index, 64);
            REQUIRE(index.search("\"!\"").total_matches == 1);
            fs::resize_file(content_file(w.index), 2);
            rejects([&] { index.search("\"!\""); }, "E_INDEX_CORRUPT");
        }},
        {"reader_resources_are_query_local_and_keep_pinned_snapshot", [] {
            Workspace w;
            for (unsigned n = 0; n < 40; ++n) write(w.root / (std::to_string(n) + ".txt"), "old!\nold!\n");
            auto o = options();
            create_index(w.root, w.index, o);
            Index old(w.index, 64);
            for (unsigned n = 0; n < 40; ++n) write(w.root / (std::to_string(n) + ".txt"), "new!\n");
            update_index(w.index, o);
            garbage_collect(w.index, 1);
            REQUIRE(Index(w.index, 64).search("\"new!\"").total_matches == 40);
            std::vector<std::future<void>> readers;
            for (unsigned n = 0; n < 6; ++n) readers.push_back(std::async(std::launch::async, [&] {
                for (unsigned iteration = 0; iteration < 3; ++iteration) {
                    const auto result = old.search("\"old!\"", search_options());
                    REQUIRE(result.total_matches == 40);
                    REQUIRE(result.hits.size() == 40);
                    for (const auto& hit : result.hits) {
                        REQUIRE(hit.matches == 2);
                        REQUIRE(hit.snippets.size() == 2);
                        REQUIRE(hit.snippets[0].text == "old!");
                    }
                }
            }));
            for (auto& reader : readers) reader.get();
            REQUIRE(old.verify().files == 40);
        }}
    });
}
