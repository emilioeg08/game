#include "Tests/TestFramework.h"

#include "Engine/Jobs/JobSystem.h"
#include "Engine/Text/Localization.h"
#include "Game/Sandbox/Content.h"
#include "Game/Sandbox/Sandbox.h"
#include "Simulation/Economy/Economy.h"
#include "Simulation/Kernel/Simulation.h"
#include "Space/Destruction/Debris.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace gx;

namespace {

ShipModule part(ModuleType type, f64 health, f64 left) {
    return {type, 0, health, left, 0.0};
}

// A hauler-like design; `wrecked` modules are at zero health.
ShipModules hull(bool wreckSome) {
    ShipModules modules;
    modules.modules = {part(ModuleType::Structure, 600.0, 0.0),
                       part(ModuleType::Reactor, 150.0, 150.0),
                       part(ModuleType::Drive, 200.0, wreckSome ? 0.0 : 200.0),
                       part(ModuleType::HyperDrive, 150.0, 150.0),
                       part(ModuleType::Sensors, 80.0, 40.0),
                       part(ModuleType::Cargo, 600.0, wreckSome ? 0.0 : 600.0),
                       part(ModuleType::Quarters, 150.0, 150.0)};
    return modules;
}

f64 totalScrap(const std::vector<FragmentDesc>& pieces) {
    f64 total = 0.0;
    for (const FragmentDesc& piece : pieces) {
        total += piece.scrap;
    }
    return total;
}

SandboxConfig quiet() {
    SandboxConfig config;
    config.pirates = 1;
    config.maxPatrols = 0;
    return config;
}

struct Session {
    explicit Session(u32 workers = 0, const SandboxConfig& config = quiet())
        : jobs(workers), sandbox(config), simulation({.seed = config.seed}, jobs) {
        sandbox.install(simulation);
        sandbox.populate(simulation);
    }
    JobSystem jobs;
    Sandbox sandbox;
    Simulation simulation;

    [[nodiscard]] World& world() { return simulation.world(); }
    [[nodiscard]] bool journalContains(const std::string& text) const {
        for (const JournalEntry& entry : sandbox.journal()) {
            if (render(entry.text, nullptr).find(text) != std::string::npos) {
                return true;
            }
        }
        return false;
    }
    // The raider, without power and carrying 20 t of fuel, 20,000 km from the player's ship, shot to pieces
    // by it. Returns once it is gone.
    void killThePirate() {
        World& w = world();
        const EntityId pirate = w.components<PirateBrain>().entities()[0];
        const Kinematics& player = w.components<Kinematics>().get(sandbox.playerShip());
        w.components<Kinematics>().get(pirate) = {
            player.position + Vec3d{2e7, 0.0, 0.0}, player.velocity, {}};
        for (ShipModule& module : w.components<ShipModules>().get(pirate).modules) {
            module.health = module.type == ModuleType::Reactor ? 0.0 : module.health;
        }
        applyModuleEffects(w, pirate);
        w.components<CargoHold>().get(pirate).add(content::kGoodFuel, 20);
        simulation.runFor(SimDuration::seconds(2)); // the sensors pick it up
        u32 track = 0;
        for (const SensorContact& contact : sandbox.sensors().picture(content::kFactionPlayer).contacts) {
            track = contact.target == pirate ? contact.trackId : track;
        }
        GX_REQUIRE(track != 0);
        simulation.submitCommand(EngageCommand{sandbox.playerShip(), track, true, true});
        for (int second = 0; second < 900 && w.isAlive(pirate); ++second) {
            simulation.runFor(SimDuration::seconds(1));
        }
        GX_REQUIRE(!w.isAlive(pirate));
        simulation.submitCommand(EngageCommand{sandbox.playerShip(), 0, false, false});
    }
    [[nodiscard]] EntityId hulk() {
        const ComponentStore<Wreck>& wrecks = world().components<Wreck>();
        for (usize i = 0; i < wrecks.size(); ++i) {
            if (wrecks.values()[i].hulk) {
                return wrecks.entities()[i];
            }
        }
        return {};
    }
};

} // namespace

