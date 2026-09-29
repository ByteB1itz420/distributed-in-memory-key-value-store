#pragma once

#include <sys/socket.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace kv {

inline constexpr std::uint32_t max_response_bytes = 16 * 1024 * 1024;

inline void send_all(int fd, const std::uint8_t* data, std::size_t size) {
    std::size_t sent = 0;
    while (sent < size) {
        const auto count = send(fd, data + sent, size - sent, MSG_NOSIGNAL);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            throw std::runtime_error("send() failed");
        }
        sent += static_cast<std::size_t>(count);
    }
}

inline void recv_exact(int fd, std::uint8_t* data, std::size_t size) {
    std::size_t received = 0;
    while (received < size) {
        const auto count = recv(fd, data + received, size - received, 0);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            throw std::runtime_error("connection closed before the response was complete");
        }
        received += static_cast<std::size_t>(count);
    }
}

inline std::vector<std::uint8_t> receive_frame(int fd) {
    std::array<std::uint8_t, 4> header{};
    recv_exact(fd, header.data(), header.size());
    const std::uint32_t size = static_cast<std::uint32_t>(header[0]) |
        (static_cast<std::uint32_t>(header[1]) << 8) |
        (static_cast<std::uint32_t>(header[2]) << 16) |
        (static_cast<std::uint32_t>(header[3]) << 24);
    if (size < 1 || size > max_response_bytes) {
        throw std::runtime_error("invalid response frame length");
    }
    std::vector<std::uint8_t> frame(size + header.size());
    std::copy(header.begin(), header.end(), frame.begin());
    recv_exact(fd, frame.data() + header.size(), size);
    return frame;
}

}  // namespace kv
