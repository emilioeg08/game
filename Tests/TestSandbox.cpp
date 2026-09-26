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

GX_TEST(Sandbox, NewGameHasPlayerHaulersAndPorts) {
    Session session(0);
    session.sandbox.populate(session.simulation);
    const World& world = session.simulation.world();
    GX_REQUIRE(world.isAlive(session.sandbox.playerShip()));
    GX_EXPECT_EQ(world.components<ShipIdentity>().get(session.sandbox.playerShip()).faction,
                 static_cast<u32>(content::kFactionPlayer));
    GX_EXPECT_EQ(world.components<HaulerBrain>().size(),
                 static_cast<usize>(session.sandbox.config().haulers));
    GX_EXPECT(session.sandbox.ports().size() >= 6); // 4-9 planets + 2 stations
    GX_EXPECT(!session.sandbox.systemName().empty());
    GX_EXPECT_EQ(session.sandbox.journal().size(), 1u);
    GX_EXPECT(session.playerControl().arrived); // starts docked at the main station
}

GX_TEST(Sandbox, HaulersTravelWithoutThePlayer) {
    Session session(3);
    session.sandbox.populate(session.simulation);
    session.simulation.runFor(SimDuration::days(2));
    GX_EXPECT(session.sandbox.stats().haulerDepartures >= session.sandbox.config().haulers);
    GX_EXPECT(session.sandbox.stats().arrivals > 0);
    GX_EXPECT(session.journalContains("atraca en"));
}

GX_TEST(Sandbox, PlayerFliesToAPortByCommand) {
    Session session(3);
    session.sandbox.populate(session.simulation);
    const EntityId destination = session.destinationPort();
    session.pilot(FlightMode::Approach, destination);
    session.simulation.runFor(SimDuration::minutes(1));
    GX_EXPECT(session.playerControl().target == destination);
    GX_EXPECT(!session.playerControl().arrived);

    for (int day = 0; day < 3 && !session.playerControl().arrived; ++day) {
        session.simulation.runFor(SimDuration::days(1));
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
        session.simulation.runFor(SimDuration::hours(3));
        session.pilot(FlightMode::Manual, {}, {}, Vec3d{1.0, 0.0, 0.0});
        session.simulation.runFor(SimDuration::minutes(10));
        session.pilot(FlightMode::Approach, destination);
        session.simulation.runFor(SimDuration::days(1));
        return session.simulation.stateHash();
    };
    const u64 reference = play(0);
    GX_EXPECT_EQ(play(3), reference);
    GX_EXPECT_EQ(play(11), reference);
}

GX_TEST(Sandbox, SaveLoadContinuesIdentically) {
    const auto opening = [](Session& session) {
        session.sandbox.populate(session.simulation);
        session.simulation.runFor(SimDuration::hours(20));
        session.pilot(FlightMode::Approach, session.destinationPort()); // in flight when saved
        session.simulation.runFor(SimDuration::hours(1));
    };
    Session continuous(3);
    opening(continuous);
    continuous.simulation.runFor(SimDuration::days(1));

    Session saver(3);
    opening(saver);
    const std::vector<std::byte> saved = saver.simulation.saveState();

    Session loaded(0); // no populate: everything comes from the save
    std::string error;
    GX_REQUIRE(loaded.simulation.loadState(saved, error));
    GX_EXPECT(loaded.simulation.saveState() == saved);
    GX_EXPECT_EQ(loaded.sandbox.systemName(), saver.sandbox.systemName());
    GX_EXPECT(loaded.sandbox.ports() == saver.sandbox.ports());
    loaded.simulation.runFor(SimDuration::days(1));
    GX_EXPECT_EQ(loaded.simulation.stateHash(), continuous.simulation.stateHash());
}

GX_TEST(Sandbox, SnapshotDescribesTheWorld) {
    Session session(0);
    session.sandbox.populate(session.simulation);
    session.simulation.runFor(SimDuration::minutes(30) + SimDuration::milliseconds(500));
    SnapshotBuilder builder;
    SystemSnapshot snapshot;
    builder.build(session.simulation, session.sandbox, snapshot);
    GX_EXPECT_EQ(snapshot.ships.size(), static_cast<usize>(session.sandbox.config().haulers + 1));
    GX_EXPECT_EQ(snapshot.bodies.size(), session.simulation.world().components<CelestialBody>().size());
    const ShipView* player = snapshot.findShip(session.sandbox.playerShip());
    GX_REQUIRE(player != nullptr);
    GX_EXPECT(player->isPlayer);
    for (const BodyView& body : snapshot.bodies) {
        GX_EXPECT(body.kind == BodyKind::Star ||
                  (body.orbitPath != nullptr && body.orbitPath->size() == SnapshotBuilder::kOrbitPathPoints));
    }
    // Ships are extrapolated half a second past their last flight step (1 s period).
    const Kinematics& state =
        session.simulation.world().components<Kinematics>().get(session.sandbox.playerShip());
    GX_EXPECT_NEAR(length(player->position - (state.position + state.velocity * 0.5)), 0.0, 1e-3);
}
