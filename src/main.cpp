#include "researchify/api.hpp"
#include <iostream>
#include <string>
#include <vector>
namespace {
using namespace researchify;
#ifdef _MSC_VER
std::string utf8_argument(const wchar_t* input) {
    std::string output;
    auto emit = [&](std::uint32_t cp) {
        if (cp <= 0x7F) output += static_cast<char>(cp);
        else if (cp <= 0x7FF) {
            output += static_cast<char>(0xC0 | (cp >> 6));
            output += static_cast<char>(0x80 | (cp & 63));
        } else if (cp <= 0xFFFF) {
            output += static_cast<char>(0xE0 | (cp >> 12));
            output += static_cast<char>(0x80 | ((cp >> 6) & 63));
            output += static_cast<char>(0x80 | (cp & 63));
        } else {
            output += static_cast<char>(0xF0 | (cp >> 18));
            output += static_cast<char>(0x80 | ((cp >> 12) & 63));
            output += static_cast<char>(0x80 | ((cp >> 6) & 63));
            output += static_cast<char>(0x80 | (cp & 63));
        }
    };
    for (std::size_t i = 0; input[i]; ++i) {
        std::uint32_t cp = static_cast<std::uint16_t>(input[i]);
        if (cp >= 0xD800 && cp <= 0xDBFF) {
            const auto low = static_cast<std::uint16_t>(input[++i]);
            if (low < 0xDC00 || low > 0xDFFF) throw Error("Invalid UTF-16 command-line argument");
            cp = 0x10000 + ((cp - 0xD800) << 10) + low - 0xDC00;
        } else if (cp >= 0xDC00 && cp <= 0xDFFF) throw Error("Invalid UTF-16 command-line argument");
        emit(cp);
    }
    return output;
}
#endif
}
#ifdef _MSC_VER
int wmain(int argc, wchar_t* argv[]) {
    try {
        std::vector<std::string> args;
        for (int i = 0; i < argc; ++i) args.push_back(utf8_argument(argv[i]));
        return researchify::run_cli(args);
    } catch (const std::exception& e) { std::cerr << "Startup error: " << e.what() << '\n'; return 2; }
}
#else
int main(int argc, char* argv[]) {
    try { return researchify::run_cli(std::vector<std::string>(argv, argv + argc)); }
    catch (const std::exception& e) { std::cerr << "Startup error: " << e.what() << '\n'; return 2; }
}
#endif
