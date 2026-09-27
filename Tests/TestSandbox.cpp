#include "Tests/TestFramework.h"

#include "Engine/Jobs/JobSystem.h"
#include "Game/Presentation/SystemSnapshot.h"
#include "Game/Sandbox/Content.h"
#include "Game/Sandbox/Sandbox.h"
#include "Simulation/Kernel/Simulation.h"
#include "Space/Bodies/CelestialBody.h"

#include <cmath>
#include <limits>
#include <string>

using namespace gx;

namespace {

// No pirates: for tests about flying, not fighting.
SandboxConfig peaceful() {
    SandboxConfig config;
    config.pirates = 0;
    return config;
}

// Same setup as the graphical client, without any window.
struct Session {
    explicit Session(u32 workers, const SandboxConfig& config = {})
        : jobs(workers), sandbox(config), simulation({.seed = config.seed}, jobs) {
        sandbox.install(simulation);
    }
    JobSystem jobs;
    Sandbox sandbox;
    Simulation simulation;

    void pilot(FlightMode mode, EntityId target = {}, Vec3d point = {}, Vec3d thrust = {}) {
        simulation.submitCommand(PilotCommand{sandbox.playerShip(), mode, target, point, thrust});
    }
    [[nodiscard]] const ShipControl& playerControl() {
        return simulation.world().components<ShipControl>().get(sandbox.playerShip());
    }
    [[nodiscard]] bool journalContains(const std::string& text) const {
        for (const JournalEntry& entry : sandbox.journal()) {
            if (entry.text.find(text) != std::string::npos) {
                return true;
            }
        }
        return false;
    }
    // The nearest port that is not next to the starting station (a trip of hours, not days).
    [[nodiscard]] EntityId destinationPort() {
        const World& world = simulation.world();
        const Vec3d player = world.components<Kinematics>().get(sandbox.playerShip()).position;
        EntityId best;
        f64 bestDistance = std::numeric_limits<f64>::max();
        for (const EntityId port : sandbox.ports()) {
            const f64 d = length(bodyStateAt(world, port, simulation.now()).position - player);
            if (d > 1e9 && d < bestDistance) {
                bestDistance = d;
                best = port;
            }
        }
        return best;
    }
};

} // namespace

GX_TEST(Sandbox, NewGameHasPlayerHaulersPiratesAndPorts) {
    Session session(0);
    session.sandbox.populate(session.simulation);
    const World& world = session.simulation.world();
    GX_REQUIRE(world.isAlive(session.sandbox.playerShip()));
    GX_EXPECT_EQ(world.components<ShipIdentity>().get(session.sandbox.playerShip()).faction,
                 static_cast<u32>(content::kFactionPlayer));
    GX_EXPECT_EQ(world.components<HaulerBrain>().size(),
                 static_cast<usize>(session.sandbox.config().haulers));
    GX_EXPECT_EQ(world.components<PirateBrain>().size(),
                 static_cast<usize>(session.sandbox.config().pirates));
    for (const EntityId pirate : world.components<PirateBrain>().entities()) {
        GX_EXPECT(!world.components<SensorSuite>().get(pirate).transponderOn); // raiders run silent
    }
    GX_EXPECT(world.components<CombatControl>().contains(session.sandbox.playerShip())); // armed
    GX_EXPECT(session.sandbox.ports().size() >= 6); // 4-9 planets + 2 stations
    GX_EXPECT(!session.sandbox.systemName().empty());
    GX_EXPECT_EQ(session.sandbox.journal().size(), 1u);
    GX_EXPECT(session.playerControl().arrived); // starts docked at the main station
}

GX_TEST(Sandbox, HaulersTravelWithoutThePlayer) {
    Session session(3);
    session.sandbox.populate(session.simulation);
    session.simulation.runFor(SimDuration::minutes(30));
    GX_EXPECT(session.sandbox.stats().haulerDepartures >= session.sandbox.config().haulers);
    GX_EXPECT(session.sandbox.stats().arrivals > 0);
    GX_EXPECT(session.journalContains("atraca en"));
}

