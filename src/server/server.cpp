#include "server.hpp"

#include "../proto/protocol.hpp"
#include "../repl/backlog.hpp"
#include "../store/store.hpp"

#include <arpa/inet.h>
#include <errno.h>
#include <sys/eventfd.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <cstring>
#include <deque>
#include <iostream>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace kv {
namespace {

struct ConnectionState {
    std::vector<std::uint8_t> inbuf;
    std::vector<std::uint8_t> outbuf;
    bool peer_closed = false;
};

int make_listen_socket(const std::string& host, int port) {
    if (port < 1 || port > 65535) {
        throw std::invalid_argument("port must be between 1 and 65535");
    }
    const int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (fd < 0) {
        throw std::runtime_error("socket() failed");
    }

    int opt = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        close(fd);
        throw std::runtime_error("setsockopt(SO_REUSEADDR) failed");
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
        close(fd);
        throw std::runtime_error("invalid listen address");
    }
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

std::int64_t parse_integer(std::string_view text) {
    std::int64_t value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size()) {
        throw std::invalid_argument("value is not a valid integer");
    }
    return value;
}

std::vector<std::uint8_t> process_command(Store& store, ReplBacklog* backlog, const std::vector<std::string>& argv) {
    if (argv.empty()) {
        return FrameCodec::encode_response_error("ERR empty command");
    }

    const std::string& cmd = argv[0];
    if (cmd == "PING") {
        if (argv.size() != 1) {
            return FrameCodec::encode_response_error("ERR wrong number of arguments for PING");
        }
        return FrameCodec::encode_response_ok();
    }
    if (cmd == "SET") {
        if (argv.size() != 3 && argv.size() != 4) {
            return FrameCodec::encode_response_error("ERR wrong number of arguments for SET");
        }
        const auto ttl = (argv.size() == 4) ? std::optional<std::int64_t>(parse_integer(argv[3])) : std::nullopt;
        if (!store.set(argv[1], argv[2], ttl)) {
            return FrameCodec::encode_response_error("OOM command not allowed when used memory exceeds maxmemory");
        }
        if (backlog != nullptr) {
            backlog->append(argv);
        }
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
        if (backlog != nullptr && ok) {
            backlog->append(argv);
        }
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
        const bool ok = store.expire(argv[1], parse_integer(argv[2]));
        if (backlog != nullptr && ok) {
            backlog->append(argv);
        }
        return FrameCodec::encode_response_int(ok ? 1 : 0);
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
        const bool ok = store.persist(argv[1]);
        if (backlog != nullptr && ok) {
            backlog->append(argv);
        }
        return FrameCodec::encode_response_int(ok ? 1 : 0);
    }
    if (cmd == "INCR") {
        if (argv.size() != 2) {
            return FrameCodec::encode_response_error("ERR wrong number of arguments for INCR");
        }
        const auto value = store.incr(argv[1]);
        if (backlog != nullptr) {
            backlog->append({"INCR", argv[1]});
        }
        return FrameCodec::encode_response_int(value);
    }
    if (cmd == "DECR") {
        if (argv.size() != 2) {
            return FrameCodec::encode_response_error("ERR wrong number of arguments for DECR");
        }
        const auto value = store.decr(argv[1]);
        if (backlog != nullptr) {
            backlog->append({"DECR", argv[1]});
        }
        return FrameCodec::encode_response_int(value);
    }
    if (cmd == "KEYS") {
        if (argv.size() > 2) {
            return FrameCodec::encode_response_error("ERR wrong number of arguments for KEYS");
        }
        auto values = store.keys(argv.size() == 2 ? argv[1] : std::string{});
        return FrameCodec::encode_response_array(values);
    }
    if (cmd == "INFO") {
        if (argv.size() != 1) {
            return FrameCodec::encode_response_error("ERR wrong number of arguments for INFO");
        }
        std::string info = store.info();
        if (backlog != nullptr) {
            info += " repl_offset=" + std::to_string(backlog->offset());
        }
        return FrameCodec::encode_response_string(info);
    }
    if (cmd == "REPLICAOF") {
        return FrameCodec::encode_response_error("ERR replication is not implemented");
    }
    if (cmd == "QUIT") {
        if (argv.size() != 1) {
            return FrameCodec::encode_response_error("ERR wrong number of arguments for QUIT");
        }
        return FrameCodec::encode_response_ok();
    }
    return FrameCodec::encode_response_error("ERR unknown command");
}

