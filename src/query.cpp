#include "researchify/query.hpp"
#include "researchify/engine.hpp"

#include <algorithm>
#include <array>
#include <iterator>
#include <optional>
#include <limits>
#include <utility>

namespace researchify {
namespace {
constexpr std::size_t max_expression_bytes = 8192;
constexpr std::size_t max_nesting = 64;
constexpr std::size_t max_leaves = 128;
constexpr std::size_t max_regex_states = 4096;
constexpr std::size_t max_query_regex_states = 32768;
constexpr std::uint32_t max_repeat = 1000;
constexpr std::size_t absent = std::numeric_limits<std::size_t>::max();

bool decode(std::string_view text, std::size_t& offset, char32_t& value) {
    if (offset >= text.size()) return false;
    const auto b0 = static_cast<unsigned char>(text[offset]);
    if (b0 < 0x80) { value = b0; ++offset; return true; }
    std::size_t length = 0;
    char32_t lower = 0;
    if (b0 >= 0xc2 && b0 <= 0xdf) { length = 2; value = b0 & 0x1f; lower = 0x80; }
    else if (b0 >= 0xe0 && b0 <= 0xef) { length = 3; value = b0 & 0x0f; lower = 0x800; }
    else if (b0 >= 0xf0 && b0 <= 0xf4) { length = 4; value = b0 & 0x07; lower = 0x10000; }
    else return false;
    if (length > text.size() - offset) return false;
    for (std::size_t i = 1; i < length; ++i) {
        const auto b = static_cast<unsigned char>(text[offset + i]);
        if ((b & 0xc0) != 0x80) return false;
        value = static_cast<char32_t>((value << 6) | (b & 0x3f));
    }
    if (value < lower || value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff)) return false;
    offset += length;
    return true;
}

bool word(char32_t cp) {
    return (cp >= U'a' && cp <= U'z') || (cp >= U'A' && cp <= U'Z') ||
           (cp >= U'0' && cp <= U'9') || cp == U'_' || cp >= 0x80;
}
bool whitespace(char32_t cp) {
    return cp == U' ' || cp == U'\t' || cp == U'\n' || cp == U'\r' || cp == U'\v' || cp == U'\f';
}
bool digit(char32_t cp) { return cp >= U'0' && cp <= U'9'; }

enum class AstKind { Empty, Literal, Any, Class, Concat, Alternate, Repeat, Bol, Eol, Boundary, NotBoundary };
struct CharClass {
    std::vector<std::pair<char32_t, char32_t>> ranges;
    // Each category bit means a union member; uppercase escapes use inverse bits.
    unsigned categories = 0;
    bool negative = false;
    bool matches(char32_t cp) const {
        bool found = false;
        if ((categories & 1U) != 0 && digit(cp)) found = true;
        if ((categories & 2U) != 0 && !digit(cp)) found = true;
        if ((categories & 4U) != 0 && word(cp)) found = true;
        if ((categories & 8U) != 0 && !word(cp)) found = true;
        if ((categories & 16U) != 0 && whitespace(cp)) found = true;
        if ((categories & 32U) != 0 && !whitespace(cp)) found = true;
        // Ranges are normalized, sorted, disjoint. Binary search avoids work
        // proportional to a potentially long character-class definition.
        const auto it = std::upper_bound(ranges.begin(), ranges.end(), cp,
            [](char32_t c, const auto& range) { return c < range.first; });
        if (it != ranges.begin()) {
            const auto& range = *std::prev(it);
            if (cp <= range.second) found = true;
        }
        return negative ? !found : found;
    }
    void normalize() {
        std::sort(ranges.begin(), ranges.end());
        std::size_t used = 0;
        for (const auto& range : ranges) {
            if (used != 0 && range.first <= ranges[used - 1].second + 1) {
                ranges[used - 1].second = std::max(ranges[used - 1].second, range.second);
            } else {
                ranges[used++] = range;
            }
        }
        ranges.resize(used);
    }
};
struct Ast {
    AstKind kind = AstKind::Empty;
    char32_t literal = 0;
    std::size_t class_id = 0;
    std::size_t left = absent, right = absent;
    std::uint32_t minimum = 0, maximum = 0;
    bool unlimited = false;
};

class RegexParser {
public:
    explicit RegexParser(std::string_view pattern) : text_(pattern) {
        if (text_.size() > max_expression_bytes) fail("pattern is longer than 8192 bytes");
        if (!valid_utf8(text_)) fail("pattern is not valid UTF-8");
    }
    std::size_t parse() {
        const auto root = alternate(0);
        if (pos_ != text_.size()) fail("unexpected closing parenthesis");
        return root;
    }
    std::vector<Ast> nodes;
    std::vector<CharClass> classes;
private:
    std::string_view text_;
    std::size_t pos_ = 0;
    [[noreturn]] void fail(const std::string& message) const {
        throw Error("regex at byte " + std::to_string(pos_ + 1) + ": " + message);
    }
    bool at(char c) const { return pos_ < text_.size() && text_[pos_] == c; }
    std::size_t add(Ast node) {
        if (nodes.size() >= max_regex_states) fail("expression is too complex");
        nodes.push_back(std::move(node));
        return nodes.size() - 1;
    }
    std::size_t binary(AstKind kind, std::size_t left, std::size_t right) {
        Ast n; n.kind = kind; n.left = left; n.right = right; return add(n);
    }
    std::size_t alternate(std::size_t depth) {
        std::vector<std::size_t> choices{concatenate(depth)};
        while (at('|')) { ++pos_; choices.push_back(concatenate(depth)); }
        return join(choices, 0, choices.size(), AstKind::Alternate);
    }
    std::size_t concatenate(std::size_t depth) {
        std::vector<std::size_t> parts;
        while (pos_ < text_.size() && !at(')') && !at('|')) parts.push_back(repeat(depth));
        if (parts.empty()) return add(Ast{});
        // A balanced tree keeps compilation stack depth bounded for long patterns.
        return join(parts, 0, parts.size(), AstKind::Concat);
    }
    std::size_t join(const std::vector<std::size_t>& parts, std::size_t begin, std::size_t end, AstKind kind) {
        if (end - begin == 1) return parts[begin];
        const auto middle = begin + (end - begin) / 2;
        return binary(kind, join(parts, begin, middle, kind), join(parts, middle, end, kind));
    }
    std::uint32_t number() {
        if (pos_ == text_.size() || text_[pos_] < '0' || text_[pos_] > '9') fail("expected repetition count");
        std::uint32_t value = 0;
        while (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9') {
            value = value * 10U + static_cast<unsigned>(text_[pos_++] - '0');
            if (value > max_repeat) fail("repetition count exceeds 1000");
        }
        return value;
    }
    std::size_t repeat(std::size_t depth) {
        const auto child = atom(depth);
        if (!(at('*') || at('+') || at('?') || at('{'))) return child;
        const auto child_kind = nodes[child].kind;
        if (child_kind == AstKind::Bol || child_kind == AstKind::Eol ||
            child_kind == AstKind::Boundary || child_kind == AstKind::NotBoundary) fail("assertion cannot be repeated");
        Ast node; node.kind = AstKind::Repeat; node.left = child;
        if (at('*')) { ++pos_; node.unlimited = true; }
        else if (at('+')) { ++pos_; node.minimum = 1; node.unlimited = true; }
        else if (at('?')) { ++pos_; node.maximum = 1; }
        else {
            ++pos_;
            node.minimum = number();
            node.maximum = node.minimum;
            if (at(',')) {
                ++pos_;
                if (at('}')) node.unlimited = true;
                else node.maximum = number();
            }
            if (!at('}')) fail("expected closing brace");
            ++pos_;
            if (!node.unlimited && node.maximum < node.minimum) fail("repetition maximum is smaller than minimum");
        }
        if (at('*') || at('+') || at('?') || at('{')) fail("nested, lazy and possessive quantifiers are unsupported");
        return add(node);
    }
    char32_t scalar() {
        char32_t cp = 0;
        if (!decode(text_, pos_, cp)) fail("invalid UTF-8 scalar");
        return cp;
    }
    unsigned category(char32_t cp) const {
        switch (cp) {
        case U'd': return 1;
        case U'D': return 2;
        case U'w': return 4;
        case U'W': return 8;
        case U's': return 16;
        case U'S': return 32;
        default: return 0;
        }
    }
    char32_t hex_escape(unsigned length) {
        char32_t value = 0;
        for (unsigned i = 0; i < length; ++i) {
            if (pos_ == text_.size()) fail("truncated hexadecimal escape");
            const char c = text_[pos_++];
            unsigned nibble = 0;
            if (c >= '0' && c <= '9') nibble = static_cast<unsigned>(c - '0');
            else if (c >= 'a' && c <= 'f') nibble = static_cast<unsigned>(c - 'a') + 10U;
            else if (c >= 'A' && c <= 'F') nibble = static_cast<unsigned>(c - 'A') + 10U;
            else fail("invalid hexadecimal escape");
            value = static_cast<char32_t>(value * 16U + nibble);
        }
        if (value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff)) fail("escape is not a Unicode scalar");
        return value;
    }
    char32_t escaped_literal(char32_t cp, bool in_class) {
        switch (cp) {
        case U'n': return U'\n';
        case U'r': return U'\r';
        case U't': return U'\t';
        case U'f': return U'\f';
        case U'v': return U'\v';
        case U'b': if (in_class) return U'\b'; break;
        case U'x': return hex_escape(2);
        case U'u': return hex_escape(4);
        case U'U': return hex_escape(8);
        default:
            if ((cp >= U'A' && cp <= U'Z') || (cp >= U'a' && cp <= U'z') || digit(cp))
                fail("unsupported escape or backreference");
            return cp;
        }
        fail("unsupported escape");
    }
    struct ClassItem { bool is_category = false; unsigned bits = 0; char32_t cp = 0; };
    ClassItem class_item() {
        const auto cp = scalar();
        if (cp != U'\\') return {false, 0, cp};
        if (pos_ == text_.size()) fail("trailing backslash");
        const auto escaped = scalar();
        const auto bits = category(escaped);
        if (bits != 0) return {true, bits, 0};
        return {false, 0, escaped_literal(escaped, true)};
    }
    std::size_t character_class() {
        ++pos_;
        CharClass value;
        if (at('^')) { value.negative = true; ++pos_; }
        bool any = false;
        while (pos_ < text_.size() && !at(']')) {
            const auto first = class_item();
            any = true;
            if (at('-') && pos_ + 1 < text_.size() && text_[pos_ + 1] != ']') {
                ++pos_;
                const auto last = class_item();
                if (first.is_category || last.is_category) fail("character-class range requires literal endpoints");
                if (first.cp > last.cp) fail("reversed character-class range");
                value.ranges.emplace_back(first.cp, last.cp);
            } else if (first.is_category) value.categories |= first.bits;
            else value.ranges.emplace_back(first.cp, first.cp);
        }
        if (!at(']')) fail("unclosed character class");
        ++pos_;
        if (!any) fail("empty character class");
        value.normalize();
        Ast node; node.kind = AstKind::Class; node.class_id = classes.size();
        classes.push_back(std::move(value));
        return add(node);
    }
    std::size_t atom(std::size_t depth) {
        if (at('(')) {
            if (depth >= max_nesting) fail("group nesting exceeds 64");
            ++pos_;
            if (at('?')) fail("lookaround, flags and special groups are unsupported");
            const auto result = alternate(depth + 1);
            if (!at(')')) fail("unclosed group");
            ++pos_;
            return result;
        }
        if (at('[')) return character_class();
        Ast node;
        const auto cp = scalar();
        switch (cp) {
        case U'.': node.kind = AstKind::Any; break;
        case U'^': node.kind = AstKind::Bol; break;
        case U'$': node.kind = AstKind::Eol; break;
        case U'*': case U'+': case U'?': case U'{': case U'}': case U']':
            fail("unescaped metacharacter or quantifier without an operand");
        case U'\\': {
            if (pos_ == text_.size()) fail("trailing backslash");
            const auto escaped = scalar();
            const auto bits = category(escaped);
            if (bits != 0) {
                CharClass value; value.categories = bits;
                node.kind = AstKind::Class; node.class_id = classes.size();
                classes.push_back(std::move(value));
            } else if (escaped == U'b') node.kind = AstKind::Boundary;
            else if (escaped == U'B') node.kind = AstKind::NotBoundary;
            else { node.kind = AstKind::Literal; node.literal = escaped_literal(escaped, false); }
            break;
        }
        default: node.kind = AstKind::Literal; node.literal = cp; break;
        }
        return add(node);
    }
};

