#include "strata.hpp"
#include <arpa/inet.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstring>
#include <deque>
#include <fcntl.h>
#include <iostream>
#include <mutex>
#include <netinet/in.h>
#include <poll.h>
#include <sstream>
#include <stdexcept>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

namespace {
using Clock = std::chrono::steady_clock;
volatile std::sig_atomic_t interrupted = 0;
void interrupt(int) { interrupted = 1; }
constexpr size_t max_line_bytes = 2304;
struct Options {
    std::string directory;
    uint16_t port = 7201;
    size_t workers = 4, queue = 64, max_records = 1000000;
    int timeout_ms = 2000;
    strata::Durability durability = strata::Durability::always;
};
struct Socket {
    int fd;
    explicit Socket(int value) : fd(value) { if (fd < 0) throw std::runtime_error(std::strerror(errno)); }
    ~Socket() { if (fd >= 0) ::close(fd); }
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;
};
Options options(int argc, char** argv) {
    if (argc < 2 || std::string(argv[1]) != "serve")
        throw std::invalid_argument("usage: strata serve --dir PATH [--port 7201] [--workers 4] [--queue 64] [--timeout-ms 2000] [--max-records 1000000] [--durability always|buffered]");
    Options result;
    for (int i = 2; i < argc; i += 2) {
        if (i+1 == argc) throw std::invalid_argument("option requires a value");
        const std::string name = argv[i], value = argv[i+1];
        if (name == "--dir") result.directory = value;
        else if (name == "--port") result.port = static_cast<uint16_t>(strata::parse_uint(value, 65535));
        else if (name == "--workers") result.workers = strata::parse_uint(value, 64);
        else if (name == "--queue") result.queue = strata::parse_uint(value, 4096);
        else if (name == "--max-records") result.max_records = strata::parse_uint(value, 10000000);
        else if (name == "--timeout-ms") result.timeout_ms = static_cast<int>(strata::parse_uint(value, 60000));
        else if (name == "--durability" && value == "always") result.durability = strata::Durability::always;
        else if (name == "--durability" && value == "buffered") result.durability = strata::Durability::buffered;
        else throw std::invalid_argument("unknown option or value: " + name);
    }
    if (result.directory.empty() || result.workers == 0 || result.queue == 0 || result.max_records == 0 || result.timeout_ms == 0)
        throw std::invalid_argument("directory and positive resource limits are required");
    return result;
}
void nonblocking(int fd) {
    const int flags = ::fcntl(fd, F_GETFL);
    if (flags < 0 || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0 || ::fcntl(fd, F_SETFD, FD_CLOEXEC) < 0)
        throw std::runtime_error("cannot configure socket");
}
bool wait_fd(int fd, short events, Clock::time_point deadline) {
    while (true) {
        auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(deadline-Clock::now()).count();
        if (millis <= 0) return false;
        pollfd pfd{fd, events, 0};
        const int result = ::poll(&pfd, 1, static_cast<int>(millis));
        if (result < 0 && errno == EINTR) continue;
        return result > 0 && (pfd.revents & (events | POLLHUP | POLLERR));
    }
}
bool send_all(int fd, const std::string& data, Clock::time_point deadline) {
    size_t done = 0;
    while (done < data.size()) {
        if (!wait_fd(fd, POLLOUT, deadline)) return false;
        const auto n = ::send(fd, data.data()+done, data.size()-done, 0);
        if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) continue;
        if (n <= 0) return false;
        done += static_cast<size_t>(n);
    }
    return true;
}

class Server {
    struct Pending { int fd; Clock::time_point accepted; };
    Options options_;
    strata::Engine engine_;
    Socket listener_;
    std::mutex mutex_;
    std::condition_variable available_;
    std::deque<Pending> queue_;
    std::vector<int> active_;
    std::vector<std::thread> workers_;
    bool stopping_ = false;
    std::atomic<size_t> rejected_{0}, queue_high_water_{0};

    std::string execute(const std::string& line) {
        std::istringstream input(line);
        std::vector<std::string> words;
        for (std::string word; input >> word;) words.push_back(std::move(word));
        try {
            if (words.size() == 1 && words[0] == "PING") return "PONG\n";
            if (words.size() == 1 && words[0] == "SYNC") return "OK " + std::to_string(engine_.sync()) + "\n";
            if (words.size() == 1 && words[0] == "STATS") {
                const auto s = engine_.stats();
                size_t queued, active = 0;
                { std::lock_guard guard(mutex_); queued = queue_.size(); for (auto fd : active_) if (fd >= 0) ++active; }
                return "{\"records\":" + std::to_string(s.records) + ",\"streams\":" + std::to_string(s.streams) +
                    ",\"durable_sequence\":" + std::to_string(s.durable_sequence) + ",\"wal_bytes\":" + std::to_string(s.wal_bytes) +
                    ",\"recovered_tail_bytes\":" + std::to_string(s.recovered_tail_bytes) +
                    ",\"active_connections\":" + std::to_string(active) + ",\"queued_connections\":" + std::to_string(queued) +
                    ",\"queue_high_water\":" + std::to_string(queue_high_water_.load()) +
                    ",\"rejected_connections\":" + std::to_string(rejected_.load()) + "}\n";
            }
            if (words.size() == 4 && words[0] == "PUT") {
                const auto seq = engine_.put(words[1], strata::parse_uint(words[2], strata::max_timestamp), strata::hex_decode(words[3]));
                return "OK " + std::to_string(seq) + "\n";
            }
            if (words.size() == 5 && words[0] == "RANGE") {
                const auto rows = engine_.range(words[1], strata::parse_uint(words[2], strata::max_timestamp),
                    strata::parse_uint(words[3], strata::max_timestamp), strata::parse_uint(words[4], strata::max_query_rows));
                std::string result = "DATA " + std::to_string(rows.size()) + "\n";
                for (const auto& row : rows)
                    result += "ROW " + std::to_string(row.sequence) + " " + std::to_string(row.timestamp) + " " + strata::hex_encode(row.value) + "\n";
                return result + "END\n";
            }
            return "ERR INVALID command or argument count\n";
        } catch (const std::invalid_argument& error) { return std::string("ERR INVALID ") + error.what() + "\n"; }
          catch (const std::exception& error) { return std::string("ERR STORAGE ") + error.what() + "\n"; }
    }

