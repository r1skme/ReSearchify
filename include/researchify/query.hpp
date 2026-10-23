#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace researchify {
namespace detail { struct ValidatedRegexAccess; }
// UTF-8 is validated strictly. Words comprise ASCII letters/digits/underscore
// and non-ASCII scalar values. Matching is case-sensitive, without normalization.
bool valid_utf8(std::string_view text);
struct Token { std::string_view text; std::size_t column = 0; };
void for_each_token(std::string_view line, const std::function<void(Token)>& visitor);

struct RegexWorkspace {
    struct Thread { std::size_t state = 0, origin = 0; };
    std::vector<Thread> current, next;
    std::vector<std::size_t> stack, visited;
    std::size_t epoch = 0;
};
struct RegexFilter {
    enum class Kind { Always, Literal, And, Or };
    Kind kind = Kind::Always;
    std::string literal;
    std::shared_ptr<const RegexFilter> left, right;
};
struct RegexMatch { std::size_t begin = 0, end = 0; };
// Thompson NFA; no std::regex backtracking or external dependencies.
class Regex {
public:
    explicit Regex(const std::string& pattern);
    bool find(std::string_view text, RegexMatch& match, std::uint64_t& steps_left,
              std::size_t start = 0) const;
    bool find(std::string_view text, RegexMatch& match, std::uint64_t& steps_left,
              RegexWorkspace& workspace, std::size_t start = 0) const;
    std::shared_ptr<const RegexFilter> prefilter() const;
    std::size_t state_count() const;
private:
    friend struct detail::ValidatedRegexAccess;
    bool find_impl(std::string_view text, RegexMatch& match, std::uint64_t& steps_left,
                   RegexWorkspace& workspace, std::size_t start, bool input_validated) const;
    struct Impl;
    std::shared_ptr<const Impl> impl_;
};
enum class LeafKind { Term, Phrase, Regex };
struct QueryLeaf {
    LeafKind kind = LeafKind::Term;
    std::string text;
    std::shared_ptr<const Regex> regex;
};
enum class NodeKind { Leaf, And, Or, Not };
struct QueryNode {
    NodeKind kind = NodeKind::Leaf;
    std::size_t leaf = 0;
    std::shared_ptr<const QueryNode> left, right;
};
struct Query {
    std::vector<QueryLeaf> leaves;
    std::shared_ptr<const QueryNode> root;
    bool evaluate(const std::vector<bool>& present) const;
    // 0=false, 1=true, 2=unknown. Used to prune without false negatives.
    std::uint8_t possible(const std::vector<std::uint8_t>& values) const;
};
Query parse_query(const std::string& input);
}