bool drain_output(int fd, ConnectionState& state) {
    while (!state.outbuf.empty()) {
        const auto sent = send(fd, state.outbuf.data(), state.outbuf.size(), MSG_NOSIGNAL);
        if (sent < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return true;
            }
            return false;
        }
        state.outbuf.erase(state.outbuf.begin(), state.outbuf.begin() + sent);
        if (sent == 0) {
            return false;
        }
    }
    return true;
}

bool handle_client_event(Store& store, ReplBacklog* backlog, int fd, std::uint32_t events,
                         ConnectionState& state) {
    if ((events & EPOLLERR) != 0) {
        return false;
    }
    const auto process_input = [&]() {
        std::size_t offset = 0;
        while (true) {
            std::optional<Request> request;
            try {
                request = FrameCodec::decode_one(state.inbuf, offset);
            } catch (const std::exception&) {
                return false;
            }
            if (!request.has_value()) {
                break;
            }
            std::vector<std::uint8_t> response;
            try {
                response = process_command(store, backlog, request->argv);
            } catch (const std::exception& error) {
                response = FrameCodec::encode_response_error(std::string("ERR ") + error.what());
            }
            if (state.outbuf.size() > 16 * 1024 * 1024 ||
                response.size() > 16 * 1024 * 1024 - state.outbuf.size()) {
                return false;
            }
            state.outbuf.insert(state.outbuf.end(), response.begin(), response.end());
        }
        if (offset != 0) {
            state.inbuf.erase(state.inbuf.begin(), state.inbuf.begin() + static_cast<std::ptrdiff_t>(offset));
        }
        return state.inbuf.size() <= FrameCodec::max_request_bytes;
    };

    std::array<std::uint8_t, 4096> buffer{};
    if (!state.peer_closed && (events & (EPOLLIN | EPOLLRDHUP | EPOLLHUP)) != 0) {
        for (;;) {
            const auto n = recv(fd, buffer.data(), buffer.size(), 0);
            if (n == 0) {
                state.peer_closed = true;
                break;
            }
            if (n < 0) {
                if (errno == EINTR) {
                    continue;
                }
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    break;
                }
                return false;
            }
            state.inbuf.insert(state.inbuf.end(), buffer.begin(), buffer.begin() + n);
            if (!process_input()) {
                return false;
            }
        }
    }
    if ((events & EPOLLOUT) != 0 || !state.outbuf.empty()) {
        if (!drain_output(fd, state)) {
            return false;
        }
    }
    return !state.peer_closed || !state.outbuf.empty();
}

std::uint32_t connection_events(const ConnectionState& state) {
    std::uint32_t events = EPOLLET | EPOLLRDHUP;
    if (!state.peer_closed) {
        events |= EPOLLIN;
    }
    if (!state.outbuf.empty()) {
        events |= EPOLLOUT;
    }
    return events;
}

struct WorkerQueue {
    std::mutex mutex;
    std::deque<int> pending;
    int wake_fd = -1;
};

