#include "Tests/TestFramework.h"

#include "Engine/Jobs/JobSystem.h"
#include "Game/Sandbox/Content.h"
#include "Game/Sandbox/Sandbox.h"
#include "Simulation/Kernel/Simulation.h"
#include "Space/Sensors/Sensors.h"
#include "Space/Ships/Flight.h"

#include <cmath>

using namespace gx;

namespace {

constexpr f64 kAu = 1.495978707e11;
constexpr SensorSuite kHaulerSensors{5e11, 0.0, false, true};
constexpr SensorSuite kQuietCourier{1e12, 6.25e34, false, false}; // radar off, transponder off
constexpr SignatureProfile kCourierProfile{1e3, 1e6, 1e3};
constexpr SignatureProfile kHaulerProfile{5e3, 2e6, 1e4};

// A minimal world with sensors and static ships.
struct SensorWorld {
    SensorWorld() {
        registerSpaceTypes(simulation);
        SensorSystem::registerTypes(simulation);
        sensors.install(simulation, SimDuration::seconds(1));
    }

    EntityId addShip(u32 faction, const Vec3d& position, const SensorSuite& suite,
                     const SignatureProfile& profile) {
        World& world = simulation.world();
        const EntityId ship = world.createEntity();
        world.components<Kinematics>().add(ship, {position, {}, {}});
        world.components<ShipDrive>().add(ship, {1'000.0, 1e6, 0.0, 0.0});
        world.components<ShipControl>().add(ship, {});
        world.components<ShipIdentity>().add(ship, {"ship", faction, 0});
        world.components<SensorSuite>().add(ship, suite);
        world.components<SignatureProfile>().add(ship, profile);
        return ship;
    }

    [[nodiscard]] const SensorContact* contactOn(u32 faction, EntityId target) const {
        for (const SensorContact& contact : sensors.picture(faction).contacts) {
            if (!contact.ghost && contact.target == target) {
                return &contact;
            }
        }
        return nullptr;
    }

    JobSystem jobs{0};
    Simulation simulation{Simulation::Config{.seed = 5}, jobs};
    SensorSystem sensors;
};

} // namespace

GX_TEST(Sensors, DetectionPhysics) {
    GX_EXPECT_NEAR(passiveDetectionRange(1e3, 5e11), 2.2360679775e7, 1.0); // idle Correo vs Carguero sensors
    GX_EXPECT_NEAR(passiveSnr(1e6, 5e11, std::sqrt(5e17)), 1.0, 1e-9);
    GX_EXPECT_NEAR(activeSnr(6.25e34, 1e4, 5e9), 1.0, 1e-9); // radar range against a Carguero
    GX_EXPECT_EQ(detectionProbability(0.1), 0.0);
    GX_EXPECT_EQ(detectionProbability(1.0), 1.0);
    GX_EXPECT_NEAR(detectionProbability(0.625), 0.5, 1e-12);
}

GX_TEST(Sensors, EmissionReflectsActivity) {
    const ShipDrive drive{1'000.0, 1e6, 1e9, 5.0};
    Kinematics state;
    ShipControl control;
    SensorSuite suite = kQuietCourier;
    const f64 idle = shipEmission(kCourierProfile, drive, state, control, suite);
    state.acceleration = {500.0, 0.0, 0.0};
    const f64 halfThrust = shipEmission(kCourierProfile, drive, state, control, suite);
    suite.activeOn = true;
    const f64 withRadar = shipEmission(kCourierProfile, drive, state, control, suite);
    control.phase = DrivePhase::Hyperspace;
    const f64 inHyperspace = shipEmission(kCourierProfile, drive, state, control, suite);
    GX_EXPECT_EQ(idle, 1e3);
    GX_EXPECT_EQ(halfThrust, 1e3 + 5e5);
    GX_EXPECT_EQ(withRadar, halfThrust + kActiveSensorEmission);
    GX_EXPECT_EQ(inHyperspace, withRadar + kHyperspaceEmission);
}

GX_TEST(Sensors, QuietShipsStayHiddenAndRadarGivesThemAway) {
    SensorWorld world;
    world.addShip(1, {}, kHaulerSensors, kHaulerProfile);
    const EntityId intruder = world.addShip(0, {1e8, 0.0, 0.0}, kQuietCourier, kCourierProfile); // 100,000 km
    world.simulation.runFor(SimDuration::seconds(10));
    GX_EXPECT(world.contactOn(1, intruder) == nullptr); // idle, no transponder: invisible at this range

    world.simulation.world().components<SensorSuite>().get(intruder).activeOn = true; // its radar pulses...
    world.simulation.runFor(SimDuration::seconds(2));
    const SensorContact* contact = world.contactOn(1, intruder);
    GX_REQUIRE(contact != nullptr); // ...are heard far beyond the radar's own range
    GX_EXPECT(contact->level == ContactLevel::Identified);
}

GX_TEST(Sensors, TransponderIdentifiesWithinRange) {
    SensorWorld world;
    world.addShip(1, {}, kHaulerSensors, kHaulerProfile);
    SensorSuite beacon = kQuietCourier;
    beacon.transponderOn = true;
    const EntityId nearShip = world.addShip(0, {0.6 * kAu, 0.0, 0.0}, beacon, kCourierProfile);
    const EntityId farShip = world.addShip(0, {-1.5 * kAu, 0.0, 0.0}, beacon, kCourierProfile);
    world.simulation.runFor(SimDuration::seconds(3));
    const SensorContact* contact = world.contactOn(1, nearShip);
    GX_REQUIRE(contact != nullptr);
    GX_EXPECT(contact->level == ContactLevel::Identified);
    GX_EXPECT_NEAR(contact->uncertainty, 1'000.0, 1e-9);
    GX_EXPECT(world.contactOn(1, farShip) == nullptr);
}

GX_TEST(Sensors, RadarFindsQuietTargetsWithNoise) {
    SensorWorld world;
    SensorSuite radar = kQuietCourier;
    radar.activeOn = true;
    world.addShip(0, {}, radar, kCourierProfile);
    const Vec3d truth{2e9, 5e8, 0.0};
    SensorSuite silent = kHaulerSensors;
    silent.transponderOn = false;
    const EntityId target = world.addShip(1, truth, silent, kHaulerProfile);
    world.simulation.runFor(SimDuration::seconds(3));
    const SensorContact* contact = world.contactOn(0, target);
    GX_REQUIRE(contact != nullptr);
    GX_EXPECT(contact->level == ContactLevel::Identified); // active SNR ~ 37
    GX_EXPECT(contact->uncertainty > 1e4 && contact->uncertainty < 1e6);
    GX_EXPECT(length(contact->position - truth) > 0.0); // an estimate, not the truth...
    GX_EXPECT(length(contact->position - truth) < 5.0 * contact->uncertainty); // ...but a consistent one
}

GX_TEST(Sensors, ContactsAreLostAndGhostsFade) {
    SensorWorld world;
    world.addShip(1, {}, kHaulerSensors, kHaulerProfile);
    SensorSuite beacon = kQuietCourier;
    beacon.transponderOn = true;
    const EntityId target = world.addShip(0, {1e10, 0.0, 0.0}, beacon, kCourierProfile);
    world.simulation.runFor(SimDuration::seconds(2));
    GX_REQUIRE(world.contactOn(1, target) != nullptr);
    const u32 trackId = world.contactOn(1, target)->trackId;

    // It slips out of range: the last known position lingers, then the track is dropped.
    world.simulation.world().components<Kinematics>().get(target).position = {3.0 * kAu, 0.0, 0.0};
    world.simulation.runFor(SimDuration::seconds(10));
    const SensorContact* stale = world.contactOn(1, target);
    GX_REQUIRE(stale != nullptr);
    GX_EXPECT_EQ(stale->trackId, trackId);
    GX_EXPECT(world.simulation.now() - stale->lastSeen >= SimDuration::seconds(9));
    world.simulation.runFor(SimDuration::seconds(15));
    GX_EXPECT(world.contactOn(1, target) == nullptr);

    // Noise produces the occasional ghost, which never outlives its short lifetime.
    u32 ghostsSeen = 0;
    for (int second = 0; second < 600; ++second) {
        world.simulation.runFor(SimDuration::seconds(1));
        for (const SensorContact& contact : world.sensors.picture(1).contacts) {
            if (contact.ghost) {
                ++ghostsSeen;
                GX_EXPECT(world.simulation.now() - contact.lastSeen <= kGhostLifetime);
                GX_EXPECT(!contact.target.isValid());
            }
        }
    }
    GX_EXPECT(ghostsSeen > 0);
}

GX_TEST(Sensors, NoOmniscienceInTheSandbox) {
    JobSystem jobs(0);
    Sandbox sandbox(SandboxConfig{});
    Simulation simulation(Simulation::Config{.seed = 2400}, jobs);
    sandbox.install(simulation);
    sandbox.populate(simulation);
    simulation.runFor(SimDuration::seconds(5));

    const World& world = simulation.world();
    const FactionPicture& picture = sandbox.sensors().picture(content::kFactionPlayer);
    usize identified = 0;
    for (const SensorContact& contact : picture.contacts) {
        if (contact.ghost) {
            continue;
        }
        GX_EXPECT(contact.target != sandbox.playerShip()); // own ships are not sensor contacts
        GX_EXPECT(world.components<ShipIdentity>().get(contact.target).faction !=
                  static_cast<u32>(content::kFactionPlayer));
        identified += contact.level == ContactLevel::Identified ? 1 : 0;
    }
    // Ships far away and idle are beyond every sensor, and raiders keep their transponders off: the player
    // does not know where everybody is.
    GX_EXPECT(identified < world.components<ShipIdentity>().size() - 1);
    GX_EXPECT(identified > 0); // nearby haulers broadcast their transponders
}
