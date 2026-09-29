#include "server.hpp"

#include <iostream>
#include <stdexcept>
#include <string>

int main(int argc, char** argv) {
    kv::ServerConfig config;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--port" && i + 1 < argc) {
            config.port = std::stoi(argv[++i]);
        } else if (arg == "--threads" && i + 1 < argc) {
            config.threads = static_cast<std::size_t>(std::stoull(argv[++i]));
        } else if (arg == "--maxmemory" && i + 1 < argc) {
            config.maxmemory = static_cast<std::size_t>(std::stoll(argv[++i]));
        } else if (arg == "--maxmemory-policy" && i + 1 < argc) {
            config.maxmemory_policy = argv[++i];
        } else {
            std::cerr << "Unknown argument: " << arg << '\n';
            return 1;
        }
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
