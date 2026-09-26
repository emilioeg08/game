#include "Tests/TestFramework.h"

#include "Engine/Jobs/JobSystem.h"
#include "Scenarios/SyntheticGalaxy.h"
#include "Simulation/Kernel/Simulation.h"

using namespace gx;

namespace {

SyntheticGalaxyConfig testConfig(u64 seed) {
    SyntheticGalaxyConfig config;
    config.seed = seed;
    config.starSystems = 300;
    config.bodiesPerSystem = 12;
    config.goodsPerSystem = 6;
    config.motionPeriod = SimDuration::minutes(5);
    config.economyPeriod = SimDuration::hours(1);
    // Small grains: many chunks, so the work really is spread over every thread.
    config.motionGrain = 256;
    config.systemGrain = 16;
    return config;
}

u64 simulate(u64 seed, u32 workers, SimDuration duration) {
    JobSystem jobs(workers);
    SyntheticGalaxy galaxy(testConfig(seed));
    Simulation simulation({.seed = seed}, jobs);
    galaxy.registerSystems(simulation);
    simulation.runFor(duration);
    return galaxy.stateHash();
}

} // namespace

GX_TEST(Determinism, GenerationIsReproducible) {
    const SyntheticGalaxy a(testConfig(5));
    const SyntheticGalaxy b(testConfig(5));
    const SyntheticGalaxy c(testConfig(6));
    GX_EXPECT_EQ(a.stateHash(), b.stateHash());
    GX_EXPECT(a.stateHash() != c.stateHash());
}

GX_TEST(Determinism, SameInputsSameResult) {
    GX_EXPECT_EQ(simulate(11, 3, SimDuration::days(2)), simulate(11, 3, SimDuration::days(2)));
}

GX_TEST(Determinism, ResultIsIndependentOfThreadCount) {
    const u64 reference = simulate(21, 0, SimDuration::days(2));
    for (const u32 workers : {1u, 3u, 7u, 11u}) {
        GX_EXPECT_EQ(simulate(21, workers, SimDuration::days(2)), reference);
    }
}

GX_TEST(Determinism, DifferentSeedsDiverge) {
    GX_EXPECT(simulate(1, 2, SimDuration::days(1)) != simulate(2, 2, SimDuration::days(1)));
}

GX_TEST(Determinism, StateEvolves) {
    const SyntheticGalaxy initial(testConfig(9));
    GX_EXPECT(simulate(9, 2, SimDuration::hours(3)) != initial.stateHash());
}