GX_TEST(Destruction, AShipBreaksUpAlongItsModules) {
    Rng rng(42);
    const std::vector<FragmentDesc> pieces = breakUp(hull(false), {1'000.0, 0.0, 0.0}, false, rng);
    GX_REQUIRE(pieces.size() >= 2u);
    GX_EXPECT(pieces.front().hulk);
    // Scrap: every intact module at kScrapPerHealth, and half the frame's worth.
    const f64 expected =
        (150.0 + 200.0 + 150.0 + 80.0 + 600.0 + 150.0) * kScrapPerHealth + 600.0 * kScrapPerHealth * 0.5;
    GX_EXPECT_NEAR(totalScrap(pieces), expected, 1e-9);
    f64 shares = 0.0;
    usize modules = 0;
    for (usize i = 0; i < pieces.size(); ++i) {
        shares += pieces[i].share;
        modules += pieces[i].modules.size();
        if (i > 0) {
            const f64 kick = length(pieces[i].velocity - Vec3d{1'000.0, 0.0, 0.0});
            GX_EXPECT(kick >= 50.0 - 1e-9 && kick <= 400.0 + 1e-9);
        }
    }
    GX_EXPECT_NEAR(shares, 1.0, 1e-12);
    GX_EXPECT_EQ(modules, 7u); // the structure and the six intact modules, each in one piece
    // Wrecked modules are vaporised; a reactor breach leaves half the scrap.
    Rng again(42);
    const f64 damaged = totalScrap(breakUp(hull(true), {}, false, again));
    GX_EXPECT_NEAR(damaged, expected - (200.0 + 600.0) * kScrapPerHealth, 1e-9);
    Rng breach(42);
    GX_EXPECT_NEAR(totalScrap(breakUp(hull(false), {}, true, breach)), expected * 0.5, 1e-9);
    // Deterministic.
    Rng a(7);
    Rng b(7);
    const auto first = breakUp(hull(false), {}, false, a);
    const auto second = breakUp(hull(false), {}, false, b);
    GX_REQUIRE(first.size() == second.size());
    for (usize i = 0; i < first.size(); ++i) {
        GX_EXPECT(first[i].velocity == second[i].velocity);
    }
}

GX_TEST(Destruction, DebrisHurtsOnlyWhoCrossesItFast) {
    GX_EXPECT_NEAR(chordThroughSphere({-10.0, 0.0, 0.0}, {10.0, 0.0, 0.0}, {}, 5.0), 10.0, 1e-9);
    GX_EXPECT_NEAR(chordThroughSphere({-10.0, 3.0, 0.0}, {10.0, 3.0, 0.0}, {}, 5.0), 8.0, 1e-9);
    GX_EXPECT_EQ(chordThroughSphere({-10.0, 6.0, 0.0}, {10.0, 6.0, 0.0}, {}, 5.0), 0.0);
    GX_EXPECT_NEAR(chordThroughSphere({0.0, 0.0, 0.0}, {10.0, 0.0, 0.0}, {}, 5.0), 5.0,
                   1e-9); // starts inside

    const DebrisCloud cloud{{}, {}, SimTime::epoch(), 1.0};
    const SimTime fresh = SimTime::epoch();
    // Across the fresh cloud (4 km) at 20 km/s: 0.1 x 4 = 0.4 impacts on average.
    const Vec3d from{-1e5, 0.0, 0.0};
    const Vec3d to{1e5, 0.0, 0.0};
    Rng rng(3);
    u32 impacts = 0;
    constexpr int kTrials = 20'000;
    for (int i = 0; i < kTrials; ++i) {
        impacts += static_cast<u32>(debrisImpacts(cloud, fresh, from, to, {20'000.0, 0.0, 0.0}, rng).size());
    }
    GX_EXPECT_NEAR(static_cast<f64>(impacts) / kTrials, 0.4, 0.02);
    // At a salvager's speed, nothing; and the cloud thins out and is gone after its lifetime.
    GX_EXPECT(debrisImpacts(cloud, fresh, from, to, {500.0, 0.0, 0.0}, rng).empty());
    const SimTime later = fresh + SimDuration::minutes(10);
    GX_EXPECT(debrisDensity(cloud, later) < 0.1 * debrisDensity(cloud, fresh));
    GX_EXPECT(debrisRadius(cloud, later) > debrisRadius(cloud, fresh));
    GX_EXPECT(debrisExpired(cloud, fresh + SimDuration::minutes(31)));
    // Poisson draws have the right mean.
    Rng poissonRng(11);
    u64 sum = 0;
    for (int i = 0; i < kTrials; ++i) {
        sum += poisson(2.5, poissonRng);
    }
    GX_EXPECT_NEAR(static_cast<f64>(sum) / kTrials, 2.5, 0.05);
}

GX_TEST(Destruction, ADestroyedShipLeavesWrecksThePlayerCanSalvage) {
    Session session;
    World& world = session.world();
    session.killThePirate();
    const EntityId hulk = session.hulk();
    GX_REQUIRE(hulk.isValid());
    const Wreck& wreck = world.components<Wreck>().get(hulk);
    GX_EXPECT(wreck.knownTo(content::kFactionPlayer)); // it watched it die
    GX_EXPECT(session.sandbox.stats().wrecksFormed >= 1u);
    GX_EXPECT(session.sandbox.stats().scrapCreated > 0u);
    GX_EXPECT(!session.sandbox.debris().empty());
    u32 fuel = 0;
    for (const EntityId piece : world.components<Wreck>().entities()) {
        fuel += world.components<CargoHold>().get(piece).amount(content::kGoodFuel);
    }
    GX_EXPECT_EQ(fuel, 10u); // half the cargo survives
    GX_EXPECT(session.journalContains("a la deriva"));

    // Too far: refused. Next to it, matching its drift: the hold fills.
    const u64 rejected = session.sandbox.stats().commandsRejected;
    session.simulation.submitCommand(SalvageCommand{session.sandbox.playerShip(), hulk});
    session.simulation.runFor(SimDuration::seconds(1));
    GX_EXPECT_EQ(session.sandbox.stats().commandsRejected, rejected + 1);
    const Kinematics wreckState = world.components<Kinematics>().get(hulk);
    world.components<Kinematics>().get(session.sandbox.playerShip()) = {
        wreckState.position + Vec3d{1'000.0, 0.0, 0.0}, wreckState.velocity, {}};
    ShipControl& control = world.components<ShipControl>().get(session.sandbox.playerShip());
    control = {};
    control.mode = FlightMode::Coast;
    session.simulation.submitCommand(SalvageCommand{session.sandbox.playerShip(), hulk});
    session.simulation.runFor(SimDuration::seconds(1));
    const CargoHold& hold = world.components<CargoHold>().get(session.sandbox.playerShip());
    GX_EXPECT(hold.amount(content::kGoodMetals) > 0u);
    GX_EXPECT(session.sandbox.stats().tonnesSalvaged > 0u);
    GX_EXPECT(session.journalContains("Recuperas de los restos"));
}

GX_TEST(Destruction, WrecksDriftAwayAndWhatTheyHoldIsLost) {
    Session session;
    session.killThePirate();
    const std::vector<EntityId> pieces(session.world().components<Wreck>().entities().begin(),
                                       session.world().components<Wreck>().entities().end());
    GX_REQUIRE(!pieces.empty());
    const u64 lostBefore = session.sandbox.stats().cargoLost[content::kGoodMetals];
    session.simulation.runFor(content::kWreckLifetime + SimDuration::minutes(1));
    for (const EntityId piece : pieces) {
        GX_EXPECT(!session.world().isAlive(piece)); // other wrecks may have formed since
    }
    GX_EXPECT(session.sandbox.stats().cargoLost[content::kGoodMetals] > lostBefore);
}

GX_TEST(Destruction, CompanySalvagersCollectAndSell) {
    Session session;
    session.sandbox.account().credits += 20'000;
    session.killThePirate();
    // Back to the station, then a Carguero with a salvage order.
    World& world = session.world();
    const EntityId player = session.sandbox.playerShip();
    session.simulation.submitCommand(
        PilotCommand{player, FlightMode::Approach, session.sandbox.homePort(), {}, {}});
    session.simulation.runFor(SimDuration::minutes(2));
    session.simulation.submitCommand(BuyShipCommand{player, content::kShipClassHauler,
                                                    content::kShipPrices[content::kShipClassHauler], true});
    session.simulation.runFor(SimDuration::seconds(1));
    GX_REQUIRE(world.components<FleetBrain>().size() == 1u);
    const EntityId salvager = world.components<FleetBrain>().entities()[0];
    session.simulation.submitCommand(FleetOrderCommand{salvager, FleetOrder::Salvage, {}, {}});
    session.simulation.runFor(SimDuration::hours(1));
    GX_EXPECT(session.sandbox.company().totals.tonnesSalvaged > 0u);
    GX_EXPECT(session.sandbox.company().totals.sales > 0);
    GX_EXPECT(session.journalContains("recupera de los restos"));
}

GX_TEST(Destruction, DebrisStrikesAShipRushingThroughIt) {
    Session session;
    World& world = session.world();
    session.killThePirate();
    GX_REQUIRE(!session.sandbox.debris().empty());
    const DebrisCloud cloud = session.sandbox.debris().back();
    // The player's ship dives through the fresh cloud at 20 km/s, back and forth, for two minutes (each
    // second it crosses from 10 km on one side to 10 km on the other).
    const EntityId player = session.sandbox.playerShip();
    const u64 hitsBefore = session.sandbox.stats().debrisHits;
    for (int pass = 0; pass < 120; ++pass) {
        const Vec3d center = cloud.centerAt(session.simulation.now());
        const Vec3d relative{pass % 2 == 0 ? 20'000.0 : -20'000.0, 0.0, 0.0};
        world.components<Kinematics>().get(player) = {center - relative * 0.5, cloud.velocity + relative, {}};
        ShipControl& control = world.components<ShipControl>().get(player);
        control = {};
        control.mode = FlightMode::Coast;
        session.simulation.runFor(SimDuration::seconds(1));
    }
    GX_EXPECT(session.sandbox.stats().debrisHits > hitsBefore);
    GX_EXPECT(session.journalContains("Impacto de escombros"));
}

GX_TEST(Destruction, WrecksSaveAndLoad) {
    Session saver;
    saver.killThePirate();
    saver.simulation.runFor(SimDuration::minutes(5));
    const std::vector<std::byte> saved = saver.simulation.saveState();
    JobSystem jobs(0);
    Sandbox sandbox(quiet());
    Simulation loaded({.seed = quiet().seed}, jobs);
    sandbox.install(loaded);
    std::string error;
    GX_REQUIRE(loaded.loadState(saved, error));
    GX_EXPECT(loaded.saveState() == saved);
    GX_EXPECT_EQ(sandbox.debris().size(), saver.sandbox.debris().size());
    saver.simulation.runFor(SimDuration::minutes(20));
    loaded.runFor(SimDuration::minutes(20));
    GX_EXPECT_EQ(loaded.stateHash(), saver.simulation.stateHash());
}
