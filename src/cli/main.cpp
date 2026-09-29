#include "../proto/protocol.hpp"
#include "../proto/socket_io.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <charconv>
#include <cerrno>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
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
    char quote = '\0';
    bool escaped = false;
    bool started = false;
    for (char ch : text) {
        if (escaped) {
            if (ch == 'n') current.push_back('\n');
            else if (ch == 't') current.push_back('\t');
            else current.push_back(ch);
            escaped = false;
            started = true;
        } else if (ch == '\\') {
            escaped = true;
            started = true;
        } else if (quote != '\0') {
            if (ch == quote) {
                quote = '\0';
            } else {
                current.push_back(ch);
            }
        } else if (ch == '"' || ch == '\'') {
            quote = ch;
            started = true;
        } else if (ch == ' ' || ch == '\t') {
            if (started) {
                out.push_back(current);
                current.clear();
                started = false;
            }
        } else {
            current.push_back(ch);
            started = true;
        }
    }
    if (escaped || quote != '\0') {
        throw std::invalid_argument("unterminated escape or quote");
    }
    if (started) {
        out.push_back(current);
    }
    return out;
}

std::string read_reply(int fd) {
    const auto frame = kv::receive_frame(fd);
    const auto type = frame[4];
    if (type == 4) {
        std::size_t offset = 5;
        const auto read_length = [&frame, &offset]() {
            if (frame.size() - offset < 4) throw std::runtime_error("invalid response array");
            const std::uint32_t length = static_cast<std::uint32_t>(frame[offset]) |
                (static_cast<std::uint32_t>(frame[offset + 1]) << 8) |
                (static_cast<std::uint32_t>(frame[offset + 2]) << 16) |
                (static_cast<std::uint32_t>(frame[offset + 3]) << 24);
            offset += 4;
            return length;
        };
        const auto count = read_length();
        std::ostringstream rendered;
        rendered << '[';
        for (std::uint32_t i = 0; i < count; ++i) {
            const auto length = read_length();
            if (frame.size() - offset < length) throw std::runtime_error("invalid response array");
            if (i != 0) rendered << ", ";
            rendered << std::quoted(std::string(frame.begin() + static_cast<std::ptrdiff_t>(offset),
                                                 frame.begin() + static_cast<std::ptrdiff_t>(offset + length)));
            offset += length;
        }
        if (offset != frame.size()) throw std::runtime_error("invalid response array");
        rendered << ']';
        return rendered.str();
    }
    std::string payload(frame.begin() + 5, frame.end());
    if (type == 0) return "(nil)";
    if (type == 1) return "ERR " + payload;
    if (type == 3) return "(integer) " + payload;
    if (type == 2) return payload;
    throw std::runtime_error("unknown response type");
}

int parse_port(std::string_view text) {
    int port = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), port);
    if (error != std::errc{} || end != text.data() + text.size() || port < 1 || port > 65535) {
        throw std::invalid_argument("port must be an integer between 1 and 65535");
    }
    return port;
}

}  // namespace

int main(int argc, char** argv) {
    std::string host = "127.0.0.1";
    int port = 6380;
    try {
        for (int i = 1; i < argc; ++i) {
            const std::string arg = argv[i];
            if (arg == "-h" && i + 1 < argc) {
                host = argv[++i];
            } else if (arg == "-p" && i + 1 < argc) {
                port = parse_port(argv[++i]);
            } else {
                throw std::invalid_argument("unknown or incomplete argument: " + arg);
            }
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
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
            kv::send_all(fd, request.data(), request.size());
            std::cout << read_reply(fd) << '\n';
            std::cout << "> ";
        }
        close(fd);
    } catch (const std::exception& ex) {
        std::cerr << ex.what() << '\n';
        return 1;
    }

    return 0;
}
