#include "test_support.hpp"
#include "researchify/disk.hpp"
#include "researchify/literal_filter.hpp"
#include "researchify/storage.hpp"
#include <algorithm>
#include <map>
#include <thread>

using namespace test;

namespace {
SearchOptions search_options() {
    SearchOptions result;
    result.memory_mb = 64;
    result.limit = 100;
    return result;
}

void check_hit(const Index& index, const std::string& term, const std::string& path,
               std::uint64_t count, std::uint64_t line, std::uint64_t column) {
    const auto found = index.search(term, search_options());
    REQUIRE(found.total_matches == 1);
    REQUIRE(found.hits.size() == 1);
    REQUIRE(found.hits[0].path == path);
    REQUIRE(found.hits[0].matches == count);
    REQUIRE(!found.hits[0].snippets.empty());
    REQUIRE(found.hits[0].snippets.front().line == line);
    REQUIRE(found.hits[0].snippets.front().column == column);
}

fs::path only_sidecar(const fs::path& directory) {
    std::vector<fs::path> found;
    for (const auto& entry : fs::recursive_directory_iterator(directory))
        if (entry.path().filename() == "literal.tbl") found.push_back(entry.path());
    REQUIRE(found.size() == 1);
    return found.front();
}

std::vector<disk::Entry> table_entries(const fs::path& path) {
    auto cache = std::make_shared<disk::PageCache>(1024 * 1024);
    disk::Table table(path, cache);
    std::vector<disk::Entry> result;
    table.each([&](auto key, auto value) { result.emplace_back(key, value); });
    return result;
}

void replace_table(const fs::path& path, const std::vector<disk::Entry>& entries) {
    disk::TableWriter table(path);
    for (const auto& [key, value] : entries) table.add(key, value);
    table.finish();
}
}

