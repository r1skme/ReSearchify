#include "researchify/query.hpp"
#include "researchify/engine.hpp"
#include "../src/query_internal.hpp"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <iostream>
#include <random>
#include <regex>
#include <string>
#include <thread>
#include <vector>

namespace {
int checks = 0;
void require(bool condition, const std::string& message) {
    ++checks;
    if (!condition) throw std::runtime_error(message);
}
void rejects(const std::function<void()>& action, const std::string& name) {
    bool rejected = false;
    try { action(); } catch (const researchify::Error&) { rejected = true; }
    require(rejected, "expected rejection: " + name);
}
void match(const std::string& expression, const std::string& text,
           std::size_t begin, std::size_t end, std::size_t start = 0) {
    researchify::Regex regex(expression);
    researchify::RegexMatch result;
    std::uint64_t budget = 10000000;
    require(regex.find(text, result, budget, start), "regex should match: " + expression);
    require(result.begin == begin && result.end == end, "wrong match range: " + expression);
    require(budget < 10000000, "regex did not account for work");
}
void no_match(const std::string& expression, const std::string& text) {
    researchify::Regex regex(expression);
    researchify::RegexMatch result;
    std::uint64_t budget = 10000000;
    require(!regex.find(text, result, budget), "unexpected regex match: " + expression);
}

void utf8_and_tokens() {
    using namespace researchify;
    require(valid_utf8(""), "empty UTF-8");
    require(valid_utf8("ASCII\t\r\n"), "ASCII UTF-8");
    require(valid_utf8("привет 世界 😀"), "Unicode UTF-8");
    require(valid_utf8(std::string("a\0b", 3)), "NUL is a valid Unicode scalar");
    for (const std::string& invalid : {
             std::string("\x80", 1), std::string("\xc0\xaf", 2), std::string("\xc1\xbf", 2),
             std::string("\xe0\x80\xaf", 3), std::string("\xed\xa0\x80", 3),
             std::string("\xf0\x80\x80\x80", 4), std::string("\xf4\x90\x80\x80", 4),
             std::string("\xf5\x80\x80\x80", 4), std::string("\xe2\x82", 2),
             std::string("\xff", 1), std::string("a\xc3z", 3)}) {
        require(!valid_utf8(invalid), "accepted malformed UTF-8");
        rejects([&] { for_each_token(invalid, [](Token) {}); }, "malformed token stream");
    }
    std::vector<std::string> tokens;
    std::vector<std::size_t> columns;
    for_each_token("  foo_42::бар + 世界;", [&](Token t) {
        tokens.emplace_back(t.text); columns.push_back(t.column);
    });
    require(tokens == std::vector<std::string>{"foo_42", "бар", "世界"}, "token boundaries");
    require(columns == std::vector<std::size_t>{2, 10, 19}, "token byte columns");
    tokens.clear();
    for_each_token("-+ ;\t", [&](Token t) { tokens.emplace_back(t.text); });
    require(tokens.empty(), "punctuation is not an identifier");
    for_each_token("abc", [&](Token t) { tokens.emplace_back(t.text); });
    require(tokens == std::vector<std::string>{"abc"}, "final token without delimiter");
    for (std::size_t padding = 0; padding < 24; ++padding) {
        const std::string prefix(padding, 'a');
        require(valid_utf8(prefix + "😀" + std::string(24 - padding, 'b')), "Unicode scalar at varied byte offsets");
        require(!valid_utf8(prefix + std::string("\xed\xa0\x80", 3)), "surrogate rejection at varied byte offsets");
        require(!valid_utf8(prefix + std::string("\xf0\x80\x80\x80", 4)), "overlong rejection at varied byte offsets");
        require(!valid_utf8(prefix + std::string("\xf0\x90\x80", 3)), "truncated tail at varied byte offsets");
    }
}

void parser() {
    using namespace researchify;
    auto q = parse_query("alpha OR beta AND NOT gamma");
    require(q.leaves.size() == 3, "query leaves");
    require(q.evaluate({true, false, true}), "AND/OR precedence");
    require(q.evaluate({false, true, false}), "NOT precedence");
    require(!q.evaluate({false, true, true}), "negative term");
    auto grouped = parse_query("(alpha OR beta) gamma");
    require(!grouped.evaluate({true, false, false}), "parentheses and implicit AND");
    require(grouped.evaluate({false, true, true}), "parentheses match");
    require(parse_query("NOT NOT a").evaluate({true}), "double NOT");
    require(parse_query("and").leaves[0].text == "and", "operators are uppercase only");
    require(parse_query("\"AND\"").leaves[0].kind == LeafKind::Phrase, "quoted operator");
    auto phrase = parse_query("\"foo \\\"quoted\\\" \\\\ tab\\t\"");
    require(phrase.leaves[0].text == "foo \"quoted\" \\ tab\t", "phrase escapes");
    auto regex = parse_query("/path\\/file/ AND функция");
    require(regex.leaves[0].kind == LeafKind::Regex && regex.leaves[0].regex, "regex leaf compiled");
    require(regex.leaves[1].text == "функция", "Unicode term");
    require(parse_query("//").leaves[0].regex != nullptr, "empty regex allowed");
    for (const auto* invalid : {"", " ", "()", "a AND", "OR a", "a OR OR b", "NOT", "(a", "a)",
                                "\"", "\"\"", "/", "/[/", "a-b", "std::vector", "\"a\\q\"", "\"a\\nb\""}) {
        rejects([&] { parse_query(invalid); }, invalid);
    }
    rejects([&] { parse_query(std::string(8193, 'a')); }, "query length");
    rejects([&] { parse_query(std::string(65, '(') + "a" + std::string(65, ')')); }, "query nesting");
    std::string many = "a";
    for (unsigned i = 1; i < 128; ++i) many += " a";
    require(parse_query(many).leaves.size() == 128, "128 leaves allowed");
    rejects([&] { parse_query(many + " a"); }, "129 leaves rejected");
    rejects([&] { q.evaluate({true}); }, "short evaluation vector");
    rejects([&] { q.possible({0, 0, 3}); }, "invalid possibility value");
    const std::string full_program = "/a{1000}a{1000}a{1000}a{1000}a{90}/";
    const auto exact_regex = parse_query(full_program);
    require(exact_regex.leaves[0].regex->state_count() == 4096, "exact per-regex state limit");
    std::string full_query = full_program;
    for (unsigned i = 1; i < 8; ++i) full_query += " OR " + full_program;
    const auto compiled = parse_query(full_query);
    std::size_t total_states = 0;
    for (const auto& leaf : compiled.leaves) total_states += leaf.regex->state_count();
    require(total_states == 32768, "exact aggregate regex state limit");
    rejects([&] { parse_query(full_query + " OR //"); }, "aggregate NFA state cap");
    rejects([&] { parse_query(full_query + " OR " + full_program); }, "many compressed NFA expansions");
}

void truth_tables() {
    using namespace researchify;
    const std::vector<std::string> expressions{
        "a AND b", "a OR b", "NOT a AND b", "NOT (a OR b)", "NOT (a AND b)",
        "(a OR NOT b) AND c", "NOT NOT a", "a OR b AND c", "a AND (b OR c)"};
    for (const auto& expression : expressions) {
        const auto query = parse_query(expression);
        std::size_t assignments = 1;
        for (std::size_t i = 0; i < query.leaves.size(); ++i) assignments *= 3;
        for (std::size_t a = 0; a < assignments; ++a) {
            std::vector<std::uint8_t> values(query.leaves.size());
            auto rest = a;
            for (auto& value : values) { value = static_cast<std::uint8_t>(rest % 3); rest /= 3; }
            bool can_true = false, can_false = false;
            for (std::size_t concrete = 0; concrete < (std::size_t{1} << values.size()); ++concrete) {
                bool consistent = true;
                std::vector<bool> real(values.size());
                for (std::size_t i = 0; i < values.size(); ++i) {
                    real[i] = (concrete & (std::size_t{1} << i)) != 0;
                    if (values[i] != 2 && real[i] != (values[i] != 0)) consistent = false;
                }
                if (consistent) {
                    if (query.evaluate(real)) can_true = true;
                    else can_false = true;
                }
            }
            const auto expected = can_true && can_false ? 2 : can_true ? 1 : 0;
            require(query.possible(values) == expected, "three-valued truth table: " + expression);
        }
    }
}

void regex_matching() {
    match("abc", "xxabczz", 2, 5);
    match("a+", "zaaaa", 1, 2);
    match("a+", "zaaaa", 2, 3, 2);
    match("(abc|ab)", "zabc", 1, 3);
    match("ab.*z|b", "abxxz", 0, 5);
    match("ab.*z|b", "abxx", 1, 2);
    match("a*", "aaaa", 0, 0);
    match("a*", "bbbb", 0, 0);
    match("", "abc", 0, 0);
    match("", "abc", 3, 3, 3);
    match("^$", "", 0, 0);
    match("$", "abc", 3, 3);
    match("^a", "abc", 0, 1);
    no_match("^a", "za");
    no_match("^a", "za\na");
    no_match("a$", "aa\n");
    no_match(".", "\n");
    no_match(".", "\r");
    match("[^a-c]", "abcq", 3, 4);
    match("[a-cx-z]+", "qxyz", 1, 2);
    match("[a-cx-z]+$", "qxyz", 1, 4);
    match("[a-a]", "ba", 1, 2);
    match("[a-]", "-", 0, 1);
    match("[-a]", "-", 0, 1);
    match("[\\]\\-]", "]", 0, 1);
    match("\\d{2,4}", "a12345", 1, 3);
    match("\\d{2,4}$", "a1234", 1, 5);
    match("a{2,}", "aaaa", 0, 2);
    match("a{0}b", "b", 0, 1);
    match("(ab){2}", "abab", 0, 4);
    match("a?b", "b", 0, 1);
    match("(a?)*b", "aaab", 0, 4);
    match("((a*)*)*b", "aaab", 0, 4);
    match("\\bword\\b", "a word!", 2, 6);
    no_match("\\bword\\b", "words");
    match("\\Bword", "sword", 1, 5);
    match("\\s+", "x\t ", 1, 2);
    match("\\S", " \tx", 2, 3);
    match("\\D", "42x", 2, 3);
    match("\\W", "foo!", 3, 4);
    match("[\\D0]", "120", 2, 3);
    match("[^\\d]", "42x", 2, 3);
    match("\\x41\\u0042\\U00000043", "ABC", 0, 3);
    match("path\\/file", "path/file", 0, 9);
    match("\\.\\*\\+\\?\\(\\)\\[\\]\\{\\}\\|\\^\\$\\\\", ".*+?()[]{}|^$\\", 0, 14);
    match("привет", "xприветy", 1, 13);
    match("[а-я]+$", "123абв", 3, 9);
    match(".", "😀", 0, 4);
    match("\\w+", "😀", 0, 4);
    match("\\u0410", "А", 0, 2);
    match("\\U0001F600", "😀", 0, 4);
    match("\\bмир\\b", " мир!", 1, 7);
    match("世界", "а世界", 2, 8, 2);
    match("a|", "bbb", 0, 0);
    match("|a", "bbb", 0, 0);
    no_match("a{3,4}", "aa");
    no_match("[a-z]", "世界");
    no_match("abc", "ab");
    no_match("abc", "");
    match(std::string(64, '(') + "a" + std::string(64, ')'), "a", 0, 1);
    std::string alternatives = "a";
    for (unsigned i = 1; i < 1800; ++i) alternatives += "|a";
    match(alternatives, "ba", 1, 2);
    const std::string literal(1800, 'a');
    match(literal, "b" + literal, 1, 1801);
}

void regex_rejections_and_budget() {
    using namespace researchify;
    for (const auto* invalid : {"[", "[]", "[^]", "[z-a]", "[a-\\d]", "[\\w-z]", "(", ")", "*a", "a**",
                                "a+?", "a?+", "a{2,1}", "a{", "a{,2}", "a{1001}", "a{2", "a{2,3", "a}",
                                "\\", "\\1", "\\q", "(?=a)", "(?:a)", "(?i)a", "\\x0", "\\xGG", "\\uD800",
                                "\\U00110000", "^+", "\\b*"}) {
        rejects([&] { Regex regex(invalid); }, invalid);
    }
    rejects([&] { Regex regex(std::string(65, '(') + "a" + std::string(65, ')')); }, "regex nesting");
    rejects([&] { Regex regex("(abcde){1000}"); }, "NFA expansion cap");
    rejects([&] { Regex regex(std::string("\xff", 1)); }, "invalid UTF-8 pattern");
    Regex regex("(a|aa)*b");
    RegexMatch result;
    std::uint64_t budget = 100;
    rejects([&] { regex.find(std::string(200, 'a'), result, budget); }, "runtime budget");
    budget = 1000000;
    require(!regex.find(std::string(10000, 'a'), result, budget), "nonmatching exponential-backtracking trap");
    require(budget > 100000, "NFA should be linear for fixed expression");
    budget = 10000;
    rejects([&] { regex.find("я", result, budget, 1); }, "mid-codepoint offset");
    rejects([&] { regex.find("a", result, budget, 2); }, "offset out of range");
    rejects([&] { regex.find(std::string("\xff", 1), result, budget); }, "invalid UTF-8 text");
    budget = 0;
    rejects([&] { regex.find("", result, budget); }, "zero budget");
}

void differential_boolean_regex() {
    // std::regex is only an independent test oracle for small, bounded inputs.
    // Match endpoints deliberately differ because our engine chooses earliest end.
    const std::vector<std::string> patterns{
        "a", "a*", "ab+", "a?b", "(ab|ba)", "a(b|c)*d", "[a-c]{1,3}", "[^a]+b",
        "^(a|b){0,4}$", "a{2,4}", "a{0,}", "(a?)*b", "(a|aa)*b", "a.*b", ".", "^$",
        "a|b*", "(ab)?(ba)*", "[a-cx-z]*q", "a(b|c)d|b", "ab.*z|b"};
    std::mt19937 random(20261020);
    for (const auto& pattern : patterns) {
        const std::regex oracle(pattern);
        const researchify::Regex actual(pattern);
        for (unsigned trial = 0; trial < 400; ++trial) {
            std::string input;
            const auto length = random() % 14;
            for (unsigned i = 0; i < length; ++i) input.push_back("abcdqxyz"[random() % 8]);
            std::uint64_t budget = 1000000;
            researchify::RegexMatch result;
            const bool found = actual.find(input, result, budget);
            std::smatch expected;
            const bool oracle_found = std::regex_search(input, expected, oracle);
            require(found == oracle_found, "differential regex boolean: " + pattern + " / " + input);
            if (found) {
                require(result.begin == static_cast<std::size_t>(expected.position()), "differential leftmost start");
                // Brute-force the earliest accepted endpoint at the leftmost start.
                std::size_t earliest = input.size() + 1;
                for (auto end = result.begin; end <= input.size(); ++end) {
                    // Anchors must still see the entire subject, so this oracle
                    // checks only patterns without end-of-input assertions.
                    if (pattern.find('$') != std::string::npos) break;
                    if (std::regex_match(input.substr(result.begin, end - result.begin), oracle)) { earliest = end; break; }
                }
                if (earliest <= input.size()) require(result.end == earliest, "differential earliest end");
            }
        }
    }
}

void regex_prefix_and_class_fast_paths() {
    using namespace researchify;
    std::string long_unicode_prefix;
    for (unsigned i = 0; i < 31; ++i) long_unicode_prefix += "я";
    long_unicode_prefix += "😀";
    // A never-accepting alternative prevents prefix extraction. The two NFAs
    // must still return identical leftmost / earliest-end byte ranges, at every
    // valid start offset, including assertions and nullable constructions.
    const std::vector<std::string> patterns{
        "needle_[0-9]+", "^abc", "abc$", "\\babc", "\\Babc", "abc\\bdef", "ab\\Bcd",
        "a{0}abc", "(abc){2,3}", "a?bc", "a*bc", "ab(c|d)", "(ab|a)b", "a.*b",
        "abc|", "a{0}", "^$", "", "\\b世界\\b", "привет.+", "😀[a-z]+", "a$bc",
        std::string(70, 'a') + "b", long_unicode_prefix + "[0-9]+", "abc\\x00def",
        "[\\D0]", "[^\\s]", "[a-z\\W]"
    };
    const std::vector<std::string> fixed_inputs{
        "", "abc", "xabc abc!", "xxabcdef abcd", "needle_x needle_42 needle_43", "ababcd",
        "abcabcabc", "\nabc\r", " 世界!", "a世界世界", "😀abc", "xпривет мир", std::string(90, 'a') + "b",
        std::string("ab\0abc", 6), std::string("xabc\0def", 8), "x" + long_unicode_prefix + "42"
    };
    std::mt19937 random(20261023);
    for (const auto& pattern : patterns) {
        const Regex fast(pattern), reference("(" + pattern + ")|(\\b\\B)");
        RegexWorkspace fast_work, reference_work, validated_work;
        for (std::size_t trial = 0; trial < fixed_inputs.size() + 100; ++trial) {
            std::string input;
            if (trial < fixed_inputs.size()) input = fixed_inputs[trial];
            else {
                const auto length = random() % 20;
                for (unsigned i = 0; i < length; ++i) input += "abc !012_\r\n"[random() % 11];
            }
            for (std::size_t start = 0; start <= input.size(); ++start) {
                if (start < input.size() && (static_cast<unsigned char>(input[start]) & 0xc0) == 0x80) continue;
                std::uint64_t fast_budget = 10000000, reference_budget = 10000000, validated_budget = 10000000;
                RegexMatch actual, expected, validated;
                const bool actual_found = fast.find(input, actual, fast_budget, fast_work, start);
                const bool expected_found = reference.find(input, expected, reference_budget, reference_work, start);
                const bool validated_found = detail::ValidatedRegexAccess::find(
                    fast, input, validated, validated_budget, validated_work, start);
                require(actual_found == expected_found, "prefix boolean differential: " + pattern);
                require(actual_found == validated_found && fast_budget == validated_budget,
                        "validated input preserves results and work budget: " + pattern);
                if (actual_found) require(actual.begin == expected.begin && actual.end == expected.end,
                                          "prefix byte range differential: " + pattern);
                if (actual_found) require(actual.begin == validated.begin && actual.end == validated.end,
                                          "validated input preserves byte ranges: " + pattern);
            }
        }
    }
    // Independent oracle for every ASCII class member, including controls and
    // NUL. Non-ASCII is covered separately by the Unicode tests above.
    for (const auto* pattern : {"[a-z]", "[a-zA-Z0-9_]", "[^a-z]", "[\\D0]", "[\\s]", "[^\\w]", "[a-z\\W]"}) {
        const Regex actual(pattern);
        const std::regex expected(pattern);
        for (unsigned cp = 0; cp < 128; ++cp) {
            const std::string input(1, static_cast<char>(cp));
            RegexMatch result;
            std::uint64_t budget = 10000;
            require(actual.find(input, result, budget) == std::regex_search(input, expected), "ASCII class bitmap differential");
        }
    }
    Regex prefixed("needle_[0-9]+");
    RegexMatch result;
    std::uint64_t budget = 10000;
    rejects([&] { prefixed.find(std::string(32, 'x') + std::string("\xff", 1), result, budget); },
            "prefix must not bypass invalid UTF-8 without a match");
    rejects([&] { prefixed.find("needle_42" + std::string("\xff", 1), result, budget); },
            "prefix must not bypass invalid UTF-8 after a match");
    budget = 0;
    rejects([&] { prefixed.find("no match", result, budget); }, "prefix must not bypass zero budget");
    budget = 10;
    rejects([&] { prefixed.find("no match", result, budget); }, "prefix scan charges skipped bytes");
}

void regex_validated_input_budget() {
    using namespace researchify;
    // Check every budget boundary for several actual NFA executions. This also
    // covers failure after workspace reuse, nullable patterns and UTF-8 offsets.
    const std::vector<std::pair<std::string, std::string>> cases{
        {"needle_[0-9]+", "no match"}, {"needle_[0-9]+", "xneedle_42"},
        {"\\b世界\\b", " 世界!"}, {"^a.*b$", "aaab"}, {"a*", "ba"}, {"[а-я]+", " яя!"}
    };
    for (const auto& [pattern, input] : cases) {
        const Regex regex(pattern);
        RegexWorkspace strict_work, validated_work;
        for (std::size_t start = 0; start <= input.size(); ++start) {
            for (std::uint64_t limit = 0; limit < 256; ++limit) {
                std::uint64_t strict_left = limit, validated_left = limit;
                RegexMatch strict_match, validated_match;
                bool strict_found = false, validated_found = false;
                std::string strict_error, validated_error;
                try { strict_found = regex.find(input, strict_match, strict_left, strict_work, start); }
                catch (const Error& e) { strict_error = e.code() + ":" + e.what(); }
                try { validated_found = detail::ValidatedRegexAccess::find(
                    regex, input, validated_match, validated_left, validated_work, start); }
                catch (const Error& e) { validated_error = e.code() + ":" + e.what(); }
                require(strict_error == validated_error, "validated input preserves budget/offset error");
                require(strict_left == validated_left, "validated input preserves remaining work units");
                require(strict_found == validated_found, "validated input preserves budgeted result");
                if (strict_found) require(strict_match.begin == validated_match.begin && strict_match.end == validated_match.end,
                                          "validated input preserves budgeted byte range");
            }
        }
    }
    // Neither public overload can inherit a validated state from reused storage
    // or workspace. Reject invalid bytes before and after an otherwise valid hit.
    const Regex regex("needle_[0-9]+");
    RegexWorkspace workspace;
    RegexMatch match;
    std::string input = "needle_42";
    std::uint64_t budget = 10000;
    require(regex.find(input, match, budget, workspace), "strict input warm-up");
    for (const auto& invalid : {std::string("\xff", 1) + "needle_42", "needle_42" + std::string("\xff", 1),
                               std::string("no match\xc3", 9)}) {
        input = invalid;
        budget = 10000;
        rejects([&] { regex.find(input, match, budget, workspace); }, "strict reused workspace rejects invalid UTF-8");
        budget = 10000;
        rejects([&] { regex.find(input, match, budget); }, "strict public overload rejects invalid UTF-8");
    }
}

void concurrency() {
    const researchify::Regex regex("^([a-z]+|[а-я]+)_[0-9]{2}$");
    std::vector<std::thread> workers;
    std::vector<int> failures(12, 0);
    for (std::size_t i = 0; i < failures.size(); ++i) {
        workers.emplace_back([&, i] {
            try {
                for (unsigned j = 0; j < 200; ++j) {
                    std::uint64_t budget = 10000;
                    researchify::RegexMatch result;
                    if (!regex.find("номер_42", result, budget) || result.begin != 0 || result.end != 13) ++failures[i];
                }
            } catch (...) { ++failures[i]; }
        });
    }
    for (auto& thread : workers) thread.join();
    require(std::all_of(failures.begin(), failures.end(), [](int n) { return n == 0; }), "shared immutable regex concurrency");
}
}

int main() {
    const std::vector<std::pair<const char*, std::function<void()>>> tests{
        {"utf8_and_tokens", utf8_and_tokens}, {"query_parser", parser}, {"three_valued_truth_tables", truth_tables},
        {"regex_matching", regex_matching}, {"regex_errors_and_budget", regex_rejections_and_budget},
        {"regex_differential", differential_boolean_regex}, {"regex_fast_paths", regex_prefix_and_class_fast_paths},
        {"regex_validated_input_budget", regex_validated_input_budget},
        {"regex_concurrency", concurrency}};
    try {
        for (const auto& test : tests) {
            std::cout << "[RUN ] " << test.first << '\n';
            test.second();
            std::cout << "[ OK ] " << test.first << '\n';
        }
        std::cout << "query tests: " << tests.size() << " groups, " << checks << " assertions passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "[FAIL] " << e.what() << '\n';
        return 1;
    }
}
