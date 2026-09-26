// Engine benchmarks. Prints tables to stdout and writes a JSON report for tracking results over time.
//
//   profiler.zone_overhead  cost of a profiling zone, enabled and disabled
//   jobs.dispatch           latency of an (almost) empty parallelFor and of one submitted job
//   jobs.scaling            speedup of a compute-bound parallelFor across thread counts
//   entity.storage          memory layouts for entities: dense store vs objects vs hash map
//   sim.scaling             synthetic galaxy from 1 to 10,000 star systems, full resolution (no LOD):
//                           generation, memory, per-step cost, throughput, determinism across thread counts
//   sim.grain               motion chunk size vs wall time
//   sim.saveload            save/load cost and round-trip verification

#include "Benchmarks/BenchCommon.h"

#include "Engine/Core/Log.h"
#include "Engine/Core/Platform.h"
#include "Engine/Time/Stopwatch.h"

#include <charconv>
#include <chrono>

using namespace gx;
using namespace gx::bench;

namespace {

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
    options.threads = platform::hardwareThreadCount();
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

    struct Benchmark {
        const char* name;
        void (*run)(Report&, const Options&);
    };
    const Benchmark benchmarks[] = {
        {"profiler.zone_overhead", &benchProfilerOverhead},
        {"jobs.dispatch", &benchJobDispatch},
        {"jobs.scaling", &benchJobScaling},
        {"entity.storage", &benchEntityStorage},
        {"sim.scaling", &benchSimulationScaling},
        {"sim.grain", &benchGrain},
        {"sim.saveload", &benchSaveLoad},
    };

    Report report;
    const Stopwatch total;
    for (const Benchmark& benchmark : benchmarks) {
        if (options.filter.empty() ||
            std::string_view(benchmark.name).find(options.filter) != std::string_view::npos) {
            benchmark.run(report, options);
        }
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
