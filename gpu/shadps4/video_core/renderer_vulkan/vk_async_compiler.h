// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <algorithm>
#include <chrono>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <condition_variable>
#include <deque>
#include <functional>
#include <future>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#undef min
#undef max
#undef ERROR
#undef MemoryBarrier
#undef near
#undef far
#endif

namespace Vulkan {

// port: bounded compiler queue. Stop drains accepted work before Vulkan resources are released.
class AsyncCompiler {
public:
    explicit AsyncCompiler(unsigned count, size_t capacity = 64,
                           std::function<void(unsigned)> initialize = {})
        : capacity{capacity}, worker_count{count}, allowed_workers{count} {
        for (unsigned i = 0; i < count; ++i) {
            workers.emplace_back([this, i, initialize] {
                worker_index = i;
                on_worker = true;
                if (initialize) {
                    initialize(i);
                }
                for (;;) {
                    std::function<void()> task;
                    {
                        std::unique_lock lock{mutex};
                        cv.wait(lock, [&] { return stopping || !tasks.empty() || !urgent.empty(); });
                        if (tasks.empty() && urgent.empty()) {
                            return;
                        }
                        if (urgent.empty() && !stopping && worker_count > 2) {
                            const unsigned budget = SampleAllowedWorkers();
                            if (active_workers >= budget) {
                                ++budget_throttles;
                                cv.wait_for(lock, std::chrono::milliseconds(4), [&] {
                                    return stopping || !urgent.empty() || active_workers < budget;
                                });
                                if (tasks.empty() && urgent.empty()) {
                                    if (stopping) {
                                        return;
                                    }
                                    continue;
                                }
                            }
                        }
                        static const bool fair_queue = [] {
                            const char* env = std::getenv("THEORDER_FAIR_QUEUE");
                            return !env || env[0] != '0';
                        }();
                        const bool take_fair_normal =
                            fair_queue && !urgent.empty() && !tasks.empty() &&
                            prioritized_urgent == 0 && urgent_streak >= 8;
                        auto& queue = (urgent.empty() || take_fair_normal) ? tasks : urgent;
                        if (&queue == &urgent) {
                            ++urgent_streak;
                            if (prioritized_urgent > 0) {
                                --prioritized_urgent;
                            }
                        } else {
                            urgent_streak = 0;
                        }
                        auto next = std::move(queue.front());
                        queue.pop_front();
                        queue_wait_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::steady_clock::now() - next.enqueued).count();
                        task = std::move(next.function);
                        ++active_workers;
                    }
                    task();
                    {
                        std::scoped_lock lock{mutex};
                        --active_workers;
                    }
                    cv.notify_one();
                }
            });
        }
    }
    ~AsyncCompiler() { Stop(); }

