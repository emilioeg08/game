// Engine benchmarks. Prints tables to stdout and writes a JSON report for tracking results over time.
//
//   profiler.zone_overhead  cost of a profiling zone, enabled and disabled
//   jobs.dispatch           latency of an (almost) empty parallelFor and of one submitted job
//   jobs.scaling            speedup of a compute-bound parallelFor across thread counts
//   sim.scaling             synthetic galaxy from 1 to 10,000 star systems, full resolution (no LOD):
//                           generation, memory, per-step cost, throughput, determinism across thread counts

#include "Engine/Core/Log.h"
#include "Engine/Core/Platform.h"
#include "Engine/Jobs/JobSystem.h"
#include "Engine/Profiling/MemoryUsage.h"
#include "Engine/Profiling/Profiler.h"
#include "Engine/Profiling/Statistics.h"
#include "Engine/Time/Stopwatch.h"
#include "Scenarios/SyntheticGalaxy.h"
#include "Simulation/Kernel/Simulation.h"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <format>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

using namespace gx;

namespace {

struct Options {
    bool quick = false;
    u32 threads = platform::hardwareThreadCount();
    std::string outPath;
    std::string filter;
};

// Minimal JSON report: each result is a flat object of pre-encoded values.
class Report {
public:
    using Fields = std::vector<std::pair<std::string, std::string>>;

    static std::string number(f64 v) { return std::format("{:.6g}", v); }
    static std::string integer(u64 v) { return std::format("{}", v); }
    static std::string text(std::string_view v) { return std::format("\"{}\"", v); }
    static std::string boolean(bool v) { return v ? "true" : "false"; }

    void add(std::string_view benchmark, Fields fields) {
        fields.insert(fields.begin(), std::pair<std::string, std::string>("benchmark", text(benchmark)));
        m_results.push_back(std::move(fields));
    }

