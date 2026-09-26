// Headless simulation runner: simulates the synthetic galaxy without any renderer or UI.
// Unpaced by default (as fast as possible); --speed paces it against the wall clock like interactive play.
// Also the first save tool: --save/--load continue a campaign, --inspect describes a save file, and
// --record/--replay verify that a recorded command sequence reproduces the same final state.

#include "Engine/Core/Log.h"
#include "Engine/Core/Platform.h"
#include "Engine/Core/Random.h"
#include "Engine/Jobs/JobSystem.h"
#include "Engine/Profiling/MemoryUsage.h"
#include "Engine/Profiling/Profiler.h"
#include "Engine/Serialization/Binary.h"
#include "Engine/Serialization/SaveFile.h"
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
constexpr u32 kReplayChunk = fourCC("RPLY");

struct Options {
    u64 seed = 1;
    u32 systems = 1000;
    u32 bodies = 16;
    u32 goods = 8;
    u32 days = 30;
    u32 threads = platform::hardwareThreadCount();
    u32 speed = 0; // 0: unpaced
    bool raids = false;
    std::string tracePath;
    std::string savePath;
    std::string loadPath;
    std::string inspectPath;
    std::string recordPath;
    std::string replayPath;
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
                "  --raids             submit a RaidConvoys command every simulated day (external input)\n"
                "  --save <file>       save the simulation at the end of the run\n"
                "  --load <file>       continue from a save (same --systems/--bodies/--goods)\n"
                "  --inspect <file>    describe a save file and exit\n"
                "  --record <file>     record the applied commands and the final state hash\n"
                "  --replay <file>     re-run a recording and verify the final state hash\n"
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
        const auto path = [&](std::string& out) {
            out = std::string(value());
            return !out.empty();
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
        } else if (arg == "--raids") {
            options.raids = true;
        } else if (arg == "--save") {
            ok = path(options.savePath);
        } else if (arg == "--load") {
            ok = path(options.loadPath);
        } else if (arg == "--inspect") {
            ok = path(options.inspectPath);
        } else if (arg == "--record") {
            ok = path(options.recordPath);
        } else if (arg == "--replay") {
            ok = path(options.replayPath);
        } else if (arg == "--trace") {
            ok = path(options.tracePath);
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
    if (!options.recordPath.empty() && (!options.loadPath.empty() || !options.replayPath.empty())) {
        std::fprintf(
            stderr,
            "--record starts from a generated galaxy: it cannot be combined with --load or --replay\n");
        return 2;
    }
    return -1;
}

int inspectSave(const std::string& path) {
    SaveFileContents save;
    std::string error;
    if (!readSaveFile(path, save, error)) {
        std::fprintf(stderr, "cannot read %s: %s\n", path.c_str(), error.c_str());
        return 1;
    }
    std::printf("file            : %s\n", path.c_str());
    std::printf("format version  : %u\n", save.info.formatVersion);
    std::printf("engine version  : %s\n", save.info.engineVersion.c_str());
    std::printf("description     : %s\n", save.info.description.c_str());
    std::printf("payload         : %llu bytes, hash %016llx (verified)\n",
                static_cast<unsigned long long>(save.info.payloadBytes),
                static_cast<unsigned long long>(save.info.payloadHash));
    std::printf("chunks          :\n");
    for (const ChunkInfo& chunk : listChunks(save.payload)) {
        std::printf("  %s v%u  %12llu bytes at offset %llu\n", fourCCToString(chunk.tag).c_str(),
                    chunk.version, static_cast<unsigned long long>(chunk.size),
                    static_cast<unsigned long long>(chunk.offset));
    }
    return 0;
}

// A recording: the parameters that generate the initial state, the applied commands and the final hash.
struct Recording {
    u64 seed = 0;
    u32 systems = 0;
    u32 bodies = 0;
    u32 goods = 0;
    u32 days = 0;
    std::vector<CommandRecord> commands;
    u64 finalHash = 0;

