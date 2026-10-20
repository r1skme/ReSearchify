#pragma once
#include <utility>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace researchify::json {
struct Number { std::string text; };
class Value {
public:
    using Array = std::vector<Value>;
    using Object = std::map<std::string, Value, std::less<>>;
    using Data = std::variant<std::nullptr_t, bool, Number, std::string, Array, Object>;
    Value() : data_(nullptr) {}
    Value(std::nullptr_t) : data_(nullptr) {}
    Value(bool value) : data_(value) {}
    Value(int value);
    Value(std::uint64_t value);
    Value(double value);
    Value(Number value) : data_(std::move(value)) {}
    Value(const char* value) : data_(std::string(value)) {}
    Value(std::string value) : data_(std::move(value)) {}
    Value(Array value) : data_(std::move(value)) {}
    Value(Object value) : data_(std::move(value)) {}
    bool is_null() const;
    bool is_bool() const;
    bool is_number() const;
    bool is_string() const;
    bool is_array() const;
    bool is_object() const;
    bool boolean() const;
    std::uint64_t integer() const;
    const std::string& text() const;
    const Array& array() const;
    Array& array();
    const Object& object() const;
    Object& object();
    bool contains(std::string_view name) const;
    const Value& at(std::string_view name) const;
    Value& operator[](const std::string& name);
    const Data& data() const { return data_; }
private:
    Data data_;
};
Value parse(std::string_view text, std::size_t max_bytes = 1048576,
            std::size_t max_depth = 64, std::size_t max_nodes = 100000);
std::string dump(const Value& value, std::size_t max_bytes = 16777216);
}