enum class Op { Literal, Any, Class, Split, Epsilon, Bol, Eol, Boundary, NotBoundary, Accept };
struct State {
    Op op = Op::Epsilon;
    char32_t literal = 0;
    std::size_t class_id = 0;
    std::size_t next = absent, alternate = absent;
};
struct Patch { std::size_t state; bool alternate; };
struct Fragment { std::size_t start; std::vector<Patch> exits; };

class Compiler {
public:
    explicit Compiler(const std::vector<Ast>& nodes) : nodes_(nodes) {}
    std::vector<State> states;
    std::size_t compile(std::size_t root) {
        auto fragment = emit(root);
        State accept; accept.op = Op::Accept;
        const auto end = append(accept);
        patch(fragment.exits, end);
        return fragment.start;
    }
private:
    const std::vector<Ast>& nodes_;
    std::size_t append(State state) {
        if (states.size() >= max_regex_states) throw Error("regex expands beyond 4096 NFA states");
        states.push_back(state);
        return states.size() - 1;
    }
    void patch(const std::vector<Patch>& exits, std::size_t target) {
        for (const auto& exit : exits) {
            if (exit.alternate) states[exit.state].alternate = target;
            else states[exit.state].next = target;
        }
    }
    Fragment connect(Fragment left, Fragment right) {
        patch(left.exits, right.start);
        return {left.start, std::move(right.exits)};
    }
    Fragment empty() { const auto i = append(State{}); return {i, {{i, false}}}; }
    Fragment emit(std::size_t node_id) {
        const auto& node = nodes_[node_id];
        if (node.kind == AstKind::Concat) {
            auto left = emit(node.left);
            auto right = emit(node.right);
            return connect(std::move(left), std::move(right));
        }
        if (node.kind == AstKind::Alternate) {
            auto left = emit(node.left);
            auto right = emit(node.right);
            State split; split.op = Op::Split; split.next = left.start; split.alternate = right.start;
            const auto begin = append(split);
            left.exits.insert(left.exits.end(), right.exits.begin(), right.exits.end());
            return {begin, std::move(left.exits)};
        }
        if (node.kind == AstKind::Repeat) {
            auto result = empty();
            for (std::uint32_t i = 0; i < node.minimum; ++i) result = connect(std::move(result), emit(node.left));
            if (node.unlimited) {
                auto body = emit(node.left);
                State split; split.op = Op::Split; split.next = body.start;
                const auto loop = append(split);
                patch(body.exits, loop);
                result = connect(std::move(result), Fragment{loop, {{loop, true}}});
            } else {
                for (std::uint32_t i = node.minimum; i < node.maximum; ++i) {
                    auto body = emit(node.left);
                    State split; split.op = Op::Split; split.next = body.start;
                    const auto begin = append(split);
                    body.exits.push_back({begin, true});
                    result = connect(std::move(result), Fragment{begin, std::move(body.exits)});
                }
            }
            return result;
        }
        State state;
        state.literal = node.literal;
        state.class_id = node.class_id;
        switch (node.kind) {
        case AstKind::Empty: state.op = Op::Epsilon; break;
        case AstKind::Literal: state.op = Op::Literal; break;
        case AstKind::Any: state.op = Op::Any; break;
        case AstKind::Class: state.op = Op::Class; break;
        case AstKind::Bol: state.op = Op::Bol; break;
        case AstKind::Eol: state.op = Op::Eol; break;
        case AstKind::Boundary: state.op = Op::Boundary; break;
        case AstKind::NotBoundary: state.op = Op::NotBoundary; break;
        default: throw Error("internal regex compiler error");
        }
        const auto id = append(state);
        return {id, {{id, false}}};
    }
};

