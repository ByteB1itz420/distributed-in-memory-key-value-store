#pragma once

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace kv {

struct Entry {
    std::string value;
    std::uint64_t last_access = 0;
    std::int64_t expire_at_ms = -1;
    std::size_t approx_size = 0;
};

class Store {
public:
    explicit Store(std::size_t maxmemory_bytes = 0, std::string policy = "noeviction");

    bool set(const std::string& key, const std::string& value, std::optional<std::int64_t> ttl_seconds = std::nullopt);
    std::optional<std::string> get(const std::string& key);
    bool del(const std::string& key);
    bool exists(const std::string& key) const;

    bool expire(const std::string& key, std::int64_t ttl_seconds);
    bool persist(const std::string& key);
    std::int64_t ttl(const std::string& key) const;
    std::int64_t incr(const std::string& key, std::int64_t delta = 1);
    std::int64_t decr(const std::string& key, std::int64_t delta = 1);
    std::vector<std::string> keys(const std::string& prefix = {}) const;
    std::string info() const;

    void set_maxmemory(std::size_t maxmemory_bytes);
    void set_policy(const std::string& policy);

    std::size_t size() const;
    bool empty() const;
    void clear();

private:
    mutable std::mutex mutex_;
    mutable std::unordered_map<std::string, Entry> data_;
    std::size_t maxmemory_bytes_ = 0;
    std::string policy_ = "noeviction";
    mutable std::uint64_t access_counter_ = 0;
    mutable std::size_t memory_used_ = 0;

    void prune_expired_locked() const;
    void maybe_evict_locked();
    void touch_locked(const std::string& key) const;
    bool set_locked(const std::string& key, const std::string& value, std::optional<std::int64_t> ttl_seconds,
                    bool preserve_ttl = false);
    static std::size_t sizeof_value(const std::string& value);
};

}  // namespace kv
