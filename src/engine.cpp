#include "strata.hpp"
#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

namespace strata {
namespace {
constexpr size_t file_header_size = 16, frame_header_size = 36;
using Bytes = std::vector<uint8_t>;
[[noreturn]] void io_error(const char* operation) {
    throw std::runtime_error(std::string(operation) + ": " + std::strerror(errno));
}
[[noreturn]] void corrupt(uint64_t offset, const char* reason) {
    throw std::runtime_error("corrupt WAL at byte " + std::to_string(offset) + ": " + reason);
}
uint32_t crc32c(const uint8_t* data, size_t size) {
    static const auto table = [] {
        std::array<uint32_t, 256> result{};
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t crc = i;
            for (int bit = 0; bit < 8; ++bit)
                crc = (crc >> 1) ^ ((crc & 1) ? 0x82f63b78u : 0u);
            result[i] = crc;
        }
        return result;
    }();
    uint32_t crc = ~0u;
    for (size_t i = 0; i < size; ++i) crc = (crc >> 8) ^ table[(crc ^ data[i]) & 255];
    return ~crc;
}
void store(Bytes& data, size_t offset, uint64_t value, size_t width) {
    for (size_t i = 0; i < width; ++i) data[offset+i] = static_cast<uint8_t>(value >> (i*8));
}
uint64_t load(const uint8_t* data, size_t width) {
    uint64_t value = 0;
    for (size_t i = 0; i < width; ++i) value |= uint64_t(data[i]) << (i*8);
    return value;
}
void write_all(int fd, const Bytes& data) {
    size_t done = 0;
    while (done < data.size()) {
        const auto n = ::write(fd, data.data()+done, data.size()-done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) io_error("write WAL");
        done += static_cast<size_t>(n);
    }
}
void read_at(int fd, uint8_t* target, size_t size, uint64_t offset) {
    size_t done = 0;
    while (done < size) {
        const auto n = ::pread(fd, target+done, size-done, static_cast<off_t>(offset+done));
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) io_error("read WAL");
        if (n == 0) throw std::runtime_error("WAL changed during recovery");
        done += static_cast<size_t>(n);
    }
}
void flush(int fd) {
    while (::fsync(fd) != 0) { if (errno != EINTR) io_error("fsync"); }
}
void check_query(const std::string& stream, uint64_t from, uint64_t through, size_t limit) {
    if (!Engine::valid_stream(stream) || from > through || through > max_timestamp ||
        limit == 0 || limit > max_query_rows) throw std::invalid_argument("invalid range");
}
Bytes frame(const Record& record) {
    Bytes bytes(frame_header_size + record.stream.size() + record.value.size() + 4);
    std::memcpy(bytes.data(), "REC1", 4);
    store(bytes, 4, bytes.size(), 4);
    store(bytes, 8, record.sequence, 8);
    store(bytes, 16, record.timestamp, 8);
    store(bytes, 24, record.stream.size(), 4);
    store(bytes, 28, record.value.size(), 4);
    store(bytes, 32, crc32c(bytes.data(), 32), 4);
    std::memcpy(bytes.data()+36, record.stream.data(), record.stream.size());
    std::memcpy(bytes.data()+36+record.stream.size(), record.value.data(), record.value.size());
    store(bytes, bytes.size()-4, crc32c(bytes.data(), bytes.size()-4), 4);
    return bytes;
}
} // namespace

struct Engine::File {
    int fd;
    explicit File(int value) : fd(value) { if (fd < 0) io_error("open storage path"); }
    ~File() { if (fd >= 0) ::close(fd); }
};

