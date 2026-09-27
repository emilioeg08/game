#include "Tests/TestFramework.h"

#include "Engine/Jobs/JobSystem.h"
#include "Simulation/Kernel/Simulation.h"
#include "Space/Combat/Combat.h"
#include "Space/Sensors/Sensors.h"
#include "Space/Ships/Flight.h"
#include "Space/Ships/Modules.h"

#include <cmath>
#include <string>
#include <vector>

using namespace gx;

namespace {

constexpr u32 kLaser = 0;
constexpr u32 kRailgun = 1;

std::vector<WeaponDef> testWeapons() {
    return {
        {"laser", WeaponKind::Beam, 800'000.0, 25.0, 0.0, 0.0, 1e-5},
        {"railgun", WeaponKind::Projectile, 3'000'000.0, 80.0, 2.0, 5'000'000.0, 2e-5},
    };
}

ShipModules makeModules(std::initializer_list<ShipModule> modules) {
    ShipModules result;
    result.modules = modules;
    return result;
}

ShipModule module(ModuleType type, f64 health, u32 weapon = 0) {
    return {type, weapon, health, health, 0.0};
}

// Flight, sensors and combat on the same 100 ms cadence, with no star system: ships in empty space.
struct CombatWorld {
    CombatWorld() : jobs(0), simulation({.seed = 7}, jobs) {
        registerSpaceTypes(simulation);
        SensorSystem::registerTypes(simulation);
        CombatSystem::registerTypes(simulation);
        flight.setSensors(&sensors);
        flight.install(simulation, SimDuration::milliseconds(100));
        sensors.install(simulation, SimDuration::seconds(1));
        combat.setWeapons(testWeapons());
        combat.install(simulation, sensors, SimDuration::milliseconds(100));
        simulation.events().channel<ShipDestroyed>().subscribe(
            [this](const ShipDestroyed& event, const TickContext& context) {
                destroyed.push_back(event.ship);
                aliveWhenNotified = context.world.isAlive(event.ship);
            });
    }

    // A ship with a reactor, a drive, sensors and the given weapons; radar strength 3e34 when `radar`.
    EntityId addShip(u32 faction, const Vec3d& position, std::initializer_list<u32> weapons, bool radar,
                     f64 structure = 400.0) {
        World& world = simulation.world();
        const EntityId ship = world.createEntity();
        world.components<Kinematics>().add(ship, {position, {}, {}});
        world.components<ShipDrive>().add(ship, {15'000.0, 6e6, 0.0, 0.0});
        world.components<ShipControl>().add(ship, {});
        world.components<ShipIdentity>().add(ship, {"ship", faction, 0});
        world.components<SensorSuite>().add(ship, {1e12, radar ? 3e34 : 0.0, radar, false});
        world.components<SignatureProfile>().add(ship, {5e3, 2e6, 1e4});
        ShipModules modules =
            makeModules({module(ModuleType::Structure, structure), module(ModuleType::Reactor, 150.0),
                         module(ModuleType::Drive, 200.0), module(ModuleType::Sensors, 80.0),
                         module(ModuleType::Cargo, 600.0)});
        for (const u32 weapon : weapons) {
            modules.modules.push_back(module(ModuleType::Weapon, 80.0, weapon));
        }
        world.components<ShipModules>().add(ship, std::move(modules));
        world.components<ShipDesignStats>().add(ship,
                                                {15'000.0, 6e6, 0.0, 0.0, 1e12, radar ? 3e34 : 0.0, 120.0});
        world.components<CombatControl>().add(ship, {});
        return ship;
    }

    // Lets the shooter's faction pick the target up on its sensors, then orders it to fire at that track.
    u32 engage(EntityId shooter, EntityId target) {
        simulation.runFor(SimDuration::seconds(1) + SimDuration::milliseconds(50));
        const u32 faction = simulation.world().components<ShipIdentity>().get(shooter).faction;
        for (const SensorContact& contact : sensors.picture(faction).contacts) {
            if (contact.target == target) {
                simulation.world().components<CombatControl>().get(shooter).targetTrack = contact.trackId;
                return contact.trackId;
            }
        }
        return 0;
    }

    JobSystem jobs;
    Simulation simulation;
    FlightSystem flight;
    SensorSystem sensors;
    CombatSystem combat;
    std::vector<EntityId> destroyed;
    bool aliveWhenNotified = false;
};

} // namespace

GX_TEST(Combat, InterceptTimeLeadsAMovingTarget) {
    const Vec3d offset{1'000.0, 0.0, 0.0};
    const Vec3d velocity{0.0, 100.0, 0.0};
    const f64 t = interceptTime(offset, velocity, 1'000.0);
    GX_REQUIRE(t > 0.0);
    GX_EXPECT_NEAR(length(offset + velocity * t), 1'000.0 * t, 1e-6);
    GX_EXPECT(interceptTime(offset, Vec3d{2'000.0, 0.0, 0.0}, 1'000.0) < 0.0); // running away too fast
}

GX_TEST(Combat, ClosestApproachIsClampedToTheStep) {
    GX_EXPECT_NEAR(closestApproachTime({-100.0, 5.0, 0.0}, {100.0, 0.0, 0.0}, 2.0), 1.0, 1e-12);
    GX_EXPECT_NEAR(closestApproachTime({-100.0, 5.0, 0.0}, {10.0, 0.0, 0.0}, 2.0), 2.0, 1e-12);
    GX_EXPECT_NEAR(closestApproachTime({100.0, 5.0, 0.0}, {10.0, 0.0, 0.0}, 2.0), 0.0, 1e-12);
}

GX_TEST(Combat, HitsLandOnModulesInProportionToTheirSize) {
    ShipModules modules = makeModules({module(ModuleType::Structure, 1e9), module(ModuleType::Sensors, 1e6),
                                       module(ModuleType::Cargo, 9e6)});
    Rng rng(42);
    for (int i = 0; i < 10'000; ++i) {
        (void)applyDamage(modules, 1.0, rng);
    }
    const f64 sensorsHits = modules.modules[1].maxHealth - modules.modules[1].health;
    const f64 cargoHits = modules.modules[2].maxHealth - modules.modules[2].health;
    GX_EXPECT_NEAR(sensorsHits + cargoHits, 10'000.0, 1e-6);
    GX_EXPECT_NEAR(cargoHits / sensorsHits, 9.0, 1.0);
    GX_EXPECT_NEAR(modules.modules[0].maxHealth - modules.modules[0].health, 5'000.0, 1e-6); // half of each
}

GX_TEST(Combat, StructureAtZeroDestroysTheShip) {
    ShipModules modules = makeModules({module(ModuleType::Structure, 100.0), module(ModuleType::Cargo, 1e6)});
    Rng rng(1);
    GX_EXPECT(!applyDamage(modules, 150.0, rng).shipDestroyed);
    const DamageResult result = applyDamage(modules, 150.0, rng);
    GX_EXPECT(result.shipDestroyed);
    GX_EXPECT(!result.reactorBreach);
}

GX_TEST(Combat, LosingTheReactorDisablesTheShip) {
    CombatWorld arena;
    const EntityId ship = arena.addShip(0, {}, {kLaser}, true);
    World& world = arena.simulation.world();
    applyModuleEffects(world, ship);
    GX_EXPECT(world.components<ShipDrive>().get(ship).maxAcceleration > 0.0);
    GX_EXPECT(world.components<SensorSuite>().get(ship).activeOn);

    world.components<ShipModules>().get(ship).modules[1].health = 10.0; // reactor below 10%: out of action
    applyModuleEffects(world, ship);
    GX_EXPECT_EQ(world.components<ShipDrive>().get(ship).maxAcceleration, 0.0);
    GX_EXPECT_EQ(world.components<SensorSuite>().get(ship).activeStrength, 0.0);
    GX_EXPECT(!world.components<SensorSuite>().get(ship).activeOn);
    GX_EXPECT(world.components<SensorSuite>().get(ship).passiveSensitivity > 0.0); // passive still works

    world.components<ShipModules>().get(ship).modules[1].health = 150.0;
    world.components<ShipModules>().get(ship).modules[2].health = 100.0; // drive at half
    applyModuleEffects(world, ship);
    GX_EXPECT_NEAR(world.components<ShipDrive>().get(ship).maxAcceleration, 7'500.0, 1e-9);
}

GX_TEST(Combat, LaserWithRadarLockDestroysATarget) {
    CombatWorld arena;
    const EntityId shooter = arena.addShip(0, {}, {kLaser}, true);
    const EntityId target = arena.addShip(1, {300'000.0, 0.0, 0.0}, {}, false, 200.0);
    GX_REQUIRE(arena.engage(shooter, target) != 0);
    arena.simulation.runFor(SimDuration::seconds(30));
    // 25/s on target, half of it to the structure (200): destroyed within ~16 s unless the reactor goes
    // first.
    GX_EXPECT(!arena.simulation.world().isAlive(target));
    GX_REQUIRE(arena.destroyed.size() == 1u);
    GX_EXPECT(arena.destroyed[0] == target);
    GX_EXPECT(arena.aliveWhenNotified); // subscribers see the ship before it is removed
    GX_EXPECT(arena.combat.stats().hits > 0);
    for (const SensorContact& contact : arena.sensors.picture(0).contacts) {
        GX_EXPECT(contact.ghost || contact.target != target); // its track ends with it
    }
}

GX_TEST(Combat, NoLockNoFire) {
    CombatWorld arena;
    const EntityId shooter = arena.addShip(0, {}, {kLaser, kRailgun}, true);
    arena.addShip(1, {300'000.0, 0.0, 0.0}, {}, false);
    arena.simulation.runFor(SimDuration::seconds(2));
    arena.simulation.world().components<CombatControl>().get(shooter).targetTrack = 999; // no such track
    arena.simulation.runFor(SimDuration::seconds(10));
    GX_EXPECT_EQ(arena.combat.stats().shotsFired, 0u);

    // A real track out of weapon range: no shots either.
    CombatWorld far;
    const EntityId farShooter = far.addShip(0, {}, {kLaser, kRailgun}, true);
    const EntityId farTarget = far.addShip(1, {5e6, 0.0, 0.0}, {}, false);
    GX_REQUIRE(far.engage(farShooter, farTarget) != 0);
    far.simulation.runFor(SimDuration::seconds(10));
    GX_EXPECT_EQ(far.combat.stats().shotsFired, 0u);
}

GX_TEST(Combat, RadarMakesFireControlPrecise) {
    // A small, quiet, idle target 700 km away: the passive lock is ~500 m off, the radar lock is exact.
    const auto hitRate = [](bool radar) {
        CombatWorld arena;
        const EntityId shooter = arena.addShip(0, {}, {kLaser}, radar);
        const EntityId target = arena.addShip(1, {700'000.0, 0.0, 0.0}, {}, false, 1e9);
        arena.simulation.world().components<SignatureProfile>().get(target) = {100.0, 1e6, 1e3};
        arena.simulation.world().components<ShipDesignStats>().get(target).hullRadius = 40.0;
        if (arena.engage(shooter, target) == 0) {
            return -1.0;
        }
        arena.simulation.runFor(SimDuration::seconds(10));
        const CombatStats& stats = arena.combat.stats();
        return stats.shotsFired == 0 ? 0.0
                                     : static_cast<f64>(stats.hits) / static_cast<f64>(stats.shotsFired);
    };
    const f64 passive = hitRate(false);
    const f64 active = hitRate(true);
    GX_EXPECT(passive >= 0.0);
    GX_EXPECT(passive < 0.2);
    GX_EXPECT(active > 0.95);
}

GX_TEST(Combat, RailgunSlugsMissTargetsThatAccelerate) {
    const auto hits = [](bool evade) {
        CombatWorld arena;
        const EntityId shooter = arena.addShip(0, {}, {kRailgun}, true);
        const EntityId target = arena.addShip(1, {2'000'000.0, 0.0, 0.0}, {}, false, 1e9);
        if (evade) {
            ShipControl& control = arena.simulation.world().components<ShipControl>().get(target);
            control.mode = FlightMode::Manual;
            control.manualThrust = {0.0, 1.0, 0.0}; // full burn across the line of fire
        }
        if (arena.engage(shooter, target) == 0) {
            return u64{9999};
        }
        arena.simulation.runFor(SimDuration::seconds(10));
        GX_EXPECT(arena.combat.stats().shotsFired >= 5u); // one slug every 2 s
        return arena.combat.stats().hits;
    };
    GX_EXPECT(hits(false) >= 4u);
    GX_EXPECT_EQ(hits(true), 0u);
}

GX_TEST(Combat, ProjectilesAreSavedAndCombatIsDeterministic) {
    const auto run = [](bool saveHalfway) {
        CombatWorld arena;
        const EntityId shooter = arena.addShip(0, {}, {kLaser, kRailgun}, true);
        const EntityId target = arena.addShip(1, {1'500'000.0, 0.0, 0.0}, {}, false, 1'000.0);
        arena.engage(shooter, target);
        for (int i = 0; i < 100 && arena.combat.projectiles().empty(); ++i) {
            arena.simulation.runFor(SimDuration::milliseconds(50));
        }
        GX_EXPECT(!arena.combat.projectiles().empty()); // save with a slug in flight
        if (saveHalfway) {
            const std::vector<std::byte> saved = arena.simulation.saveState();
            CombatWorld restored; // same registrations; the save brings the ships
            std::string error;
            GX_REQUIRE(restored.simulation.loadState(saved, error));
            GX_EXPECT(restored.combat.projectiles().size() == arena.combat.projectiles().size());
            restored.simulation.runFor(SimDuration::seconds(20));
            return restored.simulation.stateHash();
        }
        arena.simulation.runFor(SimDuration::seconds(20));
        return arena.simulation.stateHash();
    };
    GX_EXPECT_EQ(run(true), run(false));
}
