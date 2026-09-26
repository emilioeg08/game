#include "Tests/TestFramework.h"

#include "Engine/Jobs/JobSystem.h"
#include "Simulation/Kernel/Simulation.h"

#include <algorithm>
#include <string>
#include <vector>

using namespace gx;

namespace {

struct Ping {
    u32 value = 0;
};
struct Pong {
    u32 value = 0;
};

} // namespace

GX_TEST(Events, SubscribersRunInOrderAndCascadesSettle) {
    JobSystem jobs(0);
    Simulation simulation({}, jobs);
    std::vector<std::string> log;
    EventChannel<Ping>& pings = simulation.events().registerEvent<Ping>("Ping");
    simulation.events().registerEvent<Pong>("Pong").subscribe(
        [&](const Pong& e, const TickContext&) { log.push_back("pong " + std::to_string(e.value)); });
    pings.subscribe([&](const Ping& e, const TickContext& c) {
        log.push_back("ping " + std::to_string(e.value));
        c.events.channel<Pong>().emit({e.value * 10}); // cascade into another channel
    });
    pings.subscribe(
        [&](const Ping& e, const TickContext&) { log.push_back("second " + std::to_string(e.value)); });
    simulation.addSystem(
        {"Emitter", TickPhase::Simulation, SimDuration::seconds(1), {}, [&](const TickContext& c) {
             c.events.channel<Ping>().emit({1});
             c.events.channel<Ping>().emit({2});
         }});
    simulation.step();
    const std::vector<std::string> expected = {"ping 1",   "second 1", "ping 2",
                                               "second 2", "pong 10",  "pong 20"};
    GX_EXPECT(log == expected);
    GX_EXPECT_EQ(simulation.stats().eventsEmitted, 4u);
}

GX_TEST(Events, EventsLiveForOneStep) {
    JobSystem jobs(0);
    Simulation simulation({}, jobs);
    simulation.events().registerEvent<Ping>("Ping");
    std::vector<usize> seenAtStart;
    std::vector<usize> seenInHistory;
    simulation.addSystem(
        {"Start", TickPhase::Commands, SimDuration::seconds(1), {}, [&](const TickContext& c) {
             seenAtStart.push_back(c.events.channel<Ping>().size());
         }});
    simulation.addSystem(
        {"Emit", TickPhase::Simulation, SimDuration::seconds(1), {}, [&](const TickContext& c) {
             c.events.channel<Ping>().emit({7});
         }});
    simulation.addSystem(
        {"History", TickPhase::History, SimDuration::seconds(1), {}, [&](const TickContext& c) {
             seenInHistory.push_back(c.events.channel<Ping>().size());
         }});
    simulation.runFor(SimDuration::seconds(3));
    GX_EXPECT(seenAtStart == (std::vector<usize>{0, 0, 0}));   // nothing leaks into the next step
    GX_EXPECT(seenInHistory == (std::vector<usize>{1, 1, 1})); // later phases can read the step's events
}

GX_TEST(Events, ParallelEmissionOrderIsIndependentOfThreadCount) {
    const auto emitAll = [](u32 workers) {
        JobSystem jobs(workers);
        Simulation simulation({}, jobs);
        EventChannel<Ping>& channel = simulation.events().registerEvent<Ping>("Ping");
        std::vector<u32> result;
        simulation.addSystem(
            {"Emit", TickPhase::Simulation, SimDuration::seconds(1), {}, [&](const TickContext& c) {
                 constexpr u32 kCount = 10'000;
                 constexpr u32 kGrain = 37;
                 channel.beginParallel(JobSystem::chunkCount(kCount, kGrain));
                 c.jobs.parallelFor(kCount, kGrain, [&](u32 begin, u32 end) {
                     for (u32 i = begin; i < end; ++i) {
                         if (i % 7 == 0) {
                             channel.emitFromChunk(begin / kGrain, {i});
                         }
                     }
                 });
                 channel.endParallel();
             }});
        simulation.addSystem(
            {"Collect", TickPhase::History, SimDuration::seconds(1), {}, [&](const TickContext&) {
                 for (const Ping& p : channel.events()) {
                     result.push_back(p.value);
                 }
             }});
        simulation.step();
        return result;
    };
    const std::vector<u32> serial = emitAll(0);
    GX_REQUIRE(serial.size() == 1429);
    GX_EXPECT(std::is_sorted(serial.begin(), serial.end()));
    for (const u32 workers : {1u, 3u, 7u}) {
        GX_EXPECT(emitAll(workers) == serial);
    }
}

GX_TEST(Events, RunawayCascadeIsChecked) {
    JobSystem jobs(0);
    Simulation simulation({}, jobs);
    simulation.events().registerEvent<Ping>("Ping").subscribe(
        [](const Ping& e, const TickContext& c) { c.events.channel<Ping>().emit({e.value + 1}); });
    simulation.addSystem(
        {"Emit", TickPhase::Simulation, SimDuration::seconds(1), {}, [](const TickContext& c) {
             c.events.channel<Ping>().emit({0});
         }});
    test::ScopedAssertCapture capture;
    simulation.step();
    GX_EXPECT_EQ(capture.count(), 1);
}
