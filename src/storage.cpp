#include "researchify/storage.hpp"
#include "researchify/engine.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <fstream>
#include <limits>
#include <optional>
#include <queue>
#include <system_error>
#include <utility>

namespace researchify::storage {
namespace {
constexpr std::uint32_t version = 1;
constexpr std::size_t header_bytes = 64;
constexpr std::size_t record_bytes = 32;
constexpr std::size_t records_per_block = 256;
constexpr std::size_t block_bytes = 4 + records_per_block * record_bytes + 4;
constexpr std::size_t merge_fan_in = 32;
constexpr std::array<char, 8> magic = {'C', 'S', 'P', 'O', 'S', 'T', '2', '0'};

std::string display_path(const fs::path& path) {
    const auto text = path.u8string();
    return std::string(text.begin(), text.end());
}

void write_exact(std::ostream& out, const char* bytes, std::size_t size) {
    if (size > static_cast<std::size_t>((std::numeric_limits<std::streamsize>::max)()))
        throw Error("Write exceeds stream size limit");
    out.write(bytes, static_cast<std::streamsize>(size));
    if (!out) throw Error("Failed to write index data");
}

void read_exact(std::istream& in, char* bytes, std::size_t size) {
    if (size > static_cast<std::size_t>((std::numeric_limits<std::streamsize>::max)()))
        throw Error("Read exceeds stream size limit");
    in.read(bytes, static_cast<std::streamsize>(size));
    if (!in) throw Error("Truncated or unreadable index data");
}

void encode32(char* out, std::uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) out[i] = static_cast<char>((value >> (i * 8)) & 255U);
}

void encode64(char* out, std::uint64_t value) {
    for (unsigned i = 0; i < 8; ++i) out[i] = static_cast<char>((value >> (i * 8)) & 255U);
}

std::uint32_t decode32(const char* in) {
    std::uint32_t value = 0;
    for (unsigned i = 0; i < 4; ++i)
        value |= static_cast<std::uint32_t>(static_cast<unsigned char>(in[i])) << (i * 8);
    return value;
}

std::uint64_t decode64(const char* in) {
    std::uint64_t value = 0;
    for (unsigned i = 0; i < 8; ++i)
        value |= static_cast<std::uint64_t>(static_cast<unsigned char>(in[i])) << (i * 8);
    return value;
}

bool key_less(const Posting& a, const Posting& b) {
    return a.hash < b.hash || (a.hash == b.hash && a.document < b.document);
}

bool key_equal(const Posting& a, const Posting& b) {
    return a.hash == b.hash && a.document == b.document;
}

void combine(Posting& into, const Posting& other) {
    if (other.frequency > (std::numeric_limits<std::uint64_t>::max)() - into.frequency)
        throw Error("Posting frequency overflow");
    into.frequency += other.frequency;
    if (other.line < into.line || (other.line == into.line && other.column < into.column)) {
        into.line = other.line;
        into.column = other.column;
    }
}

void encode_posting(char* out, const Posting& posting) {
    encode64(out, posting.hash);
    encode64(out + 8, posting.document);
    encode64(out + 16, posting.frequency);
    encode32(out + 24, posting.line);
    encode32(out + 28, posting.column);
}

Posting decode_posting(const char* in) {
    Posting posting;
    posting.hash = decode64(in);
    posting.document = decode64(in + 8);
    posting.frequency = decode64(in + 16);
    posting.line = decode32(in + 24);
    posting.column = decode32(in + 28);
    if (posting.frequency == 0) throw Error("Corrupt posting: zero frequency");
    return posting;
}

std::uint64_t blocks_for(std::uint64_t count) {
    return count / records_per_block + (count % records_per_block != 0 ? 1U : 0U);
}

std::uint64_t physical_bytes(std::uint64_t blocks) {
    const auto maximum = static_cast<std::uint64_t>((std::numeric_limits<std::streamoff>::max)());
    if (blocks > (maximum - header_bytes) / block_bytes)
        throw Error("Postings file exceeds addressable stream size");
    return header_bytes + blocks * block_bytes;
}

std::array<char, header_bytes> make_header(std::uint64_t count) {
    std::array<char, header_bytes> header{};
    std::copy(magic.begin(), magic.end(), header.begin());
    encode32(header.data() + 8, version);
    encode32(header.data() + 12, static_cast<std::uint32_t>(records_per_block));
    encode32(header.data() + 16, static_cast<std::uint32_t>(record_bytes));
    encode32(header.data() + 20, static_cast<std::uint32_t>(block_bytes));
    encode64(header.data() + 24, count);
    encode64(header.data() + 32, blocks_for(count));
    encode32(header.data() + 56, crc32(std::string_view(header.data(), 56)));
    return header;
}

class BlockWriter {
public:
    explicit BlockWriter(const fs::path& path) : out_(path, std::ios::binary | std::ios::trunc) {
        if (!out_) throw Error("Cannot create postings file: " + display_path(path));
        const auto header = make_header(0);
        write_exact(out_, header.data(), header.size());
    }

