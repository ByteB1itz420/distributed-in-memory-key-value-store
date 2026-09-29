#include "backlog.hpp"

#include <utility>

namespace kv {

void ReplBacklog::append(const std::vector<std::string>& command) {
    if (command.empty()) {
        return;
    }

    std::size_t serialized_size = 0;
    bool too_large = false;
    for (std::size_t i = 0; i < command.size(); ++i) {
        const std::size_t delimiter_size = i == 0 ? 0 : 1;
        if (serialized_size > max_bytes_ - delimiter_size ||
            command[i].size() > max_bytes_ - serialized_size - delimiter_size) {
            too_large = true;
            break;
        }
        serialized_size += delimiter_size + command[i].size();
    }

    std::string serialized;
    if (!too_large) {
        serialized.reserve(serialized_size);
        for (std::size_t i = 0; i < command.size(); ++i) {
            if (i != 0) {
                serialized.push_back('\n');
            }
            serialized += command[i];
        }
    }
    std::lock_guard<std::mutex> lock(mutex_);
    ++offset_;
    if (too_large) {
        return;
    }
    while (!entries_.empty() &&
           (bytes_used_ > max_bytes_ - serialized.size() || entries_.size() >= max_entries_)) {
        bytes_used_ -= entries_.front().command.size();
        entries_.pop_front();
    }
    bytes_used_ += serialized.size();
    entries_.push_back({offset_, std::move(serialized)});
}

std::vector<std::string> ReplBacklog::since(std::uint64_t offset) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> result;
    for (const auto& entry : entries_) {
        if (entry.offset > offset) {
            result.push_back(entry.command);
        }
    }
    return result;
}

std::uint64_t ReplBacklog::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return static_cast<std::uint64_t>(entries_.size());
}

std::uint64_t ReplBacklog::offset() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return offset_;
}

void ReplBacklog::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    entries_.clear();
    bytes_used_ = 0;
    offset_ = 0;
}

}  // namespace kv
