// Build this source against before/after researchify_core libraries. Compare
// the complete stdout files byte for byte outside performance measurements.
// The probe exposes no optimization toggle and exercises the public API plus
// the internal already-validated path with reused and wrapping workspaces.
#include "researchify/query.hpp"
#include "researchify/engine.hpp"
#include "../src/query_internal.hpp"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <limits>
#include <random>
#include <string>
#include <vector>

int main() {
    using namespace researchify;
    const std::vector<std::string> patterns{
        "(a|b)", "(return|if|while)[ \\t]", "(ab|a)b", "(a|aa)*b", "([ab]?)*c", "(a?|b?)*c",
        "(a|b)*", "a?", "a*bc", "a.*b", "(^a|b)", "(\\babc|def)", "(a|b\\b)", "a$bc",
        "[ab]+c", "[^ab]*c", "(😀|π|a)b", "[\\D0]", "[^\\w]", "^$", "", "\\Babc",
        "abc|", "a{0}abc", "(ab|ba){1,3}", "a{0,3}b", "[а-я]+", "a\\x00b"
    };
    std::vector<std::string> inputs{
        "", "xxxx", "ababaac", "xxreturn x; if\t(x) while (x)", "xxabcdef abcd", "abc abc!",
        "\nabc\r", " 世界!", "😀ab", "xπb", "ааб", " ", std::string("ab\0abc", 6),
        std::string("ab\xff", 3), std::string("x\xc3", 2)
    };
    std::mt19937 random(20261023);
    for (unsigned trial = 0; trial < 24; ++trial) {
        std::string input;
        const auto length = random() % 18;
        for (unsigned i = 0; i < length; ++i) input += "abcdqxyz !012_\r\n"[random() % 16];
        if (trial % 4 == 0) input += "π😀";
        inputs.push_back(std::move(input));
    }
    std::uint64_t calls = 0;
    for (std::size_t pi = 0; pi < patterns.size(); ++pi) {
        const Regex regex(patterns[pi]);
        RegexWorkspace work[2];
        for (std::size_t ti = 0; ti < inputs.size(); ++ti) {
            const auto& input = inputs[ti];
            const bool validated = valid_utf8(input);
            for (std::size_t start = 0; start <= input.size() + 1; ++start) {
                std::vector<std::uint64_t> limits;
                for (std::uint64_t limit = 0; limit <= (ti < 4 ? 128 : 16); ++limit) limits.push_back(limit);
                for (auto limit : {31ULL, 32ULL, 63ULL, 64ULL, 127ULL, 128ULL, 255ULL, 256ULL, 1024ULL, 1000000ULL})
                    limits.push_back(limit);
                std::uint64_t remaining = 1000000;
                RegexMatch complete;
                try {
                    regex.find(input, complete, remaining, start);
                    const auto cost = 1000000 - remaining;
                    if (cost) limits.push_back(cost - 1);
                    limits.push_back(cost); limits.push_back(cost + 1);
                } catch (const Error&) {}
                std::sort(limits.begin(), limits.end());
                limits.erase(std::unique(limits.begin(), limits.end()), limits.end());
                for (const auto limit : limits) {
                    for (unsigned mode = 0; mode < (validated ? 2U : 1U); ++mode) {
                        auto budget = limit;
                        RegexMatch match{777, 888};
                        bool found = false;
                        std::string error;
                        if (calls++ % 31 == 0) work[mode].epoch = (std::numeric_limits<std::size_t>::max)();
                        try {
                            found = mode ? detail::ValidatedRegexAccess::find(regex, input, match, budget, work[mode], start)
                                         : regex.find(input, match, budget, work[mode], start);
                        } catch (const Error& e) { error = e.code() + ":" + e.what(); }
                        std::cout << pi << '\t' << ti << '\t' << start << '\t' << limit << '\t' << mode << '\t'
                                  << found << '\t' << match.begin << '\t' << match.end << '\t' << budget << '\t' << error << '\n';
                    }
                }
            }
        }
    }
}
