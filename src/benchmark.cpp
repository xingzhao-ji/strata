#include "strata.hpp"
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <random>
#include <stdexcept>
#include <sys/resource.h>

namespace {
using Clock = std::chrono::steady_clock;
double seconds(Clock::time_point start) { return std::chrono::duration<double>(Clock::now()-start).count(); }
double percentile(std::vector<double> values, double p) {
    std::sort(values.begin(), values.end());
    return values[static_cast<size_t>(static_cast<double>(values.size()-1)*p)];
}
std::string latency(const std::vector<double>& values) {
    return "{\"p50_us\":" + std::to_string(percentile(values, .50)) +
        ",\"p95_us\":" + std::to_string(percentile(values, .95)) +
        ",\"p99_us\":" + std::to_string(percentile(values, .99)) + "}";
}
} // namespace

int main(int argc, char** argv) {
    try {
        if (argc != 5) throw std::invalid_argument("usage: strata-bench EMPTY_DIR RECORDS QUERIES DURABLE_WRITES");
        const std::filesystem::path directory(argv[1]);
        const auto count = strata::parse_uint(argv[2], 10000000);
        const auto queries = strata::parse_uint(argv[3], 100000);
        const auto writes = strata::parse_uint(argv[4], 100000);
        if (count < 10000 || queries < 10 || writes < 10) throw std::invalid_argument("benchmark sample too small");
        if (std::filesystem::exists(directory) && !std::filesystem::is_empty(directory))
            throw std::invalid_argument("benchmark directory must be empty (will not overwrite evidence)");
        constexpr uint64_t stream_count = 128;
        const std::string payload(32, 'x');
        double load_time, sync_time, recovery_time;
        uint64_t wal_bytes;
        {
            strata::Engine engine(directory / "indexed", count, strata::Durability::buffered);
            std::vector<uint64_t> ids(count);
            for (uint64_t i = 0; i < count; ++i) ids[i] = i;
            std::mt19937_64 random(20261023);
            std::shuffle(ids.begin(), ids.end(), random);
            const auto start = Clock::now();
            for (uint64_t id : ids)
                engine.put("sensor" + std::to_string(id%stream_count), (id/stream_count/2)*1000, payload);
            load_time = seconds(start);
            const auto sync_start = Clock::now();
            engine.sync(); sync_time = seconds(sync_start);
            wal_bytes = engine.stats().wal_bytes;
        }
        const auto recovery_start = Clock::now();
        strata::Engine engine(directory / "indexed", count, strata::Durability::buffered);
        recovery_time = seconds(recovery_start);
        std::vector<double> indexed, scanned;
        uint64_t rows_checked = 0, digest = 0;
        std::mt19937_64 random(813492);
        // Warm both paths, then alternate their execution order for each sampled query.
        engine.range("sensor1", 0, 5000, 100);
        engine.scan("sensor1", 0, 5000, 100);
        for (uint64_t i = 0; i < queries; ++i) {
            const std::string stream = "sensor" + std::to_string(random()%135);
            const uint64_t from = (random()%(count/stream_count/2))*1000;
            const uint64_t through = from + (1 + random()%20)*1000;
            const size_t limit = 1 + random()%60;
            std::vector<strata::Record> a, b;
            const auto indexed_query = [&] { const auto start = Clock::now(); a = engine.range(stream, from, through, limit); indexed.push_back(seconds(start)*1e6); };
            const auto scan_query = [&] { const auto start = Clock::now(); b = engine.scan(stream, from, through, limit); scanned.push_back(seconds(start)*1e6); };
            if (i%2) { indexed_query(); scan_query(); } else { scan_query(); indexed_query(); }
            if (a != b) throw std::runtime_error("index and scan returned different complete records");
            rows_checked += a.size();
            for (const auto& row : a) digest ^= row.sequence + row.timestamp;
        }
        std::vector<double> durable;
        double durable_elapsed;
        {
            strata::Engine durable_engine(directory / "durable", writes, strata::Durability::always);
            const auto batch_start = Clock::now();
            for (uint64_t i = 0; i < writes; ++i) {
                const auto start = Clock::now();
                durable_engine.put("durable", i, payload);
                durable.push_back(seconds(start)*1e6);
            }
            durable_elapsed = seconds(batch_start);
            if (durable_engine.stats().durable_sequence != writes) throw std::runtime_error("durable write watermark mismatch");
        }
        rusage usage{};
        if (::getrusage(RUSAGE_SELF, &usage) != 0) throw std::runtime_error("getrusage failed");
#if defined(__APPLE__)
        const auto rss_bytes = usage.ru_maxrss;
#else
        const auto rss_bytes = usage.ru_maxrss * 1024;
#endif
        std::cout << std::fixed << std::setprecision(6)
            << "{\"dataset_records\":" << count << ",\"streams\":" << stream_count << ",\"payload_bytes\":32,"
            << "\"queries\":" << queries << ",\"identical_results\":true,\"rows_compared\":" << rows_checked
            << ",\"result_digest\":" << digest << ",\"wal_bytes\":" << wal_bytes
            << ",\"buffered_load_seconds\":" << load_time << ",\"explicit_sync_seconds\":" << sync_time
            << ",\"recovery_seconds\":" << recovery_time << ",\"peak_rss_bytes\":" << rss_bytes
            << ",\"indexed\":" << latency(indexed) << ",\"full_scan\":" << latency(scanned)
            << ",\"median_query_speedup\":" << percentile(scanned, .50)/percentile(indexed, .50)
            << ",\"durable\":{\"writes\":" << writes << ",\"elapsed_seconds\":" << durable_elapsed
            << ",\"writes_per_second\":" << static_cast<double>(writes)/durable_elapsed
            << ",\"latency\":" << latency(durable) << "}}\n";
    } catch (const std::exception& error) { std::cerr << "benchmark: " << error.what() << '\n'; return 1; }
}
