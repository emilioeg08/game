#include "Tests/TestFramework.h"

#include "Engine/Jobs/JobSystem.h"
#include "Scenarios/SyntheticGalaxy.h"
#include "Simulation/Kernel/Simulation.h"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

using namespace gx;

namespace {

SystemDesc counterSystem(std::string name, SimDuration period, u64& counter, SimDuration offset = {}) {
    return {std::move(name), TickPhase::Simulation, period, offset,
            [&counter](const TickContext&) { ++counter; }};
}

} // namespace

GX_TEST(Kernel, SystemsRunAtTheirOwnPeriods) {
    JobSystem jobs(0);
    Simulation simulation({.seed = 1}, jobs);
    u64 fast = 0;
    u64 slow = 0;
    u64 staggered = 0;
    simulation.addSystem(counterSystem("Fast", SimDuration::seconds(1), fast));
    simulation.addSystem(counterSystem("Slow", SimDuration::hours(1), slow));
    simulation.addSystem(
        counterSystem("Staggered", SimDuration::hours(1), staggered, SimDuration::minutes(30)));

    simulation.runUntil(SimTime::epoch() + SimDuration::hours(2));
    GX_EXPECT_EQ(fast, 7200u);
    GX_EXPECT_EQ(slow, 2u);
    GX_EXPECT_EQ(staggered, 2u); // 00:30 and 01:30
    GX_EXPECT_EQ(simulation.stepCount(), 7200u);
    GX_EXPECT(simulation.now() == SimTime::epoch() + SimDuration::hours(2));
}

GX_TEST(Kernel, IdleTimeIsSkipped) {
    // With only an hourly system, a simulated year costs 8760 steps, not 31.5 million one-second ticks.
    JobSystem jobs(0);
    Simulation simulation({}, jobs);
    u64 runs = 0;
    simulation.addSystem(counterSystem("Hourly", SimDuration::hours(1), runs));
    simulation.runFor(SimDuration::years(1));
    GX_EXPECT_EQ(runs, 8760u);
    GX_EXPECT_EQ(simulation.stepCount(), 8760u);
}

GX_TEST(Kernel, DtIsTimeSinceLastRun) {
    JobSystem jobs(0);
    Simulation simulation({}, jobs);
    std::vector<SimDuration> dts;
    simulation.addSystem({"Offset", TickPhase::Simulation, SimDuration::seconds(10), SimDuration::seconds(3),
                          [&](const TickContext& c) { dts.push_back(c.dt); }});
    simulation.runFor(SimDuration::seconds(25));
    GX_REQUIRE(dts.size() == 3); // at 3 s, 13 s, 23 s
    GX_EXPECT(dts[0] == SimDuration::seconds(3));
    GX_EXPECT(dts[1] == SimDuration::seconds(10));
    GX_EXPECT(dts[2] == SimDuration::seconds(10));
}

GX_TEST(Kernel, PhasesOrderSystemsWithinAStep) {
    JobSystem jobs(0);
    Simulation simulation({}, jobs);
    std::vector<std::string> order;
    const auto add = [&](const char* name, TickPhase phase) {
        simulation.addSystem({name, phase, SimDuration::seconds(1), {}, [&order, name](const TickContext&) {
                                  order.emplace_back(name);
                              }});
    };
    add("History", TickPhase::History);
    add("SimA", TickPhase::Simulation);
    add("Commands", TickPhase::Commands);
    add("Sync", TickPhase::Synchronization);
    add("SimB", TickPhase::Simulation);
    GX_REQUIRE(simulation.step());
    const std::vector<std::string> expected = {"Commands", "SimA", "SimB", "Sync", "History"};
    GX_EXPECT(order == expected);
}

GX_TEST(Kernel, ScratchArenaIsResetEveryStep) {
    JobSystem jobs(0);
    Simulation simulation({.scratchBytes = 4096}, jobs);
    bool dirtyAtStart = false;
    simulation.addSystem(
        {"Scratch", TickPhase::Simulation, SimDuration::seconds(1), {}, [&](const TickContext& c) {
             dirtyAtStart = dirtyAtStart || c.scratch.used() != 0;
             (void)c.scratch.allocate(1024);
         }});
    simulation.runFor(SimDuration::seconds(100));
    GX_EXPECT(!dirtyAtStart);
    GX_EXPECT_EQ(simulation.scratch().highWaterMark(), 1024u);
}

GX_TEST(Kernel, EmptySimulationOnlyMovesTheClock) {
    JobSystem jobs(0);
    Simulation simulation({}, jobs);
    GX_EXPECT(!simulation.step());
    GX_EXPECT_EQ(simulation.runFor(SimDuration::hours(1)), 0u);
    GX_EXPECT(simulation.now() == SimTime::epoch() + SimDuration::hours(1));
}

GX_TEST(Kernel, AddingSystemsDuringAStepIsRejected) {
    JobSystem jobs(0);
    Simulation simulation({}, jobs);
    test::ScopedAssertCapture capture;
    bool added = false;
    simulation.addSystem(
        {"Adder", TickPhase::Simulation, SimDuration::seconds(1), {}, [&](const TickContext&) {
             if (!added) {
                 added = true;
                 simulation.addSystem(
                     {"Late", TickPhase::Simulation, SimDuration::hours(1), {}, [](const TickContext&) {}});
             }
         }});
    simulation.step();
    GX_EXPECT_EQ(capture.count(), 1);
    GX_EXPECT_EQ(simulation.scheduler().size(), 1u);
}

GX_TEST(Kernel, ResultDoesNotDependOnHowTheRunIsSliced) {
    SyntheticGalaxyConfig config;
    config.seed = 42;
    config.starSystems = 50;
    config.bodiesPerSystem = 8;
    config.motionPeriod = SimDuration::minutes(5);
    const SimTime end = SimTime::epoch() + SimDuration::hours(12);

    const auto run = [&](auto&& drive) {
        JobSystem jobs(2);
        SyntheticGalaxy galaxy(config);
        Simulation simulation({.seed = config.seed}, jobs);
        galaxy.registerSystems(simulation);
        drive(simulation);
        GX_EXPECT(simulation.now() == end);
        return std::pair{galaxy.stateHash(), simulation.stepCount()};
    };

    const auto oneShot = run([&](Simulation& s) { s.runUntil(end); });
    const auto sliced = run([&](Simulation& s) {
        while (s.now() < end) {
            s.runUntil(std::min(end, s.now() + SimDuration::minutes(7)));
        }
    });
    const auto budgeted = run([&](Simulation& s) {
        while (s.now() < end) {
            s.runUntil(end, 1); // 1 ns budget: returns after every step
        }
    });
    GX_EXPECT_EQ(sliced.first, oneShot.first);
    GX_EXPECT_EQ(budgeted.first, oneShot.first);
    GX_EXPECT_EQ(sliced.second, oneShot.second);
    GX_EXPECT_EQ(budgeted.second, oneShot.second);
}
