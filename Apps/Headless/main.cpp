// Headless simulation runner: simulates the synthetic galaxy without any renderer or UI.
// Unpaced by default (as fast as possible); --speed paces it against the wall clock like interactive play.

#include "Engine/Core/Log.h"
#include "Engine/Core/Platform.h"
#include "Engine/Jobs/JobSystem.h"
#include "Engine/Profiling/MemoryUsage.h"
#include "Engine/Profiling/Profiler.h"
#include "Engine/Time/Stopwatch.h"
#include "Engine/Time/TimeController.h"
#include "Scenarios/SyntheticGalaxy.h"
#include "Simulation/Kernel/Simulation.h"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <string>
#include <string_view>
#include <thread>

using namespace gx;

namespace {

constexpr std::string_view kChannel = "Headless";

struct Options {
    u64 seed = 1;
    u32 systems = 1000;
    u32 bodies = 16;
    u32 goods = 8;
    u32 days = 30;
    u32 threads = platform::hardwareThreadCount();
    u32 speed = 0; // 0: unpaced
    std::string tracePath;
    bool profile = true;
    LogLevel logLevel = LogLevel::Info;
};

void printUsage() {
    std::printf("Usage: gx_headless [options]\n"
                "  --seed <n>          world seed (default 1)\n"
                "  --systems <n>       star systems (default 1000)\n"
                "  --bodies <n>        bodies per star system (default 16)\n"
                "  --goods <n>         goods per star system (default 8)\n"
                "  --days <n>          simulated days (default 30)\n"
                "  --threads <n>       total threads including the main thread (default: hardware threads)\n"
                "  --speed <x>         pace against the wall clock at x simulated seconds per real second\n"
                "                      (default: unpaced, as fast as possible)\n"
                "  --trace <file>      write a Chrome/Perfetto trace (https://ui.perfetto.dev)\n"
                "  --no-profile        disable profiling zones\n"
                "  --log-level <lvl>   trace|debug|info|warn|error (default info)\n");
}

template <typename T>
bool parseNumber(std::string_view text, T& out) {
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), out);
    return error == std::errc{} && end == text.data() + text.size();
}

// Returns an exit code if the program should stop, -1 to continue.
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
        } else if (arg == "--seed") {
            ok = parseNumber(value(), options.seed);
        } else if (arg == "--systems") {
            ok = parseNumber(value(), options.systems) && options.systems > 0;
        } else if (arg == "--bodies") {
            ok = parseNumber(value(), options.bodies) && options.bodies > 0;
        } else if (arg == "--goods") {
            ok = parseNumber(value(), options.goods) && options.goods > 0;
        } else if (arg == "--days") {
            ok = parseNumber(value(), options.days) && options.days > 0;
        } else if (arg == "--threads") {
            ok = parseNumber(value(), options.threads) && options.threads > 0;
        } else if (arg == "--speed") {
            ok = parseNumber(value(), options.speed) && options.speed > 0;
        } else if (arg == "--trace") {
            options.tracePath = std::string(value());
            ok = !options.tracePath.empty();
        } else if (arg == "--no-profile") {
            options.profile = false;
        } else if (arg == "--log-level") {
            ok = parseLogLevel(value(), options.logLevel);
        } else {
            ok = false;
        }
        if (!ok) {
            std::fprintf(stderr, "invalid or incomplete argument: %.*s\n\n", static_cast<int>(arg.size()),
                         arg.data());
            printUsage();
            return 2;
        }
    }
    return -1;
}

void logProgress(const Simulation& simulation, const SyntheticGalaxy& galaxy, const Stopwatch& wall) {
    const f64 simulatedDays = simulation.clock().elapsed().toDays();
    GX_LOG_INFO(kChannel, "{} | steps {:>9} | galactic output {:.4e} | {:.1f} sim-days/s",
                formatSimTime(simulation.now()), simulation.stepCount(), galaxy.galacticOutput(),
                simulatedDays / std::max(wall.elapsedSeconds(), 1e-9));
}

void runUnpaced(Simulation& simulation, const SyntheticGalaxy& galaxy, SimTime end, u32 days,
                const Stopwatch& wall) {
    const u32 reportEveryDays = std::max(1u, days / 10);
    while (simulation.now() < end) {
        simulation.runUntil(std::min(end, simulation.now() + SimDuration::days(reportEveryDays)));
        logProgress(simulation, galaxy, wall);
    }
}