GX_TEST(Sandbox, PlayerFliesToAPortByCommand) {
    Session session(3, peaceful());
    session.sandbox.populate(session.simulation);
    const EntityId destination = session.destinationPort();
    session.pilot(FlightMode::Approach, destination);
    session.simulation.runFor(SimDuration::minutes(1));
    GX_EXPECT(session.playerControl().target == destination);
    GX_EXPECT(!session.playerControl().arrived);

    for (int slice = 0; slice < 12 && !session.playerControl().arrived; ++slice) {
        session.simulation.runFor(SimDuration::minutes(5)); // real-time scale: minutes per trip
    }
    GX_EXPECT(session.playerControl().arrived);
    GX_EXPECT(session.journalContains("ha llegado a"));
}

GX_TEST(Sandbox, PilotCommandsAreValidated) {
    Session session(0);
    session.sandbox.populate(session.simulation);
    const World& world = session.simulation.world();
    const EntityId hauler = world.components<HaulerBrain>().entities()[0];
    const f64 nan = std::numeric_limits<f64>::quiet_NaN();

    session.simulation.submitCommand(PilotCommand{hauler, FlightMode::Stop, {}, {}, {}}); // not the player's
    session.pilot(FlightMode::Approach, session.sandbox.playerShip());                    // itself
    session.pilot(FlightMode::Approach, EntityId{9999, 0});                               // nonexistent
    session.pilot(FlightMode::MoveTo, {}, Vec3d{nan, 0.0, 0.0});                          // not finite
    session.pilot(FlightMode::Count);                                                     // invalid mode
    session.simulation.runFor(SimDuration::minutes(1));
    GX_EXPECT_EQ(session.sandbox.stats().commandsRejected, 5u);
    GX_EXPECT(session.playerControl().mode == FlightMode::Approach); // unchanged: still docked
}

GX_TEST(Sandbox, ManualPilotingSwitchesToTacticalFlightRate) {
    Session session(0);
    session.sandbox.populate(session.simulation);
    const auto flightPeriod = [&] {
        return session.simulation.scheduler().system(session.sandbox.flightSystem()).desc.period;
    };
    GX_EXPECT(flightPeriod() == session.sandbox.config().strategicFlightPeriod);
    session.pilot(FlightMode::Manual, {}, {}, Vec3d{0.0, 1.0, 0.0});
    session.simulation.runFor(SimDuration::seconds(2));
    GX_EXPECT(flightPeriod() == session.sandbox.config().tacticalFlightPeriod);
    const Kinematics& state =
        session.simulation.world().components<Kinematics>().get(session.sandbox.playerShip());
    GX_EXPECT(length(state.acceleration) > 0.0);
    session.pilot(FlightMode::Stop);
    session.simulation.runFor(SimDuration::seconds(2));
    GX_EXPECT(flightPeriod() == session.sandbox.config().strategicFlightPeriod);
}

GX_TEST(Sandbox, SessionIsDeterministicAcrossThreadCounts) {
    const auto play = [](u32 workers) {
        Session session(workers);
        session.sandbox.populate(session.simulation);
        const EntityId destination = session.destinationPort();
        session.simulation.runFor(SimDuration::minutes(3));
        session.pilot(FlightMode::Manual, {}, {}, Vec3d{1.0, 0.0, 0.0});
        session.simulation.runFor(SimDuration::seconds(30));
        session.pilot(FlightMode::Approach, destination);
        session.simulation.runFor(SimDuration::minutes(20));
        return session.simulation.stateHash();
    };
    const u64 reference = play(0);
    GX_EXPECT_EQ(play(3), reference);
    GX_EXPECT_EQ(play(11), reference);
}

GX_TEST(Sandbox, SaveLoadContinuesIdentically) {
    const auto opening = [](Session& session) {
        session.sandbox.populate(session.simulation);
        session.simulation.runFor(SimDuration::minutes(10));
        session.pilot(FlightMode::Approach, session.destinationPort()); // in flight when saved
        session.simulation.runFor(SimDuration::seconds(30));
    };
    Session continuous(3);
    opening(continuous);
    continuous.simulation.runFor(SimDuration::minutes(20));

    Session saver(3);
    opening(saver);
    const std::vector<std::byte> saved = saver.simulation.saveState();

    Session loaded(0); // no populate: everything comes from the save
    std::string error;
    GX_REQUIRE(loaded.simulation.loadState(saved, error));
    GX_EXPECT(loaded.simulation.saveState() == saved);
    GX_EXPECT_EQ(loaded.sandbox.systemName(), saver.sandbox.systemName());
    GX_EXPECT(loaded.sandbox.ports() == saver.sandbox.ports());
    loaded.simulation.runFor(SimDuration::minutes(20));
    GX_EXPECT_EQ(loaded.simulation.stateHash(), continuous.simulation.stateHash());
}

