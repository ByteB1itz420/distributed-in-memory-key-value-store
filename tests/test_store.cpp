#include "../src/store/store.hpp"

#include <cassert>
#include <chrono>
#include <iostream>
#include <optional>
#include <thread>

int main() {
    kv::Store store(1024 * 1024);

    store.set("hello", "world");
    const auto value = store.get("hello");
    assert(value.has_value());
    assert(value.value() == "world");

    assert(store.exists("hello"));
    assert(store.del("hello"));
    assert(!store.exists("hello"));

    store.set("count", "10");
    assert(store.incr("count", 4) == 14);
    assert(store.decr("count", 2) == 12);

    store.set("sleepy", "value", 1);
    assert(store.ttl("sleepy") > 0);
    std::cout << "waiting for expiry...\n";
    std::this_thread::sleep_for(std::chrono::seconds(2));
    assert(!store.exists("sleepy"));

    kv::Store capped(64);
    capped.set("a", "abc");
    capped.set("b", "defgh");
    assert(capped.get("a").has_value() || capped.get("b").has_value());

    std::cout << "kvstore tests passed\n";
    return 0;
}
