#pragma once
#include "researchify/engine.hpp"
#include <atomic>
#include <chrono>
#include <fstream>
#include <functional>
#include <iostream>
#include <string>
#include <vector>
namespace test {
using namespace researchify;
inline std::atomic<unsigned> sequence{0};
inline std::atomic<std::uint64_t> checks{0};
inline void require(bool result, const char* expression, const char* file, int line) {
    ++checks;
    if (!result) throw std::runtime_error(std::string(file) + ":" + std::to_string(line) + ": " + expression);
}
#define REQUIRE(...) ::test::require(bool(__VA_ARGS__), #__VA_ARGS__, __FILE__, __LINE__)
template<class F> void rejects(F&& fn, const std::string& code = "") {
    bool caught = false;
    try { fn(); } catch (const Error& e) { caught = true; if (!code.empty() && e.code() != code) throw std::runtime_error("expected " + code + ", got " + e.code() + ": " + e.what()); }
    REQUIRE(caught);
}
struct Workspace {
    fs::path base, root, index;
    Workspace() {
        for (;;) {
            const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
            base = fs::temp_directory_path() / ("cs2-test-" + std::to_string(now) + "-" + std::to_string(sequence++));
            if (fs::create_directory(base)) break;
        }
        root = base / "source"; index = base / "index"; fs::create_directory(root);
    }
    ~Workspace() { std::error_code e; fs::remove_all(base, e); }
    Workspace(const Workspace&) = delete;
    Workspace& operator=(const Workspace&) = delete;
};
inline BuildOptions options() { BuildOptions o; o.threads = 3; o.memory_mb = 64; return o; }
inline void write(const fs::path& file, const std::string& value) {
    fs::create_directories(file.parent_path());
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out.write(value.data(), static_cast<std::streamsize>(value.size())); out.close(); REQUIRE(bool(out));
}
inline std::string read(const fs::path& file) {
    std::ifstream in(file, std::ios::binary); REQUIRE(bool(in));
    return {std::istreambuf_iterator<char>(in), {}};
}
inline void flip(const fs::path& file, std::uint64_t at) {
    std::fstream stream(file, std::ios::binary | std::ios::in | std::ios::out); REQUIRE(bool(stream));
    stream.seekg(static_cast<std::streamoff>(at)); char c = 0; stream.get(c); REQUIRE(bool(stream));
    stream.seekp(static_cast<std::streamoff>(at)); stream.put(static_cast<char>(c ^ 0x55)); stream.close(); REQUIRE(bool(stream));
}
inline std::uint64_t bytes(const fs::path& dir) {
    std::uint64_t total = 0;
    for (const auto& e : fs::recursive_directory_iterator(dir)) if (e.is_regular_file()) total += e.file_size();
    return total;
}
using Case = std::pair<std::string, std::function<void()>>;
inline int run(const std::vector<Case>& tests) {
    unsigned failures = 0;
    for (const auto& [name, fn] : tests) {
        try { fn(); std::cout << "PASS " << name << '\n'; }
        catch (const std::exception& e) { ++failures; std::cerr << "FAIL " << name << ": " << e.what() << '\n'; }
    }
    std::cout << (tests.size() - failures) << '/' << tests.size() << " cases; " << checks << " assertions\n";
    return failures ? 1 : 0;
}
}