void charge(std::uint64_t& remaining, std::uint64_t amount = 1) {
    if (remaining < amount) throw Error("E_BUDGET", "regex execution budget exhausted");
    remaining -= amount;
}

enum class LexKind { End, Term, Phrase, Regex, And, Or, Not, Open, Close };
struct Lexeme { LexKind kind = LexKind::End; std::string text; std::size_t offset = 0; };
class QueryParser {
public:
    explicit QueryParser(std::string_view text) : input_(text) {
        if (input_.size() > max_expression_bytes) throw Error("query is longer than 8192 bytes");
        if (!valid_utf8(input_)) throw Error("query is not valid UTF-8");
        advance();
    }
    Query parse() {
        if (current_.kind == LexKind::End) fail("query is empty");
        result_.root = parse_or(0);
        if (current_.kind != LexKind::End) fail("unexpected token");
        return std::move(result_);
    }
private:
    std::string_view input_;
    std::size_t pos_ = 0;
    Lexeme current_;
    Query result_;
    std::size_t compiled_states_ = 0;
    [[noreturn]] void fail(const std::string& message) const {
        throw Error("query at byte " + std::to_string(current_.offset + 1) + ": " + message);
    }
    void advance() {
        while (pos_ < input_.size() && whitespace(static_cast<unsigned char>(input_[pos_]))) ++pos_;
        current_ = {}; current_.offset = pos_;
        if (pos_ == input_.size()) return;
        const char first = input_[pos_++];
        if (first == '(') { current_.kind = LexKind::Open; return; }
        if (first == ')') { current_.kind = LexKind::Close; return; }
        if (first == '"' || first == '/') {
            current_.kind = first == '"' ? LexKind::Phrase : LexKind::Regex;
            bool closed = false;
            while (pos_ < input_.size()) {
                const char c = input_[pos_++];
                if (c == first) { closed = true; break; }
                if (c != '\\') { current_.text.push_back(c); continue; }
                if (pos_ == input_.size()) fail("trailing backslash in quoted expression");
                const char next = input_[pos_++];
                if (first == '/') { current_.text.push_back('\\'); current_.text.push_back(next); }
                else {
                    switch (next) {
                    case '\\': current_.text.push_back('\\'); break;
                    case '"': current_.text.push_back('"'); break;
                    case 't': current_.text.push_back('\t'); break;
                    case 'n': current_.text.push_back('\n'); break;
                    case 'r': current_.text.push_back('\r'); break;
                    default: fail("unsupported phrase escape; use \\\\ for a literal backslash");
                    }
                }
            }
            if (!closed) fail("unclosed quoted expression");
            if (first == '"') {
                if (current_.text.empty()) fail("exact phrase must not be empty");
                if (current_.text.find_first_of("\r\n") != std::string::npos) fail("exact phrases must fit on one line");
            }
            return;
        }
        --pos_;
        const auto begin = pos_;
        while (pos_ < input_.size()) {
            const char c = input_[pos_];
            if (whitespace(static_cast<unsigned char>(c)) || c == '(' || c == ')' || c == '"' || c == '/') break;
            ++pos_;
        }
        current_.text = std::string(input_.substr(begin, pos_ - begin));
        if (current_.text == "AND") current_.kind = LexKind::And;
        else if (current_.text == "OR") current_.kind = LexKind::Or;
        else if (current_.text == "NOT") current_.kind = LexKind::Not;
        else {
            current_.kind = LexKind::Term;
            std::size_t p = 0;
            while (p < current_.text.size()) {
                char32_t cp = 0;
                if (!decode(current_.text, p, cp) || !word(cp))
                    fail("bare terms must be identifiers; put punctuation inside double quotes or /regex/");
            }
        }
    }
    std::shared_ptr<const QueryNode> combine(NodeKind kind,
        std::shared_ptr<const QueryNode> left, std::shared_ptr<const QueryNode> right = {}) {
        auto node = std::make_shared<QueryNode>();
        node->kind = kind; node->left = std::move(left); node->right = std::move(right);
        return node;
    }
    std::shared_ptr<const QueryNode> parse_or(std::size_t depth) {
        auto left = parse_and(depth);
        while (current_.kind == LexKind::Or) { advance(); left = combine(NodeKind::Or, std::move(left), parse_and(depth)); }
        return left;
    }
    static bool begins_operand(LexKind kind) {
        return kind == LexKind::Term || kind == LexKind::Phrase || kind == LexKind::Regex ||
               kind == LexKind::Not || kind == LexKind::Open;
    }
    std::shared_ptr<const QueryNode> parse_and(std::size_t depth) {
        auto left = parse_unary(depth);
        while (current_.kind == LexKind::And || begins_operand(current_.kind)) {
            if (current_.kind == LexKind::And) advance();
            left = combine(NodeKind::And, std::move(left), parse_unary(depth));
        }
        return left;
    }
    std::shared_ptr<const QueryNode> parse_unary(std::size_t depth) {
        if (depth > max_nesting) fail("parenthesis/NOT nesting exceeds 64");
        if (current_.kind == LexKind::Not) { advance(); return combine(NodeKind::Not, parse_unary(depth + 1)); }
        if (current_.kind == LexKind::Open) {
            advance();
            auto node = parse_or(depth + 1);
            if (current_.kind != LexKind::Close) fail("expected closing parenthesis");
            advance();
            return node;
        }
        if (current_.kind != LexKind::Term && current_.kind != LexKind::Phrase && current_.kind != LexKind::Regex)
            fail("expected a term, phrase, regex or parenthesized expression");
        if (result_.leaves.size() == max_leaves) fail("query has more than 128 leaves");
        QueryLeaf leaf;
        leaf.kind = current_.kind == LexKind::Term ? LeafKind::Term :
                    current_.kind == LexKind::Phrase ? LeafKind::Phrase : LeafKind::Regex;
        leaf.text = current_.text;
        if (leaf.kind == LeafKind::Regex) {
            leaf.regex = std::make_shared<Regex>(leaf.text);
            if (leaf.regex->state_count() > max_query_regex_states - compiled_states_)
                fail("query regexes expand beyond 32768 total NFA states");
            compiled_states_ += leaf.regex->state_count();
        }
        auto node = std::make_shared<QueryNode>();
        node->leaf = result_.leaves.size();
        result_.leaves.push_back(std::move(leaf));
        advance();
        return node;
    }
};

