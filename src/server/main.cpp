#include "server.hpp"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

int parse_port(std::string_view text) {
    int value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size() || value < 1 || value > 65535) {
        throw std::invalid_argument("port must be an integer between 1 and 65535");
    }
    return value;
}

std::size_t parse_size(std::string_view text, const char* option) {
    std::uint64_t value = 0;
    const auto digit_end = std::find_if_not(text.begin(), text.end(), [](unsigned char ch) {
        return std::isdigit(ch) != 0;
    });
    if (digit_end == text.begin()) {
        throw std::invalid_argument(std::string(option) + " must start with a non-negative integer");
    }
    const auto [end, error] = std::from_chars(text.data(), text.data() + (digit_end - text.begin()), value);
    if (error != std::errc{} || end != text.data() + (digit_end - text.begin())) {
        throw std::invalid_argument(std::string(option) + " is out of range");
    }

    std::string suffix(digit_end, text.end());
    std::transform(suffix.begin(), suffix.end(), suffix.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    std::uint64_t multiplier = 1;
    if (suffix == "k" || suffix == "kb" || suffix == "kib") {
        multiplier = 1024ULL;
    } else if (suffix == "m" || suffix == "mb" || suffix == "mib") {
        multiplier = 1024ULL * 1024;
    } else if (suffix == "g" || suffix == "gb" || suffix == "gib") {
        multiplier = 1024ULL * 1024 * 1024;
    } else if (!suffix.empty() && suffix != "b") {
        throw std::invalid_argument(std::string(option) + " has an unsupported size suffix");
    }
    if (value > std::numeric_limits<std::size_t>::max() / multiplier) {
        throw std::invalid_argument(std::string(option) + " is out of range");
    }
    return static_cast<std::size_t>(value * multiplier);
}

}  // namespace

int main(int argc, char** argv) {
    kv::ServerConfig config;
    try {
        for (int i = 1; i < argc; ++i) {
            const std::string arg = argv[i];
            if (arg == "--host" && i + 1 < argc) {
                config.host = argv[++i];
            } else if (arg == "--port" && i + 1 < argc) {
                config.port = parse_port(argv[++i]);
            } else if (arg == "--threads" && i + 1 < argc) {
                config.threads = parse_size(argv[++i], "--threads");
                if (config.threads == 0 || config.threads > 256) {
                    throw std::invalid_argument("--threads must be between 1 and 256");
                }
            } else if (arg == "--maxmemory" && i + 1 < argc) {
                config.maxmemory = parse_size(argv[++i], "--maxmemory");
            } else if (arg == "--maxmemory-policy" && i + 1 < argc) {
                config.maxmemory_policy = argv[++i];
                std::transform(config.maxmemory_policy.begin(), config.maxmemory_policy.end(),
                               config.maxmemory_policy.begin(), [](unsigned char ch) {
                                   return static_cast<char>(std::tolower(ch));
                               });
                if (config.maxmemory_policy != "noeviction" && config.maxmemory_policy != "allkeys-lru") {
                    throw std::invalid_argument("--maxmemory-policy must be noeviction or allkeys-lru");
                }
            } else if (arg == "--replicaof" && i + 2 < argc) {
                config.replicaof_host = argv[++i];
                config.replicaof_port = parse_port(argv[++i]);
                config.leader_mode = false;
            } else {
                throw std::invalid_argument("unknown or incomplete argument: " + arg);
            }
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }

    try {
        kv::Server server(config);
        server.run();
    } catch (const std::exception& ex) {
        std::cerr << "server failed: " << ex.what() << '\n';
        return 1;
    }

    return 0;
}