GX_TEST(Sandbox, SnapshotDescribesTheWorld) {
    Session session(0);
    session.sandbox.populate(session.simulation);
    session.simulation.runFor(SimDuration::minutes(30) + SimDuration::milliseconds(500));
    SnapshotBuilder builder;
    SystemSnapshot snapshot;
    builder.build(session.simulation, session.sandbox, snapshot);
    GX_EXPECT_EQ(snapshot.ships.size(), session.simulation.world().components<ShipIdentity>().size());
    GX_EXPECT(snapshot.playerAlive);
    GX_EXPECT_EQ(snapshot.playerModules.size(), content::kCourierModules.size());
    GX_EXPECT_EQ(snapshot.bodies.size(), session.simulation.world().components<CelestialBody>().size());
    const ShipView* player = snapshot.findShip(session.sandbox.playerShip());
    GX_REQUIRE(player != nullptr);
    GX_EXPECT(player->isPlayer);
    for (const BodyView& body : snapshot.bodies) {
        GX_EXPECT(body.kind == BodyKind::Star ||
                  (body.orbitPath != nullptr && body.orbitPath->size() == SnapshotBuilder::kOrbitPathPoints));
    }
    // Ships are extrapolated from their last flight step to "now".
    const Kinematics& state =
        session.simulation.world().components<Kinematics>().get(session.sandbox.playerShip());
    const f64 sinceFlight = (session.simulation.now() -
                             session.simulation.scheduler().system(session.sandbox.flightSystem()).lastRun)
                                .toSeconds();
    GX_EXPECT(sinceFlight > 0.0);
    GX_EXPECT_NEAR(length(player->position - (state.position + state.velocity * sinceFlight)), 0.0, 1e-3);
}

GX_TEST(Sandbox, PiratesHuntHaulersAndLossesAreReplaced) {
    SandboxConfig config;
    config.pirates = 4;
    Session session(3, config);
    session.sandbox.populate(session.simulation);
    session.simulation.runFor(SimDuration::minutes(60));
    const SandboxStats& stats = session.sandbox.stats();
    GX_EXPECT(stats.hunts > 0);
    GX_EXPECT(session.sandbox.combat().stats().hits > 0);
    GX_EXPECT(stats.haulersLost + stats.piratesLost + stats.piratesLeft + stats.playerDeaths > 0);
    GX_EXPECT(stats.spawns > 0); // losses are replaced
    const World& world = session.simulation.world();
    GX_EXPECT(world.components<HaulerBrain>().size() <= config.haulers);
    GX_EXPECT(world.components<HaulerBrain>().size() + 3 >= config.haulers); // one newcomer a minute
}

