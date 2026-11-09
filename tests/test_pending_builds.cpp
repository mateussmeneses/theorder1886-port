// SPDX-License-Identifier: GPL-2.0-or-later
// RetirePendingBuild (PipelineCache::WaitGraphics): a waited build is retired by key after a wait
// that may insert into, remove from or replace entries of the tsl::robin_map of pending builds.
// No Vulkan device or game asset is needed; the map, key size and wait pattern mirror the cache.
#include <array>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <thread>
#include <unordered_map>
#include <vector>
#include <tsl/robin_map.h>
#include "video_core/renderer_vulkan/vk_pending_builds.h"

namespace {

// As large as a GraphicsPipelineKey, with a hash that callers can make collide.
struct Key {
    std::array<uint64_t, 40> bytes{};
    bool operator==(const Key&) const = default;
};
Key MakeKey(uint64_t id) {
    Key key;
    key.bytes[0] = id;
    key.bytes[39] = ~id;
    return key;
}
struct KeyHash {
    uint64_t mask = ~0ull;  // a small mask piles keys into few ideal buckets (robin-hood shifts)
    size_t operator()(const Key& key) const {
        return static_cast<size_t>((key.bytes[0] & mask) * 0x9E3779B97F4A7C15ull);
    }
};
struct Build {
    uint64_t id;
    std::atomic<bool> done{false};
    explicit Build(uint64_t i) : id(i) {}
};
using Pending = tsl::robin_map<Key, std::shared_ptr<Build>, KeyHash>;

size_t Walk(const Pending& pending) {
    size_t n = 0;
    for (auto it = pending.begin(); it != pending.end(); ++it) {
        ++n;
    }
    return n;
}

void CheckConsistent(const Pending& pending) {
    assert(pending.size() == Walk(pending));
    for (auto it = pending.begin(); it != pending.end(); ++it) {
        assert(it->second && it.key() == MakeKey(it->second->id));
        assert(pending.find(it.key()) == it);
    }
}

// The WaitGraphics shape: look the key up, wait (the callback is what PublishShaders does to the
// map meanwhile), then retire by key. Returns what RetirePendingBuild returned.
bool WaitAndRetire(Pending& pending, uint64_t id, const std::function<void()>& during_wait) {
    std::shared_ptr<Build> build;
    if (const auto it = pending.find(MakeKey(id)); it != pending.end()) {
        build = it.value();
    } else {
        return false;
    }
    during_wait();
    return Vulkan::RetirePendingBuild(pending, MakeKey(id), build);
}

void Fill(Pending& pending, uint64_t first, uint64_t count) {
    for (uint64_t id = first; id < first + count; ++id) {
        pending.emplace(MakeKey(id), std::make_shared<Build>(id));
    }
}

void TestInsertionRehashesDuringWait() {
    Pending pending;
    Fill(pending, 0, 6);
    const size_t buckets = pending.bucket_count();
    uint64_t next = 1000;
    assert(WaitAndRetire(pending, 3, [&] {
        while (pending.bucket_count() == buckets) {  // prequeue drain until the table rehashes
            Fill(pending, next++, 1);
        }
    }));
    assert(pending.bucket_count() != buckets);
    assert(!pending.contains(MakeKey(3)));
    assert(pending.size() == 5 + (next - 1000));
    CheckConsistent(pending);
    // The stale-iterator version undercounted here, wrapped size() below zero once every build
    // was retired, and the next insertion grew the table until std::bad_alloc.
    for (auto it = pending.begin(); it != pending.end();) {
        it = pending.erase(it);
    }
    assert(pending.size() == 0 && Walk(pending) == 0);
    const size_t after_drain = pending.bucket_count();
    Fill(pending, 7, 1);
    assert(pending.size() == 1 && pending.bucket_count() <= after_drain);
}

void TestInsertionShiftsWithoutRehash() {
    Pending pending(64, KeyHash{0x3});  // four ideal buckets: inserts displace the waited entry
    Fill(pending, 0, 8);
    const size_t buckets = pending.bucket_count();
    assert(WaitAndRetire(pending, 4, [&] { Fill(pending, 100, 8); }));
    assert(pending.bucket_count() == buckets);
    assert(!pending.contains(MakeKey(4)) && pending.size() == 15);
    for (uint64_t id : {0, 1, 2, 3, 5, 6, 7}) {
        assert(pending.contains(MakeKey(id)));  // no other build lost its publication
    }
    CheckConsistent(pending);
}

void TestRemovedOrReplacedDuringWait() {
    Pending pending;
    Fill(pending, 0, 4);
    // Removed meanwhile: nothing to retire, nothing else touched.
    assert(!WaitAndRetire(pending, 2, [&] { pending.erase(MakeKey(2)); }));
    assert(pending.size() == 3 && !pending.contains(MakeKey(2)));
    // Replaced by another build of the same key: that build stays pending.
    std::shared_ptr<Build> replacement = std::make_shared<Build>(1);
    assert(!WaitAndRetire(pending, 1, [&] { pending[MakeKey(1)] = replacement; }));
    assert(pending.at(MakeKey(1)) == replacement);
    // Absent key: no wait, no change.
    assert(!WaitAndRetire(pending, 42, [] { assert(false); }));
    CheckConsistent(pending);
}

void TestNestedAndRepeatedWaits() {
    Pending pending;
    Fill(pending, 0, 3);
    uint64_t next = 200;
    // A wait whose drain queues work that is itself waited on (several publications per wait).
    assert(WaitAndRetire(pending, 0, [&] {
        Fill(pending, next, 40);
        const uint64_t inner = next;
        next += 40;
        assert(WaitAndRetire(pending, inner, [&] { Fill(pending, next, 40); next += 40; }));
        assert(WaitAndRetire(pending, 1, [] {}));
    }));
    assert(pending.size() == 1 + 80 - 1);  // key 2 and both drains, minus the inner waited key
    CheckConsistent(pending);
}

void TestPrequeueOnAndOff() {
    for (const bool prequeue : {true, false}) {
        Pending pending;
        Fill(pending, 0, 10);
        uint64_t next = 10;
        for (uint64_t waited = 0; waited < 10; ++waited) {
            assert(WaitAndRetire(pending, waited, [&] {
                if (prequeue) {
                    Fill(pending, next, 7);  // DrainDeferredSelections queues new builds
                    next += 7;
                }
            }));
            CheckConsistent(pending);
        }
        assert(pending.size() == (prequeue ? 70u : 0u));
    }
}

// Compiler workers finish builds on their own threads while the owning thread waits, publishes
// and drains; every queued key must be published exactly once and the map must stay consistent.
void TestConcurrentCompletions() {
    for (int round = 0; round < 200; ++round) {
        Pending pending;
        std::unordered_map<uint64_t, int> published;
        std::atomic<size_t> cursor{0};
        std::atomic<bool> stop{false};
        std::vector<std::shared_ptr<Build>> jobs;  // worker-visible, appended before start
        uint64_t next = 0;
        auto enqueue = [&](uint64_t count) {
            for (uint64_t i = 0; i < count; ++i, ++next) {
                auto build = jobs[next];
                pending.emplace(MakeKey(build->id), build);
            }
        };
        const uint64_t total = 600;
        for (uint64_t id = 0; id < total; ++id) {
            jobs.push_back(std::make_shared<Build>(id));
        }
        std::vector<std::thread> workers;
        for (int w = 0; w < 3; ++w) {
            workers.emplace_back([&] {
                while (!stop.load()) {
                    const size_t i = cursor.fetch_add(1);
                    if (i >= total) {
                        break;
                    }
                    jobs[i]->done.store(true, std::memory_order_release);
                    std::this_thread::yield();
                }
            });
        }
        enqueue(20);
        auto publish_done = [&] {  // the PublishGraphics sweep
            for (auto it = pending.begin(); it != pending.end();) {
                if (it->second->done.load(std::memory_order_acquire)) {
                    ++published[it->second->id];
                    it = pending.erase(it);
                } else {
                    ++it;
                }
            }
        };
        while (next < total || !pending.empty()) {
            if (!pending.empty()) {
                const uint64_t waited = pending.begin()->second->id;
                const auto build = pending.begin()->second;
                const bool retired = WaitAndRetire(pending, waited, [&] {
                    while (!build->done.load(std::memory_order_acquire)) {
                        if (next < total) {
                            enqueue(std::min<uint64_t>(5, total - next));  // drain while waiting
                        }
                        std::this_thread::yield();
                    }
                });
                assert(retired);
                ++published[waited];
            }
            if (next < total) {
                enqueue(std::min<uint64_t>(3, total - next));
            }
            publish_done();
            assert(pending.size() == Walk(pending));
        }
        stop.store(true);
        for (auto& worker : workers) {
            worker.join();
        }
        assert(published.size() == total);
        for (const auto& [id, count] : published) {
            assert(count == 1);
        }
        assert(pending.empty() && pending.size() == 0);
    }
}

} // namespace

int main() {
    TestInsertionRehashesDuringWait();
    TestInsertionShiftsWithoutRehash();
    TestRemovedOrReplacedDuringWait();
    TestNestedAndRepeatedWaits();
    TestPrequeueOnAndOff();
    TestConcurrentCompletions();
    std::puts("pending-builds: ok");
    return 0;
}
