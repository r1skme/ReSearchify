#pragma once
#include "researchify/engine.hpp"
#include "researchify/json.hpp"
#include <iosfwd>
namespace researchify {
class Service {
public:
    explicit Service(fs::path fixed_index = {}, bool read_only = false);
    json::Value execute(const json::Value& request);
    void refresh();
private:
    fs::path fixed_, cached_directory_;
    bool read_only_;
    std::shared_ptr<Index> cached_;
    std::size_t cached_memory_ = 0;
    json::Value dispatch(const json::Value&);
    Index& index(const fs::path&, std::size_t memory, std::uint64_t generation);
};
json::Value command_schema(const std::string& name);
json::Value capabilities();
json::Value error_response(const std::string& code, const std::string& message, bool retryable = false,
                           const json::Value& id = nullptr);
int run_mcp(std::istream&, std::ostream&, const fs::path& fixed_index);
int run_cli(const std::vector<std::string>& args);
}