    template <typename Archive>
    void io(Archive& ar) {
        ar.io(seed);
        ar.io(systems);
        ar.io(bodies);
        ar.io(goods);
        ar.io(days);
        ar.io(commands);
        ar.io(finalHash);
    }
};

bool readRecording(const std::string& path, Recording& recording, std::string& error) {
    SaveFileContents file;
    if (!readSaveFile(path, file, error)) {
        return false;
    }
    BinaryReader reader(file.payload);
    BinaryReader::Chunk chunk;
    if (reader.beginChunk(kReplayChunk, chunk)) {
        reader.io(recording);
        reader.endChunk(chunk);
    }
    if (!reader.ok()) {
        error = reader.error();
        return false;
    }
    return true;
}

void logProgress(const Simulation& simulation, const SyntheticGalaxy& galaxy, const Stopwatch& wall,
                 SimTime runStart) {
    const f64 simulatedDays = (simulation.now() - runStart).toDays();
    GX_LOG_INFO(kChannel, "{} | steps {:>9} | convoys {:>6} | output {:.4e} | {:.1f} sim-days/s",
                formatSimTime(simulation.now()), simulation.stepCount(), galaxy.activeConvoys(),
                galaxy.galacticOutput(), simulatedDays / std::max(wall.elapsedSeconds(), 1e-9));
}

void runUnpaced(Simulation& simulation, const SyntheticGalaxy& galaxy, SimTime end, u32 days,
                const Stopwatch& wall) {
    const SimTime runStart = simulation.now();
    const u32 reportEveryDays = std::max(1u, days / 10);
    while (simulation.now() < end) {
        simulation.runUntil(std::min(end, simulation.now() + SimDuration::days(reportEveryDays)));
        logProgress(simulation, galaxy, wall, runStart);
    }
}

void runPaced(Simulation& simulation, const SyntheticGalaxy& galaxy, SimTime end, u32 speed,
              const Stopwatch& wall) {
    constexpr u64 kFrameBudgetNs = 10'000'000; // keep a ~60 Hz frame responsive
    const SimTime runStart = simulation.now();
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
            logProgress(simulation, galaxy, wall, runStart);
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
    if (!options.inspectPath.empty()) {
        return inspectSave(options.inspectPath);
    }

    Recording replay;
    if (!options.replayPath.empty()) {
        std::string error;
        if (!readRecording(options.replayPath, replay, error)) {
            GX_LOG_ERROR(kChannel, "cannot read replay {}: {}", options.replayPath, error);
            return 1;
        }
        options.seed = replay.seed;
        options.systems = replay.systems;
        options.bodies = replay.bodies;
        options.goods = replay.goods;
        options.days = replay.days;
        options.raids = false; // the recorded commands are the input
    }

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

    JobSystem jobs(options.threads - 1);
    Simulation simulation(Simulation::Config{.seed = options.seed}, jobs);
    galaxy.install(simulation);
    GX_LOG_INFO(kChannel, "generated {} bodies in {:.1f} ms | galaxy arrays {:.1f} MiB", galaxy.bodyCount(),
                generationMs, toMiB(galaxy.stateBytes()));

    if (!options.loadPath.empty()) {
        SaveFileContents save;
        std::string error;
        const Stopwatch loadTimer;
        if (!readSaveFile(options.loadPath, save, error) || !simulation.loadState(save.payload, error)) {
            GX_LOG_ERROR(kChannel, "cannot load {}: {}", options.loadPath, error);
            return 1;
        }
        GX_LOG_INFO(kChannel, "loaded {} ({:.1f} MiB) in {:.1f} ms: {} | {} convoys in flight",
                    options.loadPath, toMiB(save.payload.size()), loadTimer.elapsedMs(),
                    formatSimTime(simulation.now()), galaxy.activeConvoys());
    }

    const SimTime start = simulation.now();
    const SimTime end = start + SimDuration::days(options.days);
    simulation.commands().setRecording(!options.recordPath.empty());
    if (options.raids) {
        // Stand-in for player input: one raid per simulated day at noon, on a seeded pseudo-random system.
        Rng rng = Rng::forStream(options.seed, fnv1a64("headless.raids"),
                                 static_cast<u64>(start.microsecondsSinceEpoch()));
        for (u32 day = 0; day < options.days; ++day) {
            simulation.submitCommand(RaidConvoysCommand{rng.uniformU32(options.systems)},
                                     start + SimDuration::days(day) + SimDuration::hours(12));
        }
    }
    for (const CommandRecord& record : replay.commands) {
        simulation.commands().submitRecord(record);
    }

    const Stopwatch wall;
    if (options.speed == 0) {
        runUnpaced(simulation, galaxy, end, options.days, wall);
    } else {
        runPaced(simulation, galaxy, end, options.speed, wall);
    }
    const f64 wallSeconds = wall.elapsedSeconds();
    const u64 finalHash = simulation.stateHash();

    const JobSystemStats jobStats = jobs.stats();
    const SimulationStats& simStats = simulation.stats();
    const SyntheticGalaxyStats& galaxyStats = galaxy.stats();
    const ProcessMemory memory = queryProcessMemory();
    std::printf("\n== Headless run complete ==\n");
    std::printf("simulated          : %u days, now %s\n", options.days,
                formatSimTime(simulation.now()).c_str());
    std::printf("steps              : %llu (%llu system runs)\n",
                static_cast<unsigned long long>(simStats.steps),
                static_cast<unsigned long long>(simStats.systemRuns));
    std::printf("wall time          : %.3f s -> %.2f simulated days per second\n", wallSeconds,
                static_cast<f64>(options.days) / std::max(wallSeconds, 1e-9));
    std::printf("state hash         : %016llx\n", static_cast<unsigned long long>(finalHash));
    std::printf("entities           : %u alive (%u slots)\n", simulation.world().entityCount(),
                simulation.world().registry().slotCount());
    std::printf("convoys            : %llu spawned, %llu arrived, %llu raided, %zu in flight\n",
                static_cast<unsigned long long>(galaxyStats.convoysSpawned),
                static_cast<unsigned long long>(galaxyStats.convoysArrived),
                static_cast<unsigned long long>(galaxyStats.convoysRaided), galaxy.activeConvoys());
    std::printf("events / commands  : %llu events, %llu commands applied\n",
                static_cast<unsigned long long>(simStats.eventsEmitted),
                static_cast<unsigned long long>(simStats.commandsApplied));
    std::printf("galactic output    : %.6e\n", galaxy.galacticOutput());
    std::printf("jobs executed      : %llu (%llu parallelFor calls)\n",
                static_cast<unsigned long long>(jobStats.jobsExecuted),
                static_cast<unsigned long long>(jobStats.parallelForCalls));
    std::printf("memory             : working set %.1f MiB (peak %.1f MiB), private %.1f MiB\n",
                toMiB(memory.workingSetBytes), toMiB(memory.peakWorkingSetBytes), toMiB(memory.privateBytes));

    if (options.profile) {
        std::printf("\n%s", profiling::formatSummaryTable(profiling::summary()).c_str());
    }

    int exitCode = 0;
    if (!options.savePath.empty()) {
        const Stopwatch saveTimer;
        const std::vector<std::byte> payload = simulation.saveState();
        std::string error;
        const std::string description =
            std::format("synthetic galaxy {}x{}x{}, {}", options.systems, options.bodies, options.goods,
                        formatSimTime(simulation.now()));
        if (writeSaveFile(options.savePath, GX_VERSION, description, payload, error)) {
            GX_LOG_INFO(kChannel, "saved {} ({:.1f} MiB) in {:.1f} ms", options.savePath,
                        toMiB(payload.size()), saveTimer.elapsedMs());
        } else {
            GX_LOG_ERROR(kChannel, "cannot save {}: {}", options.savePath, error);
            exitCode = 1;
        }
    }
    if (!options.recordPath.empty()) {
        Recording recording{options.seed,  options.systems, options.bodies,
                            options.goods, options.days,    simulation.commands().recorded(),
                            finalHash};
        BinaryWriter writer;
        const auto mark = writer.beginChunk(kReplayChunk, 1);
        writer.io(recording);
        writer.endChunk(mark);
        std::string error;
        if (writeSaveFile(options.recordPath, GX_VERSION, "replay", writer.bytes(), error)) {
            GX_LOG_INFO(kChannel, "recorded {} commands and final hash {:016x} to {}",
                        recording.commands.size(), finalHash, options.recordPath);
        } else {
            GX_LOG_ERROR(kChannel, "cannot write recording {}: {}", options.recordPath, error);
            exitCode = 1;
        }
    }
    if (!options.replayPath.empty()) {
        if (finalHash == replay.finalHash) {
            std::printf("\nreplay            : MATCH (%zu commands, hash %016llx)\n", replay.commands.size(),
                        static_cast<unsigned long long>(finalHash));
        } else {
            std::printf("\nreplay            : MISMATCH (recorded %016llx, replayed %016llx)\n",
                        static_cast<unsigned long long>(replay.finalHash),
                        static_cast<unsigned long long>(finalHash));
            exitCode = 1;
        }
    }
    if (!options.tracePath.empty()) {
        if (profiling::writeChromeTrace(options.tracePath)) {
            GX_LOG_INFO(kChannel, "trace written to {} ({} events dropped)", options.tracePath,
                        profiling::droppedTraceEvents());
        } else {
            GX_LOG_ERROR(kChannel, "could not write trace to {}", options.tracePath);
            exitCode = 1;
        }
    }
    return exitCode;
}