    bool Submit(std::function<void()> task, bool priority = false,
                const void* identity = nullptr) {
        {
            std::scoped_lock lock{mutex};
            if (stopping || tasks.size() + urgent.size() >= capacity) {
                return false;
            }
            (priority ? urgent : tasks).push_back(
                {std::move(task), std::chrono::steady_clock::now(), identity});
        }
        cv.notify_one();
        return true;
    }
    bool Prioritize(const void* identity) {
        if (!identity) {
            return false;
        }
        {
            std::scoped_lock lock{mutex};
            const auto found = std::find_if(tasks.begin(), tasks.end(),
                                            [identity](const Task& task) {
                                                return task.identity == identity;
                                            });
            if (found == tasks.end()) {
                return false;
            }
            urgent.push_back(std::move(*found));
            tasks.erase(found);
            ++prioritized_urgent;
        }
        cv.notify_one();
        return true;
    }
    bool HasCapacity() {
        std::scoped_lock lock{mutex};
        return !stopping && tasks.size() + urgent.size() < capacity;
    }
    static unsigned WorkerIndex() { return worker_index; }
    /// port: this thread is a compiler worker (statistics: work done off the GPU thread).
    static bool OnWorker() { return on_worker; }
    size_t Depth() {
        std::scoped_lock lock{mutex};
        return tasks.size() + urgent.size();
    }
    uint64_t QueueWaitNs() { return queue_wait_ns.exchange(0); }
    uint64_t BudgetThrottles() { return budget_throttles.exchange(0); }
    void Stop() {
        {
            std::scoped_lock lock{mutex};
            stopping = true;
        }
        cv.notify_all();
        for (auto& worker : workers) {
            if (worker.joinable()) {
                worker.join();
            }
        }
    }

private:
    unsigned SampleAllowedWorkers() {
        static const bool budget_enabled = [] {
            const char* env = std::getenv("THEORDER_COMPILE_BUDGET");
            return !env || env[0] != '0';
        }();
        if (!budget_enabled || worker_count <= 2) {
            return worker_count;
        }
#ifdef _WIN32
        const auto now = std::chrono::steady_clock::now();
        if (now - last_budget_sample < std::chrono::milliseconds(250)) {
            return allowed_workers;
        }
        last_budget_sample = now;
        unsigned target = worker_count;
        MEMORYSTATUSEX mem{};
        mem.dwLength = sizeof(mem);
        if (GlobalMemoryStatusEx(&mem)) {
            const uint64_t avail_mib = mem.ullAvailPhys >> 20;
            if (avail_mib > 0 && avail_mib < 1536) {
                target = std::max(1u, worker_count / 2);
            }
        }
        FILETIME idle{}, kernel{}, user{};
        if (GetSystemTimes(&idle, &kernel, &user)) {
            const auto to_u64 = [](const FILETIME& ft) {
                return (uint64_t(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
            };
            const uint64_t idle_v = to_u64(idle);
            const uint64_t total_v = to_u64(kernel) + to_u64(user);
            if (prev_total_ticks != 0 && total_v > prev_total_ticks) {
                const uint64_t d_total = total_v - prev_total_ticks;
                const uint64_t d_idle = idle_v >= prev_idle_ticks ? (idle_v - prev_idle_ticks) : 0;
                const uint64_t busy_pct =
                    d_total > d_idle ? ((d_total - d_idle) * 100) / d_total : 0;
                if (busy_pct >= 90) {
                    target = std::min(target, std::max(2u, (worker_count * 3) / 4));
                }
            }
            prev_idle_ticks = idle_v;
            prev_total_ticks = total_v;
        }
        allowed_workers = target;
#endif
        return allowed_workers;
    }

    size_t capacity;
    unsigned worker_count = 0;
    unsigned active_workers = 0;
    unsigned allowed_workers = 0;
    unsigned urgent_streak = 0;
    size_t prioritized_urgent = 0;
    std::chrono::steady_clock::time_point last_budget_sample{};
    uint64_t prev_idle_ticks = 0;
    uint64_t prev_total_ticks = 0;
    std::mutex mutex;
    std::condition_variable cv;
    struct Task {
        std::function<void()> function;
        std::chrono::steady_clock::time_point enqueued;
        const void* identity;
    };
    std::deque<Task> tasks, urgent;
    static inline thread_local unsigned worker_index = 0;
    static inline thread_local bool on_worker = false;
    std::atomic<uint64_t> queue_wait_ns{};
    std::atomic<uint64_t> budget_throttles{};
    bool stopping = false;
    std::vector<std::thread> workers;
};

// port: one job per cache entry; failures are terminal until the next session.
template <typename T>
class AsyncJob {
public:
    template <typename F>
    bool Start(AsyncCompiler& compiler, F&& function, bool priority = false) {
        if (started) {
            return false;
        }
        auto task = std::make_shared<std::packaged_task<T()>>(std::forward<F>(function));
        auto completion = task->get_future();
        if (!compiler.Submit([task] { (*task)(); }, priority, task.get())) {
            return false;
        }
        task_identity = task;
        future = std::move(completion);
        started = true;
        return true;
    }
    bool Prioritize(AsyncCompiler& compiler) const {
        const auto task = task_identity.lock();
        return task && compiler.Prioritize(task.get());
    }
    bool Pending() const { return future.valid(); }
    bool Failed() const { return failed; }
    const std::string& Error() const { return error; }
    void Wait() const {
        if (future.valid()) {
            future.wait();
        }
    }
    std::optional<T> Poll() {
        if (!future.valid() || future.wait_for(std::chrono::seconds{0}) != std::future_status::ready) {
            return std::nullopt;
        }
        try {
            return future.get();
        } catch (const std::exception& exception) {
            error = exception.what();
        } catch (...) {
            error = "Unknown compiler error";
        }
        failed = true;
        return std::nullopt;
    }

private:
    std::future<T> future;
    std::weak_ptr<void> task_identity;
    bool started = false;
    bool failed = false;
    std::string error;
};

// port: IR retains its pools until emission finishes, then returns them for another translation.
template <typename T>
class CompilerPool {
public:
    explicit CompilerPool(size_t capacity) : capacity{capacity} {}
    std::pair<std::unique_ptr<T>, bool> Acquire() {
        std::scoped_lock lock{mutex};
        if (available.empty()) {
            return {std::make_unique<T>(), false};
        }
        auto pool = std::move(available.back());
        available.pop_back();
        return {std::move(pool), true};
    }
    void Recycle(std::unique_ptr<T> pool) noexcept {
        try {
            pool->ReleaseContents();
            std::scoped_lock lock{mutex};
            if (available.size() < capacity) {
                available.push_back(std::move(pool));
            }
        } catch (...) {
            // Reuse is optional; discard the pool if resetting or retaining it fails.
        }
    }
    size_t Retained() {
        std::scoped_lock lock{mutex};
        return available.size();
    }
private:
    size_t capacity;
    std::mutex mutex;
    std::vector<std::unique_ptr<T>> available;
};

} // namespace Vulkan
