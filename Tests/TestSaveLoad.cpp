#include "Tests/TestFramework.h"

#include "Engine/Jobs/JobSystem.h"
#include "Engine/Serialization/SaveFile.h"
#include "Scenarios/SyntheticGalaxy.h"
#include "Simulation/Kernel/Simulation.h"

#include <filesystem>
#include <memory>

using namespace gx;

namespace {

SyntheticGalaxyConfig galaxyConfig(u32 systems = 60) {
    SyntheticGalaxyConfig config;
    config.seed = 404;
    config.starSystems = systems;
    config.bodiesPerSystem = 6;
    config.goodsPerSystem = 5;
    config.motionPeriod = SimDuration::minutes(5);
    config.motionGrain = 64;
    config.systemGrain = 8;
    config.convoyGrain = 16;
    return config;
}

// A simulation plus everything that must outlive it.
struct Session {
    explicit Session(u32 workers, const SyntheticGalaxyConfig& config = galaxyConfig())
        : jobs(workers), galaxy(config), simulation({.seed = config.seed}, jobs) {
        galaxy.install(simulation);
    }
    JobSystem jobs;
    SyntheticGalaxy galaxy;
    Simulation simulation;
};

constexpr SimTime at(i64 hours) {
    return SimTime::epoch() + SimDuration::hours(hours);
}

} // namespace

GX_TEST(SaveLoad, ContinuingFromASaveMatchesAnUninterruptedRun) {
    Session continuous(3);
    continuous.simulation.runUntil(at(40));

    Session first(3);
    first.simulation.runUntil(at(13) + SimDuration::minutes(7)); // mid-way between scheduled instants
    const std::vector<std::byte> saved = first.simulation.saveState();
    GX_EXPECT(first.galaxy.activeConvoys() > 0); // convoys (entities) in flight are part of the save

    Session resumed(1); // thread count may differ
    std::string error;
    GX_REQUIRE(resumed.simulation.loadState(saved, error));
    GX_EXPECT(resumed.simulation.saveState() == saved); // loading then saving is lossless
    GX_EXPECT_EQ(resumed.galaxy.activeConvoys(), first.galaxy.activeConvoys());
    resumed.simulation.runUntil(at(40));

    GX_EXPECT_EQ(resumed.simulation.stateHash(), continuous.simulation.stateHash());
    GX_EXPECT_EQ(resumed.simulation.stepCount(), continuous.simulation.stepCount());
    GX_EXPECT_EQ(resumed.galaxy.stats().convoysArrived, continuous.galaxy.stats().convoysArrived);
}

GX_TEST(SaveLoad, PendingCommandsAndRateChangesSurvive) {
    const auto prepare = [](Simulation& simulation) {
        simulation.runUntil(at(5));
        simulation.setSystemPeriod(simulation.findSystem("Synthetic.Motion"), SimDuration::minutes(30));
        simulation.submitCommand(SpawnConvoyCommand{1, 2, 0, 5.0, 20.0}, at(9));
        simulation.submitCommand(RaidConvoysCommand{3}, at(11));
    };
    Session continuous(2);
    prepare(continuous.simulation);
    continuous.simulation.runUntil(at(30));

    Session saver(2);
    prepare(saver.simulation);
    const std::vector<std::byte> saved = saver.simulation.saveState();

    Session resumed(2);
    std::string error;
    GX_REQUIRE(resumed.simulation.loadState(saved, error));
    GX_EXPECT_EQ(resumed.simulation.commands().pendingCount(), 2u);
    GX_EXPECT(resumed.simulation.scheduler()
                  .system(resumed.simulation.findSystem("Synthetic.Motion"))
                  .desc.period == SimDuration::minutes(30));
    resumed.simulation.runUntil(at(30));
    GX_EXPECT_EQ(resumed.simulation.stateHash(), continuous.simulation.stateHash());
    GX_EXPECT_EQ(resumed.simulation.stats().commandsApplied, 2u);
}

GX_TEST(SaveLoad, SaveFilesOnDiskRoundTrip) {
    Session original(2);
    original.simulation.runUntil(at(20));
    const auto path = std::filesystem::temp_directory_path() / "gx_test_simulation.gxsave";
    std::string error;
    GX_REQUIRE(writeSaveFile(path, "test", "SaveLoad test", original.simulation.saveState(), error));
    SaveFileContents contents;
    GX_REQUIRE(readSaveFile(path, contents, error));
    std::filesystem::remove(path);

    Session loaded(2);
    GX_REQUIRE(loaded.simulation.loadState(contents.payload, error));
    GX_EXPECT_EQ(loaded.simulation.stateHash(), original.simulation.stateHash());
    GX_EXPECT(loaded.simulation.now() == at(20));
}

GX_TEST(SaveLoad, RejectsSavesFromDifferentSetups) {
    Session original(0);
    original.simulation.runUntil(at(2));
    const std::vector<std::byte> saved = original.simulation.saveState();
    std::string error;

    Session otherSize(0, galaxyConfig(61));
    GX_EXPECT(!otherSize.simulation.loadState(saved, error));
    GX_EXPECT(error.find("dimensions") != std::string::npos);

    JobSystem jobs(0);
    Simulation bare({}, jobs); // no systems, components or blocks registered
    GX_EXPECT(!bare.loadState(saved, error));
    GX_EXPECT(error.find("systems") != std::string::npos);
}

GX_TEST(SaveLoad, RejectsCorruptPayloads) {
    Session original(0);
    original.simulation.runUntil(at(2));
    std::vector<std::byte> saved = original.simulation.saveState();
    std::string error;

    std::vector<std::byte> truncated(saved.begin(),
                                     saved.begin() + static_cast<std::ptrdiff_t>(saved.size() / 2));
    Session a(0);
    GX_EXPECT(!a.simulation.loadState(truncated, error));

    std::vector<std::byte> trailing = saved;
    trailing.push_back(std::byte{0});
    Session b(0);
    GX_EXPECT(!b.simulation.loadState(trailing, error));
    GX_EXPECT(error.find("unexpected data") != std::string::npos);

    Session c(0);
    GX_EXPECT(!c.simulation.loadState({}, error));
}
