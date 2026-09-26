#include "Tests/TestFramework.h"

#include "Engine/Core/Random.h"
#include "Engine/Jobs/JobSystem.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cmath>
#include <vector>

using namespace gx;

GX_TEST(Jobs, ChunkCountRoundsUp) {
    static_assert(JobSystem::chunkCount(0, 8) == 0);
    static_assert(JobSystem::chunkCount(1, 8) == 1);
    static_assert(JobSystem::chunkCount(9, 3) == 3);
    static_assert(JobSystem::chunkCount(10, 3) == 4);
    static_assert(JobSystem::chunkCount(0xffffffffu, 1) == 0xffffffffu);
    static_assert(JobSystem::chunkCount(0xffffffffu, 0x80000000u) == 2);
}

GX_TEST(Jobs, SubmitAndWaitRunsEveryJob) {
    for (const u32 workers : {0u, 1u, 4u}) {
        JobSystem jobs(workers);
        std::atomic<u32> executed{0};
        const std::vector<Job> batch(
            1000,
            Job{[](void* p) { static_cast<std::atomic<u32>*>(p)->fetch_add(1, std::memory_order_relaxed); },
                &executed});
        JobCounter counter;
        jobs.submit(batch, counter);
        jobs.wait(counter);
        GX_EXPECT(counter.isDone());
        GX_EXPECT_EQ(executed.load(), 1000u);
        GX_EXPECT(jobs.stats().jobsExecuted >= 1000u);
    }
}

GX_TEST(Jobs, ParallelForCoversEveryIndexExactlyOnce) {
    struct Case {
        u32 count;
        u32 grain;
    };
    const Case cases[] = {{0, 1}, {1, 1}, {1000, 1}, {1000, 7}, {1000, 1000}, {1000, 5000}, {100'000, 64}};
    for (const u32 workers : {0u, 1u, 3u, 7u}) {
        JobSystem jobs(workers);
        for (const Case& c : cases) {
            std::vector<std::atomic<u32>> hits(c.count);
            std::atomic<bool> badRange{false};
            jobs.parallelFor(c.count, c.grain, [&](u32 begin, u32 end) {
                if (begin >= end || end > c.count || end - begin > c.grain || begin % c.grain != 0) {
                    badRange = true;
                }
                for (u32 i = begin; i < end; ++i) {
                    hits[i].fetch_add(1, std::memory_order_relaxed);
                }
            });
            const bool allOnce = std::all_of(hits.begin(), hits.end(),
                                             [](const std::atomic<u32>& h) { return h.load() == 1; });
            GX_EXPECT(!badRange);
            GX_EXPECT(allOnce);
        }
    }
}

GX_TEST(Jobs, ParallelReduceIsBitIdenticalForAnyThreadCount) {
    // Magnitudes spanning 16 orders make floating-point addition order-dependent, so identical bits prove the
    // combination order does not depend on scheduling.
    std::vector<f64> values(200'000);
    Rng rng(99);
    for (f64& v : values) {
        v = rng.uniform(-1.0, 1.0) * std::pow(10.0, rng.uniform(-8.0, 8.0));
    }
    const auto sum = [&](JobSystem& jobs) {
        return jobs.parallelReduce(
            static_cast<u32>(values.size()), 1000, 0.0,
            [&](u32 begin, u32 end) {
                f64 partial = 0.0;
                for (u32 i = begin; i < end; ++i) {
                    partial += values[i];
                }
                return partial;
            },
            [](f64 accumulated, f64 partial) { return accumulated + partial; });
    };

    JobSystem serial(0);
    const u64 reference = std::bit_cast<u64>(sum(serial));
    for (const u32 workers : {1u, 3u, 7u, 11u}) {
        JobSystem jobs(workers);
        for (int repeat = 0; repeat < 5; ++repeat) {
            GX_EXPECT_EQ(std::bit_cast<u64>(sum(jobs)), reference);
        }
    }
}

GX_TEST(Jobs, NestedParallelForCompletes) {
    JobSystem jobs(3);
    std::atomic<u32> total{0};
    jobs.parallelFor(16, 1, [&](u32, u32) {
        jobs.parallelFor(
            1000, 10, [&](u32 begin, u32 end) { total.fetch_add(end - begin, std::memory_order_relaxed); });
    });
    GX_EXPECT_EQ(total.load(), 16'000u);
}

GX_TEST(Jobs, ThreadIndicesIdentifyWorkers) {
    JobSystem jobs(4);
    GX_EXPECT_EQ(JobSystem::currentThreadIndex(), 0u);
    GX_EXPECT_EQ(jobs.threadCount(), 5u);
    std::atomic<bool> outOfRange{false};
    jobs.parallelFor(20'000, 1, [&](u32, u32) {
        if (JobSystem::currentThreadIndex() > 4) {
            outOfRange = true;
        }
    });
    GX_EXPECT(!outOfRange);
}

GX_TEST(Jobs, StatsCountParallelForCalls) {
    JobSystem jobs(2);
    const u64 before = jobs.stats().parallelForCalls;
    for (int i = 0; i < 10; ++i) {
        jobs.parallelFor(100, 10, [](u32, u32) {});
    }
    GX_EXPECT_EQ(jobs.stats().parallelForCalls - before, 10u);
}