bool Engine::valid_stream(const std::string& name) {
    if (name.empty() || name.size() > 64) return false;
    return std::all_of(name.begin(), name.end(), [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
               (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.';
    });
}

Engine::Engine(const std::filesystem::path& path, size_t maximum, Durability durability)
    : max_records_(maximum), durability_(durability) {
    if (maximum == 0 || maximum > 10000000) throw std::invalid_argument("invalid record capacity");
    const bool created = std::filesystem::create_directories(path);
    directory_ = std::make_unique<File>(::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
    if (created) {
        // The new database name (and any newly created ancestors) must survive too.
        auto parent = std::filesystem::absolute(path).parent_path();
        while (!parent.empty()) {
            File ancestor(::open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
            flush(ancestor.fd);
            if (parent == parent.root_path()) break;
            parent = parent.parent_path();
        }
    }
    lock_ = std::make_unique<File>(::openat(directory_->fd, "writer.lock", O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600));
    struct stat info{};
    if (::fstat(lock_->fd, &info) != 0) io_error("stat lock");
    if (!S_ISREG(info.st_mode)) throw std::runtime_error("lock must be a regular file");
    if (::flock(lock_->fd, LOCK_EX | LOCK_NB) != 0) throw std::runtime_error("database locked by another writer");
    wal_ = std::make_unique<File>(::openat(directory_->fd, "data.wal", O_CREAT | O_RDWR | O_APPEND | O_CLOEXEC | O_NOFOLLOW, 0600));
    if (::fstat(wal_->fd, &info) != 0) io_error("stat WAL");
    if (!S_ISREG(info.st_mode)) throw std::runtime_error("WAL must be a regular file");
    wal_bytes_ = static_cast<uint64_t>(info.st_size);
    if (wal_bytes_ == 0) {
        Bytes header(file_header_size);
        std::memcpy(header.data(), "STRATA01", 8);
        store(header, 8, 1, 4);
        store(header, 12, crc32c(header.data(), 12), 4);
        write_all(wal_->fd, header);
        flush(wal_->fd);
        wal_bytes_ = header.size();
    }
    // Persist both file entries before advertising readiness or acknowledging writes.
    flush(directory_->fd);
    replay();
}
Engine::~Engine() = default;

void Engine::publish(std::unique_ptr<Record> record) {
    const auto* pointer = record.get();
    records_.push_back(std::move(record));
    streams_[pointer->stream].emplace(Key{pointer->timestamp, pointer->sequence}, pointer);
}

void Engine::replay() {
    if (wal_bytes_ < file_header_size) corrupt(0, "incomplete file header");
    Bytes header(file_header_size);
    read_at(wal_->fd, header.data(), header.size(), 0);
    if (std::memcmp(header.data(), "STRATA01", 8) || load(header.data()+8, 4) != 1 ||
        load(header.data()+12, 4) != crc32c(header.data(), 12)) corrupt(0, "file header");
    uint64_t offset = file_header_size;
    while (offset < wal_bytes_) {
        const uint64_t remaining = wal_bytes_ - offset;
        Bytes bytes(static_cast<size_t>(std::min<uint64_t>(remaining, frame_header_size)));
        read_at(wal_->fd, bytes.data(), bytes.size(), offset);
        if (std::memcmp(bytes.data(), "REC1", std::min<size_t>(4, bytes.size()))) corrupt(offset, "frame magic");
        if (remaining < frame_header_size) break;
        if (load(bytes.data()+32, 4) != crc32c(bytes.data(), 32)) corrupt(offset, "header checksum");
        const auto total = load(bytes.data()+4, 4);
        const auto stream_size = load(bytes.data()+24, 4), value_size = load(bytes.data()+28, 4);
        if (stream_size == 0 || stream_size > 64 || value_size == 0 || value_size > max_value_bytes ||
            total != frame_header_size + stream_size + value_size + 4) corrupt(offset, "frame lengths");
        if (remaining < total) break;
        bytes.resize(static_cast<size_t>(total));
        read_at(wal_->fd, bytes.data()+frame_header_size, bytes.size()-frame_header_size, offset+frame_header_size);
        if (load(bytes.data()+total-4, 4) != crc32c(bytes.data(), bytes.size()-4)) corrupt(offset, "record checksum");
        Record record{load(bytes.data()+8, 8), load(bytes.data()+16, 8),
                      std::string(reinterpret_cast<char*>(bytes.data()+36), stream_size),
                      std::string(reinterpret_cast<char*>(bytes.data()+36+stream_size), value_size)};
        if (record.sequence != records_.size()+1 || record.timestamp > max_timestamp || !valid_stream(record.stream))
            corrupt(offset, "invalid sequence, timestamp or stream");
        if (records_.size() >= max_records_) throw std::runtime_error("WAL exceeds configured record capacity");
        publish(std::make_unique<Record>(std::move(record)));
        offset += total;
    }
    if (offset != wal_bytes_) {
        recovered_tail_bytes_ = wal_bytes_ - offset;
        if (::ftruncate(wal_->fd, static_cast<off_t>(offset)) != 0) io_error("truncate torn tail");
        flush(wal_->fd);
        wal_bytes_ = offset;
    }
    // Recovery fsync also establishes a durable boundary for buffered writes that survived.
    flush(wal_->fd);
    durable_sequence_ = records_.size();
}

void Engine::healthy() const {
    if (poisoned_) throw std::runtime_error("STORAGE_UNAVAILABLE restart and inspect WAL");
}

uint64_t Engine::put(std::string stream, uint64_t timestamp, std::string value) {
    if (!valid_stream(stream) || timestamp > max_timestamp || value.empty() || value.size() > max_value_bytes)
        throw std::invalid_argument("invalid record");
    std::unique_lock guard(mutex_);
    healthy();
    if (records_.size() >= max_records_) throw std::runtime_error("CAPACITY record limit reached");
    auto record = std::make_unique<Record>(Record{records_.size()+1, timestamp, std::move(stream), std::move(value)});
    const auto bytes = frame(*record);
    const uint64_t sequence = record->sequence;
    try {
        write_all(wal_->fd, bytes);
        if (durability_ == Durability::always) flush(wal_->fd);
        publish(std::move(record));
        wal_bytes_ += bytes.size();
        if (durability_ == Durability::always) durable_sequence_ = sequence;
    } catch (...) {
        // A write, fsync or allocation failure can leave uncertain on-disk state.
        // Never append past it or acknowledge a partially published index.
        poisoned_ = true;
        throw;
    }
    return sequence;
}

std::vector<Record> Engine::range(const std::string& stream, uint64_t from, uint64_t through, size_t limit) const {
    check_query(stream, from, through, limit);
    std::shared_lock guard(mutex_);
    healthy();
    std::vector<Record> rows;
    const auto found = streams_.find(stream);
    if (found == streams_.end()) return rows;
    for (auto it = found->second.lower_bound(Key{from, 0});
         it != found->second.end() && it->first.first <= through && rows.size() < limit; ++it)
        rows.push_back(*it->second);
    return rows;
}

std::vector<Record> Engine::scan(const std::string& stream, uint64_t from, uint64_t through, size_t limit) const {
    check_query(stream, from, through, limit);
    std::shared_lock guard(mutex_);
    healthy();
    std::vector<Record> rows;
    for (const auto& record : records_)
        if (record->stream == stream && record->timestamp >= from && record->timestamp <= through)
            rows.push_back(*record);
    std::sort(rows.begin(), rows.end(), [](const Record& a, const Record& b) {
        return Key{a.timestamp, a.sequence} < Key{b.timestamp, b.sequence};
    });
    if (rows.size() > limit) rows.resize(limit);
    return rows;
}

uint64_t Engine::sync() {
    std::unique_lock guard(mutex_);
    healthy();
    try { flush(wal_->fd); } catch (...) { poisoned_ = true; throw; }
    durable_sequence_ = records_.size();
    return durable_sequence_;
}
Stats Engine::stats() const {
    std::shared_lock guard(mutex_);
    healthy();
    return {records_.size(), streams_.size(), durable_sequence_, wal_bytes_, recovered_tail_bytes_};
}

uint64_t parse_uint(const std::string& text, uint64_t maximum) {
    uint64_t value = 0;
    if (text.empty() || text[0] < '0' || text[0] > '9') throw std::invalid_argument("expected unsigned decimal");
    const auto [end, error] = std::from_chars(text.data(), text.data()+text.size(), value);
    if (error != std::errc{} || end != text.data()+text.size() || value > maximum)
        throw std::invalid_argument("unsigned decimal out of range");
    return value;
}
std::string hex_encode(const std::string& value) {
    static constexpr char alphabet[] = "0123456789abcdef";
    std::string result;
    result.reserve(value.size()*2);
    for (char character : value) {
        const auto c = static_cast<unsigned char>(character);
        result += alphabet[c >> 4]; result += alphabet[c & 15];
    }
    return result;
}
std::string hex_decode(const std::string& hex) {
    if (hex.empty() || hex.size()%2 || hex.size() > max_value_bytes*2) throw std::invalid_argument("invalid hex payload size");
    const auto nibble = [](char c) -> unsigned {
        if (c >= '0' && c <= '9') return static_cast<unsigned>(c-'0');
        if (c >= 'a' && c <= 'f') return static_cast<unsigned>(c-'a'+10);
        if (c >= 'A' && c <= 'F') return static_cast<unsigned>(c-'A'+10);
        throw std::invalid_argument("invalid hex payload");
    };
    std::string result;
    for (size_t i = 0; i < hex.size(); i += 2) result += static_cast<char>((nibble(hex[i]) << 4) | nibble(hex[i+1]));
    return result;
}
} // namespace strata