int main() {
    return run({
        {"literal_sidecar_corruption_is_explicit_and_recoverable", [] {
            Workspace workspace;
            write(workspace.root / "a.cpp", "target first\n" + std::string(700, 'x') + "\n");
            write(workspace.root / "b.cpp", "target second\n" + std::string(700, 'y') + "\n");
            auto build_options = options();
            build_options.threads = 1;
            create_index(workspace.root, workspace.index, build_options);
            const auto sidecar = only_sidecar(workspace.index);
            const auto pristine = read(sidecar);
            const auto entries = table_entries(sidecar);
            REQUIRE(entries.size() == 2);
            auto must_reject = [&] {
                // Each operation uses a fresh cache, so successful reads before
                // corruption cannot mask later damage through cached pages.
                rejects([&] { Index(workspace.index, 64).search("\"target\""); }, "E_INDEX_CORRUPT");
                rejects([&] { Index(workspace.index, 64).verify(); }, "E_INDEX_CORRUPT");
            };
            flip(sidecar, pristine.size() / 2);
            must_reject();
            write(sidecar, pristine);
            fs::resize_file(sidecar, 27);
            must_reject();
            write(sidecar, pristine);

            auto unknown_version = entries;
            unknown_version.front().second[4] = '2';
            replace_table(sidecar, unknown_version); // valid table/page CRCs
            must_reject();
            auto foreign_identity = entries;
            foreign_identity.front().second[5] ^= 1;
            replace_table(sidecar, foreign_identity);
            must_reject();
            auto invalid_crc_identity = entries;
            invalid_crc_identity.front().second[21] ^= 1;
            replace_table(sidecar, invalid_crc_identity);
            must_reject();
            auto shortened_value = entries;
            shortened_value.front().second.pop_back();
            replace_table(sidecar, shortened_value);
            must_reject();

            // A structurally valid but wrong mask is detectable by full verify,
            // which recomputes it from the saved and integrity-checked content.
            auto incorrect_mask = entries;
            incorrect_mask.front().second[29] ^= 1;
            replace_table(sidecar, incorrect_mask);
            rejects([&] { Index(workspace.index, 64).verify(); }, "E_INDEX_CORRUPT");

            replace_table(sidecar, entries);
            REQUIRE(Index(workspace.index, 64).search("\"target\"").total_matches == 2);
            REQUIRE(Index(workspace.index, 64).verify().files == 2);
            // Rebuilding optional accelerators from snapshots restores them
            // without trusting their old values.
            replace_table(sidecar, foreign_identity);
            compact_index(workspace.index, 1, 64);
            garbage_collect(workspace.index, 1);
            REQUIRE(Index(workspace.index, 64).verify().files == 2);
            REQUIRE(Index(workspace.index, 64).search("\"target\"").total_matches == 2);
        }},
        {"literal_sidecar_missing_entries_and_file_fall_back_to_content", [] {
            Workspace workspace;
            write(workspace.root / "a.cpp", "target first\n" + std::string(700, 'x') + "\n");
            write(workspace.root / "b.cpp", "target second\n" + std::string(700, 'y') + "\n");
            auto build_options = options();
            build_options.threads = 1;
            create_index(workspace.root, workspace.index, build_options);
            const auto sidecar = only_sidecar(workspace.index);
            auto entries = table_entries(sidecar);
            REQUIRE(entries.size() == 2);
            entries.erase(entries.begin());
            replace_table(sidecar, entries);
            REQUIRE(Index(workspace.index, 64).search("\"target\"").total_matches == 2);
            REQUIRE(Index(workspace.index, 64).verify().files == 2);
            REQUIRE(fs::remove(sidecar));
            REQUIRE(Index(workspace.index, 64).search("\"target\"").total_matches == 2);
            REQUIRE(Index(workspace.index, 64).verify().files == 2);
        }},
        {"compact_literal_sidecars_are_optional_content_bound_and_rebuilt", [] {
            Workspace workspace;
            const std::string large_a = "needle_42 = 0;\n" + std::string(700, 'x') + "\n";
            const std::string large_b = "another_marker\n" + std::string(700, 'x') + "\n";
            write(workspace.root / "a.cpp", large_a);
            write(workspace.root / "b.cpp", large_b);
            write(workspace.root / "tiny.cpp", "tiny only\n");
            auto build_options = options();
            build_options.threads = 1;
            create_index(workspace.root, workspace.index, build_options);
            std::vector<fs::path> sidecars;
            for (const auto& entry : fs::recursive_directory_iterator(workspace.index))
                if (entry.path().filename() == "literal.tbl") sidecars.push_back(entry.path());
            REQUIRE(sidecars.size() == 1);
            auto cache = std::make_shared<disk::PageCache>(1024 * 1024);
            disk::Table table(sidecars.front(), cache);
            REQUIRE(table.count() == 2);
            REQUIRE(table.verify() > 0);
            std::map<std::pair<std::uint64_t, std::uint64_t>, std::uint32_t> identities;
            for (const auto& content : {large_a, large_b}) {
                storage::Hasher hasher;
                hasher.add(content);
                const auto fingerprint = hasher.finish();
                identities.emplace(std::pair{fingerprint.a, fingerprint.b}, storage::crc32(content));
            }
            table.each([&](auto key, auto value) {
                REQUIRE(key.size() == 8 && value.size() == 157);
                REQUIRE(value.substr(0, 5) == "RSLF1");
                std::size_t at = 5;
                const auto a = disk::get_u64(value, at), b = disk::get_u64(value, at);
                const auto crc = disk::get_u64(value, at);
                REQUIRE(identities.contains({a, b}));
                REQUIRE(crc == identities.at({a, b}));
                const auto filter = LiteralFilter::decode(value.substr(at));
                REQUIRE(filter.has_value());
                REQUIRE(filter->may_contain(std::string(10, 'x')));
            });
            REQUIRE(Index(workspace.index, 64).search("\"needle_42 = 0;\"").total_matches == 1);
            // Absence is the old-format fixture: the original term/snapshot
            // objects remain intact and literal verification must still work.
            for (const auto& path : sidecars) REQUIRE(fs::remove(path));
            REQUIRE(Index(workspace.index, 64).search("\"needle_42 = 0;\"").total_matches == 1);
            write(workspace.root / "a.cpp", "updated_marker\n" + std::string(700, 'y') + "\n");
            build_options.changed_only = true;
            build_options.changed_files = {"a.cpp"};
            REQUIRE(update_index(workspace.index, build_options).changed == 1);
            REQUIRE(Index(workspace.index, 64).search("\"needle_42 = 0;\"").total_matches == 0);
            REQUIRE(Index(workspace.index, 64).search("\"updated_marker\"").total_matches == 1);
            compact_index(workspace.index, 2, 64);
            garbage_collect(workspace.index, 1);
            REQUIRE(Index(workspace.index, 64).verify().files == 3);
            std::uint64_t regenerated = 0;
            for (const auto& entry : fs::recursive_directory_iterator(workspace.index))
                if (entry.path().filename() == "literal.tbl") {
                    disk::Table restored(entry.path(), std::make_shared<disk::PageCache>(1024 * 1024));
                    regenerated += restored.count();
                }
            REQUIRE(regenerated == 2);
            build_options.profile = "fast";
            build_options.changed_only = false;
            build_options.changed_files.clear();
            const auto fast = workspace.base / "fast";
            create_index(workspace.root, fast, build_options);
            for (const auto& entry : fs::recursive_directory_iterator(fast))
                REQUIRE(entry.path().filename() != "literal.tbl");
        }},
        {"posting_aggregate_preserves_frequency_spills_and_namespaces", [] {
            Workspace workspace;
            const std::string long_term = "identifier_" + std::string(100, 'x');
            std::string text = "prefix " + long_term + " aaa\n";
            for (unsigned i = 0; i < 7000; ++i)
                text += "unique_" + std::to_string(i) + " " + long_term + " aaa\n";
            write(workspace.root / "large.cpp", text);
            // Exercise reuse after the large aggregate and repeated documents
            // in the same worker. A gram and a whole token must stay distinct.
            write(workspace.root / "small.cpp", "bbb bbb bbb\n");
            for (const auto& profile : {"compact", "fast"}) {
                auto build_options = options();
                build_options.threads = 1;
                build_options.profile = profile;
                build_options.profile_stages = true;
                const auto directory = workspace.base / profile;
                const auto created = create_index(workspace.root, directory, build_options);
                REQUIRE(created.files == 2 && created.tokenized == 2);
                REQUIRE(created.profile.enabled);
                REQUIRE(created.profile.validation_seconds > 0);
                REQUIRE(created.profile.capture_seconds > 0);
                REQUIRE(created.profile.posting_write_seconds > 0);
                REQUIRE(created.profile.catalog_seconds > 0);
                Index index(directory, 64);
                REQUIRE(!index.policy().profile_stages);
                check_hit(index, long_term, "large.cpp", 7001, 1, 8);
                check_hit(index, "aaa", "large.cpp", 7001, 1, long_term.size() + 9);
                check_hit(index, "bbb", "small.cpp", 3, 1, 1);
                check_hit(index, "unique_6999", "large.cpp", 1, 7001, 1);
                const auto before = index.search(long_term, search_options());
                const auto noop = update_index(directory);
                REQUIRE(noop.tokenized == 0 && noop.unchanged == 2);
                compact_index(directory, 1, 64);
                const auto after = Index(directory, 64).search(long_term, search_options());
                REQUIRE(before.hits[0].score == after.hits[0].score);
                REQUIRE(before.hits[0].matches == after.hits[0].matches);
                REQUIRE(Index(directory, 64).verify().files == 2);
            }
        }},
        {"many_tiny_files_queue_updates_and_cancellation", [] {
            Workspace workspace;
            constexpr unsigned count = 1200;
            for (unsigned i = 0; i < count; ++i)
                write(workspace.root / (std::to_string(i) + ".txt"), "common tiny_" + std::to_string(i) + "\n");
            auto build_options = options();
            build_options.threads = 16;
            build_options.max_line_bytes = 16384;
            const auto created = create_index(workspace.root, workspace.index, build_options);
            REQUIRE(created.files == count && created.tokenized == count);
            REQUIRE(!created.profile.enabled);
            REQUIRE(created.profile.enumeration_seconds == 0 && created.profile.capture_seconds == 0);
            SearchOptions search;
            search.memory_mb = 64;
            search.limit = 0;
            search.snippets_per_file = 0;
            REQUIRE(Index(workspace.index, 64).search("common", search).total_matches == count);
            for (unsigned i = 0; i < count; i += 3)
                write(workspace.root / (std::to_string(i) + ".txt"), "changed tiny_" + std::to_string(i) + "\n");
            const auto updated = update_index(workspace.index, build_options);
            REQUIRE(updated.changed == count / 3);
            REQUIRE(updated.unchanged == count - count / 3);
            REQUIRE(Index(workspace.index, 64).search("changed", search).total_matches == count / 3);
            const auto generation = Index(workspace.index, 64).generation();
            std::atomic<unsigned> started{0};
            build_options.checkpoint = [&](const std::string& phase) {
                if (phase == "before_source_read" && started.fetch_add(1) == 20)
                    throw Error("E_TEST_CANCEL", "Injected worker cancellation");
            };
            rejects([&] { update_index(workspace.index, build_options); }, "E_TEST_CANCEL");
            REQUIRE(Index(workspace.index, 64).generation() == generation);
            REQUIRE(Index(workspace.index, 64).verify().files == count);
            build_options.checkpoint = {};
            const auto noop = update_index(workspace.index, build_options);
            REQUIRE(noop.tokenized == 0 && noop.unchanged == count);
        }},
        {"known_edit_updates_bound_workers_and_preserve_strict_scope", [] {
            Workspace workspace;
            write(workspace.root / "a.cpp", "alpha\n");
            write(workspace.root / "b.cpp", "bravo\n");
            auto build_options = options();
            build_options.threads = 16;
            build_options.max_line_bytes = 16384;
            create_index(workspace.root, workspace.index, build_options);
            Index pinned(workspace.index, 64);
            const auto stamp = fs::last_write_time(workspace.root / "b.cpp");
            write(workspace.root / "a.cpp", "omega\n");
            write(workspace.root / "b.cpp", "delta\n");
            fs::last_write_time(workspace.root / "b.cpp", stamp);
            build_options.changed_only = true;
            build_options.changed_files = {"a.cpp"};
            build_options.profile_stages = true;
            build_options.checkpoint = [](const std::string& stage) {
                if (stage == "before_source_read") throw Error("E_TEST_CANCEL", "Cancel inline update");
            };
            rejects([&] { update_index(workspace.index, build_options); }, "E_TEST_CANCEL");
            REQUIRE(Index(workspace.index, 64).generation() == pinned.generation());
            REQUIRE(!fs::exists(workspace.index / "writer.lock"));
            const auto caller = std::this_thread::get_id();
            bool read_on_caller = false;
            build_options.checkpoint = [&](const std::string& stage) {
                if (stage == "before_source_read") read_on_caller = std::this_thread::get_id() == caller;
            };
            const auto changed = update_index(workspace.index, build_options);
            build_options.checkpoint = {};
            REQUIRE(changed.workers == 1 && !changed.global_scan);
            REQUIRE(read_on_caller);
            REQUIRE(changed.changed == 1 && changed.hashed == 1 && changed.files == 2);
            REQUIRE(changed.profile.metadata_lookup_seconds > 0);
            REQUIRE(Index(workspace.index, 64).search("bravo").total_matches == 1);
            REQUIRE(Index(workspace.index, 64).search("delta").total_matches == 0);
            REQUIRE(pinned.search("alpha").total_matches == 1);
            build_options.changed_only = false;
            build_options.changed_files.clear();
            const auto strict = update_index(workspace.index, build_options);
            REQUIRE(strict.global_scan && strict.hashed == 2 && strict.changed == 1);
            REQUIRE(Index(workspace.index, 64).search("delta").total_matches == 1);
            REQUIRE(pinned.search("bravo").total_matches == 1);
            const auto renamed = path_from_utf8("nested/новое.cpp");
            fs::create_directory(workspace.root / "nested");
            fs::rename(workspace.root / "a.cpp", workspace.root / renamed);
            build_options.changed_only = true;
            build_options.changed_files = {"a.cpp", path_to_utf8(renamed)};
            const auto moved = update_index(workspace.index, build_options);
            REQUIRE(moved.workers == 2 && moved.hashed == 1 && moved.added == 1 && moved.removed == 1);
            check_hit(Index(workspace.index, 64), "omega", path_to_utf8(renamed), 1, 1, 1);
            build_options.changed_files = {path_to_utf8(renamed)};
            build_options.exclude_paths.push_back(path_to_utf8(renamed));
            const auto excluded = update_index(workspace.index, build_options);
            REQUIRE(excluded.workers == 1 && excluded.hashed == 0 && excluded.removed == 1 && excluded.files == 1);
            fs::remove(workspace.root / "b.cpp");
            build_options.changed_files = {"b.cpp"};
            const auto removed = update_index(workspace.index, build_options);
            REQUIRE(removed.workers == 1 && removed.hashed == 0 && removed.removed == 1 && removed.files == 0);
            build_options.changed_files.clear();
            const auto empty = update_index(workspace.index, build_options);
            REQUIRE(empty.workers == 1 && !empty.global_scan && empty.hashed == 0 && empty.files == 0);
            REQUIRE(Index(workspace.index, 64).verify().files == 0);
            REQUIRE(pinned.verify().files == 2);
        }},
        {"update_catalog_cursors_respect_small_memory_budgets", [] {
            for (const auto& profile : {std::string("compact"), std::string("fast")}) {
                Workspace workspace;
                write(workspace.root / "a.cpp", "alpha\n");
                write(workspace.root / "b.cpp", "bravo\n");
                auto build_options = options();
                build_options.profile = profile;
                build_options.max_line_bytes = 16384;
                build_options.threads = 16;
                create_index(workspace.root, workspace.index, build_options);
                write(workspace.root / "a.cpp", "omega\n");
                build_options.changed_only = true;
                build_options.changed_files = {"a.cpp"};
                build_options.memory_mb = profile == "compact" ? 16 : 24;
                const auto changed = update_index(workspace.index, build_options);
                REQUIRE(changed.workers == 1 && changed.changed == 1 && changed.hashed == 1);
                REQUIRE(changed.memory_budget_bytes == build_options.memory_mb * 1024ULL * 1024ULL);
                build_options.changed_only = false;
                build_options.changed_files.clear();
                build_options.memory_mb = profile == "compact" ? 24 : 32;
                const auto global = update_index(workspace.index, build_options);
                REQUIRE(global.workers == 1 && global.global_scan && global.hashed == 2 && global.changed == 0);
                REQUIRE(Index(workspace.index, 64).verify().files == 2);
                check_hit(Index(workspace.index, 64), "omega", "a.cpp", 1, 1, 1);
            }
        }},
        {"update_catalog_cursor_rejects_valid_crc_wrong_path_identity", [] {
            Workspace workspace;
            write(workspace.root / "a.cpp", "alpha\n");
            write(workspace.root / "b.cpp", "bravo\n");
            auto build_options = options();
            const auto created = create_index(workspace.root, workspace.index, build_options);
            const auto bucket = storage::term_hash("a.cpp") & 1023U;
            const auto catalog = workspace.index / ("catalog-" + std::to_string(created.generation) + "-" + std::to_string(bucket) + ".tbl");
            std::map<std::string, std::string> rows;
            {
                disk::Table table(catalog, std::make_shared<disk::PageCache>(1024 * 1024));
                table.each([&](auto key, auto value) { rows.emplace(key, value); });
            }
            // Keep a valid page CRC but make the path directory refer to an
            // impossible record. Lookup must fail without publishing a commit.
            rows.at("Pa.cpp") = disk::ordered((bucket << 54) | ((1ULL << 54) - 2));
            {
                disk::TableWriter table(catalog);
                for (const auto& [key, value] : rows) table.add(key, value);
                table.finish();
            }
            build_options.changed_only = true;
            build_options.changed_files = {"a.cpp"};
            rejects([&] { update_index(workspace.index, build_options); }, "E_INDEX_CORRUPT");
            REQUIRE(Index(workspace.index, 64).generation() == created.generation);
        }}
    });
}
