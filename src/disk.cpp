#include "researchify/disk.hpp"
#include "researchify/storage.hpp"
#include <algorithm>
#include <array>
#include <fstream>
#include <iterator>
#include <limits>
#include <list>
#include <map>
#include <mutex>
#include <queue>
#include <sstream>
#include <unordered_map>

namespace researchify::disk {
struct DecodedPage {
    bool internal = false;
    std::vector<Entry> entries;
    std::uint64_t next = 0;
};
namespace {
constexpr std::uint64_t header_size = 36;
constexpr std::size_t max_page = 262144, max_field = 65536;
[[noreturn]] void corrupt(const std::string& what) { throw Error("E_INDEX_CORRUPT", what);
}
void fixed32(std::string& out, std::uint32_t v) {
    for (unsigned i = 0; i != 4; ++i) out.push_back(static_cast<char>((v >> (i * 8)) & 255U));
}
std::uint32_t fixed32(std::string_view s, std::size_t at) {
    if (at > s.size() || s.size() - at < 4) corrupt("Truncated 32-bit field");
    std::uint32_t v = 0;
    for (unsigned i = 0; i != 4; ++i) v |= std::uint32_t(static_cast<unsigned char>(s[at + i])) << (i * 8);
    return v;
}
std::ifstream input(const fs::path& p) {
    std::ifstream s(p, std::ios::binary);
    if (!s) throw Error("E_IO", "Cannot open " + path_to_utf8(p));
    return s;
}
std::ofstream output(const fs::path& p) {
    std::ofstream s(p, std::ios::binary | std::ios::trunc);
    if (!s) throw Error("E_IO", "Cannot create " + path_to_utf8(p));
    return s;
}
std::uint64_t position(std::ostream& s) {
    auto p = s.tellp();
    if (p < 0) throw Error("E_IO", "Cannot determine output position");
    return static_cast<std::uint64_t>(p);
}
void bytes(std::ostream& s, std::string_view v) {
    s.write(v.data(), static_cast<std::streamsize>(v.size()));
    if (!s) throw Error("E_IO", "Cannot write index data");
}
void row(std::ostream& out, const Entry& e) {
    storage::put_u32(out, static_cast<std::uint32_t>(e.first.size()));
    storage::put_u32(out, static_cast<std::uint32_t>(e.second.size()));
    storage::put_u32(out, storage::crc32(e.second, storage::crc32(e.first)));
    bytes(out, e.first);
    bytes(out, e.second);
}
bool row(std::istream& in, Entry& e) {
    if (in.peek() == std::char_traits<char>::eof()) {
        if (in.bad()) throw Error("E_IO", "Cannot read temporary run");
        return false;
    }
    auto a = storage::get_u32(in), b = storage::get_u32(in), crc = storage::get_u32(in);
    if (a > max_field || b > max_field) corrupt("Oversized temporary record");
    e.first.resize(a);
    e.second.resize(b);
    in.read(e.first.data(), static_cast<std::streamsize>(a));
    in.read(e.second.data(), static_cast<std::streamsize>(b));
    if (!in || storage::crc32(e.second, storage::crc32(e.first)) != crc) corrupt("Damaged temporary record");
    return true;
}
DecodedPage load_node(const fs::path& p, const PageCache* cache, std::uint64_t offset,
               std::uint64_t size) {
    if (offset < header_size || offset > size || size - offset < 8) corrupt("B+tree page offset out of bounds");
    auto h = cache->read(p, offset, 8);
    auto len = fixed32(*h, 0), crc = fixed32(*h, 4);
    if (len < 2 || len > max_page || len > size - offset - 8) corrupt("B+tree page length out of bounds");
    auto body = cache->read(p, offset + 8, len);
    if (storage::crc32(*body) != crc) corrupt("B+tree page checksum mismatch: " + path_to_utf8(p));
    std::size_t at = 0;
    auto type = static_cast<unsigned char>((*body)[at++]);
    if (type > 1) corrupt("Unknown B+tree page type");
    DecodedPage n;
    n.internal = type == 1;
    n.next = offset + 8 + len;
    auto count = get_varint(*body, at);
    if (count > len / 3 || (n.internal && count == 0)) corrupt("Invalid B+tree entry count");
    n.entries.reserve(static_cast<std::size_t>(count));
    std::string last;
    for (std::uint64_t i = 0; i < count; ++i) {
        auto prefix = get_varint(*body, at);
        auto suffix = get_string(*body, at, max_field);
        if (prefix > last.size() || prefix + suffix.size() > max_field) corrupt("Invalid key prefix compression");
        std::string key = last.substr(0, static_cast<std::size_t>(prefix)) + suffix;
        auto value = get_string(*body, at, max_field);
        if (i && key <= last) corrupt("B+tree keys are not strictly ordered");
        if (n.internal && value.size() != 8) corrupt("Invalid B+tree child descriptor");
        last = key;
        n.entries.emplace_back(std::move(key), std::move(value));
    }
    if (at != body->size()) corrupt("Trailing B+tree page data");
    return n;
}
}
void varint(std::string& out, std::uint64_t v) {
    while (v >= 128) { out.push_back(static_cast<char>((v & 127U) | 128U));
    v >>= 7;
    }
    out.push_back(static_cast<char>(v));
}
std::uint64_t get_varint(std::string_view s, std::size_t& at) {
    std::uint64_t value = 0;
    for (unsigned shift = 0; shift <= 63; shift += 7) {
        if (at >= s.size()) corrupt("Truncated varint");
        auto b = static_cast<unsigned char>(s[at++]);
        if (shift == 63 && b > 1) corrupt("Varint overflow");
        value |= std::uint64_t(b & 127U) << shift;
        if (!(b & 128U)) {
            if (shift && b == 0) corrupt("Noncanonical varint");
            return value;
        }
    }
    corrupt("Varint overflow");
}
void string(std::string& out, std::string_view value) { varint(out, value.size());
out.append(value);
}
std::string get_string(std::string_view data, std::size_t& at, std::size_t maximum) {
    auto len = get_varint(data, at);
    if (len > maximum || at > data.size() || len > data.size() - at) corrupt("String length out of bounds");
    auto value = std::string(data.substr(at, static_cast<std::size_t>(len)));
    at += static_cast<std::size_t>(len);
    return value;
}
void u64(std::string& out, std::uint64_t v) {
    for (unsigned i = 0; i != 8; ++i) out.push_back(static_cast<char>((v >> (i * 8)) & 255U));
}
std::uint64_t get_u64(std::string_view s, std::size_t& at) {
    if (at > s.size() || s.size() - at < 8) corrupt("Truncated 64-bit field");
    std::uint64_t v = 0;
    for (unsigned i = 0; i != 8; ++i) v |= std::uint64_t(static_cast<unsigned char>(s[at++])) << (i * 8);
    return v;
}
std::string ordered(std::uint64_t v) {
    std::string out;
    for (int i = 7; i >= 0; --i) out.push_back(static_cast<char>((v >> (i * 8)) & 255U));
    return out;
}
std::uint64_t ordered(std::string_view v) {
    if (v.size() != 8) corrupt("Invalid ordered integer");
    std::uint64_t out = 0;
    for (char c : v) out = (out << 8) | static_cast<unsigned char>(c);
    return out;
}
void finish(std::ofstream& out, const fs::path& p) {
    out.flush();
    if (!out) throw Error("E_IO", "Cannot flush " + path_to_utf8(p));
    out.close();
    if (out.fail()) throw Error("E_IO", "Cannot close " + path_to_utf8(p));
}
void write_file(const fs::path& p, std::string_view data) {
    auto out = output(p);
    bytes(out, data);
    storage::put_u32(out, storage::crc32(data));
    finish(out, p);
}
std::string read_file(const fs::path& p, std::size_t maximum) {
    auto size = storage::checked_file_size(p);
    if (size < 4 || size - 4 > maximum) corrupt("Invalid checked file size: " + path_to_utf8(p));
    auto in = input(p);
    std::string out(static_cast<std::size_t>(size - 4), '\0');
    in.read(out.data(), static_cast<std::streamsize>(out.size()));
    if (!in || storage::get_u32(in) != storage::crc32(out)) corrupt("File checksum mismatch: " + path_to_utf8(p));
    return out;
}
struct PageCache::Impl {
    explicit Impl(std::size_t b) : maximum(b / 2) {}
    std::size_t maximum, used = 0;
    struct Item { std::shared_ptr<const std::string> data;
    std::list<std::string>::iterator lru;
    std::size_t bytes;
    };
    std::list<std::string> lru;
    std::unordered_map<std::string, Item> pages;
    struct DecodedItem { std::shared_ptr<const DecodedPage> data; std::list<std::string>::iterator lru; std::size_t bytes; };
    std::unordered_map<std::string, DecodedItem> nodes;
    std::list<std::string> node_lru;
    std::size_t node_bytes = 0;
    std::mutex mutex;
};
PageCache::PageCache(std::size_t bytes_) : impl_(std::make_unique<Impl>(bytes_)) {}
PageCache::~PageCache() = default;
std::shared_ptr<const std::string> PageCache::read(const fs::path& p, std::uint64_t off, std::size_t length) const {
    if (length > max_page || off > static_cast<std::uint64_t>((std::numeric_limits<std::streamoff>::max)()))
        corrupt("Read block exceeds format limits");
    auto key = path_to_utf8(p);
    key.push_back('\0');
    u64(key, off);
    u64(key, length);
    auto& c = *impl_;
    std::unique_lock lock(c.mutex);
    auto found = c.pages.find(key);
    if (found != c.pages.end()) {
        c.lru.splice(c.lru.begin(), c.lru, found->second.lru);
        return found->second.data;
    }
    auto in = input(p);
    in.seekg(static_cast<std::streamoff>(off));
    auto data = std::make_shared<std::string>(length, '\0');
    in.read(data->data(), static_cast<std::streamsize>(length));
    if (!in) corrupt("Truncated block: " + path_to_utf8(p));
    const auto cost = length + 2 * key.size() + 256;
    while (!c.lru.empty() && c.used + cost > c.maximum) {
        auto old = c.pages.find(c.lru.back());
        c.used -= old->second.bytes;
        c.pages.erase(old);
        c.lru.pop_back();
    }
    if (cost <= c.maximum) {
        c.lru.push_front(key);
        c.pages.emplace(key, Impl::Item{data, c.lru.begin(), cost});
        c.used += cost;
    }
    return data;
}
std::shared_ptr<const DecodedPage> PageCache::node(const fs::path& path, std::uint64_t offset, std::uint64_t file_size) const {
    auto key = path_to_utf8(path);
    key.push_back('\0');
    u64(key, offset);
    u64(key, file_size);
    auto& c = *impl_;
    {
        std::lock_guard lock(c.mutex);
        auto found = c.nodes.find(key);
        if (found != c.nodes.end()) {
            c.node_lru.splice(c.node_lru.begin(), c.node_lru, found->second.lru);
            return found->second.data;
        }
    }
    auto page = std::make_shared<const DecodedPage>(load_node(path, this, offset, file_size));
    std::size_t cost = sizeof(DecodedPage) + page->entries.capacity() * sizeof(Entry) + key.size() * 2 + 256;
    for (const auto& entry : page->entries) cost += entry.first.capacity() + entry.second.capacity() + 2;
    if (cost > c.maximum) return page;
    std::lock_guard lock(c.mutex);
    if (auto found = c.nodes.find(key); found != c.nodes.end()) return found->second.data;
    while (!c.node_lru.empty() && c.node_bytes + cost > c.maximum) {
        auto victim = c.nodes.find(c.node_lru.back());
        c.node_bytes -= victim->second.bytes;
        c.nodes.erase(victim);
        c.node_lru.pop_back();
    }
    c.node_lru.push_front(key);
    c.nodes.emplace(key, Impl::DecodedItem{page, c.node_lru.begin(), cost});
    c.node_bytes += cost;
    return page;
}
void PageCache::clear() {
    std::lock_guard lock(impl_->mutex);
    impl_->pages.clear();
    impl_->lru.clear();
    impl_->used = 0;
    impl_->nodes.clear();
    impl_->node_lru.clear();
    impl_->node_bytes = 0;
}
struct TableWriter::Impl {
    fs::path path, level;
    std::ofstream out, refs;
    std::vector<Entry> entries;
    std::size_t target, estimated = 0;
    std::uint64_t total = 0, references = 0;
    std::string last;
    bool done = false;
    explicit Impl(const fs::path& p, std::size_t b) : path(p), level(p),
        out(output(p)), target(b) { level = p;
        level += ".level0";
        refs = output(level);
        bytes(out, std::string(header_size, '\0'));
        }
    std::uint64_t page(bool internal, const std::vector<Entry>& items) {
        std::string body(1, internal ? '\1' : '\0');
        varint(body, items.size());
        std::string_view previous;
        for (const auto& [k, v] : items) {
            std::size_t n = 0;
            while (n < previous.size() && n < k.size() && previous[n] == k[n]) ++n;
            varint(body, n);
            string(body, std::string_view(k).substr(n));
            string(body, v);
            previous = k;
        }
        if (body.size() > max_page) throw Error("E_BUDGET", "B+tree page exceeds format bound");
        auto off = position(out);
        storage::put_u32(out, static_cast<std::uint32_t>(body.size()));
        storage::put_u32(out, storage::crc32(body));
        bytes(out, body);
        return off;
    }
    void flush() {
        if (entries.empty() && total) return;
        auto off = page(false, entries);
        std::string value;
        u64(value, off);
        row(refs, {entries.empty() ? std::string() : entries.front().first, value});
        ++references;
        entries.clear();
        estimated = 0;
    }
};
TableWriter::TableWriter(const fs::path& p, std::size_t page) : impl_(std::make_unique<Impl>(p, page)) {
    if (page < 1024 || page > 65536) throw Error("E_ARGUMENT", "Invalid table page target");
}
TableWriter::~TableWriter() = default;
void TableWriter::add(std::string_view k, std::string_view v) {
    auto& w = *impl_;
    if (w.done || k.size() > max_field || v.size() > max_field) throw Error("E_ARGUMENT", "Invalid table record");
    if (w.total && k <= w.last) throw Error("E_ARGUMENT", "Table input is not strictly sorted");
    if (!w.entries.empty() && w.estimated + k.size() + v.size() + 30 > w.target) w.flush();
    w.entries.emplace_back(k, v);
    w.estimated += k.size() + v.size() + 30;
    w.last = k;
    ++w.total;
}
void TableWriter::finish() {
    auto& w = *impl_;
    if (w.done) return;
    if (!w.entries.empty() || !w.total) w.flush();
    disk::finish(w.refs, w.level);
    auto leaves_end = position(w.out);
    std::uint64_t root = 0;
    unsigned level = 0;
    while (true) {
        auto in = input(w.level);
        Entry e;
        if (w.references == 1) { if (!row(in, e)) corrupt("Missing root reference");
        std::size_t at = 0;
        root = get_u64(e.second, at);
        break;
        }
        auto next = w.path;
        next += ".level" + std::to_string(++level);
        auto out = output(next);
        std::vector<Entry> page;
        std::size_t used = 0;
        std::uint64_t count = 0;
        auto flush = [&] {
            if (page.empty()) return;
            std::string value;
            u64(value, w.page(true, page));
            row(out, {page.front().first, value});
            page.clear();
            used = 0;
            ++count;
        };
        while (row(in, e)) {
            // At least two children per internal page guarantees convergence with long keys.
            if (page.size() >= 2 && used + e.first.size() + 40 > w.target) flush();
            used += e.first.size() + 40;
            page.push_back(std::move(e));
        }
        flush();
        disk::finish(out, next);
        in.close();
        fs::remove(w.level);
        w.level = next;
        w.references = count;
    }
    std::string h = "CSTBL2\r\n";
    u64(h, root);
    u64(h, leaves_end);
    u64(h, w.total);
    fixed32(h, storage::crc32(h));
    w.out.seekp(0);
    bytes(w.out, h);
    disk::finish(w.out, w.path);
    fs::remove(w.level);
    w.done = true;
}
struct Table::Impl {
    fs::path path;
    std::shared_ptr<PageCache> cache;
    std::uint64_t size = 0, root = 0, leaves_end = 0, total = 0;
};
Table::Table(const fs::path& p, std::shared_ptr<PageCache> c) : impl_(std::make_shared<Impl>()) {
    auto& t = *impl_;
    t.path = p;
    t.cache = std::move(c);
    t.size = storage::checked_file_size(p);
    if (t.size < header_size + 10) corrupt("Table too small");
    auto h = t.cache->read(p, 0, header_size);
    if (h->substr(0, 8) != "CSTBL2\r\n" || fixed32(*h, 32) != storage::crc32(std::string_view(*h).substr(0, 32)))
        corrupt("Table header checksum or magic mismatch");
    std::size_t at = 8;
    t.root = get_u64(*h, at);
    t.leaves_end = get_u64(*h, at);
    t.total = get_u64(*h, at);
    if (t.root < header_size || t.root >= t.size || t.leaves_end <= header_size || t.leaves_end > t.size || t.total > t.size)
        corrupt("Invalid table header bounds");
}
std::optional<std::string> Table::get(std::string_view key) const {
    const auto& t = *impl_;
    auto off = t.root;
    for (unsigned depth = 0; depth < 32; ++depth) {
        auto page_holder = t.cache->node(t.path, off, t.size);
        const auto& node = *page_holder;
        auto it = std::lower_bound(node.entries.begin(), node.entries.end(), key,
            [](const Entry& e, std::string_view k) { return e.first < k;
            });
        if (!node.internal) return it != node.entries.end() && it->first == key ? std::optional<std::string>(it->second) : std::nullopt;
        if (it == node.entries.end() || it->first != key) {
            if (it == node.entries.begin()) return std::nullopt;
            --it;
        }
        std::size_t at = 0;
        auto child = get_u64(it->second, at);
        if (child >= off) corrupt("Non-decreasing B+tree child offset");
        off = child;
    }
    corrupt("B+tree depth limit exceeded");
}
bool Table::get(std::string_view key, Cursor& cursor,
                const std::function<void(std::string_view)>& visitor) const {
    auto visit = [&](const DecodedPage& page) {
        const auto found = std::lower_bound(page.entries.begin(), page.entries.end(), key,
            [](const Entry& entry, std::string_view wanted) { return entry.first < wanted; });
        if (found == page.entries.end() || found->first != key) return false;
        visitor(found->second);
        return true;
    };
    if (cursor.owner_.get() == impl_.get()) {
        auto page = cursor.leaf_.lock();
        if (page && !page->entries.empty() && key >= page->entries.front().first && key <= page->entries.back().first)
            return visit(*page);
    }
    const auto& table = *impl_;
    auto offset = table.root;
    std::string lower;
    std::optional<std::string> upper;
    for (unsigned depth = 0; depth < 32; ++depth) {
        auto holder = table.cache->node(table.path, offset, table.size);
        const auto& page = *holder;
        if (!page.internal) {
            // A cached leaf is reusable only when its whole key range belongs
            // to this root-to-leaf route. Check separators once when entering
            // the leaf, rather than bypassing a corrupt sibling separator on a
            // later adjacent lookup. Page contents/CRC were checked by node().
            if (!page.entries.empty() && (page.entries.front().first < lower ||
                    (upper && page.entries.back().first >= *upper)))
                corrupt("B+tree leaf exceeds separator bounds");
            cursor.owner_ = impl_;
            cursor.leaf_ = holder;
            return visit(page);
        }
        auto found = std::lower_bound(page.entries.begin(), page.entries.end(), key,
            [](const Entry& entry, std::string_view wanted) { return entry.first < wanted; });
        if (found == page.entries.end() || found->first != key) {
            if (found == page.entries.begin()) return false;
            --found;
        }
        if (found->first > lower) lower = found->first;
        const auto next = std::next(found);
        if (next != page.entries.end() && (!upper || next->first < *upper)) upper = next->first;
        std::size_t at = 0;
        const auto child = get_u64(found->second, at);
        if (child >= offset) corrupt("Non-decreasing B+tree child offset");
        offset = child;
    }
    corrupt("B+tree depth limit exceeded");
}
void Table::each(const std::function<void(std::string_view, std::string_view)>& visitor, std::string_view prefix) const {
    const auto& t = *impl_;
    auto off = header_size;
    std::uint64_t count = 0;
    std::string last;
    while (off < t.leaves_end) {
        auto page_holder = t.cache->node(t.path, off, t.size);
        const auto& node = *page_holder;
        if (node.internal || node.next > t.leaves_end) corrupt("Invalid leaf region");
        for (const auto& [k, v] : node.entries) {
            if (count && k <= last) corrupt("Unsorted leaves");
            last = k;
            ++count;
            if (prefix.empty() || std::string_view(k).starts_with(prefix)) visitor(k, v);
            else if (k > prefix) return;
        }
        off = node.next;
    }
    if (prefix.empty() && count != t.total) corrupt("Table record count mismatch");
}
std::uint64_t Table::verify() const {
    const auto& t = *impl_;
    each([](auto, auto) {});
    // Also validate internal pages not reached by a corrupt root and reject a
    // truncated/appended root. Leaf traversal must cover the leaf area exactly.
    for (auto at = t.leaves_end; at < t.size;) {
        auto page_holder = t.cache->node(t.path, at, t.size);
        const auto& page = *page_holder;
        if (!page.internal) corrupt("Non-internal page after leaf region");
        at = page.next;
    }
    if (t.cache->node(t.path, t.root, t.size)->next != t.size) corrupt("Trailing table data after root");
    std::uint64_t expected_leaf = header_size;
    std::function<std::pair<std::string, std::uint64_t>(std::uint64_t, unsigned)> check;
    check = [&](std::uint64_t off, unsigned depth) -> std::pair<std::string, std::uint64_t> {
        if (depth >= 32) corrupt("B+tree depth exceeded");
        auto page_holder = t.cache->node(t.path, off, t.size);
        const auto& n = *page_holder;
        if (!n.internal) {
            if (off >= t.leaves_end || n.next > t.leaves_end || off != expected_leaf) corrupt("Unreachable, repeated or unordered leaf");
            expected_leaf = n.next;
            return {n.entries.empty() ? std::string() : n.entries.front().first, n.entries.size()};
        }
        std::uint64_t count = 0;
        for (const auto& [k, v] : n.entries) {
            std::size_t at = 0;
            auto child = get_u64(v, at);
            if (child >= off) corrupt("Invalid child link");
            auto sub = check(child, depth + 1);
            if (sub.first != k) corrupt("Child separator mismatch");
            if (sub.second > t.total - (std::min)(count, t.total)) corrupt("Tree count overflow");
            count += sub.second;
        }
        return {n.entries.front().first, count};
    };
    if (check(t.root, 0).second != t.total || expected_leaf != t.leaves_end) corrupt("Root count/coverage mismatch");
    return t.size;
}
std::uint64_t Table::count() const { return impl_->total;
}
struct Sorter::Impl {
    fs::path work;
    std::size_t budget, used = 0, serial = 0;
    Combine combine;
    std::vector<Entry> buffer;
    std::vector<fs::path> runs;
    bool done = false;
    Impl(fs::path p, std::size_t b, Combine c) : work(std::move(p)), budget(b), combine(std::move(c)) { fs::create_directories(work);
    }
    fs::path name() { return work / ("run-" + std::to_string(++serial) + ".bin");
    }
    void sorted(const std::function<void(std::string_view, std::string_view)>& visit) {
        std::sort(buffer.begin(), buffer.end(), [](const Entry& a, const Entry& b) { return a.first < b.first; });
        std::optional<Entry> previous;
        for (auto& e : buffer) {
            if (previous && previous->first == e.first) combine(previous->second, e.second);
            else { if (previous) visit(previous->first, previous->second);
            previous = std::move(e);
            }
        }
        if (previous) visit(previous->first, previous->second);
        std::vector<Entry>().swap(buffer);
        used = 0;
    }
    void flush() {
        if (buffer.empty()) return;
        auto p = name();
        auto out = output(p);
        sorted([&](auto k, auto v) { row(out, {std::string(k), std::string(v)}); });
        disk::finish(out, p);
        runs.push_back(p);
    }
    void merge(const std::vector<fs::path>& group, const std::function<void(std::string_view, std::string_view)>& visit) {
        struct Reader { std::ifstream in;
        Entry current;
        };
        std::vector<Reader> readers;
        readers.reserve(group.size());
        for (const auto& p : group) readers.push_back({input(p), {}});
        auto cmp = [&](std::size_t a, std::size_t b) { return readers[a].current.first > readers[b].current.first;
        };
        std::priority_queue<std::size_t, std::vector<std::size_t>, decltype(cmp)> q(cmp);
        for (std::size_t i = 0; i < readers.size(); ++i) if (row(readers[i].in, readers[i].current)) q.push(i);
        std::optional<Entry> previous;
        while (!q.empty()) {
            auto i = q.top();
            q.pop();
            auto& r = readers[i];
            if (previous && previous->first == r.current.first) combine(previous->second, r.current.second);
            else { if (previous) visit(previous->first, previous->second);
            previous = r.current;
            }
            auto last = r.current.first;
            if (row(r.in, r.current)) { if (r.current.first <= last) corrupt("Unsorted external run");
            q.push(i);
            }
        }
        if (previous) visit(previous->first, previous->second);
    }
};
Sorter::Sorter(const fs::path& p, std::size_t b, Combine c) : impl_(std::make_unique<Impl>(p, b, std::move(c))) {
    if (b < 262144) throw Error("E_BUDGET", "Sort buffer must be at least 256 KiB");
}
Sorter::~Sorter() = default;
void Sorter::add(std::string k, std::string v) {
    auto& s = *impl_;
    if (s.done || k.size() > max_field || v.size() > max_field) throw Error("E_ARGUMENT", "Invalid external-sort record");
    auto cost = k.size() + v.size() + 144;
    if (!s.buffer.empty() && s.used + cost > s.budget) s.flush();
    s.used += cost;
    s.buffer.emplace_back(std::move(k), std::move(v));
}
void Sorter::finish(const std::function<void(std::string_view, std::string_view)>& visitor) {
    auto& s = *impl_;
    if (s.done) throw Error("E_ARGUMENT", "Sorter already finished");
    if (s.runs.empty()) s.sorted(visitor);
    else {
        s.flush();
        while (s.runs.size() > 24) {
            std::vector<fs::path> next;
            for (std::size_t at = 0; at < s.runs.size(); at += 24) {
                auto end = (std::min)(at + 24, s.runs.size());
                std::vector<fs::path> group(s.runs.begin() + static_cast<std::ptrdiff_t>(at), s.runs.begin() + static_cast<std::ptrdiff_t>(end));
                auto p = s.name();
                auto out = output(p);
                s.merge(group, [&](auto k, auto v) { row(out, {std::string(k), std::string(v)}); });
                disk::finish(out, p);
                for (const auto& old : group) fs::remove(old);
                next.push_back(p);
            }
            s.runs = std::move(next);
        }
        s.merge(s.runs, visitor);
        for (const auto& p : s.runs) fs::remove(p);
    }
    s.done = true;
}
std::string posting_value(const Posting& p) { std::string v;
varint(v, p.frequency);
varint(v, p.line);
varint(v, p.column);
return v;
}
Posting posting_value(std::string_view v) {
    Posting p;
    std::size_t at = 0;
    p.frequency = get_varint(v, at);
    p.line = get_varint(v, at);
    p.column = get_varint(v, at);
    if (at != v.size() || !p.frequency || !p.line || !p.column) corrupt("Invalid posting value");
    return p;
}
void combine_posting(std::string& a, const std::string& b) {
    auto x = posting_value(a), y = posting_value(b);
    if (y.frequency > (std::numeric_limits<std::uint64_t>::max)() - x.frequency) corrupt("Posting frequency overflow");
    x.frequency += y.frequency;
    if (std::pair(y.line, y.column) < std::pair(x.line, x.column)) { x.line = y.line;
    x.column = y.column;
    }
    a = posting_value(x);
}
struct PostingWriter::Impl {
    fs::path directory, path;
    std::ofstream out;
    TableWriter terms;
    std::string term;
    std::uint64_t start = 8, df = 0, previous = 0;
    std::vector<Posting> block;
    bool done = false;
    explicit Impl(const fs::path& p) : directory(p), path(p / "postings.bin"), out(output(path)), terms(p / "terms.tbl") { bytes(out, "CSPOST2\n");
    }
    void flush() {
        if (block.empty()) return;
        std::string payload;
        std::uint64_t last = 0;
        for (const auto& p : block) { varint(payload, p.document - last);
        payload += posting_value(p);
        last = p.document;
        }
        std::string h;
        fixed32(h, static_cast<std::uint32_t>(payload.size()));
        fixed32(h, static_cast<std::uint32_t>(block.size()));
        u64(h, block.front().document);
        u64(h, block.back().document);
        fixed32(h, storage::crc32(payload));
        fixed32(h, storage::crc32(h));
        bytes(out, h);
        bytes(out, payload);
        block.clear();
    }
    void end_term() {
        if (term.empty()) return;
        std::string d;
        if (df <= 4) {
            d.push_back(0);
            varint(d, df);
            std::uint64_t last = 0;
            for (const auto& p : block) {
                varint(d, p.document - last);
                d += posting_value(p);
                last = p.document;
            }
            block.clear();
        } else {
            flush();
            d.push_back(1);
            varint(d, start);
            varint(d, position(out));
            varint(d, df);
        }
        terms.add(term, d);
        start = position(out);
        df = 0;
        previous = 0;
    }
};
PostingWriter::PostingWriter(const fs::path& p) : impl_(std::make_unique<Impl>(p)) {}
PostingWriter::~PostingWriter() = default;
void PostingWriter::add(std::string_view key, std::string_view val) {
    auto& w = *impl_;
    if (key.size() < 10 || key[key.size() - 9] != '\0' || w.done) corrupt("Invalid posting sort key");
    auto term = key.substr(0, key.size() - 9);
    auto document = ordered(key.substr(key.size() - 8));
    if (term != w.term) { if (!w.term.empty() && term <= w.term) corrupt("Unsorted terms");
    w.end_term();
    w.term = term;
    }
    if (!document || document <= w.previous) corrupt("Unsorted posting documents");
    auto p = posting_value(val);
    p.document = document;
    w.previous = document;
    ++w.df;
    w.block.push_back(p);
    if (w.block.size() == 128) w.flush();
}
void PostingWriter::finish() {
    auto& w = *impl_;
    if (w.done) return;
    w.end_term();
    disk::finish(w.out, w.path);
    w.terms.finish();
    w.done = true;
}
struct PostingCursor::Impl {
    fs::path path;
    std::shared_ptr<PageCache> cache;
    std::uint64_t position = 0, end = 0, df = 0, previous_block = 0;
    std::vector<Posting> block;
    std::size_t at = 0;
    bool exhausted = false;
};
std::uint64_t posting_count(std::string_view d) {
    if (d.empty() || static_cast<unsigned char>(d.front()) > 1) corrupt("Invalid postings descriptor tag");
    std::size_t at = 1;
    if (d.front() == 1) {
        (void)get_varint(d, at);
        (void)get_varint(d, at);
    }
    const auto count = get_varint(d, at);
    if (!count || (d.front() == 0 && count > 4) || (d.front() == 1 && at != d.size()))
        corrupt("Invalid postings count");
    return count;
}
PostingCursor::PostingCursor(const fs::path& p, std::shared_ptr<PageCache> c, std::string_view d) : impl_(std::make_unique<Impl>()) {
    auto& r = *impl_;
    r.path = p / "postings.bin";
    r.cache = std::move(c);
    r.df = posting_count(d);
    std::size_t at = 1;
    if (d.front() == 0) {
        (void)get_varint(d, at);
        std::uint64_t document = 0;
        for (std::uint64_t i = 0; i < r.df; ++i) {
            const auto delta = get_varint(d, at);
            if (!delta || delta > (std::numeric_limits<std::uint64_t>::max)() - document)
                corrupt("Inline posting delta overflow");
            document += delta;
            Posting value{document, get_varint(d, at), get_varint(d, at), get_varint(d, at)};
            if (!value.frequency || !value.line || !value.column) corrupt("Invalid inline posting");
            r.block.push_back(value);
        }
        if (at != d.size()) corrupt("Trailing inline posting bytes");
    } else {
        r.position = get_varint(d, at);
        r.end = get_varint(d, at);
        (void)get_varint(d, at);
        if (at != d.size() || r.position < 8 || r.end <= r.position || r.end > storage::checked_file_size(r.path))
            corrupt("Invalid postings descriptor");
        if (*r.cache->read(r.path, 0, 8) != "CSPOST2\n") corrupt("Invalid postings magic");
    }
}
PostingCursor::~PostingCursor() = default;
PostingCursor::PostingCursor(PostingCursor&&) noexcept = default;
PostingCursor& PostingCursor::operator=(PostingCursor&&) noexcept = default;
bool PostingCursor::seek(std::uint64_t target) {
    auto& r = *impl_;
    if (r.exhausted) return false;
    for (;;) {
        while (r.at < r.block.size() && r.block[r.at].document < target) ++r.at;
        if (r.at < r.block.size()) return true;
        r.block.clear();
        r.at = 0;
        if (r.position == r.end) { r.exhausted = true;
        return false;
        }
        if (r.position > r.end || r.end - r.position < 32) corrupt("Truncated postings header");
        auto h = r.cache->read(r.path, r.position, 32);
        if (fixed32(*h, 28) != storage::crc32(std::string_view(*h).substr(0, 28))) corrupt("Posting header CRC mismatch");
        auto len = fixed32(*h, 0), count = fixed32(*h, 4), crc = fixed32(*h, 24);
        std::size_t at = 8;
        auto first = get_u64(*h, at), last = get_u64(*h, at);
        if (!count || count > 128 || len < count * 4 || len > 5120 || len > r.end - r.position - 32 ||
            !first || first > last || first <= r.previous_block) corrupt("Invalid postings block bounds");
        r.previous_block = last;
        auto payload_at = r.position + 32;
        r.position += 32 + len;
        if (last < target) continue;
        auto data = r.cache->read(r.path, payload_at, len);
        if (storage::crc32(*data) != crc) corrupt("Posting payload CRC mismatch");
        at = 0;
        std::uint64_t doc = 0;
        for (std::uint32_t i = 0; i < count; ++i) {
            auto delta = get_varint(*data, at);
            if (!delta || delta > (std::numeric_limits<std::uint64_t>::max)() - doc) corrupt("Posting delta overflow");
            doc += delta;
            Posting p;
            p.document = doc;
            p.frequency = get_varint(*data, at);
            p.line = get_varint(*data, at);
            p.column = get_varint(*data, at);
            if (!p.frequency || !p.line || !p.column) corrupt("Zero posting field");
            r.block.push_back(p);
        }
        if (at != data->size() || r.block.front().document != first || r.block.back().document != last) corrupt("Posting block extent mismatch");
    }
}
const Posting& PostingCursor::current() const { if (impl_->at >= impl_->block.size()) throw Error("E_ARGUMENT", "No current posting");
return impl_->block[impl_->at];
}
std::uint64_t PostingCursor::count() const { return impl_->df;
}
std::uint64_t verify_postings(const fs::path& p, std::shared_ptr<PageCache> c) {
    Table terms(p / "terms.tbl", c);
    auto size = terms.verify();
    std::uint64_t expected_offset = 8;
    if (*c->read(p / "postings.bin", 0, 8) != "CSPOST2\n") corrupt("Invalid postings magic");
    terms.each([&](auto, auto d) {
        (void)posting_count(d);
        if (d.front() == 1) {
            std::size_t at = 1;
            auto start = get_varint(d, at), end = get_varint(d, at);
            if (start != expected_offset) corrupt("Non-contiguous posting lists");
            expected_offset = end;
        }
        PostingCursor cursor(p, c, d);
        std::uint64_t doc = 0, count = 0;
        while (cursor.seek(doc)) { ++count;
        auto id = cursor.current().document;
        if (id == (std::numeric_limits<std::uint64_t>::max)()) break;
        doc = id + 1;
        }
        if (count != cursor.count()) corrupt("Posting df mismatch");
    });
    if (expected_offset != storage::checked_file_size(p / "postings.bin")) corrupt("Unreferenced posting bytes");
    return size + expected_offset;
}
}