bool eval(const QueryNode& node, const std::vector<bool>& values) {
    switch (node.kind) {
    case NodeKind::Leaf:
        if (node.leaf >= values.size()) throw Error("query leaf vector is too short");
        return values[node.leaf];
    case NodeKind::And: return eval(*node.left, values) && eval(*node.right, values);
    case NodeKind::Or: return eval(*node.left, values) || eval(*node.right, values);
    case NodeKind::Not: return !eval(*node.left, values);
    }
    throw Error("invalid query node");
}
std::uint8_t possible_eval(const QueryNode& node, const std::vector<std::uint8_t>& values) {
    if (node.kind == NodeKind::Leaf) {
        if (node.leaf >= values.size() || values[node.leaf] > 2) throw Error("invalid query possibility vector");
        return values[node.leaf];
    }
    const auto left = possible_eval(*node.left, values);
    if (node.kind == NodeKind::Not) return left == 2 ? 2 : static_cast<std::uint8_t>(1 - left);
    const auto right = possible_eval(*node.right, values);
    if (node.kind == NodeKind::And) return left == 0 || right == 0 ? 0 : left == 1 && right == 1 ? 1 : 2;
    if (node.kind == NodeKind::Or) return left == 1 || right == 1 ? 1 : left == 0 && right == 0 ? 0 : 2;
    throw Error("invalid query node");
}
}

