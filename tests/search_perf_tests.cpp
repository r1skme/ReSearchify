#include "test_support.hpp"
#include "researchify/disk.hpp"
#include "researchify/query.hpp"
#include "researchify/storage.hpp"
#include <algorithm>
#include <future>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <thread>

using namespace test;

namespace {
using Corpus = std::map<std::string, std::string>;
bool word(unsigned char c) {
    return c >= 128 || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9') || c == '_';
}
void positive(const std::shared_ptr<const QueryNode>& node, bool negative, std::vector<bool>& flags) {
    if (node->kind == NodeKind::Leaf) { if (!negative) flags[node->leaf] = true; }
    else if (node->kind == NodeKind::Not) positive(node->left, !negative, flags);
    else { positive(node->left, negative, flags); positive(node->right, negative, flags); }
}
// This deliberately scans the original source, not index postings, candidate
// filters, cached statistics, or the engine's tokenizer/matcher.
std::map<std::string, std::uint64_t> oracle(const Corpus& corpus, const std::string& expression) {
    const auto query = parse_query(expression);
    std::vector<bool> positives(query.leaves.size());
    positive(query.root, false, positives);
    std::map<std::string, std::uint64_t> result;
    for (const auto& [path, raw] : corpus) {
        std::vector<std::uint64_t> counts(query.leaves.size());
        std::vector<bool> present(query.leaves.size());
        std::istringstream input(raw);
        std::string line;
        bool first = true;
        while (std::getline(input, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (first && line.starts_with("\xef\xbb\xbf")) line.erase(0, 3);
            first = false;
            for (std::size_t i = 0; i < query.leaves.size(); ++i) {
                const auto& leaf = query.leaves[i];
                REQUIRE(leaf.kind != LeafKind::Regex);
                if (leaf.kind == LeafKind::Term) {
                    std::size_t at = 0;
                    while (at < line.size()) {
                        if (!word(static_cast<unsigned char>(line[at]))) { ++at; continue; }
                        const auto begin = at++;
                        while (at < line.size() && word(static_cast<unsigned char>(line[at]))) ++at;
                        if (std::string_view(line).substr(begin, at - begin) == leaf.text) ++counts[i];
                    }
                } else {
                    std::size_t at = 0;
                    while ((at = line.find(leaf.text, at)) != std::string::npos) {
                        ++counts[i];
                        at += leaf.text.size();
                    }
                }
            }
        }
        std::uint64_t matches = 0;
        for (std::size_t i = 0; i < counts.size(); ++i) {
            present[i] = counts[i] != 0;
            if (positives[i]) matches += counts[i];
        }
        if (query.evaluate(present)) result.emplace(path, matches);
    }
    return result;
}
Corpus make_corpus(const fs::path& root) {
    Corpus corpus;
    for (unsigned n = 0; n < 768; ++n) {
        const auto number = std::to_string(n);
        const auto name = std::string("source/") + (n == 9 ? "\xc3\xa9_" : "file_") +
            std::string(4 - number.size(), '0') + number + ".cpp";
        std::string text = n % 17 == 0 ? "\xef\xbb\xbf" : "";
        text += n % 2 ? "alpha " : "other ";
        if (n % 3) text += "beta ";
        if (n % 5) text += "gamma ";
        text += "\r\n";
        switch (n % 6) {
        case 0: text += "namespace { int needle_42 = 0; }\r\n"; break;
        case 1: text += "namespace { int xneedle_42 = 0; }\n"; break;
        case 2: text += "namespace { int a_needle_42 = 0; }\n"; break;
        case 3: text += "namespace { int \xc3\xa9" "needle_42 = 0; }\n"; break;
        case 4: text += "nothing literal-like\n"; break;
        default: text += "namespace { int needle_42 = 00; }\n"; break;
        }
        text += "MARK! punctuation a.b[c] \xe7\xb5\x82\xe3\x82\x8f\xe3\x82\x8a!\n";
        if (n % 7 == 0) text += "\nMARK! MARK!\r\n";
        if (n == 6) text += "\xef\xbb\xbf" "MARK!\n";
        for (unsigned line = 0; line < 80; ++line)
            text += "int common = value + 17; return common; // padding\n";
        text += "tail_without_final_newline";
        corpus.emplace(name, text);
        write(root / path_from_utf8(name), text);
    }
    corpus.emplace("long_only.cpp", std::string(9000, 'x') + "needle_42!\n");
    corpus.emplace("unicode_only.cpp", "\xef\xbb\xbf" "\xc3\xa9" "needle_42 = 0;\r\n");
    corpus.emplace("empty.cpp", "");
    for (const auto& name : {"long_only.cpp", "unicode_only.cpp", "empty.cpp"}) write(root / name, corpus.at(name));
    return corpus;
}
SearchOptions search_options() {
    SearchOptions result;
    result.memory_mb = 128;
    result.limit = 1000;
    result.snippets_per_file = 2;
    result.threads = 1;
    return result;
}
void same_hit(const SearchHit& a, const SearchHit& b) {
    REQUIRE(a.path == b.path);
    REQUIRE(a.content_id == b.content_id);
    REQUIRE(a.score == b.score);
    REQUIRE(a.matches == b.matches);
    REQUIRE(a.snippets.size() == b.snippets.size());
    for (std::size_t i = 0; i < a.snippets.size(); ++i) {
        REQUIRE(a.snippets[i].line == b.snippets[i].line);
        REQUIRE(a.snippets[i].column == b.snippets[i].column);
        REQUIRE(a.snippets[i].text == b.snippets[i].text);
    }
}
void same_result(const SearchReport& a, const SearchReport& b) {
    REQUIRE(a.generation == b.generation);
    REQUIRE(a.total_matches == b.total_matches);
    REQUIRE(a.remaining_matches == b.remaining_matches);
    REQUIRE(a.hits.size() == b.hits.size());
    for (std::size_t i = 0; i < a.hits.size(); ++i) same_hit(a.hits[i], b.hits[i]);
}
void matches_oracle(const SearchReport& actual, const std::map<std::string, std::uint64_t>& expected) {
    REQUIRE(actual.total_matches == expected.size());
    REQUIRE(actual.hits.size() == expected.size());
    std::set<std::string> seen;
    for (const auto& hit : actual.hits) {
        REQUIRE(expected.contains(hit.path));
        REQUIRE(seen.insert(hit.path).second);
        REQUIRE(hit.matches == expected.at(hit.path));
    }
}
}

int main() {
    return run({
        {"raw_content_intersection_keeps_versions_bounds_and_cancellation", [] {
            Workspace w;
            Corpus corpus;
            const std::string unicode = "\xc3\xa9\xe7\xb5\x82\xf0\x9f\x99\x82";
            const std::string literal = "\"abcDEFghi\"";
            const std::string legacy_plan = " AND NOT absent_guard";
            constexpr unsigned matches = 128;
            for (unsigned n = 0; n < matches; ++n) {
                corpus.emplace("keep/" + std::to_string(n) + ".cpp", "abcDEFghi\n" + unicode + "\nmarker\n");
                corpus.emplace("prefix/" + std::to_string(n) + ".cpp",
                    "abcDEFxxx\n\xc3\xa9\xe7\xb5\x82x\nuvwXYZxxx\nmarker\n");
                corpus.emplace("suffix/" + std::to_string(n) + ".cpp",
                    "xxxDEFghi\nx\xe7\xb5\x82\xf0\x9f\x99\x82\nxxxXYZrst\nmarker\n");
            }
            for (const auto& [path, text] : corpus) write(w.root / path, text);
            auto bo = options();
            bo.profile = "fast";
            bo.threads = 1;
            bo.segment_documents = 1024;
            create_index(w.root, w.index, bo);
            Index pinned(w.index, 128);
            const auto original = corpus;
            auto so = search_options();
            const auto check = [&](Index& index, const Corpus& expected) {
                for (const auto& expression : {literal, "\"" + unicode + "\""}) {
                    const auto actual = index.search(expression, so);
                    matches_oracle(actual, oracle(expected, expression));
                    same_result(actual, index.search(expression + legacy_plan, so));
                }
                const auto regex = index.search("/abcDEFghi/", so);
                matches_oracle(regex, oracle(expected, literal));
                same_result(regex, index.search("/abcDEFghi/" + legacy_plan, so));
                for (const auto& expression : {"marker", "abcDEFghi OR abcDEFxxx", "marker AND NOT abcDEFghi",
                        "\"abcDEFghi\" OR \"uvwXYZrst\""})
                    matches_oracle(index.search(expression, so), oracle(expected, expression));
            };
            check(pinned, corpus);
            const auto initial = pinned.search(literal, so);
            REQUIRE(initial.candidates == matches);
            auto limited = so;
            limited.max_candidates = matches;
            same_result(initial, pinned.search(literal, limited));
            limited.max_candidates = matches - 1;
            rejects([&] { pinned.search(literal, limited); }, "E_BUDGET");
            limited.max_candidates = 0;
            limited.max_scan_bytes = 1;
            rejects([&] { pinned.search(literal, limited); }, "E_BUDGET");
            // Every necessary gram exists, but no document contains both halves.
            // Cancellation must still be polled inside a run of raw rejections
            // that never reaches the content matcher or evaluate().
            REQUIRE(pinned.search("\"uvwXYZrst\"", so).candidates == 0);
            limited.max_scan_bytes = 0;
            std::size_t polls = 0;
            limited.cancelled = [&] { return ++polls >= 3; };
            rejects([&] { pinned.search("\"uvwXYZrst\"", limited); }, "E_CANCELLED");
            REQUIRE(polls == 3);

            fs::path segment;
            for (const auto& item : fs::directory_iterator(w.index))
                if (item.is_directory() && item.path().filename().string().starts_with("segment-")) {
                    REQUIRE(segment.empty());
                    segment = item.path();
                }
            REQUIRE(!segment.empty());
            const auto postings = segment / "postings.bin";
            const auto saved_postings = read(postings);
            auto cache = std::make_shared<disk::PageCache>(1024 * 1024);
            disk::Table terms(segment / "terms.tbl", cache);
            const auto descriptor = terms.get(std::string(1, '\1') + "abc");
            REQUIRE(descriptor.has_value());
            REQUIRE(descriptor->front() == 1);
            std::size_t at = 1;
            const auto begin = disk::get_varint(*descriptor, at);
            const auto end = disk::get_varint(*descriptor, at);
            // CRC failure in a required membership stream remains an error.
            flip(postings, begin + 32);
            rejects([&] { Index(w.index, 128).search(literal, so); }, "E_INDEX_CORRUPT");
            write(postings, saved_postings);
            // Even a valid-CRC posting is checked against the accepted record.
            // Change its first-line fields to 127 without changing varint widths.
            auto altered = saved_postings;
            const auto get32 = [&](std::size_t pos) {
                std::uint32_t value = 0;
                for (unsigned n = 0; n < 4; ++n)
                    value |= std::uint32_t(static_cast<unsigned char>(altered[pos + n])) << (8 * n);
                return value;
            };
            const auto put32 = [&](std::size_t pos, std::uint32_t value) {
                for (unsigned n = 0; n < 4; ++n) altered[pos + n] = static_cast<char>((value >> (8 * n)) & 255U);
            };
            for (auto block = begin; block < end;) {
                const auto bytes = get32(block), count = get32(block + 4);
                std::size_t pos = block + 32;
                for (std::uint32_t n = 0; n < count; ++n) {
                    (void)disk::get_varint(altered, pos);
                    (void)disk::get_varint(altered, pos);
                    const auto line_at = pos;
                    REQUIRE(disk::get_varint(altered, pos) < 128);
                    REQUIRE(pos == line_at + 1);
                    altered[line_at] = 127;
                    (void)disk::get_varint(altered, pos);
                }
                REQUIRE(pos == block + 32 + bytes);
                put32(block + 24, storage::crc32(std::string_view(altered).substr(block + 32, bytes)));
                put32(block + 28, storage::crc32(std::string_view(altered).substr(block, 28)));
                block += 32 + bytes;
            }
            write(postings, altered);
            rejects([&] { Index(w.index, 128).search(literal, so); }, "E_INDEX_CORRUPT");
            write(postings, saved_postings);
            const auto bucket = storage::term_hash("keep/0.cpp") & 1023U;
            const auto catalog = w.index / ("catalog-1-" + std::to_string(bucket) + ".tbl");
            const auto saved_catalog = read(catalog);
            flip(catalog, 44);
            rejects([&] { Index(w.index, 128).search(literal, so); }, "E_INDEX_CORRUPT");
            write(catalog, saved_catalog);

            for (unsigned n = 0; n < matches; ++n) {
                const auto path = "prefix/" + std::to_string(n) + ".cpp";
                corpus[path] = corpus.at("suffix/" + std::to_string(n) + ".cpp");
                write(w.root / path, corpus.at(path));
                if (n % 2 == 0) {
                    const auto keep = "keep/" + std::to_string(n) + ".cpp";
                    corpus[keep] += "abcDEFghi " + unicode + "\n";
                    write(w.root / keep, corpus.at(keep));
                }
            }
            REQUIRE(update_index(w.index, bo).changed == matches + matches / 2);
            Index fresh(w.index, 128);
            check(fresh, corpus);
            limited = so;
            limited.max_candidates = matches;
            REQUIRE(fresh.search(literal, limited).candidates == matches);
            check(pinned, original);
            REQUIRE(fresh.verify().files == corpus.size());
        }},
        {"single_common_content_anchor_preserves_snapshots_and_work_budgets", [] {
            Workspace w;
            Corpus corpus;
            constexpr unsigned count = 96;
            const std::string literal = "\"needle_42 = 0;\"";
            const std::string regex = "/needle_42 = 0;/";
            const std::string legacy_plan = " AND NOT definitely_missing_token";
            for (unsigned n = 0; n < count; ++n) {
                auto text = n % 3 == 0 ? "xneedle_42 = 0;\nneedle_42 = 0;\n" :
                    "other_" + std::to_string(n) + " = 0;\n";
                text += " all_marker;\n";
                const auto path = n == 3 ? "\xc3\xa9.cpp" : std::to_string(n) + ".cpp";
                corpus.emplace(path, text);
                write(w.root / path_from_utf8(path), text);
            }
            auto bo = options();
            bo.threads = 1;
            bo.segment_documents = 32;
            create_index(w.root, w.index, bo);
            Index pinned(w.index, 128);
            const auto original = corpus;
            auto so = search_options();
            so.max_regex_steps = 1000000000;
            so.max_candidates = count;
            const auto initial = pinned.search(literal, so);
            REQUIRE(initial.candidates == count);
            matches_oracle(initial, oracle(corpus, literal));
            // Both content matchers retain frequency, first position, scoring,
            // snippets and ordering compared with the equivalent Boolean query
            // that deliberately retains the ordinary posting-driven plan.
            const auto ordinary = pinned.search(literal + legacy_plan, so);
            same_result(initial, ordinary);
            REQUIRE(initial.scanned_bytes == ordinary.scanned_bytes);
            const auto regex_initial = pinned.search(regex, so);
            matches_oracle(regex_initial, oracle(corpus, literal));
            same_result(regex_initial, pinned.search(regex + legacy_plan, so));
            for (const bool sort_path : {false, true}) {
                auto page = so;
                page.sort_path = sort_path;
                page.limit = 7;
                page.offset = 3;
                same_result(pinned.search(literal, page), pinned.search(literal + legacy_plan, page));
                page.offset = 0;
                const auto first_page = pinned.search(literal, page);
                REQUIRE(first_page.hits.size() == page.limit);
                page.after = true;
                page.after_path = first_page.hits.back().path;
                page.after_score = first_page.hits.back().score;
                same_result(pinned.search(literal, page), pinned.search(literal + legacy_plan, page));
            }
            auto limited = so;
            limited.max_candidates = count - 1;
            rejects([&] { pinned.search(literal, limited); }, "E_BUDGET");
            limited.max_candidates = count;
            limited.max_scan_bytes = 1;
            rejects([&] { pinned.search(literal, limited); }, "E_BUDGET");
            limited.max_scan_bytes = 0;
            limited.cancelled = [] { return true; };
            rejects([&] { pinned.search(literal, limited); }, "E_CANCELLED");
            bool checked_content = false;
            for (const auto& item : fs::recursive_directory_iterator(w.index)) {
                if (item.path().filename() != "content.bin" || item.file_size() == 0) continue;
                const auto saved = read(item.path());
                flip(item.path(), 0);
                rejects([&] { pinned.search(literal, so); }, "E_INDEX_CORRUPT");
                write(item.path(), saved);
                checked_content = true;
                break;
            }
            REQUIRE(checked_content);

            // Keep old segments partially live while replacing most records.
            // Their raw df still covers the old corpus, but only eight of the
            // current files contain token 0. It must remain a selective
            // posting plan, rather than treating stale df as a live count.
            unsigned at = 0;
            std::uint64_t retained = 0;
            for (auto& [path, text] : corpus) {
                if (at++ % 12 == 0) { ++retained; continue; }
                text = "replacement without the necessary token\n all_marker;\n";
                write(w.root / path_from_utf8(path), text);
            }
            REQUIRE(retained == 8);
            REQUIRE(update_index(w.index, bo).changed == count - retained);
            Index fresh(w.index, 128);
            so.max_candidates = retained;
            const auto updated = fresh.search(literal, so);
            REQUIRE(updated.candidates == retained);
            matches_oracle(updated, oracle(corpus, literal));
            same_result(updated, fresh.search(literal + legacy_plan, so));
            const auto regex_updated = fresh.search(regex, so);
            matches_oracle(regex_updated, oracle(corpus, literal));
            same_result(regex_updated, fresh.search(regex + legacy_plan, so));
            matches_oracle(pinned.search(literal, search_options()), oracle(original, literal));
            REQUIRE(fresh.verify().files == count);

            // A full compaction can safely use current-generation knowledge
            // again; it must keep the new results and the pinned old snapshot.
            compact_index(w.index, 1, 128);
            Index compacted(w.index, 128);
            matches_oracle(compacted.search(literal, search_options()), oracle(corpus, literal));
            const auto common = compacted.search("\" all_marker;\"", search_options());
            REQUIRE(common.candidates == count);
            matches_oracle(common, oracle(corpus, "\" all_marker;\""));
            matches_oracle(pinned.search(literal, search_options()), oracle(original, literal));
        }},
        {"compact_fast_literal_boolean_oracle_and_parallel_equivalence", [] {
            Workspace w;
            const auto corpus = make_corpus(w.root);
            const std::vector<std::string> expressions{
                "\"needle_42 = 0;\"", "\" needle_42 = 0;\"", "\"needle_42 = 0\"",
                "\"needle_42\"", "\"xneedle_42\"", "\"42 = 0;\"", "\"42\"",
                "\"\xc3\xa9" "needle_42 = 0;\"", "\"a.b[c]\"", "\"MARK!\"",
                "\"\xef\xbb\xbf" "MARK!\"", "\"\xe7\xb5\x82\xe3\x82\x8f\xe3\x82\x8a!\"",
                "\"there_are_no_matches!\"", "\"needle_42 = 0;\" AND alpha",
                "alpha OR \"needle_42 = 0;\"", "NOT \"needle_42 = 0;\"",
                "NOT NOT \"needle_42 = 0;\"", "\"MARK!\" AND NOT alpha",
                "(alpha AND \"MARK!\") OR (beta AND NOT gamma)",
                "\"return common;\" AND \"needle_42 = 0;\"",
                "common AND alpha", "alpha OR beta", "NOT absent_token"
            };
            std::vector<std::map<std::string, std::uint64_t>> expected;
            for (const auto& expression : expressions) expected.push_back(oracle(corpus, expression));
            for (const auto& profile : {"compact", "fast"}) {
                auto bo = options();
                bo.profile = profile;
                const auto directory = w.base / profile;
                create_index(w.root, directory, bo);
                Index index(directory, 128);
                auto serial = search_options();
                auto parallel = serial;
                parallel.threads = 4;
                for (std::size_t i = 0; i < expressions.size(); ++i) {
                    const auto a = index.search(expressions[i], serial);
                    const auto b = index.search(expressions[i], parallel);
                    matches_oracle(a, expected[i]);
                    matches_oracle(b, expected[i]);
                    same_result(a, b);
                }
                auto profiled = parallel;
                profiled.profile = true;
                profiled.snippets_per_file = 0;
                // A two-byte literal has no trigram prefilter and leaves
                // enough candidate work to exercise the actual worker pool.
                const auto broad = index.search("\"42\"", profiled);
                REQUIRE(broad.scanned_bytes > 2ULL * 1024 * 1024);
                REQUIRE(broad.profile.workers > 1);

                // The universe estimate and corpus are large, but path scope
                // leaves only a small amount of actual content verification.
                // A short literal cannot hide that work behind a gram filter.
                auto scoped_auto = search_options();
                scoped_auto.threads = 0;
                scoped_auto.profile = true;
                scoped_auto.snippets_per_file = 0;
                std::uint64_t scoped_bytes = 0;
                for (const auto& [path, text] : corpus) {
                    if (!path.starts_with("source/")) continue;
                    scoped_auto.paths.push_back(path);
                    scoped_bytes += text.size();
                    if (scoped_auto.paths.size() == 112) break;
                }
                REQUIRE(scoped_auto.paths.size() == 112);
                auto scoped_serial = scoped_auto;
                scoped_serial.threads = 1;
                const auto small = index.search("\"!\"", scoped_auto);
                same_result(index.search("\"!\"", scoped_serial), small);
                REQUIRE(small.total_matches == 112);
                REQUIRE(small.scanned_bytes == scoped_bytes);
                REQUIRE(small.scanned_bytes < 2ULL * 1024 * 1024);
                REQUIRE(small.profile.workers == 1);
                // Regex retains its single global step budget even when a
                // worker count is requested; result equivalence is mandatory.
                serial.max_regex_steps = parallel.max_regex_steps = 1000000000;
                same_result(index.search("/needle_[0-9]+/", serial), index.search("/needle_[0-9]+/", parallel));
            }
        }},
        {"fast_trigram_membership_accepts_zero_token_documents_after_updates", [] {
            Workspace w;
            Corpus corpus;
            const std::string literal = "@@@%%%$$$";
            const std::string alternative = "!!!^^^&&&";
            const auto primary_path = [](unsigned n) {
                return "primary/" + std::to_string(n) + ".txt";
            };
            const auto alternative_path = [](unsigned n) {
                return "alternative/" + std::to_string(n) + ".txt";
            };
            for (unsigned n = 0; n < 128; ++n) {
                auto text = literal + "\n";
                if (n % 4 == 0) text += literal + "\n";
                corpus.emplace(primary_path(n), std::move(text));
            }
            for (unsigned n = 0; n < 40; ++n)
                corpus.emplace(alternative_path(n), alternative + "\n");
            for (const auto& [path, text] : corpus)
                write(w.root / path_from_utf8(path), text);

            auto bo = options();
            bo.profile = "fast";
            bo.threads = 1;
            bo.segment_documents = 32;
            create_index(w.root, w.index, bo);
            Index pinned(w.index, 128);
            const auto before = corpus;
            // Each trigram has a posting with frequency one, although every
            // document has zero indexed words. More than 32 matching files
            // keep the planner's bounded conjunction path exercised.
            const std::vector<std::string> expressions{
                "\"@@@%%%$$$\"", "\"!!!^^^&&&\"",
                "\"@@@%%%\" AND \"%%%$$$\"",
                "\"@@@%%%$$$\" OR \"!!!^^^&&&\""
            };
            const auto check = [&](const Index& view, const Corpus& expected) {
                REQUIRE(view.stats(false).tokens == 0);
                for (const auto& expression : expressions)
                    matches_oracle(view.search(expression, search_options()),
                                   oracle(expected, expression));
            };
            check(pinned, before);

            for (unsigned n = 0; n < 32; ++n) {
                const auto path = primary_path(n);
                corpus.at(path) = alternative + "\n" + alternative + "\n";
                write(w.root / path_from_utf8(path), corpus.at(path));
            }
            for (unsigned n = 32; n < 48; ++n) {
                const auto path = primary_path(n);
                REQUIRE(fs::remove(w.root / path_from_utf8(path)));
                REQUIRE(corpus.erase(path) == 1);
            }
            for (unsigned n = 0; n < 8; ++n) {
                const auto path = alternative_path(n);
                corpus.at(path) = literal + "\n";
                write(w.root / path_from_utf8(path), corpus.at(path));
            }
            for (unsigned n = 8; n < 12; ++n) {
                const auto path = alternative_path(n);
                REQUIRE(fs::remove(w.root / path_from_utf8(path)));
                REQUIRE(corpus.erase(path) == 1);
            }
            const auto updated = update_index(w.index, bo);
            REQUIRE(updated.changed == 40);
            REQUIRE(updated.removed == 20);
            Index fresh(w.index, 128);
            check(fresh, corpus);
            REQUIRE(fresh.verify().files == corpus.size());
            check(pinned, before);
        }},
        {"parallel_sort_offset_keyset_scope_and_global_budgets", [] {
            Workspace w;
            make_corpus(w.root);
            create_index(w.root, w.index, options());
            Index index(w.index, 128);
            auto serial = search_options();
            for (bool sort_path : {false, true}) {
                serial.sort_path = sort_path;
                serial.limit = 19;
                serial.offset = 7;
                auto parallel = serial;
                parallel.threads = 4;
                same_result(index.search("\"MARK!\"", serial), index.search("\"MARK!\"", parallel));
                serial.offset = 0;
                const auto first = index.search("\"MARK!\"", serial);
                REQUIRE(first.hits.size() == 19);
                serial.after = true;
                serial.after_path = first.hits.back().path;
                serial.after_score = first.hits.back().score;
                parallel = serial;
                parallel.threads = 4;
                const auto continuation = index.search("\"MARK!\"", parallel);
                same_result(index.search("\"MARK!\"", serial), continuation);
                auto offset = serial;
                offset.after = false;
                offset.offset = 19;
                const auto second = index.search("\"MARK!\"", offset);
                REQUIRE(second.hits.size() == continuation.hits.size());
                for (std::size_t i = 0; i < second.hits.size(); ++i) same_hit(second.hits[i], continuation.hits[i]);
                serial.after = false;
            }
            serial = search_options();
            serial.paths = {"source/*7.cpp"};
            auto parallel = serial;
            parallel.threads = 4;
            same_result(index.search("\"MARK!\"", serial), index.search("\"MARK!\"", parallel));
            for (std::size_t threads : {std::size_t(1), std::size_t(4)}) {
                auto limited = search_options();
                limited.threads = threads;
                limited.max_candidates = 4;
                rejects([&] { index.search("\"MARK!\"", limited); }, "E_BUDGET");
                limited.max_candidates = 0;
                limited.max_scan_bytes = 16;
                rejects([&] { index.search("\"MARK!\"", limited); }, "E_BUDGET");
                limited.max_scan_bytes = 0;
                limited.max_regex_steps = 2;
                rejects([&] { index.search("/needle_[0-9]+/", limited); }, "E_BUDGET");
                limited.max_regex_steps = 100000000;
                std::atomic<unsigned> cancellation_checks{0};
                limited.cancelled = [&] { return cancellation_checks.fetch_add(1, std::memory_order_relaxed) >= 128; };
                rejects([&] { index.search("\"MARK!\"", limited); }, "E_CANCELLED");
                limited.cancelled = {};
                REQUIRE(index.search("\"MARK!\"", limited).total_matches == 768);
            }
        }},
        {"queued_empty_documents_observe_cancellation_without_line_callbacks", [] {
            Workspace w;
            constexpr std::size_t group_size = 512;
            const std::string content = std::string(4096, '.') + "\n";
            std::string first_empty;
            const auto populate = [&](const std::string& prefix, bool nonempty) {
                std::size_t added = 0;
                for (unsigned n = 0; added < group_size; ++n) {
                    const auto path = prefix + "/" + std::to_string(n) + ".txt";
                    // Catalog IDs visit the nonempty prefix before the empty
                    // suffix, independently of filesystem enumeration order.
                    const bool lower_half = (storage::term_hash(path) & 1023U) < 512;
                    if (lower_half != nonempty) continue;
                    write(w.root / path, nonempty ? content : std::string{});
                    if (!nonempty && first_empty.empty()) first_empty = path;
                    ++added;
                }
            };
            populate("warm", true);
            populate("empty", false);
            auto bo = options();
            bo.threads = 1;
            bo.max_line_bytes = 8192;
            bo.segment_documents = 64;
            create_index(w.root, w.index, bo);
            Index index(w.index, 128);
            REQUIRE(index.stats(false).files == 2 * group_size);
            REQUIRE(index.stats(false).lines == group_size);
            const auto empties = index.files("empty/*", group_size);
            REQUIRE(empties.total == group_size);
            for (const auto& file : empties.files) {
                REQUIRE(file.bytes == 0);
                REQUIRE(file.lines == 0);
            }

            auto so = search_options();
            so.threads = 2;
            so.limit = 0;
            so.snippets_per_file = 0;
            so.profile = true;
            // A one-byte literal has no content prefilter. Measure ordinary
            // search/admission polls without reading any selected document.
            std::atomic<std::size_t> polls{0};
            so.paths = {"absent/*"};
            so.cancelled = [&] { polls.fetch_add(1, std::memory_order_relaxed); return false; };
            const auto no_reads = index.search("\"!\"", so);
            REQUIRE(no_reads.candidates == 2 * group_size);
            REQUIRE(no_reads.profile.content_opens == 0);
            const auto admission_polls = polls.load(std::memory_order_relaxed);
            REQUIRE(admission_polls > 0);
            // Only one zero-byte item is enqueued, so its final pending flush
            // remains serial even though this corpus permits worker execution.
            so.paths = {first_empty};
            polls.store(0, std::memory_order_relaxed);
            so.cancelled = [&] { return polls.fetch_add(1, std::memory_order_relaxed) >= admission_polls; };
            rejects([&] { index.search("\"!\"", so); }, "E_CANCELLED");
            REQUIRE(polls.load(std::memory_order_relaxed) > admission_polls);
            so.cancelled = {};
            const auto serial_empty = index.search("\"!\"", so);
            REQUIRE(serial_empty.total_matches == 0);
            REQUIRE(serial_empty.scanned_bytes == 0);
            REQUIRE(serial_empty.profile.workers == 1);
            REQUIRE(serial_empty.profile.content_opens > 0);

            // The equal-size prefix crosses the pool's admitted-work threshold.
            // Calibrate its helper-thread polls rather than depending on an
            // exact number of implementation checkpoints or thread timing.
            const auto caller = std::this_thread::get_id();
            std::atomic<std::size_t> worker_polls{0};
            so.paths = {"warm/*"};
            so.cancelled = [&] {
                if (std::this_thread::get_id() != caller)
                    worker_polls.fetch_add(1, std::memory_order_relaxed);
                return false;
            };
            const auto warm = index.search("\"!\"", so);
            REQUIRE(warm.profile.workers == 2);
            REQUIRE(warm.scanned_bytes == group_size * content.size());
            const auto warm_worker_polls = worker_polls.load(std::memory_order_relaxed);
            REQUIRE(warm_worker_polls > 0);
            // Prefix batches are unchanged when the empty suffix is admitted.
            // Cancel only from a helper after its measured nonempty work; an
            // implementation polling only per line would never see this stop.
            so.paths = {"warm/*", "empty/*"};
            worker_polls.store(0, std::memory_order_relaxed);
            so.cancelled = [&] {
                if (std::this_thread::get_id() == caller) return false;
                return worker_polls.fetch_add(1, std::memory_order_relaxed) >= warm_worker_polls;
            };
            rejects([&] { index.search("\"!\"", so); }, "E_CANCELLED");
            REQUIRE(worker_polls.load(std::memory_order_relaxed) > warm_worker_polls);
            so.cancelled = {};
            const auto recovered = index.search("\"!\"", so);
            REQUIRE(recovered.profile.workers == 2);
            REQUIRE(recovered.scanned_bytes == warm.scanned_bytes);
            REQUIRE(recovered.total_matches == 0);
        }},
        {"and_range_skips_and_no_match_cancellation", [] {
            Workspace w;
            Corpus corpus;
            for (unsigned n = 0; n < 1536; ++n) {
                const auto name = "range_" + std::to_string(n) + ".cpp";
                const bool low = (storage::term_hash(name) & 1023U) < 400;
                const auto text = std::string(low ? "low " : "high ") + (n % 2 ? "odd\n" : "even\n");
                corpus.emplace(name, text);
                write(w.root / name, text);
            }
            auto bo = options();
            bo.segment_documents = 64;
            create_index(w.root, w.index, bo);
            Index index(w.index, 128);
            auto so = search_options();
            for (const auto* expression : {"low AND high", "even AND odd", "low AND even", "high AND odd"})
                matches_oracle(index.search(expression, so), oracle(corpus, expression));
            auto limited = so;
            limited.max_candidates = 1;
            limited.max_scan_bytes = 1;
            REQUIRE(index.search("low AND high", limited).candidates == 0);
            rejects([&] { index.search("low AND even", limited); }, "E_BUDGET");
            std::size_t polls = 0;
            so.cancelled = [&] { return ++polls >= 8; };
            rejects([&] { index.search("even AND odd", so); }, "E_CANCELLED");
            REQUIRE(polls == 8);
            polls = 0;
            rejects([&] { index.search("even AND (odd OR absent_token)", so); }, "E_CANCELLED");
            REQUIRE(polls == 8);
            so.cancelled = [] { return true; };
            rejects([&] { index.search("absent_token", so); }, "E_CANCELLED");
            bo.changed_only = true;
            for (unsigned n = 0; n < 1536; n += 2) {
                const auto name = "range_" + std::to_string(n) + ".cpp";
                write(w.root / name, "replacement\n");
                bo.changed_files.push_back(name);
            }
            update_index(w.index, bo);
            Index fresh(w.index, 128);
            so.cancelled = {};
            REQUIRE(fresh.search("even", so).total_matches == 0);
            polls = 0;
            so.cancelled = [&] { return ++polls >= 8; };
            rejects([&] { fresh.search("even", so); }, "E_CANCELLED");
            REQUIRE(polls == 8);
        }},
        {"parallel_queries_and_update_gc_preserve_snapshot", [] {
            Workspace w;
            make_corpus(w.root);
            auto bo = options();
            create_index(w.root, w.index, bo);
            Index old(w.index, 128);
            auto serial = search_options();
            serial.limit = 30;
            const auto expected = old.search("\"MARK!\"", serial);
            auto parallel = serial;
            parallel.threads = 4;
            std::vector<std::future<void>> readers;
            for (unsigned n = 0; n < 3; ++n) readers.push_back(std::async(std::launch::async, [&] {
                for (unsigned iteration = 0; iteration < 3; ++iteration)
                    same_result(expected, old.search("\"MARK!\"", parallel));
            }));
            write(w.root / "source/file_0000.cpp", "replacement!\n");
            bo.changed_only = true;
            bo.changed_files = {"source/file_0000.cpp"};
            const auto update = update_index(w.index, bo);
            REQUIRE(update.changed == 1);
            garbage_collect(w.index, 1);
            for (auto& reader : readers) reader.get();
            same_result(expected, old.search("\"MARK!\"", parallel));
            REQUIRE(Index(w.index, 128).search("\"replacement!\"", parallel).total_matches == 1);
        }}
    });
}
