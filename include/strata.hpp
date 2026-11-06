#pragma once
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <shared_mutex>
#include <string>
#include <vector>

namespace strata {
constexpr uint64_t max_timestamp = INT64_MAX;
constexpr size_t max_value_bytes = 1024;
constexpr size_t max_query_rows = 10000;
enum class Durability { always, buffered };

struct Record {
    uint64_t sequence;
    uint64_t timestamp;
    std::string stream;
    std::string value;
    bool operator==(const Record&) const = default;
};
struct Stats {
    size_t records, streams;
    uint64_t durable_sequence, wal_bytes, recovered_tail_bytes;
};

// A directory has exactly one Engine owner. Readers share the index lock;
// appends serialize WAL publication and index publication under its write lock.
class Engine {
public:
    Engine(const std::filesystem::path&, size_t max_records = 1000000,
           Durability durability = Durability::always);
    ~Engine();
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;
    uint64_t put(std::string stream, uint64_t timestamp, std::string value);
    std::vector<Record> range(const std::string& stream, uint64_t from,
                              uint64_t through, size_t limit) const;
    // Independent baseline used by the benchmark; deliberately scans all records.
    std::vector<Record> scan(const std::string& stream, uint64_t from,
                             uint64_t through, size_t limit) const;
    uint64_t sync();
    Stats stats() const;
    static bool valid_stream(const std::string&);
private:
    struct File;
    std::unique_ptr<File> directory_, lock_, wal_;
    size_t max_records_;
    Durability durability_;
    mutable std::shared_mutex mutex_;
    std::vector<std::unique_ptr<Record>> records_;
    using Key = std::pair<uint64_t, uint64_t>;
    std::map<std::string, std::map<Key, const Record*>> streams_;
    uint64_t durable_sequence_ = 0, wal_bytes_ = 0, recovered_tail_bytes_ = 0;
    bool poisoned_ = false;
    void replay();
    void publish(std::unique_ptr<Record>);
    void healthy() const;
};
uint64_t parse_uint(const std::string&, uint64_t maximum);
std::string hex_encode(const std::string&);
std::string hex_decode(const std::string&);
} // namespace strata