bool valid_utf8(std::string_view text) {
    std::size_t offset = 0;
    while (offset < text.size()) {
        char32_t cp = 0;
        if (!decode(text, offset, cp)) return false;
    }
    return true;
}

void for_each_token(std::string_view line, const std::function<void(Token)>& visitor) {
    std::size_t offset = 0, begin = absent;
    while (offset < line.size()) {
        const auto before = offset;
        char32_t cp = 0;
        if (!decode(line, offset, cp)) throw Error("cannot tokenize invalid UTF-8");
        if (word(cp)) { if (begin == absent) begin = before; }
        else if (begin != absent) {
            visitor({line.substr(begin, before - begin), begin});
            begin = absent;
        }
    }
    if (begin != absent) visitor({line.substr(begin), begin});
}

struct Regex::Impl {
    std::vector<State> states;
    std::vector<CharClass> classes;
    std::size_t start = 0;
    std::shared_ptr<const RegexFilter> filter;
};

Regex::Regex(const std::string& pattern) {
    RegexParser parser(pattern);
    const auto root = parser.parse();
    Compiler compiler(parser.nodes);
    auto impl = std::make_shared<Impl>();
    impl->start = compiler.compile(root);
    impl->states = std::move(compiler.states);
    impl->classes = std::move(parser.classes);
    // Necessary conditions only. Unknown constructs weaken the filter to true.
    using F = RegexFilter;
    struct Necessary { std::optional<std::string> exact; std::shared_ptr<const F> filter; };
    auto always = std::make_shared<F>();
    auto literal_filter = [&](const std::string& text) -> std::shared_ptr<const F> {
        if (text.size() < 3) return always;
        auto f = std::make_shared<F>(); f->kind = F::Kind::Literal; f->literal = text; return f;
    };
    std::size_t filter_nodes = 0;
    std::function<Necessary(std::size_t)> derive = [&](std::size_t id) -> Necessary {
        if (++filter_nodes > 512) return {std::nullopt, always};
        const auto& n = parser.nodes[id];
        if (n.kind == AstKind::Empty || n.kind == AstKind::Bol || n.kind == AstKind::Eol ||
            n.kind == AstKind::Boundary || n.kind == AstKind::NotBoundary) return {std::string(), always};
        if (n.kind == AstKind::Literal) {
            std::string text; const auto cp = static_cast<std::uint32_t>(n.literal);
            if (cp < 0x80) text += static_cast<char>(cp);
            else if (cp < 0x800) { text += static_cast<char>(0xc0 | (cp >> 6)); text += static_cast<char>(0x80 | (cp & 63)); }
            else if (cp < 0x10000) { text += static_cast<char>(0xe0 | (cp >> 12)); text += static_cast<char>(0x80 | ((cp >> 6) & 63)); text += static_cast<char>(0x80 | (cp & 63)); }
            else { text += static_cast<char>(0xf0 | (cp >> 18)); text += static_cast<char>(0x80 | ((cp >> 12) & 63)); text += static_cast<char>(0x80 | ((cp >> 6) & 63)); text += static_cast<char>(0x80 | (cp & 63)); }
            return {text, literal_filter(text)};
        }
        if (n.kind == AstKind::Concat || n.kind == AstKind::Alternate) {
            auto a = derive(n.left), b = derive(n.right);
            if (n.kind == AstKind::Concat && a.exact && b.exact && a.exact->size() + b.exact->size() <= 8192) {
                auto text = *a.exact + *b.exact; return {text, literal_filter(text)};
            }
            auto f = std::make_shared<F>(); f->kind = n.kind == AstKind::Concat ? F::Kind::And : F::Kind::Or;
            f->left = a.filter; f->right = b.filter; return {std::nullopt, f};
        }
        if (n.kind == AstKind::Repeat) {
            if (!n.minimum) return {std::nullopt, always};
            auto a = derive(n.left);
            if (a.exact && a.exact->size() * n.minimum <= 8192) {
                std::string repeated; for (std::uint32_t i = 0; i < n.minimum; ++i) repeated += *a.exact;
                return {!n.unlimited && n.minimum == n.maximum ? std::optional<std::string>(repeated) : std::nullopt, literal_filter(repeated)};
            }
            return {std::nullopt, a.filter};
        }
        return {std::nullopt, always};
    };
    impl->filter = derive(root).filter;
    impl_ = std::move(impl);
}

