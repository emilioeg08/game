#include "Benchmarks/BenchCommon.h"

#include "Engine/Jobs/JobSystem.h"
#include "Engine/Profiling/Profiler.h"
#include "Engine/Profiling/Statistics.h"
#include "Engine/Time/Stopwatch.h"

#include <atomic>

namespace gx::bench {

void benchProfilerOverhead(Report& report, const Options& options) {
    section("profiler.zone_overhead");
    const u32 iterations = options.quick ? 200'000 : 1'000'000;

    profiling::resetStats();
    profiling::setEnabled(true);
    const Stopwatch enabledTimer;
    for (u32 i = 0; i < iterations; ++i) {
        GX_PROFILE_SCOPE("Bench.EmptyZone");
    }
    const f64 enabledNs = static_cast<f64>(enabledTimer.elapsedNs()) / iterations;
    const Stopwatch collectTimer;
    profiling::collect();
    const f64 collectNs = static_cast<f64>(collectTimer.elapsedNs()) / iterations;
    profiling::resetStats();

    profiling::setEnabled(false);
    const Stopwatch disabledTimer;
    for (u32 i = 0; i < iterations; ++i) {
        GX_PROFILE_SCOPE("Bench.EmptyZone");
    }
    const f64 disabledNs = static_cast<f64>(disabledTimer.elapsedNs()) / iterations;
    profiling::setEnabled(true);

    std::printf("zone enabled : %7.1f ns record + %5.1f ns collect\n", enabledNs, collectNs);
    std::printf("zone disabled: %7.1f ns\n", disabledNs);
    report.add("profiler.zone_overhead", {{"iterations", Report::integer(iterations)},
                                          {"enabled_record_ns", Report::number(enabledNs)},
                                          {"enabled_collect_ns", Report::number(collectNs)},
                                          {"disabled_ns", Report::number(disabledNs)}});
}

void benchJobDispatch(Report& report, const Options& options) {
    section("jobs.dispatch");
    const u32 iterations = options.quick ? 5'000 : 20'000;
    profiling::setEnabled(false); // measure the job system alone
    JobSystem jobs(options.threads - 1);
    std::atomic<u64> sink{0};

    // A parallelFor with one chunk per thread and trivial work: pure fork/join overhead.
    std::vector<f64> forkJoin;
    forkJoin.reserve(iterations);
    for (u32 i = 0; i < iterations; ++i) {
        const Stopwatch timer;
        jobs.parallelFor(jobs.threadCount(), 1,
                         [&](u32 begin, u32 end) { sink.fetch_add(end - begin, std::memory_order_relaxed); });
        forkJoin.push_back(static_cast<f64>(timer.elapsedNs()) / 1e3);
    }

    // One job submitted and waited for by the main thread (which may run it itself).
    std::vector<f64> single;
    single.reserve(iterations);
    const Job job{[](void* p) { static_cast<std::atomic<u64>*>(p)->fetch_add(1, std::memory_order_relaxed); },
                  &sink};
    for (u32 i = 0; i < iterations; ++i) {
        const Stopwatch timer;
        JobCounter counter;
        jobs.submit(job, counter);
        jobs.wait(counter);
        single.push_back(static_cast<f64>(timer.elapsedNs()) / 1e3);
    }
    profiling::setEnabled(true);

    const SampleStats fj = computeSampleStats(std::move(forkJoin));
    const SampleStats sj = computeSampleStats(std::move(single));
    std::printf("threads %u\n", options.threads);
    std::printf("%-22s %10s %10s %10s %10s\n", "", "mean us", "p50 us", "p99 us", "max us");
    std::printf("%-22s %10.2f %10.2f %10.2f %10.2f\n", "parallelFor fork/join", fj.mean, fj.p50, fj.p99,
                fj.max);
    std::printf("%-22s %10.2f %10.2f %10.2f %10.2f\n", "submit + wait (1 job)", sj.mean, sj.p50, sj.p99,
                sj.max);
    report.add("jobs.dispatch", {{"threads", Report::integer(options.threads)},
                                 {"iterations", Report::integer(iterations)},
                                 {"fork_join_mean_us", Report::number(fj.mean)},
                                 {"fork_join_p50_us", Report::number(fj.p50)},
                                 {"fork_join_p99_us", Report::number(fj.p99)},
                                 {"single_job_mean_us", Report::number(sj.mean)},
                                 {"single_job_p99_us", Report::number(sj.p99)}});
}

void benchJobScaling(Report& report, const Options& options) {
    section("jobs.scaling");
    const u32 elements = options.quick ? (1u << 20) : (1u << 22);
    constexpr u32 kGrain = 4096;
    constexpr int kRepetitions = 5;
    std::vector<f64> data(elements, 1.0);
    const auto kernel = [&](u32 begin, u32 end) {
        for (u32 i = begin; i < end; ++i) {
            f64 x = data[i];
            for (int k = 0; k < 32; ++k) {
                x = x * 1.0000001 + 1e-9; // dependent chain: compute-bound, no memory bottleneck
            }
            data[i] = x;
        }
    };

    std::vector<u32> threadCounts;
    for (const u32 t : {1u, 2u, 4u, 6u, 8u, 12u, 16u, 24u, 32u}) {
        if (t <= options.threads) {
            threadCounts.push_back(t);
        }
    }
    if (threadCounts.back() != options.threads) {
        threadCounts.push_back(options.threads);
    }

    profiling::setEnabled(false);
    f64 baselineMs = 0.0;
    std::printf("%8s %12s %10s %12s\n", "threads", "median ms", "speedup", "efficiency");
    for (const u32 threads : threadCounts) {
        JobSystem jobs(threads - 1);
        jobs.parallelFor(elements, kGrain, kernel); // warm-up
        std::vector<f64> samples;
        for (int r = 0; r < kRepetitions; ++r) {
            const Stopwatch timer;
            jobs.parallelFor(elements, kGrain, kernel);
            samples.push_back(timer.elapsedMs());
        }
        const f64 medianMs = computeSampleStats(samples).p50;
        if (threads == 1) {
            baselineMs = medianMs;
        }
        const f64 speedup = baselineMs / medianMs;
        const f64 efficiency = speedup / threads;
        std::printf("%8u %12.2f %10.2f %11.0f%%\n", threads, medianMs, speedup, efficiency * 100.0);
        report.add("jobs.scaling", {{"threads", Report::integer(threads)},
                                    {"elements", Report::integer(elements)},
                                    {"median_ms", Report::number(medianMs)},
                                    {"speedup", Report::number(speedup)},
                                    {"efficiency", Report::number(efficiency)}});
    }
    profiling::setEnabled(true);
}

} // namespace gx::bench
