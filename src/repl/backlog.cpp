#include "backlog.hpp"

#include <sstream>

namespace kv {

void ReplBacklog::append(const std::vector<std::string>& command) {
    if (command.empty()) {
        return;
    }

    std::ostringstream oss;
    for (std::size_t i = 0; i < command.size(); ++i) {
        if (i != 0) {
            oss << '\n';
        }
        oss << command[i];
    }

    std::lock_guard<std::mutex> lock(mutex_);
    entries_.push_back(oss.str());
    ++offset_;
}

std::vector<std::string> ReplBacklog::since(std::uint64_t offset) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> result;
    if (offset >= entries_.size()) {
        return result;
    }

    result.reserve(entries_.size() - offset);
    for (std::size_t i = offset; i < entries_.size(); ++i) {
        result.push_back(entries_[i]);
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
    offset_ = 0;
}

}  // namespace kv