    void add(const Posting& posting) {
        if (finished_) throw Error("Cannot append to completed postings file");
        if (posting.frequency == 0) throw Error("Posting frequency must be positive");
        if (previous_ && !key_less(*previous_, posting))
            throw Error("Postings output must have strictly increasing keys");
        if (count_ == (std::numeric_limits<std::uint64_t>::max)())
            throw Error("Postings count overflow");
        encode_posting(block_.data() + 4 + used_ * record_bytes, posting);
        previous_ = posting;
        ++used_;
        ++count_;
        if (used_ == records_per_block) flush_block();
    }

    void finish() {
        if (finished_) return;
        if (used_ != 0) flush_block();
        const auto header = make_header(count_);
        static_cast<void>(physical_bytes(blocks_for(count_)));
        out_.seekp(0);
        if (!out_) throw Error("Cannot seek postings header");
        write_exact(out_, header.data(), header.size());
        out_.flush();
        if (!out_) throw Error("Cannot flush postings file");
        out_.close();
        if (out_.fail()) throw Error("Cannot close postings file");
        finished_ = true;
    }

private:
    void flush_block() {
        encode32(block_.data(), static_cast<std::uint32_t>(used_));
        const auto checksum = crc32(std::string_view(block_.data(), block_bytes - 4));
        encode32(block_.data() + block_bytes - 4, checksum);
        write_exact(out_, block_.data(), block_.size());
        block_.fill(0);
        used_ = 0;
    }

    std::ofstream out_;
    std::array<char, block_bytes> block_{};
    std::optional<Posting> previous_;
    std::uint64_t count_ = 0;
    std::size_t used_ = 0;
    bool finished_ = false;
};

class BlockReader {
public:
    explicit BlockReader(const fs::path& path) : in_(path, std::ios::binary) {
        if (!in_) throw Error("Cannot open postings file: " + display_path(path));
        const auto actual_size = checked_file_size(path);
        if (actual_size < header_bytes) throw Error("Truncated postings header");
        std::array<char, header_bytes> header{};
        read_exact(in_, header.data(), header.size());
        if (!std::equal(magic.begin(), magic.end(), header.begin()))
            throw Error("Invalid postings magic");
        if (decode32(header.data() + 56) != crc32(std::string_view(header.data(), 56)))
            throw Error("Postings header checksum mismatch");
        if (decode32(header.data() + 8) != version ||
            decode32(header.data() + 12) != records_per_block ||
            decode32(header.data() + 16) != record_bytes ||
            decode32(header.data() + 20) != block_bytes)
            throw Error("Unsupported postings format");
        if (decode64(header.data() + 40) != 0 || decode64(header.data() + 48) != 0 ||
            decode32(header.data() + 60) != 0)
            throw Error("Invalid reserved postings header fields");
        count_ = decode64(header.data() + 24);
        blocks_ = decode64(header.data() + 32);
        if (blocks_ != blocks_for(count_)) throw Error("Inconsistent postings block count");
        file_bytes_ = physical_bytes(blocks_);
        if (file_bytes_ != actual_size) throw Error("Postings file length mismatch");
    }

