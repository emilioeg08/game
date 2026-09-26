#include "Engine/Jobs/JobSystem.h"

#include "Engine/Core/Platform.h"
#include "Engine/Profiling/Profiler.h"

#include <algorithm>
#include <format>
#include <string>

namespace gx {
namespace {
thread_local u32 t_threadIndex = 0;
constexpr u32 kSpinsBeforeYield = 64;
} // namespace

JobSystem::JobSystem(u32 workerThreadCount) {
    m_workers.reserve(workerThreadCount);
    for (u32 i = 0; i < workerThreadCount; ++i) {
        m_workers.emplace_back([this, i] { workerLoop(i + 1); });
    }
}

JobSystem::~JobSystem() {
    {
        std::lock_guard lock(m_mutex);
        m_stopping = true;
    }
    m_wakeWorkers.notify_all();
    for (std::thread& worker : m_workers) {
        worker.join();
    }
    GX_CHECK(m_queue.empty(), "JobSystem destroyed with {} jobs never waited for", m_queue.size());
}

u32 JobSystem::defaultWorkerCount() {
    return platform::hardwareThreadCount() - 1;
}

u32 JobSystem::currentThreadIndex() {
    return t_threadIndex;
}

JobSystemStats JobSystem::stats() const {
    return {m_jobsExecuted.load(std::memory_order_relaxed),
            m_parallelForCalls.load(std::memory_order_relaxed)};
}

void JobSystem::submit(const Job& job, JobCounter& counter) {
    submit(std::span<const Job>(&job, 1), counter);
}

void JobSystem::submit(std::span<const Job> jobs, JobCounter& counter) {
    if (jobs.empty()) {
        return;
    }
    const auto jobCount = static_cast<u32>(jobs.size());
    counter.m_pending.fetch_add(jobCount, std::memory_order_relaxed);
    {
        std::lock_guard lock(m_mutex);
        for (const Job& job : jobs) {
            GX_ASSERT(job.function != nullptr, "submitted a job without a function");
            m_queue.push_back({job, &counter});
        }
        m_queued.fetch_add(jobCount, std::memory_order_relaxed);
    }
    wakeWorkers(jobCount);
}

void JobSystem::wait(JobCounter& counter) {
    u32 idleSpins = 0;
    while (counter.m_pending.load(std::memory_order_acquire) != 0) {
        if (tryRunOne()) {
            idleSpins = 0;
        } else if (++idleSpins < kSpinsBeforeYield) {
            platform::cpuRelax();
        } else {
            std::this_thread::yield();
        }
    }
}

bool JobSystem::tryRunOne() {
    if (m_queued.load(std::memory_order_relaxed) == 0) {
        return false;
    }
    QueuedJob queued;
    {
        std::lock_guard lock(m_mutex);
        if (m_queue.empty()) {
            return false;
        }
        queued = m_queue.front();
        m_queue.pop_front();
        m_queued.fetch_sub(1, std::memory_order_relaxed);
    }
    execute(queued);
    return true;
}

void JobSystem::execute(const QueuedJob& queued) {
    queued.job.function(queued.job.userData);
    m_jobsExecuted.fetch_add(1, std::memory_order_relaxed);
    // Release: everything the job wrote is visible to whoever observes the counter reaching zero.
    // The counter may be destroyed by the waiter right after this, so it is the last access.
    queued.counter->m_pending.fetch_sub(1, std::memory_order_acq_rel);
}

void JobSystem::wakeWorkers(usize jobCount) {
    if (jobCount >= m_workers.size()) {
        m_wakeWorkers.notify_all();
    } else {
        for (usize i = 0; i < jobCount; ++i) {
            m_wakeWorkers.notify_one();
        }
    }
}

void JobSystem::workerLoop(u32 threadIndex) {
    t_threadIndex = threadIndex;
    const std::string name = std::format("worker-{}", threadIndex);
    platform::setCurrentThreadName(name.c_str());

    for (;;) {
        QueuedJob queued;
        {
            std::unique_lock lock(m_mutex);
            m_wakeWorkers.wait(lock, [this] { return m_stopping || !m_queue.empty(); });
            if (m_queue.empty()) {
                return; // stopping and drained
            }
            queued = m_queue.front();
            m_queue.pop_front();
            m_queued.fetch_sub(1, std::memory_order_relaxed);
        }
        execute(queued);
    }
}

void JobSystem::dispatchParallelFor(ParallelForState& state) {
    m_parallelForCalls.fetch_add(1, std::memory_order_relaxed);

    // One helper job per worker that can get a chunk; each helper keeps claiming chunks until none remain.
    const u32 helpers = std::min(workerCount(), state.chunks - 1);
    JobCounter counter;
    if (helpers > 0) {
        counter.m_pending.fetch_add(helpers, std::memory_order_relaxed);
        {
            std::lock_guard lock(m_mutex);
            for (u32 i = 0; i < helpers; ++i) {
                m_queue.push_back({Job{&JobSystem::runParallelForChunks, &state}, &counter});
            }
            m_queued.fetch_add(helpers, std::memory_order_relaxed);
        }
        wakeWorkers(helpers);
    }

    runParallelForChunks(&state); // the calling thread claims chunks too
    // Helpers still queued are drained here by the caller (they find no chunks left and return at once),
    // so completion never waits on a sleeping worker waking up.
    wait(counter);
}

void JobSystem::runParallelForChunks(void* opaque) {
    auto& state = *static_cast<ParallelForState*>(opaque);
#if GX_ENABLE_PROFILING
    const bool profile = profiling::isEnabled();
    const u64 startNs = profile ? platform::monotonicNanoseconds() : 0;
#endif
    u32 processed = 0;
    for (;;) {
        const u32 chunk = state.nextChunk.fetch_add(1, std::memory_order_relaxed);
        if (chunk >= state.chunks) {
            break;
        }
        const u32 begin = chunk * state.grain;
        const auto end = static_cast<u32>(std::min<u64>(static_cast<u64>(begin) + state.grain, state.count));
        state.invoke(state.fn, begin, end);
        ++processed;
    }
#if GX_ENABLE_PROFILING
    if (profile && processed > 0) {
        profiling::detail::recordZone(state.profileZone, startNs, platform::monotonicNanoseconds());
    }
#else
    (void)processed;
#endif
}

} // namespace gx
