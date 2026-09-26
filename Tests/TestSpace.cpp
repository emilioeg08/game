#include "Tests/TestFramework.h"

#include "Engine/Jobs/JobSystem.h"
#include "Simulation/Kernel/Simulation.h"
#include "Simulation/World/Inspect.h"
#include "Space/Bodies/CelestialBody.h"
#include "Space/Generation/StarSystemGenerator.h"
#include "Space/Orbits/Kepler.h"
#include "Space/Ships/Flight.h"

#include <algorithm>
#include <cmath>
#include <set>
#include <string>
#include <vector>

using namespace gx;

namespace {

constexpr f64 kSunGm = 1.32712440018e20;
constexpr f64 kAu = 1.495978707e11;

SimTime seconds(f64 s) {
    return SimTime::epoch() + SimDuration::microseconds(static_cast<i64>(s * 1e6));
}

} // namespace

GX_TEST(Space, KeplerSolverResidualIsTiny) {
    for (const f64 e : {0.0, 0.1, 0.5, 0.9, 0.97}) {
        for (f64 m = -kPi; m <= kPi; m += 0.37) {
            const f64 eccentricAnomaly = solveKepler(m, e);
            GX_EXPECT_NEAR(eccentricAnomaly - e * std::sin(eccentricAnomaly), m, 1e-12);
        }
    }
}

