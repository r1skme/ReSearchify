#include "researchify/literal_filter.hpp"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

using researchify::LiteralFilter;

namespace {
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

void check_substrings(const LiteralFilter& filter, std::string_view text) {
    const auto restored = LiteralFilter::decode(filter.encode());
    require(restored.has_value(), "Filter encoding does not decode");
    require(restored->encode() == filter.encode(), "Filter encoding not stable");
    for (std::size_t start = 0; start <= text.size(); ++start) {
        const auto maximum = (std::min)(std::size_t{24}, text.size() - start);
        for (std::size_t length = 0; length <= maximum; ++length) {
            const auto needle = text.substr(start, length);
            const auto query = LiteralFilter::required(needle);
            require(filter.may_contain(needle), "Real substring rejected");
            require(filter.maybe(query), "Real substring query mask rejected");
            require(restored->may_contain(needle), "Restored filter rejects substring");
            require(restored->maybe(query), "Restored filter rejects query mask");
        }
    }
}
}

int main() {
    try {
        LiteralFilter empty;
        require(empty.may_contain("") && empty.may_contain("a") && empty.may_contain("abc"),
                "Short literals must always pass");
        require(!empty.may_contain("needle_42 = 0;"), "Empty document should reject a long literal");
        require(!LiteralFilter::decode("").has_value(), "Truncated filter accepted");
        require(!LiteralFilter::decode(std::string(LiteralFilter::Bytes + 1, '\0')).has_value(), "Trailing bytes accepted");

        const std::string source = "int xneedle_42 = 0; return common;";
        LiteralFilter basic;
        basic.add_line(source);
        require(basic.may_contain("needle_42 = 0;"), "Substring inside token rejected");
        check_substrings(basic, source);

        const std::string unicode = "a\xc3\xa9" "b\xd0\x9f\xd1\x80\xd0\xb8\xd0\xb2\xd0\xb5\xd1\x82" "\xf0\x9f\x98\x80" "needle";
        LiteralFilter multibyte;
        multibyte.add_line(unicode);
        check_substrings(multibyte, unicode);

        LiteralFilter multiline;
        multiline.add_line("line_one");
        multiline.add_line("needle_42 = 0;");
        multiline.add_line("line_three");
        check_substrings(multiline, "line_one");
        check_substrings(multiline, "needle_42 = 0;");
        check_substrings(multiline, "line_three");

        std::string long_token(9000, 'x');
        long_token += "needle_42 = 0;";
        LiteralFilter long_filter;
        long_filter.add_line(long_token);
        require(long_filter.may_contain("xneedle_42 = 0;"), "Token exceeding query token limit rejected");
        check_substrings(long_filter, std::string_view(long_token).substr(8980));

        LiteralFilter limited;
        limited.add_line(std::string(LiteralFilter::MaximumBuildBytes + 1, 'a'));
        require(limited.may_contain("anything must pass saturated filter"), "Build limit did not safely saturate");
        LiteralFilter incremental_limit;
        incremental_limit.add_line(std::string(LiteralFilter::MaximumBuildBytes, 'a'));
        incremental_limit.add_line("x");
        require(incremental_limit.may_contain("anything must pass saturated filter"), "Incremental build limit unsafe");

        std::uint64_t state = 0x4d595df4d0f33173ULL;
        const auto next = [&] {
            state = state * 6364136223846793005ULL + 1442695040888963407ULL;
            return state;
        };
        std::string dense_line(4096, '\0');
        for (auto& byte : dense_line) byte = static_cast<char>(next() >> 56);
        LiteralFilter dense;
        dense.add_line(dense_line);
        require(dense.encode() == std::string(LiteralFilter::Bytes, static_cast<char>(255)),
                "Dense filter did not conservatively saturate");
        require(dense.may_contain("arbitrary absent pattern"), "Density fallback rejected a possible match");
        // Query masks must remain exact even when the document builder would
        // choose its conservative all-ones saturation shortcut.
        std::string sparse_long(8192, 'a');
        for (std::size_t i = 0; i < 1700; ++i) sparse_long[i] = static_cast<char>(next() >> 56);
        const auto required_long = LiteralFilter::required(sparse_long);
        require(required_long.encode() != std::string(LiteralFilter::Bytes, static_cast<char>(255)),
                "Query mask incorrectly uses document saturation");
        const auto required_restored = LiteralFilter::decode(required_long.encode());
        require(required_restored.has_value(), "Query mask failed serialization");
        for (const std::size_t prefix : {0U, 1U, 1000U, 1023U, 1024U, 1025U, 12000U}) {
            LiteralFilter document;
            document.add_line(std::string(prefix, 'x'));
            document.add_line("prefix" + sparse_long + "suffix");
            require(document.maybe(required_long), "Long query lost due to different density schedule");
            require(document.maybe(*required_restored), "Restored long query mask lost a match");
            require(document.may_contain(sparse_long), "Direct long query lost a match");
        }
        const std::string longer_than_budget(LiteralFilter::MaximumBuildBytes + 1, 'a');
        require(LiteralFilter::required(longer_than_budget).encode() == LiteralFilter::required("aaaa").encode(),
                "Query mask incorrectly uses document work budget");
        for (std::size_t trial = 0; trial < 100; ++trial) {
            const auto size = static_cast<std::size_t>(next() % 500);
            std::string line(size, '\0');
            for (auto& byte : line) byte = static_cast<char>(next() >> 56);
            LiteralFilter filter;
            filter.add_line(line);
            check_substrings(filter, line);
        }

        // A saturated decoded filter retains conservative behavior even if a
        // caller accidentally tries to resume construction from persisted bytes.
        auto decoded = LiteralFilter::decode(basic.encode());
        decoded->add_line("a new line after decoding");
        require(decoded->may_contain(source), "Resuming a decoded filter dropped old grams");
        require(decoded->may_contain("a new line after decoding"), "Resuming decoded filter dropped new grams");
        std::cout << "Literal filter: normalization-independent substring, UTF-8, long-token, budget, serialization and randomized tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Literal filter test failed: " << error.what() << '\n';
        return 1;
    }
}
