#include "researchify/json.hpp"
#include "researchify/engine.hpp"
#include "researchify/query.hpp"
#include <charconv>
#include <cmath>
#include <limits>

namespace researchify::json {
namespace {
[[noreturn]] void type_error(const char* type) { throw Error("E_ARGUMENT", std::string("JSON value must be ") + type);
}
void encode_utf8(std::string& s, std::uint32_t cp) {
    if (cp < 128) s += static_cast<char>(cp);
    else if (cp < 2048) { s += static_cast<char>(0xc0 | (cp >> 6));
    s += static_cast<char>(0x80 | (cp & 63));
    }
    else if (cp < 65536) { s += static_cast<char>(0xe0 | (cp >> 12));
    s += static_cast<char>(0x80 | ((cp >> 6) & 63));
    s += static_cast<char>(0x80 | (cp & 63));
    }
    else { s += static_cast<char>(0xf0 | (cp >> 18));
    s += static_cast<char>(0x80 | ((cp >> 12) & 63));
    s += static_cast<char>(0x80 | ((cp >> 6) & 63));
    s += static_cast<char>(0x80 | (cp & 63));
    }
}
class Parser {
    std::string_view text_;
    std::size_t at_ = 0, nodes_ = 0, max_depth_, max_nodes_;
    [[noreturn]] void fail(const char* message) const { throw Error("E_JSON", std::string(message) + " at byte " + std::to_string(at_));
    }
    void space() { while (at_ < text_.size() && (text_[at_] == ' ' || text_[at_] == '\t' || text_[at_] == '\r' || text_[at_] == '\n')) ++at_;
    }
    bool eat(char c) { space();
    if (at_ < text_.size() && text_[at_] == c) { ++at_;
    return true;
    } return false;
    }
    void require(char c) { if (!eat(c)) fail("Missing JSON delimiter");
    }
    std::uint32_t hex() {
        std::uint32_t cp = 0;
        for (unsigned i = 0; i < 4; ++i) {
            if (at_ == text_.size()) fail("Truncated unicode escape");
            auto c = text_[at_++];
            unsigned n;
            if (c >= '0' && c <= '9') n = static_cast<unsigned>(c - '0');
            else if (c >= 'a' && c <= 'f') n = static_cast<unsigned>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') n = static_cast<unsigned>(c - 'A' + 10);
            else fail("Invalid unicode escape");
            cp = (cp << 4) | n;
        }
        return cp;
    }
    std::string string() {
        require('"');
        std::string out;
        while (at_ < text_.size()) {
            auto c = static_cast<unsigned char>(text_[at_++]);
            if (c == '"') return out;
            if (c < 32) fail("Unescaped control in JSON string");
            if (c != '\\') { out += static_cast<char>(c);
            continue;
            }
            if (at_ == text_.size()) fail("Truncated string escape");
            switch (text_[at_++]) {
            case '"': out += '"';
            break;
            case '\\': out += '\\';
            break;
            case '/': out += '/';
            break;
            case 'b': out += '\b';
            break;
            case 'f': out += '\f';
            break;
            case 'n': out += '\n';
            break;
            case 'r': out += '\r';
            break;
            case 't': out += '\t';
            break;
            case 'u': {
                auto cp = hex();
                if (cp >= 0xd800 && cp <= 0xdbff) {
                    if (at_ + 2 > text_.size() || text_.substr(at_, 2) != "\\u") fail("Missing low surrogate");
                    at_ += 2;
                    auto low = hex();
                    if (low < 0xdc00 || low > 0xdfff) fail("Invalid low surrogate");
                    cp = 0x10000 + ((cp - 0xd800) << 10) + low - 0xdc00;
                } else if (cp >= 0xdc00 && cp <= 0xdfff) fail("Unpaired low surrogate");
                encode_utf8(out, cp);
                break;
            }
            default: fail("Unknown JSON escape");
            }
        }
        fail("Unterminated JSON string");
    }
    Value number() {
        auto start = at_;
        if (text_[at_] == '-') ++at_;
        if (at_ == text_.size()) fail("Incomplete JSON number");
        if (text_[at_] == '0') ++at_;
        else {
            if (text_[at_] < '1' || text_[at_] > '9') fail("Invalid JSON number");
            while (at_ < text_.size() && text_[at_] >= '0' && text_[at_] <= '9') ++at_;
        }
        if (at_ < text_.size() && text_[at_] == '.') {
            ++at_;
            auto digits = at_;
            while (at_ < text_.size() && text_[at_] >= '0' && text_[at_] <= '9') ++at_;
            if (at_ == digits) fail("Missing fractional digits");
        }
        if (at_ < text_.size() && (text_[at_] == 'e' || text_[at_] == 'E')) {
            ++at_;
            if (at_ < text_.size() && (text_[at_] == '+' || text_[at_] == '-')) ++at_;
            auto digits = at_;
            while (at_ < text_.size() && text_[at_] >= '0' && text_[at_] <= '9') ++at_;
            if (at_ == digits) fail("Missing exponent digits");
        }
        if (at_ - start > 128) fail("Number representation too large");
        return Number{std::string(text_.substr(start, at_ - start))};
    }
    Value value(std::size_t depth) {
        space();
        if (++nodes_ > max_nodes_ || depth > max_depth_) fail("JSON node/depth limit exceeded");
        if (at_ == text_.size()) fail("Missing JSON value");
        char c = text_[at_];
        if (c == '"') return string();
        if (c == '{') {
            ++at_;
            Value::Object out;
            if (eat('}')) return out;
            for (;;) {
                space();
                if (at_ == text_.size() || text_[at_] != '"') fail("Object key must be a string");
                auto key = string();
                require(':');
                auto v = value(depth + 1);
                if (!out.emplace(std::move(key), std::move(v)).second) fail("Duplicate object key");
                if (eat('}')) break;
                require(',');
            }
            return out;
        }
        if (c == '[') {
            ++at_;
            Value::Array out;
            if (eat(']')) return out;
            for (;;) { out.push_back(value(depth + 1));
            if (eat(']')) break;
            require(',');
            } return out;
        }
        for (const auto& [word, v] : std::vector<std::pair<std::string_view, Value>>{{"true", true}, {"false", false}, {"null", nullptr}}) {
            if (text_.substr(at_, word.size()) == word) { at_ += word.size();
            return v;
            }
        }
        if (c == '-' || (c >= '0' && c <= '9')) return number();
        fail("Unexpected JSON token");
    }
public:
    Parser(std::string_view text, std::size_t depth, std::size_t nodes) : text_(text), max_depth_(depth), max_nodes_(nodes) {}
    Value run() { auto out = value(0);
    space();
    if (at_ != text_.size()) fail("Trailing JSON data");
    return out;
    }
};
class Writer {
    std::string out_;
    std::size_t maximum_;
    void append(std::string_view s) {
        if (s.size() > maximum_ - out_.size()) throw Error("E_OUTPUT_LIMIT", "Serialized JSON exceeds byte budget");
        out_.append(s);
    }
    void quoted(const std::string& text) {
        if (!valid_utf8(text)) throw Error("E_ARGUMENT", "Cannot serialize invalid UTF-8");
        append("\"");
        static constexpr char hex[] = "0123456789abcdef";
        for (char raw : text) {
            const auto c = static_cast<unsigned char>(raw);
            if (c == '"') append("\\\"");
            else if (c == '\\') append("\\\\");
            else if (c < 32) { char escape[6] = {'\\', 'u', '0', '0', hex[c >> 4], hex[c & 15]};
            append(std::string_view(escape, 6));
            }
            else { char v = static_cast<char>(c);
            append(std::string_view(&v, 1));
            }
        }
        append("\"");
    }
    void value(const Value& v, unsigned depth) {
        if (depth > 128) throw Error("E_ARGUMENT", "Serialization depth exceeded");
        if (v.is_null()) append("null");
        else if (v.is_bool()) append(v.boolean() ? "true" : "false");
        else if (v.is_number()) {
            const auto& number = std::get<Number>(v.data()).text;
            // Number is public for exact representations; validate before writing.
            auto checked = Parser(number, 1, 1).run();
            if (!checked.is_number()) type_error("a number");
            append(number);
        } else if (v.is_string()) quoted(v.text());
        else if (v.is_array()) {
            append("[");
            bool first = true;
            for (const auto& x : v.array()) { if (!first) append(",");
            first = false;
            value(x, depth + 1);
            } append("]");
        } else {
            append("{");
            bool first = true;
            for (const auto& [key, x] : v.object()) { if (!first) append(",");
            first = false;
            quoted(key);
            append(":");
            value(x, depth + 1);
            } append("}");
        }
    }
public:
    explicit Writer(std::size_t maximum) : maximum_(maximum) {}
    std::string run(const Value& v) { value(v, 0);
    return std::move(out_);
    }
};
}
Value::Value(int v) : data_(Number{std::to_string(v)}) {}
Value::Value(std::uint64_t v) : data_(Number{std::to_string(v)}) {}
Value::Value(double v) {
    if (!std::isfinite(v)) throw Error("E_ARGUMENT", "Cannot serialize nonfinite number");
    char buffer[64];
    auto [end, error] = std::to_chars(buffer, buffer + sizeof(buffer), v, std::chars_format::general, std::numeric_limits<double>::max_digits10);
    if (error != std::errc{}) throw Error("E_ARGUMENT", "Number conversion failed");
    data_ = Number{std::string(buffer, end)};
}
bool Value::is_null() const { return std::holds_alternative<std::nullptr_t>(data_);
}
bool Value::is_bool() const { return std::holds_alternative<bool>(data_);
}
bool Value::is_number() const { return std::holds_alternative<Number>(data_);
}
bool Value::is_string() const { return std::holds_alternative<std::string>(data_);
}
bool Value::is_array() const { return std::holds_alternative<Array>(data_);
}
bool Value::is_object() const { return std::holds_alternative<Object>(data_);
}
bool Value::boolean() const { if (!is_bool()) type_error("a boolean");
return std::get<bool>(data_);
}
std::uint64_t Value::integer() const {
    if (!is_number()) type_error("an unsigned integer");
    const auto& s = std::get<Number>(data_).text;
    std::uint64_t v = 0;
    auto [end, error] = std::from_chars(s.data(), s.data() + s.size(), v);
    if (error != std::errc{} || end != s.data() + s.size() || s.empty() || s[0] == '-') type_error("an unsigned integer in decimal notation");
    return v;
}
const std::string& Value::text() const { if (!is_string()) type_error("a string");
return std::get<std::string>(data_);
}
const Value::Array& Value::array() const { if (!is_array()) type_error("an array");
return std::get<Array>(data_);
}
Value::Array& Value::array() { if (!is_array()) type_error("an array");
return std::get<Array>(data_);
}
const Value::Object& Value::object() const { if (!is_object()) type_error("an object");
return std::get<Object>(data_);
}
Value::Object& Value::object() { if (!is_object()) type_error("an object");
return std::get<Object>(data_);
}
bool Value::contains(std::string_view name) const { return is_object() && object().find(name) != object().end();
}
const Value& Value::at(std::string_view name) const { auto it = object().find(name);
if (it == object().end()) throw Error("E_ARGUMENT", "Missing JSON field: " + std::string(name));
return it->second;
}
Value& Value::operator[](const std::string& name) { if (is_null()) data_ = Object{};
return object()[name];
}
Value parse(std::string_view text, std::size_t maximum, std::size_t depth, std::size_t nodes) {
    if (text.size() > maximum) throw Error("E_JSON", "JSON input exceeds byte limit");
    if (!valid_utf8(text)) throw Error("E_JSON", "JSON input is not valid UTF-8");
    return Parser(text, depth, nodes).run();
}
std::string dump(const Value& value, std::size_t maximum) { return Writer(maximum).run(value);
}
}