    std::uint64_t count() const { return count_; }
    std::uint64_t blocks() const { return blocks_; }
    std::uint64_t bytes() const { return file_bytes_; }
    std::size_t loaded_size() const { return loaded_size_; }

    void load(std::uint64_t index) {
        if (index >= blocks_) throw Error("Postings block offset out of range");
        if (loaded_index_ == index) return;
        const auto offset = header_bytes + index * block_bytes;
        in_.clear();
        in_.seekg(static_cast<std::streamoff>(offset));
        if (!in_) throw Error("Cannot seek postings block");
        read_exact(in_, block_.data(), block_.size());
        if (decode32(block_.data() + block_bytes - 4) !=
            crc32(std::string_view(block_.data(), block_bytes - 4)))
            throw Error("Postings block checksum mismatch");
        const auto size = decode32(block_.data());
        const auto expected = index + 1 == blocks_
            ? static_cast<std::size_t>(count_ - index * records_per_block)
            : records_per_block;
        if (size != expected || size == 0 || size > records_per_block)
            throw Error("Invalid postings block length");
        std::optional<Posting> previous;
        for (std::size_t i = 0; i < size; ++i) {
            const auto posting = decode_posting(block_.data() + 4 + i * record_bytes);
            if (previous && !key_less(*previous, posting))
                throw Error("Postings block keys are not strictly sorted");
            previous = posting;
        }
        const auto padding_begin = 4 + static_cast<std::size_t>(size) * record_bytes;
        if (std::any_of(block_.begin() + static_cast<std::ptrdiff_t>(padding_begin),
                        block_.end() - 4, [](char value) { return value != 0; }))
            throw Error("Nonzero postings block padding");
        loaded_size_ = size;
        loaded_index_ = index;
    }

    Posting at(std::size_t index) const {
        if (index >= loaded_size_) throw Error("Postings record offset out of range");
        return decode_posting(block_.data() + 4 + index * record_bytes);
    }

    bool next(Posting& output) {
        if (cursor_ == count_) return false;
        load(cursor_ / records_per_block);
        output = at(static_cast<std::size_t>(cursor_ % records_per_block));
        if (last_iterated_ && !key_less(*last_iterated_, output))
            throw Error("Postings keys are not globally sorted");
        last_iterated_ = output;
        ++cursor_;
        return true;
    }

    void lookup(std::uint64_t hash, const std::function<void(const Posting&)>& visitor) {
        std::uint64_t low = 0, high = blocks_;
        while (low < high) {
            const auto middle = low + (high - low) / 2;
            load(middle);
            if (at(loaded_size_ - 1).hash < hash) low = middle + 1;
            else high = middle;
        }
        std::optional<Posting> previous;
        for (std::uint64_t index = low; index < blocks_; ++index) {
            load(index);
            if (previous && !key_less(*previous, at(0)))
                throw Error("Postings keys are not globally sorted");
            if (at(0).hash > hash) break;
            for (std::size_t i = 0; i < loaded_size_; ++i) {
                const auto posting = at(i);
                if (posting.hash == hash) visitor(posting);
                if (posting.hash > hash) return;
            }
            previous = at(loaded_size_ - 1);
            if (previous->hash > hash) break;
        }
    }