GX_TEST(Sandbox, EngageCommandsAreValidated) {
    Session session(0, peaceful());
    session.sandbox.populate(session.simulation);
    session.simulation.runFor(SimDuration::seconds(3)); // a few scans: nearby haulers become contacts
    const FactionPicture& picture = session.sandbox.sensors().picture(content::kFactionPlayer);
    GX_REQUIRE(!picture.contacts.empty());
    const u32 track = picture.contacts.front().trackId;
    const EntityId player = session.sandbox.playerShip();
    const World& world = session.simulation.world();

    session.simulation.submitCommand(EngageCommand{player, 99'999, true, true}); // unknown track
    session.simulation.submitCommand(EngageCommand{player, 0, true, false});     // fire at nothing
    const EntityId hauler = world.components<HaulerBrain>().entities()[0];
    session.simulation.submitCommand(EngageCommand{hauler, track, true, true}); // not the player's
    session.simulation.runFor(SimDuration::seconds(1));
    GX_EXPECT_EQ(session.sandbox.stats().commandsRejected, 3u);
    GX_EXPECT_EQ(world.components<CombatControl>().get(player).targetTrack, 0u);

    session.simulation.submitCommand(EngageCommand{player, track, true, true});
    session.simulation.runFor(SimDuration::seconds(1));
    GX_EXPECT_EQ(world.components<CombatControl>().get(player).targetTrack, track);
    GX_EXPECT(session.playerControl().mode == FlightMode::Pursue);
    GX_EXPECT_EQ(session.playerControl().track, track);
    GX_EXPECT(session.sandbox.tactical()); // fighting: fine flight and combat steps
    GX_EXPECT(session.journalContains("Fuego sobre"));

    session.simulation.submitCommand(EngageCommand{player, 0, false, false});
    session.simulation.runFor(SimDuration::seconds(1));
    GX_EXPECT_EQ(world.components<CombatControl>().get(player).targetTrack, 0u);
    GX_EXPECT(session.playerControl().mode == FlightMode::Pursue); // cease fire does not change course
    GX_EXPECT(session.journalContains("Alto el fuego"));
}

GX_TEST(Sandbox, StationsRepairAndDamageControlRestoresPower) {
    Session session(0, peaceful());
    session.sandbox.populate(session.simulation);
    World& world = session.simulation.world();
    const EntityId player = session.sandbox.playerShip();
    const auto modules = [&]() -> ShipModules& { return world.components<ShipModules>().get(player); };

    // Docked at the home station: everything is patched up at 2% per second.
    for (ShipModule& module : modules().modules) {
        module.health = module.maxHealth * 0.3;
    }
    session.simulation.runFor(SimDuration::seconds(40));
    for (const ShipModule& module : modules().modules) {
        GX_EXPECT_NEAR(module.fraction(), 1.0, 1e-9);
    }

    // Adrift with a wrecked reactor: no power until damage control gets it back to 10% (10 s + 40 s).
    session.pilot(FlightMode::Stop);
    session.simulation.runFor(SimDuration::seconds(1));
    ShipModule& reactor = modules().modules[1];
    GX_REQUIRE(reactor.type == ModuleType::Reactor);
    reactor.health = 0.0;
    modules().lastDamaged = session.simulation.now();
    applyModuleEffects(world, player);
    GX_EXPECT(!hasPower(modules()));
    GX_EXPECT_EQ(world.components<ShipDrive>().get(player).maxAcceleration, 0.0);
    session.simulation.runFor(SimDuration::seconds(30));
    GX_EXPECT(!hasPower(modules()));
    session.simulation.runFor(SimDuration::seconds(30));
    GX_EXPECT(hasPower(modules()));
    GX_EXPECT(world.components<ShipDrive>().get(player).maxAcceleration > 0.0);
    session.simulation.runFor(SimDuration::minutes(5));
    GX_EXPECT_NEAR(modules().modules[1].fraction(), content::kDamageControlCap, 1e-9); // limp-home level
}

GX_TEST(Sandbox, PlayerIsReplacedAfterBeingDestroyed) {
    SandboxConfig config;
    config.haulers = 0;
    config.pirates = 1;
    Session session(0, config);
    session.sandbox.populate(session.simulation);
    World& world = session.simulation.world();
    const EntityId player = session.sandbox.playerShip();
    const EntityId pirate = world.components<PirateBrain>().entities()[0];

    // Put the raider and the player (transponder on, nearly wrecked) together, far from any station.
    session.pilot(FlightMode::Stop);
    session.simulation.runFor(SimDuration::milliseconds(10));
    const Vec3d spot{0.0, 0.0, 5e10};
    world.components<Kinematics>().get(pirate) = {spot, {}, {}};
    world.components<ShipControl>().get(pirate).point = spot;
    world.components<Kinematics>().get(player) = {spot + Vec3d{150'000.0, 0.0, 0.0}, {}, {}};
    world.components<ShipModules>().get(player).modules[0].health = 5.0;
    session.simulation.runFor(SimDuration::seconds(20));

    GX_EXPECT(!world.isAlive(player));
    GX_EXPECT_EQ(session.sandbox.stats().playerDeaths, 1u);
    GX_EXPECT(session.journalContains("destruida"));
    GX_EXPECT(session.sandbox.stats().hunts >= 1);
    session.simulation.runFor(SimDuration::seconds(15)); // replacement after 10 s
    const EntityId replacement = session.sandbox.playerShip();
    GX_REQUIRE(replacement.isValid() && world.isAlive(replacement));
    GX_EXPECT(replacement != player);
    GX_EXPECT(session.playerControl().target == session.sandbox.homePort());
    GX_EXPECT(session.journalContains("Una nave nueva"));
}
