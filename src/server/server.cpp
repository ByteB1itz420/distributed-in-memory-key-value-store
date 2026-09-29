#include "server.hpp"

#include "../proto/protocol.hpp"
#include "../store/store.hpp"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <iostream>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace kv {
namespace {

struct ConnectionState {
    std::vector<std::uint8_t> inbuf;
    std::vector<std::uint8_t> outbuf;
};

int make_listen_socket(int port) {
    const int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (fd < 0) {
        throw std::runtime_error("socket() failed");
    }

    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(static_cast<uint16_t>(port));

    if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        close(fd);
        throw std::runtime_error("bind() failed");
    }

    if (listen(fd, 128) < 0) {
        close(fd);
        throw std::runtime_error("listen() failed");
    }
    return fd;
}

std::vector<std::uint8_t> process_command(Store& store, const std::vector<std::string>& argv) {
    if (argv.empty()) {
        return FrameCodec::encode_response_error("ERR empty command");
    }

    const std::string& cmd = argv[0];
    if (cmd == "PING") {
        return FrameCodec::encode_response_ok();
    }
    if (cmd == "SET") {
        if (argv.size() < 3) {
            return FrameCodec::encode_response_error("ERR wrong number of arguments for SET");
        }
        const auto ttl = (argv.size() >= 4) ? std::optional<std::int64_t>(std::stoll(argv[3])) : std::nullopt;
        store.set(argv[1], argv[2], ttl);
        return FrameCodec::encode_response_ok();
    }
    if (cmd == "GET") {
        if (argv.size() != 2) {
            return FrameCodec::encode_response_error("ERR wrong number of arguments for GET");
        }
        const auto value = store.get(argv[1]);
        if (!value.has_value()) {
            return FrameCodec::encode_response_nil();
        }
        return FrameCodec::encode_response_string(value.value());
    }
    if (cmd == "DEL") {
        if (argv.size() != 2) {
            return FrameCodec::encode_response_error("ERR wrong number of arguments for DEL");
        }
        const bool ok = store.del(argv[1]);
        return FrameCodec::encode_response_int(ok ? 1 : 0);
    }
    if (cmd == "EXISTS") {
        if (argv.size() != 2) {
            return FrameCodec::encode_response_error("ERR wrong number of arguments for EXISTS");
        }
        return FrameCodec::encode_response_int(store.exists(argv[1]) ? 1 : 0);
    }
    if (cmd == "EXPIRE") {
        if (argv.size() != 3) {
            return FrameCodec::encode_response_error("ERR wrong number of arguments for EXPIRE");
        }
        return FrameCodec::encode_response_int(store.expire(argv[1], std::stoll(argv[2])) ? 1 : 0);
    }
    if (cmd == "TTL") {
        if (argv.size() != 2) {
            return FrameCodec::encode_response_error("ERR wrong number of arguments for TTL");
        }
        return FrameCodec::encode_response_int(store.ttl(argv[1]));
    }
    if (cmd == "PERSIST") {
        if (argv.size() != 2) {
            return FrameCodec::encode_response_error("ERR wrong number of arguments for PERSIST");
        }
        return FrameCodec::encode_response_int(store.persist(argv[1]) ? 1 : 0);
    }
    if (cmd == "INCR") {
        if (argv.size() != 2) {
            return FrameCodec::encode_response_error("ERR wrong number of arguments for INCR");
        }
        return FrameCodec::encode_response_int(store.incr(argv[1]));
    }
    if (cmd == "DECR") {
        if (argv.size() != 2) {
            return FrameCodec::encode_response_error("ERR wrong number of arguments for DECR");
        }
        return FrameCodec::encode_response_int(store.decr(argv[1]));
    }
    if (cmd == "KEYS") {
        auto values = store.keys();
        return FrameCodec::encode_response_array(values);
    }
    if (cmd == "INFO") {
        return FrameCodec::encode_response_string(store.info());
    }
    if (cmd == "QUIT") {
        return FrameCodec::encode_response_ok();
    }
    return FrameCodec::encode_response_error("ERR unknown command");
}

