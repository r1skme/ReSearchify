#include "test_support.hpp"
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
