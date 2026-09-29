#include "../src/repl/backlog.hpp"
#include "../src/proto/protocol.hpp"
#include "../src/store/store.hpp"

#include <chrono>
#include <cstdint>
#include <algorithm>
#include <iostream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {

void check(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

template <typename Exception, typename Function>
void check_throws(Function&& function, const char* message) {
    try {
        function();
    } catch (const Exception&) {
        return;
    }
    throw std::runtime_error(message);
}

}  // namespace

int main() {
    const auto request_frame = kv::FrameCodec::encode_request({"SET", "key", "value"});
    std::size_t request_offset = 0;
    const std::vector<std::uint8_t> partial_frame(request_frame.begin(), request_frame.begin() + 5);
    check(!kv::FrameCodec::decode_one(partial_frame, request_offset).has_value(),
          "protocol decoder should retain incomplete frames");
    const auto decoded_request = kv::FrameCodec::decode_one(request_frame, request_offset);
    check(decoded_request.has_value() && decoded_request->argv == std::vector<std::string>({"SET", "key", "value"}),
          "protocol decoder should round-trip complete requests");
    check(request_offset == request_frame.size(), "protocol decoder should advance the frame offset");
    check_throws<std::invalid_argument>(
        [] { std::size_t offset = 1; kv::FrameCodec::decode_one({}, offset); },
        "protocol decoder should reject offsets outside the input buffer");
    std::vector<std::uint8_t> too_many_arguments{65, 0, 0, 0};
    check_throws<std::length_error>(
        [&] { std::size_t offset = 0; kv::FrameCodec::decode_one(too_many_arguments, offset); },
        "protocol decoder should bound the number of request arguments");

    kv::Store store(1024 * 1024);

    store.set("hello", "world");
    const auto value = store.get("hello");
    check(value.has_value(), "GET should return a stored value");
    check(value.value() == "world", "GET should preserve the stored value");

    check(store.exists("hello"), "EXISTS should find a stored key");
    check(store.del("hello"), "DEL should remove a stored key");
    check(!store.exists("hello"), "DEL should remove the key");

    kv::Store namespaced;
    namespaced.set("user:one:a", "1");
    namespaced.set("user:two:b", "2");
    namespaced.set("user:one:c", "3");
    auto scoped_keys = namespaced.keys("user:one:");
    std::sort(scoped_keys.begin(), scoped_keys.end());
    check(scoped_keys == std::vector<std::string>({"user:one:a", "user:one:c"}),
          "prefix key listing should return only matching keys");

    store.set("count", "10");
    check(store.incr("count", 4) == 14, "INCR should add to an integer value");
    check(store.decr("count", 2) == 12, "DECR should subtract from an integer value");

    store.set("sleepy", "value", 1);
    check(store.ttl("sleepy") > 0, "TTL should report a positive remaining lifetime");
    std::cout << "waiting for expiry...\n";
    std::this_thread::sleep_for(std::chrono::seconds(2));
    check(!store.exists("sleepy"), "expired keys should be removed");

    kv::Store noeviction(12);
    check(noeviction.set("a", "123"), "first write should fit the memory cap");
    check(!noeviction.set("b", "123"), "noeviction should reject writes over the memory cap");
    check(noeviction.get("a") == std::optional<std::string>("123"), "rejected writes must preserve existing data");
    check(!noeviction.set("a", "12345678"), "oversized replacement should be rejected");
    check(noeviction.get("a") == std::optional<std::string>("123"), "rejected replacement must preserve the old value");

    kv::Store lru(15, "allkeys-lru");
    check(lru.set("a", "1"), "first LRU write should succeed");
    check(lru.set("b", "1"), "second LRU write should fit");
    check(lru.get("a").has_value(), "GET should update the LRU access time");
    check(lru.set("c", "123"), "LRU should make room for a fitting value");
    check(lru.exists("a") && !lru.exists("b") && lru.exists("c"), "LRU should evict the least recently used key");

    kv::Store policy_change;
    policy_change.set("a", "123");
    policy_change.set("b", "123");
    policy_change.set_maxmemory(12);
    policy_change.set_policy("allkeys-lru");
    check(policy_change.size() == 1, "enabling LRU should immediately enforce the memory cap");

    kv::Store counters;
    counters.set("ttl-count", "1", 60);
    check(counters.incr("ttl-count") == 2 && counters.ttl("ttl-count") > 0,
          "INCR should preserve a key's expiration");
    counters.set("invalid", "12tail");
    check_throws<std::invalid_argument>([&] { counters.incr("invalid"); },
                                        "INCR should reject trailing non-numeric characters");
    check(counters.get("invalid") == std::optional<std::string>("12tail"),
          "failed INCR should not change the stored value");
    counters.set("maximum", std::to_string(std::numeric_limits<std::int64_t>::max()));
    check_throws<std::out_of_range>([&] { counters.incr("maximum"); },
                                    "INCR should detect signed integer overflow");
    counters.set("minimum", std::to_string(std::numeric_limits<std::int64_t>::min()));
    check_throws<std::out_of_range>([&] { counters.decr("minimum"); },
                                    "DECR should detect signed integer underflow");
    check_throws<std::out_of_range>(
        [&] { counters.set("long-ttl", "value", std::numeric_limits<std::int64_t>::max()); },
        "SET should reject TTL values that overflow the clock");
    check(!counters.exists("long-ttl"), "a rejected TTL must not create a key");

    kv::Store concurrent;
    concurrent.set("counter", "0");
    std::vector<std::thread> writers;
    for (int i = 0; i < 4; ++i) {
        writers.emplace_back([&concurrent]() {
            for (int j = 0; j < 500; ++j) {
                concurrent.incr("counter");
            }
        });
    }
    for (auto& writer : writers) {
        writer.join();
    }
    check(concurrent.get("counter") == std::optional<std::string>("2000"),
          "concurrent increments should be atomic");

    kv::ReplBacklog backlog;
    backlog.append({"SET", "k", "v"});
    backlog.append({"DEL", "k"});
    check(backlog.size() == 2, "replication backlog should count appended commands");
    const auto since = backlog.since(1);
    check(since.size() == 1, "backlog offset should return the following command");
    check(since[0].find("DEL") != std::string::npos, "backlog should preserve appended commands");

    kv::ReplBacklog bounded_backlog;
    for (int i = 0; i < 1000; ++i) {
        bounded_backlog.append({"SET", std::to_string(i), std::string(2048, 'x')});
    }
    check(bounded_backlog.size() < 600, "replication backlog should remain bounded as commands accumulate");
    const auto latest = bounded_backlog.since(bounded_backlog.offset() - 1);
    check(latest.size() == 1 && latest[0].find("999") != std::string::npos,
          "bounded backlog should preserve the newest command and its offset");

    kv::ReplBacklog concurrent_backlog;
    writers.clear();
    for (int i = 0; i < 4; ++i) {
        writers.emplace_back([&concurrent_backlog, i]() {
            for (int j = 0; j < 300; ++j) {
                concurrent_backlog.append({"SET", std::to_string(i), std::to_string(j)});
            }
        });
    }
    for (auto& writer : writers) {
        writer.join();
    }
    check(concurrent_backlog.offset() == 1200 && concurrent_backlog.size() == 1200,
          "concurrent backlog appends should preserve offsets without races");

    std::cout << "kvstore tests passed\n";
    return 0;
}