void runPaced(Simulation& simulation, const SyntheticGalaxy& galaxy, SimTime end, u32 speed,
              const Stopwatch& wall) {
    constexpr u64 kFrameBudgetNs = 10'000'000; // keep a ~60 Hz frame responsive
    TimeController controller(simulation.now());
    controller.setSpeed(speed);
    const SimDuration reportInterval = SimDuration::seconds(std::max<i64>(1, speed)); // ~once per real second
    SimTime nextReport = simulation.now() + reportInterval;
    u64 lastNs = platform::monotonicNanoseconds();

    while (simulation.now() < end) {
        const u64 nowNs = platform::monotonicNanoseconds();
        const SimTime target = std::min(end, controller.update(nowNs - lastNs, simulation.now()));
        lastNs = nowNs;
        simulation.runUntil(target, kFrameBudgetNs);
        if (simulation.now() >= nextReport) {
            logProgress(simulation, galaxy, wall);
            nextReport = simulation.now() + reportInterval;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(16)); // stand-in for rendering and UI work
    }
    if (controller.droppedTime() > SimDuration{}) {
        GX_LOG_WARN(kChannel, "could not keep up with {}x: dropped {} of simulated time", speed,
                    formatDuration(controller.droppedTime()));
    }
}

} // namespace

int main(int argc, char** argv) {
    platform::setCurrentThreadName("main");
    Options options;
    if (const int exitCode = parseOptions(argc, argv, options); exitCode >= 0) {
        return exitCode;
    }
    logging::setLevel(options.logLevel);
    profiling::setEnabled(options.profile);
    if (!options.tracePath.empty()) {
        profiling::setTraceCapture(true, 4'000'000);
    }

    GX_LOG_INFO(
        kChannel,
        "GalaxyEngine {} ({}) headless | seed {} | {} star systems x {} bodies x {} goods | {} days | "
        "{} threads | {}",
        GX_VERSION, GX_BUILD_CONFIG, options.seed, options.systems, options.bodies, options.goods,
        options.days, options.threads,
        options.speed == 0 ? std::string("unpaced") : std::format("{}x", options.speed));

    SyntheticGalaxyConfig galaxyConfig;
    galaxyConfig.seed = options.seed;
    galaxyConfig.starSystems = options.systems;
    galaxyConfig.bodiesPerSystem = options.bodies;
    galaxyConfig.goodsPerSystem = options.goods;

    const Stopwatch generationTimer;
    SyntheticGalaxy galaxy(galaxyConfig);
    const f64 generationMs = generationTimer.elapsedMs();
    GX_LOG_INFO(kChannel, "generated {} bodies in {:.1f} ms | state {:.1f} MiB | initial hash {:016x}",
                galaxy.bodyCount(), generationMs, toMiB(galaxy.stateBytes()), galaxy.stateHash());

    JobSystem jobs(options.threads - 1);
    Simulation simulation(Simulation::Config{.seed = options.seed}, jobs);
    galaxy.registerSystems(simulation);

    const SimTime end = simulation.now() + SimDuration::days(options.days);
    const Stopwatch wall;
    if (options.speed == 0) {
        runUnpaced(simulation, galaxy, end, options.days, wall);
    } else {
        runPaced(simulation, galaxy, end, options.speed, wall);
    }
    const f64 wallSeconds = wall.elapsedSeconds();

    const JobSystemStats jobStats = jobs.stats();
    const ProcessMemory memory = queryProcessMemory();
    std::printf("\n== Headless run complete ==\n");
    std::printf("simulated          : %u days, now %s\n", options.days,
                formatSimTime(simulation.now()).c_str());
    std::printf("steps              : %llu (%llu system runs)\n",
                static_cast<unsigned long long>(simulation.stepCount()),
                static_cast<unsigned long long>(simulation.systemRunCount()));
    std::printf("wall time          : %.3f s -> %.2f simulated days per second\n", wallSeconds,
                static_cast<f64>(options.days) / std::max(wallSeconds, 1e-9));
    std::printf("state hash         : %016llx\n", static_cast<unsigned long long>(galaxy.stateHash()));
    std::printf("galactic output    : %.6e\n", galaxy.galacticOutput());
    std::printf("jobs executed      : %llu (%llu parallelFor calls)\n",
                static_cast<unsigned long long>(jobStats.jobsExecuted),
                static_cast<unsigned long long>(jobStats.parallelForCalls));
    std::printf(
        "memory             : working set %.1f MiB (peak %.1f MiB), private %.1f MiB, state %.1f MiB\n",
        toMiB(memory.workingSetBytes), toMiB(memory.peakWorkingSetBytes), toMiB(memory.privateBytes),
        toMiB(galaxy.stateBytes()));

    if (options.profile) {
        std::printf("\n%s", profiling::formatSummaryTable(profiling::summary()).c_str());
    }
    if (!options.tracePath.empty()) {
        if (profiling::writeChromeTrace(options.tracePath)) {
            GX_LOG_INFO(kChannel, "trace written to {} ({} events dropped)", options.tracePath,
                        profiling::droppedTraceEvents());
        } else {
            GX_LOG_ERROR(kChannel, "could not write trace to {}", options.tracePath);
            return 1;
        }
    }
    return 0;
}
