#include "store.hpp"

#include <algorithm>
#include <chrono>
#include <charconv>
#include <cctype>
#include <limits>
#include <stdexcept>
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

std::int64_t expiration_time(std::int64_t ttl_seconds) {
    const auto now = now_ms();
    if (ttl_seconds > std::numeric_limits<std::int64_t>::max() / 1000) {
        throw std::out_of_range("TTL is too large");
    }
    const auto delta_ms = ttl_seconds * 1000;
    if (now > std::numeric_limits<std::int64_t>::max() - delta_ms) {
        throw std::out_of_range("TTL is too large");
    }
    return now + delta_ms;
}

std::int64_t parse_integer(const std::string& value) {
    std::int64_t parsed = 0;
    const auto result = std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (result.ec == std::errc::result_out_of_range) {
        throw std::out_of_range("integer value is out of range");
    }
    if (result.ec != std::errc{} || result.ptr != value.data() + value.size()) {
        throw std::invalid_argument("value is not an integer");
    }
    return parsed;
}
}  // namespace

Store::Store(std::size_t maxmemory_bytes, std::string policy)
    : maxmemory_bytes_(maxmemory_bytes), policy_(normalize_policy(std::move(policy))) {
    if (policy_ != "noeviction" && policy_ != "allkeys-lru") {
        policy_ = "noeviction";
    }
}

bool Store::set(const std::string& key, const std::string& value, std::optional<std::int64_t> ttl_seconds) {
    std::lock_guard<std::mutex> lock(mutex_);
    return set_locked(key, value, ttl_seconds);
}

bool Store::set_locked(const std::string& key, const std::string& value, std::optional<std::int64_t> ttl_seconds,
                       bool preserve_ttl) {
    prune_expired_locked();

    const auto it = data_.find(key);
    Entry entry;
    entry.value = value;
    entry.approx_size = sizeof_value(key) + sizeof_value(value);
    entry.last_access = ++access_counter_;
    entry.expire_at_ms = -1;

    if (ttl_seconds.has_value()) {
        if (ttl_seconds.value() <= 0) {
            if (it != data_.end()) {
                memory_used_ -= it->second.approx_size;
                data_.erase(it);
            }
            return true;
        }
        entry.expire_at_ms = expiration_time(ttl_seconds.value());
    } else if (preserve_ttl && it != data_.end()) {
        entry.expire_at_ms = it->second.expire_at_ms;
    }

    const auto old_size = it == data_.end() ? 0 : it->second.approx_size;
    const auto used_without_old = memory_used_ - old_size;
    if (maxmemory_bytes_ != 0 && entry.approx_size > maxmemory_bytes_) {
        return false;
    }
    if (maxmemory_bytes_ != 0 && used_without_old > maxmemory_bytes_ - entry.approx_size) {
        if (policy_ != "allkeys-lru") {
            return false;
        }
        std::vector<std::pair<std::string, std::uint64_t>> candidates;
        candidates.reserve(data_.size());
        for (const auto& [candidate_key, candidate] : data_) {
            if (candidate_key != key) {
                candidates.emplace_back(candidate_key, candidate.last_access);
            }
        }
        std::sort(candidates.begin(), candidates.end(), [](const auto& lhs, const auto& rhs) {
            return lhs.second < rhs.second;
        });
        auto projected_size = used_without_old;
        for (const auto& [candidate_key, _] : candidates) {
            if (projected_size <= maxmemory_bytes_ - entry.approx_size) {
                break;
            }
            const auto candidate = data_.find(candidate_key);
            if (candidate == data_.end()) {
                continue;
            }
            projected_size -= candidate->second.approx_size;
            memory_used_ -= candidate->second.approx_size;
            data_.erase(candidate);
        }
        if (projected_size > maxmemory_bytes_ - entry.approx_size) {
            return false;
        }
    }

    memory_used_ -= old_size;
    data_[key] = entry;
    memory_used_ += entry.approx_size;
    return true;
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

    it->second.expire_at_ms = expiration_time(ttl_seconds);
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
    return std::max<std::int64_t>(0, remaining_ms / 1000 + (remaining_ms % 1000 != 0));
}

std::int64_t Store::incr(const std::string& key, std::int64_t delta) {
    std::lock_guard<std::mutex> lock(mutex_);
    prune_expired_locked();

    const auto it = data_.find(key);
    std::int64_t current = 0;
    if (it != data_.end()) {
        current = parse_integer(it->second.value);
    }
    if ((delta > 0 && current > std::numeric_limits<std::int64_t>::max() - delta) ||
        (delta < 0 && current < std::numeric_limits<std::int64_t>::min() - delta)) {
        throw std::out_of_range("integer overflow");
    }
    current += delta;
    const std::string serialized = std::to_string(current);
    if (!set_locked(key, serialized, std::nullopt, true)) {
        throw std::runtime_error("OOM command not allowed when used memory exceeds maxmemory");
    }
    return current;
}

std::int64_t Store::decr(const std::string& key, std::int64_t delta) {
    std::lock_guard<std::mutex> lock(mutex_);
    prune_expired_locked();
    const auto it = data_.find(key);
    std::int64_t current = 0;
    if (it != data_.end()) {
        current = parse_integer(it->second.value);
    }
    if ((delta > 0 && current < std::numeric_limits<std::int64_t>::min() + delta) ||
        (delta < 0 && current > std::numeric_limits<std::int64_t>::max() + delta)) {
        throw std::out_of_range("integer overflow");
    }
    current -= delta;
    if (!set_locked(key, std::to_string(current), std::nullopt, true)) {
        throw std::runtime_error("OOM command not allowed when used memory exceeds maxmemory");
    }
    return current;
}

std::vector<std::string> Store::keys(const std::string& prefix) const {
    std::lock_guard<std::mutex> lock(mutex_);
    prune_expired_locked();
    std::vector<std::string> result;
    result.reserve(prefix.empty() ? data_.size() : std::min<std::size_t>(data_.size(), 64));
    for (const auto& [key, _] : data_) {
        if (key.compare(0, prefix.size(), prefix) == 0) {
            result.push_back(key);
        }
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
    maybe_evict_locked();
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
