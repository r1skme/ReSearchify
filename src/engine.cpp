#include "researchify/engine.hpp"
#include "researchify/disk.hpp"
#include "researchify/query.hpp"
#include "researchify/storage.hpp"
#include "researchify/literal_filter.hpp"
#include "query_internal.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <charconv>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <fstream>
#include <iomanip>
#include <limits>
#include <list>
#include <map>
#include <mutex>
#include <numeric>
#include <optional>
#include <queue>
#include <random>
#include <set>
#include <sstream>
#include <thread>
#include <unordered_map>

namespace researchify {
namespace {
using Clock = std::chrono::steady_clock;
constexpr std::uint64_t MiB = 1024 * 1024, SerialMask = (1ULL << 54) - 1;
constexpr std::uint64_t SegmentGenerationStride = 1000000;
constexpr std::size_t Buckets = 1024, IoBytes = 65536, MaxPath = 32768;
constexpr std::uint64_t EndDoc = (std::numeric_limits<std::uint64_t>::max)();
std::uint64_t checked_add(std::uint64_t a, std::uint64_t b) {
    if (b > (std::numeric_limits<std::uint64_t>::max)() - a) throw Error("E_INDEX_CORRUPT", "Integer overflow");
    return a + b;
}
std::size_t budget_bytes(std::size_t mb) {
    if (mb < 16 || mb > (std::numeric_limits<std::size_t>::max)() / MiB) throw Error("E_BUDGET", "Memory budget must be at least 16 MiB");
    return mb * static_cast<std::size_t>(MiB);
}
void checkpoint(const BuildOptions& o, const std::string& name) { if (o.checkpoint) o.checkpoint(name);
}
std::uint64_t unsigned_number(std::string_view s) {
    std::uint64_t v = 0;
    auto [p, e] = std::from_chars(s.data(), s.data() + s.size(), v);
    if (e != std::errc{} || p != s.data() + s.size() || s.empty()) throw Error("E_ARGUMENT", "Invalid unsigned integer");
    return v;
}
bool numbered(const std::string& s, std::string_view prefix, std::string_view suffix, std::uint64_t& id) {
    if (!s.starts_with(prefix) || !s.ends_with(suffix) || s.size() <= prefix.size() + suffix.size()) return false;
    auto text = std::string_view(s).substr(prefix.size(), s.size() - prefix.size() - suffix.size());
    try { id = unsigned_number(text);
    return id != 0 && text == std::to_string(id);
    } catch (const Error&) { return false;
    }
}
fs::path commit_path(const fs::path& dir, std::uint64_t gen) { return dir / ("commit-" + std::to_string(gen) + ".bin");
}
fs::path catalog_path(const fs::path& dir, std::uint64_t gen, std::size_t bucket) {
    return dir / ("catalog-" + std::to_string(gen) + "-" + std::to_string(bucket) + ".tbl");
}
fs::path segment_path(const fs::path& dir, std::uint64_t id) { return dir / ("segment-" + std::to_string(id));
}
std::vector<std::uint64_t> commits(const fs::path& dir) {
    std::vector<std::uint64_t> result;
    if (!fs::exists(dir)) return result;
    for (const auto& item : fs::directory_iterator(dir)) {
        std::uint64_t g = 0;
        if (numbered(path_to_utf8(item.path().filename()), "commit-", ".bin", g)) result.push_back(g);
        if (result.size() > 100000) throw Error("E_BUDGET", "Too many generations; run gc");
    }
    std::sort(result.rbegin(), result.rend());
    return result;
}
std::uint64_t new_generation(const fs::path& dir) {
    std::uint64_t maximum = 0;
    for (const auto& item : fs::directory_iterator(dir)) {
        auto name = path_to_utf8(item.path().filename());
        std::uint64_t g = 0;
        if (numbered(name, "commit-", ".bin", g) || numbered(name, "transaction-", "", g) || numbered(name, "commit-", ".bin.tmp", g)) maximum = (std::max)(maximum, g);
        if (numbered(name, "segment-", "", g)) maximum = (std::max)(maximum, g / SegmentGenerationStride);
        if (name.starts_with("catalog-")) {
            auto end = name.find('-', 8);
            if (end != std::string::npos) { try { maximum = (std::max)(maximum, unsigned_number(std::string_view(name).substr(8, end - 8)));
            } catch (const Error&) {} }
        }
    }
    if (maximum >= (std::numeric_limits<std::uint64_t>::max)() / SegmentGenerationStride - 1) throw Error("E_BUDGET", "Generation space exhausted");
    return maximum + 1;
}
class DirectoryLock {
    fs::path path_;
    bool held_ = false;
public:
    DirectoryLock(const fs::path& dir, const std::string& name, bool wait = false) : path_(dir / name) {
        const auto until = Clock::now() + std::chrono::seconds(5);
        for (;;) {
            std::error_code error;
            if (fs::create_directory(path_, error)) { held_ = true;
            return;
            }
            if (!wait || Clock::now() >= until || (error && error != std::errc::file_exists))
                throw Error("E_LOCKED", name + " is held or could not be created; recover only after stopping owners", true);
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }
    ~DirectoryLock() { if (held_) { std::error_code e;
    fs::remove(path_, e);
    } }
    DirectoryLock(const DirectoryLock&) = delete;
    DirectoryLock& operator=(const DirectoryLock&) = delete;
};
bool safe_relative(const std::string& value) {
    if (value.empty() || value.size() > MaxPath || !valid_utf8(value) || value.find('\0') != std::string::npos ||
        value.find('\\') != std::string::npos || value.find(':') != std::string::npos) return false;
    const auto p = path_from_utf8(value);
    if (p.is_absolute() || p.has_root_name() || p.has_root_directory()) return false;
    for (const auto& part : p) if (part.empty() || part == "." || part == "..") return false;
    return path_to_utf8(p) == value;
}
bool within(const fs::path& child, const fs::path& parent) {
    auto c = child.begin();
    for (auto p = parent.begin(); p != parent.end(); ++p, ++c) if (c == child.end() || *c != *p) return false;
    return true;
}
void ensure_source_path(const fs::path& root, const std::string& relative) {
    if (!safe_relative(relative)) throw Error("E_SCOPE", "Unsafe relative path");
    fs::path p = root;
    for (const auto& component : path_from_utf8(relative)) {
        p /= component;
        if (fs::is_symlink(fs::symlink_status(p))) throw Error("E_SCOPE", "Symlinks are not followed: " + relative);
    }
    if (!within(fs::weakly_canonical(p), root)) throw Error("E_SCOPE", "Path leaves workspace");
}
std::size_t bucket_of(std::string_view path) { return static_cast<std::size_t>(storage::term_hash(path) & (Buckets - 1));
}
std::size_t bucket_of(std::uint64_t id) { return static_cast<std::size_t>(id >> 54);
}
struct Record {
    std::string path;
    std::uint64_t id = 0, segment = 0, offset = 0, size = 0, lines = 0, tokens = 0;
    std::int64_t mtime = 0;
    storage::Fingerprint fingerprint;
    std::uint32_t crc = 0, max_line = 0;
};
std::string encode_record(const Record& r) {
    std::string out;
    disk::string(out, r.path);
    disk::varint(out, r.id);
    disk::varint(out, r.segment);
    disk::varint(out, r.offset);
    disk::varint(out, r.size);
    disk::u64(out, std::bit_cast<std::uint64_t>(r.mtime));
    disk::u64(out, r.fingerprint.a);
    disk::u64(out, r.fingerprint.b);
    disk::varint(out, r.crc);
    disk::varint(out, r.lines);
    disk::varint(out, r.tokens);
    disk::varint(out, r.max_line);
    return out;
}
Record decode_record(std::string_view value) {
    Record r;
    std::size_t at = 0;
    r.path = disk::get_string(value, at, MaxPath);
    r.id = disk::get_varint(value, at);
    r.segment = disk::get_varint(value, at);
    r.offset = disk::get_varint(value, at);
    r.size = disk::get_varint(value, at);
    r.mtime = std::bit_cast<std::int64_t>(disk::get_u64(value, at));
    r.fingerprint.a = disk::get_u64(value, at);
    r.fingerprint.b = disk::get_u64(value, at);
    auto crc = disk::get_varint(value, at);
    r.lines = disk::get_varint(value, at);
    r.tokens = disk::get_varint(value, at);
    auto line = disk::get_varint(value, at);
    if (at != value.size() || !safe_relative(r.path) || !(r.id & SerialMask) || r.id == EndDoc || !r.segment ||
        bucket_of(r.id) != bucket_of(r.path) || crc > 0xffffffffULL || line > 16 * MiB ||
        r.lines > r.size || r.tokens > r.size || (r.size == 0 && (r.lines || line)))
        throw Error("E_INDEX_CORRUPT", "Invalid document record");
    r.crc = static_cast<std::uint32_t>(crc);
    r.max_line = static_cast<std::uint32_t>(line);
    return r;
}
std::string content_id(const Record& r) {
    std::ostringstream s;
    s << std::hex << std::setfill('0') << std::setw(16) << r.fingerprint.a << std::setw(16) << r.fingerprint.b << std::setw(8) << r.crc;
    return s.str();
}
struct Manifest {
    fs::path root;
    std::uint64_t generation = 0, next_id = 1, files = 0, lines = 0, bytes = 0, tokens = 0;
    std::array<std::uint64_t, Buckets> catalogs{};
    std::map<std::uint64_t, std::uint64_t> segments;
    BuildOptions policy;
    std::string freshness = "unknown";
};
void validate_options(const BuildOptions& o) {
    (void)budget_bytes(o.memory_mb);
    if (!o.threads || o.threads > 256 || !o.max_line_bytes || o.max_line_bytes > 16 * MiB || !o.max_file_bytes ||
        o.max_file_bytes > static_cast<std::uint64_t>((std::numeric_limits<std::streamoff>::max)()) ||
        !o.segment_documents || o.segment_documents > 1000000 || (o.profile != "compact" && o.profile != "fast"))
        throw Error("E_ARGUMENT", "Invalid build options");
    if (o.exclude_dirs.size() + o.exclude_paths.size() > 4096) throw Error("E_ARGUMENT", "Too many exclusions");
    for (const auto& v : o.exclude_dirs) if (v.size() > 8192 || !valid_utf8(v)) throw Error("E_ARGUMENT", "Invalid directory exclusion");
    for (const auto& v : o.exclude_paths) if (v.size() > 8192 || !valid_utf8(v)) throw Error("E_ARGUMENT", "Invalid path exclusion");
}
std::string encode_manifest(const Manifest& m) {
    std::string s = "CSMAN2\r\n";
    for (auto v : {m.generation, m.next_id, m.files, m.lines, m.bytes, m.tokens}) disk::varint(s, v);
    disk::string(s, path_to_utf8(m.root));
    disk::string(s, m.freshness);
    for (auto v : m.catalogs) disk::varint(s, v);
    disk::varint(s, m.segments.size());
    for (auto [id, live] : m.segments) { disk::varint(s, id);
    disk::varint(s, live);
    }
    const auto& p = m.policy;
    for (auto v : {std::uint64_t(p.threads), std::uint64_t(p.memory_mb), p.max_file_bytes, std::uint64_t(p.max_line_bytes),
                   std::uint64_t(p.trust_mtime), std::uint64_t(p.segment_documents)}) disk::varint(s, v);
    disk::string(s, p.profile);
    for (const auto* list : {&p.exclude_dirs, &p.exclude_paths}) { disk::varint(s, list->size());
    for (const auto& x : *list) disk::string(s, x);
    }
    if (s.size() > 4 * MiB || m.segments.size() > 65536) throw Error("E_BUDGET", "Manifest exceeds format limits; compact or rebuild with larger segments");
    return s;
}
Manifest read_manifest(const fs::path& dir, std::uint64_t generation) {
    auto data = disk::read_file(commit_path(dir, generation), 4 * MiB);
    if (!data.starts_with("CSMAN2\r\n")) throw Error("E_FORMAT", "Not a v2 index; rebuild v1 into a NEW directory");
    Manifest m;
    std::size_t at = 8;
    m.generation = disk::get_varint(data, at);
    m.next_id = disk::get_varint(data, at);
    m.files = disk::get_varint(data, at);
    m.lines = disk::get_varint(data, at);
    m.bytes = disk::get_varint(data, at);
    m.tokens = disk::get_varint(data, at);
    auto root = disk::get_string(data, at, MaxPath);
    m.root = path_from_utf8(root);
    m.freshness = disk::get_string(data, at, 64);
    if (m.generation != generation || !m.next_id || m.next_id > SerialMask || m.files >= m.next_id ||
        !valid_utf8(root) || root.find('\0') != std::string::npos || !m.root.is_absolute() || m.lines > m.bytes || m.tokens > m.bytes)
        throw Error("E_INDEX_CORRUPT", "Invalid manifest header");
    for (auto& v : m.catalogs) { v = disk::get_varint(data, at);
    if (v > generation) throw Error("E_INDEX_CORRUPT", "Future catalog reference");
    }
    auto count = disk::get_varint(data, at);
    if (count > 65536 || count > m.files) throw Error("E_INDEX_CORRUPT", "Invalid segment count");
    std::uint64_t total = 0, previous = 0;
    for (std::uint64_t i = 0; i < count; ++i) {
        auto id = disk::get_varint(data, at), live = disk::get_varint(data, at);
        if (!live || id <= previous || id / SegmentGenerationStride > generation) throw Error("E_INDEX_CORRUPT", "Invalid segment descriptor");
        total = checked_add(total, live);
        m.segments.emplace(id, live);
        previous = id;
    }
    if (total != m.files) throw Error("E_INDEX_CORRUPT", "Manifest live counts disagree");
    auto& p = m.policy;
    p.threads = static_cast<std::size_t>(disk::get_varint(data, at));
    p.memory_mb = static_cast<std::size_t>(disk::get_varint(data, at));
    p.max_file_bytes = disk::get_varint(data, at);
    p.max_line_bytes = static_cast<std::size_t>(disk::get_varint(data, at));
    auto trust = disk::get_varint(data, at);
    if (trust > 1) throw Error("E_INDEX_CORRUPT", "Invalid metadata mode");
    p.trust_mtime = trust != 0;
    p.segment_documents = static_cast<std::size_t>(disk::get_varint(data, at));
    p.profile = disk::get_string(data, at, 32);
    for (auto* list : {&p.exclude_dirs, &p.exclude_paths}) {
        auto n = disk::get_varint(data, at);
        if (n > 4096) throw Error("E_INDEX_CORRUPT", "Exclusion count overflow");
        list->clear();
        for (std::uint64_t i = 0; i < n; ++i) list->push_back(disk::get_string(data, at, 8192));
    }
    if (at != data.size()) throw Error("E_INDEX_CORRUPT", "Trailing manifest bytes");
    try { validate_options(p);
    } catch (const Error& e) { throw Error("E_INDEX_CORRUPT", e.what());
    }
    return m;
}
struct Snapshot {
    fs::path directory, pin;
    Manifest manifest;
    std::shared_ptr<disk::PageCache> cache;
    std::size_t budget = 0;
    std::vector<std::string> warnings;
    mutable std::array<std::shared_ptr<disk::Table>, Buckets> catalogs;
    mutable std::mutex mutex;
    struct CachedRecord { std::shared_ptr<const Record> value; std::list<std::uint64_t>::iterator position; std::size_t bytes; };
    mutable std::mutex record_mutex;
    mutable std::list<std::uint64_t> recent_records;
    mutable std::unordered_map<std::uint64_t, CachedRecord> cached_records;
    mutable std::size_t record_bytes = 0;
    std::shared_ptr<const Record> cached_record(std::uint64_t id) const {
        {
            std::lock_guard lock(record_mutex);
            auto found = cached_records.find(id);
            if (found != cached_records.end()) {
                recent_records.splice(recent_records.begin(), recent_records, found->second.position);
                return found->second.value;
            }
        }
        auto value = by_id(id);
        if (!value) return {};
        auto record = std::make_shared<const Record>(std::move(*value));
        const auto bytes = sizeof(Record) + record->path.capacity() + 192;
        const auto maximum = (std::min)(budget / 16, std::size_t(16 * MiB));
        if (bytes > maximum) return record;
        std::lock_guard lock(record_mutex);
        if (auto existing = cached_records.find(id); existing != cached_records.end()) return existing->second.value;
        while (!recent_records.empty() && record_bytes + bytes > maximum) {
            auto last = recent_records.back();
            record_bytes -= cached_records.at(last).bytes;
            cached_records.erase(last);
            recent_records.pop_back();
        }
        recent_records.push_front(id);
        cached_records.emplace(id, CachedRecord{record, recent_records.begin(), bytes});
        record_bytes += bytes;
        return record;
    }
    bool pin_published = false;
    ~Snapshot() noexcept {
        if (pin.empty()) return;
        try {
            // Release under the same gate as pin creation and GC enumeration.
            // If the gate cannot be acquired, leaving a stale pin is safe.
            if (pin_published) {
                DirectoryLock gate(directory, "access.lock", true);
                std::error_code error;
                fs::remove_all(pin, error);
            } else {
                std::error_code error;
                fs::remove_all(pin, error);
            }
        } catch (...) {}
    }
    std::shared_ptr<disk::Table> table(std::size_t bucket) const {
        if (bucket >= Buckets) throw Error("E_INDEX_CORRUPT", "Catalog bucket out of bounds");
        if (!manifest.catalogs[bucket]) return {};
        std::lock_guard lock(mutex);
        if (!catalogs[bucket]) catalogs[bucket] = std::make_shared<disk::Table>(catalog_path(directory, manifest.catalogs[bucket], bucket), cache);
        return catalogs[bucket];
    }
    std::optional<Record> by_id(std::uint64_t id) const {
        auto t = table(bucket_of(id));
        if (!t) return {};
        auto v = t->get("I" + disk::ordered(id));
        if (!v) return {};
        auto r = decode_record(*v);
        if (r.id != id || !manifest.segments.contains(r.segment)) throw Error("E_INDEX_CORRUPT", "Document identity mismatch");
        return r;
    }
    std::optional<Record> by_path(const std::string& path) const {
        auto t = table(bucket_of(path));
        if (!t) return {};
        auto id = t->get("P" + path);
        if (!id) return {};
        auto r = by_id(disk::ordered(*id));
        if (!r || r->path != path) throw Error("E_INDEX_CORRUPT", "Path directory mismatch");
        return r;
    }
    void each(const std::function<void(const Record&)>& visitor) const {
        for (std::size_t b = 0; b < Buckets; ++b) if (auto t = table(b)) {
            t->each([&](auto key, auto value) {
                auto r = decode_record(value);
                if (key.size() != 9 || disk::ordered(key.substr(1)) != r.id || bucket_of(r.id) != b || !manifest.segments.contains(r.segment))
                    throw Error("E_INDEX_CORRUPT", "Catalog key mismatch");
                visitor(r);
            }, "I");
        }
    }
};
std::shared_ptr<Snapshot> open_snapshot(const fs::path& requested, std::size_t budget, std::uint64_t selected = 0) {
    if (!fs::is_directory(requested)) throw Error("E_INDEX_NOT_FOUND", "No index here; run researchify init .");
    auto dir = fs::canonical(requested);
    DirectoryLock gate(dir, "access.lock", true);
    auto generations = selected ? std::vector<std::uint64_t>{selected} : commits(dir);
    if (generations.empty()) throw Error("E_INDEX_NOT_FOUND", "No committed index; run init/create first");
    auto result = std::make_shared<Snapshot>();
    result->directory = dir;
    result->budget = budget;
    result->cache = std::make_shared<disk::PageCache>((std::min)(budget / 4, std::size_t(64 * MiB)));
    bool loaded = false;
    for (auto g : generations) {
        if (selected && !fs::exists(commit_path(dir, g))) throw Error("E_SNAPSHOT_GONE", "Requested snapshot is no longer retained");
        try { result->manifest = read_manifest(dir, g);
        loaded = true;
        break;
        }
        catch (const Error& e) {
            if (e.code() == "E_FORMAT" || e.code() == "E_BUDGET" || selected) throw;
            if (result->warnings.size() < 32) result->warnings.push_back("Ignored generation " + std::to_string(g) + ": " + e.what());
        }
    }
    if (!loaded) throw Error("E_INDEX_CORRUPT", "No valid committed manifest");
    fs::create_directories(dir / "pins");
    static std::atomic<std::uint64_t> sequence{0};
    std::random_device random;
    for (unsigned attempts = 0; attempts < 64; ++attempts) {
        auto name = "reader-" + std::to_string(random()) + "-" + std::to_string(++sequence) + "-" + std::to_string(Clock::now().time_since_epoch().count());
        auto p = dir / "pins" / name;
        if (fs::create_directory(p)) {
            result->pin = p;
            disk::write_file(p / "generation", std::to_string(result->manifest.generation));
            result->pin_published = true;
            return result;
        }
    }
    throw Error("E_IO", "Could not reserve reader pin");
}
std::int64_t timestamp(const fs::path& p) { return static_cast<std::int64_t>(fs::last_write_time(p).time_since_epoch().count());
}
struct SkipFile { std::string reason;
};
struct ReadInfo {
    storage::Fingerprint fingerprint;
    std::uint32_t crc = 0, max_line = 0;
    std::uint64_t lines = 0, bytes = 0;
};
ReadInfo stream_text(std::istream& in, std::uint64_t size, std::size_t maximum,
                     const std::function<void(std::string_view, std::uint64_t)>& callback,
                     std::ostream* copy = nullptr, SearchProfile* profile = nullptr) {
    // Only lines crossing an I/O boundary need an owning buffer. Most source
    // lines can be validated and consumed directly from the read buffer.
    std::array<char, IoBytes> buffer;
    std::string line;
    storage::Hasher hash;
    ReadInfo info;
    if (profile && !profile->enabled) profile = nullptr;
    auto duration = [](Clock::time_point begin) { return std::chrono::duration<double>(Clock::now() - begin).count(); };
    double callbacks = 0;
    auto emit = [&](std::string_view text) {
        ++info.lines;
        info.max_line = (std::max)(info.max_line, static_cast<std::uint32_t>(text.size()));
        if (!text.empty() && text.back() == '\r') text.remove_suffix(1);
        if (info.lines == 1 && text.starts_with("\xef\xbb\xbf")) text.remove_prefix(3);
        if (text.find('\0') != std::string_view::npos) throw SkipFile{"binary NUL byte"};
        if (!valid_utf8(text)) throw SkipFile{"invalid UTF-8"};
        if (profile) {
            auto begin = Clock::now();
            callback(text, info.lines);
            callbacks += duration(begin);
        } else callback(text, info.lines);
    };
    auto left = size;
    while (left) {
        auto take = static_cast<std::size_t>((std::min)(left, std::uint64_t(buffer.size())));
        const auto read_begin = profile ? Clock::now() : Clock::time_point{};
        in.read(buffer.data(), static_cast<std::streamsize>(take));
        if (!in) throw Error("E_IO", "File truncated or unreadable during scan");
        if (profile) {
            profile->content_read_seconds += duration(read_begin);
            ++profile->content_reads;
            profile->content_bytes += take;
        }
        std::string_view data(buffer.data(), take);
        const auto verify_begin = profile ? Clock::now() : Clock::time_point{};
        hash.add(data);
        info.crc = storage::crc32(data, info.crc);
        if (profile) profile->verification_seconds += duration(verify_begin);
        if (copy) { copy->write(data.data(), static_cast<std::streamsize>(data.size()));
        if (!*copy) throw Error("E_IO", "Cannot write snapshot content");
        }
        const auto text_begin = profile ? Clock::now() : Clock::time_point{};
        const auto callbacks_before = callbacks;
        std::size_t at = 0;
        while (at < data.size()) {
            auto end = data.find('\n', at);
            auto part = data.substr(at, end == data.npos ? data.size() - at : end - at);
            if (part.size() > maximum - line.size()) throw SkipFile{"line exceeds max-line limit"};
            if (line.empty() && (end != data.npos || left == take)) {
                emit(part);
            } else {
                line.append(part);
                if (end != data.npos || left == take) {
                    emit(line);
                    line.clear();
                }
            }
            if (end == data.npos) break;
            at = end + 1;
        }
        if (profile) profile->line_processing_seconds += (std::max)(0.0, duration(text_begin) - (callbacks - callbacks_before));
        left -= take;
    }
    info.fingerprint = hash.finish();
    info.bytes = size;
    if (profile) profile->matching_seconds += callbacks;
    return info;
}
struct Job { std::string path;
std::uint64_t size = 0;
std::int64_t mtime = 0;
std::optional<Record> saved;
};
ReadInfo read_source(const fs::path& root, const Job& job, std::size_t max_line,
                     const std::function<void(std::string_view, std::uint64_t)>& callback, std::ostream* copy = nullptr) {
    ensure_source_path(root, job.path);
    auto p = root / path_from_utf8(job.path);
    if (!fs::is_regular_file(fs::symlink_status(p)) || storage::checked_file_size(p) != job.size || timestamp(p) != job.mtime)
        throw Error("E_SOURCE_CHANGED", "Source changed before reading: " + job.path, true);
    std::ifstream in(p, std::ios::binary);
    if (!in) throw Error("E_IO", "Cannot read " + job.path);
    auto info = stream_text(in, job.size, max_line, callback, copy);
    ensure_source_path(root, job.path);
    if (in.peek() != std::char_traits<char>::eof() || in.bad() || storage::checked_file_size(p) != job.size || timestamp(p) != job.mtime)
        throw Error("E_SOURCE_CHANGED", "Source changed while reading: " + job.path, true);
    return info;
}
// Query/worker-local: neither the seekable streams nor their positions are
// shared. The caller's immutable Snapshot keeps every cached segment pinned.
// LRU eviction bounds open handles even after many incremental generations.
class ContentReader {
    struct Entry {
        std::uint64_t segment = 0, size = 0, next = EndDoc;
        std::ifstream input;
    };
    const Snapshot& snapshot_;
    std::size_t maximum_;
    SearchProfile* profile_;
    std::list<Entry> files_;
public:
    explicit ContentReader(const Snapshot& snapshot, std::size_t max_open = 16, SearchProfile* profile = nullptr)
        : snapshot_(snapshot), maximum_((std::max)(std::size_t(1), (std::min)(max_open, std::size_t(32)))),
          profile_(profile && profile->enabled ? profile : nullptr) {}
    ContentReader(const ContentReader&) = delete;
    ContentReader& operator=(const ContentReader&) = delete;
    ReadInfo read(const Record& r, const std::function<void(std::string_view, std::uint64_t)>& callback,
                  std::ostream* copy = nullptr) {
        const auto opened = profile_ ? Clock::now() : Clock::time_point{};
        auto found = std::find_if(files_.begin(), files_.end(), [&](const Entry& item) { return item.segment == r.segment; });
        if (found == files_.end()) {
            if (files_.size() == maximum_) files_.pop_back();
            const auto path = segment_path(snapshot_.directory, r.segment) / "content.bin";
            Entry entry;
            entry.segment = r.segment;
            entry.size = storage::checked_file_size(path);
            entry.input.open(path, std::ios::binary);
            if (!entry.input) throw Error("E_IO", "Cannot read snapshot content");
            entry.next = 0;
            files_.push_front(std::move(entry));
            found = files_.begin();
            if (profile_) ++profile_->content_opens;
        } else files_.splice(files_.begin(), files_, found);
        auto& entry = files_.front();
        if (r.offset > entry.size || r.size > entry.size - r.offset ||
            r.offset > static_cast<std::uint64_t>((std::numeric_limits<std::streamoff>::max)()))
            throw Error("E_INDEX_CORRUPT", "Snapshot content out of bounds");
        if (entry.next != r.offset || !entry.input) {
            entry.input.clear();
            entry.input.seekg(static_cast<std::streamoff>(r.offset));
            if (!entry.input) throw Error("E_IO", "Cannot seek snapshot content");
        }
        // A callback may throw after the stream has read ahead. Only publish
        // the new position when both consumption and integrity checks succeed.
        entry.next = EndDoc;
        if (profile_) profile_->content_open_seconds += std::chrono::duration<double>(Clock::now() - opened).count();
        ReadInfo info;
        try { info = stream_text(entry.input, r.size, (std::max)(std::size_t(1), std::size_t(r.max_line)), callback, copy, profile_);
        }
        catch (const SkipFile& e) { throw Error("E_INDEX_CORRUPT", "Corrupt snapshot content: " + e.reason);
        }
        if (info.fingerprint != r.fingerprint || info.crc != r.crc || info.lines != r.lines || info.max_line != r.max_line)
            throw Error("E_INDEX_CORRUPT", "Snapshot content identity mismatch: " + r.path);
        entry.next = r.offset + r.size;
        return info;
    }
};
ReadInfo read_content(const Snapshot& s, const Record& r,
                      const std::function<void(std::string_view, std::uint64_t)>& callback, std::ostream* copy = nullptr) {
    return ContentReader(s, 1).read(r, callback, copy);
}
// Optional, additive accelerator. Old segments (or tiny/saturated documents)
// have no signature and always fall back to exact content verification. The
// table uses the existing checked B+tree format and binds each versioned value
// to its document identity; it is never an authority for positive matches.
class LiteralLookup {
    const Snapshot& snapshot_;
    struct Segment {
        std::shared_ptr<disk::Table> table;
        disk::Table::Cursor cursor;
    };
    std::map<std::uint64_t, Segment> tables_;
public:
    explicit LiteralLookup(const Snapshot& snapshot) : snapshot_(snapshot) {}
    std::optional<LiteralFilter> get(const Record& record) {
        auto found = tables_.find(record.segment);
        if (found == tables_.end()) {
            if (tables_.size() >= 32) tables_.erase(tables_.begin());
            const auto path = segment_path(snapshot_.directory, record.segment) / "literal.tbl";
            std::shared_ptr<disk::Table> table;
            if (fs::exists(path)) table = std::make_shared<disk::Table>(path, snapshot_.cache);
            found = tables_.emplace(record.segment, Segment{std::move(table), {}}).first;
        }
        auto& segment = found->second;
        if (!segment.table) return {};
        std::optional<LiteralFilter> filter;
        segment.table->get(disk::ordered(record.id), segment.cursor, [&](std::string_view value) {
            if (value.size() != 157 || !value.starts_with("RSLF1")) throw Error("E_INDEX_CORRUPT", "Invalid literal-filter version or size");
            std::size_t at = 5;
            const auto a = disk::get_u64(value, at), b = disk::get_u64(value, at), crc = disk::get_u64(value, at);
            if (a != record.fingerprint.a || b != record.fingerprint.b || crc != record.crc)
                throw Error("E_INDEX_CORRUPT", "Literal-filter document identity mismatch");
            filter = LiteralFilter::decode(value.substr(at));
            if (!filter) throw Error("E_INDEX_CORRUPT", "Invalid literal-filter payload");
        });
        return filter;
    }
};
struct JobQueue {
    std::mutex mutex;
    std::condition_variable available, space;
    std::deque<Job> jobs;
    std::size_t bytes = 0, maximum = MiB;
    bool done = false, cancelled = false;
    bool profile = false;
    double producer_wait_seconds = 0;
    void push(Job job) {
        const auto started = profile ? Clock::now() : Clock::time_point{};
        std::unique_lock lock(mutex);
        auto cost = job.path.size() + 256;
        space.wait(lock, [&] { return cancelled || bytes + cost <= maximum || jobs.empty(); });
        if (profile) producer_wait_seconds += std::chrono::duration<double>(Clock::now() - started).count();
        if (cancelled) throw Error("E_CANCELLED", "Build worker failed");
        bytes += cost;
        jobs.push_back(std::move(job));
        lock.unlock();
        available.notify_one();
    }
    bool pop(Job& job, double* wait_seconds = nullptr) {
        const auto started = wait_seconds ? Clock::now() : Clock::time_point{};
        std::unique_lock lock(mutex);
        available.wait(lock, [&] { return cancelled || done || !jobs.empty(); });
        if (wait_seconds) *wait_seconds += std::chrono::duration<double>(Clock::now() - started).count();
        if (cancelled || jobs.empty()) return false;
        job = std::move(jobs.front());
        jobs.pop_front();
        bytes -= job.path.size() + 256;
        lock.unlock();
        space.notify_one();
        return true;
    }
    void close(bool cancel = false) {
        { std::lock_guard lock(mutex);
        done = true;
        cancelled = cancelled || cancel;
        }
        available.notify_all();
        space.notify_all();
    }
};
bool excluded(const std::string& path, const BuildOptions& o) {
    auto p = path_from_utf8(path);
    for (auto it = p.begin(); it != p.end(); ++it) {
        auto next = it;
        ++next;
        if (next != p.end() && std::find(o.exclude_dirs.begin(), o.exclude_dirs.end(), path_to_utf8(*it)) != o.exclude_dirs.end()) return true;
    }
    for (const auto& pattern : o.exclude_paths) if (match_path(pattern, path)) return true;
    return false;
}
struct WorkerReport { std::uint64_t added = 0, changed = 0, unchanged = 0, skipped = 0, hashed = 0, tokenized = 0;
std::vector<std::string> warnings;
BuildProfile profile;
};
// Each update worker owns its cursors. Tables share the immutable snapshot and
// its accounted page cache; weak cursors cannot keep evicted leaves resident.
// Keep path and record cursors separate because both key namespaces share a
// catalog. A fixed number of lightweight slots bounds per-worker metadata.
class BuildCatalogLookup {
    struct Shard {
        std::shared_ptr<disk::Table> table;
        disk::Table::Cursor paths, records;
    };
    const Snapshot& snapshot_;
    std::array<Shard, Buckets> shards_;
public:
    // Expired weak owners can retain make_shared control/object storage after
    // entries have been freed. Budget that bounded overhead as well as slots;
    // live decoded entries remain in the snapshot's shared PageCache budget.
    static std::size_t working_bytes() {
        return sizeof(std::array<Shard, Buckets>) + 2 * Buckets * disk::Table::Cursor::retained_owner_bytes();
    }
    explicit BuildCatalogLookup(const Snapshot& snapshot) : snapshot_(snapshot) {}
    std::optional<Record> by_path(const std::string& path) {
        const auto bucket = bucket_of(path);
        if (!snapshot_.manifest.catalogs[bucket]) return {};
        auto& shard = shards_[bucket];
        if (!shard.table) shard.table = snapshot_.table(bucket);
        std::optional<std::uint64_t> id;
        shard.table->get("P" + path, shard.paths, [&](std::string_view value) { id = disk::ordered(value); });
        if (!id) return {};
        if (bucket_of(*id) != bucket) throw Error("E_INDEX_CORRUPT", "Path directory bucket mismatch");
        std::optional<Record> result;
        shard.table->get("I" + disk::ordered(*id), shard.records, [&](std::string_view value) { result = decode_record(value); });
        if (!result || result->id != *id || result->path != path || !snapshot_.manifest.segments.contains(result->segment))
            throw Error("E_INDEX_CORRUPT", "Path directory document identity mismatch");
        return result;
    }
};
class BuildStageTimer {
    double* destination_;
    Clock::time_point started_;
public:
    explicit BuildStageTimer(double* destination) : destination_(destination),
        started_(destination ? Clock::now() : Clock::time_point{}) {}
    ~BuildStageTimer() {
        if (destination_) *destination_ += std::chrono::duration<double>(Clock::now() - started_).count();
    }
};
// Query keys carry a namespace byte. Lookup can hash the existing token view
// without constructing the owning, prefixed string for every occurrence.
struct PostingKeyView {
    char kind;
    std::string_view text;
};
struct PostingKeyHash {
    using is_transparent = void;
    std::size_t operator()(PostingKeyView value) const noexcept {
        return std::hash<std::string_view>{}(value.text) ^ static_cast<unsigned char>(value.kind);
    }
    std::size_t operator()(const std::string& value) const noexcept {
        return (*this)(PostingKeyView{value.front(), std::string_view(value).substr(1)});
    }
};
struct PostingKeyEqual {
    using is_transparent = void;
    bool operator()(const std::string& a, const std::string& b) const noexcept { return a == b; }
    bool operator()(const std::string& a, PostingKeyView b) const noexcept {
        return a.front() == b.kind && std::string_view(a).substr(1) == b.text;
    }
    bool operator()(PostingKeyView a, const std::string& b) const noexcept { return (*this)(b, a); }
};
BuildReport build(const fs::path& source, const fs::path& requested, const BuildOptions& options, bool creating, bool compacting = false) {
    const auto started = Clock::now();
    const auto stage_start = [&] { return options.profile_stages ? Clock::now() : Clock::time_point{}; };
    const auto stage_elapsed = [&](Clock::time_point begin) {
        return options.profile_stages ? std::chrono::duration<double>(Clock::now() - begin).count() : 0.0;
    };
    validate_options(options);
    auto budget = budget_bytes(options.memory_mb);
    fs::create_directories(requested);
    auto directory = fs::canonical(requested);
    DirectoryLock writer(directory, "writer.lock");
    checkpoint(options, "after_lock");
    std::shared_ptr<Snapshot> old;
    Manifest next;
    if (creating) {
        if (!commits(directory).empty()) throw Error("E_EXISTS", "Index already exists; use update or a NEW directory");
        next.root = fs::canonical(source);
    } else { old = open_snapshot(directory, budget);
    next = old->manifest;
    }
    if (path_to_utf8(next.root).size() > MaxPath) throw Error("E_ARGUMENT", "UTF-8 workspace path exceeds format limit");
    if (!compacting && !fs::is_directory(next.root)) throw Error("E_IO", "Source root is unavailable");
    if (within(next.root, directory)) throw Error("E_SCOPE", "Source root must not be inside index directory");
    if (old && !compacting && options.profile != old->manifest.policy.profile)
        throw Error("E_ARGUMENT", "Changing index profile requires rebuilding into a NEW index");
    next.policy = options;
    next.policy.checkpoint = {};
    next.policy.changed_only = false;
    next.policy.changed_files.clear();
    next.policy.profile_stages = false;
    next.generation = new_generation(directory);
    next.freshness = options.changed_only ? "known-paths-only" : options.trust_mtime ? "metadata-scan" : "strict-scan";
    auto transaction = directory / ("transaction-" + std::to_string(next.generation));
    if (!fs::create_directory(transaction)) throw Error("E_EXISTS", "Transaction already exists");
    auto words = old && !options.changed_only && !compacting ? static_cast<std::size_t>((old->manifest.next_id + 63) / 64) : 0;
    if (words > budget / 64) throw Error("E_BUDGET", "Seen-document map exceeds budget");
    std::vector<std::uint64_t> seen(words, 0);
    // Byte budgets use size_t consistently: macOS defines uint64_t differently
    // from size_t, unlike the Linux toolchain used for earlier builds.
    constexpr auto MemoryMiB = static_cast<std::size_t>(MiB);
    const std::size_t fixed = (old && !compacting && !options.changed_only ? 12 : 8) * MemoryMiB + words * 8
        + (old ? (std::min)(budget / 4, std::size_t(64 * MiB)) : 0);
    const std::size_t gram_memory = options.profile == "fast" ? 3 * MemoryMiB : 0;
    const std::size_t literal_memory = options.profile == "compact" ? MemoryMiB / 4 : 0;
    const std::size_t catalog_memory = old && !compacting ? BuildCatalogLookup::working_bytes() : 0;
    const std::size_t per_worker = 2 * options.max_line_bytes + 3 * MemoryMiB + gram_memory + literal_memory + catalog_memory;
    const auto requested_workers = options.changed_only
        ? (std::min)(options.threads, (std::max)(std::size_t(1), options.changed_files.size())) : options.threads;
    std::size_t workers = (std::min)(requested_workers, (std::max)(std::size_t(1), budget > fixed ? (budget - fixed) / per_worker : 0));
    if (fixed + per_worker > budget) throw Error("E_BUDGET", "Memory budget too small for line buffers and metadata");
    auto sorter_budget = (std::min)(std::size_t(4 * MemoryMiB),
        (budget - fixed) / workers - 2 * options.max_line_bytes - 2 * MemoryMiB - gram_memory - literal_memory - catalog_memory);
    sorter_budget = (std::max)(std::size_t(MemoryMiB / 4), sorter_budget);
    disk::Sorter deltas(transaction / "changes", 2 * MiB, [](std::string&, const std::string&) { throw Error("E_ARGUMENT", "Duplicate changed document"); });
    std::mutex deltas_mutex, failure_mutex, segments_mutex;
    std::exception_ptr failure;
    auto delta = [&](std::uint64_t id, const std::optional<Record>& r) {
        std::lock_guard lock(deltas_mutex);
        deltas.add(disk::ordered(id), r ? encode_record(*r) : std::string());
    };
    std::atomic<std::uint64_t> serial{next.next_id}, segment_serial{0};
    std::vector<std::uint64_t> new_segments;
    std::vector<WorkerReport> reports(workers);
    JobQueue queue;
    queue.profile = options.profile_stages;
    // Apple libc++ does not expose std::jthread on all supported macOS SDKs.
    // Keep the same explicit join below, and close the queue before joining
    // during stack unwinding (including a failed thread creation).
    std::vector<std::thread> pool;
    struct PoolJoiner {
        JobQueue& queue;
        std::vector<std::thread>& threads;
        ~PoolJoiner() {
            if (std::any_of(threads.begin(), threads.end(), [](const std::thread& thread) { return thread.joinable(); }))
                queue.close(true);
            for (auto& thread : threads) if (thread.joinable()) thread.join();
        }
    } joiner{queue, pool};
    pool.reserve(workers);
    auto fail = [&] { std::lock_guard lock(failure_mutex);
    if (!failure) failure = std::current_exception();
    queue.close(true);
    };
    auto run_worker = [&](std::size_t worker) {
            try {
                auto& report = reports[worker];
                report.profile.enabled = options.profile_stages;
                std::unique_ptr<disk::Sorter> postings;
                std::unique_ptr<disk::Sorter> literals;
                std::ofstream blob;
                fs::path part;
                std::uint64_t segment = 0, documents = 0, blob_bytes = 0;
                auto finish_segment = [&] {
                    if (!postings) return;
                    const auto finish_started = stage_start();
                    disk::finish(blob, part / "content.bin");
                    disk::PostingWriter output(part);
                    postings->finish([&](auto k, auto v) { output.add(k, v); });
                    output.finish();
                    postings.reset();
                    fs::remove_all(part / "sort");
                    if (literals) {
                        disk::TableWriter literal_table(part / "literal.tbl");
                        literals->finish([&](auto key, auto value) { literal_table.add(key, value); });
                        literal_table.finish();
                        literals.reset();
                        fs::remove_all(part / "literal-sort");
                    }
                    report.profile.posting_write_seconds += stage_elapsed(finish_started);
                    std::lock_guard lock(segments_mutex);
                    new_segments.push_back(segment);
                };
                auto start_segment = [&] {
                    if (postings && documents < options.segment_documents && blob_bytes < 256 * MiB) return;
                    finish_segment();
                    auto n = ++segment_serial;
                    if (n >= SegmentGenerationStride) throw Error("E_BUDGET", "Too many segments in one generation");
                    segment = next.generation * SegmentGenerationStride + n;
                    part = segment_path(transaction, segment);
                    fs::create_directory(part);
                    blob.open(part / "content.bin", std::ios::binary | std::ios::trunc);
                    if (!blob) throw Error("E_IO", "Cannot create snapshot content");
                    postings = std::make_unique<disk::Sorter>(part / "sort", sorter_budget, disk::combine_posting);
                    documents = 0;
                    blob_bytes = 0;
                };
                std::vector<std::uint64_t> gram_bits;
                std::vector<std::uint32_t> gram_words;
                // Preserve the bounded bucket allocation across small documents.
                // Entries still leave the aggregate at every spill/document.
                std::unordered_map<std::string, disk::Posting, PostingKeyHash, PostingKeyEqual> aggregate;
                aggregate.reserve(512);
                std::unique_ptr<ContentReader> saved_content;
                if (compacting) saved_content = std::make_unique<ContentReader>(*old,
                    (std::max)(std::size_t(1), (std::min)(std::size_t(16), std::size_t(128) / workers)));
                std::unique_ptr<BuildCatalogLookup> catalogs;
                if (old && !compacting) catalogs = std::make_unique<BuildCatalogLookup>(*old);
                Job job;
                while (queue.pop(job, options.profile_stages ? &report.profile.queue_wait_seconds : nullptr)) {
                    std::optional<Record> prior;
                    {
                        BuildStageTimer timer(options.profile_stages ? &report.profile.metadata_lookup_seconds : nullptr);
                        prior = job.saved ? job.saved : catalogs ? catalogs->by_path(job.path) : std::optional<Record>();
                    }
                    if (prior && !seen.empty()) {
                        auto id = prior->id & SerialMask;
                        if (id >= old->manifest.next_id) throw Error("E_INDEX_CORRUPT", "Document serial out of bounds");
                        std::atomic_ref<std::uint64_t>(seen[static_cast<std::size_t>(id / 64)]).fetch_or(1ULL << (id % 64), std::memory_order_relaxed);
                    }
                    try {
                        if (job.size > options.max_file_bytes) throw SkipFile{"file exceeds max-file limit"};
                        if (!compacting && prior && options.trust_mtime && prior->size == job.size && prior->mtime == job.mtime && prior->max_line <= options.max_line_bytes) {
                            ++report.unchanged;
                            continue;
                        }
                        ReadInfo first;
                        if (!compacting) {
                            ++report.hashed;
                            checkpoint(options, "before_source_read");
                            {
                                BuildStageTimer timer(options.profile_stages ? &report.profile.validation_seconds : nullptr);
                                first = read_source(next.root, job, options.max_line_bytes, [](auto, auto) {});
                            }
                            if (prior && prior->size == job.size && prior->fingerprint == first.fingerprint && prior->crc == first.crc) {
                                if (prior->mtime != job.mtime) { prior->mtime = job.mtime;
                                delta(prior->id, prior);
                                }
                                ++report.unchanged;
                                continue;
                            }
                        }
                        start_segment();
                        Record file;
                        file.path = job.path;
                        file.id = prior ? prior->id : 0;
                        if (!file.id) {
                            auto n = serial.fetch_add(1);
                            if (!n || n >= SerialMask) throw Error("E_BUDGET", "Document ID space exhausted");
                            file.id = (std::uint64_t(bucket_of(file.path)) << 54) | n;
                        }
                        file.segment = segment;
                        file.offset = blob_bytes;
                        file.size = job.size;
                        file.mtime = job.mtime;
                        aggregate.clear();
                        std::size_t aggregate_bytes = 0;
                        const auto document_suffix = std::string(1, '\0') + disk::ordered(file.id);
                        auto spill = [&] {
                            for (const auto& [key, value] : aggregate) postings->add(key + document_suffix, disk::posting_value(value));
                            aggregate.clear();
                            aggregate_bytes = 0;
                        };
                        auto add = [&](char kind, std::string_view token, std::uint64_t line, std::uint64_t column) {
                            auto found = aggregate.find(PostingKeyView{kind, token});
                            if (found == aggregate.end()) {
                                std::string key;
                                key.reserve(token.size() + 1);
                                key.push_back(kind);
                                key.append(token);
                                aggregate_bytes += key.size() + 144;
                                aggregate.emplace(std::move(key), disk::Posting{0, 1, line, column});
                            } else found->second.frequency = checked_add(found->second.frequency, 1);
                            if (aggregate_bytes >= MiB / 2) spill();
                        };
                        // Strict no-op updates and idle workers do not need a
                        // 2 MiB trigram bitmap. Allocate it only when capturing
                        // the first changed/new document in this worker.
                        if (options.profile == "fast" && gram_bits.empty()) gram_bits.resize(1U << 18);
                        for (auto word : gram_words) gram_bits[word] = 0;
                        gram_words.clear();
                        // Small documents are cheaper to verify directly. This
                        // optional sidecar adds no entries for them or for fast
                        // indexes, which already have byte-trigram postings.
                        const bool filter_content = options.profile == "compact" && file.size >= 512;
                        LiteralFilter literal_filter;
                        auto tokenize = [&](std::string_view text, std::uint64_t line) {
                            if (filter_content) literal_filter.add_line(text);
                            for_each_token(text, [&](Token token) {
                                ++file.tokens;
                                // The query grammar permits at most 8192 bytes per whole expression.
                                if (token.text.size() <= 8192) add('\2', token.text, line, token.column + 1);
                            });
                            if (options.profile == "fast" && text.size() >= 3) {
                                for (std::size_t i = 0; i + 3 <= text.size(); ++i) {
                                    const auto gram = (std::uint32_t(static_cast<unsigned char>(text[i])) << 16) |
                                        (std::uint32_t(static_cast<unsigned char>(text[i + 1])) << 8) |
                                        std::uint32_t(static_cast<unsigned char>(text[i + 2]));
                                    const auto word = gram >> 6;
                                    const auto mask = std::uint64_t(1) << (gram & 63U);
                                    if (gram_bits[word] & mask) continue;
                                    if (!gram_bits[word]) gram_words.push_back(word);
                                    gram_bits[word] |= mask;
                                    add('\1', text.substr(i, 3), line, i + 1);
                                }
                            }
                        };
                        ReadInfo info;
                        try {
                            BuildStageTimer timer(options.profile_stages ? &report.profile.capture_seconds : nullptr);
                            info = compacting ? saved_content->read(*prior, tokenize, &blob) : read_source(next.root, job, options.max_line_bytes, tokenize, &blob);
                        } catch (const SkipFile& e) { throw Error("E_SOURCE_CHANGED", "Source changed after validation: " + job.path + ": " + e.reason, true);
                        }
                        if (!compacting && (info.fingerprint != first.fingerprint || info.crc != first.crc))
                            throw Error("E_SOURCE_CHANGED", "Source changed between validation and capture: " + job.path, true);
                        spill();
                        file.fingerprint = info.fingerprint;
                        file.crc = info.crc;
                        file.lines = info.lines;
                        file.max_line = info.max_line;
                        if (filter_content) {
                            auto mask = literal_filter.encode();
                            // Saturated signatures are not selective. Missing
                            // entries deliberately mean 'verify this document'.
                            if (!std::all_of(mask.begin(), mask.end(), [](char c) { return static_cast<unsigned char>(c) == 255U; })) {
                                if (!literals) literals = std::make_unique<disk::Sorter>(part / "literal-sort", literal_memory,
                                    [](std::string&, const std::string&) { throw Error("E_INDEX_CORRUPT", "Duplicate literal signature"); });
                                std::string value = "RSLF1";
                                disk::u64(value, file.fingerprint.a);
                                disk::u64(value, file.fingerprint.b);
                                disk::u64(value, file.crc);
                                value += mask;
                                literals->add(disk::ordered(file.id), std::move(value));
                            }
                        }
                        blob_bytes = checked_add(blob_bytes, file.size);
                        ++documents;
                        ++report.tokenized;
                        if (prior) ++report.changed;
                        else ++report.added;
                        delta(file.id, file);
                    } catch (const SkipFile& e) {
                        ++report.skipped;
                        if (prior) delta(prior->id, {});
                        if (report.warnings.size() < 8) report.warnings.push_back(job.path + ": " + e.reason);
                    }
                }
                finish_segment();
            } catch (...) { fail();
            }
    };
    // This producer can enqueue at most one job, so it cannot block waiting
    // for a consumer. Keep one-file editor updates on the caller's thread.
    // Larger supplied lists still start consumers before bounded queue pushes.
    const bool inline_worker = options.changed_only && options.changed_files.size() <= 1 && workers == 1;
    if (!inline_worker) for (std::size_t worker = 0; worker < workers; ++worker)
        pool.emplace_back([&, worker] { run_worker(worker); });
    // Group a bounded window of update jobs by metadata shard. This avoids
    // random page-cache thrashing without materializing the whole file tree.
    std::vector<Job> pending_jobs;
    std::size_t pending_bytes = 0;
    auto flush_jobs = [&] {
        std::sort(pending_jobs.begin(), pending_jobs.end(), [](const Job& a, const Job& b) {
            auto x = bucket_of(a.path), y = bucket_of(b.path);
            return x < y || (x == y && a.path < b.path);
        });
        for (auto& job : pending_jobs) queue.push(std::move(job));
        pending_jobs.clear();
        pending_bytes = 0;
    };
    auto enqueue = [&](Job job) {
        if (!old || compacting || options.changed_only) { queue.push(std::move(job)); return; }
        pending_bytes += sizeof(Job) + job.path.size() + 64;
        pending_jobs.push_back(std::move(job));
        if (pending_bytes >= 2 * MiB || pending_jobs.size() >= 8192) flush_jobs();
    };
    const auto enumeration_started = stage_start();
    try {
        if (compacting) old->each([&](const Record& r) { queue.push({r.path, r.size, r.mtime, r}); });
        else if (options.changed_only) {
            std::set<std::string> paths;
            for (const auto& path : options.changed_files) {
                if (!safe_relative(path) || !paths.insert(path).second) throw Error("E_ARGUMENT", "Changed paths must be unique safe relative filenames");
                if (paths.size() > budget / 512) throw Error("E_BUDGET", "Changed path list exceeds memory budget");
                ensure_source_path(next.root, path);
                auto full = next.root / path_from_utf8(path);
                if (!fs::exists(full) || excluded(path, options)) { if (old) if (auto r = old->by_path(path)) delta(r->id, {});
                continue;
                }
                if (!fs::is_regular_file(fs::symlink_status(full))) throw Error("E_ARGUMENT", "Changed path is not a regular file: " + path);
                queue.push({path, storage::checked_file_size(full), timestamp(full), {}});
            }
        } else {
            for (fs::recursive_directory_iterator it(next.root), end; it != end; ++it) {
                const auto status = it->symlink_status();
                if (fs::is_symlink(status)) { it.disable_recursion_pending();
                continue;
                }
                const auto name = path_to_utf8(it->path().filename());
                if (fs::is_directory(status)) {
                    if (std::find(options.exclude_dirs.begin(), options.exclude_dirs.end(), name) != options.exclude_dirs.end() || fs::equivalent(it->path(), directory)) it.disable_recursion_pending();
                    continue;
                }
                if (!fs::is_regular_file(status)) continue;
                auto relative = path_to_utf8(it->path().lexically_relative(next.root));
                if (!safe_relative(relative)) throw Error("E_SCOPE", "Unsupported source path: " + relative);
                if (!excluded(relative, options)) enqueue({relative, it->file_size(), timestamp(it->path()), {}});
            }
        }
        flush_jobs();
        checkpoint(options, "after_scan");
        queue.close();
    } catch (...) { fail();
    }
    const auto enumeration_seconds = stage_elapsed(enumeration_started);
    if (inline_worker && !failure && !queue.jobs.empty()) run_worker(0);
    for (auto& thread : pool) thread.join();
    if (failure) std::rethrow_exception(failure);
    if (old && !compacting && !options.changed_only) {
        old->each([&](const Record& r) { auto id = r.id & SerialMask;
            if (id >= old->manifest.next_id) throw Error("E_INDEX_CORRUPT", "Serial out of range");
            if (!((seen[static_cast<std::size_t>(id / 64)] >> (id % 64)) & 1)) delta(r.id, {});
        });
    }
    BuildReport report;
    report.profile.enabled = options.profile_stages;
    report.profile.enumeration_seconds = enumeration_seconds;
    report.profile.queue_wait_seconds = queue.producer_wait_seconds;
    report.generation = next.generation;
    report.workers = workers;
    report.memory_budget_bytes = budget;
    report.global_scan = !options.changed_only;
    if (old) report.warnings = old->warnings;
    if (options.changed_only) report.warnings.push_back("Only supplied paths were checked; other workspace changes may be absent");
    for (const auto& r : reports) {
        report.profile.queue_wait_seconds += r.profile.queue_wait_seconds;
        report.profile.metadata_lookup_seconds += r.profile.metadata_lookup_seconds;
        report.profile.validation_seconds += r.profile.validation_seconds;
        report.profile.capture_seconds += r.profile.capture_seconds;
        report.profile.posting_write_seconds += r.profile.posting_write_seconds;
        report.added += r.added;
        report.changed += r.changed;
        report.unchanged += r.unchanged;
        report.skipped += r.skipped;
        report.hashed += r.hashed;
        report.tokenized += r.tokenized;
        for (const auto& w : r.warnings) if (report.warnings.size() < 64) report.warnings.push_back(w);
    }
    for (auto id : new_segments) fs::rename(segment_path(transaction, id), segment_path(directory, id));
    checkpoint(options, "after_segments");
    const auto catalog_started = stage_start();
    std::size_t active_bucket = Buckets;
    std::map<std::uint64_t, Record> records;
    std::size_t record_memory = 0;
    bool modified = false;
    auto save_bucket = [&] {
        if (active_bucket == Buckets) return;
        if (records.empty()) { next.catalogs[active_bucket] = 0;
        return;
        }
        auto p = catalog_path(directory, next.generation, active_bucket);
        disk::TableWriter writer_table(p);
        std::vector<const Record*> by_path;
        by_path.reserve(records.size());
        for (const auto& [id, r] : records) { writer_table.add("I" + disk::ordered(id), encode_record(r));
        by_path.push_back(&r);
        }
        std::sort(by_path.begin(), by_path.end(), [](auto a, auto b) { return a->path < b->path; });
        for (auto r : by_path) writer_table.add("P" + r->path, disk::ordered(r->id));
        writer_table.finish();
        next.catalogs[active_bucket] = next.generation;
    };
    auto adjust = [&](const Record& r, bool adding) {
        if (adding) {
            next.files = checked_add(next.files, 1);
            next.lines = checked_add(next.lines, r.lines);
            next.bytes = checked_add(next.bytes, r.size);
            next.tokens = checked_add(next.tokens, r.tokens);
            ++next.segments[r.segment];
        } else {
            if (!next.files || next.lines < r.lines || next.bytes < r.size || next.tokens < r.tokens || !next.segments.contains(r.segment) || !next.segments.at(r.segment))
                throw Error("E_INDEX_CORRUPT", "Manifest accounting underflow");
            --next.files;
            next.lines -= r.lines;
            next.bytes -= r.size;
            next.tokens -= r.tokens;
            if (--next.segments[r.segment] == 0) next.segments.erase(r.segment);
        }
    };
    deltas.finish([&](auto key, auto value) {
        auto id = disk::ordered(key);
        auto b = bucket_of(id);
        modified = true;
        if (b != active_bucket) {
            save_bucket();
            records.clear();
            record_memory = 0;
            active_bucket = b;
            if (old) if (auto t = old->table(b)) t->each([&](auto, auto v) {
                auto r = decode_record(v);
                record_memory += sizeof(Record) + r.path.size() + 128;
                if (record_memory > budget / 8) throw Error("E_BUDGET", "Catalog shard exceeds working budget");
                if (!records.emplace(r.id, std::move(r)).second) throw Error("E_INDEX_CORRUPT", "Duplicate catalog id");
            }, "I");
        }
        auto prior = records.find(id);
        if (prior != records.end()) { adjust(prior->second, false);
        record_memory -= sizeof(Record) + prior->second.path.size() + 128;
        records.erase(prior);
        }
        if (!value.empty()) {
            auto r = decode_record(value);
            if (r.id != id) throw Error("E_INDEX_CORRUPT", "Delta id mismatch");
            record_memory += sizeof(Record) + r.path.size() + 128;
            if (record_memory > budget / 8) throw Error("E_BUDGET", "Catalog shard exceeds budget");
            adjust(r, true);
            records.emplace(id, std::move(r));
        } else ++report.removed;
    });
    save_bucket();
    report.profile.catalog_seconds = stage_elapsed(catalog_started);
    next.next_id = serial.load();
    report.files = next.files;
    report.lines = next.lines;
    report.bytes = next.bytes;
    report.segments = next.segments.size();
    if (!creating && !modified && old->warnings.empty()) {
        // Policy changes are published even when no file content changed.
        auto same = next;
        same.generation = old->manifest.generation;
        same.freshness = old->manifest.freshness;
        if (encode_manifest(same) == encode_manifest(old->manifest)) {
            const auto commit_started = stage_start();
            fs::remove_all(transaction);
            report.profile.commit_seconds = stage_elapsed(commit_started);
            report.generation = old->manifest.generation;
            report.seconds = std::chrono::duration<double>(Clock::now() - started).count();
            return report;
        }
    }
    checkpoint(options, "after_catalog");
    const auto commit_started = stage_start();
    auto temporary = commit_path(directory, next.generation);
    temporary += ".tmp";
    disk::write_file(temporary, encode_manifest(next));
    checkpoint(options, "before_commit");
    fs::rename(temporary, commit_path(directory, next.generation));
    checkpoint(options, "after_commit");
    fs::remove_all(transaction);
    report.profile.commit_seconds = stage_elapsed(commit_started);
    report.seconds = std::chrono::duration<double>(Clock::now() - started).count();
    return report;
}
// One small decoded catalog shard is sufficient for ordered postings access.
// An LRU prevents interleaved segment cursors from decoding it repeatedly.
class DocumentLookup {
    const Snapshot& snapshot_;
    struct Shard { std::vector<Record> records;
    std::size_t bytes = 0;
    std::uint64_t used = 0;
    };
    std::map<std::size_t, Shard> shards_;
    std::size_t used_ = 0, maximum_;
    std::uint64_t clock_ = 0;
    bool sparse_ = false;
    std::shared_ptr<const Record> held_;
public:
    void set_sparse(bool value) { sparse_ = value; }
    DocumentLookup(const Snapshot& s, std::size_t maximum) : snapshot_(s), maximum_(maximum) {}
    const std::vector<Record>& shard(std::size_t bucket) {
        auto it = shards_.find(bucket);
        if (it != shards_.end()) { it->second.used = ++clock_;
        return it->second.records;
        }
        Shard sh;
        if (auto t = snapshot_.table(bucket)) t->each([&](auto key, auto value) {
            auto r = decode_record(value);
            if (key.size() != 9 || disk::ordered(key.substr(1)) != r.id || bucket_of(r.id) != bucket || !snapshot_.manifest.segments.contains(r.segment))
                throw Error("E_INDEX_CORRUPT", "Invalid catalog lookup");
            sh.bytes += 2 * sizeof(Record) + r.path.size() + 64;
            if (sh.bytes > maximum_) throw Error("E_BUDGET", "Catalog shard exceeds decoded-cache budget");
            sh.records.push_back(std::move(r));
        }, "I");
        while (!shards_.empty() && used_ + sh.bytes > maximum_) {
            auto victim = std::min_element(shards_.begin(), shards_.end(), [](const auto& a, const auto& b) { return a.second.used < b.second.used; });
            used_ -= victim->second.bytes;
            shards_.erase(victim);
        }
        sh.used = ++clock_;
        used_ += sh.bytes;
        return shards_.emplace(bucket, std::move(sh)).first->second.records;
    }
    const Record* get(std::uint64_t id) {
        if (sparse_) {
            held_ = snapshot_.cached_record(id);
            return held_.get();
        }
        const auto& rows = shard(bucket_of(id));
        auto found = std::lower_bound(rows.begin(), rows.end(), id, [](const Record& r, std::uint64_t x) { return r.id < x; });
        return found != rows.end() && found->id == id ? &*found : nullptr;
    }
};
struct TermSource {
    std::string term;
    std::vector<std::pair<std::uint64_t, std::string>> lists;
    std::uint64_t df = 0;
};
// A candidate already identifies its active segment. Exact boolean leaves
// only need that segment's cursor, not a merge of every obsolete/live version.
class ExactStream {
    std::map<std::uint64_t, std::unique_ptr<disk::PostingCursor>> cursors_;
    bool terms_ = false;
    std::size_t misses_ = 0;
    const std::function<bool()>* cancelled_ = nullptr;
    std::size_t cancellation_poll_ = 0;
public:
    ExactStream(const Snapshot& snapshot, const TermSource& source,
                const std::function<bool()>* cancelled = nullptr)
        : terms_(!source.term.empty() && source.term.front() == '\2'), cancelled_(cancelled) {
        for (const auto& [segment, descriptor] : source.lists)
            cursors_.emplace(segment, std::make_unique<disk::PostingCursor>(segment_path(snapshot.directory, segment), snapshot.cache, descriptor));
    }
    const disk::Posting* raw_match(std::uint64_t segment, std::uint64_t id) {
        auto found = cursors_.find(segment);
        if (found == cursors_.end() || !found->second->seek(id)) return nullptr;
        const auto& value = found->second->current();
        return value.document == id ? &value : nullptr;
    }
    const disk::Posting* match(const Record& record) {
        const auto* value = raw_match(record.segment, record.id);
        if (!value) return nullptr;
        if (value->line > record.lines || value->column > std::uint64_t(record.max_line) + 1 || (terms_ && value->frequency > record.tokens))
            throw Error("E_INDEX_CORRUPT", "Term posting exceeds document bounds");
        misses_ = 0;
        return value;
    }
    std::uint64_t hint_after_miss(std::uint64_t target) {
        // A global raw-posting lower bound can skip a disjoint ID range, but
        // inspecting every segment on every miss would penalize interleaved
        // lists. Amortize those inspections over at least four misses per cursor.
        if (++misses_ < (std::max)(std::size_t(64), cursors_.size() * 4)) return target;
        misses_ = 0;
        auto next = EndDoc;
        for (auto& [segment, cursor] : cursors_) {
            if (cancelled_ && (cancellation_poll_++ & 63U) == 0 && (*cancelled_)())
                throw Error("E_CANCELLED", "Search cancelled", true);
            (void)segment;
            if (cursor->seek(target)) next = (std::min)(next, cursor->current().document);
        }
        return next;
    }
};
class LiveStream {
    DocumentLookup& documents_;
    const std::function<bool()>* cancelled_ = nullptr;
    const std::vector<std::unique_ptr<ExactStream>>* raw_memberships_ = nullptr;
    std::size_t cancellation_poll_ = 0;
    struct Part { std::uint64_t segment;
    disk::PostingCursor cursor;
    disk::Posting current;
    };
    std::vector<Part> parts_;
    struct Head { std::uint64_t doc;
    std::size_t part;
    bool operator>(const Head& b) const { return doc > b.doc || (doc == b.doc && part > b.part);
    } };
    std::priority_queue<Head, std::vector<Head>, std::greater<Head>> heads_;
    bool initialized_ = false, terms_ = false;
    void check_cancelled() {
        if (cancelled_ && (cancellation_poll_++ & 63U) == 0 && (*cancelled_)())
            throw Error("E_CANCELLED", "Search cancelled", true);
    }
    bool advance(std::size_t p, std::uint64_t target) {
        check_cancelled();
        auto& part = parts_[p];
        while (part.cursor.seek(target)) {
            check_cancelled();
            const auto& value = part.cursor.current();
            // Necessary content anchors can reject this segment's raw posting
            // before resolving its catalog record. Never mix postings belonging
            // to different versions of one document. Each part advances only
            // its own segment's monotone membership cursors; accepted records
            // still undergo the ordinary liveness and posting-bounds checks.
            bool accepted = true;
            if (raw_memberships_) for (const auto& membership : *raw_memberships_) {
                if (!membership->raw_match(part.segment, value.document)) { accepted = false; break; }
            }
            auto r = accepted ? documents_.get(value.document) : nullptr;
            if (r && r->segment == part.segment) {
                if (value.line > r->lines || value.column > std::uint64_t(r->max_line) + 1 || (terms_ && value.frequency > r->tokens))
                    throw Error("E_INDEX_CORRUPT", "Posting exceeds document bounds");
                part.current = value;
                heads_.push({value.document, p});
                return true;
            }
            if (value.document == EndDoc) return false;
            target = value.document + 1;
        }
        return false;
    }
public:
    LiveStream(const Snapshot& s, DocumentLookup& d, const TermSource& source,
               const std::function<bool()>* cancelled = nullptr,
               const std::vector<std::unique_ptr<ExactStream>>* raw_memberships = nullptr)
        : documents_(d), cancelled_(cancelled), raw_memberships_(raw_memberships),
          terms_(!source.term.empty() && source.term.front() == '\2') {
        parts_.reserve(source.lists.size());
        for (const auto& [id, descriptor] : source.lists) parts_.push_back({id, disk::PostingCursor(segment_path(s.directory, id), s.cache, descriptor), {}});
    }
    std::uint64_t seek(std::uint64_t target) {
        if (!initialized_) { for (std::size_t i = 0; i < parts_.size(); ++i) advance(i, target);
        initialized_ = true;
        }
        while (!heads_.empty() && heads_.top().doc < target) { auto p = heads_.top().part;
        heads_.pop();
        advance(p, target);
        }
        return heads_.empty() ? EndDoc : heads_.top().doc;
    }
    const disk::Posting& current() const { if (heads_.empty()) throw Error("E_ARGUMENT", "Stream has no current document");
    return parts_[heads_.top().part].current;
    }
};
struct Anchor {
    bool universe = true;
    std::uint64_t cost = EndDoc;
    enum class Kind { Term, And, Or } kind = Kind::Term;
    std::string term;
    std::shared_ptr<const Anchor> left, right;
    std::size_t streams = 0;
};
Anchor and_anchor(Anchor a, Anchor b) {
    if (a.universe) return b;
    if (b.universe) return a;
    if (a.cost > b.cost) std::swap(a, b);
    if (!a.cost || (a.kind == Anchor::Kind::Term && b.kind == Anchor::Kind::Term && a.term == b.term)) return a;
    // A tiny anchor is already cheap to verify. Bound cursor state and avoid
    // intersecting a rare list with a vastly more expensive common list.
    if (a.cost <= 32 || a.streams + b.streams > 4 || b.cost / a.cost > 16) return a;
    Anchor out;
    out.universe = false;
    out.cost = a.cost;
    out.kind = Anchor::Kind::And;
    out.streams = a.streams + b.streams;
    out.left = std::make_shared<Anchor>(std::move(a));
    out.right = std::make_shared<Anchor>(std::move(b));
    return out;
}
Anchor or_anchor(Anchor a, Anchor b) {
    if (a.universe || b.universe) return {};
    if (a.kind == Anchor::Kind::Term && b.kind == Anchor::Kind::Term && a.term == b.term) return a;
    Anchor out;
    out.universe = false;
    out.cost = b.cost > EndDoc - a.cost ? EndDoc - 1 : a.cost + b.cost;
    out.kind = Anchor::Kind::Or;
    out.streams = a.streams + b.streams;
    out.left = std::make_shared<Anchor>(std::move(a));
    out.right = std::make_shared<Anchor>(std::move(b));
    return out;
}
// This proof is valid only when every posting belongs to a current-generation
// segment. Merely frequent lists are not enough: their joint membership may
// be selective. Exact full coverage composes through AND and OR normally.
bool anchor_covers_all(const Anchor& anchor, std::uint64_t files) {
    if (anchor.universe) return true;
    if (anchor.kind == Anchor::Kind::Term) return anchor.cost == files;
    if (anchor.kind == Anchor::Kind::And)
        return anchor_covers_all(*anchor.left, files) && anchor_covers_all(*anchor.right, files);
    return anchor_covers_all(*anchor.left, files) || anchor_covers_all(*anchor.right, files);
}
// Composable monotone cursors: AND leapfrogs sorted lists, OR merges them.
// No posting list or candidate set is materialized.
class AnchorStream {
    Anchor::Kind kind_;
    DocumentLookup& documents_;
    const std::function<bool()>* cancelled_ = nullptr;
    std::size_t cancellation_poll_ = 0;
    // The driver borrows this fixed membership vector and is destroyed first.
    std::vector<std::unique_ptr<ExactStream>> memberships_;
    std::unique_ptr<LiveStream> term_;
    std::unique_ptr<AnchorStream> left_, right_;
public:
    AnchorStream(const Snapshot& snapshot, DocumentLookup& documents, const Anchor& anchor,
                 const std::map<std::string, std::shared_ptr<TermSource>>& sources,
                 const std::function<bool()>* cancelled = nullptr,
                 bool prefilter_before_record = false)
        : kind_(anchor.kind), documents_(documents), cancelled_(cancelled) {
        if (kind_ == Anchor::Kind::Term) term_ = std::make_unique<LiveStream>(snapshot, documents, *sources.at(anchor.term), cancelled_);
        else {
            // A pure conjunction has one live-document driver.
            // Additional posting streams only need membership checks in that
            // document's active segment, not separate catalog lookups for the
            // union of all their intermediate postings. OR subtrees retain
            // the generic monotone merge/intersection path below.
            std::vector<const Anchor*> terms;
            std::function<bool(const Anchor&)> collect = [&](const Anchor& node) {
                if (node.universe || node.kind == Anchor::Kind::Or) return false;
                if (node.kind == Anchor::Kind::Term) { terms.push_back(&node); return true; }
                return collect(*node.left) && collect(*node.right);
            };
            if (kind_ == Anchor::Kind::And && collect(anchor)) {
                const auto cheapest = *std::min_element(terms.begin(), terms.end(), [](const auto* a, const auto* b) { return a->cost < b->cost; });
                memberships_.reserve(terms.size() - 1);
                for (const auto* item : terms) if (item != cheapest)
                    memberships_.push_back(std::make_unique<ExactStream>(snapshot, *sources.at(item->term), cancelled_));
                term_ = std::make_unique<LiveStream>(snapshot, documents, *sources.at(cheapest->term), cancelled_,
                                                   prefilter_before_record ? &memberships_ : nullptr);
                return;
            }
            left_ = std::make_unique<AnchorStream>(snapshot, documents, *anchor.left, sources, cancelled_);
            right_ = std::make_unique<AnchorStream>(snapshot, documents, *anchor.right, sources, cancelled_);
        }
    }
    AnchorStream(const AnchorStream&) = delete;
    AnchorStream& operator=(const AnchorStream&) = delete;
    AnchorStream(AnchorStream&&) = delete;
    AnchorStream& operator=(AnchorStream&&) = delete;
    std::uint64_t seek(std::uint64_t target) {
        if (term_) {
            auto doc = term_->seek(target);
            if (memberships_.empty()) return doc;
            while (doc != EndDoc) {
                // A selective conjunction may reject every driver document,
                // never reaching evaluate()'s ordinary cancellation checks.
                if (cancelled_ && (cancellation_poll_++ & 63U) == 0 && (*cancelled_)())
                    throw Error("E_CANCELLED", "Search cancelled", true);
                const auto* record = documents_.get(doc);
                if (!record) throw Error("E_INDEX_CORRUPT", "Live anchor posting has no document");
                bool accepted = true;
                auto next = doc + 1;
                for (auto& membership : memberships_) {
                    if (!membership->match(*record)) {
                        accepted = false;
                        next = membership->hint_after_miss(next);
                        break;
                    }
                }
                if (accepted) return doc;
                if (next == EndDoc) return EndDoc;
                doc = term_->seek(next);
            }
            return EndDoc;
        }
        auto a = left_->seek(target), b = right_->seek(target);
        if (kind_ == Anchor::Kind::Or) return (std::min)(a, b);
        while (a != b && a != EndDoc && b != EndDoc) {
            if (cancelled_ && (cancellation_poll_++ & 63U) == 0 && (*cancelled_)())
                throw Error("E_CANCELLED", "Search cancelled", true);
            if (a < b) a = left_->seek(b);
            else b = right_->seek(a);
        }
        return a == b ? a : EndDoc;
    }
};
// A query-local bounded pool. The caller participates as worker 0. Batches
// retain document order, while content streams/workspaces are worker-local.
class SearchWorkers {
    std::mutex mutex_;
    std::condition_variable changed_, finished_;
    std::vector<std::thread> threads_;
    std::function<void(std::size_t)> job_;
    std::size_t generation_ = 0, pending_ = 0;
    bool stopping_ = false;
    void stop() noexcept {
        { std::lock_guard lock(mutex_); stopping_ = true; }
        changed_.notify_all();
        for (auto& thread : threads_) if (thread.joinable()) thread.join();
    }
public:
    explicit SearchWorkers(std::size_t count) {
        try {
            for (std::size_t id = 1; id < count; ++id) threads_.emplace_back([&, id] {
                std::unique_lock lock(mutex_);
                std::size_t seen = 0;
                for (;;) {
                    changed_.wait(lock, [&] { return stopping_ || seen != generation_; });
                    if (stopping_) return;
                    seen = generation_;
                    lock.unlock();
                    // The supplied job catches all document exceptions and
                    // publishes them for propagation after joining the batch.
                    job_(id);
                    lock.lock();
                    if (--pending_ == 0) finished_.notify_one();
                }
            });
        } catch (...) { stop(); throw; }
    }
    ~SearchWorkers() { stop(); }
    SearchWorkers(const SearchWorkers&) = delete;
    SearchWorkers& operator=(const SearchWorkers&) = delete;
    void run(const std::function<void(std::size_t)>& job) {
        { std::lock_guard lock(mutex_);
        job_ = job;
        pending_ = threads_.size();
        ++generation_;
        }
        changed_.notify_all();
        job_(0);
        std::unique_lock lock(mutex_);
        finished_.wait(lock, [&] { return pending_ == 0; });
        job_ = {};
    }
};
void positive_leaves(const std::shared_ptr<const QueryNode>& node, bool negative, std::vector<bool>& flags) {
    if (node->kind == NodeKind::Leaf) { if (!negative) flags[node->leaf] = true;
    return;
    }
    if (node->kind == NodeKind::Not) positive_leaves(node->left, !negative, flags);
    else { positive_leaves(node->left, negative, flags);
    positive_leaves(node->right, negative, flags);
    }
}
void validate_query(const Query& query) {
    if (!query.root || query.leaves.empty() || query.leaves.size() > 128) throw Error("E_QUERY_SYNTAX", "Invalid query shape");
    std::size_t total = 0, bytes = 0;
    std::function<void(const std::shared_ptr<const QueryNode>&, unsigned)> visit = [&](const auto& n, unsigned depth) {
        if (!n || depth > 256 || ++total > 512) throw Error("E_QUERY_SYNTAX", "Query exceeds node/depth budget");
        if (n->kind == NodeKind::Leaf) { if (n->leaf >= query.leaves.size()) throw Error("E_QUERY_SYNTAX", "Invalid leaf index");
        }
        else { visit(n->left, depth + 1);
        if (n->kind != NodeKind::Not) visit(n->right, depth + 1);
        }
    };
    visit(query.root, 0);
    for (const auto& l : query.leaves) {
        bytes += l.text.size();
        if (bytes > 8192 || !valid_utf8(l.text) || l.text.find('\0') != std::string::npos || (l.kind != LeafKind::Regex && l.text.empty()) || (l.kind == LeafKind::Regex && !l.regex))
            throw Error("E_QUERY_SYNTAX", "Invalid query leaf");
        if (l.kind == LeafKind::Term) {
            std::size_t tokens = 0, size = 0;
            for_each_token(l.text, [&](Token t) { ++tokens; size = t.text.size(); });
            if (tokens != 1 || size != l.text.size()) throw Error("E_QUERY_SYNTAX", "Term must be a whole word; use literal for punctuation");
        }
    }
}
std::string snippet_text(std::string_view line, std::size_t column, std::size_t maximum) {
    if (line.size() <= maximum) return std::string(line);
    auto start = column > maximum / 3 ? column - maximum / 3 : 0;
    while (start && start < line.size() && (static_cast<unsigned char>(line[start]) & 0xc0) == 0x80) --start;
    auto end = (std::min)(line.size(), start + maximum);
    while (end > start && end < line.size() && (static_cast<unsigned char>(line[end]) & 0xc0) == 0x80) --end;
    return (start ? "..." : "") + std::string(line.substr(start, end - start)) + (end < line.size() ? "..." : "");
}
SearchReport do_search(const Snapshot& snapshot, const Query& query, const SearchOptions& options) {
    const auto started = Clock::now();
    if (options.cancelled && options.cancelled()) throw Error("E_CANCELLED", "Search cancelled", true);
    validate_query(query);
    const auto budget = budget_bytes(options.memory_mb);
    if (options.limit > 10000 || options.offset > 10000 || options.offset + options.limit > 10000 || options.snippets_per_file > 32 ||
        options.snippet_bytes < 16 || options.snippet_bytes > 65536 || !options.max_regex_steps || options.paths.size() > 128 || options.threads > 256)
        throw Error("E_ARGUMENT", "Invalid search limits");
    if (options.after && (options.after_path.empty() || options.after_path.size() > MaxPath || !valid_utf8(options.after_path) || !std::isfinite(options.after_score)))
        throw Error("E_CURSOR", "Invalid continuation key");
    auto result_memory = (options.offset + options.limit) * (512ULL + MaxPath + options.snippets_per_file * (options.snippet_bytes + 128ULL));
    // Non-overlapping upper allocations: shared page/record caches 5/16,
    // results + main line buffers <=1/3, decoded documents/sources/cursors
    // each <=1/16, workers <=1/8, pending batch <=1/64. OS thread stacks and
    // allocator overhead are not controllable through standard C++.
    if (result_memory + 2 * snapshot.manifest.policy.max_line_bytes + MiB > budget / 3)
        throw Error("E_BUDGET", "Result buffers exceed memory budget; reduce limit/snippet size");
    for (const auto& p : options.paths) if (p.size() > 8192 || !valid_utf8(p)) throw Error("E_ARGUMENT", "Invalid path filter");
    DocumentLookup documents(snapshot, budget / 16);
    std::map<std::string, std::shared_ptr<TermSource>> sources;
    std::size_t source_memory = 0;
    auto source = [&](const std::string& term) -> std::shared_ptr<TermSource> {
        auto found = sources.find(term);
        if (found != sources.end()) return found->second;
        auto s = std::make_shared<TermSource>();
        s->term = term;
        for (const auto& [id, live] : snapshot.manifest.segments) {
            (void)live;
            disk::Table table(segment_path(snapshot.directory, id) / "terms.tbl", snapshot.cache);
            if (auto descriptor = table.get(term)) {
                auto df = disk::posting_count(*descriptor);
                s->df = checked_add(s->df, df);
                s->lists.emplace_back(id, *descriptor);
                source_memory += descriptor->size() + 96;
            }
        }
        source_memory += term.size() * 2 + 256;
        if (source_memory > budget / 16) throw Error("E_BUDGET", "Query dictionary state exceeds budget");
        sources.emplace(term, s);
        return s;
    };
    auto term_anchor = [&](const std::string& term) { auto s = source(term);
    return Anchor{false, s->df, Anchor::Kind::Term, term, {}, {}, 1};
    };
    auto literal_anchor = [&](const std::string& text) {
        Anchor best;
        // Only fully enclosed tokens are necessary exact terms. A token at a
        // literal edge can match inside a larger identifier and is NOT safe.
        for_each_token(text, [&](Token token) {
            if (token.column && token.column + token.text.size() < text.size())
                best = and_anchor(std::move(best), term_anchor(std::string(1, '\2') + std::string(token.text)));
        });
        if (snapshot.manifest.policy.profile != "fast" || text.size() < 3) return best;
        // Sampling only weakens the necessary condition; it cannot lose a match.
        const auto n = text.size() - 2, count = (std::min)(std::size_t(16), n);
        std::vector<Anchor> choices;
        for (std::size_t i = 0; i < count; ++i) {
            auto at = count == 1 ? 0 : i * (n - 1) / (count - 1);
            choices.push_back(term_anchor(std::string(1, '\1') + text.substr(at, 3)));
            if (!choices.back().cost) return choices.back();
        }
        std::sort(choices.begin(), choices.end(), [](const Anchor& a, const Anchor& b) {
            return a.cost < b.cost || (a.cost == b.cost && a.term < b.term);
        });
        for (auto& candidate : choices) best = and_anchor(std::move(best), std::move(candidate));
        return best;
    };
    std::function<Anchor(const std::shared_ptr<const RegexFilter>&)> regex_anchor;
    regex_anchor = [&](const auto& f) -> Anchor {
        if (!f || f->kind == RegexFilter::Kind::Always) return {};
        if (f->kind == RegexFilter::Kind::Literal) return literal_anchor(f->literal);
        auto a = regex_anchor(f->left), b = regex_anchor(f->right);
        return f->kind == RegexFilter::Kind::And ? and_anchor(std::move(a), std::move(b)) : or_anchor(std::move(a), std::move(b));
    };
    std::vector<Anchor> leaves;
    std::vector<std::shared_ptr<TermSource>> exact(query.leaves.size());
    for (std::size_t i = 0; i < query.leaves.size(); ++i) {
        const auto& l = query.leaves[i];
        if (l.kind == LeafKind::Term) { exact[i] = source(std::string(1, '\2') + l.text);
        leaves.push_back({false, exact[i]->df, Anchor::Kind::Term, exact[i]->term, {}, {}, 1});
        }
        else if (l.kind == LeafKind::Phrase) leaves.push_back(literal_anchor(l.text));
        else leaves.push_back(regex_anchor(l.regex->prefilter()));
    }
    std::function<Anchor(const std::shared_ptr<const QueryNode>&)> plan = [&](const auto& n) -> Anchor {
        if (n->kind == NodeKind::Leaf) return leaves[n->leaf];
        if (n->kind == NodeKind::Not) return {}; // Never subtract an approximate candidate set.
        auto a = plan(n->left), b = plan(n->right);
        return n->kind == NodeKind::And ? and_anchor(std::move(a), std::move(b)) : or_anchor(std::move(a), std::move(b));
    };
    auto anchor = plan(query.root);
    // A single content predicate whose necessary postings cover every live
    // document gains nothing from merging those lists and looking up the
    // same catalog rows again. The ordered catalog scan visits the identical
    // candidates directly. Only freshly written segments make df exact: after
    // an incremental update, obsolete postings can inflate it arbitrarily.
    // Keep Boolean query roots and merely common component lists intact.
    if (query.root->kind == NodeKind::Leaf &&
        query.leaves[query.root->leaf].kind != LeafKind::Term &&
        !anchor.universe && snapshot.manifest.files &&
        anchor_covers_all(anchor, snapshot.manifest.files) &&
        std::all_of(snapshot.manifest.segments.begin(), snapshot.manifest.segments.end(),
            [&](const auto& item) { return item.first / SegmentGenerationStride == snapshot.manifest.generation; }))
        anchor = {};
    documents.set_sparse(!anchor.universe && anchor.cost < snapshot.manifest.files / 16);
    std::size_t stream_parts = 0;
    for (const auto& s : exact) if (s) stream_parts += s->lists.size();
    std::function<void(const Anchor&)> count_parts = [&](const Anchor& a) {
        if (a.universe) return;
        if (a.kind == Anchor::Kind::Term) stream_parts += source(a.term)->lists.size();
        else { count_parts(*a.left); count_parts(*a.right); }
    };
    count_parts(anchor);
    if (stream_parts > budget / (16 * 8192)) throw Error("E_BUDGET", "Too many query/segment cursors; compact index or simplify query");
    std::unique_ptr<AnchorStream> anchors;
    std::vector<std::unique_ptr<ExactStream>> values(query.leaves.size());
    for (std::size_t i = 0; i < exact.size(); ++i) if (exact[i]) values[i] = std::make_unique<ExactStream>(snapshot, *exact[i]);
    if (!anchor.universe) anchors = std::make_unique<AnchorStream>(snapshot, documents, anchor, sources,
        options.cancelled ? &options.cancelled : nullptr,
        query.root->kind == NodeKind::Leaf && query.leaves[query.root->leaf].kind != LeafKind::Term);
    std::vector<bool> positive(query.leaves.size(), false), present(query.leaves.size());
    positive_leaves(query.root, false, positive);
    std::vector<std::uint64_t> scratch_counts(query.leaves.size()), scratch_lines(query.leaves.size());
    std::vector<std::uint8_t> possible(query.leaves.size());
    std::vector<RegexWorkspace> regex_work(query.leaves.size());
    std::uint64_t steps = options.max_regex_steps;
    SearchReport report;
    report.generation = snapshot.manifest.generation;
    report.profile.enabled = options.profile;
    ContentReader content_reader(snapshot, 16, options.profile ? &report.profile : nullptr);
    LiteralLookup literal_filters(snapshot);
    std::vector<LiteralFilter> literal_masks(query.leaves.size());
    std::map<const RegexFilter*, LiteralFilter> regex_masks;
    std::function<void(const std::shared_ptr<const RegexFilter>&)> prepare_filter = [&](const auto& filter) {
        if (!filter) return;
        if (filter->kind == RegexFilter::Kind::Literal) regex_masks.emplace(filter.get(), LiteralFilter::required(filter->literal));
        else { prepare_filter(filter->left); prepare_filter(filter->right); }
    };
    bool filter_useful = false;
    for (std::size_t i = 0; i < query.leaves.size(); ++i) {
        const auto& leaf = query.leaves[i];
        if (leaf.kind == LeafKind::Phrase && leaf.text.size() >= LiteralFilter::GramBytes) {
            literal_masks[i] = LiteralFilter::required(leaf.text);
            filter_useful = true;
        } else if (leaf.kind == LeafKind::Regex) prepare_filter(leaf.regex->prefilter());
    }
    filter_useful = filter_useful || !regex_masks.empty();
    std::function<bool(const std::shared_ptr<const RegexFilter>&, const LiteralFilter&)> regex_possible;
    regex_possible = [&](const auto& filter, const LiteralFilter& document) -> bool {
        if (!filter || filter->kind == RegexFilter::Kind::Always) return true;
        if (filter->kind == RegexFilter::Kind::Literal) return document.maybe(regex_masks.at(filter.get()));
        if (filter->kind == RegexFilter::Kind::And) return regex_possible(filter->left, document) && regex_possible(filter->right, document);
        return regex_possible(filter->left, document) || regex_possible(filter->right, document);
    };
    auto charge_read = [&](const Record& r) {
        report.scanned_bytes = checked_add(report.scanned_bytes, r.size);
        if (options.max_scan_bytes && report.scanned_bytes > options.max_scan_bytes) throw Error("E_BUDGET", "Search scan-byte budget exhausted");
    };
    struct Scored { SearchHit hit;
    Record record;
    };
    auto better = [&](const Scored& a, const Scored& b) {
        if (!options.sort_path && a.hit.score != b.hit.score) return a.hit.score > b.hit.score;
        return a.record.path < b.record.path;
    };
    std::priority_queue<Scored, std::vector<Scored>, decltype(better)> best(better);
    auto wanted = options.limit == 0 ? std::size_t(0) : options.offset + options.limit;
    const auto average = (std::max)(1.0, static_cast<double>(snapshot.manifest.tokens) / (std::max)(1.0, static_cast<double>(snapshot.manifest.files)));
    std::vector<double> idfs(query.leaves.size(), 1.0);
    for (std::size_t i = 0; i < exact.size(); ++i) if (exact[i]) {
        const auto df = static_cast<double>((std::min)(snapshot.manifest.files, exact[i]->df));
        idfs[i] = std::log1p((static_cast<double>(snapshot.manifest.files) - df + 0.5) / (df + 0.5));
    }
    const auto candidate_started = options.profile ? Clock::now() : Clock::time_point{};
    if (options.profile) report.profile.planning_seconds = std::chrono::duration<double>(candidate_started - started).count();
    auto score_document = [&](const Record& file, const std::vector<std::uint64_t>& counts,
                              const std::vector<std::uint64_t>& first_line) {
        for (std::size_t i = 0; i < counts.size(); ++i) present[i] = counts[i] != 0;
        if (!query.evaluate(present)) return;
        ++report.total_matches;
        if (!wanted) { ++report.remaining_matches;
        return;
        }
        Scored hit;
        std::uint64_t minimum = EndDoc, maximum = 0;
        std::size_t contributing = 0;
        const auto norm = 1.2 * (0.25 + 0.75 * static_cast<double>(file.tokens) / average);
        for (std::size_t i = 0; i < counts.size(); ++i) if (positive[i] && counts[i]) {
            ++contributing;
            hit.hit.matches = checked_add(hit.hit.matches, counts[i]);
            minimum = (std::min)(minimum, first_line[i]);
            maximum = (std::max)(maximum, first_line[i]);
            // Segment df is an upper estimate until compaction removes obsolete versions.
            auto tf = static_cast<double>(counts[i]);
            auto weight = query.leaves[i].kind == LeafKind::Phrase ? 1.6 : 1.0;
            hit.hit.score += weight * idfs[i] * (tf * 2.2 / (tf + norm)) * (1.0 + 0.4 / (1.0 + static_cast<double>(first_line[i]) / 30.0));
            if (file.path.find(query.leaves[i].text) != std::string::npos) hit.hit.score += 0.5;
        }
        if (contributing > 1) hit.hit.score += 0.2 / (1.0 + static_cast<double>(maximum - minimum));
        if (options.after) {
            if (options.sort_path) { if (file.path <= options.after_path) return;
            }
            else if (hit.hit.score > options.after_score || (hit.hit.score == options.after_score && file.path <= options.after_path)) return;
        }
        ++report.remaining_matches;
        const bool retain = best.size() < wanted ||
            (!options.sort_path && hit.hit.score != best.top().hit.score ? hit.hit.score > best.top().hit.score : file.path < best.top().record.path);
        if (retain) {
            // Most matching documents never enter top-k. Do not allocate their
            // paths or format content IDs, and keep only one owning path copy.
            hit.record = file;
            if (best.size() == wanted) best.pop();
            best.push(std::move(hit));
        }
    };
    const bool has_regex = std::any_of(query.leaves.begin(), query.leaves.end(), [](const QueryLeaf& leaf) { return leaf.kind == LeafKind::Regex; });
    const bool has_content = std::any_of(query.leaves.begin(), query.leaves.end(), [](const QueryLeaf& leaf) { return leaf.kind != LeafKind::Term; });
    const auto estimated = (std::min)(anchor.cost, snapshot.manifest.files);
    // Finite query-wide NFA work is charged in document order. Keep regex
    // queries serial rather than changing their budget contract with races or
    // speculative per-worker limits. Term-only queries already use postings.
    const auto per_worker = 2 * snapshot.manifest.policy.max_line_bytes + IoBytes + 256 * 1024;
    std::size_t workers = 1;
    if (has_content && !has_regex && estimated >= 512 && snapshot.manifest.bytes >= MiB) {
        // On some standard libraries this asks the operating system on every
        // call. Posting-only and small searches cannot use workers, so do not
        // pay that setup cost for their otherwise microsecond-sized workload.
        const auto desired = options.threads ? options.threads : (std::min)(std::size_t(8), std::size_t((std::max)(1U, std::thread::hardware_concurrency())));
        workers = (std::max)(std::size_t(1), (std::min)({desired, std::size_t(16), (budget / 8) / per_worker}));
    }
    struct Pending {
        Record file;
        std::vector<std::uint64_t> counts, lines;
    };
    std::vector<Pending> pending;
    std::size_t pending_memory = 0;
    constexpr std::uint64_t ParallelContentThreshold = 2 * MiB;
    std::uint64_t admitted_content_bytes = 0;
    const auto batch_limit = (std::max)(std::size_t(16), workers * 128);
    std::vector<SearchProfile> worker_profiles(workers);
    std::vector<std::unique_ptr<ContentReader>> readers;
    std::unique_ptr<SearchWorkers> pool;
    auto check_cancelled = [&] {
        if (options.cancelled && options.cancelled()) throw Error("E_CANCELLED", "Search cancelled", true);
    };
    auto scan_document = [&](ContentReader& reader, const Record& file, std::vector<std::uint64_t>& counts,
                             std::vector<std::uint64_t>& first_line, const std::atomic<bool>* stopped = nullptr) {
        // Empty documents have no line callback, but queued work must still
        // observe cancellation in both serial and worker-driven flushes.
        check_cancelled();
        reader.read(file, [&](std::string_view text, std::uint64_t line) {
            if (stopped && stopped->load(std::memory_order_relaxed)) throw Error("E_CANCELLED", "Search worker stopped");
            check_cancelled();
            for (std::size_t i = 0; i < query.leaves.size(); ++i) {
                const auto& leaf = query.leaves[i];
                if (leaf.kind == LeafKind::Phrase) {
                    std::size_t start = 0;
                    while (true) {
                        const auto at = text.find(leaf.text, start);
                        if (at == text.npos) break;
                        ++counts[i];
                        if (!first_line[i]) first_line[i] = line;
                        start = at + leaf.text.size();
                    }
                } else if (leaf.kind == LeafKind::Regex) {
                    RegexMatch match;
                    if (detail::ValidatedRegexAccess::find(*leaf.regex, text, match, steps, regex_work[i])) {
                        ++counts[i];
                        if (!first_line[i]) first_line[i] = line;
                    }
                }
            }
        });
    };
    auto flush = [&] {
        if (pending.empty()) return;
        // Do not create threads for a selective prefilter's tiny remainder.
        if (!pool && admitted_content_bytes >= ParallelContentThreshold &&
            pending.size() >= (std::max)(std::size_t(32), workers * 4)) {
            for (std::size_t i = 0; i < workers; ++i) {
                worker_profiles[i].enabled = options.profile;
                readers.push_back(std::make_unique<ContentReader>(snapshot, (std::max)(std::size_t(1), std::size_t(64) / workers),
                    options.profile ? &worker_profiles[i] : nullptr));
            }
            pool = std::make_unique<SearchWorkers>(workers);
            report.profile.workers = workers;
        }
        if (pool) {
            // Reorder only dispatch indices for locality, never the charged
            // candidate stream or the deterministic ordered result reduction.
            std::uint64_t total_bytes = 0, smallest = EndDoc, largest = 0;
            for (const auto& item : pending) {
                total_bytes += item.file.size; // Already bounded by charge_read().
                smallest = (std::min)(smallest, item.file.size);
                largest = (std::max)(largest, item.file.size);
            }
            // Preserve the striped schedule for empty/extremely mixed batches,
            // where byte-only partitions can hide per-file overhead.
            const bool grouped = smallest && largest / smallest <= 16 &&
                largest <= (total_bytes / workers) * 2;
            std::vector<std::size_t> order;
            std::array<std::size_t, 17> boundaries{};
            if (grouped) {
                order.resize(pending.size());
                std::iota(order.begin(), order.end(), std::size_t(0));
                std::sort(order.begin(), order.end(), [&](auto a, auto b) {
                    const auto& x = pending[a].file;
                    const auto& y = pending[b].file;
                    return x.segment != y.segment ? x.segment < y.segment : x.offset < y.offset;
                });
                std::uint64_t consumed = 0;
                std::size_t next = 0;
                for (std::size_t worker = 1; worker < workers; ++worker) {
                    const auto target = (total_bytes / workers) * worker +
                        ((total_bytes % workers) * worker) / workers;
                    while (next < order.size() && consumed <= target &&
                           pending[order[next]].file.size <= target - consumed) {
                        consumed += pending[order[next++]].file.size;
                    }
                    if (next < order.size() && consumed <= target) {
                        const auto remaining = target - consumed;
                        const auto bytes = pending[order[next]].file.size;
                        if (remaining > bytes - remaining)
                            consumed += pending[order[next++]].file.size;
                    }
                    boundaries[worker] = next;
                }
                boundaries[workers] = order.size();
            }
            std::atomic<bool> stopped{false};
            std::exception_ptr failure;
            std::mutex failure_mutex;
            pool->run([&](std::size_t worker) {
                const auto begin = grouped ? boundaries[worker] : worker;
                const auto end = grouped ? boundaries[worker + 1] : pending.size();
                const auto stride = grouped ? std::size_t(1) : workers;
                for (std::size_t position = begin; position < end; position += stride) {
                    if (stopped.load(std::memory_order_relaxed)) return;
                    const auto i = grouped ? order[position] : position;
                    auto& item = pending[i];
                    try { scan_document(*readers[worker], item.file, item.counts, item.lines, &stopped); }
                    catch (...) {
                        // Preserve the original failure, not a secondary
                        // cancellation raised by another worker unwinding.
                        std::lock_guard lock(failure_mutex);
                        if (!failure) failure = std::current_exception();
                        stopped.store(true, std::memory_order_relaxed);
                        return;
                    }
                }
            });
            if (failure) std::rethrow_exception(failure);
        } else {
            for (auto& item : pending) scan_document(content_reader, item.file, item.counts, item.lines);
        }
        // One deterministic ordered reduction retains exact scores/ties,
        // totals, top-k and continuation semantics, without per-thread heaps.
        for (const auto& item : pending) score_document(item.file, item.counts, item.lines);
        pending.clear();
        pending_memory = 0;
    };
    auto evaluate = [&](const Record& file) {
        check_cancelled();
        ++report.candidates;
        if (options.max_candidates && report.candidates > options.max_candidates) throw Error("E_BUDGET", "Candidate budget exhausted");
        if (!options.paths.empty() && std::none_of(options.paths.begin(), options.paths.end(), [&](const auto& p) { return match_path(p, file.path); })) return;
        std::fill(scratch_counts.begin(), scratch_counts.end(), 0);
        std::fill(scratch_lines.begin(), scratch_lines.end(), 0);
        bool scans = false;
        for (std::size_t i = 0; i < values.size(); ++i) {
            if (values[i]) {
                if (auto value = values[i]->match(file)) {
                    scratch_counts[i] = value->frequency;
                    scratch_lines[i] = value->line;
                }
                possible[i] = scratch_counts[i] ? 1 : 0;
            } else { possible[i] = 2; scans = true; }
        }
        if (query.possible(possible) == 0) return;
        if (scans) {
            if (filter_useful && snapshot.manifest.policy.profile == "compact") {
                if (const auto filter = literal_filters.get(file)) {
                    for (std::size_t i = 0; i < query.leaves.size(); ++i) {
                        const auto& leaf = query.leaves[i];
                        if ((leaf.kind == LeafKind::Phrase && !filter->maybe(literal_masks[i])) ||
                            (leaf.kind == LeafKind::Regex && !regex_possible(leaf.regex->prefilter(), *filter))) possible[i] = 0;
                    }
                    if (query.possible(possible) == 0) {
                        if (options.profile) ++report.profile.filtered_documents;
                        return;
                    }
                }
            }
            // Reserve global work in candidate order before dispatch; workers
            // cannot multiply max_scan_bytes/max_candidates by their count.
            charge_read(file);
            if (workers > 1) {
                const auto cost = sizeof(Pending) + sizeof(std::size_t) + file.path.size() + 128 + query.leaves.size() * 16;
                if (pending_memory + cost > budget / 64) flush();
                pending.push_back({file, scratch_counts, scratch_lines});
                // Count admitted work only after an owning queue item exists:
                // charge_read() can precede a pressure flush of the old batch.
                admitted_content_bytes += (std::min)(file.size, ParallelContentThreshold - admitted_content_bytes);
                pending_memory += cost;
                if (pending.size() >= batch_limit) flush();
                return;
            }
            scan_document(content_reader, file, scratch_counts, scratch_lines);
        }
        score_document(file, scratch_counts, scratch_lines);
    };
    if (anchor.universe) {
        // evaluate() never advances the anchor or accesses DocumentLookup.
        // Exact term cursors, content readers and literal filters have separate
        // state; pending work and retained hits make their own owning copies.
        // Therefore one decoded shard stays valid throughout this traversal.
        for (std::size_t b = 0; b < Buckets; ++b) {
            const auto& rows = documents.shard(b);
            for (const auto& record : rows) evaluate(record);
        }
    } else {
        std::uint64_t from = 0;
        for (;;) {
            auto doc = anchors->seek(from);
            if (doc == EndDoc) break;
            auto found = documents.get(doc);
            if (!found) throw Error("E_INDEX_CORRUPT", "Live posting has no document");
            // Consume the borrowed record before the next anchor seek can
            // evict its decoded shard (or replace sparse lookup's held row).
            evaluate(*found);
            from = doc + 1;
        }
    }
    flush();
    if (options.profile) for (const auto& p : worker_profiles) {
        report.profile.content_open_seconds += p.content_open_seconds;
        report.profile.content_read_seconds += p.content_read_seconds;
        report.profile.verification_seconds += p.verification_seconds;
        report.profile.line_processing_seconds += p.line_processing_seconds;
        report.profile.matching_seconds += p.matching_seconds;
        report.profile.content_opens += p.content_opens;
        report.profile.content_reads += p.content_reads;
        report.profile.content_bytes += p.content_bytes;
    }
    const auto snippets_started = options.profile ? Clock::now() : Clock::time_point{};
    if (options.profile) report.profile.candidate_seconds = std::chrono::duration<double>(snippets_started - candidate_started).count();
    std::vector<Scored> ordered;
    ordered.reserve(best.size());
    while (!best.empty()) { ordered.push_back(best.top());
    best.pop();
    }
    std::sort(ordered.begin(), ordered.end(), better);
    for (std::size_t index = (std::min)(options.offset, ordered.size()); index < ordered.size(); ++index) {
        auto& result = ordered[index];
        auto& snippets = result.hit.snippets;
        if (options.snippets_per_file && std::any_of(positive.begin(), positive.end(), [](bool b) { return b; })) {
            charge_read(result.record);
            content_reader.read(result.record, [&](std::string_view text, std::uint64_t line) {
                check_cancelled();
                if (snippets.size() >= options.snippets_per_file) return;
                std::size_t column = (std::numeric_limits<std::size_t>::max)();
                for_each_token(text, [&](Token t) {
                    for (std::size_t i = 0; i < query.leaves.size(); ++i) if (positive[i] && query.leaves[i].kind == LeafKind::Term && t.text == query.leaves[i].text)
                        column = (std::min)(column, t.column);
                });
                for (std::size_t i = 0; i < query.leaves.size(); ++i) if (positive[i]) {
                    const auto& l = query.leaves[i];
                    if (l.kind == LeafKind::Phrase) { auto at = text.find(l.text);
                    if (at != text.npos) column = (std::min)(column, at);
                    }
                    else if (l.kind == LeafKind::Regex) { RegexMatch match;
                    if (detail::ValidatedRegexAccess::find(*l.regex, text, match, steps, regex_work[i])) column = (std::min)(column, match.begin);
                    }
                }
                if (column != (std::numeric_limits<std::size_t>::max)()) snippets.push_back({line, column + 1, snippet_text(text, column, options.snippet_bytes)});
            });
        }
        result.hit.content_id = content_id(result.record);
        result.hit.path = std::move(result.record.path);
        report.hits.push_back(std::move(result.hit));
    }
    report.seconds = std::chrono::duration<double>(Clock::now() - started).count();
    if (options.profile) {
        report.profile.snippet_seconds = std::chrono::duration<double>(Clock::now() - snippets_started).count();
        report.profile.regex_steps = options.max_regex_steps - steps;
    }
    return report;
}
std::uint64_t object_bytes(const fs::path& p) {
    std::uint64_t n = 0;
    if (fs::is_symlink(fs::symlink_status(p))) throw Error("E_SCOPE", "Refusing symlink in managed index objects");
    if (fs::is_regular_file(p)) return storage::checked_file_size(p);
    for (fs::recursive_directory_iterator it(p), end; it != end; ++it) {
        if (fs::is_symlink(it->symlink_status())) { it.disable_recursion_pending();
        continue;
        }
        if (it->is_regular_file()) n = checked_add(n, it->file_size());
    }
    return n;
}
} // namespace

std::string path_to_utf8(const fs::path& path) {
    auto value = path.generic_u8string();
    return std::string(reinterpret_cast<const char*>(value.data()), value.size());
}
fs::path path_from_utf8(const std::string& text) {
    std::u8string value;
    value.reserve(text.size());
    for (char c : text) value += static_cast<char8_t>(static_cast<unsigned char>(c));
    return fs::path(value);
}
bool match_path(const std::string& pattern, const std::string& path) {
    if (pattern.size() > 8192 || path.size() > MaxPath) throw Error("E_ARGUMENT", "Glob exceeds size bound");
    std::string_view text(path);
    if (pattern.find('/') == std::string::npos) { auto pos = text.rfind('/');
    if (pos != text.npos) text.remove_prefix(pos + 1);
    }
    // Iterative DP, bounded O(pattern bytes * path bytes), no recursive backtracking.
    if (pattern.size() && text.size() > 2000000 / pattern.size()) throw Error("E_BUDGET", "Path glob work budget exceeded");
    std::vector<unsigned char> current(text.size() + 1), next(text.size() + 1);
    current[0] = 1;
    for (std::size_t i = 0; i < pattern.size(); ++i) {
        std::fill(next.begin(), next.end(), 0);
        char c = pattern[i];
        if (c == '*') {
            bool all = i + 1 < pattern.size() && pattern[i + 1] == '*';
            if (all) ++i;
            next[0] = current[0];
            for (std::size_t j = 1; j <= text.size(); ++j) next[j] = static_cast<unsigned char>(current[j] || (next[j - 1] && (all || text[j - 1] != '/')));
            // **/ also matches zero directory components.
            if (all && i + 1 < pattern.size() && pattern[i + 1] == '/') {
                auto before = current;
                current = next;
                ++i;
                std::fill(next.begin(), next.end(), 0);
                for (std::size_t j = 0; j <= text.size(); ++j) next[j] = static_cast<unsigned char>(before[j] || (j && current[j - 1] && text[j - 1] == '/'));
            }
        } else {
            for (std::size_t j = 1; j <= text.size(); ++j) next[j] = static_cast<unsigned char>(current[j - 1] && (c == '?' ? text[j - 1] != '/' : c == text[j - 1]));
        }
        current.swap(next);
    }
    return current.back() != 0;
}
struct Index::Impl { std::shared_ptr<Snapshot> snapshot;
};
Index::Index(const fs::path& dir, std::size_t memory, std::uint64_t generation)
    : impl_(std::make_shared<Impl>(Impl{open_snapshot(dir, budget_bytes(memory), generation)})) {}
SearchReport Index::search(const std::string& q, const SearchOptions& o) const { return search(parse_query(q), o);
}
SearchReport Index::search(const Query& q, const SearchOptions& o) const { return do_search(*impl_->snapshot, q, o);
}
std::vector<std::string> Index::warnings() const { return impl_->snapshot->warnings;
}
BuildOptions Index::policy() const { return impl_->snapshot->manifest.policy;
}
std::uint64_t Index::generation() const noexcept { return impl_->snapshot->manifest.generation;
}
Statistics Index::stats(bool disk_usage) const {
    const auto& s = *impl_->snapshot;
    const auto& m = s.manifest;
    Statistics out;
    out.generation = m.generation;
    out.files = m.files;
    out.lines = m.lines;
    out.bytes = m.bytes;
    out.tokens = m.tokens;
    out.segments = m.segments.size();
    out.root = path_to_utf8(m.root);
    out.profile = m.policy.profile;
    out.warnings = s.warnings;
    out.recovered = !s.warnings.empty();
    out.retained_generations = commits(s.directory).size();
    if (disk_usage) out.index_bytes = object_bytes(s.directory);
    return out;
}
VerifyReport Index::verify() const {
    const auto& original = *impl_->snapshot;
    // Verification uses a fresh cache so it notices corruption introduced after open.
    auto s = open_snapshot(original.directory, original.budget, original.manifest.generation);
    VerifyReport out;
    out.generation = s->manifest.generation;
    std::uint64_t lines = 0, bytes = 0, tokens = 0;
    std::map<std::uint64_t, std::uint64_t> segments;
    std::uint64_t catalog_entries = 0;
    ContentReader content_reader(*s);
    LiteralLookup literal_filters(*s);
    for (std::size_t b = 0; b < Buckets; ++b) if (auto table = s->table(b)) {
        out.bytes_checked = checked_add(out.bytes_checked, table->verify());
        catalog_entries = checked_add(catalog_entries, table->count());
    }
    s->each([&](const Record& r) {
        auto linked = s->by_path(r.path);
        if (!linked || linked->id != r.id) throw Error("E_INDEX_CORRUPT", "Broken path link");
        const auto saved_filter = literal_filters.get(r);
        LiteralFilter actual_filter;
        content_reader.read(r, [&](std::string_view text, auto) { if (saved_filter) actual_filter.add_line(text); });
        if (saved_filter && actual_filter.encode() != saved_filter->encode())
            throw Error("E_INDEX_CORRUPT", "Literal filter differs from verified snapshot content");
        ++out.files;
        out.bytes_checked = checked_add(out.bytes_checked, r.size);
        lines = checked_add(lines, r.lines);
        bytes = checked_add(bytes, r.size);
        tokens = checked_add(tokens, r.tokens);
        ++segments[r.segment];
    });
    const auto& m = s->manifest;
    if (out.files != m.files || catalog_entries != 2 * out.files || lines != m.lines || bytes != m.bytes || tokens != m.tokens || segments != m.segments)
        throw Error("E_INDEX_CORRUPT", "Manifest/catalog accounting mismatch");
    for (const auto& [id, count] : m.segments) { (void)count;
    out.bytes_checked = checked_add(out.bytes_checked, disk::verify_postings(segment_path(s->directory, id), s->cache));
    const auto filter_path = segment_path(s->directory, id) / "literal.tbl";
    if (fs::exists(filter_path)) {
        disk::Table filters(filter_path, s->cache);
        out.bytes_checked = checked_add(out.bytes_checked, filters.verify());
        filters.each([&](auto key, auto value) {
            if (key.size() != 8 || value.size() != 157 || !value.starts_with("RSLF1"))
                throw Error("E_INDEX_CORRUPT", "Invalid literal-filter entry");
        });
    }
    ++out.segments;
    }
    return out;
}
FileList Index::files(const std::string& pattern, std::size_t limit, const std::string& after) const {
    if (limit > 10000) throw Error("E_ARGUMENT", "File limit exceeds 10000");
    FileList out;
    out.generation = impl_->snapshot->manifest.generation;
    auto cmp = [](const FileInfo& a, const FileInfo& b) { return a.path < b.path;
    };
    std::priority_queue<FileInfo, std::vector<FileInfo>, decltype(cmp)> best(cmp);
    impl_->snapshot->each([&](const Record& r) {
        if (r.path <= after || !match_path(pattern, r.path)) return;
        ++out.total;
        if (!limit) return;
        FileInfo info{r.path, content_id(r), r.size, r.lines};
        if (best.size() < limit) best.push(std::move(info));
        else if (info.path < best.top().path) { best.pop();
        best.push(std::move(info));
        }
    });
    while (!best.empty()) { out.files.push_back(best.top());
    best.pop();
    } std::reverse(out.files.begin(), out.files.end());
    return out;
}
ReadReport Index::read(const std::string& path, std::uint64_t first, std::uint64_t last, const std::string& source,
                       const std::string& expected, std::size_t max_bytes) const {
    if (!safe_relative(path)) throw Error("E_SCOPE", "Read requires a safe relative path");
    if (!first || last < first || last - first > 100000 || max_bytes < 16 || max_bytes > MiB || (source != "snapshot" && source != "workspace"))
        throw Error("E_ARGUMENT", "Invalid read range/source/byte limit");
    const auto& s = *impl_->snapshot;
    auto file = s.by_path(path);
    if (!file) throw Error("E_NOT_FOUND", "Path is not in this snapshot");
    auto id = content_id(*file);
    if (!expected.empty() && expected != id) throw Error("E_STALE_READ", "content_id differs from this snapshot", true);
    ReadReport out;
    out.generation = s.manifest.generation;
    out.path = path;
    out.source = source;
    out.content_id = id;
    auto line = [&](std::string_view text, std::uint64_t number) {
        if (number < first || number > last) return;
        if (out.truncated) return;
        if (out.text.size() + text.size() + 1 > max_bytes) { out.truncated = true;
        return;
        }
        if (!out.first_line) out.first_line = number;
        out.last_line = number;
        out.text.append(text);
        out.text += '\n';
    };
    if (source == "snapshot") read_content(s, *file, line);
    else {
        ensure_source_path(s.manifest.root, path);
        auto full = s.manifest.root / path_from_utf8(path);
        if (!fs::is_regular_file(full)) throw Error("E_STALE_READ", "Workspace file is missing", true);
        Job job{path, storage::checked_file_size(full), timestamp(full), {}};
        try {
            auto got = read_source(s.manifest.root, job, s.manifest.policy.max_line_bytes, line);
            if (got.fingerprint != file->fingerprint || got.crc != file->crc || got.bytes != file->size)
                throw Error("E_STALE_READ", "Workspace content differs from snapshot; update before editing", true);
        } catch (const SkipFile&) { throw Error("E_STALE_READ", "Workspace file is no longer valid indexed text", true);
        }
    }
    return out;
}
BuildReport create_index(const fs::path& root, const fs::path& directory, const BuildOptions& options) { return build(root, directory, options, true);
}
BuildReport update_index(const fs::path& directory) { auto policy = Index(directory).policy();
return build({}, directory, policy, false);
}
BuildReport update_index(const fs::path& directory, const BuildOptions& options) { return build({}, directory, options, false);
}
BuildReport compact_index(const fs::path& directory, std::size_t threads, std::size_t memory) {
    auto policy = Index(directory, memory).policy();
    policy.threads = threads;
    policy.memory_mb = memory;
    return build({}, directory, policy, false, true);
}
GarbageReport garbage_collect(const fs::path& requested, std::size_t keep) {
    if (!keep || keep > 100000) throw Error("E_ARGUMENT", "GC must retain at least one generation");
    auto dir = fs::canonical(requested);
    DirectoryLock writer(dir, "writer.lock");
    DirectoryLock gate(dir, "access.lock", true);
    auto generations = commits(dir);
    if (generations.empty()) throw Error("E_INDEX_NOT_FOUND", "No index to collect");
    std::set<std::uint64_t> retained;
    for (std::size_t i = 0; i < (std::min)(keep, generations.size()); ++i) retained.insert(generations[i]);
    GarbageReport report;
    if (fs::exists(dir / "pins")) for (const auto& item : fs::directory_iterator(dir / "pins")) {
        if (!fs::is_directory(item.symlink_status()) || fs::is_symlink(item.symlink_status())) throw Error("E_LOCKED", "Unrecognized reader pin; offline recovery required");
        auto generation = unsigned_number(disk::read_file(item.path() / "generation", 32));
        if (!fs::exists(commit_path(dir, generation))) throw Error("E_LOCKED", "Reader pin references unavailable snapshot");
        retained.insert(generation);
        ++report.pinned;
    }
    std::set<std::string> reachable;
    for (auto generation : retained) {
        auto m = read_manifest(dir, generation);
        reachable.insert(path_to_utf8(commit_path(dir, generation).filename()));
        for (std::size_t b = 0; b < Buckets; ++b) if (m.catalogs[b]) reachable.insert(path_to_utf8(catalog_path(dir, m.catalogs[b], b).filename()));
        for (auto [id, count] : m.segments) { (void)count;
        reachable.insert(path_to_utf8(segment_path(dir, id).filename()));
        }
    }
    // Unpublish obsolete snapshots first. A crash during deletion must never leave
    // a visible commit whose unpinned objects have already been removed.
    for (auto generation : generations) if (!retained.contains(generation)) {
        auto path = commit_path(dir, generation);
        report.bytes_removed = checked_add(report.bytes_removed, storage::checked_file_size(path));
        fs::remove(path); ++report.generations_removed; ++report.objects_removed;
    }
    for (const auto& item : fs::directory_iterator(dir)) {
        auto name = path_to_utf8(item.path().filename());
        if (reachable.contains(name)) continue;
        std::uint64_t id = 0;
        bool commit = numbered(name, "commit-", ".bin", id);
        bool managed = !commit && (numbered(name, "segment-", "", id) || numbered(name, "transaction-", "", id) ||
            (name.starts_with("catalog-") && name.ends_with(".tbl")) || numbered(name, "commit-", ".bin.tmp", id));
        if (!managed) continue;
        report.bytes_removed = checked_add(report.bytes_removed, object_bytes(item.path()));
        fs::remove_all(item.path());
        ++report.objects_removed;
    }
    return report;
}
void recover_writer_lock(const fs::path& directory) {
    auto p = directory / "writer.lock";
    if (!fs::exists(fs::symlink_status(p))) return;
    if (fs::is_symlink(fs::symlink_status(p)) || !fs::is_directory(p) || !fs::is_empty(p)) throw Error("E_SCOPE", "Unexpected writer.lock contents");
    fs::remove(p);
}
void recover_index(const fs::path& directory, bool clear_reader_pins) {
    recover_writer_lock(directory);
    auto p = directory / "access.lock";
    if (fs::exists(fs::symlink_status(p))) {
        if (fs::is_symlink(fs::symlink_status(p)) || !fs::is_directory(p) || !fs::is_empty(p)) throw Error("E_SCOPE", "Unexpected access.lock contents");
        fs::remove(p);
    }
    if (clear_reader_pins && fs::exists(directory / "pins")) {
        if (fs::is_symlink(fs::symlink_status(directory / "pins"))) throw Error("E_SCOPE", "Refusing symlink pin directory");
        for (const auto& item : fs::directory_iterator(directory / "pins")) {
            if (fs::is_symlink(item.symlink_status())) throw Error("E_SCOPE", "Refusing symlink pin");
            fs::remove_all(item.path());
        }
    }
}
}
