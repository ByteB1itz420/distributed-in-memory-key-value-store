#include "../src/proto/protocol.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <numeric>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

struct BenchmarkStats {
    std::uint64_t total_ops = 0;
    double throughput_ops_per_sec = 0.0;
    std::uint64_t p50_us = 0;
    std::uint64_t p99_us = 0;
    std::uint64_t p999_us = 0;
    double elapsed_seconds = 0.0;
};

int connect_to(const std::string& host, int port) {
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        throw std::runtime_error("socket() failed");
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
        close(fd);
        throw std::runtime_error("inet_pton() failed");
    }
    if (connect(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) < 0) {
        close(fd);
        throw std::runtime_error("connect() failed");
    }
    return fd;
}

std::vector<std::uint8_t> send_and_receive(int fd, const std::vector<std::string>& argv) {
    const auto frame = kv::FrameCodec::encode_request(argv);
    if (send(fd, frame.data(), frame.size(), 0) < 0) {
        throw std::runtime_error("send() failed");
    }

    std::array<std::uint8_t, 4096> buffer{};
    const auto n = recv(fd, buffer.data(), buffer.size(), 0);
    if (n <= 0) {
        throw std::runtime_error("recv() failed");
    }
    return std::vector<std::uint8_t>(buffer.begin(), buffer.begin() + n);
}

std::uint64_t percentile_us(const std::vector<std::uint64_t>& samples, double pct) {
    if (samples.empty()) {
        return 0;
    }
    std::vector<std::uint64_t> sorted = samples;
    std::sort(sorted.begin(), sorted.end());
    const std::size_t index = std::min<std::size_t>(sorted.size() - 1, static_cast<std::size_t>(std::ceil(pct * sorted.size())));
    return sorted[index];
}

void run_client(const std::string& host, int port, std::size_t ops_per_client, std::size_t keyspace, std::size_t value_size,
                std::vector<std::uint64_t>& latencies, std::mutex& latencies_mutex) {
    int fd = connect_to(host, port);
    std::vector<std::uint64_t> local_latencies;
    local_latencies.reserve(ops_per_client);

    for (std::size_t i = 0; i < ops_per_client; ++i) {
        const std::string key = "bench:" + std::to_string((i + 1) % keyspace);
        std::string value(value_size, 'x');
        const auto start = std::chrono::steady_clock::now();
        send_and_receive(fd, {"SET", key, value});
        const auto end = std::chrono::steady_clock::now();
        const auto us = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
        local_latencies.push_back(static_cast<std::uint64_t>(us));
    }

    std::lock_guard<std::mutex> lock(latencies_mutex);
    latencies.insert(latencies.end(), local_latencies.begin(), local_latencies.end());
    close(fd);
}

BenchmarkStats run_benchmark(const std::string& host, int port, std::size_t clients, std::size_t ops_per_client,
                            std::size_t keyspace, std::size_t value_size) {
    std::vector<std::uint64_t> latencies;
    latencies.reserve(clients * ops_per_client);
    std::mutex latencies_mutex;

    const auto start = std::chrono::steady_clock::now();
    std::vector<std::thread> threads;
    threads.reserve(clients);
    for (std::size_t i = 0; i < clients; ++i) {
        threads.emplace_back(run_client, host, port, ops_per_client, keyspace, value_size, std::ref(latencies), std::ref(latencies_mutex));
    }
    for (auto& t : threads) {
        t.join();
    }
    const auto end = std::chrono::steady_clock::now();
    const double elapsed = std::chrono::duration<double>(end - start).count();

    BenchmarkStats stats;
    stats.total_ops = static_cast<std::uint64_t>(clients * ops_per_client);
    stats.elapsed_seconds = elapsed;
    stats.throughput_ops_per_sec = elapsed > 0.0 ? static_cast<double>(stats.total_ops) / elapsed : 0.0;
    stats.p50_us = percentile_us(latencies, 0.50);
    stats.p99_us = percentile_us(latencies, 0.99);
    stats.p999_us = percentile_us(latencies, 0.999);
    return stats;
}

void print_usage(const char* prog) {
    std::cerr << "Usage: " << prog << " --host 127.0.0.1 --port 6380 --clients 8 --ops 1000 --keyspace 256 --value-size 64\n";
}

}  // namespace

int main(int argc, char** argv) {
    std::string host = "127.0.0.1";
    int port = 6380;
    std::size_t clients = 8;
    std::size_t ops_per_client = 1000;
    std::size_t keyspace = 256;
    std::size_t value_size = 64;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--host" && i + 1 < argc) {
            host = argv[++i];
        } else if (arg == "--port" && i + 1 < argc) {
            port = std::stoi(argv[++i]);
        } else if (arg == "--clients" && i + 1 < argc) {
            clients = static_cast<std::size_t>(std::stoull(argv[++i]));
        } else if (arg == "--ops" && i + 1 < argc) {
            ops_per_client = static_cast<std::size_t>(std::stoull(argv[++i]));
        } else if (arg == "--keyspace" && i + 1 < argc) {
            keyspace = static_cast<std::size_t>(std::stoull(argv[++i]));
        } else if (arg == "--value-size" && i + 1 < argc) {
            value_size = static_cast<std::size_t>(std::stoull(argv[++i]));
        } else if (arg == "--help") {
            print_usage(argv[0]);
            return 0;
        } else {
            std::cerr << "Unknown argument: " << arg << '\n';
            print_usage(argv[0]);
            return 1;
        }
    }

    try {
        const auto stats = run_benchmark(host, port, clients, ops_per_client, keyspace, value_size);
        std::cout << "clients=" << clients
                  << " ops_per_client=" << ops_per_client
                  << " total_ops=" << stats.total_ops
                  << " elapsed_sec=" << stats.elapsed_seconds
                  << " throughput_ops_per_sec=" << stats.throughput_ops_per_sec
                  << " p50_us=" << stats.p50_us
                  << " p99_us=" << stats.p99_us
                  << " p999_us=" << stats.p999_us
                  << '\n';
    } catch (const std::exception& ex) {
        std::cerr << "benchmark failed: " << ex.what() << '\n';
        return 1;
    }

    return 0;
}
