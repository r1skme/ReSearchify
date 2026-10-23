#pragma once

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace researchify {

// Optional document-level necessary condition for line-wise literal matching.
// Feed precisely the normalized lines seen by the matcher (after BOM/CR removal).
// Every 4-byte window is indexed, including windows inside long tokens and across
// UTF-8 code points. Collisions and saturation only cause extra verification.
// Persisted bytes need an external version, checksum and content fingerprint;
// Missing sidecars must fall back to verification; corrupted or unsupported
// sidecars must fail explicitly or fall back, never silently reject a real match.
class LiteralFilter {
public:
    static constexpr std::size_t Bytes = 128;
    static constexpr std::size_t GramBytes = 4;
    static constexpr std::size_t MaximumBuildBytes = 128 * 1024;

    void add_line(std::string_view line) {
        if (saturated_) return;
        if (line.size() > MaximumBuildBytes - processed_) {
            saturate();
            return;
        }
        processed_ += line.size();
        each_gram(line, [&](std::uint64_t hash) {
            set(hash);
            set(hash >> 32);
            return true;
        });
        // At this density the filter has little value. Stop hashing subsequent
        // lines, conservatively representing every possible gram instead.
        if (processed_ < next_density_check_) return;
        next_density_check_ = processed_ + 1024;
        std::size_t bits = 0;
        for (const auto word : words_) bits += static_cast<std::size_t>(std::popcount(word));
        if (bits >= Bytes * 8 * 9 / 10) saturate();
    }

    bool may_contain(std::string_view literal) const {
        if (saturated_) return true;
        return each_gram(literal, [&](std::uint64_t hash) {
            return test(hash) && test(hash >> 32);
        });
    }

    // Query masks must contain only bits actually required by the literal. In
    // particular, document-side density/work-budget shortcuts must NEVER add
    // extra required bits: an over-approximated document is safe, an
    // over-constrained query is not. Do not use add_line() to prepare queries.
    static LiteralFilter required(std::string_view literal) {
        LiteralFilter result;
        each_gram(literal, [&](std::uint64_t hash) {
            result.set(hash);
            result.set(hash >> 32);
            return true;
        });
        return result;
    }

    // Precomputing the query mask avoids repeated hashing during a candidate scan.
    // Construct required with required(), not with the document builder.
    // A query shorter than four bytes produces an empty mask and always passes.
    bool maybe(const LiteralFilter& required) const {
        for (std::size_t i = 0; i < words_.size(); ++i)
            if ((words_[i] & required.words_[i]) != required.words_[i]) return false;
        return true;
    }

    // Fixed little-endian representation, independent of the host byte order.
    std::string encode() const {
        std::string result(Bytes, '\0');
        for (std::size_t i = 0; i < words_.size(); ++i)
            for (unsigned byte = 0; byte < 8; ++byte)
                result[i * 8 + byte] = static_cast<char>((words_[i] >> (byte * 8)) & 255U);
        return result;
    }

    static std::optional<LiteralFilter> decode(std::string_view bytes) {
        if (bytes.size() != Bytes) return std::nullopt;
        LiteralFilter result;
        for (std::size_t i = 0; i < result.words_.size(); ++i)
            for (unsigned byte = 0; byte < 8; ++byte)
                result.words_[i] |= static_cast<std::uint64_t>(static_cast<unsigned char>(bytes[i * 8 + byte])) << (byte * 8);
        result.saturated_ = true;
        for (const auto word : result.words_) result.saturated_ &= word == ~std::uint64_t{0};
        // A decoded mask is intended for filtering, not resumed indexing. Counting
        // the limit conservatively makes a subsequent add_line safe as well.
        result.processed_ = MaximumBuildBytes;
        return result;
    }

private:
    std::array<std::uint64_t, Bytes / 8> words_{};
    std::size_t processed_ = 0;
    std::size_t next_density_check_ = 1024;
    bool saturated_ = false;

    static std::uint64_t mix(std::uint64_t hash) {
        hash ^= hash >> 30;
        hash *= 0xbf58476d1ce4e5b9ULL;
        hash ^= hash >> 27;
        hash *= 0x94d049bb133111ebULL;
        return hash ^ (hash >> 31);
    }

    template<class Visitor> static bool each_gram(std::string_view text, Visitor&& visit) {
        if (text.size() < GramBytes) return true;
        std::uint64_t gram = 0;
        for (std::size_t i = 0; i < GramBytes; ++i)
            gram = gram * 257U + static_cast<unsigned char>(text[i]) + 1U;
        if (!visit(mix(gram))) return false;
        constexpr std::uint64_t high_factor = 257ULL * 257 * 257;
        for (std::size_t i = GramBytes; i < text.size(); ++i) {
            gram = (gram - (static_cast<unsigned char>(text[i - GramBytes]) + 1ULL) * high_factor) * 257U
                 + static_cast<unsigned char>(text[i]) + 1U;
            if (!visit(mix(gram))) return false;
        }
        return true;
    }

    void set(std::uint64_t hash) {
        const auto bit = static_cast<std::size_t>(hash & (Bytes * 8 - 1));
        words_[bit / 64] |= std::uint64_t{1} << (bit % 64);
    }
    bool test(std::uint64_t hash) const {
        const auto bit = static_cast<std::size_t>(hash & (Bytes * 8 - 1));
        return (words_[bit / 64] & (std::uint64_t{1} << (bit % 64))) != 0;
    }
    void saturate() {
        words_.fill(~std::uint64_t{0});
        saturated_ = true;
    }
};

}