    bool write(const std::filesystem::path& path, const Fields& header) const {
        std::error_code error;
        if (path.has_parent_path()) {
            std::filesystem::create_directories(path.parent_path(), error);
        }
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if (!out) {
            return false;
        }
        out << "{\n";
        for (const auto& [key, value] : header) {
            out << std::format("  \"{}\": {},\n", key, value);
        }
        out << "  \"results\": [\n";
        for (usize r = 0; r < m_results.size(); ++r) {
            out << "    {";
            for (usize f = 0; f < m_results[r].size(); ++f) {
                out << std::format("{}\"{}\": {}", f == 0 ? "" : ", ", m_results[r][f].first,
                                   m_results[r][f].second);
            }
            out << (r + 1 < m_results.size() ? "},\n" : "}\n");
        }
        out << "  ]\n}\n";
        return static_cast<bool>(out);
    }

private:
    std::vector<Fields> m_results;
};

bool selected(const Options& options, std::string_view name) {
    return options.filter.empty() || name.find(options.filter) != std::string_view::npos;
}

void section(const char* title) {
    std::printf("\n== %s ==\n", title);
    std::fflush(stdout);
}

// ---------------------------------------------------------------------------------------------------------

void benchProfilerOverhead(Report& report, bool quick) {
    section("profiler.zone_overhead");
    const u32 iterations = quick ? 200'000 : 1'000'000;

    profiling::resetStats();
    profiling::setEnabled(true);
    Stopwatch enabledTimer;
    for (u32 i = 0; i < iterations; ++i) {
        GX_PROFILE_SCOPE("Bench.EmptyZone");
    }
    const f64 enabledNs = static_cast<f64>(enabledTimer.elapsedNs()) / iterations;
    Stopwatch collectTimer;
    profiling::collect();
    const f64 collectNs = static_cast<f64>(collectTimer.elapsedNs()) / iterations;
    profiling::resetStats();

    profiling::setEnabled(false);
    Stopwatch disabledTimer;
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

// ---------------------------------------------------------------------------------------------------------

void benchJobDispatch(Report& report, u32 threads, bool quick) {
    section("jobs.dispatch");
    const u32 iterations = quick ? 5'000 : 20'000;
    profiling::setEnabled(false); // measure the job system alone
    JobSystem jobs(threads - 1);
    std::atomic<u64> sink{0};

    // A parallelFor with one chunk per thread and trivial work: pure fork/join overhead.
    std::vector<f64> forkJoin;
    forkJoin.reserve(iterations);
    for (u32 i = 0; i < iterations; ++i) {
        Stopwatch timer;
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
        Stopwatch timer;
        JobCounter counter;
        jobs.submit(job, counter);
        jobs.wait(counter);
        single.push_back(static_cast<f64>(timer.elapsedNs()) / 1e3);
    }
    profiling::setEnabled(true);

    const SampleStats fj = computeSampleStats(std::move(forkJoin));
    const SampleStats sj = computeSampleStats(std::move(single));
    std::printf("threads %u\n", threads);
    std::printf("%-22s %10s %10s %10s %10s\n", "", "mean us", "p50 us", "p99 us", "max us");
    std::printf("%-22s %10.2f %10.2f %10.2f %10.2f\n", "parallelFor fork/join", fj.mean, fj.p50, fj.p99,
                fj.max);
    std::printf("%-22s %10.2f %10.2f %10.2f %10.2f\n", "submit + wait (1 job)", sj.mean, sj.p50, sj.p99,
                sj.max);
    report.add("jobs.dispatch", {{"threads", Report::integer(threads)},
                                 {"iterations", Report::integer(iterations)},
                                 {"fork_join_mean_us", Report::number(fj.mean)},
                                 {"fork_join_p50_us", Report::number(fj.p50)},
                                 {"fork_join_p99_us", Report::number(fj.p99)},
                                 {"single_job_mean_us", Report::number(sj.mean)},
                                 {"single_job_p99_us", Report::number(sj.p99)}});
}

// ---------------------------------------------------------------------------------------------------------

void benchJobScaling(Report& report, u32 maxThreads, bool quick) {
    section("jobs.scaling");
    const u32 elements = quick ? (1u << 20) : (1u << 22);
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
    for (u32 t : {1u, 2u, 4u, 6u, 8u, 12u, 16u, 24u, 32u}) {
        if (t <= maxThreads) {
            threadCounts.push_back(t);
        }
    }
    if (threadCounts.back() != maxThreads) {
        threadCounts.push_back(maxThreads);
    }

    profiling::setEnabled(false);
    f64 baselineMs = 0.0;
    std::printf("%8s %12s %10s %12s\n", "threads", "median ms", "speedup", "efficiency");
    for (const u32 threads : threadCounts) {
        JobSystem jobs(threads - 1);
        jobs.parallelFor(elements, kGrain, kernel); // warm-up
        std::vector<f64> samples;
        for (int r = 0; r < kRepetitions; ++r) {
            Stopwatch timer;
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

// ---------------------------------------------------------------------------------------------------------

struct SimRun {
    u64 wallNs = 0;
    u64 steps = 0;
    u64 hash = 0;
    u64 jobs = 0;
    SampleStats stepUs;
    std::vector<profiling::ZoneSummary> zones;
};

// Warms up for one simulated hour, then measures one simulated day step by step.
SimRun runSyntheticDay(const SyntheticGalaxyConfig& config, u32 threads) {
    SyntheticGalaxy galaxy(config);
    JobSystem jobs(threads - 1);
    Simulation simulation(Simulation::Config{.seed = config.seed}, jobs);
    galaxy.registerSystems(simulation);
    simulation.runFor(SimDuration::hours(1));
    profiling::resetStats();

    const SimTime end = simulation.now() + SimDuration::days(1);
    const u64 jobsBefore = jobs.stats().jobsExecuted;
    const u64 stepsBefore = simulation.stepCount();
    std::vector<f64> stepSamples;
    stepSamples.reserve(2048);

    const Stopwatch total;
    while (simulation.nextStepTime() <= end) {
        const Stopwatch stepTimer;
        simulation.step();
        stepSamples.push_back(static_cast<f64>(stepTimer.elapsedNs()) / 1e3);
    }
    SimRun run;
    run.wallNs = total.elapsedNs();
    run.steps = simulation.stepCount() - stepsBefore;
    run.jobs = jobs.stats().jobsExecuted - jobsBefore;
    run.hash = galaxy.stateHash();
    run.stepUs = computeSampleStats(std::move(stepSamples));
    run.zones = profiling::summary();
    return run;
}

f64 zoneMeanUs(const std::vector<profiling::ZoneSummary>& zones, std::string_view name) {
    const profiling::ZoneSummary* zone = profiling::findZone(zones, name);
    return zone != nullptr ? zone->meanNs() / 1e3 : 0.0;
}

void benchSimulationScaling(Report& report, u32 threads, bool quick) {
    section("sim.scaling (synthetic galaxy, full resolution, 1 simulated day, motion 1 min, economy 1 h)");
    const std::vector<u32> scales =
        quick ? std::vector<u32>{1, 10, 100, 1000} : std::vector<u32>{1, 10, 100, 1000, 5000, 10000};

    std::printf("%8s %8s %8s %8s %9s %9s %9s %9s %9s %9s %11s %8s %6s\n", "systems", "bodies", "gen ms",
                "MiB", "step p50", "step p95", "step max", "motion", "economy", "day ms", "sim-days/s",
                "speedup", "det");
    std::printf("%8s %8s %8s %8s %9s %9s %9s %9s %9s %9s %11s %8s %6s\n", "", "", "", "", "us", "us", "us",
                "us", "us", "", "", "vs 1 thr", "");
    for (const u32 systems : scales) {
        SyntheticGalaxyConfig config;
        config.seed = 2026;
        config.starSystems = systems;

        const ProcessMemory before = queryProcessMemory();
        const Stopwatch generationTimer;
        auto galaxy = std::make_unique<SyntheticGalaxy>(config);
        const f64 generationMs = generationTimer.elapsedMs();
        const ProcessMemory after = queryProcessMemory();
        const usize stateBytes = galaxy->stateBytes();
        const usize bodies = galaxy->bodyCount();
        galaxy.reset();

        const SimRun parallel = runSyntheticDay(config, threads);
        const SimRun serial = runSyntheticDay(config, 1);
        const bool deterministic = parallel.hash == serial.hash;

        const f64 dayMs = static_cast<f64>(parallel.wallNs) / 1e6;
        const f64 simDaysPerSecond = 1000.0 / dayMs;
        const f64 speedup = static_cast<f64>(serial.wallNs) / static_cast<f64>(parallel.wallNs);
        const f64 motionUs = zoneMeanUs(parallel.zones, "Synthetic.Motion");
        const f64 economyUs = zoneMeanUs(parallel.zones, "Synthetic.Economy") +
                              zoneMeanUs(parallel.zones, "Synthetic.TradeCompute") +
                              zoneMeanUs(parallel.zones, "Synthetic.TradeApply");

        std::printf("%8u %8zu %8.2f %8.2f %9.1f %9.1f %9.1f %9.1f %9.1f %9.1f %11.1f %8.2f %6s\n", systems,
                    bodies, generationMs, toMiB(stateBytes), parallel.stepUs.p50, parallel.stepUs.p95,
                    parallel.stepUs.max, motionUs, economyUs, dayMs, simDaysPerSecond, speedup,
                    deterministic ? "yes" : "NO");
        std::fflush(stdout);

        report.add(
            "sim.scaling",
            {{"star_systems", Report::integer(systems)},
             {"bodies", Report::integer(bodies)},
             {"threads", Report::integer(threads)},
             {"generation_ms", Report::number(generationMs)},
             {"state_mib", Report::number(toMiB(stateBytes))},
             {"private_mib_delta", Report::number(toMiB(after.privateBytes) - toMiB(before.privateBytes))},
             {"steps_per_day", Report::integer(parallel.steps)},
             {"jobs_per_step",
              Report::number(static_cast<f64>(parallel.jobs) / static_cast<f64>(parallel.steps))},
             {"step_mean_us", Report::number(parallel.stepUs.mean)},
             {"step_p50_us", Report::number(parallel.stepUs.p50)},
             {"step_p95_us", Report::number(parallel.stepUs.p95)},
             {"step_p99_us", Report::number(parallel.stepUs.p99)},
             {"step_max_us", Report::number(parallel.stepUs.max)},
             {"motion_mean_us", Report::number(motionUs)},
             {"economy_tick_mean_us", Report::number(economyUs)},
             {"day_wall_ms", Report::number(dayMs)},
             {"sim_days_per_second", Report::number(simDaysPerSecond)},
             {"serial_day_wall_ms", Report::number(static_cast<f64>(serial.wallNs) / 1e6)},
             {"speedup_vs_1_thread", Report::number(speedup)},
             {"deterministic_across_threads", Report::boolean(deterministic)}});
    }
    std::printf("(save/load and event metrics: not available until the Simulation Kernel milestone)\n");
}

// ---------------------------------------------------------------------------------------------------------

void printUsage() {
    std::printf("Usage: gx_bench [--quick] [--threads <n>] [--filter <substring>] [--out <file.json>]\n");
}

int parseOptions(int argc, char** argv, Options& options) {
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        const auto value = [&]() -> std::string_view {
            return i + 1 < argc ? std::string_view(argv[++i]) : "";
        };
        bool ok = true;
        if (arg == "--help" || arg == "-h") {
            printUsage();
            return 0;
        } else if (arg == "--quick") {
            options.quick = true;
        } else if (arg == "--threads") {
            const std::string_view v = value();
            const auto [end, error] = std::from_chars(v.data(), v.data() + v.size(), options.threads);
            ok = error == std::errc{} && end == v.data() + v.size() && options.threads > 0;
        } else if (arg == "--filter") {
            options.filter = std::string(value());
        } else if (arg == "--out") {
            options.outPath = std::string(value());
            ok = !options.outPath.empty();
        } else {
            ok = false;
        }
        if (!ok) {
            std::fprintf(stderr, "invalid or incomplete argument: %.*s\n", static_cast<int>(arg.size()),
                         arg.data());
            printUsage();
            return 2;
        }
    }
    return -1;
}

std::string timestampUtc() {
    const auto now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
    return std::format("{:%Y%m%d-%H%M%SZ}", now);
}

} // namespace

int main(int argc, char** argv) {
    platform::setCurrentThreadName("main");
    Options options;
    if (const int exitCode = parseOptions(argc, argv, options); exitCode >= 0) {
        return exitCode;
    }
    logging::setLevel(LogLevel::Warn);
    if (options.outPath.empty()) {
        options.outPath = std::format("bench-results/bench-{}.json", timestampUtc());
    }

    const std::string cpu = platform::cpuBrandString();
    std::printf("GalaxyEngine %s benchmarks | build %s | %s | %u hardware threads | using %u threads%s\n",
                GX_VERSION, GX_BUILD_CONFIG, cpu.c_str(), platform::hardwareThreadCount(), options.threads,
                options.quick ? " | quick" : "");
    if (std::string_view(GX_BUILD_CONFIG) != "Release") {
        std::printf("WARNING: not a Release build; numbers are not representative.\n");
    }

    Report report;
    const Stopwatch total;
    if (selected(options, "profiler.zone_overhead")) {
        benchProfilerOverhead(report, options.quick);
    }
    if (selected(options, "jobs.dispatch")) {
        benchJobDispatch(report, options.threads, options.quick);
    }
    if (selected(options, "jobs.scaling")) {
        benchJobScaling(report, options.threads, options.quick);
    }
    if (selected(options, "sim.scaling")) {
        benchSimulationScaling(report, options.threads, options.quick);
    }

    const Report::Fields header = {{"engine_version", Report::text(GX_VERSION)},
                                   {"build", Report::text(GX_BUILD_CONFIG)},
                                   {"timestamp_utc", Report::text(timestampUtc())},
                                   {"cpu", Report::text(cpu)},
                                   {"hardware_threads", Report::integer(platform::hardwareThreadCount())},
                                   {"threads", Report::integer(options.threads)},
                                   {"quick", Report::boolean(options.quick)},
                                   {"total_seconds", Report::number(total.elapsedSeconds())}};
    if (report.write(options.outPath, header)) {
        std::printf("\nreport written to %s (%.1f s)\n", options.outPath.c_str(), total.elapsedSeconds());
    } else {
        std::fprintf(stderr, "could not write report to %s\n", options.outPath.c_str());
        return 1;
    }
    return 0;
}