void accept_loop(int listen_fd, std::vector<WorkerQueue>& queues, std::atomic<std::size_t>& next_worker) {
    pollfd listener{listen_fd, POLLIN, 0};
    for (;;) {
        const int ready = poll(&listener, 1, -1);
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            throw std::runtime_error("poll() failed");
        }
        for (;;) {
            sockaddr_in client_addr{};
            socklen_t len = sizeof(client_addr);
            const int client_fd = accept4(listen_fd, reinterpret_cast<sockaddr*>(&client_addr), &len,
                                          SOCK_NONBLOCK | SOCK_CLOEXEC);
            if (client_fd < 0) {
                if (errno == EINTR) {
                    continue;
                }
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    break;
                }
                throw std::runtime_error("accept() failed");
            }
            const std::size_t queue_index = next_worker.fetch_add(1, std::memory_order_relaxed) % queues.size();
            {
                std::lock_guard<std::mutex> lock(queues[queue_index].mutex);
                queues[queue_index].pending.push_back(client_fd);
            }
            const std::uint64_t wake = 1;
            ssize_t written;
            do {
                written = write(queues[queue_index].wake_fd, &wake, sizeof(wake));
            } while (written < 0 && errno == EINTR);
            if (written < 0 && errno != EAGAIN) {
                close(client_fd);
                throw std::runtime_error("worker wakeup failed");
            }
        }
    }
}

void worker_loop(Store& store, ReplBacklog* backlog, WorkerQueue& queue) {
    const int epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (epoll_fd < 0) {
        throw std::runtime_error("epoll_create1() failed");
    }
    epoll_event wake_event{};
    wake_event.events = EPOLLIN;
    wake_event.data.fd = queue.wake_fd;
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, queue.wake_fd, &wake_event) < 0) {
        close(epoll_fd);
        throw std::runtime_error("epoll_ctl() failed for worker wakeup");
    }
    std::unordered_map<int, ConnectionState> connections;
    std::array<epoll_event, 128> events{};
    for (;;) {
        const int event_count = epoll_wait(epoll_fd, events.data(), static_cast<int>(events.size()), -1);
        if (event_count < 0) {
            if (errno == EINTR) {
                continue;
            }
            close(epoll_fd);
            throw std::runtime_error("epoll_wait() failed");
        }
        for (int i = 0; i < event_count; ++i) {
            const int fd = events[static_cast<std::size_t>(i)].data.fd;
            if (fd == queue.wake_fd) {
                std::uint64_t wake_count = 0;
                while (read(queue.wake_fd, &wake_count, sizeof(wake_count)) > 0) {}
                std::deque<int> pending;
                {
                    std::lock_guard<std::mutex> lock(queue.mutex);
                    pending.swap(queue.pending);
                }
                for (const int client_fd : pending) {
                    epoll_event client_event{};
                    client_event.events = EPOLLIN | EPOLLRDHUP | EPOLLET;
                    client_event.data.fd = client_fd;
                    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client_fd, &client_event) < 0) {
                        close(client_fd);
                    } else {
                        connections.try_emplace(client_fd);
                    }
                }
                continue;
            }
            auto connection = connections.find(fd);
            if (connection == connections.end() ||
                !handle_client_event(store, backlog, fd, events[static_cast<std::size_t>(i)].events,
                                     connection->second)) {
                epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, nullptr);
                if (connection != connections.end()) {
                    close(fd);
                }
                connections.erase(fd);
                continue;
            }
            epoll_event client_event{};
            client_event.events = connection_events(connection->second);
            client_event.data.fd = fd;
            if (epoll_ctl(epoll_fd, EPOLL_CTL_MOD, fd, &client_event) < 0) {
                close(fd);
                connections.erase(connection);
            }
        }
    }
}

}  // namespace

Server::Server(ServerConfig config) : config_(std::move(config)) {}

void Server::run() {
    if (!config_.leader_mode) {
        throw std::runtime_error("replication is not implemented");
    }
    Store store(config_.maxmemory, config_.maxmemory_policy);
    ReplBacklog backlog;
    const int listen_fd = make_listen_socket(config_.host, config_.port);
    const std::size_t worker_count = std::max<std::size_t>(1, config_.threads);

    std::vector<WorkerQueue> queues(worker_count);
    std::atomic<std::size_t> next_worker{0};

    for (std::size_t i = 0; i < worker_count; ++i) {
        queues[i].wake_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        if (queues[i].wake_fd < 0) {
            throw std::runtime_error("eventfd() failed");
        }
        std::thread(worker_loop, std::ref(store), &backlog, std::ref(queues[i])).detach();
    }

    accept_loop(listen_fd, queues, next_worker);
}

}  // namespace kv
