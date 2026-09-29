#include "../src/proto/protocol.hpp"
#include "../src/proto/socket_io.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <charconv>
#include <exception>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <sstream>
#include <string>
#include <string_view>
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
    kv::send_all(fd, frame.data(), frame.size());
    return kv::receive_frame(fd);
}

void require_response(const std::vector<std::uint8_t>& frame, std::uint8_t expected_type,
                      const std::string& expected_payload);

void worker(const std::string& host, int port, int operations, std::exception_ptr& worker_error,
            std::mutex& error_mutex) {
    int fd = -1;
    try {
        fd = connect_to(host, port);
        for (int i = 0; i < operations; ++i) {
            const std::string key = "stress:" + std::to_string(i % 128);
            const std::string value = "v" + std::to_string(i);
            require_response(send_cmd(fd, {"SET", key, value}), 2, "OK");
            require_response(send_cmd(fd, {"GET", key}), 2, value);
        }
        close(fd);
    } catch (...) {
        if (fd >= 0) close(fd);
        std::lock_guard<std::mutex> lock(error_mutex);
        if (!worker_error) worker_error = std::current_exception();
    }
}

int parse_integer(std::string_view text, const char* option) {
    int value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size()) {
        throw std::invalid_argument(std::string(option) + " must be an integer");
    }
    return value;
}

void require_response(const std::vector<std::uint8_t>& frame, std::uint8_t expected_type,
                      const std::string& expected_payload) {
    if (frame.size() < 5) {
        throw std::runtime_error("incomplete response frame");
    }
    const std::string payload(frame.begin() + 5, frame.end());
    if (frame[4] == 1) {
        throw std::runtime_error(payload);
    }
    if (frame[4] != expected_type || payload != expected_payload) {
        throw std::runtime_error("unexpected response from key-value server");
    }
}

}  // namespace

int main(int argc, char** argv) {
    std::string host = "127.0.0.1";
    int port = 6380;
    int threads = 4;
    int operations = 200;

    std::exception_ptr worker_error;
    std::mutex error_mutex;

    try {
        for (int i = 1; i < argc; ++i) {
            const std::string arg = argv[i];
            if (arg == "--host" && i + 1 < argc) {
                host = argv[++i];
            } else if (arg == "--port" && i + 1 < argc) {
                port = parse_integer(argv[++i], "--port");
            } else if (arg == "--threads" && i + 1 < argc) {
                threads = parse_integer(argv[++i], "--threads");
            } else if (arg == "--ops" && i + 1 < argc) {
                operations = parse_integer(argv[++i], "--ops");
            } else {
                throw std::invalid_argument("unknown or incomplete argument: " + arg);
            }
        }
        if (port < 1 || port > 65535 || threads < 1 || threads > 256 ||
            operations < 1 || operations > 10'000'000) {
            throw std::invalid_argument("port, thread, or operation count is outside the supported range");
        }
        const auto start = std::chrono::steady_clock::now();
        std::vector<std::jthread> workers;
        workers.reserve(static_cast<std::size_t>(threads));
        for (int i = 0; i < threads; ++i) {
            workers.emplace_back(worker, host, port, operations, std::ref(worker_error), std::ref(error_mutex));
        }
        for (auto& t : workers) {
            t.join();
        }
        if (worker_error) std::rethrow_exception(worker_error);
        const auto end = std::chrono::steady_clock::now();
        const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
        std::cout << "threads=" << threads << " ops_per_thread=" << operations << " elapsed_ms=" << elapsed_ms << '\n';
    } catch (const std::exception& error) {
        std::cerr << "stress test failed: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
