#include "../src/proto/protocol.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <chrono>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

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

std::vector<std::uint8_t> send_cmd(int fd, const std::vector<std::string>& argv) {
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

void worker(const std::string& host, int port, int operations) {
    const int fd = connect_to(host, port);
    for (int i = 0; i < operations; ++i) {
        const std::string key = "stress:" + std::to_string(i % 128);
        const std::string value = "v" + std::to_string(i);
        send_cmd(fd, {"SET", key, value});
        send_cmd(fd, {"GET", key});
    }
    close(fd);
}

}  // namespace

int main(int argc, char** argv) {
    std::string host = "127.0.0.1";
    int port = 6380;
    int threads = 4;
    int operations = 200;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--host" && i + 1 < argc) {
            host = argv[++i];
        } else if (arg == "--port" && i + 1 < argc) {
            port = std::stoi(argv[++i]);
        } else if (arg == "--threads" && i + 1 < argc) {
            threads = std::stoi(argv[++i]);
        } else if (arg == "--ops" && i + 1 < argc) {
            operations = std::stoi(argv[++i]);
        }
    }

    const auto start = std::chrono::steady_clock::now();
    std::vector<std::thread> workers;
    workers.reserve(threads);
    for (int i = 0; i < threads; ++i) {
        workers.emplace_back(worker, host, port, operations);
    }
    for (auto& t : workers) {
        t.join();
    }
    const auto end = std::chrono::steady_clock::now();
    const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
    std::cout << "threads=" << threads << " ops_per_thread=" << operations << " elapsed_ms=" << elapsed_ms << '\n';
    return 0;
}
