#include "store.hpp"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <sstream>

namespace kv {
namespace {
std::int64_t now_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

std::string normalize_policy(std::string policy) {
    std::transform(policy.begin(), policy.end(), policy.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return policy;
}
}  // namespace

Store::Store(std::size_t maxmemory_bytes, std::string policy)
    : maxmemory_bytes_(maxmemory_bytes), policy_(normalize_policy(std::move(policy))) {
    if (policy_ != "noeviction" && policy_ != "allkeys-lru") {
        policy_ = "noeviction";
    }
}

void Store::set(const std::string& key, const std::string& value, std::optional<int64_t> ttl_seconds) {
    std::lock_guard<std::mutex> lock(mutex_);
    set_locked(key, value, ttl_seconds);
}

void Store::set_locked(const std::string& key, const std::string& value, std::optional<int64_t> ttl_seconds) {
    prune_expired_locked();

    const auto it = data_.find(key);
    if (it != data_.end()) {
        memory_used_ -= it->second.approx_size;
    }

    Entry entry;
    entry.value = value;
    entry.approx_size = sizeof_value(key) + sizeof_value(value);
    entry.last_access = ++access_counter_;
    entry.expire_at_ms = -1;

    if (ttl_seconds.has_value()) {
        if (ttl_seconds.value() <= 0) {
            data_.erase(key);
            memory_used_ = std::max<std::size_t>(0, memory_used_);
            return;
        }
        entry.expire_at_ms = now_ms() + (ttl_seconds.value() * 1000LL);
    }

    data_[key] = entry;
    memory_used_ += entry.approx_size;
    maybe_evict_locked();
}

std::optional<std::string> Store::get(const std::string& key) {
    std::lock_guard<std::mutex> lock(mutex_);
    prune_expired_locked();
    const auto it = data_.find(key);
    if (it == data_.end()) {
        return std::nullopt;
    }

    touch_locked(key);
    return it->second.value;
}

bool Store::del(const std::string& key) {
    std::lock_guard<std::mutex> lock(mutex_);
    prune_expired_locked();
    const auto it = data_.find(key);
    if (it == data_.end()) {
        return false;
    }

    memory_used_ -= it->second.approx_size;
    data_.erase(it);
    return true;
}

bool Store::exists(const std::string& key) const {
    std::lock_guard<std::mutex> lock(mutex_);
    prune_expired_locked();
    return data_.find(key) != data_.end();
}

bool Store::expire(const std::string& key, std::int64_t ttl_seconds) {
    std::lock_guard<std::mutex> lock(mutex_);
    prune_expired_locked();
    auto it = data_.find(key);
    if (it == data_.end()) {
        return false;
    }

    if (ttl_seconds <= 0) {
        memory_used_ -= it->second.approx_size;
        data_.erase(it);
        return true;
    }

    it->second.expire_at_ms = now_ms() + (ttl_seconds * 1000LL);
    return true;
}

bool Store::persist(const std::string& key) {
    std::lock_guard<std::mutex> lock(mutex_);
    prune_expired_locked();
    auto it = data_.find(key);
    if (it == data_.end()) {
        return false;
    }

    it->second.expire_at_ms = -1;
    return true;
}

std::int64_t Store::ttl(const std::string& key) const {
    std::lock_guard<std::mutex> lock(mutex_);
    prune_expired_locked();
    auto it = data_.find(key);
    if (it == data_.end()) {
        return -2;
    }

    if (it->second.expire_at_ms < 0) {
        return -1;
    }

    const auto remaining_ms = it->second.expire_at_ms - now_ms();
    return std::max<std::int64_t>(0, (remaining_ms + 999) / 1000);
}

std::int64_t Store::incr(const std::string& key, std::int64_t delta) {
    std::lock_guard<std::mutex> lock(mutex_);
    prune_expired_locked();

    auto it = data_.find(key);
    std::int64_t current = 0;
    if (it == data_.end()) {
        current = 0;
    } else {
        current = std::stoll(it->second.value);
    }

    current += delta;
    const std::string serialized = std::to_string(current);
    set_locked(key, serialized, std::nullopt);
    return current;
}

std::int64_t Store::decr(const std::string& key, std::int64_t delta) {
    return incr(key, -delta);
}

std::vector<std::string> Store::keys() const {
    std::lock_guard<std::mutex> lock(mutex_);
    prune_expired_locked();
    std::vector<std::string> result;
    result.reserve(data_.size());
    for (const auto& [key, _] : data_) {
        result.push_back(key);
    }
    return result;
}

std::string Store::info() const {
    std::lock_guard<std::mutex> lock(mutex_);
    prune_expired_locked();
    std::ostringstream oss;
    oss << "keys=" << data_.size() << " memory_used=" << memory_used_ << " maxmemory=" << maxmemory_bytes_;
    return oss.str();
}

void Store::set_maxmemory(std::size_t maxmemory_bytes) {
    std::lock_guard<std::mutex> lock(mutex_);
    maxmemory_bytes_ = maxmemory_bytes;
    maybe_evict_locked();
}

void Store::set_policy(const std::string& policy) {
    std::lock_guard<std::mutex> lock(mutex_);
    policy_ = normalize_policy(policy);
    if (policy_ != "noeviction" && policy_ != "allkeys-lru") {
        policy_ = "noeviction";
    }
}

std::size_t Store::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    prune_expired_locked();
    return data_.size();
}

bool Store::empty() const {
    return size() == 0;
}

void Store::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    data_.clear();
    memory_used_ = 0;
}

void Store::prune_expired_locked() const {
    const auto now = now_ms();
    for (auto it = data_.begin(); it != data_.end();) {
        if (it->second.expire_at_ms >= 0 && it->second.expire_at_ms <= now) {
            memory_used_ -= it->second.approx_size;
            it = data_.erase(it);
        } else {
            ++it;
        }
    }
}

void Store::maybe_evict_locked() {
    if (maxmemory_bytes_ == 0 || policy_ == "noeviction") {
        return;
    }

    if (memory_used_ <= maxmemory_bytes_) {
        return;
    }

    if (policy_ != "allkeys-lru") {
        return;
    }

    std::vector<std::pair<std::string, std::uint64_t>> candidates;
    candidates.reserve(data_.size());
    for (const auto& [key, entry] : data_) {
        candidates.emplace_back(key, entry.last_access);
    }

    std::sort(candidates.begin(), candidates.end(), [](const auto& lhs, const auto& rhs) {
        return lhs.second < rhs.second;
    });

    for (const auto& [key, _] : candidates) {
        if (memory_used_ <= maxmemory_bytes_) {
            break;
        }
        auto it = data_.find(key);
        if (it == data_.end()) {
            continue;
        }
        memory_used_ -= it->second.approx_size;
        data_.erase(it);
    }
}

void Store::touch_locked(const std::string& key) const {
    auto it = data_.find(key);
    if (it != data_.end()) {
        it->second.last_access = ++access_counter_;
    }
}

std::size_t Store::sizeof_value(const std::string& value) {
    return value.size() + sizeof(std::string::value_type) * 2;
}

}  // namespace kv
