#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
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
    struct Entry {
        std::uint64_t offset;
        std::string command;
    };

    static constexpr std::size_t max_bytes_ = 1024 * 1024;
    static constexpr std::size_t max_entries_ = 8192;
    mutable std::mutex mutex_;
    std::deque<Entry> entries_;
    std::uint64_t offset_ = 0;
    std::size_t bytes_used_ = 0;
};

}  // namespace kv
