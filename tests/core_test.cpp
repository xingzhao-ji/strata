#include "strata.hpp"
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <unistd.h>

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
int main() {
    char pattern[] = "/tmp/strata-core-XXXXXX";
    const char* name = ::mkdtemp(pattern);
    if (!name) return 1;
    const std::filesystem::path directory(name);
    try {
        {
            strata::Engine engine(directory, 1000, strata::Durability::buffered);
            for (uint64_t i = 0; i < 1000; ++i)
                engine.put("stream" + std::to_string(i%7), (i*97)%101, std::string("\0\xff", 2));
            for (uint64_t low = 0; low <= 105; low += 5)
                for (size_t stream = 0; stream < 8; ++stream)
                    for (size_t limit : {1u, 3u, 1000u})
                        require(engine.range("stream" + std::to_string(stream), low, low+10, limit) ==
                                engine.scan("stream" + std::to_string(stream), low, low+10, limit),
                                "ordered index differs from scan baseline");
            require(engine.stats().durable_sequence == 0, "buffered durability advanced prematurely");
            require(engine.sync() == 1000, "sync did not establish complete watermark");
        }
        {
            strata::Engine engine(directory, 1000);
            const auto rows = engine.range("stream0", 0, 100, 1000);
            require(rows.size() == 143, "replay lost records");
            require(rows.front().timestamp == 0 && rows.front().sequence == 1, "wrong boundary ordering");
            require(rows.front().value == std::string("\0\xff", 2), "binary payload was not preserved");
            require(engine.stats().durable_sequence == 1000, "recovery watermark is incorrect");
        }
        std::filesystem::remove_all(directory);
        std::cout << "core: 528 indexed/scan comparisons and binary replay passed\n";
    } catch (const std::exception& error) {
        std::filesystem::remove_all(directory);
        std::cerr << error.what() << '\n';
        return 1;
    }
}