GX_TEST(Space, CircularOrbitRotatesAndCloses) {
    OrbitalElements orbit;
    orbit.semiMajorAxis = kAu;
    const f64 period = orbitalPeriod(orbit, kSunGm);
    GX_EXPECT_NEAR(period / 86'400.0, 365.25, 0.1); // Earth-like orbit around a Sun-like star

    const OrbitState start = orbitStateAt(orbit, kSunGm, SimTime::epoch());
    const OrbitState quarter = orbitStateAt(orbit, kSunGm, seconds(period / 4.0));
    const OrbitState full = orbitStateAt(orbit, kSunGm, seconds(period));
    GX_EXPECT_NEAR(length(start.position), kAu, 1.0);
    GX_EXPECT_NEAR(start.position.x, kAu, 1.0);
    GX_EXPECT_NEAR(quarter.position.y, kAu, 1e3); // counter-clockwise, a quarter turn later
    GX_EXPECT_NEAR(length(full.position - start.position), 0.0, 1e3);
    GX_EXPECT_NEAR(length(start.velocity), std::sqrt(kSunGm / kAu), 1e-6);
}

GX_TEST(Space, EccentricOrbitObeysVisViva) {
    OrbitalElements orbit;
    orbit.semiMajorAxis = 2.0 * kAu;
    orbit.eccentricity = 0.5;
    orbit.inclination = 0.3;
    orbit.longitudeOfAscendingNode = 1.1;
    orbit.argumentOfPeriapsis = 2.2;
    orbit.meanAnomalyAtEpoch = 0.4;
    const f64 period = orbitalPeriod(orbit, kSunGm);
    for (int i = 0; i < 12; ++i) {
        const SimTime t = seconds(period * i / 12.0 + 1234.0);
        const OrbitState s = orbitStateAt(orbit, kSunGm, t);
        const f64 r = length(s.position);
        const f64 speedSq = lengthSquared(s.velocity);
        const f64 visViva = kSunGm * (2.0 / r - 1.0 / orbit.semiMajorAxis);
        GX_EXPECT_NEAR(speedSq / visViva, 1.0, 1e-9);
        GX_EXPECT(r >= orbit.semiMajorAxis * (1.0 - orbit.eccentricity) * (1.0 - 1e-9));
        GX_EXPECT(r <= orbit.semiMajorAxis * (1.0 + orbit.eccentricity) * (1.0 + 1e-9));
        // Analytic velocity matches the derivative of the position.
        const OrbitState later = orbitStateAt(orbit, kSunGm, t + SimDuration::seconds(1));
        GX_EXPECT_NEAR(length((later.position - s.position) - s.velocity) / length(s.velocity), 0.0, 1e-5);
    }
}

GX_TEST(Space, OrbitsStayPreciseAfterCenturies) {
    OrbitalElements orbit;
    orbit.semiMajorAxis = 384'400'000.0; // a Moon-like orbit: many revolutions per year
    const f64 earthGm = 3.986004418e14;
    const f64 period = orbitalPeriod(orbit, earthGm);
    const i64 revolutions = static_cast<i64>(300.0 * 365.25 * 86'400.0 / period);
    // Exactly N periods later (to the microsecond) the body is back where it started.
    const i64 periodUs = static_cast<i64>(period * 1e6);
    const OrbitState start = orbitStateAt(orbit, earthGm, SimTime::epoch());
    const OrbitState later = orbitStateAt(orbit, earthGm, SimTime::fromMicroseconds(periodUs * revolutions));
    const f64 drift = length(later.position - start.position);
    // The only error is the sub-microsecond rounding of the period, times the orbital speed.
    GX_EXPECT(drift < 1.0 * static_cast<f64>(revolutions) * 1e-6 * 1'100.0 + 1.0);
}

GX_TEST(Space, GenerationIsDeterministicAndWellFormed) {
    const StarSystemDesc a = generateStarSystem(7);
    const StarSystemDesc b = generateStarSystem(7);
    const StarSystemDesc c = generateStarSystem(8);
    GX_REQUIRE(a.bodies.size() == b.bodies.size());
    for (usize i = 0; i < a.bodies.size(); ++i) {
        GX_EXPECT_EQ(a.bodies[i].name, b.bodies[i].name);
        GX_EXPECT_EQ(a.bodies[i].orbit.semiMajorAxis, b.bodies[i].orbit.semiMajorAxis);
    }
    GX_EXPECT(a.name != c.name || a.bodies.size() != c.bodies.size());

    for (const u64 seed : {1ull, 2ull, 3ull, 42ull, 2400ull, 99999ull}) {
        const StarSystemDesc system = generateStarSystem(seed);
        GX_REQUIRE(!system.bodies.empty());
        GX_EXPECT(system.bodies[0].kind == BodyKind::Star);
        std::set<std::string> names;
        f64 previousPlanetOrbit = 0.0;
        u32 planets = 0;
        u32 stations = 0;
        for (usize i = 0; i < system.bodies.size(); ++i) {
            const BodyDesc& body = system.bodies[i];
            GX_EXPECT(names.insert(body.name).second); // unique names
            GX_EXPECT(body.parent < static_cast<i32>(i));
            if (isPlanet(body.kind)) {
                ++planets;
                GX_EXPECT(body.parent == 0);
                GX_EXPECT(body.orbit.semiMajorAxis > previousPlanetOrbit);
                previousPlanetOrbit = body.orbit.semiMajorAxis;
            }
            if (body.kind == BodyKind::Moon || body.kind == BodyKind::Station) {
                GX_EXPECT(isPlanet(system.bodies[static_cast<usize>(body.parent)].kind));
                GX_EXPECT(body.orbit.semiMajorAxis > system.bodies[static_cast<usize>(body.parent)].radius);
            }
            stations += body.kind == BodyKind::Station ? 1 : 0;
        }
        GX_EXPECT(planets >= 4 && planets <= 9);
        GX_EXPECT_EQ(stations, 2u);
    }
}

GX_TEST(Space, AutopilotReachesAPointAndStops) {
    for (const f64 dt : {0.1, 1.0, 5.0}) {
        ShipControl control;
        control.mode = FlightMode::MoveTo;
        control.point = {kAu, 0.0, 0.0};
        Kinematics ship;
        const ShipDrive drive{600.0, 1'500'000.0};
        f64 time = 0.0;
        u32 arrivals = 0;
        while (time < 5.0 * 86'400.0) {
            bool arrivedNow = false;
            const TargetState target{control.point, {}, true};
            const Vec3d acceleration = steer(control, ship, drive, target, dt, arrivedNow);
            GX_REQUIRE(length(acceleration) <= drive.maxAcceleration * (1.0 + 1e-12));
            ship.velocity += acceleration * dt;
            ship.position += ship.velocity * dt;
            arrivals += arrivedNow ? 1 : 0;
            time += dt;
            if (control.arrived && length(ship.velocity) < 1e-6) {
                break;
            }
        }
        // Accelerate to cruise, cruise, brake: ~ d/v + v/a. Never overshoot.
        const f64 ideal = kAu / drive.cruiseSpeed + drive.cruiseSpeed / drive.maxAcceleration;
        GX_EXPECT_EQ(arrivals, 1u);
        GX_EXPECT(time < ideal * 1.15);
        GX_EXPECT_NEAR(ship.position.x, kAu, 150.0);
        GX_EXPECT_NEAR(length(ship.velocity), 0.0, 1e-3);
    }
}

GX_TEST(Space, AutopilotKeepsStationWithAnOrbitingBody) {
    JobSystem jobs(0);
    Simulation simulation({}, jobs);
    registerSpaceTypes(simulation);
    FlightSystem flight;
    flight.install(simulation, SimDuration::seconds(1));
    World& world = simulation.world();

    const EntityId star = world.createEntity();
    world.components<CelestialBody>().add(star, {"Star", BodyKind::Star, 7e8, kSunGm});
    const EntityId planet = world.createEntity();
    world.components<CelestialBody>().add(planet, {"Planet", BodyKind::OceanPlanet, 6.4e6, 3.986e14});
    OrbitalElements orbit;
    orbit.semiMajorAxis = kAu;
    world.components<OrbitsParent>().add(planet, {star, orbit});

    const EntityId ship = world.createEntity();
    world.components<Kinematics>().add(ship, {{0.9 * kAu, 0.1 * kAu, 0.0}, {}, {}});
    world.components<ShipDrive>().add(ship, {3'000.0, 4'000'000.0});
    ShipControl control;
    control.mode = FlightMode::Approach;
    control.target = planet;
    control.standoff = standoffDistance(world, planet);
    world.components<ShipControl>().add(ship, control);

    u32 arrivals = 0;
    simulation.events().channel<ShipArrived>().subscribe([&](const ShipArrived& e, const TickContext&) {
        GX_EXPECT(e.ship == ship);
        GX_EXPECT(e.target == planet);
        ++arrivals;
    });
    simulation.runFor(SimDuration::days(2));
    GX_EXPECT_EQ(arrivals, 1u);
    GX_EXPECT(world.components<ShipControl>().get(ship).arrived);

    // Then it holds formation with the planet as the planet moves ~2.5 million km per day.
    for (int hour = 0; hour < 24; ++hour) {
        simulation.runFor(SimDuration::hours(1));
        const OrbitState body = bodyStateAt(world, planet, simulation.now());
        const Kinematics& state = world.components<Kinematics>().get(ship);
        GX_EXPECT_NEAR(length(state.position - body.position), control.standoff, 2'000.0);
        GX_EXPECT_NEAR(length(state.velocity - body.velocity), 0.0, 10.0);
    }
}

GX_TEST(Space, HyperspaceJumpsOutsideWellsAndDropsAtTheTargetWell) {
    JobSystem jobs(0);
    Simulation simulation({}, jobs);
    registerSpaceTypes(simulation);
    FlightSystem flight;
    flight.install(simulation, SimDuration::milliseconds(200));
    World& world = simulation.world();

    const EntityId star = world.createEntity();
    world.components<CelestialBody>().add(star, {"Star", BodyKind::Star, 7e8, kSunGm});
    const auto addPlanet = [&](const char* name, f64 au, f64 meanAnomaly) {
        const EntityId planet = world.createEntity();
        world.components<CelestialBody>().add(planet, {name, BodyKind::OceanPlanet, 6.4e6, 3.986e14});
        OrbitalElements orbit;
        orbit.semiMajorAxis = au * kAu;
        orbit.meanAnomalyAtEpoch = meanAnomaly;
        world.components<OrbitsParent>().add(planet, {star, orbit});
        return planet;
    };
    const EntityId origin = addPlanet("Origin", 1.0, 0.0);
    const EntityId destination = addPlanet("Destination", 2.0, kPi / 2.0);
    const f64 wellRadius = gravityWellRadius(world.components<CelestialBody>().get(origin));

    // Start inside the origin's gravity well: the ship must fly out before it may jump.
    const OrbitState start = bodyStateAt(world, origin, simulation.now());
    const EntityId ship = world.createEntity();
    world.components<Kinematics>().add(
        ship, {start.position + Vec3d{0.5 * wellRadius, 0.0, 0.0}, start.velocity, {}});
    world.components<ShipDrive>().add(ship, {50'000.0, 15'000'000.0, 1.5e9, 5.0});
    ShipControl control;
    control.mode = FlightMode::Approach;
    control.target = destination;
    control.standoff = standoffDistance(world, destination);
    world.components<ShipControl>().add(ship, control);

    u32 jumps = 0;
    u32 dropOuts = 0;
    u32 arrivals = 0;
    simulation.events().channel<HyperspaceTransition>().subscribe(
        [&](const HyperspaceTransition& e, const TickContext& c) {
            const Vec3d originNow = bodyStateAt(c.world, origin, c.now).position;
            const Vec3d destinationNow = bodyStateAt(c.world, destination, c.now).position;
            if (e.entering) {
                ++jumps;
                GX_EXPECT(length(e.position - originNow) >= wellRadius); // never jumps inside a well
            } else {
                ++dropOuts;
                // Drops out on the edge of the destination's well (the planet moves ~5 km per 200 ms step).
                GX_EXPECT_NEAR(length(e.position - destinationNow), wellRadius, 20'000.0);
            }
        });
    simulation.events().channel<ShipArrived>().subscribe(
        [&](const ShipArrived&, const TickContext&) { ++arrivals; });

    for (int minute = 0; minute < 30 && arrivals == 0; ++minute) {
        simulation.runFor(SimDuration::minutes(1));
    }
    GX_EXPECT_EQ(jumps, 1u);
    GX_EXPECT_EQ(dropOuts, 1u);
    GX_EXPECT_EQ(arrivals, 1u);
    // About 2.2 AU: well exit + charge + ~3 min of hyperspace + sublight approach.
    GX_EXPECT(simulation.now() < SimTime::epoch() + SimDuration::minutes(12));
    GX_EXPECT(world.components<ShipControl>().get(ship).phase == DrivePhase::Sublight);
}

GX_TEST(Space, InspectorListsNamedFields) {
    struct Collector final : FieldVisitor {
        std::vector<std::string> lines;
        std::vector<std::string> groups;
        std::string path(std::string_view name) const {
            std::string result;
            for (const std::string& group : groups) {
                result += group + "/";
            }
            return result + std::string(name);
        }
        void beginGroup(std::string_view name) override { groups.emplace_back(name); }
        void endGroup() override { groups.pop_back(); }
        void field(std::string_view name, std::string_view value) override {
            lines.push_back(path(name) + "=" + std::string(value));
        }
        void entityField(std::string_view name, EntityId entity) override {
            lines.push_back(path(name) + "=#" + std::to_string(entity.index));
        }
    };
    JobSystem jobs(0);
    Simulation simulation({}, jobs);
    registerSpaceTypes(simulation);
    World& world = simulation.world();
    const EntityId ship = world.createEntity();
    world.components<ShipDrive>().add(ship, {600.0, 1'500'000.0});
    ShipControl control;
    control.target = EntityId{7, 0};
    world.components<ShipControl>().add(ship, control);

    Collector collector;
    world.inspect(ship, collector);
    const auto has = [&](const std::string& line) {
        return std::find(collector.lines.begin(), collector.lines.end(), line) != collector.lines.end();
    };
    GX_EXPECT(has("Space.ShipDrive/maxAcceleration=600"));
    GX_EXPECT(has("Space.ShipDrive/cruiseSpeed=1500000"));
    GX_EXPECT(has("Space.ShipControl/target=#7"));
    GX_EXPECT(has("Space.ShipControl/arrived=false"));
}
