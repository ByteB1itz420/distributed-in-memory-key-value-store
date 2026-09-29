#pragma once

#include <cstddef>
#include <string>

namespace kv {

struct ServerConfig {
    int port = 6380;
    std::size_t maxmemory = 0;
    std::string maxmemory_policy = "noeviction";
    std::size_t threads = 4;
};

class Server {
public:
    explicit Server(ServerConfig config);
    void run();

private:
    ServerConfig config_;
};

}  // namespace kv
