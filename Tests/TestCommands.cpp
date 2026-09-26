#include "Tests/TestFramework.h"

#include "Engine/Jobs/JobSystem.h"
#include "Scenarios/SyntheticGalaxy.h"
#include "Simulation/Kernel/Simulation.h"

#include <string>
#include <vector>

using namespace gx;

namespace {

struct Note {
    u32 id = 0;
    template <typename Archive>
    void io(Archive& ar) {
        ar.io(id);
    }
};

struct Recorder {
    std::vector<std::string> log;
    std::vector<SimTime> times;
};

void installRecorder(Simulation& simulation, Recorder& recorder) {
    simulation.commands().registerCommand<Note>(
        "Test.Note", [&recorder](const Note& note, const TickContext& c) {
            recorder.log.push_back("command " + std::to_string(note.id));
            recorder.times.push_back(c.now);
        });
    simulation.addSystem(
        {"Hourly", TickPhase::Commands, SimDuration::hours(1), {}, [&recorder](const TickContext&) {
             recorder.log.emplace_back("system");
         }});
}

constexpr SimTime at(i64 minutes) {
    return SimTime::epoch() + SimDuration::minutes(minutes);
}

SyntheticGalaxyConfig smallGalaxy() {
    SyntheticGalaxyConfig config;
    config.seed = 77;
    config.starSystems = 40;
    config.bodiesPerSystem = 4;
    config.goodsPerSystem = 4;
    config.motionPeriod = SimDuration::minutes(10);
    return config;
}

} // namespace

GX_TEST(Commands, ExecuteExactlyAtTheirTime) {
    JobSystem jobs(0);
    Simulation simulation({}, jobs);
    Recorder recorder;
    installRecorder(simulation, recorder);
    simulation.submitCommand(Note{1}, at(90));
    simulation.runUntil(at(180));
    GX_REQUIRE(recorder.times.size() == 1);
    GX_EXPECT(recorder.times[0] == at(90));   // a step is inserted between the hourly runs
    GX_EXPECT_EQ(simulation.stepCount(), 4u); // 1h, 1h30, 2h, 3h
    GX_EXPECT_EQ(simulation.stats().commandsApplied, 1u);
}

GX_TEST(Commands, RunBeforeSystemsOfTheSameInstant) {
    JobSystem jobs(0);
    Simulation simulation({}, jobs);
    Recorder recorder;
    installRecorder(simulation, recorder);
    simulation.submitCommand(Note{1}, at(60));
    simulation.runUntil(at(60));
    GX_EXPECT(recorder.log == (std::vector<std::string>{"command 1", "system"}));
}

GX_TEST(Commands, AreOrderedByTimeThenSubmission) {
    JobSystem jobs(0);
    Simulation simulation({}, jobs);
    Recorder recorder;
    installRecorder(simulation, recorder);
    simulation.submitCommand(Note{1}, at(30));
    simulation.submitCommand(Note{2}, at(30));
    simulation.submitCommand(Note{3}, at(10));
    simulation.submitCommand(Note{4}, at(30));
    simulation.runUntil(at(45));
    GX_EXPECT(recorder.log == (std::vector<std::string>{"command 3", "command 1", "command 2", "command 4"}));
}

GX_TEST(Commands, NeverExecuteInThePast) {
    JobSystem jobs(0);
    Simulation simulation({}, jobs);
    Recorder recorder;
    installRecorder(simulation, recorder);
    simulation.runUntil(at(300));
    simulation.submitCommand(Note{1}, SimTime::epoch()); // "now", or even earlier, means the next instant
    simulation.runUntil(at(301));
    GX_REQUIRE(recorder.times.size() == 1);
    GX_EXPECT(recorder.times[0] == at(300) + SimDuration::microseconds(1));
}

