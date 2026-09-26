#pragma once

#include "Engine/Core/Assert.h"
#include "Engine/Core/Types.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <span>
#include <thread>
#include <type_traits>
#include <vector>

namespace gx {

// Tracks completion of a group of jobs. Must outlive the jobs that reference it.
class JobCounter {
public:
    JobCounter() = default;
    JobCounter(const JobCounter&) = delete;
    JobCounter& operator=(const JobCounter&) = delete;

    [[nodiscard]] bool isDone() const { return m_pending.load(std::memory_order_acquire) == 0; }

private:
    friend class JobSystem;
    std::atomic<u32> m_pending{0};
};

using JobFunction = void (*)(void* userData);

// A plain function pointer plus a pointer to data owned by the submitter: no allocation per job.
struct Job {
    JobFunction function = nullptr;
    void* userData = nullptr;
};

struct JobSystemStats {
    u64 jobsExecuted = 0;
    u64 parallelForCalls = 0;
};

// Fixed pool of worker threads fed by one FIFO queue.
//
// * A thread blocked in wait() executes queued jobs meanwhile: nested parallelism cannot deadlock and the
//   calling (main) thread contributes instead of idling.
// * parallelFor splits [0, count) into chunks whose boundaries depend only on (count, grain), never on the
//   number of threads. Per-chunk results combined in chunk order are therefore bit-identical for any thread
//   count: this is the contract deterministic parallel code relies on (see parallelReduce). Callers must
//   pass a fixed grain, never one derived from the thread count.
//
// Deliberately simple (one mutex-protected queue; parallelFor hands out chunks through one atomic counter).
// Replace with work stealing only if benchmarks show queue contention.
class JobSystem {
public:
    // workerThreadCount == 0: every job runs on the thread that waits for it (serial, FIFO order).
    explicit JobSystem(u32 workerThreadCount);
    ~JobSystem();
    JobSystem(const JobSystem&) = delete;
    JobSystem& operator=(const JobSystem&) = delete;

    // One worker per hardware thread, minus the calling thread.
    [[nodiscard]] static u32 defaultWorkerCount();

    void submit(const Job& job, JobCounter& counter);
    void submit(std::span<const Job> jobs, JobCounter& counter);
    // Returns once the counter reaches zero, running queued jobs while waiting.
    void wait(JobCounter& counter);

    // Calls fn(begin, end) over consecutive chunks of at most `grain` indices covering [0, count), on the
    // calling thread and the workers. Returns when every chunk is done. `profileZone` must be a string
    // with static storage duration; it labels each thread's share of the work in the profiler.
    template <typename Fn>
    void parallelFor(u32 count, u32 grain, Fn&& fn, const char* profileZone = "Jobs.ParallelFor");

    // Deterministic map-reduce: map(begin, end) -> T for each chunk, then folds the chunk results with
    // combine(acc, value) in chunk order. Bit-identical results for any thread count.
    template <typename T, typename MapFn, typename CombineFn>
    [[nodiscard]] T parallelReduce(u32 count, u32 grain, T identity, MapFn&& map, CombineFn&& combine,
                                   const char* profileZone = "Jobs.ParallelReduce");

    [[nodiscard]] static constexpr u32 chunkCount(u32 count, u32 grain) {
        return grain == 0 ? 0u : static_cast<u32>((static_cast<u64>(count) + grain - 1) / grain);
    }

    [[nodiscard]] u32 workerCount() const { return static_cast<u32>(m_workers.size()); }
    // Workers plus the calling thread.
    [[nodiscard]] u32 threadCount() const { return workerCount() + 1; }
    // 0 on threads that are not workers (e.g. the main thread), 1..N on worker threads.
    [[nodiscard]] static u32 currentThreadIndex();
    [[nodiscard]] JobSystemStats stats() const;

private:
    struct QueuedJob {
        Job job;
        JobCounter* counter = nullptr;
    };

    struct ParallelForState {
        std::atomic<u32> nextChunk{0};
        u32 count = 0;
        u32 grain = 0;
        u32 chunks = 0;
        const char* profileZone = nullptr;
        void* fn = nullptr;
        void (*invoke)(void* fn, u32 begin, u32 end) = nullptr;
    };

    static void runParallelForChunks(void* state);
    void dispatchParallelFor(ParallelForState& state);
    void workerLoop(u32 threadIndex);
    bool tryRunOne();
    void execute(const QueuedJob& queued);
    void wakeWorkers(usize jobCount);

    std::vector<std::thread> m_workers;
    std::mutex m_mutex; // guards m_queue and m_stopping
    std::condition_variable m_wakeWorkers;
    std::deque<QueuedJob> m_queue;
    bool m_stopping = false;
    std::atomic<u32> m_queued{0}; // lock-free emptiness hint for waiting threads
    std::atomic<u64> m_jobsExecuted{0};
    std::atomic<u64> m_parallelForCalls{0};
};

template <typename Fn>
void JobSystem::parallelFor(u32 count, u32 grain, Fn&& fn, const char* profileZone) {
    GX_CHECK(grain > 0, "parallelFor grain must be positive");
    const u32 chunks = chunkCount(count, grain);
    if (chunks == 0) {
        return;
    }
    using Callable = std::remove_reference_t<Fn>;
    ParallelForState state;
    state.count = count;
    state.grain = grain;
    state.chunks = chunks;
    state.profileZone = profileZone;
    state.fn = const_cast<void*>(static_cast<const void*>(std::addressof(fn)));
    state.invoke = [](void* callable, u32 begin, u32 end) {
        (*static_cast<Callable*>(callable))(begin, end);
    };
    dispatchParallelFor(state);
}

template <typename T, typename MapFn, typename CombineFn>
T JobSystem::parallelReduce(u32 count, u32 grain, T identity, MapFn&& map, CombineFn&& combine,
                            const char* profileZone) {
    GX_CHECK(grain > 0, "parallelReduce grain must be positive");
    std::vector<T> partials(chunkCount(count, grain), identity);
    parallelFor(
        count, grain, [&](u32 begin, u32 end) { partials[begin / grain] = map(begin, end); }, profileZone);
    T result = identity;
    for (const T& partial : partials) {
        result = combine(result, partial);
    }
    return result;
}

} // namespace gx