void drain_output(int fd, ConnectionState& state) {
    while (!state.outbuf.empty()) {
        const auto sent = send(fd, state.outbuf.data(), state.outbuf.size(), 0);
        if (sent < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            }
            throw std::runtime_error("send() failed");
        }
        state.outbuf.erase(state.outbuf.begin(), state.outbuf.begin() + sent);
        if (sent == 0) {
            break;
        }
    }
}

bool handle_client(Store& store, int fd, ConnectionState& state) {
    std::array<std::uint8_t, 4096> buffer{};
    for (;;) {
        const auto n = recv(fd, buffer.data(), buffer.size(), 0);
        if (n == 0) {
            return false;
        }
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                if (!state.outbuf.empty()) {
                    drain_output(fd, state);
                }
                break;
            }
            throw std::runtime_error("recv() failed");
        }

        state.inbuf.insert(state.inbuf.end(), buffer.begin(), buffer.begin() + n);

        std::size_t offset = 0;
        while (true) {
            const auto request = FrameCodec::decode_one(state.inbuf, offset);
            if (!request.has_value()) {
                break;
            }
            const auto response = process_command(store, request->argv);
            state.outbuf.insert(state.outbuf.end(), response.begin(), response.end());
            if (offset == state.inbuf.size()) {
                state.inbuf.clear();
                break;
            }
            std::vector<std::uint8_t> remaining(state.inbuf.begin() + static_cast<std::ptrdiff_t>(offset), state.inbuf.end());
            state.inbuf = std::move(remaining);
            offset = 0;
        }

        if (!state.outbuf.empty()) {
            drain_output(fd, state);
        }
    }

    return true;
}

struct WorkerQueue {
    std::mutex mutex;
    std::condition_variable cv;
    std::deque<int> pending;
};

void accept_loop(int listen_fd, std::vector<WorkerQueue>& queues, std::atomic<std::size_t>& next_worker) {
    for (;;) {
        sockaddr_in client_addr{};
        socklen_t len = sizeof(client_addr);
        const int client_fd = accept(listen_fd, reinterpret_cast<sockaddr*>(&client_addr), &len);
        if (client_fd < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;
            }
            throw std::runtime_error("accept() failed");
        }

        const std::size_t queue_index = next_worker.fetch_add(1, std::memory_order_relaxed) % queues.size();
        {
            std::lock_guard<std::mutex> lock(queues[queue_index].mutex);
            queues[queue_index].pending.push_back(client_fd);
        }
        queues[queue_index].cv.notify_one();
    }
}

void worker_loop(Store& store, WorkerQueue& queue) {
    for (;;) {
        std::unique_lock<std::mutex> lock(queue.mutex);
        queue.cv.wait(lock, [&queue]() { return !queue.pending.empty(); });

        std::deque<int> batch;
        batch.swap(queue.pending);
        lock.unlock();

        while (!batch.empty()) {
            const int fd = batch.front();
            batch.pop_front();
            try {
                ConnectionState state;
                if (!handle_client(store, fd, state)) {
                    close(fd);
                }
            } catch (const std::exception&) {
                close(fd);
            }
        }
    }
}

}  // namespace

Server::Server(ServerConfig config) : config_(std::move(config)) {}

void Server::run() {
    Store store(config_.maxmemory, config_.maxmemory_policy);
    const int listen_fd = make_listen_socket(config_.port);
    const std::size_t worker_count = std::max<std::size_t>(1, config_.threads);

    std::vector<WorkerQueue> queues(worker_count);
    std::atomic<std::size_t> next_worker{0};

    for (std::size_t i = 0; i < worker_count; ++i) {
        std::thread(worker_loop, std::ref(store), std::ref(queues[i])).detach();
    }

    accept_loop(listen_fd, queues, next_worker);
}

}  // namespace kv