std::size_t Regex::state_count() const { return impl_->states.size(); }
std::shared_ptr<const RegexFilter> Regex::prefilter() const { return impl_->filter; }

bool Regex::find(std::string_view text, RegexMatch& match, std::uint64_t& steps_left, std::size_t start) const {
    RegexWorkspace workspace; return find(text, match, steps_left, workspace, start);
}
bool Regex::find(std::string_view text, RegexMatch& match, std::uint64_t& steps_left, RegexWorkspace& workspace, std::size_t start) const {
    if (start > text.size()) throw Error("regex start offset is outside the text");
    if (start < text.size() && (static_cast<unsigned char>(text[start]) & 0xc0) == 0x80)
        throw Error("regex start offset is inside a UTF-8 character");
    charge(steps_left, static_cast<std::uint64_t>(text.size()) + 1);
    if (!valid_utf8(text)) throw Error("regex input is not valid UTF-8");
    const auto& machine = *impl_;
    using Thread = RegexWorkspace::Thread;
    auto& current = workspace.current; auto& next = workspace.next;
    auto& stack = workspace.stack; auto& visited = workspace.visited;
    current.clear(); next.clear(); stack.clear();
    current.reserve(machine.states.size()); next.reserve(machine.states.size()); stack.reserve(machine.states.size() * 2);
    visited.resize(machine.states.size(), 0);
    auto& epoch = workspace.epoch;
    if (++epoch == 0) { std::fill(visited.begin(), visited.end(), 0); epoch = 1; }
    bool found = false;
    RegexMatch best{};
    char32_t previous = 0;
    bool has_previous = start != 0;
    if (has_previous) {
        auto offset = start - 1;
        while (offset != 0 && (static_cast<unsigned char>(text[offset]) & 0xc0) == 0x80) --offset;
        decode(text, offset, previous);
    }
    // Calls for a given closure are supplied in ascending origin order. The
    // first arrival at a state dominates all later arrivals, so one visit per
    // state per input position suffices even for nested epsilon cycles.
    auto closure = [&](std::size_t seed, std::size_t origin, std::size_t position,
                       bool left_word, bool right_word, std::vector<Thread>& out) {
        stack.clear();
        stack.push_back(seed);
        while (!stack.empty()) {
            const auto id = stack.back(); stack.pop_back();
            charge(steps_left);
            if (id == absent || visited[id] == epoch) continue;
            visited[id] = epoch;
            if (found && origin >= best.begin) continue;
            const auto& state = machine.states[id];
            switch (state.op) {
            case Op::Split: stack.push_back(state.alternate); stack.push_back(state.next); break;
            case Op::Epsilon: stack.push_back(state.next); break;
            case Op::Bol: if (position == 0) stack.push_back(state.next); break;
            case Op::Eol: if (position == text.size()) stack.push_back(state.next); break;
            case Op::Boundary: if (left_word != right_word) stack.push_back(state.next); break;
            case Op::NotBoundary: if (left_word == right_word) stack.push_back(state.next); break;
            case Op::Accept:
                if (!found || origin < best.begin) { found = true; best = {origin, position}; }
                break;
            default: out.push_back({id, origin}); break;
            }
        }
    };

    std::size_t position = start;
    char32_t cp = 0;
    auto following = position;
    const bool has_cp = decode(text, following, cp);
    closure(machine.start, start, start, has_previous && word(previous), has_cp && word(cp), current);
    while (true) {
        if (found && (current.empty() || current.front().origin >= best.begin)) { match = best; return true; }
        if (position == text.size()) break;
        charge(steps_left);
        following = position;
        decode(text, following, cp);
        char32_t after = 0;
        auto after_position = following;
        const bool has_after = decode(text, after_position, after);
        next.clear();
        if (++epoch == 0) { std::fill(visited.begin(), visited.end(), 0); epoch = 1; }
        for (const auto& thread : current) {
            charge(steps_left);
            if (found && thread.origin >= best.begin) continue;
            const auto& state = machine.states[thread.state];
            bool accepts = false;
            switch (state.op) {
            case Op::Literal: accepts = cp == state.literal; break;
            case Op::Any: accepts = cp != U'\n' && cp != U'\r'; break;
            case Op::Class: accepts = machine.classes[state.class_id].matches(cp); break;
            default: throw Error("internal regex transition error");
            }
            if (accepts) closure(state.next, thread.origin, following, word(cp), has_after && word(after), next);
        }
        if (!found) closure(machine.start, following, following, word(cp), has_after && word(after), next);
        current.swap(next);
        position = following;
    }
    if (found) match = best;
    return found;
}

Query parse_query(const std::string& input) {
    try { return QueryParser(input).parse(); }
    catch (const Error& e) { if (e.code() == "E_BUDGET") throw; throw Error("E_QUERY_SYNTAX", e.what()); }
}
bool Query::evaluate(const std::vector<bool>& present) const {
    if (!root || present.size() != leaves.size()) throw Error("invalid query evaluation arguments");
    return eval(*root, present);
}
std::uint8_t Query::possible(const std::vector<std::uint8_t>& values) const {
    if (!root || values.size() != leaves.size()) throw Error("invalid query possibility arguments");
    return possible_eval(*root, values);
}
}
