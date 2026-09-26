#include "Tests/TestFramework.h"

#include "Engine/Jobs/JobSystem.h"
#include "Engine/Profiling/Profiler.h"
#include "Engine/Profiling/Statistics.h"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <numeric>
#include <string>
#include <vector>

using namespace gx;

#if GX_ENABLE_PROFILING

GX_TEST(Profiling, ZonesAreAggregatedByName) {
    profiling::resetStats();
    for (int i = 0; i < 5; ++i) {
        GX_PROFILE_SCOPE("Test.ZoneA");
    }
    {
        GX_PROFILE_SCOPE("Test.ZoneB");
    }
    const auto zones = profiling::summary();
    const profiling::ZoneSummary* a = profiling::findZone(zones, "Test.ZoneA");
    const profiling::ZoneSummary* b = profiling::findZone(zones, "Test.ZoneB");
    GX_REQUIRE(a != nullptr && b != nullptr);
    GX_EXPECT_EQ(a->count, 5u);
    GX_EXPECT_EQ(b->count, 1u);
    GX_EXPECT(a->minNs <= a->maxNs);
}

GX_TEST(Profiling, DisabledProfilerRecordsNothing) {
    profiling::resetStats();
    profiling::setEnabled(false);
    {
        GX_PROFILE_SCOPE("Test.WhileDisabled");
    }
    profiling::setEnabled(true);
    const auto zones = profiling::summary();
    GX_EXPECT(profiling::findZone(zones, "Test.WhileDisabled") == nullptr);
}

GX_TEST(Profiling, ZonesFromWorkerThreadsAreCollected) {
    profiling::resetStats();
    JobSystem jobs(3);
    jobs.parallelFor(64, 1, [](u32, u32) { GX_PROFILE_SCOPE("Test.WorkerZone"); });
    const auto zones = profiling::summary();
    const profiling::ZoneSummary* zone = profiling::findZone(zones, "Test.WorkerZone");
    GX_REQUIRE(zone != nullptr);
    GX_EXPECT_EQ(zone->count, 64u);
}

GX_TEST(Profiling, PersistentNamesAreInterned) {
    const std::string dynamicName = std::string("Test.") + "Dynamic";
    const char* first = profiling::persistentName(dynamicName);
    const char* second = profiling::persistentName("Test.Dynamic");
    GX_EXPECT(first == second);
    GX_EXPECT(std::strcmp(first, "Test.Dynamic") == 0);
}

GX_TEST(Profiling, ChromeTraceIsWritten) {
    profiling::resetStats();
    profiling::setTraceCapture(true, 1000);
    {
        GX_PROFILE_SCOPE("Test.TraceZone");
    }
    const std::filesystem::path path = std::filesystem::temp_directory_path() / "gx_test_trace.json";
    GX_REQUIRE(profiling::writeChromeTrace(path));
    profiling::setTraceCapture(false);

    std::ifstream in(path);
    const std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();
    std::filesystem::remove(path);
    GX_EXPECT(content.rfind("{\"displayTimeUnit\"", 0) == 0);
    GX_EXPECT(content.find("\"name\":\"Test.TraceZone\",\"ph\":\"X\"") != std::string::npos);
    GX_EXPECT(content.find("\"thread_name\"") != std::string::npos);
}

#endif // GX_ENABLE_PROFILING

GX_TEST(Profiling, SampleStatsPercentiles) {
    std::vector<f64> samples(100);
    std::iota(samples.begin(), samples.end(), 1.0); // 1..100
    const SampleStats stats = computeSampleStats(samples);
    GX_EXPECT_EQ(stats.count, 100u);
    GX_EXPECT_EQ(stats.min, 1.0);
    GX_EXPECT_EQ(stats.max, 100.0);
    GX_EXPECT_EQ(stats.mean, 50.5);
    GX_EXPECT_EQ(stats.p50, 50.0);
    GX_EXPECT_EQ(stats.p95, 95.0);
    GX_EXPECT_EQ(stats.p99, 99.0);
    GX_EXPECT_EQ(computeSampleStats({}).count, 0u);
}
