#include "test_support.hpp"
#include <cstdlib>
#include <future>
#include <thread>
using namespace test;
namespace {
std::string quote(const fs::path& p) {
    const auto value = path_to_utf8(p);
#ifdef _WIN32
    if (value.find('"') != value.npos) throw std::runtime_error("Unsafe process test path");
    return "\"" + value + "\"";
#else
    std::string out = "'";
    for (char c : value) out += c == '\'' ? "'\\''" : std::string(1, c);
    return out + "'";
#endif
}
void wait_for(const fs::path& marker) {
    const auto stop = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (!fs::exists(marker)) {
        if (std::chrono::steady_clock::now() > stop) throw std::runtime_error("Child process synchronization timeout");
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}
int child(const fs::path& base) {
    Index snapshot(base / "index", 64);
    write(base / "ready", std::to_string(snapshot.generation()));
    wait_for(base / "release");
    REQUIRE(snapshot.search("old_identifier").total_matches == 1);
    REQUIRE(snapshot.search("new_identifier").total_matches == 0);
    REQUIRE(snapshot.read("a.cpp", 1, 5).text == "old_identifier\n");
    return 0;
}
}
int main(int argc, char** argv) {
    if (argc == 3 && std::string(argv[1]) == "--reader") {
        try { return child(path_from_utf8(argv[2])); }
        catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
    }
    const auto executable = fs::absolute(path_from_utf8(argv[0]));
    return run({{"independent_reader_survives_compaction_and_gc", [&] {
        Workspace w;
        write(w.root / "a.cpp", "old_identifier\n");
        create_index(w.root, w.index, options());
        std::string command = quote(executable) + " --reader " + quote(w.base);
#ifdef _WIN32
        command = "\"" + command + "\"";
#endif
        auto process = std::async(std::launch::async, [command] { return std::system(command.c_str()); });
        try {
            wait_for(w.base / "ready");
            write(w.root / "a.cpp", "new_identifier\n");
            update_index(w.index, options());
            compact_index(w.index, 2, 64);
            auto collection = garbage_collect(w.index, 1);
            REQUIRE(collection.pinned >= 1);
            REQUIRE(fs::exists(w.index / "commit-1.bin"));
            write(w.base / "release", "go");
            REQUIRE(process.get() == 0);
            garbage_collect(w.index, 1);
            REQUIRE(!fs::exists(w.index / "commit-1.bin"));
            REQUIRE(Index(w.index, 64).search("new_identifier").total_matches == 1);
            Index(w.index, 64).verify();
        } catch (...) {
            write(w.base / "release", "go");
            if (process.valid()) process.wait();
            throw;
        }
    }}});
}
