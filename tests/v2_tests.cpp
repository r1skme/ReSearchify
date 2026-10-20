#include "test_support.hpp"
#include "researchify/api.hpp"
#include "researchify/query.hpp"
#include <algorithm>
#include <future>
#include <random>
#include <set>
#include <sstream>
#include <thread>
using namespace test;
using J = researchify::json::Value;
using O = J::Object;
using A = J::Array;
J request(const std::string& command, const fs::path& index) { return O{{"command", command}, {"index", path_to_utf8(index)}, {"memory_mb", 64}};
}
J ok(Service& service, const J& r) {
    auto out = service.execute(r);
    if (out.at("status").text() != "ok") throw std::runtime_error("API failed: " + json::dump(out));
    return out;
}
void error(Service& s, const J& r, const std::string& code) {
    auto out = s.execute(r);
    REQUIRE(out.at("status").text() == "error");
    if (out.at("error").at("code").text() != code) throw std::runtime_error("Expected " + code + ": " + json::dump(out));
    REQUIRE(!out.at("execution_complete").boolean());
}
std::set<std::string> paths(const SearchReport& r) { std::set<std::string> out;
for (const auto& h : r.hits) out.insert(h.path);
return out;
}
std::set<std::string> brute(const std::map<std::string, std::string>& source, const std::string& text) {
    auto q = parse_query(text);
    std::set<std::string> out;
    for (const auto& [name, content] : source) {
        std::vector<bool> present(q.leaves.size());
        std::istringstream lines(content);
        std::string line;
        while (std::getline(lines, line)) for (std::size_t i = 0; i < q.leaves.size(); ++i) {
            const auto& leaf = q.leaves[i];
            if (leaf.kind == LeafKind::Term) for_each_token(line, [&](Token t) { if (t.text == leaf.text) present[i] = true; });
            else if (leaf.kind == LeafKind::Phrase) { if (line.find(leaf.text) != line.npos) present[i] = true;
            }
            else { RegexMatch m;
            std::uint64_t budget = 10000000;
            if (leaf.regex->find(line, m, budget)) present[i] = true;
            }
        }
        if (q.evaluate(present)) out.insert(name);
    }
    return out;
}
std::vector<J> rpc_exchange(const fs::path& index, const std::vector<J>& messages) {
    std::stringstream in, out;
    for (const auto& m : messages) in << json::dump(m) << '\n';
    REQUIRE(run_mcp(in, out, index) == 0);
    std::vector<J> response;
    std::string line;
    while (std::getline(out, line)) response.push_back(json::parse(line));
    return response;
}
J initialize(const std::string& version = "2025-11-25") {
    return O{{"jsonrpc", "2.0"}, {"id", 1}, {"method", "initialize"},
        {"params", O{{"protocolVersion", version}, {"capabilities", O{}}, {"clientInfo", O{{"name", "test"}, {"version", "1"}}}}}};
}
J initialized() { return O{{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}};
}
int main() {
    return run({
        {"term_search_uses_index_and_lazy_snippets", [] {
            Workspace w;
            for (int i = 0; i < 200; ++i) write(w.root / (std::to_string(i) + ".cpp"), "common common needle_" + std::to_string(i) + "\n");
            create_index(w.root, w.index, options());
            Index index(w.index, 64);
            SearchOptions o;
            o.memory_mb = 64;
            o.snippets_per_file = 0;
            auto all = index.search("common", o);
            REQUIRE(all.total_matches == 200);
            REQUIRE(all.scanned_bytes == 0);
            REQUIRE(all.candidates == 200);
            o.limit = 2;
            o.snippets_per_file = 1;
            auto two = index.search("common", o);
            REQUIRE(two.hits.size() == 2);
            REQUIRE(two.scanned_bytes < 100);
            auto rare = index.search("common AND needle_72", o);
            REQUIRE(rare.candidates == 1 && rare.total_matches == 1);
            o.limit = 0;
            REQUIRE(index.search("common", o).scanned_bytes == 0);
        }},
        {"regex_trigram_oracle_no_false_negatives", [] {
            Workspace w;
            std::map<std::string, std::string> source;
            const std::vector<std::string> pieces{"RenderFrame", "RenderFooFrame", "Update", "world", "alpha beta", "alphabeta", "xyz", "ПриветМир", "return common;", "common worker123", "foo", "barbaz", "", "abababab", "a", "z", "abc", "def"};
            std::mt19937 rng(913);
            for (unsigned n = 0; n < 90; ++n) {
                auto name = std::to_string(n) + ".txt";
                auto text = pieces[rng() % pieces.size()] + " " + pieces[rng() % pieces.size()] + "\n" + pieces[rng() % pieces.size()] + "\n";
                source.emplace(name, text);
                write(w.root / name, text);
            }
            auto fast_options = options();
            fast_options.profile = "fast";
            create_index(w.root, w.index, fast_options);
            auto compact_options = options();
            compact_options.profile = "compact";
            create_index(w.root, w.base / "compact", compact_options);
            Index fast(w.index, 64), compact(w.base / "compact", 64);
            SearchOptions o;
            o.limit = 100;
            o.memory_mb = 64;
            o.snippets_per_file = 0;
            const std::vector<std::string> queries{
                "/Render[A-Za-z]*Frame/", "/Render|Update/", "/(Render)?world/", "/(Render|Update)?/", "/foo(barbaz)?/",
                "/(abc|def)+/", "/(abc|def)*/", "/(ab){2,4}/", "/(abc|a)*/", "/(ab|abc){0,2}/", "/^$/", "/.*/",
                "/Привет.*Мир/", "/(Render|Привет).*([A-Z]|М)/", "/alpha.?beta/", "/worker[0-9]+/", "/[a-z]+/",
                "NOT /Render.*Frame/", "common AND NOT /worker[0-9]+/", "/Render|Update/ OR NOT common",
                "(common OR \"alpha beta\") AND NOT /worker[0-9]+/", "\"return common;\"", "NOT \"return common;\"",
                "common OR /xyz/", "NOT NOT /world/", "(foo AND /barbaz/) OR (common AND NOT /xyz/)"};
            for (const auto& q : queries) { auto expected = brute(source, q);
            REQUIRE(paths(fast.search(q, o)) == expected);
            REQUIRE(paths(compact.search(q, o)) == expected);
            }
            auto r = fast.search("/worker[0-9]+/", o);
            REQUIRE(r.candidates < source.size());
        }},
        {"saved_policy_exclusions_and_changed_file_scope", [] {
            Workspace w;
            write(w.root / "a.cpp", "alpha\n");
            write(w.root / "b.cpp", "beta\n");
            write(w.root / "generated" / "x", "ignored\n");
            auto o = options();
            o.exclude_dirs.push_back("generated");
            o.profile = "compact";
            create_index(w.root, w.index, o);
            write(w.root / "a.cpp", "omega\n");
            write(w.root / "b.cpp", "gamma\n");
            o.changed_only = true;
            o.changed_files = {"a.cpp"};
            auto r = update_index(w.index, o);
            REQUIRE(!r.global_scan && r.hashed == 1 && r.tokenized == 1);
            REQUIRE(!r.warnings.empty());
            REQUIRE(Index(w.index).search("beta").total_matches == 1);
            REQUIRE(Index(w.index).search("gamma").total_matches == 0);
            auto full = update_index(w.index);
            REQUIRE(full.global_scan);
            REQUIRE(full.tokenized == 1);
            REQUIRE(full.files == 2);
            REQUIRE(Index(w.index).policy().profile == "compact");
            REQUIRE(Index(w.index).search("ignored").total_matches == 0);
            fs::remove(w.root / "a.cpp");
            auto deleted = update_index(w.index, o);
            REQUIRE(deleted.removed == 1 && deleted.files == 1);
            o.changed_files = {"b.cpp", "b.cpp"};
            rejects([&] { update_index(w.index, o); }, "E_ARGUMENT");
        }},
        {"profile_switch_requires_reindex_not_missing_grams", [] {
            Workspace w;
            write(w.root / "a", "alphabet worker123\n");
            auto o = options();
            o.profile = "compact";
            create_index(w.root, w.index, o);
            o.profile = "fast";
            rejects([&] { update_index(w.index, o); }, "E_ARGUMENT");
            REQUIRE(Index(w.index).search("/worker[0-9]+/").total_matches == 1);
            create_index(w.root, w.base / "fast", o);
            REQUIRE(Index(w.base / "fast").search("/worker[0-9]+/").total_matches == 1);
            Index(w.index).verify();
        }},
        {"catalog_updates_are_sharded_and_compaction_bounds_history", [] {
            Workspace w;
            for (int n = 0; n < 2000; ++n) write(w.root / (std::to_string(n) + ".cpp"), "common x\n");
            auto o = options();
            o.profile = "compact";
            create_index(w.root, w.index, o);
            auto initial = bytes(w.index);
            write(w.root / "7.cpp", "changed!\n");
            o.changed_only = true;
            o.changed_files = {"7.cpp"};
            auto r = update_index(w.index, o);
            REQUIRE(r.tokenized == 1);
            auto growth = bytes(w.index) - initial;
            REQUIRE(growth < 50000);
            for (unsigned n = 0; n < 30; ++n) { write(w.root / "7.cpp", "version" + std::to_string(n) + " common\n");
            update_index(w.index, o);
            }
            auto grown = bytes(w.index);
            compact_index(w.index, 3, 64);
            auto gc = garbage_collect(w.index, 1);
            REQUIRE(gc.generations_removed >= 31);
            REQUIRE(bytes(w.index) < grown);
            REQUIRE(Index(w.index).verify().files == 2000);
        }},
        {"pinned_snapshot_survives_update_compact_and_gc", [] {
            Workspace w;
            write(w.root / "a", "oldterm\n");
            create_index(w.root, w.index, options());
            std::uint64_t old_generation;
            {
                Index old(w.index, 64);
                old_generation = old.generation();
                write(w.root / "a", "newterm\n");
                update_index(w.index);
                fs::remove_all(w.root);
                compact_index(w.index, 2, 64);
                auto gc = garbage_collect(w.index, 1);
                REQUIRE(gc.pinned >= 1);
                REQUIRE(old.search("oldterm").total_matches == 1);
                REQUIRE(old.read("a").text == "oldterm\n");
                REQUIRE(Index(w.index).search("newterm").total_matches == 1);
            }
            garbage_collect(w.index, 1);
            rejects([&] { Index old(w.index, 64, old_generation); }, "E_SNAPSHOT_GONE");
            REQUIRE(Index(w.index).verify().files == 1);
        }},
        {"concurrent_gc_and_reader_open_are_coordinated", [] {
            Workspace w;
            write(w.root / "a", "common\n");
            create_index(w.root, w.index, options());
            std::atomic<unsigned> failures{0};
            auto readers = std::async(std::launch::async, [&] {
                for (unsigned k = 0; k != 80; ++k) try { Index i(w.index, 32);
                if (i.search("common").total_matches != 1) ++failures;
                }
                catch (...) { ++failures;
                }
            });
            for (unsigned k = 0; k != 12; ++k) {
                write(w.root / "a", "common version" + std::to_string(k) + "\n");
                update_index(w.index);
                garbage_collect(w.index, 1);
            }
            readers.get();
            REQUIRE(failures.load() == 0);
        }},
        {"snapshot_read_stale_workspace_and_traversal", [] {
            Workspace w;
            write(w.root / "a", "alpha\nbeta\ngamma\n");
            create_index(w.root, w.index, options());
            Index i(w.index);
            auto r = i.read("a", 2, 3);
            REQUIRE(r.text == "beta\ngamma\n");
            REQUIRE(r.first_line == 2 && r.last_line == 3);
            REQUIRE(i.read("a", 1, 2, "workspace", r.content_id).text == "alpha\nbeta\n");
            write(w.root / "a", "different\n");
            REQUIRE(i.read("a", 1, 1).text == "alpha\n");
            rejects([&] { i.read("a", 1, 2, "workspace", r.content_id); }, "E_STALE_READ");
            rejects([&] { i.read("a", 1, 2, "snapshot", "wrong"); }, "E_STALE_READ");
            for (const auto& p : {"../a", "/a", "a/../a", "a\\b", "C:a"}) rejects([&] { i.read(p); }, "E_SCOPE");
            auto short_read = i.read("a", 1, 3, "snapshot", "", 16);
            REQUIRE(short_read.truncated && short_read.last_line == 2);
        }},
        {"path_globs_basename_and_double_star", [] {
            REQUIRE(match_path("*.cpp", "src/a.cpp"));
            REQUIRE(!match_path("src/*.cpp", "src/deep/a.cpp"));
            REQUIRE(match_path("src/**/*.cpp", "src/a.cpp"));
            REQUIRE(match_path("src/**/*.cpp", "src/deep/a.cpp"));
            REQUIRE(match_path("**/a.cpp", "a.cpp"));
            REQUIRE(match_path("**", "src/Привет.cpp"));
            REQUIRE(match_path("src/Привет.cpp", "src/Привет.cpp"));
        }},
        {"budgets_abort_without_partial_success", [] {
            Workspace w;
            for (int n = 0; n < 10; ++n) write(w.root / std::to_string(n), "common worker123 common\n");
            create_index(w.root, w.index, options());
            Index i(w.index);
            SearchOptions o;
            o.max_candidates = 2;
            rejects([&] { i.search("common", o); }, "E_BUDGET");
            o.max_candidates = 0;
            o.max_scan_bytes = 2;
            rejects([&] { i.search("/worker[0-9]+/", o); }, "E_BUDGET");
            o.max_scan_bytes = 0;
            o.max_regex_steps = 1;
            rejects([&] { i.search("/worker[0-9]+/", o); }, "E_BUDGET");
        }},
        {"api_schema_validation_and_json_query_tree", [] {
            Workspace w;
            write(w.root / "a", "alpha beta\n");
            write(w.root / "b", "beta gamma\n");
            create_index(w.root, w.index, options());
            Service service;
            auto r = request("find", w.index);
            r["query"] = json::parse(R"({"and":[{"term":"beta"},{"not":{"term":"gamma"}}]})");
            REQUIRE(ok(service, r).at("total").integer() == 1);
            r["bad_field"] = true;
            error(service, r, "E_ARGUMENT");
            r.object().erase("bad_field");
            r["api_version"] = 999;
            error(service, r, "E_API_VERSION");
            r["api_version"] = 1;
            r["query"] = "alpha AND (";
            error(service, r, "E_QUERY_SYNTAX");
            r["query"] = O{{"term", "alpha beta"}};
            error(service, r, "E_QUERY_SYNTAX");
            REQUIRE(command_schema("find").at("inputSchema").at("properties").contains("query"));
            REQUIRE(command_schema("search").at("name").text() == "find");
            REQUIRE(capabilities().at("api_version").integer() == 1);
        }},
        {"session_pin_refresh_readonly_and_scope", [] {
            Workspace w;
            write(w.root / "a", "alpha\n");
            create_index(w.root, w.index, options());
            Service session(w.index, true);
            J r = O{{"command", "find"}, {"query", "alpha"}};
            REQUIRE(ok(session, r).at("total").integer() == 1);
            write(w.root / "a", "omega\n");
            update_index(w.index);
            REQUIRE(ok(session, r).at("total").integer() == 1);
            r["refresh"] = true;
            REQUIRE(ok(session, r).at("total").integer() == 0);
            error(session, O{{"command", "update"}}, "E_READ_ONLY");
            error(session, O{{"command", "gc"}}, "E_READ_ONLY");
            r["index"] = path_to_utf8(w.base / "other");
            error(session, r, "E_SCOPE");
        }},
        {"pagination_query_generation_binding_and_projection", [] {
            Workspace w;
            for (int n = 0; n < 38; ++n) write(w.root / (std::to_string(n) + ".cpp"), "common common\n");
            create_index(w.root, w.index, options());
            Service service;
            auto r = request("find", w.index);
            r["query"] = "common";
            r["limit"] = 5;
            r["fields"] = A{J("path")};
            r["sort"] = "path";
            std::set<std::string> names;
            std::string cursor;
            for (;;) {
                auto out = ok(service, r);
                REQUIRE(out.at("total").integer() == 38);
                REQUIRE(out.at("metrics").at("scanned_bytes").integer() == 0);
                for (const auto& h : out.at("results").array()) { REQUIRE(h.object().size() == 1);
                REQUIRE(names.insert(h.at("path").text()).second);
                }
                if (out.at("next_cursor").is_null()) break;
                cursor = out.at("next_cursor").text();
                r["cursor"] = cursor;
            }
            REQUIRE(names.size() == 38);
            r["query"] = "different";
            error(service, r, "E_CURSOR");
            r["query"] = "common";
            auto generation = Index(w.index).generation();
            write(w.root / "a.cpp", "common\n");
            update_index(w.index);
            REQUIRE(ok(service, r).at("snapshot").at("generation").text() == std::to_string(generation));
            service.refresh();
            garbage_collect(w.index, 1);
            error(service, r, "E_SNAPSHOT_GONE");
        }},
        {"ranked_keyset_matches_single_page", [] {
            Workspace w;
            for (int n = 0; n < 45; ++n) write(w.root / (std::to_string(n) + ".cpp"), std::string(static_cast<std::size_t>(n % 7), '\n') + "common common common\n" + std::string(static_cast<std::size_t>(n % 11), 'a'));
            create_index(w.root, w.index, options());
            Service service;
            auto r = request("find", w.index);
            r["query"] = "common";
            r["limit"] = 45;
            r["snippets"] = 0;
            auto all = ok(service, r).at("results").array();
            r["limit"] = 4;
            A pages;
            for (;;) { auto out = ok(service, r);
            for (const auto& h : out.at("results").array()) pages.push_back(h);
            if (out.at("next_cursor").is_null()) break;
            r["cursor"] = out.at("next_cursor");
            }
            REQUIRE(json::dump(J(pages)) == json::dump(J(all)));
        }},
        {"output_budget_counts_escaped_envelope", [] {
            Workspace w;
            const auto line = std::string(80, '\1') + " common";
            for (int n = 0; n < 50; ++n) write(w.root / (std::to_string(n) + ".cpp"), line + "\n" + line + "\n");
            create_index(w.root, w.index, options());
            Service service;
            auto r = request("find", w.index);
            r["query"] = "common";
            r["limit"] = 50;
            r["max_output_bytes"] = 1024;
            auto out = service.execute(r);
            REQUIRE(json::dump(out).size() + 2 <= 1024);
            REQUIRE(out.at("status").text() == "ok");
            REQUIRE(out.at("output_truncated").boolean());
            auto read_request = request("read", w.index);
            read_request["file"] = "0.cpp";
            read_request["max_output_bytes"] = 1024;
            auto text = ok(service, read_request);
            REQUIRE(json::dump(text).size() + 2 <= 1024);
            auto returned = static_cast<std::uint64_t>(std::count(text.at("text").text().begin(), text.at("text").text().end(), '\n'));
            REQUIRE(text.at("last_line").integer() == returned);
            r["query"] = std::string(300, '\1') + " AND (";
            r["id"] = std::string(128, '\2');
            out = service.execute(r);
            REQUIRE(json::dump(out).size() + 2 <= 1024);
        }},
        {"mcp_handshake_tools_and_call", [] {
            Workspace w;
            write(w.root / "a", "alpha beta\n");
            create_index(w.root, w.index, options());
            auto responses = rpc_exchange(w.index, {initialize(), initialized(),
                O{{"jsonrpc", "2.0"}, {"id", 2}, {"method", "tools/list"}},
                O{{"jsonrpc", "2.0"}, {"id", 3}, {"method", "tools/call"}, {"params", O{{"name", "researchify_find"}, {"arguments", O{{"query", "alpha"}}}}}}});
            REQUIRE(responses.size() == 3);
            REQUIRE(responses[0].at("result").at("protocolVersion").text() == "2025-11-25");
            REQUIRE(responses[1].at("result").at("tools").array().size() == 4);
            const auto& result = responses[2].at("result");
            REQUIRE(!result.at("isError").boolean());
            REQUIRE(result.at("structuredContent").at("total").integer() == 1);
            REQUIRE(json::parse(result.at("content").array()[0].at("text").text()).at("total").integer() == 1);
        }},
        {"mcp_protocol_errors_legacy_and_readonly_scope", [] {
            Workspace w;
            write(w.root / "a", "alpha\n");
            create_index(w.root, w.index, options());
            auto rpc = [](int id, const std::string& method, J params = O{}) { return J(O{{"jsonrpc", "2.0"}, {"id", id}, {"method", method}, {"params", params}});
            };
            auto out = rpc_exchange(w.index, {rpc(0, "tools/list"), initialize("2024-11-05"), initialized(), rpc(2, "unknown"),
                rpc(3, "tools/call", O{{"name", "researchify_update"}, {"arguments", O{}}}),
                rpc(4, "tools/call", O{{"name", "researchify_find"}, {"arguments", O{{"query", "alpha"}, {"index", "other"}}}}),
                rpc(5, "tools/call", O{{"name", "researchify_find"}, {"arguments", O{{"query", "alpha"}}}}), rpc(6, "ping")});
            REQUIRE(out.size() == 7);
            REQUIRE(out[0].contains("error"));
            REQUIRE(out[2].contains("error"));
            REQUIRE(out[3].contains("error"));
            REQUIRE(out[4].contains("error"));
            REQUIRE(out[5].at("result").contains("content"));
            REQUIRE(!out[5].at("result").contains("structuredContent"));
            REQUIRE(out[6].at("result").object().empty());
            std::stringstream in("{bad}\n"), response;
            run_mcp(in, response, w.index);
            REQUIRE(json::parse(response.str()).contains("error"));
        }}
    });
}