    void serve_client(const Pending& pending) {
        std::string buffer;
        buffer.reserve(max_line_bytes + 4096);
        auto deadline = pending.accepted + std::chrono::milliseconds(options_.timeout_ms);
        while (true) {
            const auto newline = buffer.find('\n');
            if ((newline != std::string::npos && newline > max_line_bytes) ||
                (newline == std::string::npos && buffer.size() > max_line_bytes)) {
                send_all(pending.fd, "ERR TOO_LARGE command exceeds 2304 bytes\n", Clock::now()+std::chrono::milliseconds(100));
                return;
            }
            if (Clock::now() >= deadline) {
                send_all(pending.fd, "ERR TIMEOUT\n", Clock::now()+std::chrono::milliseconds(100));
                return;
            }
            if (newline != std::string::npos) {
                const auto response = execute(buffer.substr(0, newline));
                buffer.erase(0, newline+1);
                // Storage execution is synchronous; this deadline bounds network output, not fsync.
                if (!send_all(pending.fd, response, Clock::now()+std::chrono::milliseconds(options_.timeout_ms))) return;
                deadline = Clock::now()+std::chrono::milliseconds(options_.timeout_ms);
                continue;
            }
            if (!wait_fd(pending.fd, POLLIN, deadline)) continue;
            char bytes[4096];
            const auto n = ::recv(pending.fd, bytes, sizeof(bytes), 0);
            if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) continue;
            if (n <= 0) return;
            buffer.append(bytes, static_cast<size_t>(n));
        }
    }

    void worker(size_t slot) {
        while (true) {
            Pending pending{};
            {
                std::unique_lock guard(mutex_);
                available_.wait(guard, [&] { return stopping_ || !queue_.empty(); });
                if (stopping_) return;
                pending = queue_.front(); queue_.pop_front(); active_[slot] = pending.fd;
            }
            try { serve_client(pending); }
            catch (const std::exception& error) { std::cerr << "connection failed: " << error.what() << '\n'; }
            { std::lock_guard guard(mutex_); active_[slot] = -1; }
            ::close(pending.fd);
        }
    }

    void stop() {
        {
            std::lock_guard guard(mutex_);
            stopping_ = true;
            for (const auto& pending : queue_) ::close(pending.fd);
            queue_.clear();
            for (int fd : active_) if (fd >= 0) ::shutdown(fd, SHUT_RDWR);
        }
        available_.notify_all();
        for (auto& thread : workers_) if (thread.joinable()) thread.join();
    }
public:
    explicit Server(Options options)
        : options_(std::move(options)), engine_(options_.directory, options_.max_records, options_.durability),
          listener_(::socket(AF_INET, SOCK_STREAM, 0)), active_(options_.workers, -1) {
        nonblocking(listener_.fd);
        int reuse = 1;
        if (::setsockopt(listener_.fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse))) throw std::runtime_error("setsockopt failed");
        sockaddr_in address{};
        address.sin_family = AF_INET; address.sin_port = htons(options_.port); address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (::bind(listener_.fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 ||
            ::listen(listener_.fd, static_cast<int>(options_.queue)) != 0) throw std::runtime_error(std::strerror(errno));
        try { for (size_t i = 0; i < options_.workers; ++i) workers_.emplace_back([this, i] { worker(i); }); }
        catch (...) { stop(); throw; }
    }
    ~Server() { stop(); }
    void run() {
        sockaddr_in address{};
        socklen_t size = sizeof(address);
        if (::getsockname(listener_.fd, reinterpret_cast<sockaddr*>(&address), &size) != 0) throw std::runtime_error("getsockname failed");
        std::cout << "READY " << ntohs(address.sin_port) << std::endl;
        while (!interrupted) {
            if (!wait_fd(listener_.fd, POLLIN, Clock::now()+std::chrono::milliseconds(100))) continue;
            const int fd = ::accept(listener_.fd, nullptr, nullptr);
            if (fd < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) continue;
                throw std::runtime_error("accept failed");
            }
            try { nonblocking(fd); } catch (...) { ::close(fd); throw; }
            bool busy;
            {
                std::lock_guard guard(mutex_);
                busy = queue_.size() >= options_.queue;
                if (!busy) {
                    queue_.push_back({fd, Clock::now()});
                    if (queue_.size() > queue_high_water_) queue_high_water_ = queue_.size();
                }
            }
            if (busy) {
                ++rejected_;
                // A small nonblocking best-effort rejection cannot stall the accept loop.
                constexpr char rejected[] = "ERR BUSY\n";
                static_cast<void>(::send(fd, rejected, sizeof(rejected)-1, 0));
                ::close(fd);
            } else available_.notify_one();
        }
    }
};
} // namespace

int main(int argc, char** argv) {
    std::signal(SIGPIPE, SIG_IGN);
    std::signal(SIGTERM, interrupt);
    std::signal(SIGINT, interrupt);
    try { Server server(options(argc, argv)); server.run(); }
    catch (const std::exception& error) { std::cerr << "strata: " << error.what() << '\n'; return 1; }
    return 0;
}
