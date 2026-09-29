#pragma once

#include <cstddef>
#include <string>

namespace kv {

struct ServerConfig {
    std::string host = "0.0.0.0";
    int port = 6380;
    std::size_t maxmemory = 0;
    std::string maxmemory_policy = "noeviction";
    std::size_t threads = 4;
    std::string replicaof_host;
    int replicaof_port = -1;
    bool leader_mode = true;
};

class Server {
public:
    explicit Server(ServerConfig config);
    void run();

private:
    ServerConfig config_;
};

}  // namespace kv