    std::uint64_t verify() {
        // Verification is intentionally independent of the iterator cursor.
        std::optional<Posting> previous;
        for (std::uint64_t index = 0; index < blocks_; ++index) {
            // Force rereading so repeated verification sees modifications too.
            loaded_index_ = (std::numeric_limits<std::uint64_t>::max)();
            load(index);
            if (previous && !key_less(*previous, at(0)))
                throw Error("Postings keys are not globally sorted");
            previous = at(loaded_size_ - 1);
        }
        return file_bytes_;
    }

private:
    std::ifstream in_;
    std::array<char, block_bytes> block_{};
    std::uint64_t count_ = 0, blocks_ = 0, file_bytes_ = 0, cursor_ = 0;
    std::uint64_t loaded_index_ = (std::numeric_limits<std::uint64_t>::max)();
    std::size_t loaded_size_ = 0;
    std::optional<Posting> last_iterated_;
};

fs::path private_work_dir(const fs::path& parent, const std::string& prefix) {
    if (prefix.empty() || prefix.size() > 64 ||
        std::any_of(prefix.begin(), prefix.end(), [](unsigned char c) {
            return !((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                     (c >= '0' && c <= '9') || c == '-' || c == '_');
        }))
        throw Error("Invalid temporary sort prefix");
    std::error_code error;
    fs::create_directories(parent, error);
    if (error) throw Error("Cannot create sort work directory: " + error.message());
    static std::atomic<std::uint64_t> serial{0};
    for (unsigned attempt = 0; attempt < 100; ++attempt) {
        const auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
        const auto unique = serial.fetch_add(1, std::memory_order_relaxed);
        const auto path = parent / (prefix + "-sort-" + std::to_string(tick) + "-" +
                                    std::to_string(unique));
        error.clear();
        if (fs::create_directory(path, error)) return path;
        if (error && error != std::errc::file_exists)
            throw Error("Cannot create private sort directory: " + error.message());
    }
    throw Error("Cannot allocate unique sort directory");
}

struct TempDirectory {
    fs::path path;
    ~TempDirectory() {
        std::error_code ignored;
        fs::remove_all(path, ignored);
    }
};

void merge_group(const std::vector<fs::path>& inputs, const fs::path& output) {
    if (inputs.size() > merge_fan_in) throw Error("Too many simultaneous merge inputs");
    struct QueueItem { Posting posting; std::size_t input; };
    struct Later {
        bool operator()(const QueueItem& a, const QueueItem& b) const {
            if (key_less(b.posting, a.posting)) return true;
            if (key_less(a.posting, b.posting)) return false;
            return a.input > b.input;
        }
    };
    std::vector<std::unique_ptr<BlockReader>> readers;
    readers.reserve(inputs.size());
    std::priority_queue<QueueItem, std::vector<QueueItem>, Later> queue;
    for (const auto& input : inputs) {
        auto reader = std::make_unique<BlockReader>(input);
        Posting first;
        if (reader->next(first)) queue.push({first, readers.size()});
        readers.push_back(std::move(reader));
    }
    BlockWriter writer(output);
    std::optional<Posting> accumulated;
    while (!queue.empty()) {
        const auto item = queue.top();
        queue.pop();
        if (accumulated && key_equal(*accumulated, item.posting)) {
            combine(*accumulated, item.posting);
        } else {
            if (accumulated) writer.add(*accumulated);
            accumulated = item.posting;
        }
        Posting next;
        if (readers[item.input]->next(next)) queue.push({next, item.input});
    }
    if (accumulated) writer.add(*accumulated);
    writer.finish();
}

std::uint64_t avalanche(std::uint64_t value) {
    value ^= value >> 30;
    value *= 0xbf58476d1ce4e5b9ULL;
    value ^= value >> 27;
    value *= 0x94d049bb133111ebULL;
    return value ^ (value >> 31);
}
}

std::uint32_t crc32(std::string_view data, std::uint32_t previous) {
    static const auto table = [] {
        std::array<std::uint32_t, 256> values{};
        for (std::uint32_t i = 0; i < 256; ++i) {
            auto value = i;
            for (unsigned bit = 0; bit < 8; ++bit)
                value = (value & 1U) != 0 ? (value >> 1) ^ 0xedb88320U : value >> 1;
            values[i] = value;
        }
        return values;
    }();
    auto value = previous ^ 0xffffffffU;
    for (const char raw : data) {
        const auto byte = static_cast<unsigned char>(raw);
        value = table[(value ^ byte) & 255U] ^ (value >> 8);
    }
    return value ^ 0xffffffffU;
}

Hasher::Hasher() : a_(0xcbf29ce484222325ULL), b_(0x84222325cbf29ce4ULL) {}

void Hasher::add(std::string_view bytes) {
    if (bytes.size() > (std::numeric_limits<std::uint64_t>::max)() - count_)
        throw Error("Fingerprint input length overflow");
    for (const char raw : bytes) {
        const auto byte = static_cast<unsigned char>(raw);
        a_ ^= byte;
        a_ *= 0x100000001b3ULL;
        b_ ^= static_cast<std::uint64_t>(byte) + 0x9dU;
        b_ = std::rotl(b_, 7) * 0x9e3779b185ebca87ULL;
    }
    count_ += static_cast<std::uint64_t>(bytes.size());
}

Fingerprint Hasher::finish() const {
    return {avalanche(a_ ^ std::rotl(count_, 17)), avalanche(b_ ^ count_ ^ std::rotl(a_, 29))};
}

std::uint64_t term_hash(std::string_view token) {
    std::uint64_t value = 0xcbf29ce484222325ULL;
    for (const char raw : token) {
        const auto byte = static_cast<unsigned char>(raw);
        value ^= byte;
        value *= 0x100000001b3ULL;
    }
    return avalanche(value ^ static_cast<std::uint64_t>(token.size()));
}

void put_u32(std::ostream& stream, std::uint32_t value) {
    std::array<char, 4> bytes{};
    encode32(bytes.data(), value);
    write_exact(stream, bytes.data(), bytes.size());
}

void put_u64(std::ostream& stream, std::uint64_t value) {
    std::array<char, 8> bytes{};
    encode64(bytes.data(), value);
    write_exact(stream, bytes.data(), bytes.size());
}

std::uint32_t get_u32(std::istream& stream) {
    std::array<char, 4> bytes{};
    read_exact(stream, bytes.data(), bytes.size());
    return decode32(bytes.data());
}

std::uint64_t get_u64(std::istream& stream) {
    std::array<char, 8> bytes{};
    read_exact(stream, bytes.data(), bytes.size());
    return decode64(bytes.data());
}

void put_string(std::ostream& stream, const std::string& value) {
    put_u64(stream, static_cast<std::uint64_t>(value.size()));
    write_exact(stream, value.data(), value.size());
}

std::string get_string(std::istream& stream, std::size_t maximum) {
    const auto length = get_u64(stream);
    if (length > maximum || length > static_cast<std::uint64_t>((std::numeric_limits<std::streamsize>::max)()) ||
        length > std::string{}.max_size())
        throw Error("Index string length exceeds configured limit");
    std::string value(static_cast<std::size_t>(length), '\0');
    if (!value.empty()) read_exact(stream, value.data(), value.size());
    return value;
}

std::uint32_t file_crc(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw Error("Cannot read file checksum: " + display_path(path));
    std::array<char, 65536> buffer{};
    std::uint32_t checksum = 0;
    while (in) {
        in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto count = in.gcount();
        if (count > 0) checksum = crc32(std::string_view(buffer.data(), static_cast<std::size_t>(count)), checksum);
    }
    if (!in.eof() || in.bad()) throw Error("Failed while reading file checksum");
    return checksum;
}

std::uint64_t checked_file_size(const fs::path& path) {
    std::error_code error;
    const auto size = fs::file_size(path, error);
    if (error) throw Error("Cannot inspect file size: " + display_path(path) + ": " + error.message());
    if (size > (std::numeric_limits<std::uint64_t>::max)()) throw Error("File size exceeds 64-bit limit");
    return static_cast<std::uint64_t>(size);
}

struct ExternalSorter::Impl {
    fs::path directory;
    std::vector<Posting> buffer;
    std::vector<std::vector<fs::path>> levels;
    std::size_t capacity;
    std::uint64_t sequence = 0;
    bool finished = false;
    std::vector<fs::path> final_runs;

