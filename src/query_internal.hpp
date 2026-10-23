#pragma once
#include "researchify/query.hpp"

namespace researchify::detail {
// Internal entry point for ContentReader's line callback only. stream_text
// validates every complete line before invoking that callback. The view must
// still refer to those same validated bytes; no validation is cached by address.
// Public Regex::find remains strict, and both paths charge identical work units.
struct ValidatedRegexAccess {
    static bool find(const Regex& regex, std::string_view text, RegexMatch& match,
                     std::uint64_t& steps_left, RegexWorkspace& workspace, std::size_t start = 0) {
        return regex.find_impl(text, match, steps_left, workspace, start, true);
    }
};
}
