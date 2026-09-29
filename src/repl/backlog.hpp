#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace kv {

class ReplBacklog {
public:
    void append(const std::vector<std::string>& command);
    std::vector<std::string> since(std::uint64_t offset) const;
    std::uint64_t size() const;
    std::uint64_t offset() const;
    void clear();

private:
    mutable std::mutex mutex_;
    std::vector<std::string> entries_;
    std::uint64_t offset_ = 0;
};

}  // namespace kv
