#include "Benchmarks/BenchCommon.h"

#include "Engine/Jobs/JobSystem.h"
#include "Engine/Profiling/MemoryUsage.h"
#include "Engine/Profiling/Profiler.h"
#include "Engine/Profiling/Statistics.h"
#include "Engine/Serialization/SaveFile.h"
#include "Engine/Time/Stopwatch.h"
#include "Scenarios/SyntheticGalaxy.h"
#include "Simulation/Kernel/Simulation.h"

#include <memory>

namespace gx::bench {
namespace {

struct SimRun {
    u64 wallNs = 0;
    u64 steps = 0;
    u64 hash = 0;
    u64 jobs = 0;
    u64 events = 0;
    usize convoys = 0;
    SampleStats stepUs;
    std::vector<profiling::ZoneSummary> zones;
};

// Warms up for one simulated hour, then measures one simulated day step by step.
SimRun runSyntheticDay(const SyntheticGalaxyConfig& config, u32 threads) {
    SyntheticGalaxy galaxy(config);
    JobSystem jobs(threads - 1);
    Simulation simulation(Simulation::Config{.seed = config.seed}, jobs);
    galaxy.install(simulation);
    simulation.runFor(SimDuration::hours(1));
    profiling::resetStats();

    const SimTime end = simulation.now() + SimDuration::days(1);
    const u64 jobsBefore = jobs.stats().jobsExecuted;
    const u64 stepsBefore = simulation.stepCount();
    const u64 eventsBefore = simulation.stats().eventsEmitted;
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
    run.events = simulation.stats().eventsEmitted - eventsBefore;
    run.convoys = galaxy.activeConvoys();
    run.hash = simulation.stateHash();
    run.stepUs = computeSampleStats(std::move(stepSamples));
    run.zones = profiling::summary();
    return run;
}

f64 zoneMeanUs(const std::vector<profiling::ZoneSummary>& zones, std::string_view name) {
    const profiling::ZoneSummary* zone = profiling::findZone(zones, name);
    return zone != nullptr ? zone->meanNs() / 1e3 : 0.0;
}

SyntheticGalaxyConfig scalingConfig(u32 systems) {
    SyntheticGalaxyConfig config;
    config.seed = 2026;
    config.starSystems = systems;
    return config;
}

} // namespace

void benchSimulationScaling(Report& report, const Options& options) {
    section("sim.scaling (synthetic galaxy, full resolution, 1 simulated day, motion 1 min, economy 1 h)");
    const std::vector<u32> scales =
        options.quick ? std::vector<u32>{1, 10, 100, 1000} : std::vector<u32>{1, 10, 100, 1000, 5000, 10000};

    std::printf("%8s %8s %7s %6s %8s %8s %8s %8s %8s %8s %8s %10s %7s %4s\n", "systems", "bodies", "gen ms",
                "MiB", "step p50", "step p95", "motion", "economy", "convoys", "events", "day ms",
                "sim-days/s", "speedup", "det");
    for (const u32 systems : scales) {
        const SyntheticGalaxyConfig config = scalingConfig(systems);

        const ProcessMemory before = queryProcessMemory();
        const Stopwatch generationTimer;
        auto galaxy = std::make_unique<SyntheticGalaxy>(config);
        const f64 generationMs = generationTimer.elapsedMs();
        const ProcessMemory after = queryProcessMemory();
        const usize stateBytes = galaxy->stateBytes();
        const usize bodies = galaxy->bodyCount();
        galaxy.reset();

        const SimRun parallel = runSyntheticDay(config, options.threads);
        const SimRun serial = runSyntheticDay(config, 1);
        const bool deterministic = parallel.hash == serial.hash;

        const f64 dayMs = static_cast<f64>(parallel.wallNs) / 1e6;
        const f64 simDaysPerSecond = 1000.0 / dayMs;
        const f64 speedup = static_cast<f64>(serial.wallNs) / static_cast<f64>(parallel.wallNs);
        const f64 motionUs = zoneMeanUs(parallel.zones, "Synthetic.Motion");
        const f64 economyUs = zoneMeanUs(parallel.zones, "Synthetic.Economy") +
                              zoneMeanUs(parallel.zones, "Synthetic.TradeCompute") +
                              zoneMeanUs(parallel.zones, "Synthetic.TradeApply");

        std::printf("%8u %8zu %7.2f %6.2f %8.1f %8.1f %8.1f %8.1f %8zu %8llu %8.1f %10.1f %7.2f %4s\n",
                    systems, bodies, generationMs, toMiB(stateBytes), parallel.stepUs.p50,
                    parallel.stepUs.p95, motionUs, economyUs, parallel.convoys,
                    static_cast<unsigned long long>(parallel.events), dayMs, simDaysPerSecond, speedup,
                    deterministic ? "yes" : "NO");
        std::fflush(stdout);

        report.add(
            "sim.scaling",
            {{"star_systems", Report::integer(systems)},
             {"bodies", Report::integer(bodies)},
             {"threads", Report::integer(options.threads)},
             {"generation_ms", Report::number(generationMs)},
             {"state_mib", Report::number(toMiB(stateBytes))},
             {"private_mib_delta", Report::number(toMiB(after.privateBytes) - toMiB(before.privateBytes))},
             {"steps_per_day", Report::integer(parallel.steps)},
             {"jobs_per_step",
              Report::number(static_cast<f64>(parallel.jobs) / static_cast<f64>(parallel.steps))},
             {"events_per_day", Report::integer(parallel.events)},
             {"convoys_in_flight", Report::integer(parallel.convoys)},
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
}

void benchGrain(Report& report, const Options& options) {
    section("sim.grain (motion chunk size vs day wall time)");
    const std::vector<u32> scales =
        options.quick ? std::vector<u32>{100, 1000} : std::vector<u32>{100, 1000, 10000};
    constexpr int kRepetitions = 5;
    const std::vector<u32> grains = {256, 512, 1024, 2048, 4096, 8192};
    profiling::setEnabled(false); // chunk-level zones would bias small grains

    std::printf("%8s", "systems");
    for (const u32 grain : grains) {
        std::printf(" %9s", std::format("g={}", grain).c_str());
    }
    std::printf("   (day ms, median of %d)\n", kRepetitions);
    for (const u32 systems : scales) {
        std::printf("%8u", systems);
        for (const u32 grain : grains) {
            SyntheticGalaxyConfig config = scalingConfig(systems);
            config.motionGrain = grain;
            std::vector<f64> samples;
            for (int r = 0; r < kRepetitions; ++r) {
                samples.push_back(static_cast<f64>(runSyntheticDay(config, options.threads).wallNs) / 1e6);
            }
            const f64 dayMs = computeSampleStats(samples).p50;
            std::printf(" %9.1f", dayMs);
            std::fflush(stdout);
            report.add("sim.grain", {{"star_systems", Report::integer(systems)},
                                     {"motion_grain", Report::integer(grain)},
                                     {"threads", Report::integer(options.threads)},
                                     {"day_wall_ms", Report::number(dayMs)}});
        }
        std::printf("\n");
    }
    profiling::setEnabled(true);
}

void benchSaveLoad(Report& report, const Options& options) {
    section("sim.saveload (after 1 simulated day; round trip verified by continuing 6 h)");
    const std::vector<u32> scales = options.quick ? std::vector<u32>{1000} : std::vector<u32>{1000, 10000};
    const std::filesystem::path path = std::filesystem::temp_directory_path() / "gx_bench_save.gxsave";

    std::printf("%8s %10s %10s %10s %10s %10s %10s %6s\n", "systems", "entities", "MiB", "save ms",
                "write ms", "read ms", "load ms", "match");
    for (const u32 systems : scales) {
        const SyntheticGalaxyConfig config = scalingConfig(systems);
        JobSystem jobs(options.threads - 1);

        SyntheticGalaxy original(config);
        Simulation simulation(Simulation::Config{.seed = config.seed}, jobs);
        original.install(simulation);
        simulation.runFor(SimDuration::days(1));

        const Stopwatch saveTimer;
        const std::vector<std::byte> payload = simulation.saveState();
        const f64 saveMs = saveTimer.elapsedMs();
        std::string error;
        const Stopwatch writeTimer;
        const bool written = writeSaveFile(path, GX_VERSION, "benchmark", payload, error);
        const f64 writeMs = writeTimer.elapsedMs();

        SaveFileContents contents;
        const Stopwatch readTimer;
        const bool read = written && readSaveFile(path, contents, error);
        const f64 readMs = readTimer.elapsedMs();

        SyntheticGalaxy restoredGalaxy(config);
        Simulation restored(Simulation::Config{.seed = config.seed}, jobs);
        restoredGalaxy.install(restored);
        const Stopwatch loadTimer;
        const bool loaded = read && restored.loadState(contents.payload, error);
        const f64 loadMs = loadTimer.elapsedMs();

        simulation.runFor(SimDuration::hours(6));
        if (loaded) {
            restored.runFor(SimDuration::hours(6));
        }
        const bool match = loaded && simulation.stateHash() == restored.stateHash();
        if (!loaded) {
            std::printf("save/load failed: %s\n", error.c_str());
        }

        std::printf("%8u %10u %10.2f %10.2f %10.2f %10.2f %10.2f %6s\n", systems,
                    simulation.world().entityCount(), toMiB(payload.size()), saveMs, writeMs, readMs, loadMs,
                    match ? "yes" : "NO");
        report.add("sim.saveload", {{"star_systems", Report::integer(systems)},
                                    {"payload_mib", Report::number(toMiB(payload.size()))},
                                    {"save_ms", Report::number(saveMs)},
                                    {"write_file_ms", Report::number(writeMs)},
                                    {"read_file_ms", Report::number(readMs)},
                                    {"load_ms", Report::number(loadMs)},
                                    {"round_trip_match", Report::boolean(match)}});
    }
    std::error_code ec;
    std::filesystem::remove(path, ec);
}

} // namespace gx::bench
