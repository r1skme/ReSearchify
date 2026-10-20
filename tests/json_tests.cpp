#include "test_support.hpp"
#include "researchify/json.hpp"
#include <cmath>
#include <limits>
#include <random>
using namespace test;
using namespace researchify::json;
int main() {
    return run({
        {"roundtrip_all_types_and_exact_integers", [] {
            auto value = parse(R"({"null":null,"yes":true,"no":false,"big":18446744073709551615,"a":[-1,1.5,1e30,"text"]})");
            REQUIRE(value.at("null").is_null());
            REQUIRE(value.at("yes").boolean());
            REQUIRE(!value.at("no").boolean());
            REQUIRE(value.at("big").integer() == (std::numeric_limits<std::uint64_t>::max)());
            REQUIRE(dump(parse(dump(value))) == dump(value));
            rejects([&] { value.at("a").array()[0].integer(); }, "E_ARGUMENT");
            rejects([] { parse("18446744073709551616").integer(); }, "E_ARGUMENT");
            rejects([] { parse("1.0").integer(); }, "E_ARGUMENT");
            rejects([] { parse("1e2").integer(); }, "E_ARGUMENT");
        }},
        {"unicode_escape_surrogates_and_controls", [] {
            auto s = parse(R"("\u041f\u0440\u0438\u0432\u0435\u0442 \ud83d\ude00\u0000\n\t")").text();
            REQUIRE(s.substr(0, 12) == "Привет");
            REQUIRE(s.find('\0') != s.npos);
            REQUIRE(parse(dump(Value(s))).text() == s);
            for (const auto* bad : {R"("\ud800")", R"("\udfff")", R"("\ud800\u0041")", R"("\uZZZZ")", R"("\x22")", R"("\")"})
                rejects([&] { parse(bad); }, "E_JSON");
            rejects([] { parse(std::string("\"a\n\"")); }, "E_JSON");
            rejects([] { parse(std::string("\"\xC0\xAF\"", 4)); }, "E_JSON");
        }},
        {"strict_grammar_and_duplicate_keys", [] {
            for (const auto* bad : {"", " ", "00", "+1", "01", "-", "1.", ".1", "1e", "1e+", "NaN", "Infinity", "true false", "[1,]", "{\"x\":1,}", "{x:1}", "[", "{", "nul", "{\"x\":1,\"x\":2}", "{\"x\":1,\"\\u0078\":2}"})
                rejects([&] { parse(bad); }, "E_JSON");
            REQUIRE(parse("\r\n[ 1, true, null ] \t").array().size() == 3);
        }},
        {"bounded_depth_nodes_input_and_output", [] {
            rejects([] { parse("[[[[]]]]", 100, 2); }, "E_JSON");
            rejects([] { parse("[1,2,3,4]", 100, 64, 3); }, "E_JSON");
            rejects([] { parse("\"abcd\"", 4); }, "E_JSON");
            rejects([] { dump(Value(std::string(100, '\0')), 110); }, "E_OUTPUT_LIMIT");
            REQUIRE(dump(Value("a"), 3) == "\"a\"");
            rejects([] { dump(Value("a"), 2); }, "E_OUTPUT_LIMIT");
        }},
        {"numbers_finite_and_forged_repr", [] {
            rejects([] { Value x((std::numeric_limits<double>::infinity)()); }, "E_ARGUMENT");
            rejects([] { dump(Value(Number{"1,null"})); });
            rejects([] { dump(Value(Number{"NaN"})); });
            auto v = Value(1.23456789012345);
            REQUIRE(dump(parse(dump(v))) == dump(v));
        }},
        {"random_utf8_strings_roundtrip", [] {
            std::mt19937 rng(45898);
            const std::vector<std::string> atoms{"a", "\"", "\\", "Привет", "😀", "\n", "\r", "\t", std::string(1, '\0'), "é", "é", "中文", "[]{}:"};
            for (unsigned k = 0; k != 2000; ++k) {
                std::string s;
                for (unsigned j = 0, n = static_cast<unsigned>(rng() % 80); j < n; ++j) s += atoms[rng() % atoms.size()];
                auto encoded = dump(Value(s));
                REQUIRE(parse(encoded).text() == s);
            }
        }},
        {"random_byte_parser_never_crashes", [] {
            std::mt19937 rng(7589);
            for (unsigned k = 0; k != 5000; ++k) {
                std::string s;
                for (unsigned j = 0, n = static_cast<unsigned>(rng() % 200); j < n; ++j) s += static_cast<char>(rng() % 256);
                try { auto v = parse(s);
                REQUIRE(dump(parse(dump(v))) == dump(v));
                } catch (const Error&) {}
            }
            REQUIRE(true);
        }}
    });
}
