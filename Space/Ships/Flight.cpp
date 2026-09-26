#include "Space/Ships/Flight.h"

#include "Engine/Jobs/JobSystem.h"
#include "Engine/Profiling/Profiler.h"
#include "Simulation/Kernel/Simulation.h"
#include "Space/Bodies/CelestialBody.h"

#include <algorithm>
#include <cmath>

namespace gx {
namespace {

constexpr f64 kBrakingMargin = 0.8;         // plan braking at 80% of the drive's acceleration
constexpr f64 kArrivalSpeedTolerance = 5.0; // m/s relative to the target
constexpr f64 kMinArrivalTolerance = 100.0; // m

Vec3d clampLength(const Vec3d& v, f64 maxLength) {
    const f64 lengthSq = lengthSquared(v);
    if (lengthSq <= maxLength * maxLength) {
        return v;
    }
    return v * (maxLength / std::sqrt(lengthSq));
}

} // namespace

const char* toString(FlightMode mode) {
    switch (mode) {
    case FlightMode::Coast:
        return "Coast";
    case FlightMode::Stop:
        return "Stop";
    case FlightMode::MoveTo:
        return "MoveTo";
    case FlightMode::Approach:
        return "Approach";
    case FlightMode::Manual:
        return "Manual";
    case FlightMode::Count:
        break;
    }
    return "Unknown";
}

void registerSpaceTypes(Simulation& simulation) {
    World& world = simulation.world();
    world.registerComponent<CelestialBody>("Space.CelestialBody");
    world.registerComponent<OrbitsParent>("Space.OrbitsParent");
    world.registerComponent<Kinematics>("Space.Kinematics");
    world.registerComponent<ShipDrive>("Space.ShipDrive");
    world.registerComponent<ShipControl>("Space.ShipControl");
    world.registerComponent<ShipIdentity>("Space.ShipIdentity");
    simulation.events().registerEvent<ShipArrived>("Space.ShipArrived");
}

TargetState resolveTarget(const World& world, const ShipControl& control, SimTime time) {
    if (control.mode == FlightMode::MoveTo) {
        return {control.point, Vec3d{}, true};
    }
    if (control.mode != FlightMode::Approach || !world.isAlive(control.target)) {
        return {};
    }
    if (const Kinematics* ship = world.components<Kinematics>().tryGet(control.target)) {
        return {ship->position, ship->velocity, true};
    }
    if (world.components<CelestialBody>().contains(control.target)) {
        const OrbitState state = bodyStateAt(world, control.target, time);
        return {state.position, state.velocity, true};
    }
    return {};
}

f64 standoffDistance(const World& world, EntityId target) {
    const auto& bodies = world.components<CelestialBody>();
    if (!bodies.contains(target)) {
        return 2'000.0; // another ship
    }
    const CelestialBody& body = bodies.get(target);
    switch (body.kind) {
    case BodyKind::Station:
        return 5'000.0;
    case BodyKind::Star:
        return body.radius * 3.0;
    default:
        return body.radius * 1.25 + 200'000.0; // low orbit altitude
    }
}

Vec3d steer(ShipControl& control, const Kinematics& kinematics, const ShipDrive& drive,
            const TargetState& target, f64 dt, bool& arrivedNow) {
    arrivedNow = false;
    const f64 maxAcceleration = drive.maxAcceleration;
    switch (control.mode) {
    case FlightMode::Coast:
    case FlightMode::Count:
        return {};
    case FlightMode::Stop:
        return clampLength(-kinematics.velocity / dt, maxAcceleration);
    case FlightMode::Manual:
        return clampLength(control.manualThrust, 1.0) * maxAcceleration;
    case FlightMode::MoveTo:
    case FlightMode::Approach:
        break;
    }

    if (!target.valid) {
        // The target vanished (destroyed, or invalid order): hold position instead of flying blind.
        control.mode = FlightMode::Stop;
        control.arrived = false;
        return clampLength(-kinematics.velocity / dt, maxAcceleration);
    }

    const Vec3d offset = target.position - kinematics.position;
    const Vec3d relativeVelocity = kinematics.velocity - target.velocity;
    const f64 distance = length(offset);
    const f64 standoff = control.mode == FlightMode::Approach ? control.standoff : 0.0;
    const f64 gap = distance - standoff; // negative: inside the standoff sphere, back off
    const Vec3d direction = distance > 1e-6 ? offset / distance : Vec3d{};

    const f64 brakingSpeed = std::sqrt(2.0 * kBrakingMargin * maxAcceleration * std::abs(gap));
    const f64 closingSpeed = std::min({drive.cruiseSpeed, brakingSpeed, std::abs(gap) / dt});
    const Vec3d desiredRelativeVelocity = direction * (gap >= 0.0 ? closingSpeed : -closingSpeed);
    const Vec3d acceleration =
        clampLength((desiredRelativeVelocity - relativeVelocity) / dt, maxAcceleration);

    const f64 tolerance = std::max(kMinArrivalTolerance, 0.01 * standoff);
    if (!control.arrived && std::abs(gap) < tolerance && length(relativeVelocity) < kArrivalSpeedTolerance) {
        control.arrived = true;
        arrivedNow = true;
    }
    return acceleration;
}

const OrbitState& FlightSystem::bodyState(const World& world, EntityId body, SimTime time, int depth) {
    const ComponentStore<CelestialBody>& bodies = world.components<CelestialBody>();
    const CelestialBody* entry = bodies.tryGet(body);
    GX_CHECK(entry != nullptr, "orbit parent {} is not a celestial body", body.index);
    const auto index = static_cast<usize>(entry - bodies.values().data());
    if (m_bodyStateReady[index] == 0) {
        OrbitState state;
        const OrbitsParent* link = world.components<OrbitsParent>().tryGet(body);
        GX_CHECK(depth < 16, "orbit hierarchy too deep (cycle?) at entity {}", body.index);
        if (link != nullptr && depth < 16) {
            const OrbitState& parent = bodyState(world, link->parent, time, depth + 1);
            const OrbitState relative = orbitStateAt(link->orbit, bodies.get(link->parent).gm, time);
            state.position = parent.position + relative.position;
            state.velocity = parent.velocity + relative.velocity;
        }
        m_bodyStates[index] = state;
        m_bodyStateReady[index] = 1;
    }
    return m_bodyStates[index];
}

TargetState FlightSystem::resolveCached(const World& world, const ShipControl& control, SimTime time) {
    if (control.mode == FlightMode::Approach && world.isAlive(control.target) &&
        world.components<CelestialBody>().contains(control.target)) {
        const OrbitState& state = bodyState(world, control.target, time);
        return {state.position, state.velocity, true};
    }
    return resolveTarget(world, control, time); // points and ships: no orbit to solve
}

SystemId FlightSystem::install(Simulation& simulation, SimDuration period) {
    return simulation.addSystem(
        {"Space.Flight", TickPhase::Simulation, period, {}, [this](const TickContext& c) { update(c); }});
}

void FlightSystem::update(const TickContext& context) {
    World& world = context.world;
    ComponentStore<ShipControl>& controls = world.components<ShipControl>();
    ComponentStore<Kinematics>& kinematics = world.components<Kinematics>();
    const ComponentStore<ShipDrive>& drives = world.components<ShipDrive>();
    const auto count = static_cast<u32>(controls.size());
    if (count == 0) {
        return;
    }
    const std::span<ShipControl> control = controls.values();
    const std::span<const EntityId> ships = controls.entities();
    const f64 dt = context.dt.toSeconds();

    // Pass 1, read-only: every target is resolved before any ship moves, at the instant the ships' state
    // refers to (the previous flight step). Comparing a ship at t - dt with a target at t would bias station
    // keeping by one step of the target's motion (tens of km for a planet). Serial: with body states
    // memoized per step it is a few lookups per ship.
    const SimTime stateTime = context.now - context.dt;
    {
        GX_PROFILE_SCOPE("Space.Flight.Targets");
        const usize bodyCount = world.components<CelestialBody>().size();
        m_bodyStates.resize(bodyCount);
        m_bodyStateReady.assign(bodyCount, 0);
        m_targets.resize(count);
        for (u32 i = 0; i < count; ++i) {
            m_targets[i] = resolveCached(world, control[i], stateTime);
        }
    }

    // Pass 2: each ship steers and integrates its own state.
    EventChannel<ShipArrived>& arrivals = context.events.channel<ShipArrived>();
    arrivals.beginParallel(JobSystem::chunkCount(count, m_grain));
    context.jobs.parallelFor(
        count, m_grain,
        [&](u32 begin, u32 end) {
            const u32 chunk = begin / m_grain;
            for (u32 i = begin; i < end; ++i) {
                Kinematics& state = kinematics.get(ships[i]);
                bool arrivedNow = false;
                const Vec3d acceleration =
                    steer(control[i], state, drives.get(ships[i]), m_targets[i], dt, arrivedNow);
                state.acceleration = acceleration;
                state.velocity += acceleration * dt;
                state.position += state.velocity * dt;
                if (arrivedNow) {
                    const EntityId target =
                        control[i].mode == FlightMode::Approach ? control[i].target : EntityId{};
                    arrivals.emitFromChunk(chunk, ShipArrived{ships[i], target});
                }
            }
        },
        "Space.Flight.Ships");
    arrivals.endParallel();
}

} // namespace gx