GX_TEST(Commands, InvalidInputIsRejectedNotFatal) {
    JobSystem jobs(0);
    SyntheticGalaxy galaxy(smallGalaxy());
    Simulation simulation({.seed = 77}, jobs);
    galaxy.install(simulation);
    simulation.submitCommand(SpawnConvoyCommand{999, 1, 0, 10.0, 24.0}); // no such system
    simulation.submitCommand(SpawnConvoyCommand{0, 0, 0, 10.0, 24.0});   // origin == destination
    simulation.submitCommand(RaidConvoysCommand{12345});
    simulation.runFor(SimDuration::minutes(1));
    GX_EXPECT_EQ(galaxy.stats().commandsRejected, 3u);
}

GX_TEST(Commands, SpawnAndRaidChangeTheWorld) {
    JobSystem jobs(0);
    SyntheticGalaxyConfig config = smallGalaxy();
    config.convoys = false; // only the commanded convoys exist
    SyntheticGalaxy galaxy(config);
    Simulation simulation({.seed = 77}, jobs);
    galaxy.install(simulation);
    const f64 stockBefore = galaxy.stock(3, 1);
    simulation.submitCommand(SpawnConvoyCommand{3, 5, 1, 10.0, 48.0});
    simulation.submitCommand(SpawnConvoyCommand{3, 6, 1, 10.0, 48.0});
    simulation.runFor(SimDuration::minutes(1));
    GX_EXPECT_EQ(galaxy.activeConvoys(), 2u);
    GX_EXPECT_NEAR(galaxy.stock(3, 1), stockBefore - 20.0, 1e-9); // cargo leaves the origin at departure

    simulation.submitCommand(RaidConvoysCommand{5});
    simulation.runFor(SimDuration::minutes(1));
    GX_EXPECT_EQ(galaxy.activeConvoys(), 1u);
    GX_EXPECT_EQ(galaxy.stats().convoysRaided, 1u);
    GX_EXPECT_EQ(galaxy.stats().cargoLost, 10.0);

    simulation.runFor(SimDuration::hours(49)); // 48 h trip, plus margin for accumulated rounding
    GX_EXPECT_EQ(galaxy.activeConvoys(), 0u);
    GX_EXPECT_EQ(galaxy.stats().convoysArrived, 1u);
    GX_EXPECT_EQ(simulation.world().entityCount(), 0u);
}

GX_TEST(Commands, ReplayReproducesTheLiveRun) {
    const SimTime end = at(3 * 24 * 60);
    u64 liveHash = 0;
    std::vector<CommandRecord> log;
    {
        JobSystem jobs(3);
        SyntheticGalaxy galaxy(smallGalaxy());
        Simulation simulation({.seed = 77}, jobs);
        galaxy.install(simulation);
        simulation.commands().setRecording(true);
        // Commands arrive while the simulation runs, as player input would.
        for (u32 hour = 5; simulation.now() < end; hour += 7) {
            simulation.runUntil(std::min(end, at(hour * 60)));
            simulation.submitCommand(SpawnConvoyCommand{hour % 40, (hour + 1) % 40, hour % 4, 50.0, 30.0});
            if (hour % 3 == 0) {
                simulation.submitCommand(RaidConvoysCommand{(hour * 7) % 40},
                                         simulation.now() + SimDuration::hours(2));
            }
        }
        simulation.runUntil(end + SimDuration::hours(3)); // let the last commands apply
        liveHash = simulation.stateHash();
        log = simulation.commands().recorded();
        GX_EXPECT(galaxy.stats().convoysRaided > 0);
    }
    GX_REQUIRE(log.size() > 10);

    JobSystem jobs(0); // different thread count on purpose
    SyntheticGalaxy galaxy(smallGalaxy());
    Simulation replay({.seed = 77}, jobs);
    galaxy.install(replay);
    for (const CommandRecord& record : log) {
        replay.commands().submitRecord(record);
    }
    replay.runUntil(end + SimDuration::hours(3));
    GX_EXPECT_EQ(replay.stateHash(), liveHash);
}
