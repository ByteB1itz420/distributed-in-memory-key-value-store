#include "../proto/protocol.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

int connect_socket(const std::string& host, int port) {
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        throw std::runtime_error("socket() failed");
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<uint16_t>(port));
    if (inet_pton(AF_INET, host.c_str(), &address.sin_addr) != 1) {
        close(fd);
        throw std::runtime_error("inet_pton() failed");
    }

    if (connect(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) < 0) {
        close(fd);
        throw std::runtime_error("connect() failed");
    }

    return fd;
}

std::vector<std::string> tokenize(const std::string& text) {
    std::vector<std::string> out;
    std::string current;
    for (char ch : text) {
        if (ch == ' ' || ch == '\t') {
            if (!current.empty()) {
                out.push_back(current);
                current.clear();
            }
        } else {
            current.push_back(ch);
        }
    }
    if (!current.empty()) {
        out.push_back(current);
    }
    return out;
}

std::string read_reply(int fd) {
    std::array<std::uint8_t, 4096> buffer{};
    const auto n = recv(fd, buffer.data(), buffer.size(), 0);
    if (n <= 0) {
        return "";
    }
    std::vector<std::uint8_t> payload(buffer.begin(), buffer.begin() + n);
    std::size_t offset = 0;
    const auto reply = kv::FrameCodec::decode_one(payload, offset);
    (void)reply;
    return "";
}

}  // namespace

int main(int argc, char** argv) {
    std::string host = "127.0.0.1";
    int port = 6380;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "-h" && i + 1 < argc) {
            host = argv[++i];
        } else if (arg == "-p" && i + 1 < argc) {
            port = std::stoi(argv[++i]);
        }
    }

    try {
        const int fd = connect_socket(host, port);
        std::cout << "> ";
        std::string line;
        while (std::getline(std::cin, line)) {
            if (line.empty()) {
                std::cout << "> ";
                continue;
            }

            const auto argv = tokenize(line);
            const auto request = kv::FrameCodec::encode_request(argv);
            if (send(fd, request.data(), request.size(), 0) < 0) {
                throw std::runtime_error("send() failed");
            }

            std::array<std::uint8_t, 4096> response{};
            const auto n = recv(fd, response.data(), response.size(), 0);
            if (n <= 0) {
                break;
            }
            std::vector<std::uint8_t> payload(response.begin(), response.begin() + n);
            std::size_t offset = 0;
            std::string result;
            if (payload.size() >= 5) {
                const auto total = static_cast<std::uint32_t>(payload[0]) |
                    (static_cast<std::uint32_t>(payload[1]) << 8) |
                    (static_cast<std::uint32_t>(payload[2]) << 16) |
                    (static_cast<std::uint32_t>(payload[3]) << 24);
                const auto type = payload[4];
                result.assign(reinterpret_cast<const char*>(payload.data() + 5), std::min<std::size_t>(payload.size() - 5, total - 1));
                if (type == 0) {
                    result = "(nil)";
                } else if (type == 1) {
                    result = "ERR " + result;
                } else if (type == 3) {
                    result = "(integer) " + result;
                } else if (type == 4) {
                    result = "[" + result + "]";
                }
            }
            std::cout << result << '\n';
            std::cout << "> ";
        }
        close(fd);
    } catch (const std::exception& ex) {
        std::cerr << ex.what() << '\n';
        return 1;
    }

    return 0;
}