    Impl(const fs::path& parent, const std::string& prefix, std::size_t bytes)
        : directory(private_work_dir(parent, prefix)),
          capacity((std::max)(std::size_t{1}, bytes / sizeof(Posting))) {
        buffer.reserve(capacity);
    }

    ~Impl() {
        if (!finished) {
            std::error_code ignored;
            fs::remove_all(directory, ignored);
        }
    }

    fs::path next_name() {
        if (sequence == (std::numeric_limits<std::uint64_t>::max)())
            throw Error("Temporary run sequence overflow");
        return directory / ("run-" + std::to_string(sequence++) + ".bin");
    }

    void carry(fs::path run, std::size_t level) {
        for (;;) {
            if (levels.size() <= level) levels.resize(level + 1);
            levels[level].push_back(std::move(run));
            if (levels[level].size() < merge_fan_in) return;
            const auto merged = next_name();
            merge_group(levels[level], merged);
            for (const auto& input : levels[level]) {
                std::error_code error;
                fs::remove(input, error);
                if (error) throw Error("Cannot remove merged sort run: " + error.message());
            }
            levels[level].clear();
            run = merged;
            ++level;
        }
    }

    void flush() {
        if (buffer.empty()) return;
        std::sort(buffer.begin(), buffer.end(), key_less);
        const auto path = next_name();
        BlockWriter writer(path);
        Posting accumulated = buffer.front();
        for (std::size_t i = 1; i < buffer.size(); ++i) {
            if (key_equal(accumulated, buffer[i])) combine(accumulated, buffer[i]);
            else {
                writer.add(accumulated);
                accumulated = buffer[i];
            }
        }
        writer.add(accumulated);
        writer.finish();
        buffer.clear();
        carry(path, 0);
    }
};

ExternalSorter::ExternalSorter(const fs::path& directory, std::string prefix, std::size_t bytes)
    : impl_(std::make_unique<Impl>(directory, prefix, bytes)) {}

ExternalSorter::~ExternalSorter() = default;

void ExternalSorter::add(Posting posting) {
    if (impl_->finished) throw Error("Cannot add to a finished external sorter");
    if (posting.frequency == 0) throw Error("Posting frequency must be positive");
    impl_->buffer.push_back(posting);
    if (impl_->buffer.size() == impl_->capacity) impl_->flush();
}

std::vector<fs::path> ExternalSorter::finish() {
    if (!impl_->finished) {
        impl_->flush();
        for (const auto& level : impl_->levels)
            impl_->final_runs.insert(impl_->final_runs.end(), level.begin(), level.end());
        impl_->finished = true;
    }
    return impl_->final_runs;
}

void ExternalSorter::merge_to(const std::vector<fs::path>& inputs, const fs::path& output,
                              const fs::path& directory) {
    std::error_code error;
    if (fs::exists(output, error)) throw Error("Refusing to overwrite an existing postings file");
    if (error) throw Error("Cannot inspect postings output path: " + error.message());
    TempDirectory work{private_work_dir(directory, "merge")};
    std::vector<fs::path> current = inputs;
    std::uint64_t sequence = 0;
    while (current.size() > merge_fan_in) {
        std::vector<fs::path> next;
        next.reserve(current.size() / merge_fan_in + (current.size() % merge_fan_in != 0 ? 1U : 0U));
        for (std::size_t start = 0; start < current.size();) {
            const auto length = (std::min)(merge_fan_in, current.size() - start);
            const std::vector<fs::path> group(current.begin() + static_cast<std::ptrdiff_t>(start),
                current.begin() + static_cast<std::ptrdiff_t>(start + length));
            const auto merged = work.path / ("merge-" + std::to_string(sequence++) + ".bin");
            merge_group(group, merged);
            next.push_back(merged);
            start += length;
        }
        // Intermediate files stay private until the final output is closed.
        // They are removed by work's destructor, including on any exception.
        current = std::move(next);
    }
    merge_group(current, output);
}

struct PostingReader::Impl {
    BlockReader reader;
    explicit Impl(const fs::path& path) : reader(path) {}
};

PostingReader::PostingReader(const fs::path& file) : impl_(std::make_unique<Impl>(file)) {}
PostingReader::~PostingReader() = default;
PostingReader::PostingReader(PostingReader&&) noexcept = default;
PostingReader& PostingReader::operator=(PostingReader&&) noexcept = default;

void PostingReader::lookup(std::uint64_t hash, const std::function<void(const Posting&)>& visitor) {
    if (!impl_) throw Error("Use of moved-from PostingReader");
    impl_->reader.lookup(hash, visitor);
}

std::uint64_t PostingReader::verify() {
    if (!impl_) throw Error("Use of moved-from PostingReader");
    return impl_->reader.verify();
}

std::uint64_t PostingReader::count() const {
    if (!impl_) throw Error("Use of moved-from PostingReader");
    return impl_->reader.count();
}
}
